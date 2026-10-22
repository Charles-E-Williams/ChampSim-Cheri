#ifndef PREFETCHER_AMPM_SPAN_H
#define PREFETCHER_AMPM_SPAN_H

#include <array>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include "champsim.h"
#include "cheri.h"
#include "cheri_prefetch_utils.h"
#include "modules.h"
#include "msl/lru_table.h"
#include "../ampm/ampm.h"

// AMPM-CHERI whose small-object (page) path may continue a pattern past the 4 KB zone edge, but only where the
// capability says the data structure continues:
//  * the candidate line is inside the trigger's object (bounds), or
//  * the trigger object continues an allocation run in the candidate's direction (its base sits right after the
//    previous object's top, with the same permissions).
// The remaining out-of-object candidates on the page path pass a learned gate keyed by (permission kind, size class).
// Untagged accesses are ignored, as in AMPM-CHERI.
class ampm_span : public champsim::modules::prefetcher
{
public:
  // Ablation switches
  static constexpr bool SPAN_ENABLED = true;  // cross zone edges inside the object or along an allocation run
  static constexpr bool GATE_ENABLED = true;  // learned gate for the remaining out-of-object candidates
  static constexpr bool CROSS_ALL = false;    // control without capabilities: every page-path candidate may cross

  static constexpr int PREFETCH_DEGREE = 2;
  static constexpr unsigned AMPM_REGION_BITS = 12; // 4KB zone granularity
  static constexpr std::size_t REGION_SETS = 64;
  static constexpr std::size_t REGION_WAYS = 4;

  static constexpr uint64_t SMALL_CAP_THRESHOLD = 64; // one zone worth of cachelines (4KB)

  // Allocation runs
  static constexpr std::size_t RUN_ENTRIES = 8; // recently touched small objects
  static constexpr uint64_t RUN_SLACK = 64;     // max bytes between one object's top and the next one's base
  static constexpr int RUN_MIN = 2;             // adjacent steps in one direction before a run is trusted

  static std::size_t lines_per_zone() { return (1u << AMPM_REGION_BITS) / BLOCK_SIZE; }

  // ---- object zones (AMPM-CHERI) ----
  struct key_extent : champsim::dynamic_extent {
    key_extent() : dynamic_extent(champsim::data::bits{64}, champsim::data::bits{0}) {}
  };
  using region_key_type = champsim::address_slice<key_extent>;

  struct region_type {
    region_key_type key;
    std::vector<bool> access_map;
    std::vector<bool> prefetch_map;
    uint64_t cap_base = 0; // collision detection

    region_type() : region_type(region_key_type{}) {}
    explicit region_type(region_key_type k)
        : key(k), access_map((1u << AMPM_REGION_BITS) / BLOCK_SIZE, false), prefetch_map((1u << AMPM_REGION_BITS) / BLOCK_SIZE, false)
    {
    }
  };

  struct region_indexer {
    auto operator()(const region_type& e) const { return e.key; }
  };

  champsim::msl::lru_table<region_type, region_indexer, region_indexer> regions{REGION_SETS, REGION_WAYS};

  static uint64_t make_zone_key(uint64_t cap_base, uint64_t cap_zone_id);
  auto zone_key_and_offset(champsim::address va, const champsim::capability& cap) const -> std::pair<region_key_type, std::size_t>;
  void add_to_map(champsim::address va, const champsim::capability& cap, bool prefetch);
  bool check_map(champsim::address va, const champsim::capability& cap, bool prefetch);
  void do_prefetch(CACHE* cache, champsim::address pa, champsim::address va, const champsim::capability& cap, uint32_t metadata_in, int degree,
                   bool two_level);

  // ---- page zones (baseline AMPM maps) ----
  ampm::AMPM_Module page_engine;
  // Page path with zone-edge crossing; va_line is the trigger's virtual line (for object bounds), run_dir from run_update()
  void page_do_prefetch(CACHE* cache, champsim::address addr, std::optional<champsim::address> va_line, const champsim::capability& cap, int run_dir,
                        uint32_t metadata_in, bool two_level);

  // ---- allocation runs ----
  struct run_entry {
    uint64_t base = 0, top = 0;
    uint32_t perms = 0;
    int dir = 0, count = 0;
    uint64_t lru = 0;
    bool valid = false;
  };
  std::array<run_entry, RUN_ENTRIES> runs{};
  uint64_t run_clock = 0;
  int run_update(const champsim::capability& cap); // run direction (+1/-1) the trigger object continues, or 0

  // ---- out-of-object gate ----
  // Same policy as cheri_policy::OobConfidence (5-bit counters, +2 when a tracked line is demanded, -1 when it leaves the
  // tracker unused, dropped candidates tracked as shadows), keyed by (permission kind, size class) instead of the IP.
  enum class gate_action { ISSUE_HERE, ISSUE_LOWER, DROP };
  static constexpr unsigned NUM_KINDS = 3;   // RW, RO (no store), X (execute)
  static constexpr unsigned NUM_CLASSES = 4; // <=128B, <=4KB, <=64KB, larger
  static constexpr unsigned GATE_KEYS = NUM_KINDS * NUM_CLASSES;
  static constexpr std::size_t GATE_SETS = 64;
  static constexpr std::size_t GATE_WAYS = 4;
  static constexpr unsigned GATE_TAG_BITS = 16;
  static constexpr int GATE_CTR_MAX = 31, GATE_CTR_INIT = 16, GATE_HIGH = 20, GATE_LOW = 12, GATE_INC = 2, GATE_DEC = 1;

  struct gate_entry {
    uint16_t tag = 0;
    uint8_t key = 0;
    bool shadow = false;
    bool valid = false;
    uint64_t lru = 0;
  };
  struct gate_key_stats {
    uint64_t issued = 0, dropped = 0, used_issued = 0, used_shadow = 0;
  };
  std::array<std::array<gate_entry, GATE_WAYS>, GATE_SETS> gate_tracker{};
  std::array<int, GATE_KEYS> gate_ctr = [] {
    std::array<int, GATE_KEYS> a{};
    a.fill(GATE_CTR_INIT);
    return a;
  }();
  std::array<gate_key_stats, GATE_KEYS> gate_stats{};
  uint64_t gate_clock = 0;

  static unsigned gate_key(const champsim::capability& cap);
  gate_action gate_decide(unsigned key) const;
  void gate_record(uint64_t line, unsigned key, gate_action act);
  void gate_on_demand(uint64_t line);
  void gate_update(unsigned key, int d);

  // ---- statistics ----
  uint64_t pf_bounded = 0;
  uint64_t zone_collision = 0;
  uint64_t page_access = 0;               // accesses routed to page path
  uint64_t cursor_check_failed = 0;       // large-cap accesses with no usable VA, routed to the page path
  uint64_t evict_cursor_check_failed = 0; // evictions whose cap cursor is not in the evicted line; cleanup skipped
  uint64_t run_accesses = 0;              // page-path accesses continuing an allocation run
  uint64_t span_in_object = 0, span_along_run = 0, span_gated = 0, span_unchecked = 0, span_blocked = 0; // zone-edge crossings

  using prefetcher::prefetcher;

  uint32_t prefetcher_cache_operate(champsim::address addr, champsim::address ip, uint32_t cpu, champsim::capability cap, bool cache_hit,
                                    bool useful_prefetch, access_type type, uint32_t metadata_in, uint32_t metadata_hit);
  uint32_t prefetcher_cache_fill(champsim::address addr, champsim::address ip, uint32_t cpu, champsim::capability cap, bool useless, long set, long way,
                                 bool prefetch, champsim::address evicted_addr, champsim::capability evicted_cap, uint32_t metadata_in,
                                 uint32_t metadata_evict, uint32_t cpu_evict);
  void prefetcher_final_stats();
};

#endif
