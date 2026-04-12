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

/// UID of an item CThing already present in the world to add to inventory.
/// Set to 0 to disable (the mod will log a message but not call the function).
/// Find valid UIDs via a memory scanner while in-game.
static constexpr DWORD kDemoItemUID = 0u;

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
  DWORD heroHolder = 0;
  if (!SafeRead(reinterpret_cast<LPCVOID>(kHeroPtrPtr), heroHolder) ||
      heroHolder == 0) {
    Log("[AddItemMod] Could not read hero pointer holder at 0x%08X.",
        kHeroPtrPtr);
    return nullptr;
  }

  DWORD heroPtr = 0;
  if (!SafeRead(reinterpret_cast<LPCVOID>(heroHolder), heroPtr) ||
      heroPtr == 0) {
    Log("[AddItemMod] Hero CThing* is null (hero not loaded yet?).");
    return nullptr;
  }

  DWORD invPtr = 0;
  if (!SafeRead(reinterpret_cast<LPCVOID>(heroPtr + kInventoryOffset),
                invPtr) ||
      invPtr == 0) {
    Log("[AddItemMod] Inventory component is null (offset 0x%X from CThing "
        "0x%08X).",
        kInventoryOffset, heroPtr);
    return nullptr;
  }

  return reinterpret_cast<void *>(invPtr);
}

// ---------------------------------------------------------------------------
// Scan the game's CThing registry/linked-list for an object with the given UID.
// ---------------------------------------------------------------------------
void *FindCThingByUID(DWORD uid) {
  if (uid == 0)
    return nullptr;

  Log("[AddItemMod] FindCThingByUID: UID=0x%08X — real lookup not implemented "
      "yet.",
      uid);
  return nullptr;
}

// ---------------------------------------------------------------------------
// Perform the actual AddItemToInventory call.
// ---------------------------------------------------------------------------
void DoAddItem() {
  // Resolve the inventory 'this' pointer.
  void *pInventory = GetHeroInventory();
  if (!pInventory) {
    Log("[AddItemMod] Cannot add item: inventory pointer unavailable.");
    return;
  }

  // Resolve a CThing* for the item to add.
  void *pItem = FindCThingByUID(kDemoItemUID);
  if (!pItem) {
    Log("[AddItemMod] Cannot add item: no valid CThing* for UID 0x%08X "
        "(set kDemoItemUID to a live item UID, or implement FindCThingByUID).",
        kDemoItemUID);
    return;
  }

  // Cast the known address to our function pointer type.
  auto fn = reinterpret_cast<AddItemToInventory_t>(kAddItemAddr);

  Log("[AddItemMod] Calling AddItemToInventory(inventory=0x%p, item=0x%p) ...",
      pInventory, pItem);

  char result = fn(pInventory, pItem,
                   /*add_selected    =*/false,
                   /*add_quick_access=*/false,
                   /*price_bought_for=*/0,
                   /*silent          =*/false);

  Log("[AddItemMod] AddItemToInventory returned %d.",
      (int)(unsigned char)result);
}

// ---------------------------------------------------------------------------
// Background polling thread — fires DoAddItem() once per F1 press.
// ---------------------------------------------------------------------------
DWORD WINAPI AddItemThread(LPVOID) {
  Log("[AddItemMod] Polling thread started. Press F1 in-game to add item.");

  bool wasDown = false;
  while (true) {
    bool isDown = (GetAsyncKeyState(VK_F1) & 0x8000) != 0;
    if (isDown && !wasDown) {
      Log("[AddItemMod] F1 pressed — attempting to add item.");
      DoAddItem();
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
