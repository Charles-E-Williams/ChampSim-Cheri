#ifndef __KRATOS_HELPER_H__
#define __KRATOS_HELPER_H__

#include <cstdint>

#include "bitmap.h"
#include "cheri.h"       // champsim::capability, champsim::address

namespace kratos_helper {

// ============================================================================
// Translation Cache Entry
// ============================================================================
// Maps a virtual page number to a physical page number.
// Passively populated from every L2 demand access that reaches the prefetcher.
// Used to translate prefetch target VAs (computed from capability bounds)
// into PAs for the PIPT L2.
// ============================================================================

struct TlbEntry {
  uint64_t v_page = 0;       // virtual page number  (VA >> 12)
  uint64_t p_page = 0;       // physical page number (PA >> 12)
  uint8_t  age    = 0;       // LRU counter within the set
  bool     valid  = false;

  void reset()
  {
    v_page = 0;
    p_page = 0;
    age    = 0;
    valid  = false;
  }
};

// ============================================================================
// Filter Table Entry
// ============================================================================
// Records the *first* access to a 4 KB region.  No prediction is issued.
// On the *second* access (filter table hit), the entry is promoted to the
// Accumulation Table.
// ============================================================================

struct FilterEntry {
  uint64_t p_page         = 0;       // physical page tag  (PA >> 12)
  uint64_t v_page         = 0;       // virtual page tag   (VA >> 12)
  uint64_t ip             = 0;       // triggering instruction pointer
  uint8_t  trigger_offset = 0;       // cacheline offset within region (0–63)

  // Authorizing capability from the triggering access.
  // cap.tag == true indicates a valid capability is present.
  champsim::capability cap{};

  void reset()
  {
    p_page = v_page = ip = 0;
    trigger_offset = 0;
    cap = champsim::capability{};
  }

  FilterEntry() { reset(); }
  ~FilterEntry() = default;
};

// ============================================================================
// Accumulation Table Entry
// ============================================================================
// Created when a region receives its second access (promoted from FT).
// Tracks which cachelines within the 4 KB region are demand-accessed.
// On eviction, the learned pattern is written to the PHT.
// ============================================================================

struct AccEntry {
  uint64_t p_page         = 0;       // physical page tag
  uint64_t v_page         = 0;       // virtual page tag
  uint64_t ip             = 0;       // triggering IP (from filter entry)
  uint8_t  trigger_offset = 0;       // original trigger offset

  // Authorizing capability, used for bounds masking.
  champsim::capability cap{};

  Bitmap   pattern{};                // which cachelines were demand-accessed
  Bitmap   bounds_mask{};            // which cachelines are within capability bounds
  Bitmap   pointer_map{};            // which cachelines contained confirmed pointers
  uint32_t age = 0;                  // LRU counter

  void reset()
  {
    p_page = v_page = ip = 0;
    trigger_offset = 0;
    cap = champsim::capability{};
    pattern.reset();
    bounds_mask.reset();
    pointer_map.reset();
    age = 0;
  }

  AccEntry() { reset(); }
  ~AccEntry() = default;
};

// ============================================================================
// Pattern History Table Entry
// ============================================================================
// Stores a learned spatial footprint, keyed by a primary signature
// hash(IP, trigger_offset) and an optional secondary (type) signature
// hash(cap_length, trigger_offset).
//
// On a new region access, the PHT is consulted to predict which cachelines
// will be accessed.  Predicted positions are masked by capability bounds
// before prefetches are issued.
// ============================================================================

struct PHTEntry {
  uint64_t signature       = 0;      // primary key: hash(IP, trigger_offset)
  uint64_t type_signature  = 0;      // secondary key: hash(cap_length, trigger_offset)
  Bitmap   pattern{};                // learned spatial footprint
  Bitmap   pointer_map{};            // which positions held confirmed pointers
  uint8_t  confidence      = 0;      // [0, 7] saturating — primary path
  uint8_t  type_confidence = 0;      // [0, 7] saturating — type path
  uint32_t age             = 0;

  void reset()
  {
    signature = type_signature = 0;
    pattern.reset();
    pointer_map.reset();
    confidence = type_confidence = 0;
    age = 0;
  }

  PHTEntry() { reset(); }
  ~PHTEntry() = default;
};

// ============================================================================
// Stride Table Entry
// ============================================================================
// Used for lru_table<>-based set-associative lookup, keyed by IP.
// Handles two cases:
//   1) Large capabilities (> 256 KB) where spatial footprint tracking is
//      infeasible — stride with bounds clipping is more appropriate.
//   2) Accesses without a valid capability (~37%) — standard IP-stride
//      with page-boundary constraints.
//
// index() and tag() are required by champsim::msl::lru_table.
// ============================================================================

struct StrideEntry {
  champsim::address ip{};                                 // instruction pointer
  champsim::block_number last_cl_addr{};                  // last cacheline address (PA)
  uint64_t last_va              = 0;                      // last virtual address (for cap bounds)
  champsim::block_number::difference_type last_stride{};  // stride in cachelines

  // Authorizing capability for bounds clipping.
  // cap.tag == true indicates valid capability info.
  champsim::capability cap{};

  uint8_t confidence = 0;                                 // [0, 7] saturating

  // Required by champsim::msl::lru_table — index and tag derived from IP.
  auto index() const
  {
    using namespace champsim::data::data_literals;
    return ip.slice_upper<2_b>();
  }

  auto tag() const
  {
    using namespace champsim::data::data_literals;
    return ip.slice_upper<2_b>();
  }
};

// ============================================================================
// Pointer Buffer Entry
// ============================================================================
// Records a capability discovered in a cacheline during pointer discovery.
// Populated selectively on demand misses to small objects with LOAD_CAP
// permission.  If a subsequent demand access hits the target, the entry
// is "confirmed" and recursive prefetching is triggered for the target.
//
// Both target_va and target_pa are stored so that confirmation can be
// performed as a fast PA comparison against incoming demands (avoiding
// a translation cache lookup on every access).
// ============================================================================

struct PtrBufEntry {
  uint64_t source_v_page     = 0;    // region containing the pointer (VA >> 12)
  uint8_t  source_cl_off     = 0;    // cacheline offset within region
  uint8_t  slot_index        = 0;    // which cap slot in the cacheline (0–3)
  uint64_t source_ip         = 0;    // IP that accessed the source

  uint64_t target_va         = 0;    // virtual address the pointer points to
  uint64_t target_pa         = 0;    // physical address (translated at discovery time)

  // Target capability — used for bounds-aware recursive prefetching
  // when the pointer is confirmed.
  champsim::capability target_cap{};

  bool     confirmed         = false; // was target subsequently demand-accessed?
  bool     valid             = false;

  void reset()
  {
    source_v_page = source_ip = 0;
    source_cl_off = slot_index = 0;
    target_va = target_pa = 0;
    target_cap = champsim::capability{};
    confirmed = valid = false;
  }

  PtrBufEntry() { reset(); }
  ~PtrBufEntry() = default;
};

} // namespace kratos_helper

#endif /* __KRATOS_HELPER_H__ */