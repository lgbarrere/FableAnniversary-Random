// =============================================================================
// Author       : Yaranorgoth
// Description  : Memory safety utilities using ReadProcessMemory to replace 
//                the obsolete and dangerous IsBadReadPtr API.
// =============================================================================

#pragma once

#include <windows.h>

// ---------------------------------------------------------------------------
// SafeReadBuffer — Reads a block of memory safely without triggering
// structured exceptions (which IsBadReadPtr suppresses, corrupting OS state).
// Returns true if the entire buffer was successfully read.
// ---------------------------------------------------------------------------
inline bool SafeReadBuffer(LPCVOID addr, void* outBuffer, SIZE_T size) {
    if (!addr || !outBuffer || size == 0) return false;
    SIZE_T bytesRead = 0;
    return ReadProcessMemory(GetCurrentProcess(), addr, outBuffer, size, &bytesRead) && bytesRead == size;
}

// ---------------------------------------------------------------------------
// SafeRead<T> — Safely reads a structured type T from memory.
// Returns true on success.
// ---------------------------------------------------------------------------
template <typename T>
inline bool SafeRead(LPCVOID addr, T& outValue) {
    return SafeReadBuffer(addr, &outValue, sizeof(T));
}
