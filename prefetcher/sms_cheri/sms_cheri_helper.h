//=======================================================================================//
// File             : sms_cheri/sms_cheri_helper.h
// Description      : Auxiliary structures for CHERI-aware SMS prefetcher.
//                    Extends the original SMS (ISCA'06) entries with capability
//                    metadata so that spatial patterns can be tracked relative
//                    to object bounds instead of (or in addition to) page offsets.
//=======================================================================================//

#ifndef __SMS_CHERI_HELPER_H__
#define __SMS_CHERI_HELPER_H__

#include <stdint.h>

#include "../sms/bitmap.h"

// Filter Table entry.
// Records the first access to a new spatial region.
// When a capability is present the region is an object-relative 2KB chunk;
// otherwise it falls back to a physical-address-based 2KB region (stock SMS).
class FTEntry
{
public:
  uint64_t region_id;       // region identity (object-chunk VA or PA-based)
  uint64_t pc;
  uint32_t trigger_offset;  // CL index within region that triggered this entry

  // Capability metadata captured at insertion time.
  // Used later for bounds-checked prefetch generation.
  uint64_t cap_base;
  uint64_t cap_top;         // cap_base + cap_length
  uint64_t demand_pa_page;  // physical page of the triggering demand (for VA->PA)
  uint64_t demand_va_page;  // virtual page of the triggering demand
  bool     has_cap;         // true when entry was created with a valid capability

public:
  void reset()
  {
    region_id = 0xdeadbeef;
    pc = 0xdeadbeef;
    trigger_offset = 0;
    cap_base = 0;
    cap_top = 0;
    demand_pa_page = 0;
    demand_va_page = 0;
    has_cap = false;
  }
  FTEntry() { reset(); }
  ~FTEntry() {}
};

// Accumulation Table entry.
// Accumulates the spatial footprint bitmap for an active region.
class ATEntry
{
public:
  uint64_t region_id;
  uint64_t pc;
  uint32_t trigger_offset;
  Bitmap   pattern;
  uint32_t age;

  uint64_t cap_base;
  uint64_t cap_top;
  uint64_t demand_pa_page;
  uint64_t demand_va_page;
  bool     has_cap;

public:
  void reset()
  {
    region_id = pc = 0xdeadbeef;
    trigger_offset = 0;
    pattern.reset();
    age = 0;
    cap_base = 0;
    cap_top = 0;
    demand_pa_page = 0;
    demand_va_page = 0;
    has_cap = false;
  }
  ATEntry() { reset(); }
  ~ATEntry() {}
};

// Pattern History Table entry.
// Stores a learned spatial footprint bitmap indexed by a signature.
// The signature encodes (PC, trigger_offset); when the access had a
// capability the trigger_offset is object-relative, giving the PHT
// semantic awareness of data-structure layout.
class PHTEntry
{
public:
  uint64_t signature;
  Bitmap   pattern;
  uint32_t age;

public:
  void reset()
  {
    signature = 0xdeadbeef;
    pattern.reset();
    age = 0;
  }
  PHTEntry() { reset(); }
  ~PHTEntry() {}
};

#endif /* __SMS_CHERI_HELPER_H__ */