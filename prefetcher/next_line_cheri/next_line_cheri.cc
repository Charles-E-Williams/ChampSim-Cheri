#include "next_line_cheri.h"

uint32_t next_line_cheri::prefetcher_cache_operate(champsim::address addr, champsim::address ip, uint32_t cpu, champsim::capability cap, uint8_t cache_hit,
                                                   bool useful_prefetch, access_type type, uint32_t metadata_in, uint32_t metadata_hit)
{

  if (!cheri::is_tag_valid(cap))
    return metadata_in;

  if (!cheri::has_prefetchable_range_forward(cap)) {
    prefetches_bounded++;
    return metadata_in;
  }

  champsim::block_number pf_block{addr};
  champsim::address next_line_addr{pf_block + 1};
  if (!cheri::prefetch_safe(next_line_addr, cap)) { //no prefetch if the address is outside the bounds of the capability
    prefetches_bounded++;
    return metadata_in;
  }

  prefetch_line(next_line_addr, true, metadata_in);
  return metadata_in;
}

uint32_t next_line_cheri::prefetcher_cache_fill(champsim::address addr, champsim::address ip, uint32_t cpu, champsim::capability cap, bool useless, long set,
                                                long way, bool prefetch, champsim::address evicted_addr, champsim::capability evicted_cap,
                                                uint32_t metadata_in, uint32_t metadata_evict, uint32_t cpu_evict)
{
  return metadata_in;
}

void next_line_cheri::prefetcher_final_stats() {
  std::cout << "Next-Line CHERI Prefetcher Stats:" << "\n";
  std::cout << "Prefetches Bounded by Bounds: " << prefetches_bounded << "\n";
}
