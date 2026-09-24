#include "ampm_cheri.h"

#include <iostream>

#include "cache.h"

uint32_t ampm_cheri::prefetcher_cache_operate(champsim::address addr,
                                              champsim::address ip,
                                              uint32_t cpu,
                                              champsim::capability cap,
                                              bool cache_hit,
                                              bool useful_prefetch,
                                              access_type type,
                                              uint32_t metadata_in,
                                              uint32_t metadata_hit)
{

  if (!cheri::is_tag_valid(cap))
    return metadata_in;

  uint64_t cap_len   = cap.length.to<uint64_t>();
  uint64_t cap_lines = cap_len >> LOG2_BLOCK_SIZE;
  int cls = cap_size_category(cap_len);

  access_by_size[cls]++;
  if (useful_prefetch) {
    useful_by_size[cls]++;
  }


  if (cap_lines <= SMALL_CAP_THRESHOLD) {
    page_access++;
    bool two_level = intern_->get_mshr_occupancy_ratio() < 0.5;
    page_engine.add_to_pagemap(addr, false);
    page_engine.do_prefetch(intern_, addr, ip, cache_hit, useful_prefetch, type, metadata_in, PREFETCH_DEGREE, two_level);

    return metadata_in;
  }

  champsim::address va = intern_->v_addr;

  // Record access in object-relative bitmap
  add_to_map(va, addr, cap, false);



  bool two_level = intern_->get_mshr_occupancy_ratio() < 0.5;
  do_prefetch(intern_, addr, va, cap, metadata_in, PREFETCH_DEGREE, two_level);

  return metadata_in;
}

uint32_t ampm_cheri::prefetcher_cache_fill(champsim::address addr, champsim::address ip,
                                           uint32_t cpu, champsim::capability cap,
                                           bool useless, long set,
                                           long way, bool prefetch,
                                           champsim::address evicted_addr,
                                           champsim::capability evicted_cap,
                                           uint32_t metadata_in,
                                           uint32_t metadata_evict, uint32_t cpu_evict)
{
  if (evicted_addr == champsim::address{})
    return metadata_in;


  // fallback path
  page_engine.remove_from_pagemap(evicted_addr,false);
  page_engine.remove_from_pagemap(evicted_addr,true);


  // capability path
  if (evicted_cap.tag) {
    champsim::address evicted_va = intern_->vaddr_evicted;
    auto [key, offset] = zone_key_and_offset(evicted_va, evicted_cap);
    if (key.to<uint64_t>() != 0) {
      auto region = regions.check_hit(region_type{key});
      if (region.has_value()) {
        region->access_map.at(offset)   = false;
        region->prefetch_map.at(offset) = false;
        regions.fill(region.value());
      }
    }
  }

  return metadata_in;
}


void ampm_cheri::prefetcher_final_stats()
{
  std::cout << "\n=== AMPM-CHERI Final Stats ===\n"
            << "  Bounded by cap:        " << pf_bounded << "\n"
            << "  Zone hash collisions:  " << zone_collision << "\n"
            << "\n ==== Page Path fallback =====\n"
            << "  Page path accesses:    " << page_access << "\n";

  std::cout << "\n  Prefetch metrics with respect to capability size\n";

  static const char* size_name[NUM_SIZES] = {"SMALL","MEDIUM","LARGE","XLARGE","XXL"};
  for (int c = 0; c < NUM_SIZES; c++) {
    std::cout << "  [" << size_name[c] << "]"
              << "  accesses=" << access_by_size[c]
              << "  useful=" << useful_by_size[c] << "\n";
  }
}
