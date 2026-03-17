//=======================================================================================//
// File             : sms_cheri/sms_cheri.cc
// Description      : CHERI-aware Spatial Memory Streaming prefetcher.
//                    Top-level ChampSim interface methods.
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
  uint64_t addr = address.to<uint64_t>();
  uint64_t pc = ip.to<uint64_t>();

  auto cap = get_auth_capability();

  // Decompose the demand into region identity + intra-region offset.
  // When a valid capability is available the region aligns to the object;
  // otherwise we fall back to the stock 2KB physical region.
  region_info ri = decompose(addr, cap);

  stat_total_accesses++;
  if (ri.has_cap)
    stat_cap_accesses++;
  else
    stat_nocap_accesses++;

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

uint32_t sms_cheri::prefetcher_cache_fill(champsim::address addr, long set,
                                          long way, uint8_t prefetch,
                                          champsim::address evicted_addr,
                                          uint32_t metadata_in)
{
  return metadata_in;
}

void sms_cheri::prefetcher_final_stats()
{
  std::cout << "\nsms_cheri final stats" << std::endl;
  std::cout << "  Total accesses:              " << stat_total_accesses << std::endl;
  std::cout << "  Cap-backed accesses:         " << stat_cap_accesses << std::endl;
  std::cout << "  Page-fallback accesses:      " << stat_nocap_accesses << std::endl;
  if (stat_total_accesses > 0) {
    std::cout << "  Cap coverage:                "
              << (100.0 * static_cast<double>(stat_cap_accesses)
                  / static_cast<double>(stat_total_accesses))
              << "%" << std::endl;
  }
  std::cout << "  Prefetches generated:        " << stat_pref_generated << std::endl;
  std::cout << "    from cap-backed patterns:  " << stat_pref_cap << std::endl;
  std::cout << "    from page-based patterns:  " << stat_pref_nocap << std::endl;
  std::cout << "  Prefetches clipped (bounds):  " << stat_pref_bounds_clip << std::endl;
  std::cout << "  Prefetches clipped (page):    " << stat_pref_page_clip << std::endl;
}