#include "ip_stride_cheri.h"

#include <algorithm>
#include <cassert>
#include <iostream>

#include "cache.h"

void ip_stride_cheri::prefetcher_initialize()
{
  cap_prefetches_bounded = 0;
  cap_table_hits = 0;
  cap_table_misses = 0;
  too_small_filtered = 0;
}

uint32_t ip_stride_cheri::prefetcher_cache_operate(champsim::address addr, champsim::address ip,
                                                   uint8_t cache_hit, bool useful_prefetch,
                                                   access_type type, uint32_t metadata_in)
{
  const auto& cap = intern_->get_authorizing_capability();
  if (cheri::has_load_permissions(cap.permissions)) {
 
    // Skip single-element objects (too small to prefetch into)
    if (!cheri::has_prefetchable_range(cap)) {
      too_small_filtered++;
      return metadata_in;
    }
 
    uint64_t raw_hash = cheri::hash_capability(cap);
    uint64_t cap_hash = raw_hash ^ (ip.to<uint64_t>() * 0x517cc1b727220a95ULL);
    int64_t current_offset = cheri::lines_from_cap_base(cap);
 
    auto found = cap_table.check_hit({cap_hash, current_offset, 0, 0});
 
    if (found.has_value()) {
      cap_table_hits++;
      int64_t stride = current_offset - found->last_offset_accessed;
      
      if (stride != 0 && stride == found->last_stride) {
        // Issue K prefetches at D stride, bounded by capability
        active_lookahead = lookahead_entry{addr, stride, PREFETCH_DEGREE, cap};
      }
 
      cap_table.fill({cap_hash, current_offset, found->last_offset_prefetched,
                       stride != 0 ? stride : found->last_stride});
 
    } else {
      // first access through this capability 
      cap_table_misses++;
      cap_table.fill({cap_hash, current_offset, 0, 0});
    }
  }
 
  return metadata_in;
}

void ip_stride_cheri::prefetcher_cycle_operate()
{
  if (!active_lookahead.has_value())
    return;
 
  auto [old_pf_address, stride, degree, cap] = active_lookahead.value();
  assert(degree > 0);
 
  champsim::address pf_address{champsim::block_number{old_pf_address} + stride};
 
  // Bounds check: do not exceed capability bounds
  if (cap.has_value()) {
    if (!cheri::prefetch_safe(pf_address, *cap)) {
      cap_prefetches_bounded++;
      active_lookahead.reset();
      return;
    }
  }
 
  if (intern_->virtual_prefetch || champsim::page_number{pf_address} == champsim::page_number{old_pf_address}) {
    const bool mshr_under_light_load = intern_->get_mshr_occupancy_ratio() < 0.5;
    const bool success = prefetch_line(pf_address, mshr_under_light_load, 0);
 
    if (success) { 
      active_lookahead = {pf_address, stride, degree - 1, cap};
    } 
    
    if (active_lookahead->degree == 0)
      active_lookahead.reset();
  } else {
    active_lookahead.reset();
  }
}

uint32_t ip_stride_cheri::prefetcher_cache_fill(champsim::address addr, long set, long way, uint8_t prefetch, champsim::address evicted_addr,
                                                uint32_t metadata_in)
{
  return metadata_in;
}

void ip_stride_cheri::prefetcher_final_stats()
{
  std::cout << "\nip_stride_cheri final stats" << std::endl;
  if (cap_table_hits + cap_table_misses > 0) {
    double hit_rate = 100.0 * static_cast<double>(cap_table_hits)
                    / static_cast<double>(cap_table_hits + cap_table_misses);
    std::cout << "  Cap table hit rate:          " << hit_rate << "%" << std::endl;
  }
  std::cout << "  Single object filtered prefetch:         " << too_small_filtered << std::endl;
  std::cout << "  Cap prefetches bounded:      " << cap_prefetches_bounded << std::endl;
}