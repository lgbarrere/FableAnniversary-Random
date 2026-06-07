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

#include "cancel_vanilla_add_item_mod.h"

#include "../shared/cthing_dump.h"
#include "../shared/fable_addresses.h"
#include "../shared/fable_types.h"
#include "../shared/inline_hook.h"
#include "../shared/mod_log.h"

#include <cstring>
#include <windows.h>

namespace {

/// Monotonically increasing call counter for log correlation.
static volatile LONG g_callCount = 0;

static volatile bool g_hookInstalled = false;

static void HookThunk();
static InlineHook<kPatchSize> g_hook(kAddItemToInventoryAddr, reinterpret_cast<void*>(HookThunk));

/// Helper to check if add_item_mod is currently adding an item
static bool IsAddingItemFromMod() {
  static bool* pAdding = nullptr;
  if (!pAdding) {
    HMODULE addMod = GetModuleHandleA("add_item_mod.dll");
    if (addMod) {
      pAdding = reinterpret_cast<bool*>(GetProcAddress(addMod, "g_AddingItemFromMod"));
    }
  }
  return pAdding && *pAdding;
}

// ---------------------------------------------------------------------------
// HookBody — plain C function called from the naked thunk.
//
// Logs all arguments (same format as hook_logger_mod) then returns 0 to
// suppress the original AddItemToInventory.
// ---------------------------------------------------------------------------
static char __cdecl HookBody(void *pThis, void *item, bool add_selected,
                              bool add_quick_access, int price_bought_for,
                              bool silent) {
  if (IsAddingItemFromMod()) {
    g_hookInstalled = false;
    g_hook.Remove();

    auto fn = reinterpret_cast<AddItemToInventory_t>(kAddItemToInventoryAddr);
    char result = fn(pThis, item, add_selected, add_quick_access, price_bought_for, silent);

    g_hook.Apply();
    g_hookInstalled = true;
    return result;
  }
  const LONG idx = InterlockedIncrement(&g_callCount);
  const CThingDump d = ReadCThingDump(item);

  Log("[CancelVanillaAddItem] call #%ld BLOCKED | this=0x%p | item=0x%p | def_id=%lu (0x%X)\n"
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
// InstallHook — uses InlineHook
// ---------------------------------------------------------------------------
static bool InstallHook() {
  if (!g_hook.Install())
    return false;

  Log("[CancelVanillaAddItem] Hook installed at 0x%08X -> HookThunk @ 0x%p "
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

    if (g_hook.IsOverwritten()) {
      Log("[CancelVanillaAddItem] Patch at 0x%08X was overwritten — re-applying.",
          kAddItemToInventoryAddr);
      if (g_hook.Apply()) {
        Log("[CancelVanillaAddItem] Patch re-applied successfully.");
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

  Log("[CancelVanillaAddItem] Installing hook at 0x%08X ...", kAddItemToInventoryAddr);
  if (!InstallHook())
    Log("[CancelVanillaAddItem] Hook installation FAILED.");

  return 0;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// DllMain
// ---------------------------------------------------------------------------
BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) {
    DisableThreadLibraryCalls(hModule);
    InitModLog(hModule, "cancel_vanilla_add_item_mod.log");
    Log("[CancelVanillaAddItem] cancel_vanilla_add_item_mod loaded. Target VA=0x%08X.",
        kAddItemToInventoryAddr);
    CreateThread(nullptr, 0, HookInstallThread, nullptr, 0, nullptr);
    CreateThread(nullptr, 0, WatchdogThread,    nullptr, 0, nullptr);
  }
  return TRUE;
}
