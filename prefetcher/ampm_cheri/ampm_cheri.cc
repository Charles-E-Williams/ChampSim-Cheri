#include "ampm_cheri.h"

#include <iostream>

#include "cache.h"

void ampm_cheri::prefetcher_initialize()
{
  std::cout << "AMPM-CHERI (CI) Prefetcher initialized\n"
            << "  ZONE_BITS           = " << ZONE_BITS << "\n"
            << "  LINES_PER_ZONE      = " << lines_per_zone() << "\n"
            << "  SMALL_CAP_THRESHOLD = " << SMALL_CAP_THRESHOLD << " CL\n"
            << "  REGION_SETS         = " << REGION_SETS << "\n"
            << "  REGION_WAYS         = " << REGION_WAYS << "\n"
            << "  PAGE_REGION_SETS    = " << PAGE_REGION_SETS << "\n"
            << "  PAGE_REGION_WAYS    = " << PAGE_REGION_WAYS << "\n"
            << "  BASE_DEGREE         = " << BASE_DEGREE << "\n"
            << "  MAX_DEGREE          = " << MAX_DEGREE << "\n"
            << "  CROSS_ZONE          = " << CROSS_ZONE_FACTOR << "x\n"
            << "  SHT                 = " << SHT_SETS << "x" << SHT_WAYS << "\n"
            << "  SHT_CONF_BOOST      = " << static_cast<int>(SHT_CONF_BOOST) << "\n";
}


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

  if (cheri::has_seal_bit(cap.permissions)) {
    stat_sealed_bail++;
    return metadata_in;
  }

  uint64_t cap_len   = cap.length.to<uint64_t>();
  uint64_t cap_lines = cap_len >> LOG2_BLOCK_SIZE;
  int cls = cap_size_class(cap_len);

  stat_access_by_class[cls]++;

  if (useful_prefetch) {
    stat_useful_by_class[cls]++;
    if (cap_lines > SMALL_CAP_THRESHOLD)
      sht_reward(ip, cap);
  }


  if (cap_lines <= SMALL_CAP_THRESHOLD) {
    stat_page_access++;
    page_add_to_map(addr, false);
    bool two_level = intern_->get_mshr_occupancy_ratio() < 0.5;
    page_do_prefetch(intern_, addr, metadata_in, BASE_DEGREE, two_level, cls);
    return metadata_in;
  }

  champsim::address va = cheri::capability_cursor(cap);

  // Record access in object-relative bitmap
  add_to_map(va, addr, cap, false);

  // SHT: update stride tracker and get per-stream degree
  int degree = update_and_query_degree(ip, va, cap);

  bool two_level = intern_->get_mshr_occupancy_ratio() < 0.5;
  do_prefetch(intern_, addr, va, cap, metadata_in, degree, two_level);

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

  // Always attempt page-path eviction (keyed by PA, cap not needed)
  {
    auto [key, offset] = page_zone_key_and_offset(evicted_addr);
    auto region = page_regions.check_hit(page_region_type{key});
    if (region.has_value()) {
      region->access_map.at(offset)   = false;
      region->prefetch_map.at(offset) = false;
      page_regions.fill(region.value());
    }
  }

  // Object-path eviction (keyed by VA + cap)
  if (evicted_cap.tag) {
    champsim::address evicted_va = cheri::capability_cursor(evicted_cap);
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


static const char* size_class_name(int cls)
{
  static const char* names[] = {
    "0-128B", "128B-4KB", "4KB-64KB", "64KB-1MB", "1MB+"
  };
  return names[cls];
}

void ampm_cheri::prefetcher_final_stats()
{
  std::cout << "\n=== AMPM-CHERI (CI) Final Stats ===\n"
            << "  Sealed bail:           " << stat_sealed_bail << "\n"
            << "  Bounded by cap:        " << stat_pf_bounded << "\n"
            << "  Zone hash collisions:  " << stat_zone_collision << "\n"
            << "  Cross-zone prefetches: " << stat_cross_zone << "\n"
            << "\n  -- Page Path (caps <= " << SMALL_CAP_THRESHOLD << " CL) --\n"
            << "  Page path accesses:    " << stat_page_access << "\n"
            << "  Page path prefetches:  " << stat_page_pf << "\n"
            << "\n  -- Stride Hint Table (object path) --\n"
            << "  SHT hits:              " << stat_sht_hit << "\n"
            << "  SHT misses:            " << stat_sht_miss << "\n"
            << "  SHT stride confirmed:  " << stat_sht_confirmed << "\n"
            << "  SHT rewarded (useful): " << stat_sht_rewarded << "\n"
            << "  Degree boosted by SHT: " << stat_degree_boosted << "\n";

  std::cout << "\n  -- Per size-class breakdown --\n";
  for (int c = 0; c < NUM_SIZE_CLASSES; c++) {
    if (stat_access_by_class[c] == 0) continue;
    std::cout << "  [" << size_class_name(c) << "]"
              << "  accesses=" << stat_access_by_class[c]
              << "  pf_issued=" << stat_pf_by_class[c]
              << "  useful=" << stat_useful_by_class[c] << "\n";
  }
}
