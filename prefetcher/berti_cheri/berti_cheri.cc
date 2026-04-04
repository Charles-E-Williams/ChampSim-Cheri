#include "berti_cheri.h"

#include <iostream>

#include "cache.h"


void berti_cheri::prefetcher_initialize()
{
  std::cout << " L1D Berti-CHERI prefetcher" << std::endl;

  l1d_init_current_pages_table();
  l1d_init_prev_requests_table();
  l1d_init_prev_prefetches_table();
  l1d_init_record_pages_table();
  l1d_init_ip_table();
}

uint32_t berti_cheri::prefetcher_cache_operate(champsim::address address, champsim::address ip_addr,
                                               uint8_t cache_hit, bool useful_prefetch,
                                               access_type type, uint32_t metadata_in)
{
  uint64_t addr = address.to<uint64_t>();
  uint64_t ip = ip_addr.to<uint64_t>();
  auto current_core_cycle = intern_->current_time.time_since_epoch() / intern_->clock_period;
 
  auto cap = intern_->get_authorizing_capability();
  uint64_t cap_base = cap.base.to<uint64_t>();
  uint64_t cap_length = cap.length.to<uint64_t>();
 
  //  Compute region identity and offset 
  uint64_t cap_page_index = (addr - cap_base) >> LOG2_PAGE_SIZE;
  uint64_t region_addr = l1d_make_region_key(cap_base, cap_page_index);
  uint64_t chunk_byte_offset = (addr - cap_base) - (cap_page_index << LOG2_PAGE_SIZE);
  uint64_t bitmap_offset = chunk_byte_offset >> LOG2_BLOCK_SIZE;
 
  uint64_t line_addr = addr >> LOG2_BLOCK_SIZE;
  uint64_t page_addr = line_addr >> L1D_PAGE_BLOCKS_BITS;
 
  std::size_t pq_size = intern_->get_pq_size()[0];
  std::size_t pq_occupancy = intern_->get_pq_occupancy()[0];
 
 
  uint64_t index = l1d_get_current_pages_entry(region_addr);
 
  if (index == L1D_CURRENT_PAGES_TABLE_ENTRIES || !l1d_requested_offset_current_pages_table(index, bitmap_offset)) {
 
    if (index < L1D_CURRENT_PAGES_TABLE_ENTRIES) {
      //  Found existing entry for this region 
      if (l1d_requested_offset_current_pages_table(index, bitmap_offset))
        return 0;
 
      uint64_t first_ip = l1d_update_demand_current_pages_table(index, bitmap_offset);
      assert(l1d_ip_table[first_ip & L1D_IP_TABLE_INDEX_MASK] != L1D_IP_TABLE_NULL_POINTER);
 
      //  Update berti distance from prefetch timing 
      if (cache_hit) {
        uint64_t pref_latency = l1d_get_latency_prev_prefetches_table(region_addr, bitmap_offset);
        if (pref_latency != 0) {
          int b[L1D_CURRENT_PAGES_TABLE_NUM_BERTI_PER_ACCESS];
          l1d_get_berti_prev_requests_table(index, bitmap_offset, current_core_cycle - pref_latency, b);
          for (int i = 0; i < L1D_CURRENT_PAGES_TABLE_NUM_BERTI_PER_ACCESS; i++) {
            if (b[i] == 0)
              break;
            // CHERI: relaxed bound for cap-relative offsets
            if (abs(b[i]) >= L1D_PAGE_BLOCKS)
              continue;
            
            l1d_add_berti_current_pages_table(index, b[i]);
          }
 
          // Eliminate prev prefetch entry since its latency has been consumed
          l1d_reset_entry_prev_prefetches_table(region_addr, bitmap_offset);
        }
      }
 
      //  IP aliasing: group IPs that touch the same region 
      if (first_ip != (ip & L1D_IP_TABLE_INDEX_MASK)) {
        l1d_ip_table[ip & L1D_IP_TABLE_INDEX_MASK] = l1d_ip_table[first_ip & L1D_IP_TABLE_INDEX_MASK];
      }
 
    } else {
      //  New region: allocate current-pages entry 
      uint64_t victim_index = l1d_get_lru_current_pages_entry();
      assert(victim_index < L1D_CURRENT_PAGES_TABLE_ENTRIES);
      l1d_reset_pointer_prev_requests(victim_index);
      if (l1d_current_pages_table[victim_index].u_vector != 0)
        l1d_reset_region_prev_prefetches(l1d_current_pages_table[victim_index].region_addr);
 
      // Record the old entry before overwriting
      l1d_record_current_page(victim_index);
 
      index = victim_index;
      l1d_add_current_pages_table(index, region_addr, ip & L1D_IP_TABLE_INDEX_MASK, bitmap_offset, cap_base, cap_length);
 
      // Set up IP table for new entry
      uint64_t index_record = l1d_get_entry_record_pages_table(region_addr, bitmap_offset);
      if (l1d_ip_table[ip & L1D_IP_TABLE_INDEX_MASK] == L1D_IP_TABLE_NULL_POINTER) {
        if (index_record == L1D_RECORD_PAGES_TABLE_ENTRIES) {
          uint64_t new_pointer = l1d_get_lru_record_pages_entry();
          l1d_ip_table[ip & L1D_IP_TABLE_INDEX_MASK] = new_pointer;
        } else {
          l1d_ip_table[ip & L1D_IP_TABLE_INDEX_MASK] = index_record;
        }
      } else if (l1d_ip_table[ip & L1D_IP_TABLE_INDEX_MASK] != index_record) {
        uint64_t new_pointer = l1d_get_lru_record_pages_entry();
        l1d_copy_entries_record_pages_table(l1d_ip_table[ip & L1D_IP_TABLE_INDEX_MASK], new_pointer);
        l1d_ip_table[ip & L1D_IP_TABLE_INDEX_MASK] = new_pointer;
      }
    }
 
    //  COMMON: both found and new paths reach here 
    l1d_add_prev_requests_table(index, bitmap_offset, current_core_cycle);
 
    //  PREDICT 
    uint64_t u_vector = 0;
    uint64_t first_offset = l1d_current_pages_table[index].first_offset;
    int b = 0;
    bool recorded = false;
    uint64_t match_confidence = 0;
 
    uint64_t ip_pointer = l1d_ip_table[ip & L1D_IP_TABLE_INDEX_MASK];
    uint64_t pgo_pointer = l1d_get_entry_record_pages_table(region_addr, first_offset);
    uint64_t pg_pointer = l1d_get_entry_record_pages_table(region_addr);
    uint64_t berti_confidence = 0;
    int current_berti = l1d_get_berti_current_pages_table(index, berti_confidence);
 
    // Priority cascade
    if (pgo_pointer != L1D_RECORD_PAGES_TABLE_ENTRIES
        && (l1d_record_pages_table[pgo_pointer].u_vector | l1d_current_pages_table[index].u_vector)
               == l1d_record_pages_table[pgo_pointer].u_vector) {
      u_vector = l1d_record_pages_table[pgo_pointer].u_vector;
      b = l1d_record_pages_table[pgo_pointer].berti;
      match_confidence = 1;
      recorded = true;
    } else if (ip_pointer < L1D_RECORD_PAGES_TABLE_ENTRIES
               && l1d_record_pages_table[ip_pointer].first_offset == first_offset
               && (l1d_record_pages_table[ip_pointer].u_vector | l1d_current_pages_table[index].u_vector)
                      == l1d_record_pages_table[ip_pointer].u_vector) {
      u_vector = l1d_record_pages_table[ip_pointer].u_vector;
      b = l1d_record_pages_table[ip_pointer].berti;
      match_confidence = 1;
      recorded = true;
    } else if (current_berti != 0 && berti_confidence >= L1D_BERTI_CTR_MED_HIGH_CONFIDENCE) {
      u_vector = l1d_current_pages_table[index].u_vector;
      b = current_berti;
    } else if (pg_pointer != L1D_RECORD_PAGES_TABLE_ENTRIES) {
      u_vector = l1d_record_pages_table[pg_pointer].u_vector;
      b = l1d_record_pages_table[pg_pointer].berti;
      recorded = true;
    } else if (ip_pointer < L1D_RECORD_PAGES_TABLE_ENTRIES
               && l1d_record_pages_table[ip_pointer].u_vector) {
      u_vector = l1d_record_pages_table[ip_pointer].u_vector;
      b = l1d_record_pages_table[ip_pointer].berti;
      recorded = true;
    }
 
    //  Burst prefetches on first access or pending burst 
    if (first_offset == bitmap_offset || l1d_current_pages_table[index].last_burst != 0) {
      uint64_t first_burst;
      if (l1d_current_pages_table[index].last_burst != 0) {
        first_burst = l1d_current_pages_table[index].last_burst;
        l1d_current_pages_table[index].last_burst = 0;
      } else if (b >= 0) {
        first_burst = bitmap_offset + 1;
      } else {
        first_burst = bitmap_offset - 1;
      }
 
      if (recorded && match_confidence) {
        int bursts = 0;
        if (b > 0) {
          for (uint64_t i = first_burst; i < bitmap_offset + (uint64_t)b; i++) {
            if ((int)i >= L1D_PAGE_BLOCKS)
              break;
  
            uint64_t chunk_start = cap_base + (cap_page_index << LOG2_PAGE_SIZE);
            uint64_t pf_addr = chunk_start + (i << LOG2_BLOCK_SIZE);
            uint64_t pf_page_addr = pf_addr >> LOG2_PAGE_SIZE;
 
            if ((((uint64_t)1 << i) & u_vector) && !l1d_requested_offset_current_pages_table(index, i)) {
              if (pq_occupancy < pq_size && bursts < L1D_MAX_NUM_BURST_PREFETCHES) {
                bool in_bounds = cheri::prefetch_safe(champsim::address{pf_addr}, cap);
 
                if (in_bounds) {
                  bool prefetched = intern_->prefetch_line(champsim::address{pf_addr}, true, 0);
                  if (prefetched) {
                    l1d_add_prev_prefetches_table(region_addr, i, current_core_cycle);
                    stat_pf_issued_burst++;
                    bursts++;
                    if (pf_page_addr != page_addr)
                      stat_cross_page_in_object++;
                  }
                } else
                    stat_pf_bounded_by_cap++;
                
              } else {
                l1d_current_pages_table[index].last_burst = i;
                break;
              }
            }
          }
        } else if (b < 0) {
          for (int64_t i = (int64_t)first_burst; i > ((int64_t)bitmap_offset) + b; i--) {
            if (i < 0)
              break;
  
            uint64_t chunk_start = cap_base + (cap_page_index << LOG2_PAGE_SIZE);
            uint64_t pf_addr = chunk_start + (static_cast<uint64_t>(i) << LOG2_BLOCK_SIZE);
            uint64_t pf_page_addr = pf_addr >> LOG2_PAGE_SIZE;
 
            if ((((uint64_t)1 << i) & u_vector) && !l1d_requested_offset_current_pages_table(index, (uint64_t)i)) {
              if (pq_occupancy < pq_size && bursts < L1D_MAX_NUM_BURST_PREFETCHES) {
                bool in_bounds = cheri::prefetch_safe(champsim::address{pf_addr}, cap);
   
 
                if (in_bounds) {
                  bool prefetched = intern_->prefetch_line(champsim::address{pf_addr}, true, 0);
                  if (prefetched) {
                    l1d_add_prev_prefetches_table(region_addr, (uint64_t)i, current_core_cycle);
                    stat_pf_issued_burst++;
                    bursts++;
                    if (pf_page_addr != page_addr)
                      stat_cross_page_in_object++;
                  }
                } else 
                    stat_pf_bounded_by_cap++;
                
              } else {
                l1d_current_pages_table[index].last_burst = (uint64_t)i;
                break;
              }
            }
          }
        } else {
          // b == 0: zig-zag replay
          for (int i = (int)first_burst, j = (int)((first_offset << 1) - i);
               i < L1D_PAGE_BLOCKS || j >= 0;
               i++, j = (int)((first_offset << 1) - i)) {
            // Dir ++
            if (i < L1D_PAGE_BLOCKS) {
              uint64_t chunk_start = cap_base + (cap_page_index << LOG2_PAGE_SIZE);
              uint64_t pf_addr = chunk_start + (static_cast<uint64_t>(i) << LOG2_BLOCK_SIZE);
              uint64_t pf_offset = (uint64_t)i;
 
              if ((((uint64_t)1 << i) & u_vector) && !l1d_requested_offset_current_pages_table(index, pf_offset)) {
                if (pq_occupancy < pq_size && bursts < L1D_MAX_NUM_BURST_PREFETCHES) {
                  bool in_bounds = cheri::prefetch_safe(champsim::address{pf_addr}, cap);
                                          
                  if (in_bounds) {
                    bool prefetched = intern_->prefetch_line(champsim::address{pf_addr}, true, 0);
                    if (prefetched) {
                      l1d_add_prev_prefetches_table(region_addr, pf_offset, current_core_cycle);
                      stat_pf_issued_burst++;
                      bursts++;
                    }
                  } else 
                      stat_pf_bounded_by_cap++;
                  
                } else {
                  l1d_current_pages_table[index].last_burst = (uint64_t)i;
                  break;
                }
              }
            }
            // Dir --
            if (j >= 0) {
              uint64_t chunk_start = cap_base + (cap_page_index << LOG2_PAGE_SIZE);
              uint64_t pf_addr = chunk_start + (static_cast<uint64_t>(j) << LOG2_BLOCK_SIZE);
              uint64_t pf_offset = (uint64_t)j;
 
              if ((((uint64_t)1 << j) & u_vector) && !l1d_requested_offset_current_pages_table(index, pf_offset)) {
                if (pq_occupancy < pq_size && bursts < L1D_MAX_NUM_BURST_PREFETCHES) {
                  bool in_bounds = cheri::prefetch_safe(champsim::address{pf_addr}, cap);
                                           
                  if (in_bounds) {
                    bool prefetched = intern_->prefetch_line(champsim::address{pf_addr}, true, 0);
                    if (prefetched) {
                      l1d_add_prev_prefetches_table(region_addr, pf_offset, current_core_cycle);
                      stat_pf_issued_burst++;
                      bursts++;
                    }
                  } else  
                      stat_pf_bounded_by_cap++;
                  
                } else {
                  // record only positive burst
                }
              }
            }
          }
        }
      } else {
        // Not recorded or low confidence: no burst prefetches
      }
    }
 
    //  Single berti-distance prefetch 
    if (b != 0) {
      uint64_t pf_addr = addr + (static_cast<int64_t>(b) << LOG2_BLOCK_SIZE);
      uint64_t pf_page_addr = pf_addr >> LOG2_PAGE_SIZE;

      bool in_bounds = cheri::prefetch_safe(champsim::address{pf_addr}, cap);
      bool is_same_page = (pf_page_addr == page_addr);

      if (in_bounds) {
        uint64_t pf_cap_page_index = (pf_addr - cap_base) >> LOG2_PAGE_SIZE;
        uint64_t pf_chunk_byte_offset = (pf_addr - cap_base) - (pf_cap_page_index << LOG2_PAGE_SIZE);
        uint64_t pf_offset = pf_chunk_byte_offset >> LOG2_BLOCK_SIZE;
        if (is_same_page || intern_->virtual_prefetch) {
            if (!l1d_requested_offset_current_pages_table(index, pf_offset)
                && (!match_confidence || (((uint64_t)1 << pf_offset) & u_vector))) {
              
              bool prefetched = intern_->prefetch_line(champsim::address{pf_addr}, true, 0);
              if (prefetched) {
                // Safely track in the current page's entry
                uint64_t pf_region_addr = l1d_make_region_key(cap_base, pf_cap_page_index);
                l1d_add_prev_prefetches_table(pf_region_addr, pf_offset, current_core_cycle);
                stat_pf_issued_berti++;
              }
            }
        } else {
            bool prefetched = intern_->prefetch_line(champsim::address{pf_addr}, true, 0);
            if (prefetched) {
                uint64_t pf_region_addr = l1d_make_region_key(cap_base, pf_cap_page_index);
                l1d_add_prev_prefetches_table(pf_region_addr, pf_offset, current_core_cycle);
                stat_pf_issued_berti++;
                stat_cross_page_in_object++;
            }
        }
      } else {
          stat_pf_bounded_by_cap++;
      }
    }
  }
  return 0;
}

uint32_t berti_cheri::prefetcher_cache_fill(champsim::address address, long set, long way,
                                            uint8_t prefetch, champsim::address evicted_address,
                                            uint32_t metadata_in)
{
  uint64_t addr = address.to<uint64_t>();
  uint64_t evicted_addr = evicted_address.to<uint64_t>();
  auto current_core_cycle = intern_->current_time.time_since_epoch() / intern_->clock_period;

  // Search entries: bounds-check first, then verify region_addr matches the
  // correct page-chunk within the object. This correctly handles multi-page
  // objects that have multiple entries (one per page).
  uint64_t pointer_prev = L1D_CURRENT_PAGES_TABLE_ENTRIES;

  for (int i = 0; i < L1D_CURRENT_PAGES_TABLE_ENTRIES; i++) {
    if (l1d_current_pages_table[i].u_vector == 0)
      continue;
    uint64_t entry_cap_base = l1d_current_pages_table[i].cap_base;
    uint64_t cap_top = entry_cap_base + l1d_current_pages_table[i].cap_length;
    if (addr >= entry_cap_base && addr < cap_top) {
      uint64_t cap_page_idx = (addr - entry_cap_base) >> LOG2_PAGE_SIZE;
      uint64_t expected_region = l1d_make_region_key(entry_cap_base, cap_page_idx);
      if (l1d_current_pages_table[i].region_addr == expected_region) {
        pointer_prev = i;
        break;
      }
    }
  }

  if (pointer_prev < L1D_CURRENT_PAGES_TABLE_ENTRIES) {
    uint64_t entry_cap_base = l1d_current_pages_table[pointer_prev].cap_base;
    uint64_t cap_page_idx = (addr - entry_cap_base) >> LOG2_PAGE_SIZE;
    uint64_t chunk_byte_offset = (addr - entry_cap_base) - (cap_page_idx << LOG2_PAGE_SIZE);
    uint64_t offset = chunk_byte_offset >> LOG2_BLOCK_SIZE;

    uint64_t entry_region_addr = l1d_current_pages_table[pointer_prev].region_addr;
    uint64_t pref_latency = l1d_get_and_set_latency_prev_prefetches_table(entry_region_addr, offset, current_core_cycle);
    uint64_t demand_latency = l1d_get_latency_prev_requests_table(pointer_prev, offset, current_core_cycle);

    if (pref_latency == 0)
      pref_latency = demand_latency;

    if (demand_latency != 0) {
      int b[L1D_CURRENT_PAGES_TABLE_NUM_BERTI_PER_ACCESS];
      l1d_get_berti_prev_requests_table(pointer_prev, offset, current_core_cycle - (pref_latency + demand_latency), b);
      for (int i = 0; i < L1D_CURRENT_PAGES_TABLE_NUM_BERTI_PER_ACCESS; i++) {
        if (b[i] == 0)
          break;
        // Page-chunked: distances naturally bounded by page size
        if (abs(b[i]) >= L1D_PAGE_BLOCKS)
          continue;
        l1d_add_berti_current_pages_table(pointer_prev, b[i]);
      }
    }
  }

  // Handle evicted page/region — same region_addr verification
  uint64_t victim_index = L1D_CURRENT_PAGES_TABLE_ENTRIES;
  for (int i = 0; i < L1D_CURRENT_PAGES_TABLE_ENTRIES; i++) {
    if (l1d_current_pages_table[i].u_vector == 0)
      continue;
    uint64_t entry_cap_base = l1d_current_pages_table[i].cap_base;
    uint64_t cap_top = entry_cap_base + l1d_current_pages_table[i].cap_length;
    if (evicted_addr >= entry_cap_base && evicted_addr < cap_top) {
      uint64_t cap_page_idx = (evicted_addr - entry_cap_base) >> LOG2_PAGE_SIZE;
      uint64_t expected_region = l1d_make_region_key(entry_cap_base, cap_page_idx);
      if (l1d_current_pages_table[i].region_addr == expected_region) {
        victim_index = i;
        break;
      }
    }
  }

  if (victim_index < L1D_CURRENT_PAGES_TABLE_ENTRIES) {
    l1d_reset_region_prev_prefetches(l1d_current_pages_table[victim_index].region_addr);
    l1d_record_current_page(victim_index);
    l1d_remove_current_table_entry(victim_index);
  }

  return 0;
}

void berti_cheri::prefetcher_cycle_operate() {}

// ============================================================================
// Statistics
// ============================================================================

void berti_cheri::prefetcher_final_stats()
{
  std::cout << std::endl;
  std::cout << "berti_cheri final stats" << std::endl;
  std::cout << "  Prefetches bounded by cap:       " << stat_pf_bounded_by_cap << std::endl;
  std::cout << "  Prefetches issued (berti):       " << stat_pf_issued_berti << std::endl;
  std::cout << "  Prefetches issued (burst):       " << stat_pf_issued_burst << std::endl;
  std::cout << "  Cross-page in-object prefetches: " << stat_cross_page_in_object << std::endl;
}