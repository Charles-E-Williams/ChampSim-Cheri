//=======================================================================================//
// File             : ampm_cheri/ampm_cheri.h
// Description      : CHERI-aware Access Map Pattern Matching (AMPM) prefetcher.
//
//                    Extends baseline AMPM (Ishii, Inaba, Hiraki — JILP 2011):
//
//                    1. Object-relative zones — capability base replaces page
//                       boundary as the zone anchor, unifying multi-page objects.
//                    2. Bounds clipping — prefetch candidates outside the
//                       capability are suppressed via cheri::prefetch_safe().
//                    3. Cross-page stride detection — object-relative offsets
//                       make page boundaries invisible to pattern matching.
//
//                    The access map operates in virtual address space (using
//                    current_v_address from the cache) so that cap-relative
//                    offset computation (v_addr - cap.base) is valid. Prefetch
//                    addresses are converted back to PA for issuance. Cross-page
//                    prefetches within the same object are detected but can only
//                    be issued when the candidate falls on the same VA page as
//                    the current access (same-page PA reconstruction). A private
//                    TLB cache would lift this restriction.
//
// NOTE             : Requires adding to CACHE (inc/cache.h):
//                      champsim::address current_v_address{};
//                    And in cache.cc (try_hit, before impl_prefetcher_cache_operate):
//                      current_v_address = handle_pkt.v_address;
//=======================================================================================//

#ifndef PREFETCHER_AMPM_CHERI_H
#define PREFETCHER_AMPM_CHERI_H

#include <cstdint>
#include <utility>
#include <vector>

#include "champsim.h"
#include "cheri.h"
#include "cheri_prefetch_utils.h"
#include "modules.h"
#include "msl/lru_table.h"

class ampm_cheri : public champsim::modules::prefetcher {

  static constexpr int      PREFETCH_DEGREE = 2;
  static constexpr unsigned ZONE_BITS       = 12;

public:

  static std::size_t lines_per_zone() { return (1u << ZONE_BITS) / BLOCK_SIZE; }

  struct key_extent : champsim::dynamic_extent {
    key_extent() : dynamic_extent(champsim::data::bits{64}, champsim::data::bits{0}) {}
  };
  using region_key_type = champsim::address_slice<key_extent>;

  struct region_type {
    region_key_type key;
    std::vector<bool> access_map;
    std::vector<bool> prefetch_map;

    uint64_t cap_base = 0;
    uint64_t cap_top  = 0;

    region_type() : region_type(region_key_type{}) {}
    explicit region_type(region_key_type k)
        : key(k),
          access_map((1u << ZONE_BITS) / BLOCK_SIZE, false),
          prefetch_map((1u << ZONE_BITS) / BLOCK_SIZE, false) {}
  };

  struct region_indexer {
    auto operator()(const region_type& e) const { return e.key; }
  };

  static constexpr std::size_t REGION_SETS = 64;
  static constexpr std::size_t REGION_WAYS = 4;

  champsim::msl::lru_table<region_type, region_indexer, region_indexer> regions{REGION_SETS, REGION_WAYS};

  static uint64_t make_zone_key(uint64_t cap_base, uint64_t cap_zone_id);

  auto zone_key_and_offset(champsim::address v_addr, const champsim::capability& cap) const -> std::pair<region_key_type, std::size_t>;

  void add_to_map(champsim::address v_addr, const champsim::capability& cap, bool prefetch);
  bool check_map(champsim::address v_addr, const champsim::capability& cap, bool prefetch);

  void do_prefetch(CACHE* cache, champsim::address pa, champsim::address va,
                   const champsim::capability& cap, uint32_t metadata_in,
                   int degree, bool two_level);

  uint64_t stat_cap_lookups              = 0;
  uint64_t stat_cap_hits                 = 0;
  uint64_t stat_pf_issued                = 0;
  uint64_t stat_pf_bounded_by_cap        = 0;
  uint64_t stat_cross_page_detected      = 0;
  uint64_t stat_cross_page_cant_issue    = 0;  

  using prefetcher::prefetcher;
  void prefetcher_initialize();
  uint32_t prefetcher_cache_operate(champsim::address addr, champsim::address ip,
                                    bool cache_hit, bool useful_prefetch,
                                    access_type type, uint32_t metadata_in);
  uint32_t prefetcher_cache_fill(champsim::address addr, long set, long way,
                                 bool prefetch, champsim::address evicted_addr,
                                 uint32_t metadata_in);
  void prefetcher_final_stats();
};

#endif