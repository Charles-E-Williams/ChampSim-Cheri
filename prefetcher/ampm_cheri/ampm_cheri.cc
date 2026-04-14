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
            << "\n";
}

uint32_t ampm_cheri::prefetcher_cache_operate(champsim::address addr, champsim::address ip,
                                              bool cache_hit, bool useful_prefetch,
                                              access_type type, uint32_t metadata_in)
{
  auto cap = intern_->get_authorizing_capability();
  if (!cheri::is_tag_valid(cap)) 
    return metadata_in;

  uint64_t num_lines = cap.length.to<uint64_t>() >> LOG2_BLOCK_SIZE;
  if (num_lines < 4) {
    stat_small_cap++;
    return metadata_in;
  }

  champsim::address va = cheri::capability_cursor(cap);

  add_to_map(va, addr, cap, false);
  do_prefetch(intern_, addr, va, cap, metadata_in, PREFETCH_DEGREE, intern_->get_mshr_occupancy_ratio() < 0.5);

  return metadata_in;
}
uint32_t ampm_cheri::prefetcher_cache_fill(champsim::address addr, long set, long way, bool prefetch, champsim::address evicted_addr, uint32_t metadata_in, champsim::capability evicted_cap)

{
  if (evicted_addr == champsim::address{})
    return metadata_in;

  if (!evicted_cap.tag)
    return metadata_in;

  champsim::address evicted_va = cheri::capability_cursor(evicted_cap);
  auto [key, offset] = zone_key_and_offset(evicted_va, evicted_cap);
  if (key.to<uint64_t>() == 0) {
    return metadata_in;
  }

  auto region = regions.check_hit(region_type{key});
  if (region.has_value()) {
    region->access_map.at(offset) = false;
    region->prefetch_map.at(offset) = false;
    regions.fill(region.value());
  }

  return metadata_in;
}

void ampm_cheri::prefetcher_final_stats()
{
  std::cout << "\n=== AMPM-CHERI Final Stats ===" << "\n";
  std::cout << "Bounded by cap:           " << stat_pf_bounded << "\n";
  std::cout << "Capability too small to prefetch:        " << stat_small_cap << "\n";
  std::cout << "Zone hash collisions:     " << stat_zone_collision << "\n";
}