#ifndef CHERI_UTILS_H
#define CHERI_UTILS_H

#include <cstdint>
#include <iostream>
#include <optional>

#include "address.h"
#include "capability_memory.h"
#include "champsim.h"

namespace cheri {

// Permission bit positions
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

// ============================================================================
// Permission checks
// ============================================================================

inline bool has_load(uint32_t perms)     { return (perms & PERM_LOAD) != 0; }
inline bool has_store(uint32_t perms)    { return (perms & PERM_STORE) != 0; }
inline bool has_execute(uint32_t perms)  { return (perms & PERM_EXECUTE) != 0; }
inline bool has_load_cap(uint32_t perms) { return (perms & PERM_LOAD_CAPABILITY) != 0; }
inline bool has_seal(uint32_t perms)     { return (perms & PERM_SEAL) != 0; }


// Virtual address (base + offset)
inline champsim::address capability_cursor(const champsim::capability& cap)
{
  return champsim::address{cap.base.to<uint64_t>() + cap.offset.to<uint64_t>()};
}

// bounds of capability (length + base)
inline champsim::address capability_top(const champsim::capability& cap)
{

  if (cap.length.to<uint64_t>() == UINT64_MAX)
    return champsim::address{UINT64_MAX};

  return champsim::address{cap.base.to<uint64_t>() + cap.length.to<uint64_t>()};
}

// True pointer/memory capability: tag is set
inline bool is_pointer_cap(const champsim::capability& cap)
{
  return cap.tag;
}

// NULL capability: tag cleared, virtual address == 0
inline bool is_null_cap(const champsim::capability& cap)
{
  return !cap.tag && capability_cursor(cap).to<uint64_t>() == 0;
}

// Integer-in-capability: tag cleared, virtual address != 0
inline bool is_integer_cap(const champsim::capability& cap)
{
  return !cap.tag && capability_cursor(cap).to<uint64_t>() != 0;
}

// ============================================================================
// Bounds checks
// ============================================================================

// Check if an address falls within [base, top)
inline bool in_bounds(champsim::address addr, champsim::address base, champsim::address top)
{
  return (addr.to<uint64_t>() >= base.to<uint64_t>()) && (addr.to<uint64_t>() < top.to<uint64_t>());
}

// Check if a block number (cache-line granularity) falls within [base, top)
inline bool in_bounds(champsim::block_number block, champsim::address base, champsim::address top)
{
  uint64_t addr = block.to<uint64_t>() << LOG2_BLOCK_SIZE;
  return (addr >= base.to<uint64_t>()) && (addr < top.to<uint64_t>());
}

// Compute remaining cache lines from addr to the capability bound in the given direction.
// Returns 0 if already at or past the bound.
inline int remaining_lines(champsim::block_number block, int direction,
                           champsim::address base, champsim::address top)
{
  uint64_t addr = block.to<uint64_t>() << LOG2_BLOCK_SIZE;
  uint64_t lower_bound = base.to<uint64_t>();
  uint64_t upper_bound = top.to<uint64_t>();

  if (direction > 0) {
    if (addr >= upper_bound) return 0;
    return static_cast<int>((upper_bound - addr) >> LOG2_BLOCK_SIZE);
  } else {
    if (addr < lower_bound) return 0;
    return static_cast<int>((addr - lower_bound) >> LOG2_BLOCK_SIZE);
  }
}

// ============================================================================
// Combined safety check for prefetching
// A prefetch is safe if the target is in bounds and the capability
// grants read permission (PERMIT_LOAD).
// ============================================================================

inline bool prefetch_safe(champsim::address pf_addr, const champsim::capability& cap)
{
  return in_bounds(pf_addr, cap.base, capability_top(cap)) && has_load(cap.permissions);
}

// ============================================================================
// Debug printing
// ============================================================================

inline void print_cap(const champsim::capability& cap)
{
  std::cout << std::hex
            << "base=0x" << cap.base.to<uint64_t>()
            << " length=0x" << cap.length.to<uint64_t>()
            << " offset=0x" << cap.offset.to<uint64_t>()
            << " perms=0x" << cap.permissions
            << " tag=" << cap.tag
            << " cap_op=" << static_cast<uint32_t>(cap.cap_op)
            << std::dec << "\n";
}

} // namespace cheri

#endif