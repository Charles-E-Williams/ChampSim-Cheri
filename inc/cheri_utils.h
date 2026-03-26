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

  uint64_t base = cap.base.to<uint64_t>();
  uint64_t length = cap.length.to<uint64_t>();
  return champsim::address{ (length == UINT64_MAX) ? UINT64_MAX : (base + length) };
}

// True pointer/memory capability: tag is set
inline bool is_tag_valid(const champsim::capability& cap)
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

// Check if an address falls within [base, top)
inline bool in_bounds(champsim::address addr, champsim::address base, champsim::address top)
{
  return (addr.to<uint64_t>() >= base.to<uint64_t>()) && (addr.to<uint64_t>() < top.to<uint64_t>());
}

// Check if a cache line falls within [base, top)
inline bool in_bounds(champsim::block_number block, champsim::address base, champsim::address top)
{
  uint64_t block_start = block.to<uint64_t>() << LOG2_BLOCK_SIZE;
  uint64_t block_end   = block_start + BLOCK_SIZE;

  return (block_start < top.to<uint64_t>()) && (block_end > base.to<uint64_t>());
}
// Compute remaining cache lines from addr to the capability bound in the given direction.
// Returns 0 if already at or past the bound.
inline int remaining_lines(champsim::block_number block, int direction,
                           champsim::address base, champsim::address top)
{
  if (base.to<uint64_t>() >= top.to<uint64_t>()) return 0;

  uint64_t current_blk = block.to<uint64_t>();
  uint64_t base_blk    = base.to<uint64_t>() >> LOG2_BLOCK_SIZE;
  uint64_t top_blk     = (top.to<uint64_t>() - 1) >> LOG2_BLOCK_SIZE; 

  if (direction > 0) {
    return (current_blk >= top_blk) ? 0 : static_cast<int>(top_blk - current_blk);
  } else {
    return (current_blk <= base_blk) ? 0 : static_cast<int>(current_blk - base_blk);
  }
}

inline bool prefetch_safe(champsim::address pf_addr, const champsim::capability& cap)
{
  if (has_seal(cap.permissions)) return false;

  return in_bounds(pf_addr, cap.base, capability_top(cap)) && has_load(cap.permissions);
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