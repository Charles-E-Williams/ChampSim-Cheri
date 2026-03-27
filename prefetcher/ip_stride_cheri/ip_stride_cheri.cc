#include "ip_stride_cheri.h"

#include <algorithm>
#include <cassert>
#include <iostream>

#include "cache.h"

void ip_stride_cheri::prefetcher_initialize()
{
  cap_prefetches_issued = 0;
  cap_prefetches_bounded = 0;
  ip_prefetches_issued = 0;
  cap_table_hits = 0;
  cap_table_misses = 0;
  cap_accesses = 0;
  nocap_accesses = 0;
  too_small_filtered = 0;
}

uint32_t ip_stride_cheri::prefetcher_cache_operate(champsim::address addr, champsim::address ip,
                                                   uint8_t cache_hit, bool useful_prefetch,
                                                   access_type type, uint32_t metadata_in)
{
  const auto& cap = intern_->get_authorizing_capability();
  if (cap.tag && cheri::has_load(cap.permissions)) {
    cap_accesses++;
 
    // Skip single-element objects (too small to prefetch into)
    if (!cheri::is_prefetchable(cap)) {
      too_small_filtered++;
      return metadata_in;
    }
 
    uint64_t ch = cheri::hash_capability(cap);
    int64_t current_offset = cheri::lines_from_cap_base(addr, cap);
 
    auto found = cap_table.check_hit({ch, current_offset, 0, 0});
 
    if (found.has_value()) {
      cap_table_hits++;
 
      int64_t stride = current_offset - found->last_offset_accessed;

 
      if (stride != 0) {
        // Issue K prefetches at D stride, bounded by capability
        active_lookahead = lookahead_entry{addr, stride, PREFETCH_DEGREE, cap};
        
      }
 
      // Update entry with current offset
      cap_table.fill({ch, current_offset, found->last_offset_prefetched,
                       stride != 0 ? stride : found->last_stride});
 
    } else {
      // First access through this capability -- seed the table
      cap_table_misses++;
      cap_table.fill({ch, current_offset, 0, 0});
    }
 
  } else { //fallback
    nocap_accesses++;
    champsim::block_number cl_addr{addr};
    champsim::block_number::difference_type stride = 0;

    auto found = ip_table.check_hit({ip, cl_addr, stride});
 
    if (found.has_value()) {
      stride = champsim::offset(found->last_cl_addr, cl_addr);
      if (stride != 0 && stride == found->last_stride) {
        active_lookahead = lookahead_entry{champsim::address{cl_addr}, stride, PREFETCH_DEGREE, std::nullopt};
      }
    }
 
    ip_table.fill({ip, cl_addr, stride});
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
      if (cap.has_value())
        cap_prefetches_issued++;
      else
        ip_prefetches_issued++;
 
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
  std::cout << "  Capability accesses:         " << cap_accesses << std::endl;
  std::cout << "  Non-capability accesses:     " << nocap_accesses << std::endl;
  std::cout << "  Cap table hits:              " << cap_table_hits << std::endl;
  std::cout << "  Cap table misses:            " << cap_table_misses << std::endl;
  if (cap_table_hits + cap_table_misses > 0) {
    double hit_rate = 100.0 * static_cast<double>(cap_table_hits)
                    / static_cast<double>(cap_table_hits + cap_table_misses);
    std::cout << "  Cap table hit rate:          " << hit_rate << "%" << std::endl;
  }
  std::cout << "  Single object filtered prefetch:         " << too_small_filtered << std::endl;
  std::cout << "  Cap prefetches issued:       " << cap_prefetches_issued << std::endl;
  std::cout << "  Cap prefetches bounded:      " << cap_prefetches_bounded << std::endl;
  std::cout << "  IP fallback prefetches:      " << ip_prefetches_issued << std::endl;
}