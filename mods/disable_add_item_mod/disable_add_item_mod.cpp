// =============================================================================
// Author       : Yaranorgoth
// Description  : Hooks the game function at 0x005BF654 with a 5-byte inline
//                JMP, logs every call the same way hook_logger_mod does, then
//                returns 0 WITHOUT forwarding to the original implementation,
//                effectively disabling AddItemToInventory engine-wide.
//
// Full signature (vfunc_80 on NInventory::VCTCInventoryBase):
//   char __thiscall AddItemToInventory(
//       NInventory::VCTCInventoryBase* this,  // ECX (implicit)
//       CThing*  item,             // param 1
//       bool     add_selected,     // param 2
//       bool     add_quick_access, // param 3
//       int      price_bought_for, // param 4
//       bool     silent            // param 5
//   );
//
// Hook strategy:
//   - 5-byte relative JMP at kAddItemToInventoryAddr redirects to HookThunk.
//   - HookBody logs all arguments (identical format to hook_logger_mod) and
//     returns 0 — the original function is intentionally suppressed.
//   - A watchdog thread re-applies the patch every second in case another
//     mod (e.g. hook_logger_mod) overwrites it.
//   - Installation is deferred to a worker thread (VirtualProtect from
//     DllMain is unsafe).
// =============================================================================

#include "disable_add_item_mod.h"

#include "../shared/cthing_dump.h"
#include "../shared/fable_addresses.h"
#include "../shared/mod_log.h"

#include <cstring>
#include <windows.h>

namespace {

/// Monotonically increasing call counter for log correlation.
static volatile LONG g_callCount = 0;

/// Set to true after InstallHook() succeeds.
static volatile bool g_hookInstalled = false;

/// Snapshot of our patch bytes so the watchdog can detect overwrites.
static BYTE g_ourPatch[kPatchSize] = {};

// ---------------------------------------------------------------------------
// HookBody — plain C function called from the naked thunk.
//
// Logs all arguments (same format as hook_logger_mod) then returns 0 to
// suppress the original AddItemToInventory.
// ---------------------------------------------------------------------------
static char __cdecl HookBody(void *pThis, void *item, bool /*add_selected*/,
                              bool /*add_quick_access*/, int /*price_bought_for*/,
                              bool /*silent*/) {
  const LONG idx = InterlockedIncrement(&g_callCount);
  const CThingDump d = ReadCThingDump(item);

  Log("[DisableAddItem] call #%ld BLOCKED | this=0x%p | item=0x%p | def_id=%lu (0x%X)\n"
      "    [MemDump] %08X %08X %08X %08X | %08X %08X %08X %08X\n"
      "    [MemDump] %08X %08X %08X %08X | %08X %08X %08X %08X",
      (long)idx, pThis, item, d.def_id, d.def_id,
      d.w[0],  d.w[1],  d.w[2],  d.w[3],
      d.w[4],  d.w[5],  d.w[6],  d.w[7],
      d.w[8],  d.w[9],  d.w[10], d.w[11],
      d.w[12], d.w[13], d.w[14], d.w[15]);

  return 0; // Original function is disabled.
}

// ---------------------------------------------------------------------------
// HookThunk — naked; replaces the function's entry point.
//
// Stack layout at entry:
//   [esp+0]   return address to game caller
//   [esp+4]   item           (arg1)
//   [esp+8]   add_selected   (arg2)
//   [esp+12]  add_quick_access (arg3)
//   [esp+16]  price_bought_for (arg4)
//   [esp+20]  silent          (arg5)
//   ECX       = this (pThis)
// ---------------------------------------------------------------------------
__declspec(naked) static void HookThunk() {
  __asm {
      push dword ptr [esp+20]   // push silent
      push dword ptr [esp+20]   // push price_bought_for
      push dword ptr [esp+20]   // push add_quick_access
      push dword ptr [esp+20]   // push add_selected
      push dword ptr [esp+20]   // push item
      push ecx                  // push pThis

      call HookBody

      add  esp, 24              // clean 6 args pushed for HookBody
      ret  20                   // return & clean 5 original __thiscall args
  }
}

// ---------------------------------------------------------------------------
// ApplyPatch — writes the JMP unconditionally.
// Called on first install and by the watchdog on every detected overwrite.
// ---------------------------------------------------------------------------
static bool ApplyPatch() {
  auto *target = reinterpret_cast<BYTE *>(kAddItemToInventoryAddr);

  DWORD oldProt = 0;
  if (!VirtualProtect(target, kPatchSize, PAGE_EXECUTE_READWRITE, &oldProt)) {
    Log("[DisableAddItem] VirtualProtect(target 0x%08X) failed: %lu",
        kAddItemToInventoryAddr, GetLastError());
    return false;
  }

  DWORD rel = static_cast<DWORD>(
      reinterpret_cast<BYTE *>(HookThunk) - (target + 5));
  target[0] = 0xE9u;
  std::memcpy(target + 1, &rel, sizeof(DWORD));
  for (SIZE_T i = 5; i < kPatchSize; ++i)
    target[i] = 0x90; // NOP padding

  FlushInstructionCache(GetCurrentProcess(), target, kPatchSize);
  VirtualProtect(target, kPatchSize, oldProt, &oldProt);
  return true;
}

// ---------------------------------------------------------------------------
// InstallHook — first-time patch + snapshot for watchdog comparisons.
// No trampoline is built because we never call the original.
// ---------------------------------------------------------------------------
static bool InstallHook() {
  if (!ApplyPatch())
    return false;

  std::memcpy(g_ourPatch, reinterpret_cast<void *>(kAddItemToInventoryAddr), kPatchSize);

  Log("[DisableAddItem] Hook installed at 0x%08X -> HookThunk @ 0x%p "
      "(original DISABLED, call logging active).",
      kAddItemToInventoryAddr, reinterpret_cast<void *>(HookThunk));
  g_hookInstalled = true;
  return true;
}

// ---------------------------------------------------------------------------
// WatchdogThread — re-applies the patch every second if overwritten.
// ---------------------------------------------------------------------------
static DWORD WINAPI WatchdogThread(LPVOID) {
  while (true) {
    Sleep(1000);
    if (!g_hookInstalled)
      continue;

    auto *target = reinterpret_cast<BYTE *>(kAddItemToInventoryAddr);
    if (std::memcmp(target, g_ourPatch, kPatchSize) != 0) {
      Log("[DisableAddItem] Patch at 0x%08X was overwritten — re-applying.",
          kAddItemToInventoryAddr);
      if (ApplyPatch()) {
        std::memcpy(g_ourPatch, target, kPatchSize);
        Log("[DisableAddItem] Patch re-applied successfully.");
      }
    }
  }
  return 0;
}

// ---------------------------------------------------------------------------
// HookInstallThread — defers installation outside the loader lock.
// Sleeps longer than hook_logger_mod (500 ms) so our JMP always wins when
// both mods are loaded simultaneously; the watchdog then keeps it in place.
// ---------------------------------------------------------------------------
static DWORD WINAPI HookInstallThread(LPVOID) {
  Sleep(1500);

  Log("[DisableAddItem] Installing hook at 0x%08X ...", kAddItemToInventoryAddr);
  if (!InstallHook())
    Log("[DisableAddItem] Hook installation FAILED.");

  return 0;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// DllMain
// ---------------------------------------------------------------------------
BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) {
    DisableThreadLibraryCalls(hModule);
    InitModLog(hModule, "disable_add_item_mod.log");
    Log("[DisableAddItem] disable_add_item_mod loaded. Target VA=0x%08X.",
        kAddItemToInventoryAddr);
    CreateThread(nullptr, 0, HookInstallThread, nullptr, 0, nullptr);
    CreateThread(nullptr, 0, WatchdogThread,    nullptr, 0, nullptr);
  }
  return TRUE;
}
