//=======================================================================================//
// File             : sms_isca/sms_isca.cc
// Description      : Spatial Memory Streaming prefetcher, ISCA'06 / JILP'11 (see sms_isca.h)
//=======================================================================================//

#include "sms_isca.h"

#include <iostream>

void sms_isca::prefetcher_initialize()
{
  /* init PHT */
  pht.resize(PHT_SETS, std::vector<PHTEntry>(PHT_ASSOC));
  for (auto& set : pht)
    for (uint32_t way = 0; way < PHT_ASSOC; way++)
      set[way].lru = way;
  for (uint32_t index = 0; index < AT_SIZE; index++)
    acc_table[index].lru = index;

  std::cout << "SMS_ISCA (L2C)" << std::endl
            << "  region " << REGION_LINES << " lines, FT " << FT_SIZE << ", AT " << AT_SIZE << ", PHT " << PHT_SETS * PHT_ASSOC << " (" << PHT_ASSOC
            << "-way), prediction registers " << PRED_REGISTERS << std::endl;
}

uint32_t sms_isca::prefetcher_cache_operate(champsim::address address, champsim::address ip, uint32_t cpu, champsim::capability cap, uint8_t cache_hit,
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
  stat_triggers++;
  insert_filter_table(region, trig);
  generate_prefetch(trig, line, cap);
  return metadata_in;
}

void sms_isca::prefetcher_cycle_operate() { issue_prefetch(); }

uint32_t sms_isca::prefetcher_cache_fill(champsim::address addr, champsim::address ip, uint32_t cpu, champsim::capability cap, bool useless, long set, long way,
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

void sms_isca::prefetcher_final_stats()
{
  std::cout << "sms_isca final stats" << std::endl
            << "  Triggers / PHT hits:            " << stat_triggers << " / " << stat_pht_hits << std::endl
            << "  Predicted / issued:             " << stat_predicted << " / " << stat_issued << std::endl
            << "  Dropped (other page):           " << stat_page_dropped << std::endl
            << "  Generations ended evict / AT:   " << stat_gen_evicted << " / " << stat_gen_replaced << std::endl
            << "  Prediction registers replaced:  " << stat_reg_replaced << std::endl;
}