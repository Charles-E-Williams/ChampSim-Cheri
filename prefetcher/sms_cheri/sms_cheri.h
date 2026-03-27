//=======================================================================================//
// File             : sms_cheri/sms_cheri.h
// Description      : CHERI-aware Spatial Memory Streaming prefetcher.
//                    When a valid AUTH_CAP capability is present on the demand,
//                    spatial regions are defined relative to object bounds
//                    (cap_base) rather than physical page boundaries.  This
//                    aligns the learned spatial footprint with data-structure
//                    layout, improving pattern reuse across allocations.
//                    Falls back to stock SMS (page-based) when no capability
//                    metadata is available.
//=======================================================================================//

#ifndef __SMS_CHERI_H__
#define __SMS_CHERI_H__

#include <deque>
#include <optional>
#include <vector>

#include "champsim.h"
#include "cheri.h"
#include "modules.h"
#include "sms_cheri_helper.h"
#include "cheri_prefetch_utils.h"

struct sms_cheri : public champsim::modules::prefetcher {
private:
  // ---- Configuration ----
  constexpr static uint32_t AT_SIZE = 32;
  constexpr static uint32_t FT_SIZE = 64;
  constexpr static uint32_t PHT_SIZE = 2048;
  constexpr static uint32_t PHT_ASSOC = 16;
  constexpr static uint32_t PHT_SETS = PHT_SIZE / PHT_ASSOC;
  constexpr static uint32_t PREF_DEGREE = 4;
  constexpr static uint32_t REGION_SIZE = 2048;       // 2KB spatial region
  constexpr static uint32_t REGION_SIZE_LOG = 11;
  constexpr static uint32_t PREF_BUFFER_SIZE = 256;

  // Number of cache lines per region: (1 << (REGION_SIZE_LOG - LOG2_BLOCK_SIZE)).
  // Not constexpr because LOG2_BLOCK_SIZE is extern const in ChampSim.
  static uint32_t region_cls() { return 1u << (REGION_SIZE_LOG - LOG2_BLOCK_SIZE); }

  // ---- Internal data structures ----
  std::deque<FTEntry*> filter_table;
  std::deque<ATEntry*> acc_table;
  std::vector<std::deque<PHTEntry*>> pht;
  std::deque<uint64_t> pref_buffer;

  // ---- Capability access ----
  std::optional<champsim::capability> get_auth_capability() const;

  // ---- Region / offset decomposition ----
  // Decomposes a demand into (region_id, offset, cap metadata) using
  // capability bounds when available, page boundaries otherwise.
  struct region_info {
    uint64_t region_id;
    uint32_t offset;        // CL index within the region
    uint64_t cap_base;
    uint64_t cap_top;
    uint64_t demand_pa_page;
    uint64_t demand_va_page;
    bool     has_cap;
  };
  region_info decompose(uint64_t pa, const std::optional<champsim::capability>& cap) const;

  // ---- Filter Table ----
  std::deque<FTEntry*>::iterator search_filter_table(uint64_t region_id);
  std::deque<FTEntry*>::iterator search_victim_filter_table();
  void evict_filter_table(std::deque<FTEntry*>::iterator victim);
  void insert_filter_table(uint64_t pc, const region_info& ri);

  // ---- Accumulation Table ----
  std::deque<ATEntry*>::iterator search_acc_table(uint64_t region_id);
  std::deque<ATEntry*>::iterator search_victim_acc_table();
  void evict_acc_table(std::deque<ATEntry*>::iterator victim);
  void update_age_acc_table(std::deque<ATEntry*>::iterator current);
  void insert_acc_table(FTEntry* ftentry, uint32_t offset);

  // ---- Pattern History Table ----
  std::deque<PHTEntry*>::iterator search_pht(uint64_t signature, uint32_t& set);
  std::deque<PHTEntry*>::iterator search_victim_pht(int32_t set);
  void evict_pht(int32_t set, std::deque<PHTEntry*>::iterator victim);
  void update_age_pht(int32_t set, std::deque<PHTEntry*>::iterator current);
  void insert_pht_table(ATEntry* atentry);

  // ---- Signature / prefetch generation ----
  uint64_t create_signature(uint64_t pc, uint32_t offset);

  // Generate prefetch addresses.  For capability-backed regions the
  // addresses are bounds-checked and same-page constrained.
  std::size_t generate_prefetch(uint64_t pc, uint64_t pa,
                                const region_info& ri,
                                std::vector<uint64_t>& pref_addr);

  void buffer_prefetch(std::vector<uint64_t> pref_addr);
  void issue_prefetch();

  // ---- Statistics ----
  uint64_t stat_total_accesses = 0;
  uint64_t stat_cap_accesses = 0;     // demands with a valid capability
  uint64_t stat_nocap_accesses = 0;   // fallback to page-based
  uint64_t stat_pref_generated = 0;
  uint64_t stat_pref_cap = 0;        // prefetches from cap-backed patterns
  uint64_t stat_pref_nocap = 0;      // prefetches from page-based patterns
  uint64_t stat_pref_bounds_clip = 0; // prefetches suppressed by cap bounds
  uint64_t stat_pref_page_clip = 0;  // prefetches suppressed by same-page

public:
  using champsim::modules::prefetcher::prefetcher;

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

#endif /* __SMS_CHERI_H__ */