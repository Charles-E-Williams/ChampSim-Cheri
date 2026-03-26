#include "next_line_cheri.h"

uint32_t next_line_cheri::prefetcher_cache_operate(champsim::address addr, champsim::address ip, uint8_t cache_hit, bool useful_prefetch, access_type type,
                                             uint32_t metadata_in)
{

  auto auth_cap = intern_->get_authorizing_capability(); cap_lookups++;

  champsim::block_number pf_block{addr};
  champsim::address next_line_addr{pf_block + 1};
  if (cheri::is_tag_valid(auth_cap)) {
    cap_hits++;
    if (!cheri::prefetch_safe(next_line_addr, auth_cap)) { //no prefetch if the address is outside the bounds of the capability
      prefetches_bounded++; 
      return metadata_in;
    }
  }

  prefetch_line(next_line_addr, true, metadata_in);
  prefetches_issued++;
  return metadata_in;
}

uint32_t next_line_cheri::prefetcher_cache_fill(champsim::address addr, long set, long way, uint8_t prefetch, champsim::address evicted_addr, uint32_t metadata_in)
{
  return metadata_in;
}

void next_line_cheri::prefetcher_final_stats() {
  std::cout << "Next-Line CHERI Prefetcher Stats:" << "\n";
  std::cout << "Prefetches Issued: " << prefetches_issued << "\n";
  std::cout << "Prefetches Bounded by Bounds: " << prefetches_bounded << "\n";
  std::cout << "Capability Lookups: " << cap_lookups << "\n";
  std::cout << "Capability Hits: " << cap_hits << "\n";
}
