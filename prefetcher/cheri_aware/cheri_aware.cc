#include "cheri_aware.h"

#include <cassert>
#include <iostream>
#include <algorithm>

void cheri_aware::prefetcher_initialize() {
  // Get CPU ID from parent cache
  cpu_id = static_cast<uint32_t>(this->intern_->cpu);
}

uint32_t cheri_aware::prefetcher_cache_operate(champsim::address addr, champsim::address ip, 
                                               uint8_t cache_hit, bool useful_prefetch, 
                                               access_type type, uint32_t metadata_in) {

  champsim::block_number cl_addr{addr};
  champsim::block_number::difference_type stride = 0;
  
  auto found = table.check_hit({ip, cl_addr, stride});
  
  // Check if we have a capability for this address
  auto cap_opt = champsim::global_capability_memory[cpu_id].load_capability(addr);
  
  if (cap_opt && cap_opt->tag) {
    stat_capability_hits++;
    
    // We have a valid capability!
    champsim::address cap_base = cap_opt->base;
    champsim::address cap_bound = cap_opt->base + cap_opt->length.to<int64_t>();
    
    // If we found a matching stride entry
    if (found.has_value()) {
      stride = champsim::offset(found->last_cl_addr, cl_addr);
      
      // Check if we have a consistent stride (same stride seen twice)
      if (stride != 0 && stride == found->last_stride) {
        // Initialize prefetch state with capability bounds
        active_lookahead = {
          champsim::address{cl_addr}, 
          stride, 
          PREFETCH_DEGREE,
          cap_base,
          cap_bound
        };
      }
    }
  } else {
    stat_prefetches_no_capability++;
  }
  
  // Update tracking table
  table.fill({ip, cl_addr, stride});
  
  return metadata_in;
}

void cheri_aware::prefetcher_cycle_operate() {
  // If a lookahead is active
  if (active_lookahead.has_value()) {
    auto [old_pf_address, stride, degree, cap_base, cap_bound] = active_lookahead.value();
    assert(degree > 0);
    
    champsim::address pf_address{champsim::block_number{old_pf_address} + stride};
    
    // CHERI-AWARE: Check if prefetch address is within capability bounds
    bool within_bounds = (pf_address >= cap_base && pf_address < cap_bound);
    
    if (!within_bounds) {
      // Stop prefetching - we've hit capability bounds!
      stat_prefetches_blocked_by_bounds++;
      active_lookahead.reset();
      return;
    }
    
    stat_prefetches_bounds_ok++;
    
    // Check page boundary (original ChampSim behavior)
    bool same_page = intern_->virtual_prefetch || 
                     (champsim::page_number{pf_address} == champsim::page_number{old_pf_address});
    
    if (same_page) {
      // Check MSHR occupancy
      const bool mshr_under_light_load = intern_->get_mshr_occupancy_ratio() < 0.5;
      const bool success = prefetch_line(pf_address, mshr_under_light_load, 0);
      
      if (success) {
        stat_prefetches_issued++;
        active_lookahead = {pf_address, stride, degree - 1, cap_base, cap_bound};
      }
      
      // If we've exhausted the degree, stop
      if (active_lookahead->degree == 0) {
        active_lookahead.reset();
      }
    } else {
      // Hit page boundary, stop
      active_lookahead.reset();
    }
  }
}

uint32_t cheri_aware::prefetcher_cache_fill(champsim::address addr, long set, long way, 
                                            uint8_t prefetch, champsim::address evicted_addr, 
                                            uint32_t metadata_in) {
  return metadata_in;
}

void cheri_aware::prefetcher_final_stats() {
  std::cout << "  CHERI-Aware Prefetcher Statistics:" << std::endl;
  std::cout << "  Total prefetches issued: " << stat_prefetches_issued << std::endl;
  std::cout << "  Prefetches within bounds: " << stat_prefetches_bounds_ok << std::endl;
  std::cout << "  Prefetches blocked by capability bounds: " << stat_prefetches_blocked_by_bounds << std::endl;
  std::cout << "  Memory accesses without capability: " << stat_prefetches_no_capability << std::endl;
  std::cout << "  Capability hits during operate: " << stat_capability_hits << std::endl;
  
  if (stat_prefetches_issued > 0) {
    double bounds_ok = static_cast<double>(stat_prefetches_bounds_ok);
    double bounds_blocked = static_cast<double>(stat_prefetches_blocked_by_bounds);
    double bounds_respect_rate = (100.0 * bounds_ok) / (bounds_ok + bounds_blocked);
    std::cout << "  Bounds respect rate: " << bounds_respect_rate << "%" << std::endl;
  }
  
  std::cout << std::endl;
}