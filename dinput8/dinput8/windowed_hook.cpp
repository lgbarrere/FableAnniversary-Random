// =============================================================================
// Author       : Yaranorgoth
// Description  : Hooks IDirect3D9::CreateDevice and IDirect3DDevice9::Reset so
//                Fable.exe always runs in windowed mode, regardless of its own
//                settings or any mode-switch it performs mid-session.
//
// Strategy:
//   Phase 1 — IDirect3D9::CreateDevice hook (vtable slot 16)
//     Installed at DLL load time using a temporary IDirect3D9 object.
//     Forces Windowed=TRUE before forwarding to the real CreateDevice.
//
//   Phase 2 — IDirect3DDevice9::Reset hook (vtable slot 16)
//     Installed the first time CreateDevice succeeds (we then have a real
//     device pointer and can patch its vtable).
//     Forces Windowed=TRUE on every subsequent Reset call, which is what
//     Fable uses when it transitions from the intro movie to the title screen.
//
// Both vtable patches use VirtualProtect to temporarily make the (normally
// read-only) vtable page writable.
// =============================================================================

#include "windowed_hook.h"
#include "pch.h"

#include <d3d9.h>
#include <windows.h>
#pragma comment(lib, "d3d9.lib")

// ---------------------------------------------------------------------------
namespace {
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Vtable slot indices
// ---------------------------------------------------------------------------

// IDirect3D9 vtable:
//   0  QueryInterface  1  AddRef  2  Release  3  RegisterSoftwareDevice
//   4  GetAdapterCount  5  GetAdapterIdentifier  6  GetAdapterModeCount
//   7  EnumAdapterModes  8  GetAdapterDisplayMode  9  CheckDeviceType
//  10  CheckDeviceFormat  11  CheckDeviceMultiSampleType
//  12  CheckDepthStencilMatch  13  CheckDeviceFormatConversion
//  14  GetDeviceCaps  15  GetAdapterMonitor  16  CreateDevice
static constexpr int kD3D9_CreateDevice = 16;

// IDirect3DDevice9 vtable:
//   0  QueryInterface  1  AddRef  2  Release  3  TestCooperativeLevel
//   4  GetAvailableTextureMem  5  EvictManagedResources  6  GetDirect3D
//   7  GetDeviceCaps  8  GetDisplayMode  9  GetCreationParameters
//  10  SetCursorProperties  11  SetCursorPosition  12  ShowCursor
//  13  CreateAdditionalSwapChain  14  GetSwapChain  15  GetNumberOfSwapChains
//  16  Reset
static constexpr int kDevice_Reset = 16;

// ---------------------------------------------------------------------------
// Function pointer types
// ---------------------------------------------------------------------------
using Direct3DCreate9_t = IDirect3D9 *(WINAPI *)(UINT);

using CreateDevice_t = HRESULT(WINAPI *)(IDirect3D9 *, UINT, D3DDEVTYPE, HWND,
                                         DWORD, D3DPRESENT_PARAMETERS *,
                                         IDirect3DDevice9 **);

using Reset_t = HRESULT(WINAPI *)(IDirect3DDevice9 *, D3DPRESENT_PARAMETERS *);

// ---------------------------------------------------------------------------
// Saved originals
// ---------------------------------------------------------------------------
CreateDevice_t g_origCreateDevice = nullptr;
Reset_t g_origReset = nullptr;

// ---------------------------------------------------------------------------
// Helper: force a D3DPRESENT_PARAMETERS into windowed mode.
// ---------------------------------------------------------------------------
static void ForceWindowed(D3DPRESENT_PARAMETERS *pPP) {
  if (!pPP)
    return;
  pPP->Windowed = TRUE;
  pPP->FullScreen_RefreshRateInHz = 0;    // must be 0 in windowed mode
  pPP->BackBufferFormat = D3DFMT_UNKNOWN; // auto for windowed
}

// ---------------------------------------------------------------------------
// Helper: patch one vtable slot. Returns the original pointer.
// ---------------------------------------------------------------------------
static void *PatchVTable(void **vtable, int slot, void *newFn) {
  void **target = &vtable[slot];
  DWORD oldProt = 0;
  if (!VirtualProtect(target, sizeof(void *), PAGE_EXECUTE_READWRITE, &oldProt))
    return nullptr;
  void *old = *target;
  *target = newFn;
  VirtualProtect(target, sizeof(void *), oldProt, &oldProt);
  return old;
}

// ---------------------------------------------------------------------------
// IDirect3DDevice9::Reset hook
// Called by Fable every time it switches presentation mode (e.g. intro ->
// title).
// ---------------------------------------------------------------------------
HRESULT WINAPI HookReset(IDirect3DDevice9 *pDevice,
                         D3DPRESENT_PARAMETERS *pPP) {
  ForceWindowed(pPP);
  return g_origReset(pDevice, pPP);
}

// ---------------------------------------------------------------------------
// Install the Reset hook on a live device (once only).
// ---------------------------------------------------------------------------
static void InstallResetHook(IDirect3DDevice9 *pDevice) {
  if (g_origReset)
    return; // already installed
  void **vtable = *reinterpret_cast<void ***>(pDevice);
  void *old =
      PatchVTable(vtable, kDevice_Reset, reinterpret_cast<void *>(&HookReset));
  if (old)
    g_origReset = reinterpret_cast<Reset_t>(old);
}

// ---------------------------------------------------------------------------
// IDirect3D9::CreateDevice hook
// Forces windowed mode and, on the first successful call, patches Reset too.
// ---------------------------------------------------------------------------
HRESULT WINAPI HookCreateDevice(IDirect3D9 *pD3D, UINT Adapter,
                                D3DDEVTYPE DeviceType, HWND hFocusWindow,
                                DWORD BehaviorFlags, D3DPRESENT_PARAMETERS *pPP,
                                IDirect3DDevice9 **ppDevice) {
  ForceWindowed(pPP);

  HRESULT hr = g_origCreateDevice(pD3D, Adapter, DeviceType, hFocusWindow,
                                  BehaviorFlags, pPP, ppDevice);

  // If device creation succeeded, hook Reset on the returned device so we
  // intercept every future mode-switch the game attempts.
  if (SUCCEEDED(hr) && ppDevice && *ppDevice)
    InstallResetHook(*ppDevice);

  return hr;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Public entry point — call once from the init thread.
// ---------------------------------------------------------------------------
void InstallWindowedHook() {
  // Load d3d9.dll and resolve Direct3DCreate9.
  HMODULE hD3D9 = GetModuleHandleA("d3d9.dll");
  if (!hD3D9)
    hD3D9 = LoadLibraryA("d3d9.dll");
  if (!hD3D9)
    return;

  auto fnCreate = reinterpret_cast<Direct3DCreate9_t>(
      GetProcAddress(hD3D9, "Direct3DCreate9"));
  if (!fnCreate)
    return;

  // Create a temporary IDirect3D9 object solely to read and patch its vtable.
  IDirect3D9 *pD3D = fnCreate(D3D_SDK_VERSION);
  if (!pD3D)
    return;

  void **vtable = *reinterpret_cast<void ***>(pD3D);
  void *old = PatchVTable(vtable, kD3D9_CreateDevice,
                          reinterpret_cast<void *>(&HookCreateDevice));
  if (old)
    g_origCreateDevice = reinterpret_cast<CreateDevice_t>(old);

  // Release the temp object. The vtable patch persists because all
  // IDirect3D9 instances in this process share the same vtable page.
  pD3D->Release();
}
