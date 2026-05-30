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
//       bool     add_quick_access,            // param 3 – add to quick-access slot
//       int      price_bought_for,            // param 4 – gold paid (0 = free)
//       bool     silent                       // param 5 – suppress pickup sound/UI
//   );
//
// ---------------------------------------------------------------------------
//  How to obtain the inventory 'this' pointer
// ---------------------------------------------------------------------------
//
//  Fable TLC exposes the active hero through a native object hierarchy:
//    1. CMainGameComponent  — global singleton at kMainGameComponentPtr
//    2. CPlayerManager      — at offset kPlayerManagerOffset within it
//    3. CPlayer             — via CPlayerManager::GetMainPlayer  (kGetMainPlayerAddr)
//    4. CThing (hero)       — via CPlayer::GetCharacterThing     (kGetCharacterThingAddr)
//    5. Inventory component — via CThing::HasTC + TC-node lookup (TCI_INVENTORY = 17)
//
//  All addresses are defined in ../shared/fable_addresses.h.
//
// ---------------------------------------------------------------------------
//  Item IDs (Fable TLC definition IDs for common potions)
// ---------------------------------------------------------------------------
//   4292 – Health Potion
//   4293 – Will Potion
//   4294 – Elixir of Life
// =============================================================================

#include "add_item_mod.h"

#include "../shared/fable_addresses.h"
#include "../shared/fable_types.h"
#include "../shared/mod_log.h"

#include <windows.h>

// Exposed to disable_add_item_mod.cpp to not cancel item additions from add_item_mod
extern "C" __declspec(dllexport) bool g_AddingItemFromMod = false;

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------
namespace {

/// Safely read a DWORD from an arbitrary VA; returns false on unreadable memory.
bool SafeRead(LPCVOID addr, DWORD &out) {
  SIZE_T bytesRead = 0;
  return ReadProcessMemory(GetCurrentProcess(), addr, &out, sizeof(DWORD),
                           &bytesRead) &&
         bytesRead == sizeof(DWORD);
}

// ---------------------------------------------------------------------------
// GetHeroInventory — walks Fable's native object hierarchy to the inventory.
// ---------------------------------------------------------------------------
void *GetHeroInventory() {
  // 1. CMainGameComponent
  auto **ppMainGame = reinterpret_cast<void **>(kMainGameComponentPtr);
  if (IsBadReadPtr(ppMainGame, 4) || !*ppMainGame) {
    Log("[AddItemMod] GetHeroInventory: Failed to read CMainGameComponent.");
    return nullptr;
  }
  void *pMainGame = *ppMainGame;

  // 2. CPlayerManager (offset kPlayerManagerOffset within CMainGameComponent)
  auto **ppPlayerManager =
      reinterpret_cast<void **>(static_cast<char *>(pMainGame) + kPlayerManagerOffset);
  if (IsBadReadPtr(ppPlayerManager, 4) || !*ppPlayerManager) {
    Log("[AddItemMod] GetHeroInventory: Failed to read CPlayerManager.");
    return nullptr;
  }
  void *pPlayerManager = *ppPlayerManager;

  // 3. Main CPlayer
  auto getMainPlayer = reinterpret_cast<void *(__thiscall *)(void *)>(kGetMainPlayerAddr);
  void *pPlayer = getMainPlayer(pPlayerManager);
  if (!pPlayer) {
    Log("[AddItemMod] GetHeroInventory: Failed to get Main Player.");
    return nullptr;
  }

  // 4. Hero CThing
  auto getCharThing = reinterpret_cast<void *(__thiscall *)(void *)>(kGetCharacterThingAddr);
  void *pHero = getCharThing(pPlayer);
  if (!pHero) {
    Log("[AddItemMod] GetHeroInventory: Failed to get Character Thing.");
    return nullptr;
  }

  // 5. TCI_INVENTORY (TC id = 17)
  constexpr int kTCInventory = 17;
  auto hasTC = reinterpret_cast<bool(__thiscall *)(void *, int)>(kHasTCAddr);
  if (!hasTC(pHero, kTCInventory)) {
    Log("[AddItemMod] GetHeroInventory: Hero has no TCI_INVENTORY!");
    return nullptr;
  }

  auto getTCNode = reinterpret_cast<int(__thiscall *)(int, int *)>(kGetTCNodeAddr);
  int v29 = kTCInventory;
  int v5  = getTCNode(reinterpret_cast<int>(pHero) + 68, &v29);
  if (v5 == *reinterpret_cast<int *>(static_cast<char *>(pHero) + 72) ||
      *reinterpret_cast<int *>(v5) > kTCInventory) {
    v5 = *reinterpret_cast<int *>(static_cast<char *>(pHero) + 72);
  }

  void *pInventory = *reinterpret_cast<void **>(v5 + 4);
  Log("[AddItemMod] GetHeroInventory resolved: 0x%p", pInventory);
  return pInventory;
}

// ---------------------------------------------------------------------------
// CreateFableItem — spawns a new CThing via the engine object factory.
// Must be called on the main thread (Engine TLS allocator requirement).
// ---------------------------------------------------------------------------
void *CreateFableItem(DWORD definition_id) {
  if (definition_id == 0)
    return nullptr;

  Log("[AddItemMod] CreateFableItem: spawning def_id=%lu ...", definition_id);

  // Build a CCharString named "Item" using the native constructor.
  CCharString sysName;
  auto initStr = reinterpret_cast<void(__thiscall *)(CCharString *, const char *, int)>(
      kCCharStringCtorAddr);
  initStr(&sysName, "Item", -1);

  // Zero-origin spawn position.
  CVector origin = {0.0f, 0.0f, 0.0f};

  // Call the engine factory (__fastcall).
  auto createThing = reinterpret_cast<void *(__fastcall *)(
      int, CVector *, int, int, int, CCharString *)>(kCreateThingAddr);
  void *pItem = createThing(static_cast<int>(definition_id), &origin, 0, 0, 0, &sysName);

  Log("[AddItemMod] CreateFableItem returned: 0x%p", pItem);
  return pItem;
}

// ---------------------------------------------------------------------------
// Window subclassing — executes DoAddItem on the game's main thread.
// ---------------------------------------------------------------------------
#define WM_SPAWN_ITEM (WM_USER + 101)

HWND    g_GameHwnd       = nullptr;
WNDPROC g_OriginalWndProc = nullptr;

void DoAddItem() {
  void *inventory = GetHeroInventory();
  if (!inventory || IsBadReadPtr(inventory, 4)) {
    Log("[AddItemMod] Cannot add item: inventory pointer unavailable.");
    return;
  }

  // Elixir of Life (definition ID 4294).
  constexpr int kElixirOfLifeId = 4294;
  Log("[AddItemMod] Spawning Elixir of Life (def_id=%d)...", kElixirOfLifeId);

  void *newItem = CreateFableItem(kElixirOfLifeId);
  if (!newItem || IsBadReadPtr(newItem, 4)) {
    Log("[AddItemMod] CreateFableItem failed.");
    return;
  }

  auto fn = reinterpret_cast<AddItemToInventory_t>(kAddItemToInventoryAddr);
  Log("[AddItemMod] Calling AddItemToInventory(inv=0x%p, item=0x%p) ...",
      inventory, newItem);

  g_AddingItemFromMod = true;
  char result = fn(inventory, newItem,
                   /*add_selected    =*/false,
                   /*add_quick_access=*/true,
                   /*price_bought_for=*/0,
                   /*silent          =*/false);
  g_AddingItemFromMod = false;

  Log("[AddItemMod] AddItemToInventory returned %d.", (int)(unsigned char)result);
}

LRESULT CALLBACK GameWndProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
  if (uMsg == WM_SPAWN_ITEM) {
    Log("[AddItemMod] Main thread received WM_SPAWN_ITEM.");
    DoAddItem();
    return 0;
  }
  return CallWindowProc(g_OriginalWndProc, hwnd, uMsg, wParam, lParam);
}

BOOL CALLBACK EnumWindowsProc(HWND hwnd, LPARAM) {
  DWORD wndPid = 0;
  GetWindowThreadProcessId(hwnd, &wndPid);
  if (wndPid == GetCurrentProcessId() &&
      GetWindow(hwnd, GW_OWNER) == nullptr &&
      IsWindowVisible(hwnd)) {
    g_GameHwnd = hwnd;
    return FALSE; // Stop enumerating
  }
  return TRUE;
}

// ---------------------------------------------------------------------------
// AddItemThread — polls F1 and posts WM_SPAWN_ITEM to the main thread.
// ---------------------------------------------------------------------------
DWORD WINAPI AddItemThread(LPVOID) {
  Log("[AddItemMod] Waiting for Fable game window...");
  while (!g_GameHwnd) {
    EnumWindows(EnumWindowsProc, 0);
    Sleep(100);
  }

  Log("[AddItemMod] Found game HWND: 0x%p. Subclassing...", g_GameHwnd);
  g_OriginalWndProc = reinterpret_cast<WNDPROC>(
      SetWindowLongPtr(g_GameHwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(GameWndProc)));

  Log("[AddItemMod] Polling thread started. Press F1 to spawn an Elixir of Life.");

  bool wasDown = false;
  while (true) {
    bool isDown = (GetAsyncKeyState(VK_F1) & 0x8000) != 0;
    if (isDown && !wasDown) {
      Log("[AddItemMod] F1 pressed — posting WM_SPAWN_ITEM to main thread.");
      PostMessage(g_GameHwnd, WM_SPAWN_ITEM, 0, 0);
    }
    wasDown = isDown;
    Sleep(50); // 20 Hz poll
  }
  return 0;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// DllMain
// ---------------------------------------------------------------------------
BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) {
    DisableThreadLibraryCalls(hModule);
    InitModLog(hModule, "add_item_mod.log");
    Log("[AddItemMod] Loaded. Target VA=0x%08X.", kAddItemToInventoryAddr);
    CreateThread(nullptr, 0, AddItemThread, nullptr, 0, nullptr);
  }
  return TRUE;
}
