#include "berti_cheri.h"

uint64_t berti_cheri::l1d_get_latency(uint64_t cycle, uint64_t cycle_prev)
{
  return cycle - cycle_prev;
  uint64_t cycle_masked = cycle & L1D_TIME_MASK;
  uint64_t cycle_prev_masked = cycle_prev & L1D_TIME_MASK;
  if (cycle_prev_masked > cycle_masked) {
    return (cycle_masked + L1D_TIME_OVERFLOW) - cycle_prev_masked;
  }
  return cycle_masked - cycle_prev_masked;
}

int berti_cheri::l1d_calculate_stride(uint64_t prev_offset, uint64_t current_offset)
{
  int stride;
  if (current_offset > prev_offset) {
    stride = (int)(current_offset - prev_offset);
  } else {
    stride = (int)(prev_offset - current_offset);
    stride *= -1;
  }
  return stride;
}

// ============================================================================
// Current Pages Table
// ============================================================================

void berti_cheri::l1d_init_current_pages_table()
{
  for (int i = 0; i < L1D_CURRENT_PAGES_TABLE_ENTRIES; i++) {
    l1d_current_pages_table[i].region_addr = 0;
    l1d_current_pages_table[i].ip = 0;
    l1d_current_pages_table[i].u_vector = 0;
    l1d_current_pages_table[i].last_burst = 0;
    l1d_current_pages_table[i].lru = i;
    l1d_current_pages_table[i].cap_valid = false;
    l1d_current_pages_table[i].cap_base = 0;
    l1d_current_pages_table[i].cap_length = 0;
  }
}

uint64_t berti_cheri::l1d_get_current_pages_entry(uint64_t region_addr)
{
  for (int i = 0; i < L1D_CURRENT_PAGES_TABLE_ENTRIES; i++) {
    if (l1d_current_pages_table[i].region_addr == region_addr && l1d_current_pages_table[i].u_vector != 0)
      return i;
  }
  return L1D_CURRENT_PAGES_TABLE_ENTRIES;
}

void berti_cheri::l1d_update_lru_current_pages_table(uint64_t index)
{
  assert(index < L1D_CURRENT_PAGES_TABLE_ENTRIES);
  for (int i = 0; i < L1D_CURRENT_PAGES_TABLE_ENTRIES; i++) {
    if (l1d_current_pages_table[i].lru < l1d_current_pages_table[index].lru)
      l1d_current_pages_table[i].lru++;
  }
  l1d_current_pages_table[index].lru = 0;
}

uint64_t berti_cheri::l1d_get_lru_current_pages_entry()
{
  uint64_t lru = L1D_CURRENT_PAGES_TABLE_ENTRIES;
  for (int i = 0; i < L1D_CURRENT_PAGES_TABLE_ENTRIES; i++) {
    l1d_current_pages_table[i].lru++;
    if (l1d_current_pages_table[i].lru == L1D_CURRENT_PAGES_TABLE_ENTRIES) {
      l1d_current_pages_table[i].lru = 0;
      lru = i;
    }
  }
  assert(lru != L1D_CURRENT_PAGES_TABLE_ENTRIES);
  return lru;
}

int berti_cheri::l1d_get_berti_current_pages_table(uint64_t index, uint64_t& ctr)
{
  assert(index < L1D_CURRENT_PAGES_TABLE_ENTRIES);
  uint64_t max_score = 0;
  uint64_t b = 0;
  for (int i = 0; i < L1D_CURRENT_PAGES_TABLE_NUM_BERTI; i++) {
    uint64_t score = l1d_current_pages_table[index].berti_ctr[i];
    if (score > max_score) {
      b = l1d_current_pages_table[index].berti[i];
      max_score = score;
      ctr = l1d_current_pages_table[index].berti_ctr[i];
    }
  }
  return (int)b;
}

void berti_cheri::l1d_add_current_pages_table(uint64_t index, uint64_t region_addr, uint64_t ip,
                                              uint64_t offset, bool cap_valid, uint64_t cap_base,
                                              uint64_t cap_length)
{
  assert(index < L1D_CURRENT_PAGES_TABLE_ENTRIES);
  l1d_current_pages_table[index].region_addr = region_addr;
  l1d_current_pages_table[index].ip = ip;
  l1d_current_pages_table[index].u_vector = (uint64_t)1 << offset;
  l1d_current_pages_table[index].first_offset = offset;
  for (int i = 0; i < L1D_CURRENT_PAGES_TABLE_NUM_BERTI; i++) {
    l1d_current_pages_table[index].berti_ctr[i] = 0;
  }
  l1d_current_pages_table[index].last_burst = 0;
  l1d_current_pages_table[index].cap_valid = cap_valid;
  l1d_current_pages_table[index].cap_base = cap_base;
  l1d_current_pages_table[index].cap_length = cap_length;
}

uint64_t berti_cheri::l1d_update_demand_current_pages_table(uint64_t index, uint64_t offset)
{
  assert(index < L1D_CURRENT_PAGES_TABLE_ENTRIES);
  l1d_current_pages_table[index].u_vector |= (uint64_t)1 << offset;
  l1d_update_lru_current_pages_table(index);
  return l1d_current_pages_table[index].ip;
}

void berti_cheri::l1d_add_berti_current_pages_table(uint64_t index, int b)
{
  assert(b != 0);
  assert(index < L1D_CURRENT_PAGES_TABLE_ENTRIES);
  for (int i = 0; i < L1D_CURRENT_PAGES_TABLE_NUM_BERTI; i++) {
    if (l1d_current_pages_table[index].berti_ctr[i] == 0) {
      l1d_current_pages_table[index].berti[i] = b;
      l1d_current_pages_table[index].berti_ctr[i] = 1;
      break;
    } else if (l1d_current_pages_table[index].berti[i] == b) {
      l1d_current_pages_table[index].berti_ctr[i]++;
      break;
    }
  }
  l1d_update_lru_current_pages_table(index);
}

bool berti_cheri::l1d_requested_offset_current_pages_table(uint64_t index, uint64_t offset)
{
  assert(index < L1D_CURRENT_PAGES_TABLE_ENTRIES);
  return l1d_current_pages_table[index].u_vector & ((uint64_t)1 << offset);
}

void berti_cheri::l1d_remove_current_table_entry(uint64_t index)
{
  l1d_current_pages_table[index].region_addr = 0;
  l1d_current_pages_table[index].u_vector = 0;
  l1d_current_pages_table[index].berti[0] = 0;
  l1d_current_pages_table[index].cap_valid = false;
  l1d_current_pages_table[index].cap_base = 0;
  l1d_current_pages_table[index].cap_length = 0;
}

//------------------------------------------//
// PREVIOUS REQUESTS TABLE
//------------------------------------------//

void berti_cheri::l1d_init_prev_requests_table()
{
  l1d_prev_requests_table_head = 0;
  for (int i = 0; i < L1D_PREV_REQUESTS_TABLE_ENTRIES; i++) {
    l1d_prev_requests_table[i].page_addr_pointer = L1D_PREV_REQUESTS_TABLE_NULL_POINTER;
  }
}

uint64_t berti_cheri::l1d_find_prev_request_entry(uint64_t pointer, uint64_t offset)
{
  for (int i = 0; i < L1D_PREV_REQUESTS_TABLE_ENTRIES; i++) {
    if (l1d_prev_requests_table[i].page_addr_pointer == pointer && l1d_prev_requests_table[i].offset == offset)
      return i;
  }
  return L1D_PREV_REQUESTS_TABLE_ENTRIES;
}

void berti_cheri::l1d_add_prev_requests_table(uint64_t pointer, uint64_t offset, uint64_t cycle)
{
  if (l1d_find_prev_request_entry(pointer, offset) != L1D_PREV_REQUESTS_TABLE_ENTRIES)
    return;

  l1d_prev_requests_table[l1d_prev_requests_table_head].page_addr_pointer = pointer;
  l1d_prev_requests_table[l1d_prev_requests_table_head].offset = offset;
  l1d_prev_requests_table[l1d_prev_requests_table_head].time = cycle & L1D_TIME_MASK;
  l1d_prev_requests_table_head = (l1d_prev_requests_table_head + 1) & L1D_PREV_REQUESTS_TABLE_MASK;
}

void berti_cheri::l1d_reset_pointer_prev_requests(uint64_t pointer)
{
  for (int i = 0; i < L1D_PREV_REQUESTS_TABLE_ENTRIES; i++) {
    if (l1d_prev_requests_table[i].page_addr_pointer == pointer)
      l1d_prev_requests_table[i].page_addr_pointer = L1D_PREV_REQUESTS_TABLE_NULL_POINTER;
  }
}

uint64_t berti_cheri::l1d_get_latency_prev_requests_table(uint64_t pointer, uint64_t offset, uint64_t cycle)
{
  uint64_t index = l1d_find_prev_request_entry(pointer, offset);
  if (index == L1D_PREV_REQUESTS_TABLE_ENTRIES)
    return 0;
  return l1d_get_latency(cycle, l1d_prev_requests_table[index].time);
}

void berti_cheri::l1d_get_berti_prev_requests_table(uint64_t pointer, uint64_t offset, uint64_t cycle, int* b)
{
  int my_pos = 0;
  uint64_t extra_time = 0;
  uint64_t last_time = l1d_prev_requests_table[(l1d_prev_requests_table_head + L1D_PREV_REQUESTS_TABLE_MASK) & L1D_PREV_REQUESTS_TABLE_MASK].time;

  for (uint64_t i = (l1d_prev_requests_table_head + L1D_PREV_REQUESTS_TABLE_MASK) & L1D_PREV_REQUESTS_TABLE_MASK;
       i != l1d_prev_requests_table_head;
       i = (i + L1D_PREV_REQUESTS_TABLE_MASK) & L1D_PREV_REQUESTS_TABLE_MASK) {
    if (last_time < l1d_prev_requests_table[i].time)
      extra_time = L1D_TIME_OVERFLOW;
    last_time = l1d_prev_requests_table[i].time;

    if (l1d_prev_requests_table[i].page_addr_pointer == pointer) {
      if (l1d_prev_requests_table[i].time <= (cycle & L1D_TIME_MASK) + extra_time) {
        b[my_pos] = l1d_calculate_stride(l1d_prev_requests_table[i].offset, offset);
        my_pos++;
        if (my_pos == L1D_CURRENT_PAGES_TABLE_NUM_BERTI_PER_ACCESS)
          return;
      }
    }
  }
  b[my_pos] = 0;
}

// ============================================================================
// Previous Prefetches Table
// ============================================================================

void berti_cheri::l1d_init_prev_prefetches_table()
{
  l1d_prev_prefetches_table_head = 0;
  for (int i = 0; i < L1D_PREV_PREFETCHES_TABLE_ENTRIES; i++) {
    l1d_prev_prefetches_table[i].page_addr_pointer = L1D_PREV_PREFETCHES_TABLE_NULL_POINTER;
  }
}

uint64_t berti_cheri::l1d_find_prev_prefetch_entry(uint64_t pointer, uint64_t offset)
{
  for (int i = 0; i < L1D_PREV_PREFETCHES_TABLE_ENTRIES; i++) {
    if (l1d_prev_prefetches_table[i].page_addr_pointer == pointer && l1d_prev_prefetches_table[i].offset == offset)
      return i;
  }
  return L1D_PREV_PREFETCHES_TABLE_ENTRIES;
}

void berti_cheri::l1d_add_prev_prefetches_table(uint64_t pointer, uint64_t offset, uint64_t cycle)
{
  if (l1d_find_prev_prefetch_entry(pointer, offset) != L1D_PREV_PREFETCHES_TABLE_ENTRIES)
    return;

  l1d_prev_prefetches_table[l1d_prev_prefetches_table_head].page_addr_pointer = pointer;
  l1d_prev_prefetches_table[l1d_prev_prefetches_table_head].offset = offset;
  l1d_prev_prefetches_table[l1d_prev_prefetches_table_head].time_lat = cycle & L1D_TIME_MASK;
  l1d_prev_prefetches_table[l1d_prev_prefetches_table_head].completed = false;
  l1d_prev_prefetches_table_head = (l1d_prev_prefetches_table_head + 1) & L1D_PREV_PREFETCHES_TABLE_MASK;
}

void berti_cheri::l1d_reset_pointer_prev_prefetches(uint64_t pointer)
{
  for (int i = 0; i < L1D_PREV_PREFETCHES_TABLE_ENTRIES; i++) {
    if (l1d_prev_prefetches_table[i].page_addr_pointer == pointer)
      l1d_prev_prefetches_table[i].page_addr_pointer = L1D_PREV_PREFETCHES_TABLE_NULL_POINTER;
  }
}

void berti_cheri::l1d_reset_entry_prev_prefetches_table(uint64_t pointer, uint64_t offset)
{
  uint64_t index = l1d_find_prev_prefetch_entry(pointer, offset);
  if (index != L1D_PREV_PREFETCHES_TABLE_ENTRIES)
    l1d_prev_prefetches_table[index].page_addr_pointer = L1D_PREV_PREFETCHES_TABLE_NULL_POINTER;
}

uint64_t berti_cheri::l1d_get_and_set_latency_prev_prefetches_table(uint64_t pointer, uint64_t offset, uint64_t cycle)
{
  uint64_t index = l1d_find_prev_prefetch_entry(pointer, offset);
  if (index == L1D_PREV_PREFETCHES_TABLE_ENTRIES)
    return 0;

  if (l1d_prev_prefetches_table[index].completed)
    return 0;

  uint64_t lat = l1d_get_latency(cycle, l1d_prev_prefetches_table[index].time_lat);
  l1d_prev_prefetches_table[index].time_lat = lat;
  l1d_prev_prefetches_table[index].completed = true;
  return lat;
}

uint64_t berti_cheri::l1d_get_latency_prev_prefetches_table(uint64_t pointer, uint64_t offset)
{
  uint64_t index = l1d_find_prev_prefetch_entry(pointer, offset);
  if (index == L1D_PREV_PREFETCHES_TABLE_ENTRIES)
    return 0;

  if (!l1d_prev_prefetches_table[index].completed)
    return 0;

  return l1d_prev_prefetches_table[index].time_lat;
}

//------------------------------------------//
// RECORD PAGES TABLE
//------------------------------------------//

void berti_cheri::l1d_init_record_pages_table()
{
  for (int i = 0; i < L1D_RECORD_PAGES_TABLE_ENTRIES; i++) {
    l1d_record_pages_table[i].region_addr = 0;
    l1d_record_pages_table[i].u_vector = 0;
    l1d_record_pages_table[i].first_offset = 0;
    l1d_record_pages_table[i].berti = 0;
    l1d_record_pages_table[i].lru = i;
    l1d_record_pages_table[i].cap_valid = false;
    l1d_record_pages_table[i].cap_length = 0;
  }
}

uint64_t berti_cheri::l1d_get_lru_record_pages_entry()
{
  uint64_t lru = 0;
  uint64_t max_lru = 0;
  for (int i = 0; i < L1D_RECORD_PAGES_TABLE_ENTRIES; i++) {
    if (l1d_record_pages_table[i].lru >= max_lru) {
      max_lru = l1d_record_pages_table[i].lru;
      lru = i;
    }
  }
  return lru;
}

void berti_cheri::l1d_update_lru_record_pages_table(uint64_t index)
{
  for (int i = 0; i < L1D_RECORD_PAGES_TABLE_ENTRIES; i++) {
    l1d_record_pages_table[i].lru++;
  }
  l1d_record_pages_table[index].lru = 0;
}

void berti_cheri::l1d_add_record_pages_table(uint64_t index, uint64_t region_addr, uint64_t vector,
                                             uint64_t first_offset, int berti, bool cap_valid,
                                             uint64_t cap_length)
{
  l1d_record_pages_table[index].region_addr = region_addr;
  l1d_record_pages_table[index].u_vector = vector;
  l1d_record_pages_table[index].first_offset = first_offset;
  l1d_record_pages_table[index].berti = berti;
  l1d_record_pages_table[index].cap_valid = cap_valid;
  l1d_record_pages_table[index].cap_length = cap_length;
  l1d_update_lru_record_pages_table(index);
}

uint64_t berti_cheri::l1d_get_entry_record_pages_table(uint64_t region_addr, uint64_t first_offset)
{
  for (int i = 0; i < L1D_RECORD_PAGES_TABLE_ENTRIES; i++) {
    if (l1d_record_pages_table[i].region_addr == region_addr
        && l1d_record_pages_table[i].first_offset == first_offset
        && l1d_record_pages_table[i].u_vector != 0)
      return i;
  }
  return L1D_RECORD_PAGES_TABLE_ENTRIES;
}

uint64_t berti_cheri::l1d_get_entry_record_pages_table(uint64_t region_addr)
{
  for (int i = 0; i < L1D_RECORD_PAGES_TABLE_ENTRIES; i++) {
    if (l1d_record_pages_table[i].region_addr == region_addr && l1d_record_pages_table[i].u_vector != 0)
      return i;
  }
  return L1D_RECORD_PAGES_TABLE_ENTRIES;
}

void berti_cheri::l1d_copy_entries_record_pages_table(uint64_t index_from, uint64_t index_to)
{
  l1d_record_pages_table[index_to].region_addr = l1d_record_pages_table[index_from].region_addr;
  l1d_record_pages_table[index_to].u_vector = l1d_record_pages_table[index_from].u_vector;
  l1d_record_pages_table[index_to].first_offset = l1d_record_pages_table[index_from].first_offset;
  l1d_record_pages_table[index_to].berti = l1d_record_pages_table[index_from].berti;
  l1d_record_pages_table[index_to].cap_valid = l1d_record_pages_table[index_from].cap_valid;
  l1d_record_pages_table[index_to].cap_length = l1d_record_pages_table[index_from].cap_length;
  l1d_update_lru_record_pages_table(index_to);
}

// ============================================================================
// IP Table
// ============================================================================

void berti_cheri::l1d_init_ip_table()
{
  for (int i = 0; i < L1D_IP_TABLE_ENTRIES; i++)
    l1d_ip_table[i] = L1D_IP_TABLE_NULL_POINTER;
}

// ============================================================================
// Eviction recording: save current-page entry to record-pages table
// ============================================================================

void berti_cheri::l1d_record_current_page(uint64_t index_current)
{
  uint64_t berti_confidence = 0;
  int b = l1d_get_berti_current_pages_table(index_current, berti_confidence);

  uint64_t region_addr = l1d_current_pages_table[index_current].region_addr;
  uint64_t first_offset = l1d_current_pages_table[index_current].first_offset;
  uint64_t u_vector = l1d_current_pages_table[index_current].u_vector;
  bool cap_valid = l1d_current_pages_table[index_current].cap_valid;
  uint64_t cap_length = l1d_current_pages_table[index_current].cap_length;

  // Find existing record or allocate new one
  uint64_t record_index = l1d_get_entry_record_pages_table(region_addr, first_offset);
  if (record_index == L1D_RECORD_PAGES_TABLE_ENTRIES)
    record_index = l1d_get_lru_record_pages_entry();

  l1d_add_record_pages_table(record_index, region_addr, u_vector, first_offset, b,
                             cap_valid, cap_length);
}