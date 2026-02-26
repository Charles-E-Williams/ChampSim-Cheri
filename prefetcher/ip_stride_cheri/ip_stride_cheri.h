#ifndef IP_STRIDE_CHERI_H
#define IP_STRIDE_CHERI_H

#include <cstdint>
#include <optional>

#include "msl/cheri_utils.h"
#include "modules.h"
#include "msl/lru_table.h"

struct ip_stride_cheri : public champsim::modules::prefetcher {
  struct tracker_entry {
    champsim::address ip{};
    champsim::block_number last_cl_addr{};
    champsim::block_number::difference_type last_stride{};
    
    auto index() const
    {
      using namespace champsim::data::data_literals;
      return ip.slice_upper<2_b>();
    }
    auto tag() const
    {
      using namespace champsim::data::data_literals;
      return ip.slice_upper<2_b>();
    }
  };

  struct lookahead_entry {
    champsim::address address{};
    champsim::address::difference_type stride{};
    int degree = 0;
    std::optional<champsim::capability> cap{};
  };

  constexpr static std::size_t TRACKER_SETS = 256;
  constexpr static std::size_t TRACKER_WAYS = 4;
  constexpr static int PREFETCH_DEGREE = 32;

  std::optional<lookahead_entry> active_lookahead;
  champsim::msl::lru_table<tracker_entry> table{TRACKER_SETS, TRACKER_WAYS};

  std::optional<champsim::capability> get_auth_capability() const;
  int compute_adaptive_degree(champsim::block_number cl_addr,
                              champsim::block_number::difference_type stride,
                              const champsim::capability& cap) const;

  uint64_t stride_prefetches_issued = 0;
  uint64_t stride_prefetches_bounded = 0;
  uint64_t cap_lookups = 0;
  uint64_t cap_hits = 0;
  uint64_t degree_adapted_count = 0;   
  uint64_t degree_full_count = 0;    

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
