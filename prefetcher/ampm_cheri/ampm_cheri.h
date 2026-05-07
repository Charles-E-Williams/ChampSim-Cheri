#ifndef PREFETCHER_AMPM_CHERI_H
#define PREFETCHER_AMPM_CHERI_H

#include <cstdint>
#include <utility>
#include <vector>
#include <array>

#include "champsim.h"
#include "cheri.h"
#include "cheri_prefetch_utils.h"
#include "modules.h"
#include "msl/lru_table.h"



class ampm_cheri : public champsim::modules::prefetcher {
public:


  static uint64_t get_ip_hash(uint64_t key)
  {
    // Robert Jenkins' 32 bit mix function
    key += (key << 12);
    key ^= (key >> 22);
    key += (key << 4);
    key ^= (key >> 9);
    key += (key << 10);
    key ^= (key >> 2);
    key += (key << 7);
    key ^= (key >> 12);

    // Knuth's multiplicative method
    key = (key >> 3) * 2654435761;

    return key;
  }
  typedef enum  {
    SMALL = 0,
    MEDIUM,
    LARGE,
    XLARGE,
    XXL,
    NUM_SIZES
  } capability_size;

  static constexpr unsigned CHERI_AMPM_ZONE_BITS  = 12; 
  static constexpr int      MAX_LOG2_SIZE     = 31;
  static constexpr std::size_t REGION_SETS    = 64;
  static constexpr std::size_t REGION_WAYS    = 4;
  static constexpr uint64_t MIN_CAP_CACHE_LINES = 8;  

  static constexpr uint64_t WATCHDOG_INTERVAL = 65536;
  uint64_t watchdog_counter = 0;

  static std::size_t lines_per_zone() { return (1u << CHERI_AMPM_ZONE_BITS) / BLOCK_SIZE; } // 64 cache lines in a capability zone

  static capability_size cap_size(uint64_t cap_length);
  static uint64_t log2_size(uint64_t cap_length);

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
          access_map((1u << CHERI_AMPM_ZONE_BITS) / BLOCK_SIZE, false),
          prefetch_map((1u << CHERI_AMPM_ZONE_BITS) / BLOCK_SIZE, false) {}
  };

  struct region_indexer {
    auto operator()(const region_type& e) const { return e.key; }
  };

  champsim::msl::lru_table<region_type, region_indexer, region_indexer> regions{REGION_SETS, REGION_WAYS};

  struct capability_confidence_table {
    static constexpr std::size_t SETS = 16;
    static constexpr std::size_t WAYS = 8;

    static constexpr uint8_t MAX_CONFIDENCE      = 255;
    static constexpr uint8_t STARTING_CONFIDENCE = 180;

    // increment counter on prefetch fill, reset counter and decrement when counter saturates
    static constexpr uint8_t ISSUE_COUNTER_MAX  = 5;
    static constexpr uint8_t ISSUE_COUNTER_INCR = 1;
    static constexpr uint8_t ISSUE_CONF_DECR    = 4;

    // increments when demand hits a prefetched line, reset counter and increment confidence on saturation
    static constexpr uint8_t USEFUL_COUNTER_MAX  = 4;
    static constexpr uint8_t USEFUL_COUNTER_INCR = 1;
    static constexpr uint8_t USEFUL_CONF_INCR    = 4;

    static constexpr uint8_t DIR_COUNTER_MAX = 7;
    static constexpr uint8_t DIR_FORWARD_THRESH = 4;
    static constexpr uint8_t STARTING_DIR_FORWARD_CTR = 2;    

    static constexpr std::array<uint8_t, 16> CONF_THRESH     = {16, 32, 48, 64, 80, 96, 112, 128, 144, 160, 176, 192, 208, 224, 240, 255};
    static constexpr std::array<uint8_t, 16> CONF_DEPTHS     = {1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6};
    static constexpr std::array<uint8_t, 16> SQUASH_CHANCE   = {127, 120, 110, 100, 90, 80, 70, 60, 50, 40, 30, 20, 10, 5, 0, 0};
    static constexpr std::array<uint8_t, 16> CONF_ZONE_LA = {0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3};

    struct entry {
      uint64_t key{};
      uint16_t cap_base_tag{};       // 16 bit hash for cap-switch detection
      uint8_t  confidence{STARTING_CONFIDENCE};
      uint8_t  issue_counter{};
      uint8_t  useful_counter{};
      uint8_t  direction{STARTING_DIR_FORWARD_CTR};
      uint32_t last_cl_offset{};
      bool     has_last_cl_offset{false}; 

    };
    struct set_indexer {
      auto operator()(const entry& e) const { return e.key & INT64_MAX; }
    };
    struct way_indexer {
      auto operator()(const entry& e) const { return e.key & INT64_MAX; }
    };

    champsim::msl::lru_table<entry, set_indexer, way_indexer> table{SETS, WAYS};

    uint64_t hit              = 0;
    uint64_t miss             = 0;
    uint64_t promoted         = 0;
    uint64_t demoted          = 0;
    uint64_t squashed         = 0;
    uint64_t degree_incr   = 0;

    // per-confidence-bin histograms 
    std::array<uint64_t, 16> prefetches_at_conf{};
    std::array<uint64_t, 16> useful_at_conf{};

    
    static uint64_t make_key(champsim::address ip, const champsim::capability& cap);
    static uint16_t base_tag(uint64_t cap_base);
    static uint8_t  conf_increment(uint8_t conf, uint8_t amount);
    static uint8_t  conf_decrement(uint8_t conf, uint8_t amount);
    static int      conf_to_depth(uint8_t conf);
    static int      conf_to_zone_la(uint8_t conf);
    static uint8_t  squash_chance(uint8_t conf);

    uint8_t  update_and_query(champsim::address ip, champsim::address va, champsim::capability& cap);
    int      get_direction(champsim::address ip, const champsim::capability& cap);    
    void     update_on_useful_pf(champsim::address ip, champsim::capability& cap);
    void     update_on_fill(champsim::address ip, champsim::capability& cap);
    void     print_stats() 
    {
      std::cout << "=============CAPABILITY CONFIDENCE TABLE=============\n";
      std::cout << "HITS "  << hit << " \n";
      std::cout << "MISSES " << miss << " \n";
      std::cout << "PROMOTIONS " << promoted << " \n";
      std::cout << "DEMOTED " << demoted << " \n";
      std::cout << "SQUASHED " << squashed << " \n";
      std::cout << "DEGREE INCREASED " << degree_incr << "\n\n";
    }
  };
  capability_confidence_table cct;
 

  struct zone_walker_table {
    static constexpr std::size_t SETS    = 32;
    static constexpr std::size_t WAYS    = 4;
    static constexpr std::size_t IP_WAYS = 4;
    static constexpr uint8_t STARTING_CONFIDENCE = 180;
    static constexpr uint8_t ISSUE_COUNTER_MAX   = 5;
    static constexpr uint8_t USEFUL_COUNTER_MAX  = 4;
    static constexpr uint8_t ISSUE_CONF_DECR     = 4;
    static constexpr uint8_t USEFUL_CONF_INCR    = 4;

    struct ip_hash_indexer {
      auto operator()(const uint16_t& h) const { return h; }
    };
    struct entry {
      region_key_type key;
      champsim::msl::lru_table<uint16_t, ip_hash_indexer, ip_hash_indexer>ip_hashes{1, IP_WAYS};
      uint8_t confidence{STARTING_CONFIDENCE};
      uint8_t issue_counter{};
      uint8_t useful_counter{};

      entry() = default;
      explicit entry(region_key_type k) : key(k),confidence(STARTING_CONFIDENCE){}
    };

    struct indexer {
      auto operator()(const entry& e) const { return e.key; }
    };

    champsim::msl::lru_table<entry, indexer, indexer> table{SETS, WAYS};


    uint64_t zone_boost_eligible = 0;  // had higher confidence than current IP's conf
    uint64_t zone_boost_applied = 0;  // lookup changed the depth

    void    log_ip(region_key_type zone_key, uint16_t ip_hash);
    uint8_t zone_boost_confidence(region_key_type zone_key,uint16_t current_ip_hash, const capability_confidence_table& cct);
    void    update_on_useful_pf(region_key_type zone_key);
    void    update_on_issued_pf(region_key_type zone_key);
    void    print_stats()
    {
      std::cout << "=============ZONE WALKER TABLE=============\n";
      std::cout << "Zone boost eligible: " << zone_boost_eligible << "\n"
            << "Zone boost applied:  " << zone_boost_applied << "\n";
    }
  };

  zone_walker_table zwt;

  static constexpr std::size_t SAMPLE_TABLE_SETS = 64;
  static constexpr std::size_t SAMPLE_TABLE_WAYS = 4;
  static constexpr uint64_t    LATENCY_EPOCH_CYCLES = 300000;   
  static constexpr uint64_t    LATENCY_MIN_SAMPLES  = 100;      
  static constexpr double      LATENCY_RATIO_THRESH = 0.8;      // pf_lat > 0.8 * demand_lat ? : throttle

  struct sample_table_entry {
    champsim::block_number block;
    uint64_t cycle_missed;
    uint8_t cap_size_class; 
    sample_table_entry() : sample_table_entry(champsim::address{}, 0, 0xFF) {}
    explicit sample_table_entry(champsim::address addr, uint64_t cycles, uint8_t size)
        : block(champsim::block_number{addr}), cycle_missed(cycles), cap_size_class(size) {}
  };
  struct sample_indexer {
    auto operator()(const sample_table_entry& e) const { return e.block.to<uint64_t>() & INT64_MAX; }
  };

  champsim::msl::lru_table<sample_table_entry, sample_indexer, sample_indexer> demand_sample_table{SAMPLE_TABLE_SETS, SAMPLE_TABLE_WAYS};
  champsim::msl::lru_table<sample_table_entry, sample_indexer, sample_indexer> prefetch_sample_table{SAMPLE_TABLE_SETS, SAMPLE_TABLE_WAYS};

  static uint64_t make_zone_key(uint64_t cap_base, uint64_t cap_zone_id);
  auto zone_key_and_offset(champsim::address v_addr, const champsim::capability& cap) const-> std::pair<region_key_type, std::size_t>;
  void add_to_map(champsim::address v_addr, const champsim::capability& cap, bool prefetch);
  bool check_map(champsim::address v_addr, const champsim::capability& cap, bool prefetch);


  void do_prefetch(CACHE* cache, champsim::address pa, champsim::address va, champsim::address ip, champsim::capability& cap, uint32_t metadata_in, uint32_t cpu, int degree, int zone_la, int direction, bool two_level);

  // epoch-based usefulness tracking
  static constexpr uint64_t GLOBAL_EPOCH_LENGTH = 8192;    // accesses per epoch
  static constexpr uint8_t  GLOBAL_CONF_INCR  = 10;      // tighten when accuracy low
  static constexpr uint8_t  GLOBAL_CONF_DECR  = 5;       // relax when accuracy good
  static constexpr uint8_t  GLOBAL_SQUASH_MOD_MAX   = 128;      // max global squash penalty
  static constexpr double   GLOBAL_TARGET_ACCURACY = 0.75;  // useful/issued threshold

  uint64_t global_epoch_counter = 0;
  uint64_t global_useless_epoch  = 0;
  uint64_t global_useful_epoch  = 0;
  uint8_t  global_conf_modifier = 0;
  double   global_usefulness    = 1.0;

  uint64_t prefetch_latency_cycles_epoch = 0;
  uint64_t prefetch_sampled_epoch        = 0;
  uint64_t demand_latency_cycles_epoch   = 0;
  uint64_t demand_sampled_epoch          = 0;
  uint64_t latency_epoch_cycle_counter   = 0;

  //stats 
  uint64_t pf_bounded           = 0;
  uint64_t zone_collision       = 0;
  uint64_t cross_zone           = 0;
  uint64_t pf_by_size[capability_size::NUM_SIZES]     = {};
  uint64_t useful_by_size[capability_size::NUM_SIZES]  = {};
  uint64_t access_by_size[capability_size::NUM_SIZES]  = {};


  using prefetcher::prefetcher;
  void prefetcher_initialize();
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
  void prefetcher_cycle_operate();
  void prefetcher_final_stats();
};

#endif