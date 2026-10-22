#include "ampm_span.h"

#include <cstdio>
#include <iostream>
#include <optional>
#include <string>

#include "cache.h"

uint32_t ampm_span::prefetcher_cache_operate(champsim::address addr, champsim::address ip, uint32_t cpu, champsim::capability cap, bool cache_hit,
                                             bool useful_prefetch, access_type type, uint32_t metadata_in, uint32_t metadata_hit)
{
  if (!cheri::is_tag_valid(cap))
    return metadata_in;

  // A demand access to a tracked out-of-object candidate credits its gate counter
  if constexpr (GATE_ENABLED)
    if (type != access_type::PREFETCH)
      gate_on_demand(champsim::block_number{addr}.to<uint64_t>());

  uint64_t cap_lines = cap.length.to<uint64_t>() >> LOG2_BLOCK_SIZE;
  bool two_level = intern_->get_mshr_occupancy_ratio() < 0.5;

  // Virtual line address from the capability cursor, checked against the physical address's line position in the page
  // (cheri::line_va_from_cursor). Small capabilities, or no usable VA, take the page path.
  std::optional<champsim::address> va_line;
  if (cap_lines > SMALL_CAP_THRESHOLD) {
    va_line = cheri::line_va_from_cursor(cap, addr);
    if (!va_line.has_value())
      cursor_check_failed++;
  }

  if (!va_line.has_value()) {
    page_access++;
    page_engine.add_to_pagemap(addr, false);
    int run_dir = 0;
    if constexpr (SPAN_ENABLED) {
      run_dir = run_update(cap);
      run_accesses += (run_dir != 0);
    }
    // Small capabilities: the candidate's position relative to the object needs the VA (none for a failed large-cap check)
    const auto page_va = (cap_lines > SMALL_CAP_THRESHOLD) ? std::nullopt : cheri::line_va_from_cursor(cap, addr);
    page_do_prefetch(intern_, addr, page_va, cap, run_dir, metadata_in, two_level);
    return metadata_in;
  }

  champsim::address va = *va_line;

  // Record access in object-relative bitmap
  add_to_map(va, cap, false);

  do_prefetch(intern_, addr, va, cap, metadata_in, PREFETCH_DEGREE, two_level);

  return metadata_in;
}

uint32_t ampm_span::prefetcher_cache_fill(champsim::address addr, champsim::address ip, uint32_t cpu, champsim::capability cap, bool useless, long set,
                                          long way, bool prefetch, champsim::address evicted_addr, champsim::capability evicted_cap, uint32_t metadata_in,
                                          uint32_t metadata_evict, uint32_t cpu_evict)
{
  if (evicted_addr == champsim::address{})
    return metadata_in;

  // page path
  page_engine.remove_from_pagemap(evicted_addr, false);
  page_engine.remove_from_pagemap(evicted_addr, true);

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
          region->access_map.at(offset) = false;
          region->prefetch_map.at(offset) = false;
          regions.fill(region.value());
        }
      }
    }
  }

  return metadata_in;
}

void ampm_span::prefetcher_final_stats()
{
  std::cout << "\n=== AMPM-SPAN Final Stats ===\n"
            << "  SPAN_ENABLED: " << SPAN_ENABLED << "  GATE_ENABLED: " << GATE_ENABLED << "  CROSS_ALL: " << CROSS_ALL
            << "\n"
            << "  Bounded by cap:        " << pf_bounded << "\n"
            << "  Zone hash collisions:  " << zone_collision << "\n"
            << "\n ==== Page Path =====\n"
            << "  Page path accesses:    " << page_access << "\n"
            << "  Cursor/line check failed (to page path): " << cursor_check_failed << "\n"
            << "  Eviction cleanup skipped (cursor/line check failed): " << evict_cursor_check_failed << "\n"
            << "  Page-path accesses continuing a run: " << run_accesses << "\n"
            << "  Zone-edge crossings: in object " << span_in_object << ", along run " << span_along_run << ", gated " << span_gated
            << ", unchecked (CROSS_ALL) " << span_unchecked << ", blocked " << span_blocked << "\n";

  if constexpr (GATE_ENABLED) {
    static const char* kinds[NUM_KINDS] = {"RW", "RO", "X"};
    static const char* classes[NUM_CLASSES] = {"<=128B", "<=4KB", "<=64KB", ">64KB"};
    std::cout << "\n  Out-of-object gate      Counter     Issued    Dropped UsedIssued UsedShadow\n";
    for (unsigned k = 0; k < GATE_KEYS; k++) {
      const auto& s = gate_stats[k];
      if (s.issued == 0 && s.dropped == 0)
        continue;
      const std::string name = std::string(kinds[k / NUM_CLASSES]) + " " + classes[k % NUM_CLASSES];
      std::printf("  %-20s %9d %10lu %10lu %10lu %10lu\n", name.c_str(), gate_ctr[k], (unsigned long)s.issued, (unsigned long)s.dropped,
                  (unsigned long)s.used_issued, (unsigned long)s.used_shadow);
    }
  }
}
