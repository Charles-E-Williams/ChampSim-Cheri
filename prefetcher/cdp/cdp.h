#ifndef PREFETCHER_CDP_H
#define PREFETCHER_CDP_H

/*
 * CDP: a naive content-directed prefetcher (Cooksey et al., ASPLOS 2002) for CHERI.
 *
 * Every line filled into the cache is scanned for pointers, and the line each pointer points to is prefetched. Lines
 * CDP fetched are scanned the same way, so prefetches chain up to a depth limit from the fill that started them. There
 * is no learning, no confidence and no filter.
 *
 * Cooksey identifies pointers by comparing a word's upper bits with the line's own address. ChampSim traces carry no
 * data values, so pointers are identified by their CHERI tags instead (exact: no non-pointer data is followed).
 *
 * CDP_Module is the engine; another prefetcher can embed it (as ampm::AMPM_Module) by calling do_prefetch from its
 * cache_fill hook with the fill's metadata. CDP marks its prefetches with bits [31:28] of the metadata, so a host must
 * not use those bits in the metadata of its own prefetches.
 */

#include <cstdint>

#include "cache.h"
#include "capability_memory.h"
#include "champsim.h"
#include "cheri_prefetch_utils.h"
#include "modules.h"

class cdp : public champsim::modules::prefetcher
{
  static constexpr bool CDP_FILL_L1 = true;    // prefetch into this cache (else the next level)

public:
  class CDP_Module
  {
  public:
    static constexpr unsigned MAX_DEPTH = 3; // the largest depth limit the metadata can carry

    // Prefetch metadata: [31] issued by CDP, [30:28] depth of the prefetched line (1 = found in a line CDP did not fetch)
    static constexpr uint32_t MD_CDP = 1u << 31;
    static constexpr unsigned MD_DEPTH_SHIFT = 28;
    static constexpr uint32_t MD_MASK = MD_CDP | (0x7u << MD_DEPTH_SHIFT);
    static unsigned md_depth(uint32_t metadata) { return (metadata & MD_CDP) ? (metadata >> MD_DEPTH_SHIFT) & 0x7 : 0; }

    // Statistics (cumulative, warmup included)
    uint64_t lines_scanned = 0, pointers_found = 0, depth_stop = 0, pq_full = 0;
    uint64_t issued[MAX_DEPTH + 1] = {};

    // Scan the line filled at addr; prefetch the line of each pointer found, unless the line is max_depth links deep
    void do_prefetch(CACHE* intern, champsim::address addr, uint32_t cpu, uint32_t metadata_in, bool fill_this_level);
    void print_stats() const;
  };

  using prefetcher::prefetcher;
  uint32_t prefetcher_cache_operate(champsim::address addr, champsim::address ip, uint32_t cpu, champsim::capability cap, uint8_t cache_hit,
                                    bool useful_prefetch, access_type type, uint32_t metadata_in, uint32_t metadata_hit);
  uint32_t prefetcher_cache_fill(champsim::address addr, champsim::address ip, uint32_t cpu, champsim::capability cap, bool useless, long set, long way,
                                 bool prefetch, champsim::address evicted_addr, champsim::capability evicted_cap, uint32_t metadata_in,
                                 uint32_t metadata_evict, uint32_t cpu_evict);
  void prefetcher_final_stats();

  // void prefetcher_initialize();
  // void prefetcher_branch_operate(champsim::address ip, uint8_t branch_type, champsim::address branch_target) {}
  // void prefetcher_cycle_operate() {}
  CDP_Module engine;
};

#endif