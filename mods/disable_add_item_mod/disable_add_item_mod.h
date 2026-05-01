// =============================================================================
// Author       : Yaranorgoth
// Description  : Hooks the game function at 0x005BF654
//                (NInventory::VCTCInventoryBase::AddItemToInventory / vfunc_80)
//                and DISABLES it — the original implementation is never called.
//                Every intercepted call is logged with the same detail level
//                as hook_logger_mod (call index, this, item pointer, def_id,
//                and a 16-DWORD memory dump of the item CThing).
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
// Hook strategy: 5-byte relative JMP at the function entry point.
//                No trampoline is built because the original is intentionally
//                suppressed; HookBody returns 0 immediately after logging.
// =============================================================================

#pragma once
