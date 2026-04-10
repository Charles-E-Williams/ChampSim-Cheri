#include "berti_cheri.h"
#include <algorithm>

/*
 * Berti-CHERI: CHERI-capability-aware Berti local-delta prefetcher.
 *
 */


/******************************************************************************/
/*                      Latency table functions                               */
/******************************************************************************/

std::vector<berti_cheri::HistoryTable*> berti_cheri::historyt;
std::vector<berti_cheri::LatencyTable*> berti_cheri::latencyt;
std::vector<berti_cheri::ShadowCache*> berti_cheri::scache;
uint64_t berti_cheri::others = 0;

uint8_t berti_cheri::LatencyTable::add(uint64_t addr, uint64_t tag, bool pf,
                                       uint64_t cycle, uint64_t cap_base)
{
  if constexpr (champsim::debug_print) 
  {
    std::cout << "[BERTI_CHERI_LATENCY] " << __func__;
    std::cout << " addr: " << std::hex << addr << " tag: " << tag;
    std::cout << " pf: " << std::dec << +pf << " cycle: " << cycle;
    std::cout << " cap_base: " << std::hex << cap_base << std::dec;
  }

  latency_table *free = nullptr;

  for (int i = 0; i < size; i++)
  {
    if (latencyt[i].addr == addr)
    {
      if constexpr (champsim::debug_print) 
      {
        std::cout << " line already found; find_tag: " << latencyt[i].tag;
        std::cout << " find_pf: " << +latencyt[i].pf << std::endl;
      }
      latencyt[i].pf       = pf;
      latencyt[i].tag      = tag;
      latencyt[i].cap_base = cap_base;
      return latencyt[i].pf;
    }

    if (latencyt[i].tag == 0) free = &latencyt[i];
  }

  if (free == nullptr) assert(0 && "No free space latency table");

  free->addr     = addr;
  free->time     = cycle;
  free->tag      = tag;
  free->pf       = pf;
  free->cap_base = cap_base;

  if constexpr (champsim::debug_print) std::cout << " new entry" << std::endl;
  return free->pf;
}

uint64_t berti_cheri::LatencyTable::del(uint64_t addr)
{
  if constexpr (champsim::debug_print) 
  {
    std::cout << "[BERTI_CHERI_LATENCY] " << __func__;
    std::cout << " addr: " << std::hex << addr;
  }

  for (int i = 0; i < size; i++)
  {
    if (latencyt[i].addr == addr)
    {
      uint64_t time = latencyt[i].time;

      if constexpr (champsim::debug_print)
      {
        std::cout << " tag: " << latencyt[i].tag;
        std::cout << " pf: " << std::dec << +latencyt[i].pf;
        std::cout << " cycle: " << latencyt[i].time << std::endl;
      }

      latencyt[i].addr     = 0;
      latencyt[i].tag      = 0;
      latencyt[i].time     = 0;
      latencyt[i].pf       = 0;
      latencyt[i].cap_base = 0;

      return time;
    }
  }

  if constexpr (champsim::debug_print) std::cout << " TRANSLATION" << std::endl;
  return 0;
}

uint64_t berti_cheri::LatencyTable::get(uint64_t addr)
{
  if constexpr (champsim::debug_print)
  {
    std::cout << "[BERTI_CHERI_LATENCY] " << __func__;
    std::cout << " addr: " << std::hex << addr << std::dec;
  }

  for (int i = 0; i < size; i++)
  {
    if (latencyt[i].addr == addr)
    {
      if constexpr (champsim::debug_print)
        std::cout << " time: " << latencyt[i].time << std::endl;
      return latencyt[i].time;
    }
  }

  if constexpr (champsim::debug_print) std::cout << " NOT FOUND" << std::endl;
  return 0;
}

uint64_t berti_cheri::LatencyTable::get_tag(uint64_t addr)
{
  if constexpr (champsim::debug_print) 
  {
    std::cout << "[BERTI_CHERI_LATENCY] " << __func__;
    std::cout << " addr: " << std::hex << addr;
  }

  for (int i = 0; i < size; i++)
  {
    if (latencyt[i].addr == addr && latencyt[i].tag)
    {
      if constexpr (champsim::debug_print) 
        std::cout << " tag: " << latencyt[i].tag << std::endl;
      return latencyt[i].tag;
    }
  }

  if constexpr (champsim::debug_print) std::cout << " NOT_FOUND" << std::endl;
  return 0;
}

uint64_t berti_cheri::LatencyTable::get_cap_base(uint64_t addr)
{
  for (int i = 0; i < size; i++)
  {
    if (latencyt[i].addr == addr && latencyt[i].tag)
      return latencyt[i].cap_base;
  }
  return 0;
}


/******************************************************************************/
/*                       Shadow Cache functions                               */
/******************************************************************************/
bool berti_cheri::ShadowCache::add(long set, long way, uint64_t addr, bool pf, uint64_t lat)
{
  /*
   * Add block to shadow cache
   *
   * Parameters:
   *      - cpu: cpu
   *      - set: cache set
   *      - way: cache way
   *      - addr: cache block v_addr
   *      - access: the cache is access by a demand
   */

  if constexpr (champsim::debug_print)
  {
    std::cout << "[BERTI_SHADOW_CACHE] " << __func__;
    std::cout << " set: " << set << " way: " << way;
    std::cout << " addr: " << std::hex << addr << std::dec;
    std::cout << " pf: " << +pf;
    std::cout << " latency: " << lat << std::endl;
  }

  scache[set][way].addr = addr;
  scache[set][way].pf   = pf;
  scache[set][way].lat  = lat;
  return scache[set][way].pf;
}

bool berti_cheri::ShadowCache::get(uint64_t addr)
{
  /*
   * Parameters:
   *      - addr: cache block v_addr
   *
   * Return: true if the addr is in the l1d cache, false otherwise
   */

  if constexpr (champsim::debug_print)
  {
    std::cout << "[BERTI_SHADOW_CACHE] " << __func__;
    std::cout << " addr: " << std::hex << addr << std::endl;
  }

  for (int i = 0; i < sets; i++)
  {
    for (int ii = 0; ii < ways; ii++)
    {
      if (scache[i][ii].addr == addr) 
      {
        if constexpr (champsim::debug_print)
        {
          std::cout << " set: " << i << " way: " << i << std::endl;
        }
        return true;
      }
    }
  }

  return false;
}

void berti_cheri::ShadowCache::set_pf(uint64_t addr, bool pf)
{
  /*
   * Parameters:
   *      - addr: cache block v_addr
   *
   * Return: change value of pf field 
   */
  if constexpr (champsim::debug_print)
  {
    std::cout << "[BERTI_SHADOW_CACHE] " << __func__;
    std::cout << " addr: " << std::hex << addr << std::dec;
  }

  for (int i = 0; i < sets; i++)
  {
    for (int ii = 0; ii < ways; ii++)
    {
      if (scache[i][ii].addr == addr) 
      {
        if constexpr (champsim::debug_print)
        {
          std::cout << " set: " << i << " way: " << ii;
          std::cout << " old_pf_value: " << +scache[i][ii].pf;
          std::cout << " new_pf_value: " << +pf << std::endl;
        }
        scache[i][ii].pf = pf;
        return;
      }
    }
  }

  // The address should always be in the cache
  aliased_cache_hits++;
  //assert((0) && "Address is must be in shadow cache");
}

bool berti_cheri::ShadowCache::is_pf(uint64_t addr)
{
  /*
   * Parameters:
   *      - addr: cache block v_addr
   *
   * Return: True if the saved one is a prefetch
   */

  if constexpr (champsim::debug_print)
  {
    std::cout << "[BERTI_SHADOW_CACHE] " << __func__;
    std::cout << " addr: " << std::hex << addr << std::dec;
  }

  for (int i = 0; i < sets; i++)
  {
    for (int ii = 0; ii < ways; ii++)
    {
      if (scache[i][ii].addr == addr)
      {
        if constexpr (champsim::debug_print)
        {
          std::cout << " set: " << i << " way: " << ii;
          std::cout << " pf: " << +scache[i][ii].pf << std::endl;
        }

        return scache[i][ii].pf;
      }
    }
  }

  aliased_cache_hits++;
  //assert((0) && "Address is must be in shadow cache");
  return 0;
}

uint64_t berti_cheri::ShadowCache::get_latency(uint64_t addr)
{
  /*
   * Init shadow cache
   *
   * Parameters:
   *      - addr: cache block v_addr
   *
   * Return: the saved latency
   */
  if constexpr (champsim::debug_print)
  {
    std::cout << "[BERTI_SHADOW_CACHE] " << __func__;
    std::cout << " addr: " << std::hex << addr << std::dec;
  }

  for (int i = 0; i < sets; i++)
  {
    for (int ii = 0; ii < ways; ii++)
    {
      if (scache[i][ii].addr == addr) 
      {
        if constexpr (champsim::debug_print)
        {
          std::cout << " set: " << i << " way: " << ii;
          std::cout << " latency: " << scache[i][ii].lat << std::endl;
        }

        return scache[i][ii].lat;
      }
    }
  }
  aliased_cache_hits++;
  //assert((0) && "Address is must be in shadow cache");
  return 0;
}

/******************************************************************************/
/*                       History Table functions                              */
/******************************************************************************/
void berti_cheri::HistoryTable::add(uint64_t tag, uint64_t addr, uint64_t cycle)
{
  /*
   * Save the new information into the history table
   *
   * Parameters:
   *  - tag: PC tag
   *  - addr: addr access
   */
  uint16_t set = tag & TABLE_SET_MASK;

  // If the latest entry is the same, we do not add it
  if (history_pointers[set] == &historyt[set][ways - 1])
  {
    if (historyt[set][0].addr == (addr & ADDR_MASK)) return;
  } else if ((history_pointers[set] - 1)->addr == (addr & ADDR_MASK)) return;

  history_pointers[set]->tag  = tag;
  history_pointers[set]->time = cycle & TIME_MASK;
  history_pointers[set]->addr = addr & ADDR_MASK;

  if constexpr (champsim::debug_print)
  {
    std::cout << "[BERTI_CHERI_HISTORY] " << __func__;
    std::cout << " tag: " << std::hex << tag << " cap_cl_off: " << addr << std::dec;
    std::cout << " cycle: " << cycle << " set: " << set << std::endl;
  }

  if (history_pointers[set] == &historyt[set][ways - 1])
    history_pointers[set] = &historyt[set][0];
  else
    history_pointers[set]++;
}

uint16_t berti_cheri::HistoryTable::get_aux(uint32_t latency, 
    uint64_t tag, uint64_t act_addr, uint64_t *tags, uint64_t *addr, 
    uint64_t cycle)
{
  uint16_t num_on_time = 0;
  uint16_t set = tag & TABLE_SET_MASK;

  if constexpr (champsim::debug_print)
  {
    std::cout << "[BERTI_CHERI_HISTORY] " << __func__;
    std::cout << " tag: " << std::hex << tag << " cap_cl_off: " << act_addr << std::dec;
    std::cout << " cycle: " << cycle << " set: " << set << std::endl;
  }

  if (cycle < latency) return num_on_time;
  cycle -= latency; 

  history_table *pointer = history_pointers[set];

  do
  {
    if (pointer->tag == tag && pointer->time <= cycle)
    {
      // Skip if same offset 
      if (pointer->addr == act_addr) return num_on_time;

      tags[num_on_time] = pointer->tag;
      addr[num_on_time] = pointer->addr;
      num_on_time++;
    }

    if (pointer == historyt[set])
      pointer = &historyt[set][ways - 1];
    else
      pointer--;
  } while (pointer != history_pointers[set]);

  return num_on_time;
}

uint16_t berti_cheri::HistoryTable::get(uint32_t latency, uint64_t tag,
    uint64_t act_addr, uint64_t *tags, uint64_t *addr, uint64_t cycle)
{
  act_addr &= ADDR_MASK;
  uint16_t num_on_time = get_aux(latency, tag, act_addr, tags, addr, cycle & TIME_MASK);
  return num_on_time;
}

/******************************************************************************/
/*                        Berti table functions                               */
/******************************************************************************/
void berti_cheri::increase_conf_tag(uint64_t tag)
{
  if constexpr (champsim::debug_print)
    std::cout << "[BERTI_CHERI] " << __func__ << " tag: " << std::hex << tag << std::dec;

  if (bertit.find(tag) == bertit.end())
  {
    if constexpr (champsim::debug_print) 
      std::cout << " TAG NOT FOUND" << std::endl;
    return;
  }

  bertit[tag]->conf += CONFIDENCE_INC;

  if constexpr (champsim::debug_print) 
    std::cout << " global_conf: " << bertit[tag]->conf;

  if (bertit[tag]->conf == CONFIDENCE_MAX) 
  {
    for (auto &i: bertit[tag]->deltas)
    {
      if (i.conf > CONFIDENCE_L1)       i.rpl = BERTI_L1;
      else if (i.conf > CONFIDENCE_L2)  i.rpl = BERTI_L2;
      else if (i.conf > CONFIDENCE_L2R) i.rpl = BERTI_L2R;
      else                              i.rpl = BERTI_R;

      if constexpr (champsim::debug_print) 
      {
        std::cout << "Delta: " << i.delta;
        std::cout << " Conf: "  << i.conf << " Level: " << +i.rpl;
        std::cout << "|";
      }

      i.conf = 0;
    }

    bertit[tag]->conf = 0;
  }

  if constexpr (champsim::debug_print) std::cout << std::endl;
}

void berti_cheri::add(uint64_t tag, int64_t delta)
{
  if constexpr (champsim::debug_print)
  {
    std::cout << "[BERTI_CHERI] " << __func__;
    std::cout << " tag: " << std::hex << tag << std::dec;
    std::cout << " delta: " << delta;
  }

  auto add_delta = [](auto _delta, auto entry)
  {
    delta_t new_delta;
    new_delta.delta = _delta;
    new_delta.conf = CONFIDENCE_INIT;
    new_delta.rpl = BERTI_R;
    auto it = std::find_if(std::begin(entry->deltas), std::end(entry->deltas), [](const auto i){
      return (i.delta == 0);
    });
    assert(it != std::end(entry->deltas));
    *it = new_delta;
  };

  if (bertit.find(tag) == bertit.end())
  {
    if constexpr (champsim::debug_print)
      std::cout << " allocating a new entry;";

    if (bertit_queue.size() > BERTI_TABLE_SIZE)
    {
      uint64_t key = bertit_queue.front();
      berti_table *entry = bertit[key];

      if constexpr (champsim::debug_print)
        std::cout << " removing tag: " << std::hex << key << std::dec << ";";

      delete entry;
      bertit.erase(bertit_queue.front());
      bertit_queue.pop();
    }

    bertit_queue.push(tag);
    assert((bertit.size() <= BERTI_TABLE_SIZE) && "Tracking too much tags");

    berti_table *entry = new berti_table;
    entry->conf = CONFIDENCE_INC;

    add_delta(delta, entry);

    if constexpr (champsim::debug_print)
      std::cout << " confidence: " << CONFIDENCE_INIT << std::endl;

    bertit.insert(std::make_pair(tag, entry));
    return;
  }

  berti_table *entry = bertit[tag];

  for (auto &i: entry->deltas)
  {
    if (i.delta == delta)
    {
      i.conf += CONFIDENCE_INC;
      if (i.conf > CONFIDENCE_MAX) i.conf = CONFIDENCE_MAX;

      if constexpr (champsim::debug_print)
        std::cout << " confidence: " << i.conf << std::endl;
      return;
    }
  }

  auto ssize = std::count_if(std::begin(entry->deltas), std::end(entry->deltas),[](const auto i){
    return i.delta != 0;
  });

  if (ssize < size)
  {
    add_delta(delta, entry);
    assert((std::size(entry->deltas) <= size) && "I remember too much deltas");
    return;
  }

  // Evict lowest-confidence delta
  std::sort(std::begin(entry->deltas), std::end(entry->deltas), compare_rpl);
  if (entry->deltas.front().rpl == BERTI_R || entry->deltas.front().rpl == BERTI_L2R) 
  {
    if constexpr (champsim::debug_print)
      std::cout << " replaced_delta: " << entry->deltas.front().delta << std::endl;
    entry->deltas.front().delta = delta;
    entry->deltas.front().conf = CONFIDENCE_INIT;
    entry->deltas.front().rpl = BERTI_R;
  }
}

uint8_t berti_cheri::get(uint64_t tag, std::vector<delta_t> &res)
{
  if constexpr (champsim::debug_print)
  {
    std::cout << "[BERTI_CHERI] " << __func__ << " tag: " << std::hex << tag;
    std::cout << std::dec;
  }

  if (!bertit.count(tag))
  {
    if constexpr (champsim::debug_print)
      std::cout << " TAG NOT FOUND" << std::endl;
    no_found_berti++;
    return 0;
  }
  found_berti++;

  if constexpr (champsim::debug_print) std::cout << std::endl;

  berti_table *entry = bertit[tag];

  for (auto &i: entry->deltas)
    if (i.delta != 0 && i.rpl != BERTI_R) res.push_back(i);

  if (res.empty() && entry->conf >= LAUNCH_MIDDLE_CONF)
  {
    for (auto &i: entry->deltas)
    {
      if (i.delta != 0)
      {
        delta_t new_delta;
        new_delta.delta = i.delta;
        if (i.conf > CONFIDENCE_MIDDLE_L1) new_delta.rpl = BERTI_L1;
        else if (i.conf > CONFIDENCE_MIDDLE_L2) new_delta.rpl = BERTI_L2;
        else continue;
        res.push_back(new_delta);
      }
    }
  }

  std::sort(std::begin(res), std::end(res), compare_greater_delta);
  return 1;
}

void berti_cheri::find_and_update(uint64_t latency, uint64_t tag,  uint64_t cycle, int64_t cap_cl_offset)
{ 
  /* cap_cl_offset is the capability-relative cache-line offset of the
   current access.  History entries also store cap-relative offsets,
   so the delta is simply current_offset - history_offset. */
  uint64_t tags[HISTORY_TABLE_WAYS];
  uint64_t addr[HISTORY_TABLE_WAYS]; // cap-relative offsets from history
  uint16_t num_on_time = 0;

  num_on_time = historyt[me]->get(latency, tag,
      static_cast<uint64_t>(cap_cl_offset), tags, addr, cycle);

  for (uint32_t i = 0; i < num_on_time; i++)
  {
    if (i == 0) increase_conf_tag(tag);

    // Delta between cap-relative offsets
    int64_t stride = cap_cl_offset - static_cast<int64_t>(addr[i]);

    if ((std::abs(stride) < (1 << DELTA_MASK))) add(tags[i], stride); 
  }
}

bool berti_cheri::compare_rpl(delta_t a, delta_t b)
{
  if (a.rpl == BERTI_R && b.rpl != BERTI_R) return 1;
  else if (b.rpl == BERTI_R && a.rpl != BERTI_R) return 0;
  else if (a.rpl == BERTI_L2R && b.rpl != BERTI_L2R) return 1;
  else if (b.rpl == BERTI_L2R && a.rpl != BERTI_L2R) return 0;
  else
  {
    if (a.conf < b.conf) return 1;
    else return 0;
  }
}

bool berti_cheri::compare_greater_delta(delta_t a, delta_t b)
{
  if (a.rpl == BERTI_L1 && b.rpl != BERTI_L1) return 1;
  else if (a.rpl != BERTI_L1 && b.rpl == BERTI_L1) return 0;
  else
  {
    if (a.rpl == BERTI_L2 && b.rpl != BERTI_L2) return 1;
    else if (a.rpl != BERTI_L2 && b.rpl == BERTI_L2) return 0;
    else
    {
      if (a.rpl == BERTI_L2R && b.rpl != BERTI_L2R) return 1;
      if (a.rpl != BERTI_L2R && b.rpl == BERTI_L2R) return 0;
      else
      {
        if (std::abs(a.delta) < std::abs(b.delta)) return 1;
        return 0;
      }
    }
  }
}

uint64_t berti_cheri::ip_hash(uint64_t ip)
{
#ifdef ENTANGLING_HASH
  ip = ip ^ (ip >> 2) ^ (ip >> 5);
#endif
  return ip;
}

uint64_t berti_cheri::combined_tag(uint64_t ip_val, const champsim::capability& cap)
{
  uint64_t ih = ip_hash(ip_val) & IP_MASK;
  uint64_t ch = cheri::hash_capability(cap) & IP_MASK;
  return (ih ^ ch) & IP_MASK;
}

/******************************************************************************/
/*                        Cache Functions                                     */
/******************************************************************************/
void berti_cheri::prefetcher_initialize() 
{
  uint64_t latency_table_size = intern_->get_mshr_size();
  for (auto const &i : intern_->get_rq_size()) latency_table_size += i;
  for (auto const &i : intern_->get_wq_size()) latency_table_size += i;
  for (auto const &i : intern_->get_pq_size()) latency_table_size += i;

  latencyt.push_back(new LatencyTable(latency_table_size));
  scache.push_back(new ShadowCache(intern_->NUM_SET, intern_->NUM_WAY));
  historyt.push_back(new HistoryTable());

  me = others;
  others++;

  std::cout << "Berti-CHERI Prefetcher (capability-aware)" << std::endl;
  std::cout << "BERTI IP MASK " << std::hex << IP_MASK << std::dec << std::endl;
}

void berti_cheri::prefetcher_cycle_operate()
{}

uint32_t berti_cheri::prefetcher_cache_operate(champsim::address addr, champsim::address ip, uint8_t cache_hit, bool useful_prefetch, access_type type, uint32_t metadata_in)
{
  LatencyTable* tlatencyt = latencyt[me];
  ShadowCache* tscache    = scache[me];
  HistoryTable* thistoryt = historyt[me];

  champsim::block_number line_addr{addr};
   
  if (line_addr.to<uint64_t>() == 0) return metadata_in;

  auto cap = intern_->get_authorizing_capability();

  if (!cheri::is_tag_valid(cap))
    return metadata_in;
  

  if (!cheri::has_prefetchable_range(cap))
  {
    stat_too_small_cap++;
    return metadata_in;
  }

  uint64_t cap_base_val = cap.base.to<uint64_t>();
  int64_t cap_cl_offset = cheri::lines_from_cap_base(cap);

  // Combined tag: hash(IP) XOR hash(capability identity)
  uint64_t ctag = combined_tag(ip.to<uint64_t>(), cap);

  if constexpr (champsim::debug_print) 
  {
    std::cout << "[BERTI_CHERI] operate";
    std::cout << " ip: " << std::hex << ip;
    std::cout << " addr: " << addr;
    std::cout << " cap_base: " << cap_base_val;
    std::cout << std::dec << " cap_cl_off: " << cap_cl_offset;
    std::cout << " ctag: " << std::hex << ctag << std::dec << std::endl;
  }

  // ---- Training ----
  if (!cache_hit)
  {
    if constexpr (champsim::debug_print) 
      std::cout << "[BERTI_CHERI] cache miss" << std::endl;

    tlatencyt->add(line_addr.to<uint64_t>(), ctag, false,
                   intern_->current_cycle(), cap_base_val);
    thistoryt->add(ctag, static_cast<uint64_t>(cap_cl_offset),
                   intern_->current_cycle());
  }
  else if (cache_hit && tscache->is_pf(line_addr.to<uint64_t>()))
  {
    if constexpr (champsim::debug_print)
      std::cout << "[BERTI_CHERI] hit on prefetched line" << std::endl;

    tscache->set_pf(line_addr.to<uint64_t>(), false);

    uint64_t latency = tscache->get_latency(line_addr.to<uint64_t>());
    if (latency > LAT_MASK) latency = 0;

    find_and_update(latency, ctag,
                    intern_->current_cycle() & TIME_MASK, cap_cl_offset);
    thistoryt->add(ctag, static_cast<uint64_t>(cap_cl_offset),
                   intern_->current_cycle() & TIME_MASK);
  }
  else
  {
    if constexpr (champsim::debug_print) 
      std::cout << "[BERTI_CHERI] cache hit (not prefetched)" << std::endl;
  }

  // ---- Prediction: issue prefetch requests ----
  std::vector<delta_t> deltas(BERTI_TABLE_DELTA_SIZE);
  get(ctag, deltas);

  bool first_issue = true;
  for (auto i: deltas)
  {
    // Compute target offset within the capability
    int64_t target_offset = cap_cl_offset + i.delta;

    // Quick negative-offset rejection
    if (target_offset < 0)
    {
      stat_pf_bounded_by_cap++;
      continue;
    }

    // Reconstruct target VA from cap base + offset
    uint64_t target_va = cap_base_val +
        (static_cast<uint64_t>(target_offset) << LOG2_BLOCK_SIZE);
    champsim::address p_addr{target_va};
    champsim::block_number p_b_addr{p_addr};

    // Skip if already in the latency table (outstanding request)
    if (tlatencyt->get(p_b_addr.to<uint64_t>())) continue;

    // If this delta has no confidence yet, stop issuing
    if (i.rpl == BERTI_R) return metadata_in;

    if (p_addr.to<uint64_t>() == 0) continue;

    // ---- CHERI bounds check (replaces page-boundary check) ----
    if (!cheri::prefetch_safe(p_addr, cap))
    {
      stat_pf_bounded_by_cap++;
      continue;
    }

    // Track cross-page prefetches within capability for stats
    if (champsim::page_number{p_addr} != champsim::page_number{addr})
      stat_cross_page_in_cap++;

    float mshr_load = intern_->get_mshr_occupancy_ratio() * 100;

    bool fill_this_level = (i.rpl == BERTI_L1) && (mshr_load < MSHR_LIMIT);

    if (i.rpl == BERTI_L1 && mshr_load >= MSHR_LIMIT) pf_to_l2_bc_mshr++; 
    if (fill_this_level) pf_to_l1++;
    else pf_to_l2++;

    if (prefetch_line(p_addr, fill_this_level, metadata_in))
    {
      ++average_issued;
      if (first_issue)
      {
        first_issue = false;
        ++average_num;
      }

      if constexpr (champsim::debug_print)
      {
        std::cout << "[BERTI_CHERI] prefetch delta: " << i.delta;
        std::cout << " target_va: " << std::hex << p_addr << std::dec;
        std::cout << " this_level: " << +fill_this_level << std::endl;
      }

      if (fill_this_level)
      {
        if (!tscache->get(p_b_addr.to<uint64_t>()))
        {
          tlatencyt->add(p_b_addr.to<uint64_t>(), ctag, true,
                         intern_->current_cycle(), cap_base_val);
        }
      }
    }
  }

  return metadata_in;
}

uint32_t berti_cheri::prefetcher_cache_fill(champsim::address addr, long set,
    long way, uint8_t prefetch, champsim::address evicted_addr,
    uint32_t metadata_in)
{
  LatencyTable* tlatencyt = latencyt[me];
  ShadowCache* tscache    = scache[me];

  champsim::block_number line_addr{addr};

  // Handle dropped packets
  if (evicted_addr == champsim::address{} && way == intern_->NUM_WAY)
  {
    tlatencyt->del(line_addr.to<uint64_t>());
    return metadata_in;
  }

  // Retrieve the combined tag and cap_base BEFORE del() clears the entry
  uint64_t tag          = tlatencyt->get_tag(line_addr.to<uint64_t>());
  uint64_t cap_base_val = tlatencyt->get_cap_base(line_addr.to<uint64_t>());
  uint64_t cycle        = tlatencyt->del(line_addr.to<uint64_t>()) & TIME_MASK;
  uint64_t latency      = 0;

  if constexpr (champsim::debug_print)
  {
    std::cout << "[BERTI_CHERI] fill addr: " << std::hex << line_addr;
    std::cout << " cycle: " << cycle;
    std::cout << " prefetch: " << +prefetch;
    std::cout << " cap_base: " << cap_base_val << std::dec << std::endl;
  }

  if (cycle != 0 && ((intern_->current_cycle() & TIME_MASK) > cycle))
    latency = (intern_->current_cycle() & TIME_MASK) - cycle;

  if (latency > LAT_MASK)
  {
    latency = 0;
    cant_track_latency++;
  } else
  {
    if (latency != 0)
    {
      if (average_latency.num == 0) average_latency.average = (float) latency;
      else
      {
        average_latency.average = average_latency.average + 
          ((((float) latency) - average_latency.average) / average_latency.num);
      }
      average_latency.num++;
    }
  }

  tscache->add(set, way, line_addr.to<uint64_t>(), prefetch, latency);

  if (latency != 0 && !prefetch && tag != 0 && cap_base_val != 0)
  {
    // Reconstruct cap-relative cache-line offset from the stored cap_base
    int64_t cap_cl_offset = static_cast<int64_t>(
        line_addr.to<uint64_t>() - (cap_base_val >> LOG2_BLOCK_SIZE));

    find_and_update(latency, tag, cycle, cap_cl_offset);
  }

  return metadata_in;
}

void berti_cheri::prefetcher_final_stats()
{
  std::cout << "\nBERTI_CHERI TO_L1: " << pf_to_l1
            << " TO_L2: " << pf_to_l2
            << " TO_L2_BC_MSHR: " << pf_to_l2_bc_mshr << std::endl;
  std::cout << "DETECTED ALIASES: " << scache[me]->aliased_cache_hits << std::endl;

  std::cout << "BERTI_CHERI AVG_LAT: " << average_latency.average
            << " NUM_TRACK_LATENCY: " << average_latency.num
            << " NUM_CANT_TRACK_LATENCY: " << cant_track_latency << std::endl;

  std::cout << "BERTI_CHERI CROSS_PAGE_IN_CAP: " << stat_cross_page_in_cap << std::endl;

  std::cout << "BERTI_CHERI FOUND_BERTI: " << found_berti
            << " NO_FOUND_BERTI: " << no_found_berti << std::endl;

  std::cout << "BERTI_CHERI AVERAGE_ISSUED: "
            << (average_num > 0 ? ((1.0*average_issued)/average_num) : 0.0)
            << std::endl;

  std::cout << "BERTI_CHERI STAT_TOO_SMALL_CAP: " << stat_too_small_cap << std::endl;
  std::cout << "BERTI_CHERI STAT_PF_BOUNDED_BY_CAP: " << stat_pf_bounded_by_cap << std::endl;
}