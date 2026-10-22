#ifndef SPP_OBJ_H
#define SPP_OBJ_H

#include <cstdint>
#include <optional>
#include <vector>

#include "cache.h"
#include "cheri_oob_filter.h"
#include "modules.h"
#include "msl/lru_table.h"

struct spp_obj : public champsim::modules::prefetcher {

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

  // CHERI options. Untagged accesses are ignored (no training, no prefetch).
  constexpr static bool OOB_FILTER = true; // learned OOB filter (cheri_oob_filter.h) decides out-of-object candidates
  constexpr static bool CARRY_SIG = true; // an ST miss in an object region continues the signature of the adjacent region of the same object
  constexpr static bool END_STOP = false;  // candidates outside an object >= OBJ_KEY_MIN_BYTES are dropped
  // Large-object promotion (the former boost): for in-object candidates of objects >= PROMOTE_MIN_BYTES, with MSHR
  // occupancy below PROMOTE_MSHR_LIMIT, the issue and L2-fill thresholds are relaxed. PROMOTE_GATE keeps it on only
  // while the L2 fills it adds are used: one filter bit marks them, two counters measure their accuracy, and 1 in 16
  // eligible accesses stays promoted when gated so the measurement continues.
  constexpr static bool PROMOTE = false;
  constexpr static bool PROMOTE_GATE = false;
  constexpr static uint64_t PROMOTE_MIN_BYTES = uint64_t{2} << 20;
  constexpr static uint32_t PF_THRESHOLD_PROMOTE = 15;
  constexpr static uint32_t FILL_THRESHOLD_PROMOTE = 75;
  constexpr static double PROMOTE_MSHR_LIMIT = 70; // percent
  constexpr static uint32_t PROMOTE_MIN_ACC = 50;   // percent of promoted L2 fills used
  constexpr static uint32_t PROMOTE_WARMUP = 32;    // promoted fills before the gate applies
  constexpr static bool OBJECT_KEYED = true;            // ST keyed by (capability base, object-relative region) instead of page
  constexpr static unsigned OBJ_LOG2_REGION = 12;        // object-relative region size for OBJECT_KEYED
  constexpr static uint64_t OBJ_KEY_MIN_BYTES = uint64_t{1} << 12; // OBJECT_KEYED applies only to objects at least this large

  // SPP "page" size; 0 = the system page size (LOG2_PAGE_SIZE is not a constant expression)
  constexpr static unsigned SPP_LOG2_PAGE = 0;
  static unsigned log2_page() { return SPP_LOG2_PAGE ? SPP_LOG2_PAGE : LOG2_PAGE_SIZE; }

  // Global register parameters
  constexpr static unsigned GLOBAL_COUNTER_BIT = 10;
  constexpr static uint32_t GLOBAL_COUNTER_MAX = ((1 << GLOBAL_COUNTER_BIT) - 1);
  constexpr static std::size_t MAX_GHR_ENTRY = 8;

  using prefetcher::prefetcher;
  uint32_t prefetcher_cache_operate(champsim::address addr, champsim::address ip, uint32_t cpu, champsim::capability cap, uint8_t cache_hit,
                                    bool useful_prefetch, access_type type, uint32_t metadata_in, uint32_t metadata_hit);
  uint32_t prefetcher_cache_fill(champsim::address addr, champsim::address ip, uint32_t cpu, champsim::capability cap, bool useless, long set, long way,
                                 bool prefetch, champsim::address evicted_addr, champsim::capability evicted_cap, uint32_t metadata_in,
                                 uint32_t metadata_evict, uint32_t cpu_evict);

  void prefetcher_initialize();
  void prefetcher_cycle_operate();
  void prefetcher_final_stats();

  enum FILTER_REQUEST { SPP_L2C_PREFETCH, SPP_LLC_PREFETCH, L2C_DEMAND, L2C_EVICT }; // Request type for prefetch filter
  static uint64_t get_hash(uint64_t key);
  // OBJECT_KEYED: synthetic ST address for object line obj_line of cap: the tag bits hold a hash of (base, region),
  // the in-page offset bits hold the line within the object-relative region.
  static champsim::address object_key_address(const champsim::capability& cap, int64_t obj_line);

  struct page_extent : champsim::dynamic_extent {
    page_extent() : dynamic_extent(champsim::data::bits{64}, champsim::data::bits{log2_page()}) {}
  };
  using page_type = champsim::address_slice<page_extent>;

  struct block_in_page_extent : champsim::dynamic_extent {
    block_in_page_extent() : dynamic_extent(champsim::data::bits{log2_page()}, champsim::data::bits{LOG2_BLOCK_SIZE}) {}
  };
  using offset_type = champsim::address_slice<block_in_page_extent>;

  class SIGNATURE_TABLE
  {
    struct tag_extent : champsim::dynamic_extent {
      tag_extent() : dynamic_extent(champsim::data::bits{ST_TAG_BIT + log2_page()}, champsim::data::bits{log2_page()}) {}
    };

  public:
    spp_obj* _parent;
    using tag_type = champsim::address_slice<tag_extent>;

    bool valid[ST_SET][ST_WAY];
    tag_type tag[ST_SET][ST_WAY];
    offset_type last_offset[ST_SET][ST_WAY];
    uint32_t sig[ST_SET][ST_WAY], lru[ST_SET][ST_WAY];
    bool obj_key[ST_SET][ST_WAY]; // key kind: object key (OBJECT_KEYED) or page key; entries of different kinds never match
    uint64_t carried = 0;         // stats: signatures continued from an adjacent object region
    uint64_t hits[2] = {0, 0};    // stats, by key kind (0 page, 1 object): ST hits
    uint64_t misses[2] = {0, 0};  // stats, by key kind: ST misses
    uint64_t ghr_boots[2] = {0, 0}; // stats, by key kind: ST misses bootstrapped from the GHR

    SIGNATURE_TABLE()
    {
      for (uint32_t set = 0; set < ST_SET; set++)
        for (uint32_t way = 0; way < ST_WAY; way++) {
          valid[set][way] = 0;
          tag[set][way] = tag_type{};
          last_offset[set][way] = offset_type{};
          sig[set][way] = 0;
          lru[set][way] = way;
          obj_key[set][way] = false;
        }
    };

    // CARRY_SIG: the keys and first object lines of the adjacent object regions (n_neighbors of them), and the
    // access's object line; on an ST miss, the nearest neighbour's signature is continued across the region edge.
    void read_and_update_sig(champsim::address addr, bool obj_key_in, uint32_t& last_sig, uint32_t& curr_sig,
                             typename offset_type::difference_type& delta, const champsim::address* neighbor_key = nullptr,
                             const int64_t* neighbor_first_line = nullptr, int n_neighbors = 0, int64_t obj_line = 0);
  };

  class PATTERN_TABLE
  {
  public:
    spp_obj* _parent;
    typename offset_type::difference_type delta[PT_SET][PT_WAY];
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

    void update_pattern(uint32_t last_sig, typename offset_type::difference_type curr_delta);
    void read_pattern(uint32_t curr_sig, std::vector<typename offset_type::difference_type>& prefetch_delta, std::vector<uint32_t>& confidence_q,
                      uint32_t& lookahead_way, uint32_t& lookahead_conf, uint32_t& pf_q_tail, uint32_t& depth, uint32_t pf_threshold);
  };

  class PREFETCH_FILTER
  {
  public:
    spp_obj* _parent;
    uint64_t remainder_tag[FILTER_SET];
    bool valid[FILTER_SET], // Consider this as "prefetched"
        useful[FILTER_SET], // Consider this as "used"
        promoted[FILTER_SET]; // PROMOTE_GATE: an L2 fill issued only because of the relaxed thresholds

    PREFETCH_FILTER()
    {
      for (uint32_t set = 0; set < FILTER_SET; set++) {
        remainder_tag[set] = 0;
        valid[set] = 0;
        useful[set] = 0;
        promoted[set] = 0;
      }
    }

    bool check(champsim::address pf_addr, FILTER_REQUEST filter_request, bool promoted_pf = false);
  };

  class GLOBAL_REGISTER
  {
  public:
    spp_obj* _parent;
    // Global counters to calculate global prefetching accuracy
    uint32_t pf_useful, pf_issued;
    uint32_t global_accuracy; // Alpha value in Section III. Equation 3

    // Global History Register (GHR) entries
    uint8_t valid[MAX_GHR_ENTRY];
    uint32_t sig[MAX_GHR_ENTRY], confidence[MAX_GHR_ENTRY];
    offset_type offset[MAX_GHR_ENTRY];
    typename offset_type::difference_type delta[MAX_GHR_ENTRY];
    bool obj_key[MAX_GHR_ENTRY]; // key kind of the offset: object-region offset or physical page offset

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
        obj_key[i] = false;
      }
    }

    void update_entry(uint32_t pf_sig, uint32_t pf_confidence, offset_type pf_offset, typename offset_type::difference_type pf_delta, bool kind);
    uint32_t check_entry(offset_type page_offset, bool kind);
  };

  cheri_policy::OobConfidence oob;
  uint64_t tagged_accesses = 0;    // tagged accesses with a usable VA; tagged total = this + cursor_check_failed
  uint64_t obj_keyed_accesses = 0; // accesses whose ST lookup used an object key
  uint32_t prom_issued = 0, prom_useful = 0; // PROMOTE_GATE accuracy counters (10-bit, halved together)
  uint64_t promote_eligible = 0;  // stats: accesses eligible for promotion
  uint64_t promote_gated = 0;     // stats: eligible accesses not promoted because of the gate
  uint64_t promoted_fills = 0;    // stats: L2 fills issued only because of promotion
  uint64_t carried_sigs = 0;       // ST misses whose signature was continued from an adjacent object region (CARRY_SIG)
  uint64_t end_stopped = 0;        // candidates dropped outside an object >= OBJ_KEY_MIN_BYTES (END_STOP)
  uint64_t cursor_check_failed = 0; // tagged accesses with no usable VA (treated as untagged)

  SIGNATURE_TABLE ST;
  PATTERN_TABLE PT;
  PREFETCH_FILTER FILTER;
  GLOBAL_REGISTER GHR;
};

#endif
