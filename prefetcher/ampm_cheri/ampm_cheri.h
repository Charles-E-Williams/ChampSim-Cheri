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
public:

  // ---- Core AMPM parameters ----
  static constexpr int      BASE_DEGREE       = 2;
  static constexpr int      MAX_DEGREE        = 4;
  static constexpr unsigned ZONE_BITS         = 12;   // 4KB zone granularity

  static constexpr std::size_t REGION_SETS    = 64;
  static constexpr std::size_t REGION_WAYS    = 4;

  static constexpr int CROSS_ZONE_FACTOR      = 2;


  static constexpr uint64_t SMALL_CAP_THRESHOLD = 64;  // 4KB

  static std::size_t lines_per_zone() { return (1u << ZONE_BITS) / BLOCK_SIZE; }

  // Stat buckets: 0=0-128B  1=128B-4KB  2=4KB-64KB  3=64KB-1MB  4=1MB+
  static constexpr int NUM_SIZE_CLASSES = 5;
  static int  cap_size_class(uint64_t cap_length);
  // log2 bucket clamped to [0,31] for hashing
  static uint64_t log2_size_class(uint64_t cap_length);


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
          access_map((1u << ZONE_BITS) / BLOCK_SIZE, false),
          prefetch_map((1u << ZONE_BITS) / BLOCK_SIZE, false) {}
  };

  struct region_indexer {
    auto operator()(const region_type& e) const { return e.key; }
  };

  champsim::msl::lru_table<region_type, region_indexer, region_indexer>
      regions{REGION_SETS, REGION_WAYS};


  static constexpr std::size_t PAGE_REGION_SETS = 64;
  static constexpr std::size_t PAGE_REGION_WAYS = 4;

  struct page_region_type {
    region_key_type   key;
    std::vector<bool> access_map;
    std::vector<bool> prefetch_map;

    page_region_type() : page_region_type(region_key_type{}) {}
    explicit page_region_type(region_key_type k)
        : key(k),
          access_map((1u << ZONE_BITS) / BLOCK_SIZE, false),
          prefetch_map((1u << ZONE_BITS) / BLOCK_SIZE, false) {}
  };

  struct page_region_indexer {
    auto operator()(const page_region_type& e) const { return e.key; }
  };

  champsim::msl::lru_table<page_region_type, page_region_indexer, page_region_indexer>
      page_regions{PAGE_REGION_SETS, PAGE_REGION_WAYS};


  static constexpr std::size_t SHT_SETS       = 32;
  static constexpr std::size_t SHT_WAYS       = 4;
  static constexpr uint8_t     SHT_CONF_MAX   = 7;
  static constexpr uint8_t     SHT_CONF_BOOST = 3;

  struct stride_hint_entry {
    uint64_t key{};
    int16_t  predicted_stride{};   // CL units, object-relative
    int32_t  last_cl_offset{};     // last cap-relative CL offset
    uint16_t cap_base_tag{};       // 16-bit hash for object-switch detection
    uint8_t  confidence{};         // [0, SHT_CONF_MAX] saturating

    auto index() const { return key & 0x7FFFFFFFFFFFFFFFULL; }
    auto tag()   const { return key; }
  };

  champsim::msl::lru_table<stride_hint_entry> sht{SHT_SETS, SHT_WAYS};


  static uint64_t make_zone_key(uint64_t cap_base, uint64_t cap_zone_id);

  auto zone_key_and_offset(champsim::address v_addr, const champsim::capability& cap) const-> std::pair<region_key_type, std::size_t>;

  void add_to_map(champsim::address v_addr, champsim::address pa,
                  const champsim::capability& cap, bool prefetch);
  bool check_map(champsim::address v_addr,
                 const champsim::capability& cap, bool prefetch);


  auto page_zone_key_and_offset(champsim::address pa) const
      -> std::pair<region_key_type, std::size_t>;

  void page_add_to_map(champsim::address pa, bool prefetch);
  bool page_check_map(champsim::address pa, bool prefetch);

  void page_do_prefetch(CACHE* cache, champsim::address pa,
                        uint32_t metadata_in, int degree, bool two_level,
                        int size_cls);

  static uint64_t stride_hint_key(champsim::address ip,
                                  const champsim::capability& cap);
  static uint16_t base_tag(uint64_t cap_base);

  int  update_and_query_degree(champsim::address ip, champsim::address va,
                               const champsim::capability& cap);
  void sht_reward(champsim::address ip, const champsim::capability& cap);


  void do_prefetch(CACHE* cache, champsim::address pa, champsim::address va,
                   const champsim::capability& cap, uint32_t metadata_in,
                   int degree, bool two_level);


  uint64_t stat_pf_bounded      = 0;
  uint64_t stat_sealed_bail     = 0;
  uint64_t stat_zone_collision  = 0;
  uint64_t stat_cross_zone      = 0;

  uint64_t stat_page_access     = 0;   // accesses routed to page path
  uint64_t stat_page_pf         = 0;   // prefetches issued via page path

  uint64_t stat_sht_hit         = 0;
  uint64_t stat_sht_miss        = 0;
  uint64_t stat_sht_confirmed   = 0;
  uint64_t stat_sht_rewarded    = 0;
  uint64_t stat_degree_boosted  = 0;

  uint64_t stat_pf_by_class[NUM_SIZE_CLASSES]     = {};
  uint64_t stat_useful_by_class[NUM_SIZE_CLASSES]  = {};
  uint64_t stat_access_by_class[NUM_SIZE_CLASSES]  = {};

  using prefetcher::prefetcher;

  void     prefetcher_initialize();
  uint32_t prefetcher_cache_operate(champsim::address addr, champsim::address ip,
                                    bool cache_hit, bool useful_prefetch,
                                    access_type type, uint32_t metadata_in);
  uint32_t prefetcher_cache_fill(champsim::address addr, long set, long way,
                                 bool prefetch, champsim::address evicted_addr,
                                 uint32_t metadata_in,
                                 champsim::capability evicted_cap);
  void     prefetcher_final_stats();
};

#endif