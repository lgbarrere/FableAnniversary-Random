// =============================================================================
// Project      : FableAnniversary-Random
// File         : dllmain.cpp
// Author       : FableModLoader
// Created      : 2026-03-15
// Description  : Entry point for the dinput8 proxy DLL.
//                Forwards DirectInput8Create to the real system DLL, loads
//                any *.dll files from the "mods" folder, and triggers the
//                function-prototype dump for Fable.exe on startup.
// =============================================================================

#include "pch.h"
#include "function_dumper.h"

#include <cstdio>
#include <filesystem>
#include <string>
#include <unknwn.h>
#include <windows.h>

// ----------------------
// Proxy DirectInput8
// ----------------------
HMODULE realDinput8 = nullptr;

typedef HRESULT(WINAPI *DirectInput8Create_t)(HINSTANCE, DWORD, REFIID,
                                              LPVOID *, LPUNKNOWN);

DirectInput8Create_t real_DirectInput8Create = nullptr;

// Loads the real DLL on demand (lazy loading)
void EnsureLoaded() {
  if (real_DirectInput8Create)
    return;

  if (!realDinput8) {
    char systemPath[MAX_PATH];
    GetSystemDirectoryA(systemPath, MAX_PATH);
    strcat_s(systemPath, "\\dinput8.dll");

    realDinput8 = LoadLibraryA(systemPath);
  }

  if (realDinput8 && !real_DirectInput8Create) {
    real_DirectInput8Create =
        (DirectInput8Create_t)GetProcAddress(realDinput8, "DirectInput8Create");
  }
}

// Export for the game
extern "C" __declspec(dllexport) HRESULT WINAPI
DirectInput8Create(HINSTANCE hinst, DWORD dwVersion, REFIID riidltf,
                   LPVOID *ppvOut, LPUNKNOWN punkOuter) {
  EnsureLoaded();

  if (!real_DirectInput8Create)
    return E_FAIL;

  return real_DirectInput8Create(hinst, dwVersion, riidltf, ppvOut, punkOuter);
}

// ----------------------
// File logger
// ----------------------
void Log(const char *format, ...) {
  FILE *fp;
  fopen_s(&fp, "FableModLoader.log", "a");
  if (!fp)
    return;

  va_list args;
  va_start(args, format);
  vfprintf(fp, format, args);
  fprintf(fp, "\n");
  va_end(args);

  fclose(fp);
}

// ----------------------
// Mod loading
// ----------------------
void LoadMods() {
  std::string modPath = ".\\mods\\";

  if (!std::filesystem::exists(modPath)) {
    Log("Mod folder does not exist: %s", modPath.c_str());
    return;
  }

  for (const auto &entry : std::filesystem::directory_iterator(modPath)) {
    if (entry.path().extension() == ".dll") {
      HMODULE mod = LoadLibraryA(entry.path().string().c_str());
      if (mod)
        Log("Loaded mod: %s", entry.path().filename().string().c_str());
      else
        Log("Failed to load mod: %s", entry.path().filename().string().c_str());
    }
  }
}

// ----------------------
// Initialization thread
// ----------------------
DWORD WINAPI InitThread(LPVOID) {
  Log("Fable Mod Loader Initialized");

  LoadMods();

  // Dump all function prototypes from Fable.exe.
  // Output is written to FableFunctions.log in the game directory.
  Log("Starting function prototype dump...");
  DumpFunctionPrototypes();
  Log("Function prototype dump complete -> FableFunctions.log");

  Log("InitThread finished");
  return 0;
}

// ----------------------
// DllMain
// ----------------------
BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) {
    DisableThreadLibraryCalls(hModule);

    // Create a thread for anything that may take time
    CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr);
  }

  return TRUE;
}
