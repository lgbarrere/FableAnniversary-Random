// =============================================================================
// Author       : Yaranorgoth
// Description  : Mod that hooks the game function at 0x005BF654
//                (NInventory::VCTCInventoryBase::AddItemToInventory / vfunc_80)
//                and logs the call with all argument values every time it is
//                invoked while playing.
//
// Full signature (as recovered during the dump session):
//   char __thiscall AddItemToInventory(
//       NInventory::VCTCInventoryBase* this,  // ECX (implicit)
//       CThing*  item,             // param 1 – item CThing to add
//       bool     add_selected,     // param 2 – add as selected item
//       bool     add_quick_access, // param 3 – add to quick-access slot
//       int      price_bought_for, // param 4 – gold paid (0 = free)
//       bool     silent            // param 5 – suppress pickup sound/UI
//   );
//
// Hook strategy: 5-byte relative JMP at the function entry point stored in
// a page that has been made PAGE_EXECUTE_READWRITE.  A trampoline preserves
// the first 5 bytes so the original function can still be called.
// =============================================================================

#pragma once
