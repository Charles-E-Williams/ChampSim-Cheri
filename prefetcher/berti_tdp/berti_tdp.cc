#include "berti_tdp.h"
#include <algorithm>

/*
 * Berti: an Accurate Local-Delta Data Prefetcher
 *  
 * 55th ACM/IEEE International Conference on Microarchitecture (MICRO 2022),
 * October 1-5, 2022, Chicago, Illinois, USA.
 * 
 * @Authors: Agustín Navarro-Torres, Biswabandan Panda, J. Alastruey-Benedé, 
 *           Pablo Ibáñez, Víctor Viñals-Yúfera, and Alberto Ros
 * @Manteiners: Agustín Navarro -Torres
 * @Email: agusnt@unizar.es
 * @Date: 22/11/2022
 * 
 * This code is an update from the original code to work with the new version
 * of ChampSim: https://github.com/agusnt/Berti-Artifact
 * 
 * Maybe fine-tuning is required to get the optimal performance/accuracy.
 * 
 * Please note that this version of ChampSim has noticeable differences with 
 * the used for the paper, so results can varies.
 * 
 * Cite this:
 * 
 * A. Navarro-Torres, B. Panda, J. Alastruey-Benedé, P. Ibáñez, 
 * V. Viñals-Yúfera and A. Ros, 
 * "Berti: an Accurate Local-Delta Data Prefetcher," 
 * 2022 55th IEEE/ACM International Symposium on Microarchitecture (MICRO), 
 * 2022, pp. 975-991, doi: 10.1109/MICRO56248.2022.00072.
 * 
 * @INPROCEEDINGS{9923806,  author={Navarro-Torres, Agustín and Panda, 
 * Biswabandan and Alastruey-Benedé, Jesús and Ibáñez, Pablo and Viñals-Yúfera,
 * Víctor and Ros, Alberto},  booktitle={2022 55th IEEE/ACM International 
 * Symposium on Microarchitecture (MICRO)},   title={Berti: an Accurate 
 * Local-Delta Data Prefetcher},   year={2022},  volume={},  number={},  
 * pages={975-991},  doi={10.1109/MICRO56248.2022.00072}}
 */


/******************************************************************************/
/*                      Latency table functions                               */
/******************************************************************************/

std::vector<berti_tdp::HistoryTable*> berti_tdp::historyt;
std::vector<berti_tdp::LatencyTable*> berti_tdp::latencyt;
std::vector<berti_tdp::ShadowCache*> berti_tdp::scache;
uint64_t berti_tdp::others = 0;

uint8_t berti_tdp::LatencyTable::add(uint64_t addr, uint64_t tag, bool pf, uint64_t cycle)
{
  /*
   * Save if possible the new miss into the pqmshr (latency) table
   *
   * Parameters:
   *  - addr: address without cache offset
   *  - access: is theh entry accessed by a demand request
   *  - cycle: time to use in the latency table
   *
   * Return: pf
   */

  if constexpr (champsim::debug_print) 
  {
    std::cout << "[BERTI_LATENCY_TABLE] " << __func__;
    std::cout << " addr: " << std::hex << addr << " tag: " << tag;
    std::cout << " prefetch: " << std::dec << +pf << " cycle: " << cycle;
  }

  latency_table *free;
  free   = nullptr;

  for (int i = 0; i < size; i++)
  {
    // Search if the addr already exists. If it exist we does not have
    // to do nothing more
    if (latencyt[i].addr == addr)
    {
      if constexpr (champsim::debug_print) 
      {
        std::cout << " line already found; find_tag: " << latencyt[i].tag;
        std::cout << " find_pf: " << +latencyt[i].pf << std::endl;
      }
      // latencyt[i].time = cycle;
      latencyt[i].pf   = pf;
      latencyt[i].tag  = tag;
      return latencyt[i].pf;
    }

    // We discover a free space into the latency table, save it for later
    if (latencyt[i].tag == 0) free = &latencyt[i];
  }

  if (free == nullptr) assert(0 && "No free space latency table");

  // We save the new entry into the latency table
  free->addr = addr;
  free->time = cycle;
  free->tag  = tag;
  free->pf   = pf;

  if constexpr (champsim::debug_print) std::cout << " new entry" << std::endl;
  return free->pf;
}

uint64_t berti_tdp::LatencyTable::del(uint64_t addr)
{
  /*
   * Remove the address from the latency table
   *
   * Parameters:
   *  - addr: address without cache offset
   *
   *  Return: the latency of the address
   */

  if constexpr (champsim::debug_print) 
  {
    std::cout << "[BERTI_LATENCY_TABLE] " << __func__;
    std::cout << " addr: " << std::hex << addr;
  }

  for (int i = 0; i < size; i++)
  {
    // Line already in the table
    if (latencyt[i].addr == addr)
    {
      // Calculate latency
      uint64_t time = latencyt[i].time;

      if constexpr (champsim::debug_print)
      {
        std::cout << " tag: " << latencyt[i].tag;
        std::cout << " prefetch: " << std::dec << +latencyt[i].pf;
        std::cout << " cycle: " << latencyt[i].time << std::endl;
      }

      latencyt[i].addr = 0; // Free the entry
      latencyt[i].tag  = 0; // Free the entry
      latencyt[i].time = 0; // Free the entry
      latencyt[i].pf   = 0; // Free the entry

      // Return the latency
      return time;
    }
  }

  // We should always track the misses
  if constexpr (champsim::debug_print) std::cout << " TRANSLATION" << std::endl;
  return 0;
}

uint64_t berti_tdp::LatencyTable::get(uint64_t addr)
{
  /*
   * Return time or 0 if the addr is or is not in the pqmshr (latency) table
   *
   * Parameters:
   *  - addr: address without cache offset
   *
   * Return: time if the line is in the latency table, otherwise 0
   */

  if constexpr (champsim::debug_print)
  {
    std::cout << "[BERTI_LATENCY_TABLE] " << __func__;
    std::cout << " addr: " << std::hex << addr << std::dec;
  }

  for (int i = 0; i < size; i++)
  {
    // Search if the addr already exists
    if (latencyt[i].addr == addr)
    {
      if constexpr (champsim::debug_print)
      {
        std::cout << " time: " << latencyt[i].time << std::endl;
      }
      return latencyt[i].time;
    }
  }

  if constexpr (champsim::debug_print) std::cout << " NOT FOUND" << std::endl;
  return 0;
}

uint64_t berti_tdp::LatencyTable::get_tag(uint64_t addr)
{
  /*
   * Return IP-Tag or 0 if the addr is or is not in the pqmshr (latency) table
   *
   * Parameters:
   *  - addr: address without cache offset
   *
   * Return: ip-tag if the line is in the latency table, otherwise 0
   */

  if constexpr (champsim::debug_print) 
  {
    std::cout << "[BERTI_LATENCY_TABLE] " << __func__;
    std::cout << " addr: " << std::hex << addr;
  }

  for (int i = 0; i < size; i++)
  {
    if (latencyt[i].addr == addr && latencyt[i].tag) // This is the address
    {
      if constexpr (champsim::debug_print) 
      {
        std::cout << " tag: " << latencyt[i].tag << std::endl;
      }
      return latencyt[i].tag;
    }
  }

  if constexpr (champsim::debug_print) std::cout << " NOT_FOUND" << std::endl;
  return 0;
}

/******************************************************************************/
/*                       Shadow Cache functions                               */
/******************************************************************************/
bool berti_tdp::ShadowCache::add(long set, long way, uint64_t addr, bool pf, uint64_t lat)
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

bool berti_tdp::ShadowCache::get(uint64_t addr)
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

void berti_tdp::ShadowCache::set_pf(uint64_t addr, bool pf)
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

bool berti_tdp::ShadowCache::is_pf(uint64_t addr)
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

uint64_t berti_tdp::ShadowCache::get_latency(uint64_t addr)
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
/*                       History Table functions                               */
/******************************************************************************/
void berti_tdp::HistoryTable::add(uint64_t tag, uint64_t addr, uint64_t cycle)
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

  // Save new element into the history table
  history_pointers[set]->tag       = tag;
  history_pointers[set]->time      = cycle & TIME_MASK;
  history_pointers[set]->addr      = addr & ADDR_MASK;

  if constexpr (champsim::debug_print)
  {
    std::cout << "[BERTI_HISTORY_TABLE] " << __func__;
    std::cout << " tag: " << std::hex << tag << " line_addr: " << addr << std::dec;
    std::cout << " cycle: " << cycle << " set: " << set << std::endl;
  }

  if (history_pointers[set] == &historyt[set][ways - 1])
  {
    history_pointers[set] = &historyt[set][0]; // End the cycle
  } else history_pointers[set]++; // Pointer to the next (oldest) entry
}

uint16_t berti_tdp::HistoryTable::get_aux(uint32_t latency, 
    uint64_t tag, uint64_t act_addr, uint64_t *tags, uint64_t *addr, 
    uint64_t cycle)
{
  /*
   * Return an array (by parameter) with all the possible PC that can launch
   * an on-time and late prefetch
   *
   * Parameters:
   *  - tag: PC tag
   *  - latency: latency of the processor
   */

  uint16_t num_on_time = 0;
  uint16_t set = tag & TABLE_SET_MASK;

  if constexpr (champsim::debug_print)
  {
    std::cout << "[BERTI_HISTORY_TABLE] " << __func__;
    std::cout << " tag: " << std::hex << tag << " line_addr: " << addr << std::dec;
    std::cout << " cycle: " << cycle << " set: " << set << std::endl;
  }

  // This is the begin of the simulation
  if (cycle < latency) return num_on_time;

  // The IPs that is launch in this cycle will be able to launch this prefetch
  cycle -= latency; 

  // Pointer to guide
  history_table *pointer = history_pointers[set];

  do
  {
    // Look for the IPs that can launch this prefetch
    if (pointer->tag == tag && pointer->time <= cycle)
    {
      // Test that addr is not duplicated
      if (pointer->addr == act_addr) return num_on_time;

      // This IP can launch the prefetch
      tags[num_on_time] = pointer->tag;
      addr[num_on_time] = pointer->addr;
      num_on_time++;
    }

    if (pointer == historyt[set])
    {
      // We get at the end of the history, we start again
      pointer = &historyt[set][ways - 1];
    } else pointer--;
  } while (pointer != history_pointers[set]);

  return num_on_time;
}

uint16_t berti_tdp::HistoryTable::get(uint32_t latency, uint64_t tag, uint64_t act_addr,
    uint64_t *tags, uint64_t *addr, uint64_t cycle)
{
  /*
   * Return an array (by parameter) with all the possible PC that can launch
   * an on-time and late prefetch
   *
   * Parameters:
   *  - tag: PC tag
   *  - latency: latency of the processor
   *  - on_time_ip (out): ips that can launch an on-time prefetch
   *  - on_time_addr (out): addr that can launch an on-time prefetch
   *  - num_on_time (out): number of ips that can launch an on-time prefetch
   */

  act_addr &= ADDR_MASK;

  uint16_t num_on_time = get_aux(latency, tag, act_addr, tags, addr, cycle & TIME_MASK);

  // We found on-time prefetchs
  return num_on_time;
}

/******************************************************************************/
/*                        Berti table functions                               */
/******************************************************************************/
void berti_tdp::increase_conf_tag(uint64_t tag)
{
  /*
   * Increase the global confidence of the deltas associated to the tag
   *
   * Parameters:
   *  tag : tag to find
   */
  if constexpr (champsim::debug_print)
    std::cout << "[BERTI_BERTI] " << __func__ << " tag: " << std::hex << tag << std::dec;

  if (bertit.find(tag) == bertit.end())
  {
    // Tag not found
    if constexpr (champsim::debug_print) 
      std::cout << " TAG NOT FOUND" << std::endl;

    return;
  }

  // Get the entries and the deltas

  bertit[tag]->conf += CONFIDENCE_INC;

  if constexpr (champsim::debug_print) 
    std::cout << " global_conf: " << bertit[tag]->conf;


  if (bertit[tag]->conf == CONFIDENCE_MAX) 
  {

    // Max confidence achieve
    for (auto &i: bertit[tag]->deltas)
    {
      // Set bits to prefetch level
      if (i.conf > CONFIDENCE_L1)i.rpl = BERTI_L1;
      else if (i.conf > CONFIDENCE_L2) i.rpl = BERTI_L2;
      else if (i.conf > CONFIDENCE_L2R) i.rpl = BERTI_L2R;
      else i.rpl = BERTI_R;

      if constexpr (champsim::debug_print) 
      {
        std::cout << "Delta: " << i.delta;
        std::cout << " Conf: "  << i.conf << " Level: " << +i.rpl;
        std::cout << "|";
      }

      i.conf = 0; // Reset confidence
    }

    bertit[tag]->conf = 0; // Reset global confidence
  }

  if constexpr (champsim::debug_print) std::cout << std::endl;
}

void berti_tdp::add(uint64_t tag, int64_t delta)
{
  /*
   * Save the new information into the history table
   *
   * Parameters:
   *  - tag: PC tag
   *  - cpu: actual cpu
   *  - stride: actual cpu
   */
  if constexpr (champsim::debug_print)
  {
    std::cout << "[BERTI_BERTI] " << __func__;
    std::cout << " tag: " << std::hex << tag << std::dec;
    std::cout << " delta: " << delta;
  }

  auto add_delta = [](auto delta, auto entry)
  {
    // Lambda function to add a new element
    delta_t new_delta;
    new_delta.delta = delta;
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

    // We are not tracking this tag
    if (bertit_queue.size() > BERTI_TABLE_SIZE)
    {
      // FIFO replacent algorithm
      uint64_t key = bertit_queue.front();
      berti_table *entry = bertit[key];

      if constexpr (champsim::debug_print)
        std::cout << " removing tag: " << std::hex << key << std::dec << ";";

      delete entry; // Free previous entry

      bertit.erase(bertit_queue.front());
      bertit_queue.pop();
    }

    bertit_queue.push(tag); // Add new tag
    assert((bertit.size() <= BERTI_TABLE_SIZE) && "Tracking too much tags");

    // Confidence IP
    berti_table *entry = new berti_table;
    entry->conf = CONFIDENCE_INC;

    // Saving the new stride
    add_delta(delta, entry);

    if constexpr (champsim::debug_print)
      std::cout << " confidence: " << CONFIDENCE_INIT << std::endl;

    // Save the new tag
    bertit.insert(std::make_pair(tag, entry));
    return;
  }

  // Get the delta
  berti_table *entry  = bertit[tag];

  for (auto &i: entry->deltas)
  {
    if (i.delta == delta)
    {
      // We already track the delta
      i.conf += CONFIDENCE_INC;

      if (i.conf > CONFIDENCE_MAX) i.conf = CONFIDENCE_MAX;

      if constexpr (champsim::debug_print)
        std::cout << " confidence: " << i.conf << std::endl;

      return;
    }
  }

  // We have space to add a new entry
  auto ssize = std::count_if(std::begin(entry->deltas), std::end(entry->deltas),[](const auto i){
    return i.delta != 0;
  });

  if (ssize < size)
  {
    add_delta(delta, entry);
    assert((std::size(entry->deltas) <= size) && "I remember too much deltas");
    return;
  }

  // We find the delta with less confidence
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

uint8_t berti_tdp::get(uint64_t tag, std::vector<delta_t> &res)
{
  /*
   * Save the new information into the history table
   *
   * Parameters:
   *  - tag: PC tag
   *
   * Return: the stride to prefetch
   */
  if constexpr (champsim::debug_print)
  {
    std::cout << "[BERTI_BERTI] " << __func__ << " tag: " << std::hex << tag;
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

  // We found the tag
  berti_table *entry  = bertit[tag];

  for (auto &i: entry->deltas) if (i.delta != 0 && i.rpl != BERTI_R) res.push_back(i);

  if (res.empty() && entry->conf >= LAUNCH_MIDDLE_CONF)
  {
    // We do not find any delta, so we will try to launch with small confidence
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

  // Sort the entries
  std::sort(std::begin(res), std::end(res), compare_greater_delta);
  return 1;
}

void berti_tdp::find_and_update(uint64_t latency, uint64_t tag, uint64_t cycle, 
    uint64_t line_addr)
{ 
  // We were tracking this miss
  uint64_t tags[HISTORY_TABLE_WAYS];
  uint64_t addr[HISTORY_TABLE_WAYS];
  uint16_t num_on_time = 0;

  // Get the IPs that can launch a prefetch
  num_on_time = historyt[me]->get(latency, tag, line_addr, tags, addr, cycle);

  for (uint32_t i = 0; i < num_on_time; i++)
  {
    // Increase conf tag
    if (i == 0) increase_conf_tag(tag);

    // Add information into berti table
    int64_t stride;
    line_addr &= ADDR_MASK;

    // Usually applications go from lower to higher memory position.
    // The operation order is important (mainly because we allow
    // negative strides)
    stride = (int64_t) (line_addr - addr[i]);

    if ((std::abs(stride) < (1 << DELTA_MASK))) add(tags[i], stride); 
  }
}

bool berti_tdp::compare_rpl(delta_t a, delta_t b)
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

bool berti_tdp::compare_greater_delta(delta_t a, delta_t b)
{
  // Sorted stride when the confidence is full
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

uint64_t berti_tdp::ip_hash(uint64_t ip)
{
  /*
   * IP hash function
   */
#ifdef HASH_ORIGINAL
  ip = ((ip >> 1) ^ (ip >> 4)); // Original one
#endif
  // IP hash from here: http://burtleburtle.net/bob/hash/integer.html
#ifdef THOMAS_WANG_HASH_1
  ip = (ip ^ 61) ^ (ip >> 16);
  ip = ip + (ip << 3);
  ip = ip ^ (ip >> 4);
  ip = ip * 0x27d4eb2d;
  ip = ip ^ (ip >> 15);
#endif
#ifdef THOMAS_WANG_HASH_2
  ip = (ip+0x7ed55d16) + (ip<<12);
  ip = (ip^0xc761c23c) ^ (ip>>19);
  ip = (ip+0x165667b1) + (ip<<5);
  ip = (ip+0xd3a2646c) ^ (ip<<9);
  ip = (ip+0xfd7046c5) + (ip<<3);
  ip = (ip^0xb55a4f09) ^ (ip>>16);
#endif
#ifdef THOMAS_WANG_HASH_3
  ip -= (ip<<6);
  ip ^= (ip>>17);
  ip -= (ip<<9);
  ip ^= (ip<<4);
  ip -= (ip<<3);
  ip ^= (ip<<10);
  ip ^= (ip>>15);
#endif
#ifdef THOMAS_WANG_HASH_4
  ip += ~(ip<<15);
  ip ^=  (ip>>10);
  ip +=  (ip<<3);
  ip ^=  (ip>>6);
  ip += ~(ip<<11);
  ip ^=  (ip>>16);
#endif
#ifdef THOMAS_WANG_HASH_5
  ip = (ip+0x479ab41d) + (ip<<8);
  ip = (ip^0xe4aa10ce) ^ (ip>>5);
  ip = (ip+0x9942f0a6) - (ip<<14);
  ip = (ip^0x5aedd67d) ^ (ip>>3);
  ip = (ip+0x17bea992) + (ip<<7);
#endif
#ifdef THOMAS_WANG_HASH_6
  ip = (ip^0xdeadbeef) + (ip<<4);
  ip = ip ^ (ip>>10);
  ip = ip + (ip<<7);
  ip = ip ^ (ip>>13);
#endif
#ifdef THOMAS_WANG_HASH_7
  ip = ip ^ (ip>>4);
  ip = (ip^0xdeadbeef) + (ip<<5);
  ip = ip ^ (ip>>11);
#endif
#ifdef THOMAS_WANG_NEW_HASH
  ip ^= (ip >> 20) ^ (ip >> 12);
  ip = ip ^ (ip >> 7) ^ (ip >> 4);
#endif
#ifdef THOMAS_WANG_HASH_HALF_AVALANCHE
  ip = (ip+0x479ab41d) + (ip<<8);
  ip = (ip^0xe4aa10ce) ^ (ip>>5);
  ip = (ip+0x9942f0a6) - (ip<<14);
  ip = (ip^0x5aedd67d) ^ (ip>>3);
  ip = (ip+0x17bea992) + (ip<<7);
#endif
#ifdef THOMAS_WANG_HASH_FULL_AVALANCHE
  ip = (ip+0x7ed55d16) + (ip<<12);
  ip = (ip^0xc761c23c) ^ (ip>>19);
  ip = (ip+0x165667b1) + (ip<<5);
  ip = (ip+0xd3a2646c) ^ (ip<<9);
  ip = (ip+0xfd7046c5) + (ip<<3);
  ip = (ip^0xb55a4f09) ^ (ip>>16);
#endif
#ifdef THOMAS_WANG_HASH_INT_1
  ip -= (ip<<6);
  ip ^= (ip>>17);
  ip -= (ip<<9);
  ip ^= (ip<<4);
  ip -= (ip<<3);
  ip ^= (ip<<10);
  ip ^= (ip>>15);
#endif
#ifdef THOMAS_WANG_HASH_INT_2
  ip += ~(ip<<15);
  ip ^=  (ip>>10);
  ip +=  (ip<<3);
  ip ^=  (ip>>6);
  ip += ~(ip<<11);
  ip ^=  (ip>>16);
#endif
#ifdef ENTANGLING_HASH
  ip = ip ^ (ip >> 2) ^ (ip >> 5);
#endif
#ifdef FOLD_HASH
  uint64_t hash = 0;
  while(ip) {hash ^= (ip & IP_MASK); ip >>= SIZE_IP_MASK;}
  ip = hash;
#endif
  return ip; // No IP hash
}

/******************************************************************************/
/*                        Cache Functions                                     */
/******************************************************************************/
void berti_tdp::prefetcher_initialize() 
{
  
  // Calculate latency table size
  uint64_t latency_table_size = intern_->get_mshr_size();
  for (auto const &i : intern_->get_rq_size()) latency_table_size += i;
  for (auto const &i : intern_->get_wq_size()) latency_table_size += i;
  for (auto const &i : intern_->get_pq_size()) latency_table_size += i;


  //fix this
  latencyt.push_back(new LatencyTable(latency_table_size));
  scache.push_back(new ShadowCache(intern_->NUM_SET, intern_->NUM_WAY));
  historyt.push_back(new HistoryTable());

  me = others;
  others++;

  std::cout << "Berti Prefetcher" << std::endl;
  std::cout << "Berti-TDP: ENABLE=" << TDP_ENABLE << " CHT=" << TDP_CHT_SIZE << " PDT=" << TDP_PDT_SIZE << "x" << TDP_PDT_DELTAS
            << " EPOCH=" << TDP_EPOCH << " TH_L1=" << TDP_TH_L1 << " TH_L2=" << TDP_TH_L2 << " SIGHTQ=" << TDP_SIGHTQ
            << " SIGHT_PER_CYCLE=" << TDP_SIGHT_PER_CYCLE << " SIGHT_FILL=" << TDP_SIGHT_FILL << " SIGHT_LOAD_HIT=" << TDP_SIGHT_LOAD_HIT
            << " KEY_LEN=" << TDP_KEY_LEN << " BOUNDS=" << TDP_BOUNDS << " NOLEARN=" << TDP_NOLEARN << " TIMELY=" << TDP_TIMELY << std::endl;
  //intern_->internal_PQ.set_timeout(1500);

# ifdef NO_CROSS_PAGE
  std::cout << "No Crossing Page" << std::endl;
# endif
#ifdef HASH_ORIGINAL
  std::cout << "BERTI HASH ORIGINAL" << std::endl;
#endif
#ifdef THOMAS_WANG_HASH_1
  std::cout << "BERTI HASH 1" << std::endl;
#endif
#ifdef THOMAS_WANG_HASH_2
  std::cout << "BERTI HASH 2" << std::endl;
#endif
#ifdef THOMAS_WANG_HASH_3
  std::cout << "BERTI HASH 3" << std::endl;
#endif
#ifdef THOMAS_WANG_HASH_4
  std::cout << "BERTI HASH 4" << std::endl;
#endif
#ifdef THOMAS_WANG_HASH_5
  std::cout << "BERTI HASH 5" << std::endl;
#endif
#ifdef THOMAS_WANG_HASH_6
  std::cout << "BERTI HASH 6" << std::endl;
#endif
#ifdef THOMAS_WANG_HASH_7
  std::cout << "BERTI HASH 7" << std::endl;
#endif
#ifdef THOMAS_WANG_NEW_HASH
  std::cout << "BERTI HASH NEW" << std::endl;
#endif
#ifdef THOMAS_WANG_HASH_HALF_AVALANCHE
  std::cout << "BERTI HASH HALF AVALANCHE" << std::endl;
#endif
#ifdef THOMAS_WANG_HASH_FULL_AVALANCHE
  std::cout << "BERTI HASH FULL AVALANCHE" << std::endl;
#endif
#ifdef THOMAS_WANG_HASH_INT_1
  std::cout << "BERTI HASH INT 1" << std::endl;
#endif
#ifdef THOMAS_WANG_HASH_INT_2
  std::cout << "BERTI HASH INT 2" << std::endl;
#endif
#ifdef ENTANGLING_HASH
  std::cout << "BERTI HASH ENTANGLING" << std::endl;
#endif
#ifdef FOLD_HASH
  std::cout << "BERTI HASH FOLD" << std::endl;
#endif
  std::cout << "BERTI IP MASK " << std::hex << IP_MASK << std::dec << std::endl;
 
}

void berti_tdp::prefetcher_cycle_operate()
{
#if TDP_ENABLE
  drain_sightings();
  if (kn.interval && intern_->current_cycle() >= next_interval) {
    if (next_interval != 0) print_interval();
    next_interval = intern_->current_cycle() + kn.interval;
  }
#endif
}

uint32_t berti_tdp::prefetcher_cache_operate(champsim::address addr, champsim::address ip,
                                        uint32_t cpu, champsim::capability cap,
                                        uint8_t cache_hit, bool useful_prefetch, access_type type,
                                        uint32_t metadata_in, uint32_t metadata_hit)
{
  // We select the structures for every cpu
  LatencyTable* tlatencyt = latencyt[me];
  ShadowCache* tscache = scache[me];
  HistoryTable* thistoryt = historyt[me];

  champsim::block_number line_addr{addr}; // Line addr
   
  if (line_addr.to<uint64_t>() == 0) return metadata_in;
  
  if constexpr (champsim::debug_print) 
  {
    std::cout << "[BERTI] operate";
    std::cout << " ip: " << std::hex << ip;
    std::cout << " full_address: " << addr;
    std::cout << " line_address: " << line_addr << std::dec << std::endl ;
  }

  uint64_t ip_hash = this->ip_hash(ip.to<uint64_t>()) & IP_MASK;

#if TDP_ENABLE
  if (type == access_type::LOAD && !cache_hit) l2u.demand_miss(line_addr.to<uint64_t>()); // statistics only
  // Tag-directed prefetching (TDP): train on dereferences (tagged demand loads that miss, or hit a TDP-prefetched line)
  if (type == access_type::LOAD && cap.tag)
  {
    const bool tdp_hit = cache_hit && useful_prefetch && (metadata_hit & MD_TDP);
    if (!cache_hit || tdp_hit) train_deref(line_addr.to<uint64_t>(), cap, !cache_hit);
    st_tdp_useful += tdp_hit;
#if TDP_SIGHT_LOAD_HIT
    if (cache_hit)
    {
      // The slot the load reads: the core puts the cursor at the effective address
      const uint64_t va = cap.base.to<uint64_t>() + cap.offset.to<uint64_t>();
      if ((va >> LOG2_BLOCK_SIZE) == line_addr.to<uint64_t>())
        scan_line(va & ~uint64_t{BLOCK_SIZE - 1}, ip_hash, SRC_LOAD_HIT, cpu, 1u << ((va >> cheri::CAP_ALIGNMENT_BITS) & (cheri::CAPS_PER_CL - 1)));
    }
#endif
  }
  // Berti's prefetches carry the IP they are credited to: the demand's, or a prefetch request's trigger's
  uint32_t md_out = 0;
  if (type != access_type::PREFETCH) md_out = md_make(ip_hash, false);
  else if ((metadata_in & MD_VALID) && !(metadata_in & MD_TDP)) md_out = md_make(metadata_in & MD_IP_MASK, false);
#else
  const uint32_t md_out = metadata_in;
#endif

  if (!cache_hit) // This is a miss
  {
    if constexpr (champsim::debug_print) 
      std::cout << "[BERTI] operate cache miss" << std::endl;

    tlatencyt->add(line_addr.to<uint64_t>(), ip_hash, false, intern_->current_cycle()); // Add @ to latency
    thistoryt->add(ip_hash, line_addr.to<uint64_t>(), intern_->current_cycle()); // Add to the table
  } else if (cache_hit && tscache->is_pf(line_addr.to<uint64_t>())) // Hit bc prefetch
  {
    if constexpr (champsim::debug_print)
      std::cout << "[BERTI] operate cache hit because of pf" << std::endl;

    tscache->set_pf(line_addr.to<uint64_t>(), false);

    uint64_t latency = tscache->get_latency(line_addr.to<uint64_t>()); // Get latency

    if (latency > LAT_MASK) latency = 0;

    find_and_update(latency, ip_hash, intern_->current_cycle() & TIME_MASK, line_addr.to<uint64_t>());
    thistoryt->add(ip_hash, line_addr.to<uint64_t>(), intern_->current_cycle() & TIME_MASK);
  } else
  {
    if constexpr (champsim::debug_print) 
      std::cout << "[BERTI] operate cache hit" << std::endl;
  }

  std::vector<delta_t> deltas(BERTI_TABLE_DELTA_SIZE);
  get(ip_hash, deltas);

  bool first_issue = true;
  for (auto i: deltas)
  {
    champsim::address p_addr{line_addr + i.delta};
    champsim::block_number p_b_addr = line_addr + i.delta;

    if (tlatencyt->get(p_b_addr.to<uint64_t>())) continue;
    if (i.rpl == BERTI_R) return metadata_in;
    if (p_addr.to<uint64_t>() == 0) continue;

    if (champsim::page_number{p_addr} != champsim::page_number{addr})
    {
      cross_page++;
# ifdef NO_CROSS_PAGE
      // We do not cross virtual page
      continue;
# endif
    } else no_cross_page++;

    float mshr_load = intern_->get_mshr_occupancy_ratio() * 100;

    bool fill_this_level = (i.rpl == BERTI_L1) && (mshr_load < MSHR_LIMIT);

    if (i.rpl == BERTI_L1 && mshr_load >= MSHR_LIMIT) pf_to_l2_bc_mshr++; 
    if (fill_this_level) pf_to_l1++;
    else pf_to_l2++;
    if (prefetch_line(p_addr, fill_this_level, md_out))
    {
      ++average_issued;
      if (first_issue)
      {
        first_issue = false;
        ++average_num;
      }

      if constexpr (champsim::debug_print)
      {
        std::cout << "[BERTI] operate prefetch delta: " << i.delta;
        std::cout << " p_addr: " << std::hex << p_addr << std::dec;
        std::cout << " this_level: " << +fill_this_level << std::endl;
      }

      if (fill_this_level)
      {
        if (!tscache->get(p_b_addr.to<uint64_t>()))
        {
          tlatencyt->add(p_b_addr.to<uint64_t>(), ip_hash, true, intern_->current_cycle());
        }
      }
    }
  }

  return metadata_in;
}

uint32_t berti_tdp::prefetcher_cache_fill(champsim::address addr, champsim::address ip, uint32_t cpu, champsim::capability cap, bool useless, long set, long way,
                                      bool prefetch, champsim::address evicted_addr, champsim::capability evicted_cap, uint32_t metadata_in,
                                      uint32_t metadata_evict, uint32_t cpu_evict)
{
  // We select the structures for every cpu
  LatencyTable* tlatencyt = latencyt[me];
  ShadowCache* tscache = scache[me];
  // HistoryTable* thistoryt = historyt[me];



  champsim::block_number line_addr{addr}; // Line addr

  //added this to handle dropped packets
  if(evicted_addr == champsim::address{} && way == intern_->NUM_WAY) {
    tlatencyt->del(line_addr.to<uint64_t>());
    return metadata_in;
  }

#if TDP_ENABLE
  // Tag-directed prefetching (TDP): the pointers in the arriving line are sightings
  st_tdp_useless += (useless && (metadata_evict & MD_TDP));
  if (prefetch && (metadata_in & MD_TDP)) st_tdp_fills++;
#if TDP_SIGHT_FILL
  {
    const uint64_t line_va = line_addr.to<uint64_t>() << LOG2_BLOCK_SIZE;
    if (!prefetch)
    {
      if (ip.to<uint64_t>() != 0) scan_line(line_va, this->ip_hash(ip.to<uint64_t>()) & IP_MASK, SRC_DEMAND_FILL, cpu);
    } else if ((metadata_in & MD_VALID) && !(metadata_in & MD_TDP))
      scan_line(line_va, metadata_in & MD_IP_MASK, SRC_PF_FILL, cpu);
  }
#endif
#endif

  uint64_t tag     = tlatencyt->get_tag(line_addr.to<uint64_t>());
  uint64_t cycle   = tlatencyt->del(line_addr.to<uint64_t>()) & TIME_MASK;
  uint64_t latency = 0;

  if constexpr (champsim::debug_print)
  {
    std::cout << "[BERTI] fill addr: " << std::hex << line_addr;
    std::cout << " event_cycle: " << cycle;
    std::cout << " prefetch: " << +prefetch << std::endl;
    std::cout << " latency: " << latency << std::endl;
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
      // Calculate average latency
      if (average_latency.num == 0) average_latency.average = (float) latency;
      else
      {
        average_latency.average = average_latency.average + 
          ((((float) latency) - average_latency.average) / average_latency.num);
      }
      average_latency.num++;
    }
  }

#if TDP_ENABLE && TDP_TIMELY
  // Dereference misses of this line: credit their delta if the sighting came at least one fetch latency earlier
  for (auto it = pending.begin(); it != pending.end();)
  {
    if (it->line != line_addr.to<uint64_t>()) { ++it; continue; }
    if (latency == 0) st_nolat_dropped++; // latency not tracked: Berti does not train on such fills either
    else if (it->gap >= latency) { st_timely_credit++; pdt_train(it->key, it->delta); }
    else st_late_dropped++;
    it = pending.erase(it);
  }
#endif

  // Add to the shadow cache
#if TDP_ENABLE
  // TDP_BERTI_TRAIN=0: a TDP-prefetched line enters the shadow cache as a demand line, so a hit on it does not train Berti
  const bool berti_pf = prefetch && (kn.berti_train || !(metadata_in & MD_TDP));
  st_berti_untrained += (prefetch && !berti_pf);
  tscache->add(set, way, line_addr.to<uint64_t>(), berti_pf, latency);
#else
  tscache->add(set, way, line_addr.to<uint64_t>(), prefetch, latency);
#endif

  if (latency != 0 && !prefetch)
  {
    find_and_update(latency, tag, cycle, line_addr.to<uint64_t>());
  }
  return metadata_in;
}

void berti_tdp::prefetcher_final_stats()
{
  std::cout << "\nBERTI " << "TO_L1: " << pf_to_l1 << " TO_L2: " << pf_to_l2;
  std::cout << " TO_L2_BC_MSHR: " << pf_to_l2_bc_mshr << std::endl;
  std::cout << "DETECTED ALIASES: " << scache[me]->aliased_cache_hits << std::endl;

  std::cout << "BERTI AVG_LAT: ";
  std::cout << average_latency.average << " NUM_TRACK_LATENCY: ";
  std::cout << average_latency.num << " NUM_CANT_TRACK_LATENCY: ";
  std::cout << cant_track_latency << std::endl;

  std::cout << "BERTI CROSS_PAGE " << cross_page;
  std::cout << " NO_CROSS_PAGE: " << no_cross_page << std::endl;

  std::cout << "BERTI";
  std::cout << " FOUND_BERTI: " << found_berti;
  std::cout << " NO_FOUND_BERTI: " << no_found_berti << std::endl;

  std::cout << "BERTI";
  std::cout << " AVERAGE_ISSUED: " << ((1.0*average_issued)/average_num);
  std::cout << std::endl;

#if TDP_ENABLE
  cheri_tdp::print_config(std::cout, th_l2);
  const char* src[NUM_SRC] = {"demand_fill", "berti_pf_fill", "load_hit"};
  std::cout << "TDP LINES_SCANNED";
  for (int i = 0; i < NUM_SRC; i++) std::cout << " " << src[i] << " " << st_lines_scanned[i];
  std::cout << std::endl << "TDP SIGHTINGS";
  for (int i = 0; i < NUM_SRC; i++) std::cout << " " << src[i] << " " << st_sight[i];
  std::cout << " DUPLICATE " << st_sight_dup << " QUEUED " << st_sq_delayed << " QUEUE_DROPPED " << st_sq_dropped << std::endl;
  std::cout << "TDP TAGGED_LOAD_MISSES " << st_tagged_load_miss << " MATCH_CHT " << st_match << " MATCH_CHT_TIMELY " << st_match_timely << std::endl;
  std::cout << "TDP TRAIN miss " << st_train_miss << " tdp_pf_hit " << st_train_tdphit << " EPOCHS " << st_epochs << " STATUS_L1 " << st_status_l1
            << " STATUS_L2 " << st_status_l2 << std::endl;
#if TDP_TIMELY
  std::cout << "TDP TIMELY credited " << st_timely_credit << " late_not_credited " << st_late_dropped << " no_latency " << st_nolat_dropped
            << " pending_overflow " << st_pending_overflow << std::endl;
#endif
  std::cout << "TDP ISSUED L1 " << st_tdp_l1 << " L2 " << st_tdp_l2 << " PQ_FULL " << st_tdp_pq_full << " INFLIGHT_SKIP " << st_tdp_inflight
            << " OOB_SKIP " << st_tdp_oob << std::endl;
  std::cout << "TDP FILLS " << st_tdp_fills << " USEFUL " << st_tdp_useful << " USELESS " << st_tdp_useless << std::endl;
  std::cout << "TDP PREDICTIONS dereferenced " << st_pred_hit << " not_dereferenced " << st_pred_miss << " BUSY_DROP " << st_busy_drop
            << " FB_SKIP all " << st_fb_off << " l2 " << st_fb_l2_off << " BERTI_UNTRAINED_FILLS " << st_berti_untrained << std::endl;
  l2u.print(std::cout);
  for (int i = 0; i < NUM_SRC; i++)
  {
    std::cout << "TDP GAP_LOG2_CYCLES " << src[i];
    for (int b = 0; b < 16; b++) std::cout << " " << st_gap[i][b];
    std::cout << std::endl;
  }
  std::cout << "TDP DELTA_HIST";
  for (int i = 0; i < 2 * TDP_MAX_LINE_DELTA + 1; i++) std::cout << " " << (i - TDP_MAX_LINE_DELTA) << ":" << st_delta_hist[i];
  std::cout << std::endl;
#endif
}

/******************************************************************************/
/*                    Tag-directed prefetching (TDP) (Berti-TDP)                               */
/******************************************************************************/
uint16_t berti_tdp::hash16(uint64_t x)
{
  x ^= x >> 33;
  x *= 0xff51afd7ed558ccdULL;
  x ^= x >> 29;
  return static_cast<uint16_t>(x ^ (x >> 16) ^ (x >> 32) ^ (x >> 48));
}

uint16_t berti_tdp::make_key(uint64_t ip16, [[maybe_unused]] const champsim::capability& c)
{
#if TDP_KEY_LEN
  return hash16((ip16 & 0xFFFF) ^ (static_cast<uint64_t>(hash16(c.length.to<uint64_t>() + 0x9e37)) * 0x9E3779B1ULL));
#else
  return static_cast<uint16_t>(ip16 & 0xFFFF);
#endif
}

berti_tdp::pdt_entry* berti_tdp::pdt_find(uint16_t tag)
{
  for (auto& e : pdt)
    if (e.valid && e.tag == tag) {
      e.lru = ++pdt_clock;
      return &e;
    }
  return nullptr;
}

berti_tdp::pdt_entry* berti_tdp::pdt_alloc(uint16_t tag)
{
  if (auto* e = pdt_find(tag))
    return e;
  auto victim = std::min_element(pdt.begin(), pdt.end(), [](const auto& a, const auto& b) { return a.valid != b.valid ? !a.valid : a.lru < b.lru; });
  *victim = pdt_entry{};
  victim->valid = true;
  victim->tag = tag;
  victim->lru = ++pdt_clock;
  return &*victim;
}

// One more sighting under this key; at the end of an epoch every delta gets its prefetch level from its confidence.
void berti_tdp::pdt_count_sighting(pdt_entry* e)
{
  if (++e->sightings < TDP_EPOCH)
    return;
  st_epochs++;
  for (auto& d : e->d) {
    if (!d.valid)
      continue;
    d.status = (d.conf >= TDP_TH_L1) ? 2 : (d.conf >= th_l2) ? 1 : 0;
    st_status_l1 += (d.status == 2);
    st_status_l2 += (d.status == 1);
    d.conf = 0;
  }
  e->sightings = 0;
  if (kn.feedback && e->acc < cheri_tdp::FEEDBACK_MIN_ISSUE)
    e->acc++;
}

void berti_tdp::pdt_train(uint16_t key, int delta)
{
  pdt_entry* e = pdt_find(key);
  if (e == nullptr)
    return;
  for (auto& d : e->d)
    if (d.valid && d.delta == delta) {
      d.conf = static_cast<uint8_t>(std::min<int>(d.conf + 1, TDP_EPOCH));
      return;
    }
  pdt_delta* victim = nullptr; // a free slot, else the least confident slot without a prefetch level
  for (auto& d : e->d)
    if (!d.valid) {
      victim = &d;
      break;
    }
  if (victim == nullptr)
    for (auto& d : e->d)
      if (d.status == 0 && (victim == nullptr || d.conf < victim->conf))
        victim = &d;
  if (victim == nullptr)
    return;
  *victim = pdt_delta{};
  victim->valid = true;
  victim->delta = static_cast<int8_t>(delta);
  victim->conf = 1;
}

bool berti_tdp::issue_tdp(uint64_t line, bool fill_l1, uint64_t ip16, const champsim::capability& c)
{
  if (latencyt[me]->get(line)) { // already in flight to L1D
    st_tdp_inflight++;
    return false;
  }
  if (!prefetch_line(champsim::address{line << LOG2_BLOCK_SIZE}, fill_l1, md_make(ip16, true), c)) {
    st_tdp_pq_full++;
    return false;
  }
  if (fill_l1) {
    st_tdp_l1++;
    if (!scache[me]->get(line)) // as Berti does for its L1D prefetches: track the latency of the fill
      latencyt[me]->add(line, ip16 ? ip16 : 1, true, intern_->current_cycle());
  } else
    st_tdp_l2++;
  return true;
}

// CHT search (dedup) + record, PDT lookup, issue.
void berti_tdp::process_sighting(const sighting& s)
{
  const auto& c = s.cap;
  const uint64_t cline = (c.base.to<uint64_t>() + c.offset.to<uint64_t>()) >> LOG2_BLOCK_SIZE;
  const uint16_t bh = hash16(c.base.to<uint64_t>());
  if (std::any_of(cht.begin(), cht.end(), [&](const cht_entry& x) { return x.valid && x.base_hash == bh && x.cursor_line == cline; })) {
    st_sight_dup++;
    return; // already recorded: neither a new sighting nor a new issue
  }
  st_sight[s.source]++;
  const uint16_t key = make_key(s.ip16, c);
  cht_entry& ce = cht[cht_ptr];
  cht_ptr = (cht_ptr + 1) % TDP_CHT_SIZE;
  if (ce.valid && ce.issued) { // prefetched deltas of the overwritten sighting that were never dereferenced
    const int n = static_cast<int>(std::bitset<16>(ce.issued).count());
    st_pred_miss += static_cast<uint64_t>(n);
    feedback(ce.key, -n);
  }
  ce = cht_entry{true, bh, cline, key, 0, intern_->current_cycle(), s.source, 0};

  const bool mshr_ok = intern_->get_mshr_occupancy_ratio() * 100 < MSHR_LIMIT;
  const uint64_t own_line = s.line_va >> LOG2_BLOCK_SIZE;
#if TDP_NOLEARN
  if (cline != own_line && cheri::overlaps_bounds(champsim::block_number{champsim::address{cline << LOG2_BLOCK_SIZE}}, c.base, cheri::capability_top(c)))
    issue_tdp(cline, mshr_ok, s.ip16, c);
  return;
#endif
  pdt_entry* e = pdt_alloc(key);
  for (auto& d : e->d) { // the levels set at the end of the previous epoch
    if (!d.valid || d.status == 0)
      continue;
    const uint64_t tline = cline + static_cast<int64_t>(d.delta);
    if (tline == own_line)
      continue;
    if (TDP_BOUNDS && !cheri::overlaps_bounds(champsim::block_number{champsim::address{tline << LOG2_BLOCK_SIZE}}, c.base, cheri::capability_top(c))) {
      st_tdp_oob++;
      continue;
    }
    const bool fill_l1 = d.status == 2 && mshr_ok;
    if (d.status == 2 && !mshr_ok && !kn.demote) {
      st_busy_drop++;
      continue;
    }
    if (kn.feedback && (e->acc < cheri_tdp::FEEDBACK_MIN_ISSUE || (!fill_l1 && e->acc < cheri_tdp::FEEDBACK_MIN_L2))) {
      (e->acc < cheri_tdp::FEEDBACK_MIN_ISSUE ? st_fb_off : st_fb_l2_off)++;
      continue;
    }
    if (issue_tdp(tline, fill_l1, s.ip16, c)) {
      ce.issued |= static_cast<uint16_t>(1u << (d.delta + TDP_MAX_LINE_DELTA));
      if (!fill_l1)
        l2u.issue(tline, d.status == 1 ? cheri_tdp::l2_usage::LOWCONF : cheri_tdp::l2_usage::DEMOTED);
    }
  }
  pdt_count_sighting(e);
}

bool berti_tdp::take_budget()
{
  if (TDP_SIGHT_PER_CYCLE == 0)
    return true;
  const uint64_t now = intern_->current_cycle();
  if (now != budget_cycle) {
    budget_cycle = now;
    budget_used = 0;
  }
  if (budget_used >= TDP_SIGHT_PER_CYCLE)
    return false;
  budget_used++;
  return true;
}

void berti_tdp::drain_sightings()
{
  while (!sq.empty() && take_budget()) {
    process_sighting(sq.front());
    sq.pop_front();
  }
}

// The pointers in the 16-byte slots of slot_mask of the line at line_va: what the line's tags and data carry.
void berti_tdp::scan_line(uint64_t line_va, uint64_t ip16, uint8_t source, uint32_t cpu, unsigned slot_mask)
{
  st_lines_scanned[source]++;
  for (unsigned slot = 0; slot < cheri::CAPS_PER_CL; slot++) {
    if (!(slot_mask & (1u << slot)))
      continue;
    auto c = champsim::cap_mem[cpu].load_capability(champsim::address{line_va + slot * cheri::CAP_ALIGNMENT_BYTES});
    if (!c.has_value() || !c->tag || !cheri::has_load_permissions(c->permissions) || cheri::has_execute_permissions(c->permissions))
      continue; // not a data pointer (code pointers are never prefetched into L1D)
    if (c->base.to<uint64_t>() + c->offset.to<uint64_t>() == 0)
      continue;
    if (sq.empty() && take_budget()) {
      process_sighting(sighting{*c, ip16, line_va, source});
      continue;
    }
    if (sq.size() >= TDP_SIGHTQ) {
      sq.pop_front();
      st_sq_dropped++;
    }
    sq.push_back(sighting{*c, ip16, line_va, source});
    st_sq_delayed++;
  }
}

// A dereference: a tagged demand load that misses L1D or hits a TDP-prefetched line. Credit the line difference to the
// key of the sighting of the pointer it went through.
void berti_tdp::train_deref(uint64_t line, const champsim::capability& cap, bool is_miss)
{
  const uint16_t bh = hash16(cap.base.to<uint64_t>());
  cht_entry* best = nullptr;
  int best_d = 0;
  for (auto& ce : cht) {
    if (!ce.valid || ce.base_hash != bh)
      continue;
    const int64_t d = static_cast<int64_t>(line) - static_cast<int64_t>(ce.cursor_line);
    if (d < -TDP_MAX_LINE_DELTA || d > TDP_MAX_LINE_DELTA)
      continue;
    if (best == nullptr || std::abs(d) < std::abs(best_d)) { // nearest cursor; on a tie, the oldest
      best = &ce;
      best_d = static_cast<int>(d);
    }
  }
  if (is_miss)
    st_tagged_load_miss++;
  if (best == nullptr)
    return;
  if (is_miss) { // statistics: cycles from sighting to dereference miss
    const uint64_t now = intern_->current_cycle();
    uint64_t g = now - best->time;
    int bkt = 0;
    while (g > 1 && bkt < 15) {
      g >>= 1;
      bkt++;
    }
    st_gap[best->source][bkt]++;
    st_match++;
    if (average_latency.num && now - best->time >= static_cast<uint64_t>(average_latency.average))
      st_match_timely++;
  }
  const uint16_t bit = static_cast<uint16_t>(1u << (best_d + TDP_MAX_LINE_DELTA));
  if (best->issued & bit) { // a prefetched target was dereferenced
    best->issued = static_cast<uint16_t>(best->issued & ~bit);
    st_pred_hit++;
    feedback(best->key, +1);
  }
  if (best->trained & bit)
    return;
  best->trained |= bit;
  st_delta_hist[best_d + TDP_MAX_LINE_DELTA]++;
  (is_miss ? st_train_miss : st_train_tdphit)++;
#if TDP_TIMELY
  if (is_miss) { // decided at the fill, when the fetch latency is known
    if (pending.size() >= TDP_PENDING) {
      pending.pop_front();
      st_pending_overflow++;
    }
    pending.push_back(pending_deref{line, best->key, static_cast<int8_t>(best_d), intern_->current_cycle() - best->time});
    return;
  }
#endif
  pdt_train(best->key, best_d); // a hit on a TDP-prefetched line: the prefetch was timely
}

void berti_tdp::feedback(uint16_t key, int change)
{
  if (!kn.feedback)
    return;
  for (auto& e : pdt) // lookup without an LRU update
    if (e.valid && e.tag == key) {
      e.acc = static_cast<uint8_t>(std::clamp<int>(e.acc + change, 0, cheri_tdp::FEEDBACK_MAX));
      return;
    }
}

// TDP_INTERVAL: per-interval counters, one line
void berti_tdp::print_interval()
{
  const uint64_t cur[11] = {st_tdp_l1, l2u.issued[cheri_tdp::l2_usage::LOWCONF], l2u.issued[cheri_tdp::l2_usage::DEMOTED], st_busy_drop,
                            st_tdp_fills, st_tdp_useful, st_tdp_useless, l2u.used[cheri_tdp::l2_usage::LOWCONF] + l2u.used[cheri_tdp::l2_usage::DEMOTED],
                            st_pred_hit, st_pred_miss, st_fb_off + st_fb_l2_off};
  const char* name[11] = {"issued_l1", "l2_lowconf", "l2_demoted", "busy_drop", "fills", "useful", "useless", "l2_used", "pred_hit", "pred_miss", "fb_skip"};
  std::cout << "TDP INTERVAL cycle " << intern_->current_cycle();
  for (int i = 0; i < 11; i++) {
    std::cout << " " << name[i] << " " << cur[i] - last_iv[i];
    last_iv[i] = cur[i];
  }
  std::cout << std::endl;
}
