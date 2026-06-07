// =============================================================================
// Author       : Yaranorgoth
// Description  : Helper for snapshotting the first 64 bytes of a CThing object.
//
// Used by hook_logger_mod and disable_add_item_mod to log item metadata
// without risking an access violation on a potentially invalid pointer.
// =============================================================================

#pragma once

#include "safe_memory.h"
#include <windows.h>

// ---------------------------------------------------------------------------
// CThingDump — snapshot of the first 16 DWORDs (64 bytes) of a CThing.
// ---------------------------------------------------------------------------
struct CThingDump {
  DWORD w[16] = {};

  /// Object Definition ID — lives at offset 0x18 (DWORD index 6).
  DWORD def_id = 0;
};

// ---------------------------------------------------------------------------
// ReadCThingDump — safely reads up to 64 bytes from a CThing pointer.
//
// Returns a zeroed CThingDump if item is null or points to unreadable memory.
// ---------------------------------------------------------------------------
inline CThingDump ReadCThingDump(void *item) {
  CThingDump d{};
  if (SafeReadBuffer(item, d.w, sizeof(d.w))) {
    d.def_id = d.w[6];
  }
  return d;
}
