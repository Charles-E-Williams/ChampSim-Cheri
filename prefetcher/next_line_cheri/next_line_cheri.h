#ifndef PREFETCHER_NEXT_LINE_CHERI_H
#define PREFETCHER_NEXT_LINE_CHERI_H

#include <cstdint>

#include "address.h"
#include "modules.h"
#include "cheri_prefetch_utils.h"
#include "cache.h"


struct next_line_cheri : public champsim::modules::prefetcher {
  using prefetcher::prefetcher;
  uint32_t prefetcher_cache_operate(champsim::address addr, champsim::address ip, uint32_t cpu, champsim::capability cap, uint8_t cache_hit,
                                    bool useful_prefetch, access_type type, uint32_t metadata_in, uint32_t metadata_hit);
  uint32_t prefetcher_cache_fill(champsim::address addr, champsim::address ip, uint32_t cpu, champsim::capability cap, bool useless, long set, long way,
                                 bool prefetch, champsim::address evicted_addr, champsim::capability evicted_cap, uint32_t metadata_in,
                                 uint32_t metadata_evict, uint32_t cpu_evict);
  void prefetcher_final_stats();

  uint64_t prefetches_bounded = 0;
 
  // void prefetcher_initialize();
  // void prefetcher_branch_operate(champsim::address ip, uint8_t branch_type, champsim::address branch_target) {}
  // void prefetcher_cycle_operate() {}
};

#endif
