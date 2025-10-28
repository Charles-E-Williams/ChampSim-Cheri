#ifndef PREFETCHER_CHERI_H
#define PREFETCHER_CHERI_H

#include <cstdint>
#include <vector>
#include <optional>

#include "cache.h"
#include "modules.h"
#include "msl/lru_table.h"
#include "capability_memory.h"


struct cheri_stride_entry {
  champsim::address ip{};
  champsim::block_number last_cl_addr{};
  champsim::block_number::difference_type last_stride{};
  
  // Required methods for lru_table
  auto index() const {
    using namespace champsim::data::data_literals;
    return ip.slice_upper<2_b>();
  }
  
  auto tag() const {
    using namespace champsim::data::data_literals;
    return ip.slice_upper<2_b>();
  }
};

struct cheri_lookahead {
  champsim::address pf_address;
  champsim::block_number::difference_type stride;
  int degree;
  champsim::address cap_base;      // Capability base
  champsim::address cap_bound;     // Capability upper bound (base + length)
};

class cheri_aware : public champsim::modules::prefetcher
{
private:
  using stride_table_type = champsim::msl::lru_table<cheri_stride_entry>;
  
  constexpr static std::size_t TRACKER_SETS = 256;
  constexpr static std::size_t TRACKER_WAYS = 4;
  constexpr static int PREFETCH_DEGREE = 3;
  
  stride_table_type table{TRACKER_SETS, TRACKER_WAYS}; // Track 256 sets, 4 ways
  
  std::optional<cheri_lookahead> active_lookahead;
  
  uint32_t cpu_id;
  
  // Statistics
  uint64_t stat_prefetches_issued = 0;
  uint64_t stat_prefetches_bounds_ok = 0;
  uint64_t stat_prefetches_blocked_by_bounds = 0;
  uint64_t stat_prefetches_no_capability = 0;
  uint64_t stat_capability_hits = 0;

public:
  using prefetcher::prefetcher;

  void prefetcher_initialize();
  uint32_t prefetcher_cache_operate(champsim::address addr, champsim::address ip, 
                                    uint8_t cache_hit, bool useful_prefetch, 
                                    access_type type, uint32_t metadata_in);
  uint32_t prefetcher_cache_fill(champsim::address addr, long set, long way, 
                                 uint8_t prefetch, champsim::address evicted_addr, 
                                 uint32_t metadata_in);
  void prefetcher_cycle_operate();
  void prefetcher_final_stats();
};

#endif