#ifndef PREFETCHER_C_AMPM_H
#define PREFETCHER_C_AMPM_H

#include <array>
#include <cstdint>

#include "../ampm/ampm.h"
#include "champsim.h"
#include "cheri.h"
#include "cheri_prefetch_utils.h"
#include "modules.h"
#include "msl/lru_table.h"


struct c_ampm : public champsim::modules::prefetcher {

  // Reuse baseline AMPM as a pure pattern detector. We use its bitmap
  // primitives (add_to_pagemap, check_pagemap, remove_from_pagemap) but
  // write our own do_prefetch so we can insert tag/bounds/squash logic.
  std::vector<ampm::AMPM_Module> AMPM_Modules;


  // -------------------------------------------------------------------------
  // Capability Confidence Table
  // Per-stream confidence keyed on (IP-hash, cap_base_tag).
  // No stride / direction state -- baseline AMPM does bidirectional matching.
  // -------------------------------------------------------------------------
  struct capability_confidence_table {
    static constexpr std::size_t SETS = 16;
    static constexpr std::size_t WAYS = 8;

    static constexpr uint8_t MAX_CONFIDENCE      = 255;
    static constexpr uint8_t STARTING_CONFIDENCE = 180;

    // Sample-counter promotion: accumulate events, apply confidence delta
    // when a counter saturates (avoids per-event oscillation).
    static constexpr uint8_t ISSUE_COUNTER_MAX  = 5;
    static constexpr uint8_t ISSUE_CONF_DECR    = 4;
    static constexpr uint8_t USEFUL_COUNTER_MAX = 4;
    static constexpr uint8_t USEFUL_CONF_INCR   = 4;
    static constexpr uint8_t DIR_COUNTER_MAX = 7;
    static constexpr uint8_t DIR_FORWARD_THRESH = 4;
    static constexpr uint8_t STARTING_DIR_FORWARD_CTR = 2;

    static constexpr std::array<uint8_t, 16> CONF_THRESH     = {16, 32, 48, 64, 80, 96, 112, 128, 144, 160, 176, 192, 208, 224, 240, 255};
    static constexpr std::array<uint8_t, 16> CONF_DEPTHS     = {1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6};
    static constexpr std::array<uint8_t, 16> SQUASH_CHANCE   = {127, 120, 110, 100, 90, 80, 70, 60, 50, 40, 30, 20, 10, 5, 0, 0};
    static constexpr std::array<uint8_t, 16> CONF_ZONE_LA = {0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3};

    struct entry {
      uint64_t key{};               // (IP-hash, cap_base_tag)
      uint16_t cap_base_tag{};      // collision detect for cap-switch
      uint8_t  confidence{STARTING_CONFIDENCE};
      uint8_t  issue_counter{};
      uint8_t  useful_counter{};
      uint8_t  direction{STARTING_DIR_FORWARD_CTR};
    };
    struct set_indexer { auto operator()(const entry& e) const { return e.key; } };
    struct way_indexer { auto operator()(const entry& e) const { return e.key; } };

    champsim::msl::lru_table<entry, set_indexer, way_indexer> table{SETS, WAYS};

    // stats
    uint64_t hit      = 0;
    uint64_t miss     = 0;
    uint64_t promoted = 0;
    uint64_t demoted  = 0;
    uint64_t squashed = 0;
    std::array<uint64_t, 16> prefetches_at_conf{};
    std::array<uint64_t, 16> useful_at_conf{};

    static uint64_t make_key(champsim::address ip, champsim::capability& cap);
    static uint16_t base_tag(uint64_t cap_base);
    static uint8_t  conf_increment(uint8_t conf, uint8_t amount);
    static uint8_t  conf_decrement(uint8_t conf, uint8_t amount);
    static int      conf_to_depth(uint8_t conf);
    static uint8_t  squash_chance_for(uint8_t conf);

    // Look up confidence for this stream. If no entry exists, install one
    // seeded with `warm_start` (from cap_stats). Always returns the
    // current entry's confidence.
    uint8_t lookup_or_install(champsim::address ip,
                              const champsim::capability& cap,
                              uint8_t warm_start);
    void on_useful(champsim::address ip, const champsim::capability& cap);
    void on_fill(champsim::address ip, const champsim::capability& cap);
    void print_stats();
  };
  capability_confidence_table cct;


  struct capability_stats_table {
    static constexpr std::size_t SETS = 16;
    static constexpr std::size_t WAYS = 4;
    static constexpr uint16_t COUNT_MAX = 1023;     // 10-bit saturating counters

    struct entry {
      uint64_t cap_tag{};
      uint16_t useful_count{0};
      uint16_t issue_count{0};
    };
    struct set_indexer { auto operator()(const entry& e) const { return e.cap_tag; } };
    struct way_indexer { auto operator()(const entry& e) const { return e.cap_tag; } };

    champsim::msl::lru_table<entry, set_indexer, way_indexer> table{SETS, WAYS};

    static uint64_t make_key(const champsim::capability& cap);


    uint8_t warm_start(const champsim::capability& cap);
    void    on_useful(const champsim::capability& cap);
    void    on_fill(const champsim::capability& cap);
    void    print_stats();
  };
  capability_stats_table cap_stats;


  uint64_t pf_bounded     = 0;    
  uint64_t pf_squashed    = 0; 


  void do_prefetch(CACHE* intern,
                   champsim::address pa,
                   champsim::address ip,
                   champsim::capability& cap,
                   uint32_t metadata_in,
                   int degree,
                   bool two_level);


  using prefetcher::prefetcher;

  void     prefetcher_initialize();
  uint32_t prefetcher_cache_operate(champsim::address addr,
                                    champsim::address ip,
                                    uint32_t cpu,
                                    champsim::capability cap,
                                    bool cache_hit,
                                    bool useful_prefetch,
                                    access_type type,
                                    uint32_t metadata_in,
                                    uint32_t metadata_hit);
  uint32_t prefetcher_cache_fill(champsim::address addr,
                                 champsim::address ip,
                                 uint32_t cpu,
                                 champsim::capability cap,
                                 bool useless,
                                 long set,
                                 long way,
                                 bool prefetch,
                                 champsim::address evicted_addr,
                                 champsim::capability evicted_cap,
                                 uint32_t metadata_in,
                                 uint32_t metadata_evict,
                                 uint32_t cpu_evict);
  void     prefetcher_final_stats();
};

#endif