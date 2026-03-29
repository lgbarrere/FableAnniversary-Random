// =============================================================================
// Author       : Yaranorgoth
// Description  : Hooks IDirect3D9::CreateDevice so Fable.exe always launches
//                in windowed mode, regardless of its own settings.
// =============================================================================
#pragma once

// Must be called once from the init thread, before the game's render loop
// starts. Patches the IDirect3D9 vtable in-process.
void InstallWindowedHook();
