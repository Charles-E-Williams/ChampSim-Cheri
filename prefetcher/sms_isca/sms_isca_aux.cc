//=======================================================================================//
// File             : sms_isca/sms_isca_aux.cc
// Description      : Filter, accumulation and pattern history tables, and prediction
//                    registers of the SMS prefetcher, ISCA'06 / JILP'11 (see sms_isca.h)
//=======================================================================================//

#include "cache.h"
#include "cheri_prefetch_utils.h"
#include "sms_isca.h"

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
} // namespace

/* Functions for Filter table */
sms_isca::FTEntry* sms_isca::search_filter_table(uint64_t region)
{
  for (uint32_t index = 0; index < FT_SIZE; index++)
    if (filter_table[index].valid && filter_table[index].region == region)
      return &filter_table[index];
  return nullptr;
}

void sms_isca::insert_filter_table(uint64_t region, const Trigger& trig)
{
  FTEntry& ftentry = filter_table[ft_next];
  ft_next = (ft_next + 1) % FT_SIZE;
  ftentry.valid = true;
  ftentry.region = region;
  ftentry.trig = trig;
}

/* Functions for Accumulation Table */
sms_isca::ATEntry* sms_isca::search_acc_table(uint64_t region)
{
  for (uint32_t index = 0; index < AT_SIZE; index++)
    if (acc_table[index].valid && acc_table[index].region == region)
      return &acc_table[index];
  return nullptr;
}

void sms_isca::update_age_acc_table(ATEntry* current)
{
  for (uint32_t index = 0; index < AT_SIZE; index++)
    if (acc_table[index].lru < current->lru)
      acc_table[index].lru++;
  current->lru = 0;
}

void sms_isca::insert_acc_table(FTEntry* ftentry, uint32_t offset)
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

void sms_isca::end_generation(ATEntry* atentry)
{
  insert_pht_table(atentry->trig, atentry->pattern);
  atentry->valid = false;
}

/* Functions for Pattern History Table */
uint64_t sms_isca::create_signature(const Trigger& trig)
{
  uint64_t signature = trig.pc >> PC_ALIGN_BITS;
  signature = (signature << 9);
  signature += (uint64_t)trig.offset;
  return signature;
}

sms_isca::PHTEntry* sms_isca::search_pht(uint64_t signature)
{
  std::vector<PHTEntry>& set = pht[fold(signature, PHT_INDEX_BITS)];
  uint16_t tag = (uint16_t)fold(signature >> PHT_INDEX_BITS, PHT_TAG_BITS);
  for (uint32_t way = 0; way < PHT_ASSOC; way++)
    if (set[way].valid && set[way].tag == tag)
      return &set[way];
  return nullptr;
}

void sms_isca::update_age_pht(uint64_t signature, PHTEntry* current)
{
  std::vector<PHTEntry>& set = pht[fold(signature, PHT_INDEX_BITS)];
  for (uint32_t way = 0; way < PHT_ASSOC; way++)
    if (set[way].lru < current->lru)
      set[way].lru++;
  current->lru = 0;
}

void sms_isca::insert_pht_table(const Trigger& trig, const Pattern& pattern)
{
  uint64_t signature = create_signature(trig);
  PHTEntry* phtentry = search_pht(signature);
  if (phtentry == nullptr) {
    /* PHT miss: replace the LRU way */
    std::vector<PHTEntry>& set = pht[fold(signature, PHT_INDEX_BITS)];
    phtentry = &set[0];
    for (uint32_t way = 0; way < PHT_ASSOC; way++)
      if (set[way].lru > phtentry->lru)
        phtentry = &set[way];
    phtentry->valid = true;
    phtentry->tag = (uint16_t)fold(signature >> PHT_INDEX_BITS, PHT_TAG_BITS);
  }
  phtentry->pattern = pattern;
  update_age_pht(signature, phtentry);
}

/* Prediction */
void sms_isca::generate_prefetch(const Trigger& trig, uint64_t line, champsim::capability cap)
{
  uint64_t signature = create_signature(trig);
  PHTEntry* phtentry = search_pht(signature);
  if (phtentry == nullptr)
    return;
  stat_pht_hits++;
  update_age_pht(signature, phtentry);

  PredRegister reg;
  reg.valid = true;
  reg.trigger_line = line;
  reg.cap = cap;
  reg.cursor_line_va = (cap.base.to<uint64_t>() + cap.offset.to<uint64_t>()) >> LOG2_BLOCK_SIZE; // raw: capability_cursor() asserts on a wrapped offset
  reg.base_line = (int64_t)line - (int64_t)trig.offset;

  uint64_t page_lines_log = LOG2_PAGE_SIZE - LOG2_BLOCK_SIZE;
  for (uint32_t index = 0; index < REGION_LINES; index++) {
    if (!phtentry->pattern[index] || index == trig.offset)
      continue;
    int64_t pf_line = reg.base_line + (int64_t)index;
    if (pf_line < 0 || ((uint64_t)pf_line >> page_lines_log) != (line >> page_lines_log)) {
      stat_page_dropped++;
      continue;
    }
    reg.pending[index] = 1;
    stat_predicted++;
  }
  if (reg.pending.any())
    buffer_prefetch(reg);
}

void sms_isca::buffer_prefetch(PredRegister reg)
{
  // a free prediction register, else replace the oldest
  PredRegister* slot = nullptr;
  for (uint32_t index = 0; index < PRED_REGISTERS; index++)
    if (!pred_regs[index].valid) {
      slot = &pred_regs[index];
      break;
    }
  if (slot == nullptr) {
    slot = &pred_regs[0];
    for (uint32_t index = 0; index < PRED_REGISTERS; index++)
      if (pred_regs[index].seq < slot->seq)
        slot = &pred_regs[index];
    stat_reg_replaced++;
  }
  reg.seq = reg_seq++;
  *slot = reg;
}

void sms_isca::issue_prefetch()
{
  // up to PREF_DEGREE prefetches per cycle, round-robin between prediction registers
  uint32_t count = 0;
  for (uint32_t scanned = 0; scanned < PRED_REGISTERS && count < PREF_DEGREE;) {
    PredRegister& reg = pred_regs[rr_next];
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
    if (!prefetch_line(champsim::address{pf_line << LOG2_BLOCK_SIZE}, true, 0, pf_cap))
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