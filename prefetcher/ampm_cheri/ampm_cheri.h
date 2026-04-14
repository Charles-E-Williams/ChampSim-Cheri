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

  void add_to_map(champsim::address v_addr, champsim::address pa, const champsim::capability& cap, bool prefetch);
  bool check_map(champsim::address v_addr, const champsim::capability& cap, bool prefetch);

  void do_prefetch(CACHE* cache, champsim::address pa, champsim::address va,
                   const champsim::capability& cap, uint32_t metadata_in,
                   int degree, bool two_level);

  uint64_t stat_pf_bounded = 0;
  uint64_t stat_small_cap = 0;
  uint64_t stat_zone_collision = 0;

  using prefetcher::prefetcher;
  void prefetcher_initialize();
  uint32_t prefetcher_cache_operate(champsim::address addr, champsim::address ip, bool cache_hit, bool useful_prefetch, access_type type, uint32_t metadata_in);
  uint32_t prefetcher_cache_fill(champsim::address addr, long set, long way, bool prefetch, champsim::address evicted_addr, uint32_t metadata_in, champsim::capability evicted_cap);
  void prefetcher_final_stats();
};

#endif