//=======================================================================================//
// File             : sms_cheri/sms_cheri.cc
// Description      : CHERI-aware Spatial Memory Streaming prefetcher.
//=======================================================================================//

#include "sms_cheri.h"

#include <iostream>

#include "cache.h"

void sms_cheri::prefetcher_initialize()
{
  std::deque<PHTEntry*> d;
  pht.resize(sms_cheri::PHT_SETS, d);
}

uint32_t sms_cheri::prefetcher_cache_operate(champsim::address address,
                                             champsim::address ip,
                                             uint8_t cache_hit,
                                             bool useful_prefetch,
                                             access_type type,
                                             uint32_t metadata_in)
{
  auto cap = intern_->get_authorizing_capability();
  if (!cheri::is_tag_valid(cap)) 
    return metadata_in;
    
  uint64_t addr = address.to<uint64_t>();
  uint64_t pc = ip.to<uint64_t>();


  region_info ri = decompose(addr, cap);

  std::vector<uint64_t> pref_addr;

  auto at_index = search_acc_table(ri.region_id);
  if (at_index != acc_table.end()) {
    // Accumulation table hit: record this CL in the active pattern.
    (*at_index)->pattern[ri.offset] = 1;
    update_age_acc_table(at_index);
  } else {
    auto ft_index = search_filter_table(ri.region_id);
    if (ft_index != filter_table.end()) {
      // Filter table hit (second access to region): promote to AT.
      insert_acc_table((*ft_index), ri.offset);
      evict_filter_table(ft_index);
    } else {
      // First access to this region.  Record in FT and attempt to
      // replay a previously learned pattern from the PHT.
      insert_filter_table(pc, ri);
      generate_prefetch(pc, addr, ri, pref_addr);
      buffer_prefetch(pref_addr);
    }
  }

  return metadata_in;
}

void sms_cheri::prefetcher_cycle_operate() { issue_prefetch(); }

uint32_t sms_cheri::prefetcher_cache_fill(champsim::address addr, long set, long way, bool prefetch, champsim::address evicted_addr, uint32_t metadata_in, champsim::capability evicted_cap)
{
  return metadata_in;
}

void sms_cheri::prefetcher_final_stats()
{
  std::cout << "  Prefetches clipped (bounds):  " << stat_pref_bounds_clip << std::endl;
  std::cout << "  Prefetch next region:  " << stat_next_region_pf << std::endl;

}