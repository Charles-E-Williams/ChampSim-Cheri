#ifndef CHERI_PREFETCH_UTILS_H
#define CHERI_PREFETCH_UTILS_H

#include <cstdint>
#include <iostream>
#include <optional>

#include "capability_memory.h"
#include "champsim.h"
#include "msl/lru_table.h"

namespace cheri {

constexpr uint32_t PERM_GLOBAL                    = (1u << 0);
constexpr uint32_t PERM_EXECUTE                   = (1u << 1);
constexpr uint32_t PERM_LOAD                      = (1u << 2);
constexpr uint32_t PERM_STORE                     = (1u << 3);
constexpr uint32_t PERM_LOAD_CAPABILITY           = (1u << 4);
constexpr uint32_t PERM_STORE_CAPABILITY          = (1u << 5);
constexpr uint32_t PERM_STORE_LOCAL_CAPABILITY    = (1u << 6);
constexpr uint32_t PERM_SEAL                      = (1u << 7);
constexpr uint32_t PERM_INVOKE                    = (1u << 8);
constexpr uint32_t PERM_UNSEAL                    = (1u << 9);
constexpr uint32_t PERM_ACCESS_SYSTEM_REGISTERS   = (1u << 10);
constexpr uint32_t PERM_SET_CID                   = (1u << 11);

constexpr bool has_load_permissions(uint32_t perms)     { return (perms & PERM_LOAD) != 0; }
constexpr bool has_store_permissions(uint32_t perms)    { return (perms & PERM_STORE) != 0; }
constexpr bool has_execute_permissions(uint32_t perms)  { return (perms & PERM_EXECUTE) != 0; }
constexpr bool has_load_cap_permissions(uint32_t perms) { return (perms & PERM_LOAD_CAPABILITY) != 0; }
constexpr bool has_seal_bit(uint32_t perms)             { return (perms & PERM_SEAL) != 0; }


// CHERI capabilities are 16-byte (128-bit) values, naturally aligned.
constexpr uint8_t CAP_ALIGNMENT_BITS = 4;
constexpr uint8_t CAP_ALIGNMENT_BYTES = 1u << CAP_ALIGNMENT_BITS;   // 16

class TLBClone {
private:
  static constexpr std::size_t TLB_SETS = 128;
  static constexpr std::size_t TLB_WAYS = 16;

  struct tlb_entry {
    uint64_t vpn;
    uint64_t ppn;

    auto index() const { return vpn; }
    auto tag() const   { return vpn; }
  };

  champsim::msl::lru_table<tlb_entry> table{TLB_SETS, TLB_WAYS};

  uint64_t stat_lookups = 0;
  uint64_t stat_hits    = 0;

public:
  // Add or update a translation.
  void fill(uint64_t va, uint64_t pa)
  {
    uint64_t vpn = va >> LOG2_PAGE_SIZE;
    uint64_t ppn = pa >> LOG2_PAGE_SIZE;
    table.fill({vpn, ppn});
  }

  // Translate a VA to PA.  Returns std::nullopt on a TLB miss.
  std::optional<uint64_t> translate(uint64_t va)
  {
    uint64_t vpn = va >> LOG2_PAGE_SIZE;
    stat_lookups++;

    auto hit = table.check_hit({vpn, 0});

    if (hit.has_value()) {
      stat_hits++;
      uint64_t page_offset = va & ((1ull << LOG2_PAGE_SIZE) - 1);
      return (hit->ppn << LOG2_PAGE_SIZE) | page_offset;
    }
    return std::nullopt;
  }

  // Overload for ChampSim address types.
  std::optional<champsim::address> translate(champsim::address va)
  {
    auto pa_val = translate(va.to<uint64_t>());
    if (pa_val.has_value())
      return champsim::address{pa_val.value()};
    return std::nullopt;
  }

  void print_stats() const
  {
    uint64_t misses = stat_lookups - stat_hits;
    double hit_rate = stat_lookups ? 100.0 * static_cast<double>(stat_hits) / static_cast<double>(stat_lookups) : 0.0;
    std::cout << "TLB Clone Stats: lookups: " << stat_lookups
              << "  hits: " << stat_hits
              << "  misses: " << misses
              << "  hit rate: " << hit_rate << "%\n";
  }
};


// Virtual address (base + offset).
inline champsim::address capability_cursor(const champsim::capability& cap)
{
  assert(cap.offset.to<uint64_t>() <= UINT64_MAX - cap.base.to<uint64_t>());
  return champsim::address{cap.base.to<uint64_t>() + cap.offset.to<uint64_t>()};
}

// Upper bound of capability (base + length), saturating at UINT64_MAX.
inline champsim::address capability_top(const champsim::capability& cap)
{
  uint64_t base   = cap.base.to<uint64_t>();
  uint64_t length = cap.length.to<uint64_t>();
  if (length == UINT64_MAX || base > (UINT64_MAX - length))
    return champsim::address{UINT64_MAX};
  return champsim::address{base + length};
}

// True pointer/memory capability: tag is set.
inline bool is_tag_valid(const champsim::capability& cap)
{
  return cap.tag;
}

// True if addr falls within [base, top).
inline bool in_bounds(champsim::address addr, champsim::address base, champsim::address top)
{
  return (addr.to<uint64_t>() >= base.to<uint64_t>()) && (addr.to<uint64_t>() < top.to<uint64_t>());
}

// True if any byte of the cache line overlaps [base, top).
inline bool overlaps_bounds(champsim::block_number block, champsim::address base, champsim::address top)
{
  uint64_t block_start = block.to<uint64_t>() << LOG2_BLOCK_SIZE;
  uint64_t block_end   = block_start | (BLOCK_SIZE - 1);
  return (block_start < top.to<uint64_t>()) && (block_end > base.to<uint64_t>());
}

// Remaining cache lines from block to the capability bound in the given direction.
// Returns 0 if already at or past the bound.
inline int64_t remaining_lines(champsim::block_number block, int direction,
                           champsim::address base, champsim::address top)
{
  if (direction == 0) return 0;
  if (base.to<uint64_t>() >= top.to<uint64_t>()) return 0;

  uint64_t current_blk = block.to<uint64_t>();
  uint64_t base_blk    = base.to<uint64_t>() >> LOG2_BLOCK_SIZE;
  uint64_t top_blk     = (top.to<uint64_t>() - 1) >> LOG2_BLOCK_SIZE;

  if (direction > 0)
    return (current_blk >= top_blk) ? 0 : static_cast<int64_t>(top_blk - current_blk);
  else
    return (current_blk <= base_blk) ? 0 : static_cast<int64_t>(current_blk - base_blk);
}

// Cache-line offset of the current access relative to the capability base.
inline int lines_from_cap_base(const champsim::capability& cap)
{
  return static_cast<int>(cap.offset.to<uint64_t>() >> LOG2_BLOCK_SIZE);
}

// Objects spanning at most one cache line have nothing useful to prefetch.
inline bool has_prefetchable_range(const champsim::capability& cap)
{
  uint64_t cursor = capability_cursor(cap).to<uint64_t>();
  uint64_t top    = capability_top(cap).to<uint64_t>();

  return (top > cursor) && (top - cursor > BLOCK_SIZE);
}

// True if issuing a prefetch for pf_addr is safe under cap:
// tag valid, not sealed, has load permission, and address within bounds.
inline bool prefetch_safe(champsim::address pf_addr, const champsim::capability& cap)
{
  if (!is_tag_valid(cap) || has_seal_bit(cap.permissions))
    return false;
  return in_bounds(pf_addr, cap.base, capability_top(cap)) && has_load_permissions(cap.permissions);
}


inline uint64_t hash_capability(const champsim::capability& cap)
{
  uint64_t b = cap.base.to<uint64_t>();
  uint64_t l = cap.length.to<uint64_t>();

  uint64_t h = b ^ (l + 0x9e3779b9 + (b << 6) + (b >> 2));
  h ^= (h >> 33);
  h *= 0xff51afd7ed558ccd;
  h ^= (h >> 33);
  return h;
}

// Virtual address of capability slot `slot_idx` within the cacheline
// containing `cl_va`.  Caller must ensure 0 <= slot_idx < CAP_SLOTS_PER_CL.
inline champsim::address slot_address(champsim::address cl_va, uint32_t slot_idx)
{
    uint64_t cl_base = cl_va.to<uint64_t>() & ~((1ULL << LOG2_BLOCK_SIZE) - 1);
    return champsim::address{cl_base + (static_cast<uint64_t>(slot_idx) << CAP_ALIGNMENT_BITS)};
}

// True if a capability discovered via CDP is a viable chase target:
// tagged, non-null cursor, unsealed, has load permission, and spans
// more than one cache line (worth prefetching into).
inline bool is_chaseable_target(const champsim::capability& cap)
{
  if (!is_tag_valid(cap))
    return false;
  if (capability_cursor(cap).to<uint64_t>() == 0)
    return false;
  if (has_seal_bit(cap.permissions))
    return false;
  if (!has_load_permissions(cap.permissions))
    return false;
  return has_prefetchable_range(cap);
}


inline void print_cap(const champsim::capability& cap)
{
  std::cout << std::hex
            << "base=0x" << cap.base.to<uint64_t>()
            << " length=0x" << cap.length.to<uint64_t>()
            << " offset=0x" << cap.offset.to<uint64_t>()
            << " perms=0x" << cap.permissions
            << " tag=" << cap.tag
            << std::dec << "\n";
}

} // namespace cheri

#endif