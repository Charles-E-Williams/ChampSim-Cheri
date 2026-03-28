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
  champsim::address va = intern_->get_vaddr();

  stat_cap_lookups++;
  stat_cap_hits++;

  // Access map records in VA space (v_addr - cap.base is meaningful)
  add_to_map(va, cap, false);

  do_prefetch(intern_, addr, va, cap, metadata_in, PREFETCH_DEGREE,
              intern_->get_mshr_occupancy_ratio() < 0.5);

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
  std::cout << "  Cap lookups:              " << stat_cap_lookups << std::endl;
  std::cout << "  Cap hits:                 " << stat_cap_hits << std::endl;
  if (stat_cap_lookups > 0) {
    double hit_rate = 100.0 * static_cast<double>(stat_cap_hits)
                            / static_cast<double>(stat_cap_lookups);
    std::cout << "  Cap hit rate:             " << hit_rate << "%" << std::endl;
  }
  std::cout << "  Prefetches issued:        " << stat_pf_issued << std::endl;
  std::cout << "  Bounded by cap:           " << stat_pf_bounded_by_cap << std::endl;
  std::cout << "  Cross-page detected:      " << stat_cross_page_detected << std::endl;
  std::cout << "  Cross-page cant issue:    " << stat_cross_page_cant_issue << std::endl;
  std::cout << "==============================\n" << std::endl;
}