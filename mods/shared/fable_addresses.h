// =============================================================================
// Author       : Yaranorgoth
// Description  : All hardcoded Fable: The Lost Chapters virtual addresses.
//
// The module loads at 0x00400000 with no ASLR; all VAs are absolute.
// Update this file if you identify a different binary version.
// =============================================================================

#pragma once

#include <windows.h>

// ---------------------------------------------------------------------------
// AddItemToInventory — vfunc_80 on NInventory::VCTCInventoryBase
// ---------------------------------------------------------------------------

/// Absolute VA of AddItemToInventory.
static constexpr DWORD kAddItemToInventoryAddr = 0x005BF654u;

/// Bytes overwritten by the inline JMP patch (5-byte JMP + 3 NOPs to align).
static constexpr SIZE_T kPatchSize = 8u;

/// Stack bytes the callee must clean (__thiscall, 5 × 4-byte args = 20 bytes).
static constexpr DWORD kStackArgBytes = 20u;

// ---------------------------------------------------------------------------
// Hero inventory resolution chain (used by add_item_mod)
// ---------------------------------------------------------------------------

/// Pointer to CMainGameComponent (global singleton pointer).
static constexpr DWORD kMainGameComponentPtr = 0x13B86A0u;

/// Byte offset from CMainGameComponent to its CPlayerManager pointer.
static constexpr DWORD kPlayerManagerOffset = 28u;

// ---------------------------------------------------------------------------
// Native engine function addresses
// ---------------------------------------------------------------------------

/// CPlayerManager::GetMainPlayer  (__thiscall)
static constexpr DWORD kGetMainPlayerAddr    = 0x449970u;

/// CPlayer::GetCharacterThing  (__thiscall)
static constexpr DWORD kGetCharacterThingAddr = 0x487B70u;

/// CThing::HasTC  (__thiscall)
static constexpr DWORD kHasTCAddr            = 0x4118C0u;

/// CThing internal TC-node lookup  (__thiscall-like, receiver at arg+68)
static constexpr DWORD kGetTCNodeAddr        = 0x40F020u;

/// CCharString constructor  (__thiscall)
static constexpr DWORD kCCharStringCtorAddr  = 0x99EBF0u;

/// Engine object factory  (__fastcall)
static constexpr DWORD kCreateThingAddr      = 0x703210u;
