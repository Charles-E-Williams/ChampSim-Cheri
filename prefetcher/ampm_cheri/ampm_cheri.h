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
#include "../ampm/ampm.h"



class ampm_cheri : public champsim::modules::prefetcher {
public:

  typedef enum  {
    SMALL = 0,
    MEDIUM,
    LARGE,
    XLARGE,
    XXL,
    NUM_SIZES
  } capability_size;

  static constexpr int      PREFETCH_DEGREE   = 2;
  static constexpr unsigned AMPM_REGION_BITS  = 12;   // 4KB zone granularity
  static constexpr std::size_t REGION_SETS    = 64;
  static constexpr std::size_t REGION_WAYS    = 4;

  static constexpr uint64_t SMALL_CAP_THRESHOLD = 64;  // one zone worth of cachelines (4KB)

  static std::size_t lines_per_zone() { return (1u << AMPM_REGION_BITS) / BLOCK_SIZE; }

  static capability_size cap_size_category(uint64_t cap_length);

  struct key_extent : champsim::dynamic_extent {
    key_extent() : dynamic_extent(champsim::data::bits{64},
                                  champsim::data::bits{0}) {}
  };
  using region_key_type = champsim::address_slice<key_extent>;

  struct region_type {
    region_key_type   key;
    std::vector<bool> access_map;
    std::vector<bool> prefetch_map;
    uint64_t          cap_base     = 0;   // collision detection

    region_type() : region_type(region_key_type{}) {}
    explicit region_type(region_key_type k)
        : key(k),
          access_map((1u << AMPM_REGION_BITS) / BLOCK_SIZE, false),
          prefetch_map((1u << AMPM_REGION_BITS) / BLOCK_SIZE, false) {}
  };

  struct region_indexer {
    auto operator()(const region_type& e) const { return e.key; }
  };

  champsim::msl::lru_table<region_type, region_indexer, region_indexer> regions{REGION_SETS, REGION_WAYS};


  static uint64_t make_zone_key(uint64_t cap_base, uint64_t cap_zone_id);
  auto zone_key_and_offset(champsim::address v_addr, const champsim::capability& cap) const-> std::pair<region_key_type, std::size_t>;
  void add_to_map(champsim::address v_addr, champsim::address pa, const champsim::capability& cap, bool prefetch);
  bool check_map(champsim::address v_addr, const champsim::capability& cap, bool prefetch);
  void do_prefetch(CACHE* cache, champsim::address pa, champsim::address va, const champsim::capability& cap, uint32_t metadata_in, int degree, bool two_level);

  ampm::AMPM_Module page_engine; //baseline AMPM fallback


  uint64_t pf_bounded      = 0;
  uint64_t zone_collision  = 0;
  uint64_t page_access     = 0;   // accesses routed to page path

  uint64_t useful_by_size[NUM_SIZES]  = {};
  uint64_t access_by_size[NUM_SIZES]  = {};

  using prefetcher::prefetcher;

  uint32_t prefetcher_cache_operate(champsim::address addr, champsim::address ip,
                                    uint32_t cpu, champsim::capability cap,
                                    bool cache_hit, bool useful_prefetch,
                                    access_type type, uint32_t metadata_in,
                                    uint32_t metadata_hit);
  uint32_t prefetcher_cache_fill(champsim::address addr, champsim::address ip,
                                 uint32_t cpu, champsim::capability cap,
                                 bool useless, long set, long way,
                                 bool prefetch, champsim::address evicted_addr,
                                 champsim::capability evicted_cap, uint32_t metadata_in,
                                 uint32_t metadata_evict, uint32_t cpu_evict);
  void     prefetcher_final_stats();
};

#endif