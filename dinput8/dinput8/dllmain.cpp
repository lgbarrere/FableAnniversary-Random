// =============================================================================
// Author       : Yaranorgoth
// Description  : Entry point for the dinput8 proxy DLL.
//                Forwards DirectInput8Create to the real system DLL, loads
//                any *.dll files from the "mods" folder, and triggers the
//                function-prototype dump for Fable.exe on startup.
// =============================================================================

#include "pch.h"

#include "../../mods/shared/mod_log.h"
#include "windowed_hook.h"

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

// Loads the real DLL on demand (lazy loading).
void EnsureLoaded() {
  if (real_DirectInput8Create)
    return; // Already resolved — nothing to do.

  if (!realDinput8) {
    char systemPath[MAX_PATH];
    GetSystemDirectoryA(systemPath, MAX_PATH);
    strcat_s(systemPath, "\\dinput8.dll");
    realDinput8 = LoadLibraryA(systemPath);
  }

  if (realDinput8) {
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

  // Hook IDirect3D9::CreateDevice before the game's render loop starts,
  // so the game always launches in windowed mode.
  InstallWindowedHook();
  Log("Windowed hook installed.");

  LoadMods();

  Log("Startup complete. Mod loader running.");

  // Keep this thread alive for the entire game session.
  // Sleeping prevents CPU waste while ensuring the DLL and all its sub-threads
  // (e.g. AddItemThread) remain active until the process exits.
  while (true)
    Sleep(1000);
}

// ----------------------
// DllMain
// ----------------------
BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) {
    DisableThreadLibraryCalls(hModule);
    InitModLog(hModule, "FableModLoader.log");

    // Create a thread for anything that may take time
    CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr);
  }

  return TRUE;
}
