//=======================================================================================//
// File             : kratos/kratos.h
// Description      : KRATOS v5 – Capability-Resident Adaptive Type-Object Streamer
//                    L2 CHERI-aware prefetcher for PIPT cache.
//                    Combines SMS-style spatial footprints, CHERI bounds enforcement,
//                    capability-size routing, pointer chasing, and a private
//                    VA-to-PA translation cache.
//
// NOTE: Requires adding `champsim::address current_v_address{};` as a public
//       member of CACHE (next to auth_capability) and storing
//       `current_v_address = handle_pkt.v_address;` in cache.cc before calling
//       impl_prefetcher_cache_operate, so the prefetcher can read the virtual
//       address of the current access.
//=======================================================================================//

#ifndef PREFETCHER_KRATOS_H
#define PREFETCHER_KRATOS_H

#include <array>
#include <cstdint>
#include <deque>
#include <optional>
#include <vector>

#include "champsim.h"
#include "modules.h"
#include "cheri_prefetch_utils.h"
#include "msl/lru_table.h"
#include "capability_memory.h"
#include "kratos_helper.h"

class kratos : public champsim::modules::prefetcher {

  // =========================================================================
  // Configuration constants
  // =========================================================================

  // --- Translation Cache ---
  static constexpr uint32_t TLB_SETS         = 32;
  static constexpr uint32_t TLB_WAYS         = 4;
  static constexpr uint32_t TLB_ENTRIES      = TLB_SETS * TLB_WAYS;  // 128

  // --- Filter Table (first access to a region) ---
  static constexpr uint32_t FT_SIZE          = 64;

  // --- Accumulation Table (recording spatial pattern) ---
  static constexpr uint32_t AT_SIZE          = 32;

  // --- Pattern History Table (learned footprints) ---
  static constexpr uint32_t PHT_SIZE         = 2048;
  static constexpr uint32_t PHT_ASSOC        = 16;
  static constexpr uint32_t PHT_SETS         = PHT_SIZE / PHT_ASSOC;   // 128

  // --- Stride Table (set-associative via lru_table) ---
  static constexpr std::size_t STRIDE_SETS   = 64;
  static constexpr std::size_t STRIDE_WAYS   = 4;
  static constexpr int         STRIDE_DEGREE = 8;

  // --- Pointer Buffer ---
  static constexpr uint32_t PTR_BUF_SIZE     = 32;
  static constexpr uint32_t CAP_SLOTS_PER_CL = 4;   // 64B line / 16B cap = 4

  // --- Prefetch Buffer (rate-limited issuance) ---
  static constexpr uint32_t PREF_BUF_SIZE    = 256;
  static constexpr uint32_t PREF_DEGREE      = 4;    // max prefetches per cycle

  // --- Capability size routing thresholds ---
  static constexpr uint64_t CAP_SMALL_MAX    = 4096;       // <= 4 KB  -> Atlas (SMS)
  static constexpr uint64_t CAP_LARGE_MIN    = 262144;     // > 256 KB -> Stride engine

  // --- Confidence thresholds ---
  static constexpr uint8_t  CONF_PREDICT     = 4;   // min confidence for IP-based PHT prediction
  static constexpr uint8_t  CONF_TYPE        = 5;   // min confidence for type-based prediction
  static constexpr uint8_t  CONF_MAX         = 7;

  // --- MSHR throttling ---
  static constexpr double   MSHR_SUPPRESS_ALL   = 0.75;
  static constexpr double   MSHR_SUPPRESS_RECUR = 0.50;

  // =========================================================================
  // Internal data structures
  // =========================================================================

  // Translation cache: VA page -> PA page (set-associative, manually managed)
  std::array<kratos_helper::TlbEntry, TLB_ENTRIES> tlb_cache{};

  // SMS-style pipeline
  std::deque<kratos_helper::FilterEntry*>  filter_table;
  std::deque<kratos_helper::AccEntry*>     acc_table;
  std::vector<std::deque<kratos_helper::PHTEntry*>> pht;  // set-indexed

  // Stride table: set-associative via lru_table, keyed by IP
  champsim::msl::lru_table<kratos_helper::StrideEntry> stride_table{STRIDE_SETS, STRIDE_WAYS};

  // Active stride lookahead (issued progressively in cycle_operate)
  struct StrideLookahead {
    champsim::address address{};
    champsim::block_number::difference_type stride{};
    int degree = 0;
    std::optional<champsim::capability> cap{};
    uint64_t last_va = 0;     // for VA-based bounds checking
  };
  std::optional<StrideLookahead> active_stride_lookahead;

  // Pointer buffer for selective pointer chasing
  std::deque<kratos_helper::PtrBufEntry*> ptr_buffer;

  // Rate-limited prefetch issuance buffer
  std::deque<uint64_t> pref_buffer;   // physical addresses

  // =========================================================================
  // Internal helpers — translation
  // =========================================================================

  // Read the current access's virtual address from the cache object.
  // Requires the CACHE modification described at the top of this file.
  uint64_t get_current_v_address() const;

  // Read the authorizing capability (same pattern as ip_stride_cheri).
  std::optional<champsim::capability> get_auth_capability() const;

  // Populate translation cache from current demand access.
  void tlb_update(uint64_t v_page, uint64_t p_page);

  // Translate a virtual address to physical.  Returns nullopt on miss.
  std::optional<uint64_t> tlb_lookup(uint64_t va);

  // =========================================================================
  // Internal helpers — Aegis (bounds enforcement)
  // =========================================================================

  // Returns true if prefetch VA is within capability [base, base+length).
  static bool aegis_check(uint64_t pf_va, const champsim::capability& cap);

  // Compute a 64-bit bounds mask for a 4 KB region: bit i is set when
  // cacheline i lies within [cap_base, cap_base + cap_length).
  static Bitmap compute_bounds_mask(uint64_t region_base_va,
                                    const champsim::capability& cap);

  // =========================================================================
  // Internal helpers — Filter Table
  // =========================================================================

  std::deque<kratos_helper::FilterEntry*>::iterator ft_lookup(uint64_t p_page);
  void ft_insert(uint64_t p_page, uint64_t v_page, uint64_t ip,
                 uint8_t trigger_offset, const champsim::capability& cap);
  void ft_evict(std::deque<kratos_helper::FilterEntry*>::iterator victim);

  // =========================================================================
  // Internal helpers — Accumulation Table
  // =========================================================================

  std::deque<kratos_helper::AccEntry*>::iterator at_lookup(uint64_t p_page);
  void at_insert(kratos_helper::FilterEntry* ft_entry, uint8_t second_offset);
  void at_evict(std::deque<kratos_helper::AccEntry*>::iterator victim);
  void at_update_lru(std::deque<kratos_helper::AccEntry*>::iterator current);

  // =========================================================================
  // Internal helpers — Pattern History Table
  // =========================================================================

  // Primary signature: hash(IP, trigger_offset)
  static uint64_t pht_signature(uint64_t ip, uint8_t trigger_offset);

  // Type signature: hash(cap_length, trigger_offset)
  static uint64_t pht_type_signature(uint64_t cap_length, uint8_t trigger_offset);

  std::deque<kratos_helper::PHTEntry*>::iterator pht_lookup(uint64_t signature,
                                                            uint32_t& set);
  std::deque<kratos_helper::PHTEntry*>::iterator pht_lookup_type(uint64_t type_sig,
                                                                 uint32_t& set);
  void pht_insert(kratos_helper::AccEntry* at_entry);
  void pht_update_lru(uint32_t set,
                      std::deque<kratos_helper::PHTEntry*>::iterator current);

  // Issue prefetches from a PHT pattern, applying bounds mask,
  // translation, and buffering.  Returns number of prefetches buffered.
  uint32_t pht_replay(const Bitmap& pattern,
                      const Bitmap& bounds_mask,
                      uint64_t region_base_va, uint64_t p_page,
                      uint8_t trigger_offset);

  // =========================================================================
  // Internal helpers — Stride engine
  // =========================================================================

  // Called from prefetcher_cache_operate for large-cap and no-cap accesses.
  // Updates the stride table and initiates lookahead if a stride is confirmed.
  void stride_operate(champsim::address addr_pa, uint64_t addr_va,
                      champsim::address ip,
                      const std::optional<champsim::capability>& cap);

  // =========================================================================
  // Internal helpers — Pointer chasing
  // =========================================================================

  // Scan cap_mem for pointers in the cacheline at the given VA.
  // Only called on demand misses to small objects with LOAD_CAP permission.
  void ptr_discover(uint64_t cl_va, uint64_t ip,
                    uint64_t p_page, uint8_t cl_offset,
                    const champsim::capability& cap);

  // Check whether a demand access matches a pending pointer target.
  // Matches on physical address at cacheline granularity for speed.
  void ptr_check_confirmation(uint64_t demand_pa, uint64_t demand_va,
                              champsim::address ip);

  // =========================================================================
  // Internal helpers — Prefetch buffer
  // =========================================================================

  void buffer_prefetch(uint64_t pa);
  void buffer_prefetch(const std::vector<uint64_t>& addrs);
  void issue_prefetches();

  // =========================================================================
  // Statistics
  // =========================================================================

  // Translation cache
  uint64_t stat_tlb_lookups        = 0;
  uint64_t stat_tlb_hits           = 0;

  // Capability routing
  uint64_t stat_route_atlas        = 0;   // small/medium cap -> SMS pipeline
  uint64_t stat_route_stride       = 0;   // large cap -> stride engine
  uint64_t stat_route_nocap        = 0;   // no valid capability -> stride fallback

  // SMS pipeline
  uint64_t stat_ft_inserts         = 0;
  uint64_t stat_at_promotions      = 0;
  uint64_t stat_pht_hits_primary   = 0;
  uint64_t stat_pht_hits_type      = 0;
  uint64_t stat_pht_misses         = 0;

  // Prefetching
  uint64_t stat_pf_issued          = 0;
  uint64_t stat_pf_bounded         = 0;   // dropped by Aegis
  uint64_t stat_pf_no_xlat         = 0;   // dropped by translation miss
  uint64_t stat_pf_buffered        = 0;
  uint64_t stat_pf_useful          = 0;

  // Pointer chasing
  uint64_t stat_ptr_discovered     = 0;
  uint64_t stat_ptr_confirmed      = 0;
  uint64_t stat_ptr_pf_issued      = 0;

  // Stride engine
  uint64_t stat_stride_issued      = 0;
  uint64_t stat_stride_bounded     = 0;

  // =========================================================================
  // ChampSim interface
  // =========================================================================

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

#endif /* PREFETCHER_KRATOS_H */