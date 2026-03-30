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
  if (!cheri::has_prefetchable_range(cap) || !cheri::has_load_permissions(cap.permissions))
    return metadata_in;


  champsim::address va = cheri::capability_cursor(cap); // virtual address
  tlb.fill(va.to<uint64_t>(), addr.to<uint64_t>()); // populate TLB clone with current VA to PA mapping

  // access map records in VA space
  add_to_map(va, addr, cap, false);

  do_prefetch(intern_, addr, va, cap, metadata_in, PREFETCH_DEGREE, intern_->get_mshr_occupancy_ratio() < 0.5);

  return metadata_in;
}

uint32_t ampm_cheri::prefetcher_cache_fill(champsim::address addr, long set, long way,
                                           bool prefetch, champsim::address evicted_addr,
                                           uint32_t metadata_in)
{
  if (evicted_addr != champsim::address{}) {
    auto hit = reverse_map.check_hit({evicted_addr, {}, 0});
    if (hit.has_value()) {
      auto region = regions.check_hit(region_type{hit->zone_key});
      if (region.has_value()) {
        region->access_map.at(hit->zone_offset) = false;
        region->prefetch_map.at(hit->zone_offset) = false;
        regions.fill(region.value());
      }
    }
  }
  return metadata_in;
}

void ampm_cheri::prefetcher_final_stats()
{
  std::cout << "\n=== AMPM-CHERI Final Stats ===" << std::endl;
  std::cout << "  Bounded by cap:           " << stat_pf_bounded_by_cap << std::endl;
  std::cout << "  Cross-page detected:      " << stat_cross_page_detected << std::endl;
  std::cout << "  Cross-page cant issue:    " << stat_cross_page_cant_issue << std::endl;
  tlb.print_stats();
}