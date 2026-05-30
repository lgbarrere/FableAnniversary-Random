// =============================================================================
// Author       : Yaranorgoth
// Description  : Generic utility for 5-byte relative JMP inline hooking.
// =============================================================================

#pragma once

#include <windows.h>
#include <cstring>
#include "mod_log.h"

// ---------------------------------------------------------------------------
// InlineHook
// Safely replaces the first N bytes (at least 5) of a target function with
// a JMP to a hook function, providing utilities to temporarily unhook and rehook.
// ---------------------------------------------------------------------------
template <size_t PatchSize>
class InlineHook {
private:
  DWORD targetVA;
  void *hookFunc;
  BYTE originalBytes[PatchSize];
  BYTE patchBytes[PatchSize];
  bool installed;

public:
  InlineHook(DWORD target, void *hook)
      : targetVA(target), hookFunc(hook), installed(false) {
    std::memset(originalBytes, 0, PatchSize);
    std::memset(patchBytes, 0x90, PatchSize); // Fill padding with NOPs
  }

  // Initial installation: captures original bytes and computes patch
  bool Install() {
    auto *target = reinterpret_cast<BYTE *>(targetVA);

    // Snapshot the original bytes
    std::memcpy(originalBytes, target, PatchSize);

    // Compute the 5-byte JMP instruction
    patchBytes[0] = 0xE9u; // JMP rel32
    DWORD rel = static_cast<DWORD>(
        reinterpret_cast<BYTE *>(hookFunc) - (target + 5));
    std::memcpy(patchBytes + 1, &rel, sizeof(DWORD));

    return Apply();
  }

  // Re-applies the hook bytes to memory
  bool Apply() {
    auto *target = reinterpret_cast<BYTE *>(targetVA);
    DWORD oldProt = 0;

    if (!VirtualProtect(target, PatchSize, PAGE_EXECUTE_READWRITE, &oldProt)) {
      Log("[InlineHook] VirtualProtect(0x%08X) failed: %lu", targetVA, GetLastError());
      return false;
    }

    std::memcpy(target, patchBytes, PatchSize);
    FlushInstructionCache(GetCurrentProcess(), target, PatchSize);
    VirtualProtect(target, PatchSize, oldProt, &oldProt);

    installed = true;
    return true;
  }

  // Removes the hook by restoring original bytes
  void Remove() {
    auto *target = reinterpret_cast<BYTE *>(targetVA);
    DWORD oldProt = 0;

    if (VirtualProtect(target, PatchSize, PAGE_EXECUTE_READWRITE, &oldProt)) {
      std::memcpy(target, originalBytes, PatchSize);
      FlushInstructionCache(GetCurrentProcess(), target, PatchSize);
      VirtualProtect(target, PatchSize, oldProt, &oldProt);
    }

    installed = false;
  }

  // Detects if another mod overwrote our patch
  bool IsOverwritten() const {
    auto *target = reinterpret_cast<BYTE *>(targetVA);
    return std::memcmp(target, patchBytes, PatchSize) != 0;
  }

  bool IsInstalled() const {
    return installed;
  }
};
