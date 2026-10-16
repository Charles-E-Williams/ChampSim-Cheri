//=======================================================================================//
// File             : sms_cap/sms_cap.h
// Description      : Capability-aware SMS at the L2C: the paper's SMS (sms_isca) plus
//                      S1  object-anchored signatures: a trigger whose tagged capability is a
//                          small object (< 2 KB) is also keyed by (PC, trigger line within the
//                          object, object size class);
//                      S2  trigger-relative patterns for those keys: the PHT pattern covers
//                          lines [-8, +23] around the trigger, so one entry serves every
//                          placement of the object in a region and predictions may cross the
//                          region edge (within the physical page); updates are masked to the
//                          lines the generation could observe. The paper's (PC, offset) entry
//                          is trained by every generation as in the baseline; a small-object
//                          trigger prefers its anchored entry unless the last generation for
//                          that key reached past the window (a region-level pattern), flagged
//                          in the window's otherwise redundant trigger bit;
//                      S3  bounds at large objects: for triggers in an object of >= 2 KB,
//                          predicted lines outside the object are dropped;
//                      S4  gated next-region replay: for such a trigger, the region pattern is
//                          also replayed into the next region when the object covers all of it
//                          (and it is on the same physical page). Replays into leader regions
//                          (1 in 64, so 32 L2 sets) are marked in the line's prefetch metadata;
//                          a 2-bit trust counter moves down on each window of leader fills
//                          whose accuracy is below 0.4 and up otherwise, and at zero replays
//                          are issued only into leader regions. Replays are issued only with
//                          the slots regular predictions leave, and a replay never displaces a
//                          regular prediction register.
//                    The AT is address-region based, as in the paper. Untagged triggers take the
//                    paper's path (test 461-6 checks this).
//
//                    Placement: L2C (physical). The object position comes from the capability
//                    cursor, which the core places at the access's effective address.
//=======================================================================================//

#ifndef __SMS_CAP_H__
#define __SMS_CAP_H__

#include <bitset>
#include <cstdint>
#include <vector>

#include "champsim.h"
#include "modules.h"

struct sms_cap : public champsim::modules::prefetcher {
  friend struct sms_test_access; // unit tests (test/cpp/src/461-*)

  constexpr static uint32_t REGION_LINES = 32; // 2 KB region
  typedef std::bitset<REGION_LINES> Pattern;

  // What a trigger access knows; kept in the FT and AT until the generation ends
  class Trigger
  {
  public:
    uint64_t pc;
    uint32_t offset;     // trigger line within the region
    bool anchored;       // S1/S2: a small-object trigger, keyed and patterned relative to the object
    uint32_t obj_line;   // trigger line within the object (anchored only)
    uint32_t size_class; // 2-bit object size class (anchored only)

  public:
    void reset()
    {
      pc = 0;
      offset = 0;
      anchored = false;
      obj_line = 0;
      size_class = 0;
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
    Pattern pattern; // region pattern, or for an anchored key the trigger-relative window (S2)
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
    uint64_t seq;      // allocation order
    uint32_t metadata; // S4: the replay mark for a leader-region replay
    bool replay;       // S4: issued only with the slots regular registers leave, and never displaces one

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
      metadata = 0;
      replay = false;
      cap = champsim::capability{};
      trigger_line = 0;
      cursor_line_va = 0;
    }
    PredRegister() { reset(); }
  };

private:
  // config (paper)
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

  // config (capability layer)
  constexpr static uint64_t LARGE_OBJECT_BYTES = 2048;
  constexpr static int WINDOW_BEFORE = 8; // S2: trigger-relative window [-8, +23]
  constexpr static uint32_t OBJ_LINE_BITS = 6;
  constexpr static uint64_t REPLAY_LEADER_MASK = 0x3F; // S4: regions numbered 1 mod 64 are leaders (32 of 2048 L2 sets); odd, so with
                                                       // 4 KB pages a leader shares its page with the region that replays into it
  constexpr static uint32_t REPLAY_WINDOW = 64;        // S4: leader replay fills per decision
  constexpr static double REPLAY_ACC_LOW = 0.40;       // S4: the IPCP paper's low watermark

public:
  constexpr static uint32_t REPLAY_MARK = 1u << 28; // S4: prefetch metadata bit of a leader replay (clear of IPCP's low bits)

private:
  // internal data structures
  FTEntry filter_table[FT_SIZE];
  uint32_t ft_next = 0; // FIFO replacement
  ATEntry acc_table[AT_SIZE];
  std::vector<std::vector<PHTEntry>> pht;
  PredRegister pred_regs[PRED_REGISTERS];
  uint32_t rr_next = 0;
  uint64_t reg_seq = 0;

  // S4 state
  uint32_t replay_fills = 0;
  uint32_t replay_useful = 0;
  int replay_trust = 3; // 2-bit; blocked at 0
  bool replay_blocked = false;

  // statistics
  uint64_t stat_triggers = 0;
  uint64_t stat_anchored_triggers = 0;
  uint64_t stat_pht_hits = 0;
  uint64_t stat_anchored_hits = 0;
  // path breakdown of triggers: see prefetcher_final_stats
  uint64_t stat_trig_untagged = 0;
  uint64_t stat_trig_large = 0;             // tagged, object >= 2 KB (or cursor outside the object)
  uint64_t stat_trig_anchored_used = 0;     // anchored entry used
  uint64_t stat_trig_anchored_nonlocal = 0; // anchored entry present but flagged non-local, paper entry looked up
  uint64_t stat_trig_anchored_absent = 0;   // no anchored entry, paper entry looked up
  uint64_t stat_trig_fallback_hit = 0;      // of the two above: the paper entry hit
  uint64_t stat_predicted = 0;
  uint64_t stat_issued = 0;
  uint64_t stat_cap_dropped = 0;
  uint64_t stat_page_dropped = 0;
  uint64_t stat_cross_region = 0;
  uint64_t stat_gen_evicted = 0;
  uint64_t stat_gen_replaced = 0;
  uint64_t stat_reg_replaced = 0;
  uint64_t stat_anchored_demoted = 0;
  uint64_t stat_replay_predicted = 0;
  uint64_t stat_replay_blocked = 0;
  uint64_t stat_replay_windows = 0;
  uint64_t stat_replay_blocked_windows = 0;

  // private functions
  FTEntry* search_filter_table(uint64_t region);
  void insert_filter_table(uint64_t region, const Trigger& trig);

  ATEntry* search_acc_table(uint64_t region);
  void update_age_acc_table(ATEntry* current);
  void insert_acc_table(FTEntry* ftentry, uint32_t offset);
  void end_generation(ATEntry* atentry);

  PHTEntry* search_pht(uint64_t signature);
  void update_age_pht(uint64_t signature, PHTEntry* current);
  void write_pht(uint64_t signature, const Pattern& observed, const Pattern& visible);
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

#endif /* __SMS_CAP_H__ */