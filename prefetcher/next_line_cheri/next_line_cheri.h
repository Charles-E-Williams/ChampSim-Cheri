#ifndef PREFETCHER_NEXT_LINE_CHERI_H
#define PREFETCHER_NEXT_LINE_CHERI_H

#include <cstdint>

#include "address.h"
#include "modules.h"
#include "cheri_prefetch_utils.h"
#include "cache.h"


struct next_line_cheri : public champsim::modules::prefetcher {
  using prefetcher::prefetcher;
  uint32_t prefetcher_cache_operate(champsim::address addr, champsim::address ip, uint8_t cache_hit, bool useful_prefetch, access_type type,
                                    uint32_t metadata_in);
  uint32_t prefetcher_cache_fill(champsim::address addr, long set, long way, uint8_t prefetch, champsim::address evicted_addr, uint32_t metadata_in);
  void prefetcher_final_stats();

  uint64_t prefetches_bounded = 0;
  uint64_t prefetches_issued = 0;

  uint64_t cap_lookups = 0;
  uint64_t cap_hits = 0;
 
  // void prefetcher_initialize();
  // void prefetcher_branch_operate(champsim::address ip, uint8_t branch_type, champsim::address branch_target) {}
  // void prefetcher_cycle_operate() {}
};

#endif
