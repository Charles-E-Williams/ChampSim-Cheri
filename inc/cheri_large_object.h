#ifndef CHERI_LARGE_OBJECT_H
#define CHERI_LARGE_OBJECT_H

// Large-object boost shared by berti_cap, spp_cap and ampm_cap: the object-size cutoff and the depth budget.

#include <cstdint>

#include "cheri.h"
#include "cheri_prefetch_utils.h"

namespace cheri_policy {

// Ablation switch
constexpr bool BOOST_ENABLED = true;    // false: no boost path runs

// Large-object boost
constexpr uint64_t BOOST_MIN_OBJ_BYTES = uint64_t{2} << 20; // 2MB

inline bool boost_object(const champsim::capability& cap) { return cap.tag && cap.length.to<uint64_t>() >= BOOST_MIN_OBJ_BYTES; }

// In-bounds lines left from block in direction dir (+1/-1); 0 for an untagged capability.
inline int64_t depth_budget(const champsim::capability& cap, champsim::block_number block, int dir)
{
  if (!cap.tag)
    return 0;
  return cheri::remaining_lines(block, dir, cap.base, cheri::capability_top(cap));
}

} // namespace cheri_policy

#endif