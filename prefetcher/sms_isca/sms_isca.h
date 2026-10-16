//=======================================================================================//
// File             : sms_isca/sms_isca.h
// Description      : Spatial Memory Streaming (Somogyi et al., ISCA'06; JILP'11) at the
//                    published practical configuration (JILP'11 Sec. 5, Fig. 16): 2 KB
//                    spatial regions, 32-entry filter table, 64-entry accumulation table,
//                    16K-entry 16-way PHT indexed by PC + trigger offset, generations ended
//                    by eviction of any block of the region (or AT replacement), predictions
//                    streamed from prediction registers round-robin. Baseline for sms_cap.
//
//                    Placement: L2C (physical), as in the fork's existing SMS configs.
//=======================================================================================//

#ifndef __SMS_ISCA_H__
#define __SMS_ISCA_H__

#include <bitset>
#include <cstdint>
#include <vector>

#include "champsim.h"
#include "cheri.h"
#include "modules.h"

struct sms_isca : public champsim::modules::prefetcher {
  friend struct sms_test_access; // unit tests (test/cpp/src/461-*)

  constexpr static uint32_t REGION_LINES = 32; // 2 KB region
  typedef std::bitset<REGION_LINES> Pattern;

  // What a trigger access knows; kept in the FT and AT until the generation ends
  class Trigger
  {
  public:
    uint64_t pc;
    uint32_t offset; // trigger line within the region

  public:
    void reset()
    {
      pc = 0;
      offset = 0;
    }
    Trigger() { reset(); }
  };

  class FTEntry
  {
  public:
    bool valid;
    uint64_t region;
    Trigger trig;

  public:
    void reset()
    {
      valid = false;
      region = 0;
      trig.reset();
    }
    FTEntry() { reset(); }
  };

  class ATEntry
  {
  public:
    bool valid;
    uint64_t region;
    Trigger trig;
    Pattern pattern;
    uint32_t lru;

  public:
    void reset()
    {
      valid = false;
      region = 0;
      trig.reset();
      pattern.reset();
      lru = 0;
    }
    ATEntry() { reset(); }
  };

  class PHTEntry
  {
  public:
    bool valid;
    uint16_t tag;
    Pattern pattern;
    uint32_t lru;

  public:
    void reset()
    {
      valid = false;
      tag = 0;
      pattern.reset();
      lru = 0;
    }
    PHTEntry() { reset(); }
  };

  // One prediction register: the lines of one predicted pattern still to be streamed
  class PredRegister
  {
  public:
    bool valid;
    int64_t base_line; // physical line of pattern bit 0
    Pattern pending;
    uint64_t seq; // allocation order

    // simulation only (prefetch statistics by object size): the trigger's capability and lines
    champsim::capability cap;
    uint64_t trigger_line;
    uint64_t cursor_line_va;

  public:
    void reset()
    {
      valid = false;
      base_line = 0;
      pending.reset();
      seq = 0;
      cap = champsim::capability{};
      trigger_line = 0;
      cursor_line_va = 0;
    }
    PredRegister() { reset(); }
  };

private:
  // config
  constexpr static uint32_t LOG2_REGION_LINES = 5;
  constexpr static uint32_t FT_SIZE = 32;
  constexpr static uint32_t AT_SIZE = 64;
  constexpr static uint32_t PHT_SETS = 1024;
  constexpr static uint32_t PHT_ASSOC = 16; // 16K entries
  constexpr static uint32_t PHT_INDEX_BITS = 10;
  constexpr static uint32_t PHT_TAG_BITS = 16;
  constexpr static uint32_t PRED_REGISTERS = 16;
  constexpr static uint32_t PREF_DEGREE = 4; // prefetches issued per cycle
  constexpr static uint32_t PC_ALIGN_BITS = 1;

  // internal data structures
  FTEntry filter_table[FT_SIZE];
  uint32_t ft_next = 0; // FIFO replacement
  ATEntry acc_table[AT_SIZE];
  std::vector<std::vector<PHTEntry>> pht;
  PredRegister pred_regs[PRED_REGISTERS];
  uint32_t rr_next = 0;
  uint64_t reg_seq = 0;

  // statistics
  uint64_t stat_triggers = 0;
  uint64_t stat_pht_hits = 0;
  uint64_t stat_predicted = 0;
  uint64_t stat_issued = 0;
  uint64_t stat_page_dropped = 0;
  uint64_t stat_gen_evicted = 0;
  uint64_t stat_gen_replaced = 0;
  uint64_t stat_reg_replaced = 0;

  // private functions
  FTEntry* search_filter_table(uint64_t region);
  void insert_filter_table(uint64_t region, const Trigger& trig);

  ATEntry* search_acc_table(uint64_t region);
  void update_age_acc_table(ATEntry* current);
  void insert_acc_table(FTEntry* ftentry, uint32_t offset);
  void end_generation(ATEntry* atentry);

  PHTEntry* search_pht(uint64_t signature);
  void update_age_pht(uint64_t signature, PHTEntry* current);
  void insert_pht_table(const Trigger& trig, const Pattern& pattern);

  uint64_t create_signature(const Trigger& trig);
  void generate_prefetch(const Trigger& trig, uint64_t line, champsim::capability cap);
  void buffer_prefetch(PredRegister reg);
  void issue_prefetch();

public:
  using champsim::modules::prefetcher::prefetcher;

  // champsim interface prototypes
  void prefetcher_initialize();
  uint32_t prefetcher_cache_operate(champsim::address addr, champsim::address ip, uint32_t cpu, champsim::capability cap, uint8_t cache_hit,
                                    bool useful_prefetch, access_type type, uint32_t metadata_in, uint32_t metadata_hit);
  uint32_t prefetcher_cache_fill(champsim::address addr, champsim::address ip, uint32_t cpu, champsim::capability cap, bool useless, long set, long way,
                                 bool prefetch, champsim::address evicted_addr, champsim::capability evicted_cap, uint32_t metadata_in,
                                 uint32_t metadata_evict, uint32_t cpu_evict);
  void prefetcher_cycle_operate();
  void prefetcher_final_stats();
};

#endif /* __SMS_ISCA_H__ */