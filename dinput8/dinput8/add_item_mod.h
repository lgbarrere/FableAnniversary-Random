// =============================================================================
// Author       : Yaranorgoth
// Description  : Mod that adds an item to the hero's inventory by calling the
//                game's own AddItemToInventory function at address 0x005BF654.
//
// Full signature (vfunc_80 on NInventory::VCTCInventoryBase):
//   char __thiscall AddItemToInventory(
//       NInventory::VCTCInventoryBase* this,  // ECX (implicit)
//       CThing*  item,             // item CThing to add
//       bool     add_selected,     // add as the currently selected item
//       bool     add_quick_access, // also add to quick-access slot
//       int      price_bought_for, // gold paid for the item (0 = free/gifted)
//       bool     silent            // suppress pickup sound and UI notification
//   );
//
// The inventory base pointer is obtained from the hero's CThing via
// a known global hero-pointer and a simple offset chain uncovered in the
// same dump session.
// =============================================================================

#pragma once

/// Install the mod (called once from InitThread after the game has loaded).
/// Press F1 in-game to trigger; configure parameters in add_item_mod.cpp.
void InstallAddItemMod();
