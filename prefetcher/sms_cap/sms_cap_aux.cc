//=======================================================================================//
// File             : sms_cap/sms_cap_aux.cc
// Description      : Filter, accumulation and pattern history tables, and prediction
//                    registers of the capability-aware SMS prefetcher (see sms_cap.h)
//=======================================================================================//

#include "cache.h"
#include "cheri_prefetch_utils.h"
#include "sms_cap.h"

namespace
{
uint64_t fold(uint64_t value, uint32_t bits)
{
  uint64_t folded = 0;
  while (value != 0) {
    folded ^= value & ((1ull << bits) - 1);
    value >>= bits;
  }
  return folded;
}

// S2: window bit `window_bit` holds the line at trigger-relative position window_bit - window_before
sms_cap::Pattern window_visible_mask(uint32_t trigger_offset, int window_before)
{
  // window bits whose line lies in the trigger's region: what a generation triggered at trigger_offset can observe
  sms_cap::Pattern mask;
  for (int window_bit = 0; window_bit < (int)sms_cap::REGION_LINES; window_bit++) {
    int region_offset = (int)trigger_offset + window_bit - window_before;
    mask[(std::size_t)window_bit] = (region_offset >= 0 && region_offset < (int)sms_cap::REGION_LINES);
  }
  return mask;
}

sms_cap::Pattern window_region_mask(uint32_t trigger_offset, int window_before)
{
  // region offsets covered by the window of a trigger at trigger_offset
  sms_cap::Pattern mask;
  for (int window_bit = 0; window_bit < (int)sms_cap::REGION_LINES; window_bit++) {
    int region_offset = (int)trigger_offset + window_bit - window_before;
    if (region_offset >= 0 && region_offset < (int)sms_cap::REGION_LINES)
      mask[(std::size_t)region_offset] = 1;
  }
  return mask;
}

sms_cap::Pattern to_window(const sms_cap::Pattern& region_pattern, uint32_t trigger_offset, int window_before)
{
  sms_cap::Pattern window;
  for (int window_bit = 0; window_bit < (int)sms_cap::REGION_LINES; window_bit++) {
    int region_offset = (int)trigger_offset + window_bit - window_before;
    if (region_offset >= 0 && region_offset < (int)sms_cap::REGION_LINES)
      window[(std::size_t)window_bit] = region_pattern[(std::size_t)region_offset];
  }
  return window;
}
} // namespace

/* Functions for Filter table */
sms_cap::FTEntry* sms_cap::search_filter_table(uint64_t region)
{
  for (uint32_t index = 0; index < FT_SIZE; index++)
    if (filter_table[index].valid && filter_table[index].region == region)
      return &filter_table[index];
  return nullptr;
}

void sms_cap::insert_filter_table(uint64_t region, const Trigger& trig)
{
  FTEntry& ftentry = filter_table[ft_next];
  ft_next = (ft_next + 1) % FT_SIZE;
  ftentry.valid = true;
  ftentry.region = region;
  ftentry.trig = trig;
}

/* Functions for Accumulation Table */
sms_cap::ATEntry* sms_cap::search_acc_table(uint64_t region)
{
  for (uint32_t index = 0; index < AT_SIZE; index++)
    if (acc_table[index].valid && acc_table[index].region == region)
      return &acc_table[index];
  return nullptr;
}

void sms_cap::update_age_acc_table(ATEntry* current)
{
  for (uint32_t index = 0; index < AT_SIZE; index++)
    if (acc_table[index].lru < current->lru)
      acc_table[index].lru++;
  current->lru = 0;
}

void sms_cap::insert_acc_table(FTEntry* ftentry, uint32_t offset)
{
  // an invalid entry if there is one, else the LRU entry, whose generation ends
  ATEntry* victim = nullptr;
  for (uint32_t index = 0; index < AT_SIZE; index++)
    if (!acc_table[index].valid && (victim == nullptr || acc_table[index].lru > victim->lru))
      victim = &acc_table[index];
  if (victim == nullptr) {
    victim = &acc_table[0];
    for (uint32_t index = 0; index < AT_SIZE; index++)
      if (acc_table[index].lru > victim->lru)
        victim = &acc_table[index];
    end_generation(victim);
    stat_gen_replaced++;
  }

  victim->valid = true;
  victim->region = ftentry->region;
  victim->trig = ftentry->trig;
  victim->pattern.reset();
  victim->pattern[ftentry->trig.offset] = 1;
  victim->pattern[offset] = 1;
  update_age_acc_table(victim);
}

void sms_cap::end_generation(ATEntry* atentry)
{
  insert_pht_table(atentry->trig, atentry->pattern);
  atentry->valid = false;
}

/* Functions for Pattern History Table */
uint64_t sms_cap::create_signature(const Trigger& trig)
{
  // low 9 bits: [8] anchored, then either the region offset (5 bits) or size class (2) + object line (6)
  uint64_t field = trig.anchored ? ((1u << 8) | (trig.size_class << OBJ_LINE_BITS) | trig.obj_line) : trig.offset;
  uint64_t signature = trig.pc >> PC_ALIGN_BITS;
  signature = (signature << 9);
  signature += field;
  return signature;
}

sms_cap::PHTEntry* sms_cap::search_pht(uint64_t signature)
{
  std::vector<PHTEntry>& set = pht[fold(signature, PHT_INDEX_BITS)];
  uint16_t tag = (uint16_t)fold(signature >> PHT_INDEX_BITS, PHT_TAG_BITS);
  for (uint32_t way = 0; way < PHT_ASSOC; way++)
    if (set[way].valid && set[way].tag == tag)
      return &set[way];
  return nullptr;
}

void sms_cap::update_age_pht(uint64_t signature, PHTEntry* current)
{
  std::vector<PHTEntry>& set = pht[fold(signature, PHT_INDEX_BITS)];
  for (uint32_t way = 0; way < PHT_ASSOC; way++)
    if (set[way].lru < current->lru)
      set[way].lru++;
  current->lru = 0;
}

void sms_cap::write_pht(uint64_t signature, const Pattern& observed, const Pattern& visible)
{
  PHTEntry* phtentry = search_pht(signature);
  if (phtentry != nullptr) {
    /* PHT hit: update the lines the generation could observe */
    phtentry->pattern = (phtentry->pattern & ~visible) | (observed & visible);
  } else {
    /* PHT miss: replace the LRU way */
    std::vector<PHTEntry>& set = pht[fold(signature, PHT_INDEX_BITS)];
    phtentry = &set[0];
    for (uint32_t way = 0; way < PHT_ASSOC; way++)
      if (set[way].lru > phtentry->lru)
        phtentry = &set[way];
    phtentry->valid = true;
    phtentry->tag = (uint16_t)fold(signature >> PHT_INDEX_BITS, PHT_TAG_BITS);
    phtentry->pattern = observed;
  }
  update_age_pht(signature, phtentry);
}

void sms_cap::insert_pht_table(const Trigger& trig, const Pattern& pattern)
{
  // the paper's (PC, offset) entry is always trained, exactly as in the baseline
  Trigger paper_trig = trig;
  paper_trig.anchored = false;
  Pattern all;
  all.set();
  write_pht(create_signature(paper_trig), pattern, all);

  if (!trig.anchored)
    return;
  // S2: the object-anchored entry learns the trigger-relative window, masked to the lines this generation could see.
  // Window bit WINDOW_BEFORE is the trigger itself, so it carries no pattern information; it holds the entry's
  // "local" flag: set by a generation that stayed inside the window, cleared by one that reached past it (a
  // region-level pattern such as a run of neighbouring objects), in which case the paper's entry is used instead.
  uint64_t signature = create_signature(trig);
  if ((pattern & ~window_region_mask(trig.offset, WINDOW_BEFORE)).any()) {
    PHTEntry* phtentry = search_pht(signature);
    if (phtentry != nullptr && phtentry->pattern[WINDOW_BEFORE]) {
      phtentry->pattern[WINDOW_BEFORE] = 0;
      stat_anchored_demoted++;
    }
    return;
  }
  write_pht(signature, to_window(pattern, trig.offset, WINDOW_BEFORE), window_visible_mask(trig.offset, WINDOW_BEFORE)); // sets the flag
}

/* Prediction */
void sms_cap::generate_prefetch(const Trigger& trig, uint64_t line, champsim::capability cap)
{
  // a small-object trigger uses its object-anchored entry while that entry is flagged local (S1/S2); otherwise, and
  // for every other trigger, the paper's (PC, offset) entry
  Trigger key = trig;
  PHTEntry* phtentry = nullptr;
  if (trig.anchored) {
    phtentry = search_pht(create_signature(key));
    if (phtentry == nullptr) {
      stat_trig_anchored_absent++;
    } else if (!phtentry->pattern[WINDOW_BEFORE]) {
      stat_trig_anchored_nonlocal++;
      phtentry = nullptr;
    } else {
      stat_trig_anchored_used++;
    }
  }
  if (phtentry == nullptr) {
    key.anchored = false;
    phtentry = search_pht(create_signature(key));
  }
  if (phtentry == nullptr)
    return;
  if (trig.anchored && !key.anchored)
    stat_trig_fallback_hit++;
  stat_pht_hits++;
  if (key.anchored)
    stat_anchored_hits++;
  update_age_pht(create_signature(key), phtentry);

  PredRegister reg;
  reg.valid = true;
  reg.trigger_line = line;
  reg.cap = cap;
  reg.cursor_line_va = (cap.base.to<uint64_t>() + cap.offset.to<uint64_t>()) >> LOG2_BLOCK_SIZE; // raw: capability_cursor() asserts on a wrapped offset
  uint32_t trigger_bit = key.anchored ? (uint32_t)WINDOW_BEFORE : trig.offset;
  reg.base_line = (int64_t)line - (int64_t)trigger_bit;

  uint64_t page_lines_log = LOG2_PAGE_SIZE - LOG2_BLOCK_SIZE;
  uint64_t region = line >> LOG2_REGION_LINES;
  bool clip_to_object = cap.tag && cap.length.to<uint64_t>() >= LARGE_OBJECT_BYTES;
  uint64_t base = cap.base.to<uint64_t>();
  uint64_t top = cheri::capability_top(cap).to<uint64_t>();

  for (uint32_t index = 0; index < REGION_LINES; index++) {
    if (!phtentry->pattern[index] || index == trigger_bit)
      continue;
    int64_t pf_line = reg.base_line + (int64_t)index;
    if (pf_line < 0 || ((uint64_t)pf_line >> page_lines_log) != (line >> page_lines_log)) {
      stat_page_dropped++;
      continue;
    }
    if (clip_to_object) { // S3
      uint64_t pf_va_line = reg.cursor_line_va + (uint64_t)(pf_line - (int64_t)line);
      if (!cheri::overlaps_bounds(champsim::block_number{pf_va_line}, champsim::address{base}, champsim::address{top})) {
        stat_cap_dropped++;
        continue;
      }
    }
    if (((uint64_t)pf_line >> LOG2_REGION_LINES) != region)
      stat_cross_region++;
    reg.pending[index] = 1;
    stat_predicted++;
  }
  if (reg.pending.any())
    buffer_prefetch(reg);

  // S4: replay the region pattern into the next region of a large object
  if (key.anchored || !clip_to_object)
    return;
  uint64_t next_first = (region + 1) << LOG2_REGION_LINES;
  uint64_t next_last = next_first + REGION_LINES - 1;
  uint64_t first_va = reg.cursor_line_va + (next_first - line);
  uint64_t last_va = reg.cursor_line_va + (next_last - line);
  bool inside = (first_va << LOG2_BLOCK_SIZE) >= base && ((last_va + 1) << LOG2_BLOCK_SIZE) <= top;
  bool same_page = (next_last >> page_lines_log) == (line >> page_lines_log);
  if (!inside || !same_page)
    return;
  bool leader = ((region + 1) & REPLAY_LEADER_MASK) == 1;
  if (replay_blocked && !leader) {
    stat_replay_blocked++;
    return;
  }
  PredRegister replay = reg;
  replay.base_line = (int64_t)next_first;
  replay.pending = phtentry->pattern;
  replay.metadata = leader ? REPLAY_MARK : 0;
  replay.replay = true;
  stat_replay_predicted += replay.pending.count();
  if (replay.pending.any())
    buffer_prefetch(replay);
}

void sms_cap::buffer_prefetch(PredRegister reg)
{
  // a free prediction register, else replace the oldest; an S4 replay may replace only another replay
  PredRegister* slot = nullptr;
  for (uint32_t index = 0; index < PRED_REGISTERS; index++)
    if (!pred_regs[index].valid) {
      slot = &pred_regs[index];
      break;
    }
  if (slot == nullptr) {
    for (uint32_t index = 0; index < PRED_REGISTERS; index++)
      if ((!reg.replay || pred_regs[index].replay) && (slot == nullptr || pred_regs[index].seq < slot->seq))
        slot = &pred_regs[index];
    if (slot == nullptr) // a replay finds only regular registers: dropped
      return;
    stat_reg_replaced++;
  }
  reg.seq = reg_seq++;
  *slot = reg;
}

void sms_cap::issue_prefetch()
{
  // up to PREF_DEGREE prefetches per cycle, round-robin between prediction registers:
  // regular registers first, S4 replays only with the issue slots they leave
  uint32_t count = 0;
  for (int pass = 0; pass < 2; pass++) {
    bool replay_pass = (pass == 1);
    for (uint32_t scanned = 0; scanned < PRED_REGISTERS && count < PREF_DEGREE;) {
      PredRegister& reg = pred_regs[rr_next];
      if (reg.valid && reg.pending.any() && reg.replay != replay_pass) {
        rr_next = (rr_next + 1) % PRED_REGISTERS;
        scanned++;
        continue;
      }
      if (!reg.valid || reg.pending.none()) {
        reg.valid = false;
        rr_next = (rr_next + 1) % PRED_REGISTERS;
        scanned++;
        continue;
      }
      uint32_t index = 0;
      while (!reg.pending[index])
        index++;
      uint64_t pf_line = (uint64_t)(reg.base_line + (int64_t)index);

      // simulation only: carry the trigger's capability, re-pointed at the prefetched line, for the by-size statistics
      champsim::capability pf_cap = reg.cap;
      if (pf_cap.tag)
        cheri::repoint_cap_to_line(pf_cap, champsim::address{(reg.cursor_line_va + (pf_line - reg.trigger_line)) << LOG2_BLOCK_SIZE});
      if (!prefetch_line(champsim::address{pf_line << LOG2_BLOCK_SIZE}, true, reg.metadata, pf_cap))
        return; // prefetch queue full

      reg.pending[index] = 0;
      stat_issued++;
      count++;
      if (reg.pending.none())
        reg.valid = false;
      rr_next = (rr_next + 1) % PRED_REGISTERS;
      scanned = 0;
    }
  }
}