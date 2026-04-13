#ifndef CHERI_PTR_CHASE_H
#define CHERI_PTR_CHASE_H

#include <cstdint>
#include <optional>

#include "address.h"
#include "modules.h"
#include "msl/lru_table.h"
#include "cheri_prefetch_utils.h"
#include "capability_memory.h"
#include "bloom_filter.h"

struct cheri_ptr_chase : public champsim::modules::prefetcher {


  constexpr static std::size_t PCT_SETS      = 256;
  constexpr static std::size_t PCT_WAYS      = 4;
  constexpr static uint8_t     CONF_MAX      = 7;     // saturating counter ceiling
  constexpr static uint8_t     CHASE_THRESH  = 3;     // min confidence to issue prefetches
  constexpr static uint8_t     MAX_DEPTH     = 16;     // hard ceiling on chase hops
  constexpr static uint8_t     DEPTH_PROMOTE = 5;     // conf threshold to add a hop
  constexpr static uint8_t     DEPTH_DEMOTE  = 1;     // conf threshold to remove a hop
  constexpr static unsigned PTR_MAP_SIZE = 4096;

  constexpr static bool        DEBUG_PRINT = false;

  struct pct_entry {
    champsim::address ip{};
    uint8_t  confidence{};   // how often this IP finds pointers in cap_mem
    uint8_t  depth_limit{1}; // current chase depth for this IP

    auto index() const {
      using namespace champsim::data::data_literals;
      return ip.slice_upper<2_b>();
    }
    auto tag() const {
      using namespace champsim::data::data_literals;
      return ip.slice_upper<2_b>();
    }
  };

  struct ptr_map_entry {
    uint64_t cl_tag = 0;   // source cacheline
    uint64_t target = 0;   // VA of the pointer inside it
  };

  // 4 capabilities (16B each) can fit in a cache line
  constexpr static unsigned CAP_SLOTS_PER_CL = 4;

  
  champsim::msl::lru_table<pct_entry> pct{PCT_SETS, PCT_WAYS}; // learns which ips are likely to reference a pointer
  ptr_map_entry ptr_map[PTR_MAP_SIZE]{}; // what address does this cacheline point to?
  BloomFilter filter; 

  // stat collection
  uint64_t stat_ptr_found       = 0;
  uint64_t stat_ptr_not_found   = 0;
  uint64_t stat_chase_started   = 0;
  uint64_t stat_pf_issued       = 0;
  uint64_t stat_bloom_filtered  = 0;
  uint64_t stat_null_or_loop    = 0;
  uint64_t stat_conf_too_low    = 0;
  uint64_t stat_map_miss        = 0;
  uint64_t stat_page_crossings  = 0; 


  // Update confidence for this IP based on whether cap_mem had a pointer.
  // Called on every demand access.
  void pct_train(champsim::address ip, bool found_pointer);

  // Follow the pointer chain starting at start_cursor, up to depth_limit hops.
  // Only called when confidence >= CHASE_THRESH.
  void pct_chase(champsim::address ip, uint64_t start_cursor);


  using champsim::modules::prefetcher::prefetcher;

  void     prefetcher_initialize();
  uint32_t prefetcher_cache_operate(champsim::address addr, champsim::address ip, uint8_t cache_hit, bool useful_prefetch, access_type type, uint32_t metadata_in);
  uint32_t prefetcher_cache_fill(champsim::address addr, long set, long way, bool prefetch, champsim::address evicted_addr, uint32_t metadata_in, champsim::capability evicted_cap);
  void     prefetcher_final_stats();
};

#endif