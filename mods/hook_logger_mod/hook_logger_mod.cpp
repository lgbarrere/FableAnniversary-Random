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
//   - 5-byte relative JMP written at kTargetAddr redirects execution to
//     HookThunk (a naked function that captures ECX before calling HookBody).
//   - A 10-byte trampoline (5 stolen bytes + JMP back) lets us call the
//     original implementation cleanly.
//   - Installation is deferred to a worker thread so it never runs under
//     the DLL loader lock (calling VirtualProtect from DllMain is unsafe).
// =============================================================================

#include "hook_logger_mod.h"

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

/// Number of stack parameters pushed by the caller (5 × 4 bytes = 20).
/// __thiscall: callee cleans the *stack* args; ECX (this) is a register.
static constexpr DWORD kStackArgBytes = 20u;

// ---------------------------------------------------------------------------
// Logger
// ---------------------------------------------------------------------------
static char g_LogPath[MAX_PATH] = "hook_logger_mod.log";

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
// Function type
// ---------------------------------------------------------------------------
using AddItemToInventory_t = char(__thiscall *)(
    void * /*pThis*/, void * /*item*/, bool /*add_selected*/,
    bool /*add_quick_access*/, int /*price_bought_for*/, bool /*silent*/
);

// ---------------------------------------------------------------------------
// Trampoline + globals
// ---------------------------------------------------------------------------
#include <map>

// Exported inventory pointer that other mods (like add_item_mod) can read
extern "C" __declspec(dllexport) void* g_LastInventoryPtr = nullptr;
extern "C" __declspec(dllexport) void* g_LastItemPtr = nullptr;

namespace { // ---------------------------------------------------------------------------
// Trampoline + globals
// ---------------------------------------------------------------------------

/// 10-byte buffer: [0..4] stolen bytes, [5..9] JMP back to target+5.
static BYTE g_trampoline[kPatchSize + 5] = {};

/// Monotonically increasing call counter for log correlation.
static volatile LONG g_callCount = 0;

/// Set to true after InstallHook succeeds.
static volatile bool g_hookInstalled = false;

// ---------------------------------------------------------------------------
// HookBody — plain C function, called from the naked thunk.
// Convention: all args arrive on the stack in left-to-right order
// (pThis first, pushed manually from ECX by the thunk).
// We declare it __cdecl; stack clean-up for pThis is done by the thunk
// (add esp,4 before ret 20).  The original 5 thiscall stack args are cleaned
// by the thunk's "ret 20".
// ---------------------------------------------------------------------------
static char __cdecl HookBody(void *pThis, void *item, bool add_selected,
                             bool add_quick_access, int price_bought_for,
                             bool silent) {
  LONG idx = InterlockedIncrement(&g_callCount);

  // Safely dump the first 16 DWORDs (64 bytes) of the incoming item CThing.
  // Fable defines an Object UID and an Object Definition ID inside this struct.
  DWORD w[16] = {0};
  DWORD def_id = 0;
  if (item && !IsBadReadPtr(item, sizeof(w))) {
    DWORD *ptr = reinterpret_cast<DWORD *>(item);
    for (int i = 0; i < 16; ++i) {
      w[i] = ptr[i];
    }
    def_id = w[6]; // Offset 0x18 holds the Object Definition ID
  }

  // Save the pointer for other mods
  g_LastInventoryPtr = pThis;
  g_LastItemPtr = item;

  Log("[HookLogger] call #%ld | this=0x%p | item=0x%p | def_id=%lu (0x%X)\n"
      "    [MemDump] %08X %08X %08X %08X | %08X %08X %08X %08X\n"
      "    [MemDump] %08X %08X %08X %08X | %08X %08X %08X %08X",
      (long)idx, pThis, item, def_id, def_id,
      w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7],
      w[8], w[9], w[10], w[11], w[12], w[13], w[14], w[15]);

  // Call original via trampoline.
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
// Stack layout at entry (before we touch anything):
//   [esp+0]  = return address (pushed by CALL from game)
//   [esp+4]  = item           (arg1)
//   [esp+8]  = add_selected   (arg2)
//   [esp+12] = add_quick_access (arg3)
//   [esp+16] = price_bought_for (arg4)
//   [esp+20] = silent          (arg5)
//   ECX      = this
//
// After "push ecx":
//   [esp+0]  = ecx (pThis)    <- extra arg for HookBody
//   [esp+4]  = return address
//   [esp+8]  = item  ...
//
// HookBody is __cdecl so it never touches the stack on return.
// After the call we:
//   add esp, 4    — discard pThis we pushed
//   ret 20        — return to the original caller AND pop 5×4 stack args
//                   (matches __thiscall callee-cleanup contract)
// ---------------------------------------------------------------------------
__declspec(naked) static void HookThunk() {
  __asm {
      // Entry stack:
      // [esp+0]  = return address to game
      // [esp+4]  = item
      // [esp+8]  = add_selected
      // [esp+12] = add_quick_access
      // [esp+16] = price_bought_for
      // [esp+20] = silent
      // ECX      = pThis

      // Push args for HookBody (__cdecl). Right to left:
      // As we push, esp decreases by 4, so [esp+20] always targets the NEXT
      // argument.
        push dword ptr [esp+20] // push silent
        push dword ptr [esp+20] // push price_bought_for
        push dword ptr [esp+20] // push add_quick_access
        push dword ptr [esp+20] // push add_selected
        push dword ptr [esp+20] // push item
        push ecx // push pThis

        call HookBody

              // Clean the 6 args we just pushed for HookBody (6 * 4 = 24 bytes)
        add  esp, 24

      // Return to game, cleaning the original 5 args from the game's stack (5 *
      // 4 = 20)
        ret  20
  }
}

// ---------------------------------------------------------------------------
// InstallHook — writes a 5-byte JMP at kTargetAddr.
// MUST be called from a thread, not from DllMain directly.
// ---------------------------------------------------------------------------
static bool InstallHook() {
  BYTE *target = reinterpret_cast<BYTE *>(kTargetAddr);

  // --- Build the trampoline ---

  // Copy the 5 bytes that will be overwritten.
  std::memcpy(g_trampoline, target, kPatchSize);

  // Append JMP from trampoline back to (target + 5).
  BYTE *jmpSrc =
      g_trampoline + kPatchSize; // address of the JMP instr in trampoline
  DWORD rel = static_cast<DWORD>((target + kPatchSize) - // destination
                                 (jmpSrc + 5));          // source end
  jmpSrc[0] = 0xE9u;
  std::memcpy(jmpSrc + 1, &rel, sizeof(DWORD));

  // Make trampoline buffer executable.
  DWORD oldProt = 0;
  if (!VirtualProtect(g_trampoline, sizeof(g_trampoline),
                      PAGE_EXECUTE_READWRITE, &oldProt)) {
    Log("[HookLogger] VirtualProtect(trampoline) failed: %lu", GetLastError());
    return false;
  }
  FlushInstructionCache(GetCurrentProcess(), g_trampoline,
                        sizeof(g_trampoline));

  // --- Patch the target ---

  DWORD targetOldProt = 0;
  if (!VirtualProtect(target, kPatchSize, PAGE_EXECUTE_READWRITE,
                      &targetOldProt)) {
    Log("[HookLogger] VirtualProtect(target 0x%08X) failed: %lu", kTargetAddr,
        GetLastError());
    return false;
  }

  // Write JMP HookThunk.
  DWORD hookRel =
      static_cast<DWORD>(reinterpret_cast<BYTE *>(HookThunk) - (target + 5));
  target[0] = 0xE9u;
  std::memcpy(target + 1, &hookRel, sizeof(DWORD));

  // NOP remaining bytes to keep disassemblers happy
  for (SIZE_T i = 5; i < kPatchSize; ++i) {
    target[i] = 0x90; // NOP
  }

  FlushInstructionCache(GetCurrentProcess(), target, kPatchSize);
  VirtualProtect(target, kPatchSize, targetOldProt, &targetOldProt);

  Log("[HookLogger] Hook installed at 0x%08X -> HookThunk @ 0x%p", kTargetAddr,
      reinterpret_cast<void *>(HookThunk));
  g_hookInstalled = true;
  return true;
}

// ---------------------------------------------------------------------------
// Worker thread — defers hook installation so it never runs under the loader
// lock (calling VirtualProtect / writing to executable pages from DllMain is
// undefined behaviour and can deadlock or crash on some runtimes).
// ---------------------------------------------------------------------------
static DWORD WINAPI HookInstallThread(LPVOID) {
  // Give the game a moment to finish its own DLL initialization before
  // we start poking at its code pages.
  Sleep(500);

  Log("[HookLogger] Installing hook at 0x%08X ...", kTargetAddr);
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

    // Resolve log path: place the log file next to this DLL.
    if (GetModuleFileNameA(hModule, g_LogPath, MAX_PATH)) {
      char *lastSlash = strrchr(g_LogPath, '\\');
      if (lastSlash) {
        strcpy_s(lastSlash + 1, MAX_PATH - (lastSlash - g_LogPath) - 1,
                 "hook_logger_mod.log");
      }
    }

    // Truncate log so each session starts clean.
    FILE *fp = nullptr;
    fopen_s(&fp, g_LogPath, "w");
    if (fp)
      fclose(fp);

    Log("[HookLogger] hook_logger_mod loaded. Target VA=0x%08X.", kTargetAddr);

    // Defer hook installation to a worker thread — do NOT call
    // VirtualProtect / WriteProcessMemory from inside DllMain.
    CreateThread(nullptr, 0, HookInstallThread, nullptr, 0, nullptr);
  }

  return TRUE;
}
