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
// The inventory base pointer is obtained from the hero's CThing via Fable's
// native object hierarchy (CMainGameComponent → CPlayerManager → CPlayer →
// CThing → TCI_INVENTORY component). All addresses are in fable_addresses.h.
// =============================================================================

#pragma once
