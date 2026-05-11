// =============================================================================
// Author       : Yaranorgoth
// Description  : Header-only logger shared by all Fable mod DLLs.
//
// Usage (in each mod's DllMain, DLL_PROCESS_ATTACH branch):
//   InitModLog(hModule, "my_mod.log");
//
// Then call Log() freely from any thread.  Each call opens, appends one line,
// and closes the file — safe for multi-threaded use without an explicit lock
// (filesystem serialises concurrent writers on Windows).
// =============================================================================

#pragma once

#include <cstdarg>
#include <cstdio>
#include <windows.h>

/// Absolute path to this mod's log file.
/// Populated by InitModLog(); defaults to a relative name as a fallback.
static char g_LogPath[MAX_PATH] = "mod.log";

// ---------------------------------------------------------------------------
// InitModLog — resolve g_LogPath and truncate the file.
// Call ONCE from DLL_PROCESS_ATTACH before any Log() calls.
// ---------------------------------------------------------------------------
inline void InitModLog(HMODULE hModule, const char *logFileName) {
  // Place the log file next to the DLL, regardless of the game's CWD.
  if (GetModuleFileNameA(hModule, g_LogPath, MAX_PATH)) {
    char *lastSlash = strrchr(g_LogPath, '\\');
    if (lastSlash) {
      strcpy_s(lastSlash + 1,
               MAX_PATH - static_cast<int>(lastSlash - g_LogPath) - 1,
               logFileName);
    }
  }

  // Truncate so each game session starts with a clean log.
  FILE *fp = nullptr;
  fopen_s(&fp, g_LogPath, "w");
  if (fp)
    fclose(fp);
}

// ---------------------------------------------------------------------------
// Log — append one formatted line to the mod log file.
// ---------------------------------------------------------------------------
inline void Log(const char *fmt, ...) {
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
