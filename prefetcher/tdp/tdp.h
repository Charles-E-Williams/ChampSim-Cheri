/*
 * TDP: tag-directed prefetching as a standalone L1D prefetcher (no Berti), recursive. The engine is cheri_tdp.h in
 * this directory; this module only hosts it, with the settings of berti_tdp's proposed recursive design (berti_tdp
 * params TDP_RECUR 1, TDP_RECUR_KEY 5, TDP_RECUR_DECAY 2, TDP_RECUR_DEPTH 7):
 *   - depth up to 7, Berti's 16-bit IP hash as the depth-0 key;
 *   - no in-flight filter of its own: the cache's tag and MSHR lookup drops a prefetch to a line already present and
 *     merges one already in flight;
 *   - the fallback key travels as the parent's PDT slot in the prefetch's metadata (berti_tdp keeps it in Berti's
 *     latency table).
 */
#ifndef PREFETCHER_TDP_H
#define PREFETCHER_TDP_H

#include <cstdint>

#include "cheri_tdp.h"
#include "modules.h"

class tdp : public champsim::modules::prefetcher
{
  cheri_tdp::engine engine{cheri_tdp::settings{7, true, false}}; // max_depth, berti_ip_key, recent_filter

public:
  using prefetcher::prefetcher;

  uint32_t prefetcher_cache_operate(champsim::address addr, champsim::address ip, uint32_t cpu, champsim::capability cap, uint8_t cache_hit,
                                    bool useful_prefetch, access_type type, uint32_t metadata_in, uint32_t metadata_hit);
  uint32_t prefetcher_cache_fill(champsim::address addr, champsim::address ip, uint32_t cpu, champsim::capability cap, bool useless, long set, long way,
                                 bool prefetch, champsim::address evicted_addr, champsim::capability evicted_cap, uint32_t metadata_in,
                                 uint32_t metadata_evict, uint32_t cpu_evict);
  void prefetcher_initialize();
  void prefetcher_cycle_operate();
  void prefetcher_final_stats();
};

#endif
