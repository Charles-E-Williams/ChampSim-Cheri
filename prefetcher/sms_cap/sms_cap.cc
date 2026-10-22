//=======================================================================================//
// File             : sms_cap/sms_cap.cc
// Description      : Capability-aware Spatial Memory Streaming prefetcher (see sms_cap.h)
//=======================================================================================//

#include "sms_cap.h"

#include <iostream>

void sms_cap::prefetcher_initialize()
{
  /* init PHT */
  pht.resize(PHT_SETS, std::vector<PHTEntry>(PHT_ASSOC));
  for (auto& set : pht)
    for (uint32_t way = 0; way < PHT_ASSOC; way++)
      set[way].lru = way;
  for (uint32_t index = 0; index < AT_SIZE; index++)
    acc_table[index].lru = index;

  std::cout << "SMS_CAP (L2C)" << std::endl
            << "  region " << REGION_LINES << " lines, FT " << FT_SIZE << ", AT " << AT_SIZE << ", PHT " << PHT_SETS * PHT_ASSOC << " (" << PHT_ASSOC
            << "-way), prediction registers " << PRED_REGISTERS << ", CHERI-aware" << std::endl;
}

uint32_t sms_cap::prefetcher_cache_operate(champsim::address address, champsim::address ip, uint32_t cpu, champsim::capability cap, uint8_t cache_hit,
                                           bool useful_prefetch, access_type type, uint32_t metadata_in, uint32_t metadata_hit)
{
  uint64_t line = address.to<uint64_t>() >> LOG2_BLOCK_SIZE;
  uint64_t region = line >> LOG2_REGION_LINES;
  uint32_t offset = (uint32_t)(line & (REGION_LINES - 1));

  ATEntry* at_entry = search_acc_table(region);
  if (at_entry != nullptr) {
    /* accumulation table hit */
    at_entry->pattern[offset] = 1;
    update_age_acc_table(at_entry);
    return metadata_in;
  }

  FTEntry* ft_entry = search_filter_table(region);
  if (ft_entry != nullptr) {
    /* filter table hit: the second distinct block of the generation moves it to the AT */
    if (offset != ft_entry->trig.offset) {
      insert_acc_table(ft_entry, offset);
      ft_entry->valid = false;
    }
    return metadata_in;
  }

  /* filter table miss. Beginning of new generation. Issue prefetch */
  Trigger trig;
  trig.pc = ip.to<uint64_t>();
  trig.offset = offset;

  /* S1: a trigger in a small object is also keyed by its line within the object and the object's size class */
  uint64_t length = cap.length.to<uint64_t>();
  if (cap.tag && length > 0 && length < LARGE_OBJECT_BYTES) {
    uint64_t base_line = cap.base.to<uint64_t>() >> LOG2_BLOCK_SIZE;
    uint64_t cursor_line = (cap.base.to<uint64_t>() + cap.offset.to<uint64_t>()) >> LOG2_BLOCK_SIZE;
    uint64_t last_line = (cap.base.to<uint64_t>() + length - 1) >> LOG2_BLOCK_SIZE;
    if (cursor_line >= base_line && cursor_line <= last_line) {
      uint64_t obj_lines = last_line - base_line + 1;
      trig.anchored = true;
      trig.obj_line = (uint32_t)(cursor_line - base_line);
      trig.size_class = (obj_lines <= 1) ? 0 : ((obj_lines == 2) ? 1 : ((obj_lines <= 4) ? 2 : 3));
      stat_anchored_triggers++;
    }
  }
  stat_triggers++;
  if (!cap.tag)
    stat_trig_untagged++;
  else if (!trig.anchored)
    stat_trig_large++;

  insert_filter_table(region, trig);
  generate_prefetch(trig, line, cap);
  return metadata_in;
}

void sms_cap::prefetcher_cycle_operate() { issue_prefetch(); }

uint32_t sms_cap::prefetcher_cache_fill(champsim::address addr, champsim::address ip, uint32_t cpu, champsim::capability cap, bool useless, long set, long way,
                                        bool prefetch, champsim::address evicted_addr, champsim::capability evicted_cap, uint32_t metadata_in,
                                        uint32_t metadata_evict, uint32_t cpu_evict)
{
  if (evicted_addr == champsim::address{})
    return metadata_in;

  /* eviction of any block of a region ends its generation */
  uint64_t region = (evicted_addr.to<uint64_t>() >> LOG2_BLOCK_SIZE) >> LOG2_REGION_LINES;
  FTEntry* ft_entry = search_filter_table(region);
  if (ft_entry != nullptr)
    ft_entry->valid = false;
  ATEntry* at_entry = search_acc_table(region);
  if (at_entry != nullptr) {
    end_generation(at_entry);
    stat_gen_evicted++;
  }
  return metadata_in;
}

void sms_cap::prefetcher_final_stats()
{
  std::cout << "sms_cap final stats" << std::endl
            << "  Triggers / PHT hits:            " << stat_triggers << " / " << stat_pht_hits << std::endl
            << "  Predicted / issued:             " << stat_predicted << " / " << stat_issued << std::endl
            << "  Dropped (other page):           " << stat_page_dropped << std::endl
            << "  Generations ended evict / AT:   " << stat_gen_evicted << " / " << stat_gen_replaced << std::endl
            << "  Prediction registers replaced:  " << stat_reg_replaced << std::endl
            << "  Anchored triggers / hits (S1):  " << stat_anchored_triggers << " / " << stat_anchored_hits << std::endl
            << "  Path: trigger untagged (paper path):             " << stat_trig_untagged << std::endl
            << "  Path: trigger in object >= 2 KB (paper path+S3):  " << stat_trig_large << std::endl
            << "  Path: anchored, anchored entry used:             " << stat_trig_anchored_used << std::endl
            << "  Path: anchored, entry non-local -> paper entry:  " << stat_trig_anchored_nonlocal << std::endl
            << "  Path: anchored, no entry -> paper entry:         " << stat_trig_anchored_absent << std::endl
            << "  Path: of those falling back, paper entry hit:    " << stat_trig_fallback_hit << std::endl
            << "  Cross-region predictions (S2): " << stat_cross_region << std::endl
            << "  Anchored entries demoted (S2): " << stat_anchored_demoted << std::endl
            << "  Dropped at object end (S3):     " << stat_cap_dropped << std::endl;
}