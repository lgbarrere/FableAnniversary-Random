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
//   - 5-byte relative JMP written at kTargetAddr redirects execution to
//     HookThunk (a naked function that captures ECX before calling HookBody).
//   - HookBody logs all arguments (identical format to hook_logger_mod) and
//     returns 0 — the original function is never invoked.
//   - Installation is deferred to a worker thread so it never runs under
//     the DLL loader lock (calling VirtualProtect from DllMain is unsafe).
// =============================================================================

#include "disable_add_item_mod.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <windows.h>

// ---------------------------------------------------------------------------
// Target
// ---------------------------------------------------------------------------

/// VA of NInventory::VCTCInventoryBase::AddItemToInventory in Fable.exe.
/// Module is assumed at 0x00400000 (no ASLR on this title).
static constexpr DWORD kTargetAddr = 0x005BF654u;

/// Number of bytes overwritten by our JMP.
/// First instructions are 1+2+1+4 bytes = 8 bytes.
static constexpr SIZE_T kPatchSize = 8u;

/// Number of stack bytes the original callee was responsible for cleaning
/// (__thiscall with 5 × 4-byte stack args = 20 bytes).
static constexpr DWORD kStackArgBytes = 20u;

// ---------------------------------------------------------------------------
// Logger
// ---------------------------------------------------------------------------
static char g_LogPath[MAX_PATH] = "disable_add_item_mod.log";

static void Log(const char *fmt, ...) {
  FILE *fp = nullptr;
  fopen_s(&fp, g_LogPath, "a");
  if (!fp)
    return;
  va_list a;
  va_start(a, fmt);
  vfprintf(fp, fmt, a);
  va_end(a);
  fputc('\n', fp);
  fclose(fp);
}

// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------
namespace {

/// Monotonically increasing call counter for log correlation.
static volatile LONG g_callCount = 0;

/// Set to true after InstallHook succeeds.
static volatile bool g_hookInstalled = false;

// ---------------------------------------------------------------------------
// HookBody — plain C function, called from the naked thunk.
// Convention: all args arrive on the stack in left-to-right order
// (pThis first, pushed manually from ECX by the thunk).
// We declare it __cdecl; the thunk cleans up pThis (add esp,4) and then
// issues ret 20 to honour the original __thiscall callee-cleanup contract.
//
// The function logs every argument (same format as hook_logger_mod) and
// returns 0 — the original AddItemToInventory is intentionally suppressed.
// ---------------------------------------------------------------------------
static char __cdecl HookBody(void *pThis, void *item, bool add_selected,
                             bool add_quick_access, int price_bought_for,
                             bool silent) {
  LONG idx = InterlockedIncrement(&g_callCount);

  // Safely dump the first 16 DWORDs (64 bytes) of the incoming item CThing.
  DWORD w[16] = {0};
  DWORD def_id = 0;
  if (item && !IsBadReadPtr(item, sizeof(w))) {
    DWORD *ptr = reinterpret_cast<DWORD *>(item);
    for (int i = 0; i < 16; ++i) {
      w[i] = ptr[i];
    }
    def_id = w[6]; // Offset 0x18 holds the Object Definition ID
  }

  Log("[DisableAddItem] call #%ld BLOCKED | this=0x%p | item=0x%p | def_id=%lu "
      "(0x%X)\n"
      "    [MemDump] %08X %08X %08X %08X | %08X %08X %08X %08X\n"
      "    [MemDump] %08X %08X %08X %08X | %08X %08X %08X %08X",
      (long)idx, pThis, item, def_id, def_id, w[0], w[1], w[2], w[3], w[4],
      w[5], w[6], w[7], w[8], w[9], w[10], w[11], w[12], w[13], w[14], w[15]);

  // Return 0 — original function is disabled.
  return 0;
}

// ---------------------------------------------------------------------------
// HookThunk — naked; replaces the function's entry point.
//
// Stack layout at entry (before we touch anything):
//   [esp+0]  = return address (pushed by CALL from game)
//   [esp+4]  = item           (arg1)
//   [esp+8]  = add_selected   (arg2)
//   [esp+12] = add_quick_access (arg3)
//   [esp+16] = price_bought_for (arg4)
//   [esp+20] = silent          (arg5)
//   ECX      = this
//
// We push all 5 stack args + ECX for HookBody (__cdecl), call it, clean the
// 6 pushed args (add esp,24), then ret 20 to honour the __thiscall
// callee-cleanup contract for the 5 original stack parameters.
// ---------------------------------------------------------------------------
__declspec(naked) static void HookThunk() {
  __asm {// Push args for HookBody right-to-left.
         // As each push decreases esp by 4, [esp+20] always reaches the next
         // original argument.
        push dword ptr [esp+20] // push silent
        push dword ptr [esp+20] // push price_bought_for
        push dword ptr [esp+20] // push add_quick_access
        push dword ptr [esp+20] // push add_selected
        push dword ptr [esp+20] // push item
        push ecx // push pThis

        call HookBody

                 // Clean the 6 args we pushed for HookBody (6 * 4 = 24 bytes)
        add  esp, 24

         // Return to game, cleaning the original 5 stack args (5 * 4 = 20)
        ret  20
  }
}

// ---------------------------------------------------------------------------
// Patch bytes we expect to see at kTargetAddr when OUR hook is active.
// ---------------------------------------------------------------------------
static BYTE g_ourPatch[kPatchSize] = {}; // filled in by InstallHook

// ---------------------------------------------------------------------------
// ApplyPatch — writes the JMP unconditionally.  Called on first install and
// from the watchdog thread whenever the patch has been overwritten.
// ---------------------------------------------------------------------------
static bool ApplyPatch() {
  BYTE *target = reinterpret_cast<BYTE *>(kTargetAddr);

  DWORD oldProt = 0;
  if (!VirtualProtect(target, kPatchSize, PAGE_EXECUTE_READWRITE, &oldProt)) {
    Log("[DisableAddItem] VirtualProtect(target 0x%08X) failed: %lu",
        kTargetAddr, GetLastError());
    return false;
  }

  // Write JMP HookThunk.
  DWORD rel =
      static_cast<DWORD>(reinterpret_cast<BYTE *>(HookThunk) - (target + 5));
  target[0] = 0xE9u;
  std::memcpy(target + 1, &rel, sizeof(DWORD));

  // NOP the remaining stolen bytes so disassemblers don't see garbage.
  for (SIZE_T i = 5; i < kPatchSize; ++i) {
    target[i] = 0x90; // NOP
  }

  FlushInstructionCache(GetCurrentProcess(), target, kPatchSize);
  VirtualProtect(target, kPatchSize, oldProt, &oldProt);
  return true;
}

// ---------------------------------------------------------------------------
// InstallHook — first-time patch + snapshot of what our bytes look like.
// MUST be called from a thread, not from DllMain directly.
// No trampoline is built because we never call the original.
// ---------------------------------------------------------------------------
static bool InstallHook() {
  if (!ApplyPatch())
    return false;

  // Snapshot the patch bytes so the watchdog can detect overwrites.
  std::memcpy(g_ourPatch, reinterpret_cast<void *>(kTargetAddr), kPatchSize);

  Log("[DisableAddItem] Hook installed at 0x%08X -> HookThunk @ 0x%p "
      "(original DISABLED, call logging active).",
      kTargetAddr, reinterpret_cast<void *>(HookThunk));
  g_hookInstalled = true;
  return true;
}

// ---------------------------------------------------------------------------
// WatchdogThread — re-applies the patch every second if another mod (e.g.
// hook_logger_mod) has overwritten our JMP at kTargetAddr.
// ---------------------------------------------------------------------------
static DWORD WINAPI WatchdogThread(LPVOID) {
  while (true) {
    Sleep(1000);
    if (!g_hookInstalled)
      continue;

    BYTE *target = reinterpret_cast<BYTE *>(kTargetAddr);
    if (std::memcmp(target, g_ourPatch, kPatchSize) != 0) {
      Log("[DisableAddItem] Patch at 0x%08X was overwritten — re-applying.",
          kTargetAddr);
      if (ApplyPatch()) {
        std::memcpy(g_ourPatch, target, kPatchSize);
        Log("[DisableAddItem] Patch re-applied successfully.");
      }
    }
  }
  return 0;
}

// ---------------------------------------------------------------------------
// Worker thread — defers hook installation so it never runs under the loader
// lock (calling VirtualProtect / writing to executable pages from DllMain is
// undefined behaviour and can deadlock or crash on some runtimes).
//
// We sleep 1500 ms — longer than hook_logger_mod's 500 ms — so that if both
// mods are loaded simultaneously we always install last and our JMP wins.
// The watchdog thread then keeps re-patching should anything overwrite us.
// ---------------------------------------------------------------------------
static DWORD WINAPI HookInstallThread(LPVOID) {
  // Sleep longer than hook_logger_mod (500 ms) so we always install last.
  Sleep(1500);

  Log("[DisableAddItem] Installing hook at 0x%08X ...", kTargetAddr);
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

    // Resolve log path: place the log file next to this DLL.
    if (GetModuleFileNameA(hModule, g_LogPath, MAX_PATH)) {
      char *lastSlash = strrchr(g_LogPath, '\\');
      if (lastSlash) {
        strcpy_s(lastSlash + 1, MAX_PATH - (lastSlash - g_LogPath) - 1,
                 "disable_add_item_mod.log");
      }
    }

    // Truncate log so each session starts clean.
    FILE *fp = nullptr;
    fopen_s(&fp, g_LogPath, "w");
    if (fp)
      fclose(fp);

    Log("[DisableAddItem] disable_add_item_mod loaded. Target VA=0x%08X.",
        kTargetAddr);

    // Defer hook installation to a worker thread.
    CreateThread(nullptr, 0, HookInstallThread, nullptr, 0, nullptr);

    // Start watchdog thread that re-patches if another mod overwrites our JMP.
    CreateThread(nullptr, 0, WatchdogThread, nullptr, 0, nullptr);
  }

  return TRUE;
}
