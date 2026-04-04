#include "ampm_cheri.h"

#include <iostream>

#include "cache.h"

void ampm_cheri::prefetcher_initialize()
{
  std::cout << "AMPM-CHERI Prefetcher initialized\n "
            << " ZONE_BITS= " << ZONE_BITS << " \n"
            << "  LINES_PER_ZONE= " << lines_per_zone() <<" \n"
            << "  REGION_SETS= " << REGION_SETS  << " \n"
            << "  REGION_WAYS= " << REGION_WAYS  << " \n"
            << "  PREFETCH_DEGREE= " << PREFETCH_DEGREE
            << std::endl;
}

uint32_t ampm_cheri::prefetcher_cache_operate(champsim::address addr, champsim::address ip,
                                              bool cache_hit, bool useful_prefetch,
                                              access_type type, uint32_t metadata_in)
{
  auto cap = intern_->get_authorizing_capability();

  if (!cheri::is_tag_valid(cap)) 
    return metadata_in;
  champsim::address va = cheri::capability_cursor(cap); // virtual address

  // access map records in VA space
  add_to_map(va, addr, cap, false);

  do_prefetch(intern_, addr, va, cap, metadata_in, PREFETCH_DEGREE, intern_->get_mshr_occupancy_ratio() < 0.5);

  return metadata_in;
}

uint32_t ampm_cheri::prefetcher_cache_fill(champsim::address addr, long set, long way,
                                           bool prefetch, champsim::address evicted_addr,
                                           uint32_t metadata_in)
{
  return metadata_in;
}

void ampm_cheri::prefetcher_final_stats()
{
  std::cout << "\n=== AMPM-CHERI Final Stats ===" << std::endl;
  std::cout << "  Bounded by cap:           " << stat_pf_bounded << std::endl;
}