#include "ampm_cap.h"

#include <iostream>
#include <optional>

#include "cache.h"

uint32_t ampm_cap::prefetcher_cache_operate(champsim::address addr,
                                              champsim::address ip,
                                              uint32_t cpu,
                                              champsim::capability cap,
                                              bool cache_hit,
                                              bool useful_prefetch,
                                              access_type type,
                                              uint32_t metadata_in,
                                              uint32_t metadata_hit)
{
  // A demand access to a tracked OOB candidate credits its confidence counter (tagged or not)
  if (type == access_type::LOAD || type == access_type::RFO)
    oob.on_demand(champsim::block_number{addr}.to<uint64_t>());

  // Untagged: plain AMPM page path, candidates issued ungated (no object information)
  if (!cheri::is_tag_valid(cap)) {
    bool two_level = intern_->get_mshr_occupancy_ratio() < 0.5;
    page_engine.add_to_pagemap(addr, false);
    page_do_prefetch(intern_, addr, std::nullopt, cap, cheri_policy::OobConfidence::key{}, metadata_in, PREFETCH_DEGREE, two_level);
    return metadata_in;
  }

  uint64_t cap_len   = cap.length.to<uint64_t>();
  uint64_t cap_lines = cap_len >> LOG2_BLOCK_SIZE;
  int cls = cap_size_category(cap_len);

  access_by_size[cls]++;
  if (useful_prefetch) {
    useful_by_size[cls]++;
  }


  // Virtual line address from the capability cursor, checked against the physical address's line position in the page
  // (cheri::line_va_from_cursor). Small capabilities, or no usable VA, take the page-based fallback path.
  std::optional<champsim::address> va_line;
  if (cap_lines > SMALL_CAP_THRESHOLD) {
    va_line = cheri::line_va_from_cursor(cap, addr);
    if (!va_line.has_value())
      cursor_check_failed++;
  }

  if (!va_line.has_value()) {
    page_access++;
    bool two_level = intern_->get_mshr_occupancy_ratio() < 0.5;
    page_engine.add_to_pagemap(addr, false);
    // Small capabilities: the candidate's position relative to the object needs the VA (none for a failed large-cap check)
    const auto page_va = (cap_lines > SMALL_CAP_THRESHOLD) ? std::nullopt : cheri::line_va_from_cursor(cap, addr);
    if (cap_lines <= SMALL_CAP_THRESHOLD && !page_va.has_value())
      small_cursor_check_failed++;
    const auto oob_key = cheri_policy::OobConfidence::make_key(cheri_policy::size_class(cap_len), ip.to<uint64_t>());
    page_do_prefetch(intern_, addr, page_va, cap, oob_key, metadata_in, PREFETCH_DEGREE, two_level);

    return metadata_in;
  }

  champsim::address va = *va_line;

  // Record access in object-relative bitmap
  add_to_map(va, cap, false);



  bool two_level = intern_->get_mshr_occupancy_ratio() < 0.5;

  // Large-object boost, only when this access consumed one of our prefetches
  int degree = PREFETCH_DEGREE;
  if constexpr (cheri_policy::BOOST_ENABLED) {
    if (useful_prefetch && cheri_policy::boost_object(cap) && intern_->get_mshr_occupancy_ratio() * 100 < AMPM_CAP_BOOST_MSHR_LIMIT) {
      degree = BOOST_DEGREE;
      boost_accesses++;
    }
  }
  do_prefetch(intern_, addr, va, cap, metadata_in, degree, two_level);

  return metadata_in;
}

uint32_t ampm_cap::prefetcher_cache_fill(champsim::address addr, champsim::address ip,
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
  // VA of the evicted line = the line of evicted_cap's cursor, checked against evicted_addr's line position in the page
  if (evicted_cap.tag) {
    const auto evicted_va = cheri::line_va_from_cursor(evicted_cap, evicted_addr);
    if (!evicted_va.has_value()) {
      evict_cursor_check_failed++;
    } else {
      auto [key, offset] = zone_key_and_offset(*evicted_va, evicted_cap);
      if (key.to<uint64_t>() != 0) {
        auto region = regions.check_hit(region_type{key});
        if (region.has_value()) {
          region->access_map.at(offset)   = false;
          region->prefetch_map.at(offset) = false;
          regions.fill(region.value());
        }
      }
    }
  }

  return metadata_in;
}


void ampm_cap::prefetcher_final_stats()
{
  std::cout << "\nAMPM_CAP policy configuration\n";
  cheri_policy::OobConfidence::print_config(std::cout);
  std::cout << "  PREFETCH_DEGREE: " << PREFETCH_DEGREE << "  BOOST_DEGREE: " << BOOST_DEGREE << "  BOOST_MSHR_LIMIT: " << AMPM_CAP_BOOST_MSHR_LIMIT << "\n";

  std::cout << "\n=== AMPM-CAP Final Stats ===\n"
            << "  Bounded by cap:        " << pf_bounded << "\n"
            << "  Zone hash collisions:  " << zone_collision << "\n"
            << "\n ==== Page Path fallback =====\n"
            << "  Page path accesses:    " << page_access << "\n"
            << "  Cursor/line check failed (to page path): " << cursor_check_failed << "\n"
            << "  Small-cap cursor/line check failed (OOB gate skipped): " << small_cursor_check_failed << "\n"
            << "  Eviction cleanup skipped (cursor/line check failed): " << evict_cursor_check_failed << "\n";

  std::cout << "\n  Boost: accesses=" << boost_accesses << "  prefetches beyond degree=" << pf_boost << "\n";
  oob.print_stats(std::cout);

  std::cout << "\n  Prefetch metrics with respect to capability size\n";

  static const char* size_name[NUM_SIZES] = {"SMALL","MEDIUM","LARGE","XLARGE","XXL"};
  for (int c = 0; c < NUM_SIZES; c++) {
    std::cout << "  [" << size_name[c] << "]"
              << "  accesses=" << access_by_size[c]
              << "  useful=" << useful_by_size[c] << "\n";
  }
}
