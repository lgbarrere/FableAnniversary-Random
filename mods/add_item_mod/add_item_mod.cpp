// =============================================================================
// Author       : Yaranorgoth
// Description  : Mod that adds an item to the hero's inventory on F1 press,
//                by calling the game's own virtual function at 0x005BF654.
//
// ---------------------------------------------------------------------------
//  Full signature (vfunc_80 on NInventory::VCTCInventoryBase)
// ---------------------------------------------------------------------------
//   char __thiscall AddItemToInventory(
//       NInventory::VCTCInventoryBase* this,  // ECX (implicit, inventory obj)
//       CThing*  item,                        // param 1 – item CThing to add
//       bool     add_selected,                // param 2 – add as selected item
//       bool     add_quick_access,            // param 3 – add to quick-access
//       slot int      price_bought_for,            // param 4 – gold paid (0 =
//       free) bool     silent                       // param 5 – suppress
//       pickup sound/UI
//   );
//
// ---------------------------------------------------------------------------
//  How to obtain the inventory 'this' pointer (offset chain)
// ---------------------------------------------------------------------------
//
//  Fable TLC keeps the active hero via a global CThing* written by the engine.
//  The pointer chain below was observed in the function dump session for
//  module base 0x00400000:
//
//    Step 1 – Dereference the hero singleton:
//              g_pHeroThing = **(CThing***)(0x00983B70)
//              (the outer level is a pointer-to-pointer; adjust if ASLR moves
//              the module – but Fable TLC loads at 0x00400000 by default)
//
//    Step 2 – Walk to the inventory component:
//              pInventory  = *(void**)((BYTE*)g_pHeroThing + 0x3C4)
//
//  If either pointer is null the mod skips the call safely.
//
// ---------------------------------------------------------------------------
//  To add a different item:
//    Replace ITEM_THING_ID below with the CThing UID of the item you want.
//    The function accepts a live CThing* (an already-spawned object), not an
//    abstract definition ID. The simplest approach is to search for a nearby
//    CThing that matches the desired item type, or to call the game's own
//    object factory (out of scope for this demo – see sub_1BD404 in the dump).
//
//    For the demo we therefore look for a pre-existing item in the world
//    whose UID is stored in DEMO_ITEM_UID.  Set DEMO_ITEM_UID to 0 to skip
//    the call (safe no-op).
// =============================================================================

#include "add_item_mod.h"

#include <cstdarg>
#include <cstdio>
#include <windows.h>

// ---------------------------------------------------------------------------
// Configuration — edit these before building
// ---------------------------------------------------------------------------

/// Virtual address of NInventory::VCTCInventoryBase::vfunc_80 in Fable.exe.
/// Do NOT change unless you identified a different binary version.
static constexpr DWORD kAddItemAddr = 0x005BF654u;

/// Global pointer-to-pointer that holds the active hero CThing*.
/// Observed at module base 0x00400000; chain: **ptr → CThing*.
static constexpr DWORD kHeroPtrPtr = 0x00983B70u;

/// Byte offset from the hero CThing* to its NInventory::VCTCInventoryBase*.
/// Adjust if your reversing reveals a different offset.
static constexpr DWORD kInventoryOffset = 0x3C4u;

// (kDemoItemUID has been removed because the game uses different definition IDs 
// for seemingly identical items depending on the chest or game state).

// ---------------------------------------------------------------------------
// Local logger for this mod
// ---------------------------------------------------------------------------
char g_LogPath[MAX_PATH] = "add_item_mod.log";
HINSTANCE g_hInstance = nullptr;

void Log(const char *fmt, ...) {
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
// Function pointer type for AddItemToInventory
// ---------------------------------------------------------------------------
using AddItemToInventory_t = char(__thiscall *)(
    void * /*pThis*/, void * /*item*/, bool /*add_selected*/,
    bool /*add_quick_access*/, int /*price_bought_for*/, bool /*silent*/
);

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------
namespace {

/// Safely read a DWORD from an arbitrary VA; returns false if the address is
/// not backed by readable memory (avoids access violations during early init).
bool SafeRead(LPCVOID addr, DWORD &out) {
  SIZE_T bytesRead = 0;
  return ReadProcessMemory(GetCurrentProcess(), addr, &out, sizeof(DWORD),
                           &bytesRead) &&
         bytesRead == sizeof(DWORD);
}

// ---------------------------------------------------------------------------
// Resolve the hero's inventory pointer at call time.
// ---------------------------------------------------------------------------
void *GetHeroInventory() {
  // Use Fable's native object hierarchy to get the player's inventory component reliably!

  // 1. Get CMainGameComponent
  void** ppMainGameComponent = reinterpret_cast<void**>(0x13B86A0);
  if (IsBadReadPtr(ppMainGameComponent, 4) || !*ppMainGameComponent) {
    Log("[AddItemMod] NativeInventory: Failed to read CMainGameComponent.");
    return nullptr;
  }
  void* pMainGameComponent = *ppMainGameComponent;

  // 2. Get CPlayerManager (offset 28)
  void** ppPlayerManager = reinterpret_cast<void**>((char*)pMainGameComponent + 28);
  if (IsBadReadPtr(ppPlayerManager, 4) || !*ppPlayerManager) {
    Log("[AddItemMod] NativeInventory: Failed to read CPlayerManager.");
    return nullptr;
  }
  void* pPlayerManager = *ppPlayerManager;

  // 3. Get Main CPlayer
  auto getMainPlayer = reinterpret_cast<void*(__thiscall*)(void*)>(0x449970);
  void* pPlayer = getMainPlayer(pPlayerManager);
  if (!pPlayer) {
    Log("[AddItemMod] NativeInventory: Failed to get Main Player.");
    return nullptr;
  }

  // 4. Get Character CThing
  auto getCharacterThing = reinterpret_cast<void*(__thiscall*)(void*)>(0x487B70);
  void* pCharacterThing = getCharacterThing(pPlayer);
  if (!pCharacterThing) {
    Log("[AddItemMod] NativeInventory: Failed to get Character Thing.");
    return nullptr;
  }

  // 5. GetTC(17) -> TCI_INVENTORY
  int tc_id = 17;
  auto hasTC = reinterpret_cast<bool(__thiscall*)(void*, int)>(0x4118C0);
  if (!hasTC(pCharacterThing, tc_id)) {
    Log("[AddItemMod] NativeInventory: Hero has no TCI_INVENTORY!");
    return nullptr;
  }

  auto getTCNode = reinterpret_cast<int(__thiscall*)(int, int*)>(0x40F020);
  int v29 = tc_id;
  int v5 = getTCNode((int)pCharacterThing + 68, &v29);
  if (v5 == *(int*)((char*)pCharacterThing + 72) || *(int*)v5 > tc_id) {
    v5 = *(int*)((char*)pCharacterThing + 72);
  }
  void* pInventory = *(void**)(v5 + 4);

  Log("[AddItemMod] NativeInventory successfully resolved: 0x%p", pInventory);
  return pInventory;
}

// ---------------------------------------------------------------------------
// Scan the game's CThing registry/linked-list for an object with the given UID.
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// Engine Types
// ---------------------------------------------------------------------------
struct CVector {
  float X, Y, Z;
};

class CCharString {
public:
  int unk;
  char* str;
  // Pad if necessary, FableMenu assumes 8 bytes is sufficient for parameter passing.
};

// ---------------------------------------------------------------------------
// Dynamic Item Spawning
// ---------------------------------------------------------------------------
void *CreateFableItem(DWORD definition_id) {
  if (definition_id == 0) return nullptr;

  Log("[AddItemMod] Attempting to invoke Fable Engine CreateThing(ID=%lu)...", definition_id);

  // 1. Initialize the CCharString structure using the native constructor (0x99EBF0).
  // This is safe because we are executing on the main thread via the WM_SPAWN_ITEM message,
  // so the Engine's Thread Local Storage (TLS) allocator is available.
  Log("[AddItemMod] Allocating CCharString on stack...");
  CCharString sysName;
  auto initCCharString = reinterpret_cast<void(__thiscall*)(CCharString*, const char*, int)>(0x99EBF0);
  Log("[AddItemMod] Calling CCharString constructor at 0x99EBF0 with 'Item'...");
  initCCharString(&sysName, "Item", -1);
  Log("[AddItemMod] CCharString constructor succeeded.");

  // 2. Initialize origin positioning
  CVector origin = {0.0f, 0.0f, 0.0f};

  // 3. Inject into the native CThing memory builder!
  Log("[AddItemMod] Calling CreateThing(0x703210)...");
  auto createCThing = reinterpret_cast<void* (__fastcall*)(int, CVector*, int, int, int, CCharString*)>(0x703210);
  void* newlyAllocatedItem = createCThing(definition_id, &origin, 0, 0, 0, &sysName);

  Log("[AddItemMod] CreateThing returned completely unlinked memory structure -> 0x%p", newlyAllocatedItem);

  return newlyAllocatedItem;
}

// ---------------------------------------------------------------------------
// Window Subclassing for Main Thread Execution
// ---------------------------------------------------------------------------
#define WM_SPAWN_ITEM (WM_USER + 101)

HWND g_GameHwnd = nullptr;
WNDPROC g_OriginalWndProc = nullptr;

void DoAddItem() {
  void *inventory = GetHeroInventory();
  if (!inventory || IsBadReadPtr(inventory, 4)) {
    Log("[AddItemMod] Cannot add item: inventory pointer unavailable.");
    return;
  }

  // -------------------------------------------------------------------------
  // Elixir of Life Definition ID
  // We discovered from our RTTI extraction that the static Definition ID 
  // for the Elixir of Life is 4294 in Fable The Lost Chapters.
  // We can directly spawn it without needing to open a chest first!
  // -------------------------------------------------------------------------
  int real_def_id = 4294;

  Log("[AddItemMod] Allocating Elixir of Life using Native def_id=%d...", real_def_id);

  // We are on the main thread now (WM_SPAWN_ITEM), so the Engine TLS allocator works!
  void *newItem = CreateFableItem(real_def_id);
  if (!newItem || IsBadReadPtr(newItem, 4)) {
    Log("[AddItemMod] CreateFableItem failed to allocate new item!");
    return;
  }

  auto fn = reinterpret_cast<AddItemToInventory_t>(kAddItemAddr);

  Log("[AddItemMod] Calling AddItemToInventory(inventory=0x%p, newItem=0x%p) ...",
      inventory, newItem);

  char result = fn(inventory, newItem,
                   /*add_selected    =*/false,
                   /*add_quick_access=*/true,
                   /*price_bought_for=*/0,
                   /*silent          =*/false);

  Log("[AddItemMod] AddItemToInventory returned %d.", (int)(unsigned char)result);
}

LRESULT CALLBACK GameWndProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
  if (uMsg == WM_SPAWN_ITEM) {
    Log("[AddItemMod] Main thread received WM_SPAWN_ITEM!");
    DoAddItem();
    return 0; // Handled
  }
  return CallWindowProc(g_OriginalWndProc, hwnd, uMsg, wParam, lParam);
}

BOOL CALLBACK EnumWindowsProc(HWND hwnd, LPARAM lParam) {
  DWORD wndPid = 0;
  GetWindowThreadProcessId(hwnd, &wndPid);
  if (wndPid == GetCurrentProcessId()) {
    // Make sure it's not a hidden console or sub-window
    if (GetWindow(hwnd, GW_OWNER) == nullptr && IsWindowVisible(hwnd)) {
      g_GameHwnd = hwnd;
      return FALSE; // Stop enumerating
    }
  }
  return TRUE;
}

// ---------------------------------------------------------------------------
// Background polling thread — fires DoAddItem() once per F1 press via WndProc.
// ---------------------------------------------------------------------------
DWORD WINAPI AddItemThread(LPVOID) {
  Log("[AddItemMod] Waiting for Fable game window...");
  while (!g_GameHwnd) {
    EnumWindows(EnumWindowsProc, 0);
    Sleep(100);
  }

  Log("[AddItemMod] Found Game HWND: 0x%p. Subclassing...", g_GameHwnd);
  g_OriginalWndProc = (WNDPROC)SetWindowLongPtr(g_GameHwnd, GWLP_WNDPROC, (LONG_PTR)GameWndProc);

  Log("[AddItemMod] Polling thread started. Press F1 in-game to duplicate last item.");

  bool wasDown = false;
  while (true) {
    bool isDown = (GetAsyncKeyState(VK_F1) & 0x8000) != 0;
    if (isDown && !wasDown) {
      Log("[AddItemMod] F1 pressed — posting WM_SPAWN_ITEM to main thread.");
      PostMessage(g_GameHwnd, WM_SPAWN_ITEM, 0, 0);
    }
    wasDown = isDown;
    Sleep(50); // 20 Hz poll — low CPU overhead
  }
  return 0;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Public entry point (DllMain)
// ---------------------------------------------------------------------------
BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) {
    DisableThreadLibraryCalls(hModule);
    g_hInstance = hModule;

    // Resolve the absolute path to this DLL so we can place logs next to it
    if (GetModuleFileNameA(hModule, g_LogPath, MAX_PATH)) {
      char *lastSlash = strrchr(g_LogPath, '\\');
      if (lastSlash) {
        strcpy_s(lastSlash + 1, MAX_PATH - (lastSlash - g_LogPath) - 1,
                 "add_item_mod.log");
      }
    }

    // Truncate the log
    FILE *fp = nullptr;
    fopen_s(&fp, g_LogPath, "w");
    if (fp)
      fclose(fp);

    Log("[AddItemMod] Loaded standalone AddItemToInventory mod (target "
        "VA=0x%08X).",
        kAddItemAddr);
    CreateThread(nullptr, 0, AddItemThread, nullptr, 0, nullptr);
  }
  return TRUE;
}
