// =============================================================================
// Author       : Yaranorgoth
// Description  : Fable engine type definitions shared across mod DLLs.
// =============================================================================

#pragma once

// ---------------------------------------------------------------------------
// AddItemToInventory function pointer
// ---------------------------------------------------------------------------

/// Signature of NInventory::VCTCInventoryBase::AddItemToInventory (vfunc_80).
///   param 1  item             — live CThing* to add
///   param 2  add_selected     — mark as currently selected item
///   param 3  add_quick_access — also slot into quick-access bar
///   param 4  price_bought_for — gold paid (0 = gifted / spawned)
///   param 5  silent           — suppress pickup sound and UI notification
using AddItemToInventory_t = char(__thiscall *)(
    void * /*pThis*/,
    void * /*item*/,
    bool   /*add_selected*/,
    bool   /*add_quick_access*/,
    int    /*price_bought_for*/,
    bool   /*silent*/
);

// ---------------------------------------------------------------------------
// Basic engine structs (used when calling native constructors / factories)
// ---------------------------------------------------------------------------

/// Fable 3D vector — passed as spawn origin to the CreateThing factory.
struct CVector {
  float X, Y, Z;
};

/// Fable narrow string — 8-byte ABI layout assumed by the CCharString ctor.
class CCharString {
public:
  int   unk;
  char *str;
};
