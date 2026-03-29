#ifndef SPP_CHERI_H
#define SPP_CHERI_H

#include <cstdint>
#include <optional>
#include <vector>

#include "cache.h"
#include "modules.h"
#include "cheri_prefetch_utils.h"
#include "msl/lru_table.h"

struct spp_cheri : public champsim::modules::prefetcher {

  
  // SPP functional knobs
  constexpr static bool LOOKAHEAD_ON = true;
  constexpr static bool FILTER_ON = true;
  constexpr static bool GHR_ON = true;
  constexpr static bool SPP_SANITY_CHECK = true;
  constexpr static bool SPP_DEBUG_PRINT = false;

  // Signature table parameters
  constexpr static std::size_t ST_SET = 1;
  constexpr static std::size_t ST_WAY = 256;
  constexpr static unsigned ST_TAG_BIT = 16;
  constexpr static unsigned SIG_SHIFT = 3;
  constexpr static unsigned SIG_BIT = 12;
  constexpr static uint32_t SIG_MASK = ((1 << SIG_BIT) - 1);
  constexpr static unsigned SIG_DELTA_BIT = 7;

  // Pattern table parameters
  constexpr static std::size_t PT_SET = 512;
  constexpr static std::size_t PT_WAY = 4;
  constexpr static unsigned C_SIG_BIT = 4;
  constexpr static unsigned C_DELTA_BIT = 4;
  constexpr static uint32_t C_SIG_MAX = ((1 << C_SIG_BIT) - 1);
  constexpr static uint32_t C_DELTA_MAX = ((1 << C_DELTA_BIT) - 1);

  // Prefetch filter parameters
  constexpr static unsigned QUOTIENT_BIT = 10;
  constexpr static unsigned REMAINDER_BIT = 6;
  constexpr static unsigned HASH_BIT = (QUOTIENT_BIT + REMAINDER_BIT + 1);
  constexpr static std::size_t FILTER_SET = (1 << QUOTIENT_BIT);
  constexpr static uint32_t FILL_THRESHOLD = 90;
  constexpr static uint32_t PF_THRESHOLD = 25;

  // Global register parameters
  constexpr static unsigned GLOBAL_COUNTER_BIT = 10;
  constexpr static uint32_t GLOBAL_COUNTER_MAX = ((1 << GLOBAL_COUNTER_BIT) - 1);
  constexpr static std::size_t MAX_GHR_ENTRY = 8;

  // Page offset extraction (same as vanilla SPP)
  struct block_in_page_extent : champsim::dynamic_extent {
    block_in_page_extent() : dynamic_extent(champsim::data::bits{LOG2_PAGE_SIZE}, champsim::data::bits{LOG2_BLOCK_SIZE}) {}
  };
  using offset_type = champsim::address_slice<block_in_page_extent>;

  enum FILTER_REQUEST { SPP_L2C_PREFETCH, SPP_LLC_PREFETCH, L2C_DEMAND, L2C_EVICT };

  static uint64_t get_hash(uint64_t key);

  class SIGNATURE_TABLE
  {
    struct tag_extent : champsim::dynamic_extent {
      tag_extent() : dynamic_extent(champsim::data::bits{ST_TAG_BIT + LOG2_PAGE_SIZE}, champsim::data::bits{LOG2_PAGE_SIZE}) {}
    };

  public:
    spp_cheri* _parent;
    using tag_type = champsim::address_slice<tag_extent>;

    bool valid[ST_SET][ST_WAY];
    offset_type last_offset[ST_SET][ST_WAY];   // page-relative last offset
    uint32_t sig[ST_SET][ST_WAY];
    uint32_t lru[ST_SET][ST_WAY];
    uint64_t cap_base[ST_SET][ST_WAY];         // capability base for this entry
    uint64_t cap_length[ST_SET][ST_WAY];
    int64_t last_cap_cl_offset[ST_SET][ST_WAY]; // cap-relative last offset (cache-line units)

  SIGNATURE_TABLE()
    {
      for (uint32_t set = 0; set < ST_SET; set++)
        for (uint32_t way = 0; way < ST_WAY; way++) {
          valid[set][way] = 0;
          last_offset[set][way] = offset_type{};
          sig[set][way] = 0;
          lru[set][way] = way;
          cap_base[set][way] = 0;
          cap_length[set][way] = 0;
          last_cap_cl_offset[set][way] = 0;
        }
    };

    // Dual-mode signature read/update
    void read_and_update_sig(champsim::address addr, uint32_t& last_sig, uint32_t& curr_sig,
                             int64_t& delta, uint64_t cap_base_val, uint64_t cap_offset_val,
                             uint64_t cap_length_val);
  };

  class PATTERN_TABLE
  {
  public:
    spp_cheri* _parent;
    int64_t delta[PT_SET][PT_WAY];
    uint32_t c_delta[PT_SET][PT_WAY], c_sig[PT_SET];

    PATTERN_TABLE()
    {
      for (uint32_t set = 0; set < PT_SET; set++) {
        for (uint32_t way = 0; way < PT_WAY; way++) {
          delta[set][way] = 0;
          c_delta[set][way] = 0;
        }
        c_sig[set] = 0;
      }
    }

    void update_pattern(uint32_t last_sig, int64_t curr_delta);
    void read_pattern(uint32_t curr_sig, std::vector<int64_t>& prefetch_delta,
                      std::vector<uint32_t>& confidence_q, uint32_t& lookahead_way,
                      uint32_t& lookahead_conf, uint32_t& pf_q_tail, uint32_t& depth);
  };

  class PREFETCH_FILTER
  {
  public:
    spp_cheri* _parent;
    uint64_t remainder_tag[FILTER_SET];
    bool valid[FILTER_SET];
    bool useful[FILTER_SET];

    PREFETCH_FILTER()
    {
      for (uint32_t set = 0; set < FILTER_SET; set++) {
        remainder_tag[set] = 0;
        valid[set] = 0;
        useful[set] = 0;
      }
    }

    bool check(champsim::address pf_addr, FILTER_REQUEST filter_request);
  };


  class GLOBAL_REGISTER
  {
  public:
    spp_cheri* _parent;
    uint32_t pf_useful, pf_issued;
    uint32_t global_accuracy;

    uint8_t valid[MAX_GHR_ENTRY];
    uint32_t sig[MAX_GHR_ENTRY], confidence[MAX_GHR_ENTRY];
    offset_type offset[MAX_GHR_ENTRY];
    int64_t delta[MAX_GHR_ENTRY];

    GLOBAL_REGISTER()
    {
      pf_useful = 0;
      pf_issued = 0;
      global_accuracy = 0;
      for (uint32_t i = 0; i < MAX_GHR_ENTRY; i++) {
        valid[i] = 0;
        sig[i] = 0;
        confidence[i] = 0;
        offset[i] = offset_type{};
        delta[i] = 0;
      }
    }

    void update_entry(uint32_t pf_sig, uint32_t pf_confidence, offset_type pf_offset, int64_t pf_delta);
    uint32_t check_entry(offset_type page_offset);
  };

  SIGNATURE_TABLE ST;
  PATTERN_TABLE PT;
  PREFETCH_FILTER FILTER;
  GLOBAL_REGISTER GHR;

  // CHERI statistics
  uint64_t stat_pf_bounded_by_cap = 0;  // prefetches clipped by capability bounds
  uint64_t stat_cross_page_in_cap = 0;  // cross-page deltas within same capability

  using prefetcher::prefetcher;
  void prefetcher_initialize();
  void prefetcher_cycle_operate();
  void prefetcher_final_stats();
  uint32_t prefetcher_cache_operate(champsim::address addr, champsim::address ip, uint8_t cache_hit,
                                    bool useful_prefetch, access_type type, uint32_t metadata_in);
  uint32_t prefetcher_cache_fill(champsim::address addr, long set, long way, uint8_t prefetch,
                                 champsim::address evicted_addr, uint32_t metadata_in);
};

#endif