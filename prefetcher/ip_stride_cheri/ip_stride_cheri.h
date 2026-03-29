#ifndef PREFETCHER_IP_STRIDE_CHERI_H
#define PREFETCHER_IP_STRIDE_CHERI_H

#include <cstdint>
#include <optional>

#include "cheri_prefetch_utils.h"
#include "modules.h"
#include "msl/lru_table.h"


struct ip_stride_cheri : public champsim::modules::prefetcher {

  struct cap_entry {
    uint64_t cap_hash{};                    // capability index 
    int64_t  last_offset_accessed{};        // most recent offset accessed 
    int64_t  last_offset_prefetched{};      // most recent offset prefetched 
    int64_t  last_stride{};                 // last observed stride
 
    auto index() const { return cap_hash; }
    auto tag() const { return cap_hash; }
  };

  struct lookahead_entry {
    champsim::address address{};
    champsim::address::difference_type stride{};
    int degree = 0;
    std::optional<champsim::capability> cap{};
  };

  constexpr static std::size_t CAP_TABLE_SETS = 256;
  constexpr static std::size_t CAP_TABLE_WAYS = 4;

  constexpr static int PREFETCH_DEGREE = 4;
  
  champsim::msl::lru_table<cap_entry> cap_table{CAP_TABLE_SETS, CAP_TABLE_WAYS};
  std::optional<lookahead_entry> active_lookahead;
 
  uint64_t cap_prefetches_bounded{};
  uint64_t cap_table_hits{};
  uint64_t cap_table_misses{};
  uint64_t too_small_filtered{};   

public:
  using champsim::modules::prefetcher::prefetcher;

  void prefetcher_initialize();
  uint32_t prefetcher_cache_operate(champsim::address addr, champsim::address ip, uint8_t cache_hit, bool useful_prefetch, access_type type,
                                    uint32_t metadata_in);
  uint32_t prefetcher_cache_fill(champsim::address addr, long set, long way, uint8_t prefetch, champsim::address evicted_addr, uint32_t metadata_in);
  void prefetcher_cycle_operate();
  void prefetcher_final_stats();
};

#endif
