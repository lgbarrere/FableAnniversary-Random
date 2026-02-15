#include "pch.h"

#include <windows.h>
#include <unknwn.h>

HMODULE realDinput8 = nullptr;

typedef HRESULT(WINAPI* DirectInput8Create_t)(
    HINSTANCE,
    DWORD,
    REFIID,
    LPVOID*,
    LPUNKNOWN
    );

DirectInput8Create_t real_DirectInput8Create = nullptr;

extern "C" __declspec(dllexport)
HRESULT WINAPI DirectInput8Create(
    HINSTANCE hinst,
    DWORD dwVersion,
    REFIID riidltf,
    LPVOID* ppvOut,
    LPUNKNOWN punkOuter)
{
    return real_DirectInput8Create(
        hinst,
        dwVersion,
        riidltf,
        ppvOut,
        punkOuter
    );
}

BOOL APIENTRY DllMain(HMODULE hModule,
    DWORD reason,
    LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(hModule);

        CreateThread(nullptr, 0, [](LPVOID) -> DWORD
            {
                MessageBoxA(NULL, "Proxy Loaded!", "Fable Mod Loader", MB_OK);
                return 0;
            }, nullptr, 0, nullptr);

        // Load the real dinput8.dll
        char systemPath[MAX_PATH];
        GetSystemDirectoryA(systemPath, MAX_PATH);
        strcat_s(systemPath, "\\dinput8.dll");

        realDinput8 = LoadLibraryA(systemPath);

        if (realDinput8)
        {
            real_DirectInput8Create =
                (DirectInput8Create_t)GetProcAddress(
                    realDinput8,
                    "DirectInput8Create"
                );
        }
    }

    return TRUE;
}

