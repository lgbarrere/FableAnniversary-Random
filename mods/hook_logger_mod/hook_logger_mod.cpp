// =============================================================================
// Author       : Yaranorgoth
// Description  : Hooks the game function at 0x005BF654 with a 5-byte inline
//                JMP and logs every call alongside all argument values.
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
//   - 5-byte relative JMP written at kAddItemToInventoryAddr redirects
//     execution to HookThunk (a naked function that captures ECX before
//     calling HookBody).
//   - A trampoline (kPatchSize stolen bytes + JMP back) lets us call the
//     original implementation cleanly.
//   - Installation is deferred to a worker thread so it never runs under
//     the DLL loader lock (calling VirtualProtect from DllMain is unsafe).
// =============================================================================

#include "hook_logger_mod.h"

#include "../shared/cthing_dump.h"
#include "../shared/fable_addresses.h"
#include "../shared/fable_types.h"
#include "../shared/mod_log.h"

#include <cstring>
#include <windows.h>

// Exported inventory/item pointers that other mods can read.
extern "C" __declspec(dllexport) void *g_LastInventoryPtr = nullptr;
extern "C" __declspec(dllexport) void *g_LastItemPtr      = nullptr;

namespace {

// ---------------------------------------------------------------------------
// Trampoline buffer: [0 .. kPatchSize-1] stolen bytes, [kPatchSize .. +4] JMP back.
// ---------------------------------------------------------------------------
static BYTE g_trampoline[kPatchSize + 5] = {};

/// Monotonically increasing call counter for log correlation.
static volatile LONG g_callCount = 0;

/// Set to true after InstallHook() succeeds.
static volatile bool g_hookInstalled = false;

// ---------------------------------------------------------------------------
// HookBody — plain C function called from the naked thunk.
//
// All arguments arrive on the stack left-to-right (pThis first, pushed
// manually from ECX by the thunk).  Declared __cdecl; the thunk is
// responsible for cleaning pThis (add esp,4) before issuing ret 20.
// ---------------------------------------------------------------------------
static char __cdecl HookBody(void *pThis, void *item, bool add_selected,
                              bool add_quick_access, int price_bought_for,
                              bool silent) {
  const LONG idx = InterlockedIncrement(&g_callCount);
  const CThingDump d = ReadCThingDump(item);

  g_LastInventoryPtr = pThis;
  g_LastItemPtr      = item;

  Log("[HookLogger] call #%ld | this=0x%p | item=0x%p | def_id=%lu (0x%X)\n"
      "    [MemDump] %08X %08X %08X %08X | %08X %08X %08X %08X\n"
      "    [MemDump] %08X %08X %08X %08X | %08X %08X %08X %08X",
      (long)idx, pThis, item, d.def_id, d.def_id,
      d.w[0],  d.w[1],  d.w[2],  d.w[3],
      d.w[4],  d.w[5],  d.w[6],  d.w[7],
      d.w[8],  d.w[9],  d.w[10], d.w[11],
      d.w[12], d.w[13], d.w[14], d.w[15]);

  // Call the original via the trampoline.
  auto original = reinterpret_cast<AddItemToInventory_t>(
      reinterpret_cast<void *>(g_trampoline));
  char ret = original(pThis, item, add_selected, add_quick_access,
                      price_bought_for, silent);

  Log("[HookLogger] call #%ld returned %d", (long)idx, (int)(unsigned char)ret);
  return ret;
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
//
// We push all 5 stack args + ECX for HookBody (__cdecl), call it, clean the
// 6 pushed args (add esp,24), then ret 20 to honour the __thiscall
// callee-cleanup contract for the 5 original stack parameters.
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
// InstallHook — builds the trampoline then patches kAddItemToInventoryAddr.
// MUST be called from a thread, never from DllMain.
// ---------------------------------------------------------------------------
static bool InstallHook() {
  auto *target = reinterpret_cast<BYTE *>(kAddItemToInventoryAddr);

  // Copy stolen bytes into the trampoline.
  std::memcpy(g_trampoline, target, kPatchSize);

  // Append JMP back to (target + kPatchSize).
  BYTE  *jmpSrc = g_trampoline + kPatchSize;
  DWORD  rel    = static_cast<DWORD>((target + kPatchSize) - (jmpSrc + 5));
  jmpSrc[0] = 0xE9u;
  std::memcpy(jmpSrc + 1, &rel, sizeof(DWORD));

  // Make trampoline executable.
  DWORD oldProt = 0;
  if (!VirtualProtect(g_trampoline, sizeof(g_trampoline),
                      PAGE_EXECUTE_READWRITE, &oldProt)) {
    Log("[HookLogger] VirtualProtect(trampoline) failed: %lu", GetLastError());
    return false;
  }
  FlushInstructionCache(GetCurrentProcess(), g_trampoline, sizeof(g_trampoline));

  // Patch the target.
  DWORD targetOldProt = 0;
  if (!VirtualProtect(target, kPatchSize, PAGE_EXECUTE_READWRITE, &targetOldProt)) {
    Log("[HookLogger] VirtualProtect(target 0x%08X) failed: %lu",
        kAddItemToInventoryAddr, GetLastError());
    return false;
  }

  DWORD hookRel = static_cast<DWORD>(
      reinterpret_cast<BYTE *>(HookThunk) - (target + 5));
  target[0] = 0xE9u;
  std::memcpy(target + 1, &hookRel, sizeof(DWORD));
  for (SIZE_T i = 5; i < kPatchSize; ++i)
    target[i] = 0x90; // NOP padding

  FlushInstructionCache(GetCurrentProcess(), target, kPatchSize);
  VirtualProtect(target, kPatchSize, targetOldProt, &targetOldProt);

  Log("[HookLogger] Hook installed at 0x%08X -> HookThunk @ 0x%p",
      kAddItemToInventoryAddr, reinterpret_cast<void *>(HookThunk));
  g_hookInstalled = true;
  return true;
}

// ---------------------------------------------------------------------------
// HookInstallThread — defers installation outside the loader lock.
// ---------------------------------------------------------------------------
static DWORD WINAPI HookInstallThread(LPVOID) {
  Sleep(500); // Give the game time to finish its own DLL initialization.

  Log("[HookLogger] Installing hook at 0x%08X ...", kAddItemToInventoryAddr);
  if (!InstallHook())
    Log("[HookLogger] Hook installation FAILED.");

  return 0;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// DllMain
// ---------------------------------------------------------------------------
BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) {
    DisableThreadLibraryCalls(hModule);
    InitModLog(hModule, "hook_logger_mod.log");
    Log("[HookLogger] hook_logger_mod loaded. Target VA=0x%08X.",
        kAddItemToInventoryAddr);
    CreateThread(nullptr, 0, HookInstallThread, nullptr, 0, nullptr);
  }
  return TRUE;
}
