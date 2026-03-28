//=======================================================================================//
// File             : sms_cheri/sms_cheri_aux.cc
// Description      : Internal functions for the CHERI-aware SMS prefetcher.
//                    Implements capability-based region decomposition,
//                    FT/AT/PHT management, and bounds-checked prefetch
//                    address generation.
//=======================================================================================//

#include "sms_cheri.h"

#include <algorithm>
#include <iostream>

#include "cache.h"



// ============================================================================
// Region / offset decomposition
// ============================================================================
//
// With a valid capability:
//   VA          = cap.base + cap.offset   (the capability cursor)
//   obj_cl_off  = (VA - cap.base) >> LOG2_BLOCK_SIZE
//   region_idx  = obj_cl_off / REGION_CLS   (which 2KB chunk of the object)
//   region_id   = cap.base + (region_idx * REGION_SIZE)  [VA-space, unique per object chunk]
//   offset      = obj_cl_off % REGION_CLS
//
// Without a capability (fallback):
//   region_id   = PA >> REGION_SIZE_LOG     (stock SMS)
//   offset      = (PA >> LOG2_BLOCK_SIZE) & (REGION_CLS - 1)
//
// The region_id lives in a different namespace (VA vs shifted-PA) depending
// on has_cap, but collisions are astronomically unlikely.
// ============================================================================

sms_cheri::region_info
sms_cheri::decompose(uint64_t pa, const champsim::capability& cap) const
{
  region_info ri{};

  if (cap.tag) {
    uint64_t cap_base = cap.base.to<uint64_t>();
    uint64_t cap_len  = cap.length.to<uint64_t>();
    uint64_t cap_top  = cap_base + cap_len;
    uint64_t va       = cap_base + cap.offset.to<uint64_t>();

    // Guard: VA must be >= cap_base (should always hold for a valid cap).
    if (va < cap_base) {
      goto fallback;
    }

    uint64_t obj_byte_off = va - cap_base;
    uint64_t obj_cl_off   = obj_byte_off >> LOG2_BLOCK_SIZE;
    uint32_t rcls         = region_cls();
    uint64_t region_idx   = obj_cl_off / rcls;

    ri.region_id      = cap_base + (region_idx * REGION_SIZE);
    ri.offset         = static_cast<uint32_t>(obj_cl_off % rcls);
    ri.cap_base       = cap_base;
    ri.cap_top        = cap_top;
    ri.demand_pa_page = pa & ~((1ULL << LOG2_PAGE_SIZE) - 1);
    ri.demand_va_page = va & ~((1ULL << LOG2_PAGE_SIZE) - 1);
    ri.has_cap        = true;
    return ri;
  }

fallback:
  ri.region_id      = pa >> REGION_SIZE_LOG;
  ri.offset         = static_cast<uint32_t>((pa >> LOG2_BLOCK_SIZE)
                        & ((1ULL << (REGION_SIZE_LOG - LOG2_BLOCK_SIZE)) - 1));
  ri.cap_base       = 0;
  ri.cap_top        = 0;
  ri.demand_pa_page = pa & ~((1ULL << LOG2_PAGE_SIZE) - 1);
  ri.demand_va_page = 0;
  ri.has_cap        = false;
  return ri;
}

// ============================================================================
// Filter Table
// ============================================================================

std::deque<FTEntry*>::iterator sms_cheri::search_filter_table(uint64_t region_id)
{
  return std::find_if(filter_table.begin(), filter_table.end(),
    [region_id](FTEntry* e) { return e->region_id == region_id; });
}

void sms_cheri::insert_filter_table(uint64_t pc, const region_info& ri)
{
  if (filter_table.size() >= FT_SIZE) {
    auto victim = search_victim_filter_table();
    evict_filter_table(victim);
  }

  FTEntry* e        = new FTEntry();
  e->region_id      = ri.region_id;
  e->pc             = pc;
  e->trigger_offset = ri.offset;
  e->cap_base       = ri.cap_base;
  e->cap_top        = ri.cap_top;
  e->demand_pa_page = ri.demand_pa_page;
  e->demand_va_page = ri.demand_va_page;
  e->has_cap        = ri.has_cap;
  filter_table.push_back(e);
}

std::deque<FTEntry*>::iterator sms_cheri::search_victim_filter_table()
{
  return filter_table.begin();
}

void sms_cheri::evict_filter_table(std::deque<FTEntry*>::iterator victim)
{
  FTEntry* e = (*victim);
  filter_table.erase(victim);
  delete e;
}

// ============================================================================
// Accumulation Table
// ============================================================================

std::deque<ATEntry*>::iterator sms_cheri::search_acc_table(uint64_t region_id)
{
  return std::find_if(acc_table.begin(), acc_table.end(),
    [region_id](ATEntry* e) { return e->region_id == region_id; });
}

void sms_cheri::insert_acc_table(FTEntry* ft, uint32_t offset)
{
  if (acc_table.size() >= AT_SIZE) {
    auto victim = search_victim_acc_table();
    evict_acc_table(victim);
  }

  ATEntry* e        = new ATEntry();
  e->region_id      = ft->region_id;
  e->pc             = ft->pc;
  e->trigger_offset = ft->trigger_offset;
  e->cap_base       = ft->cap_base;
  e->cap_top        = ft->cap_top;
  e->demand_pa_page = ft->demand_pa_page;
  e->demand_va_page = ft->demand_va_page;
  e->has_cap        = ft->has_cap;
  e->pattern[ft->trigger_offset] = 1;
  e->pattern[offset] = 1;
  e->age = 0;

  for (uint32_t i = 0; i < acc_table.size(); ++i)
    acc_table[i]->age++;
  acc_table.push_back(e);
}

std::deque<ATEntry*>::iterator sms_cheri::search_victim_acc_table()
{
  uint32_t max_age = 0;
  std::deque<ATEntry*>::iterator victim = acc_table.begin();
  for (auto it = acc_table.begin(); it != acc_table.end(); ++it) {
    if ((*it)->age >= max_age) {
      max_age = (*it)->age;
      victim = it;
    }
  }
  return victim;
}

void sms_cheri::evict_acc_table(std::deque<ATEntry*>::iterator victim)
{
  ATEntry* e = (*victim);

  // Train the PHT with the accumulated pattern before eviction.
  insert_pht_table(e);

  acc_table.erase(victim);
  delete e;
}

void sms_cheri::update_age_acc_table(std::deque<ATEntry*>::iterator current)
{
  for (auto it = acc_table.begin(); it != acc_table.end(); ++it)
    (*it)->age++;
  (*current)->age = 0;
}

// ============================================================================
// Pattern History Table
// ============================================================================

void sms_cheri::insert_pht_table(ATEntry* at)
{
  uint64_t signature = create_signature(at->pc, at->trigger_offset);
  uint32_t set = 0;
  auto it = search_pht(signature, set);

  if (it != pht[set].end()) {
    // Update existing entry with new pattern.
    (*it)->pattern = at->pattern;
    update_age_pht(set, it);
  } else {
    if (pht[set].size() >= PHT_ASSOC) {
      auto victim = search_victim_pht(set);
      evict_pht(set, victim);
    }

    PHTEntry* e   = new PHTEntry();
    e->signature  = signature;
    e->pattern    = at->pattern;
    e->age        = 0;
    for (uint32_t i = 0; i < pht[set].size(); ++i)
      pht[set][i]->age = 0;
    pht[set].push_back(e);
  }
}

std::deque<PHTEntry*>::iterator sms_cheri::search_pht(uint64_t signature,
                                                      uint32_t& set)
{
  set = static_cast<uint32_t>(signature % PHT_SETS);
  return std::find_if(pht[set].begin(), pht[set].end(),
    [signature](PHTEntry* e) { return e->signature == signature; });
}

std::deque<PHTEntry*>::iterator sms_cheri::search_victim_pht(int32_t set)
{
  uint32_t max_age = 0;
  std::deque<PHTEntry*>::iterator victim = pht[set].begin();
  for (auto it = pht[set].begin(); it != pht[set].end(); ++it) {
    if ((*it)->age >= max_age) {
      max_age = (*it)->age;
      victim = it;
    }
  }
  return victim;
}

void sms_cheri::update_age_pht(int32_t set, std::deque<PHTEntry*>::iterator current)
{
  for (auto it = pht[set].begin(); it != pht[set].end(); ++it)
    (*it)->age++;
  (*current)->age = 0;
}

void sms_cheri::evict_pht(int32_t set, std::deque<PHTEntry*>::iterator victim)
{
  PHTEntry* e = (*victim);
  pht[set].erase(victim);
  delete e;
}

// ============================================================================
// Signature creation
// ============================================================================
//
// Identical to stock SMS: hash(PC, trigger_offset).  The key difference
// is that when a capability was present, `trigger_offset` is object-relative
// rather than page-relative.  Two PCs accessing the same field of same-sized
// objects will produce the same signature regardless of where those objects
// land in the physical address space.

uint64_t sms_cheri::create_signature(uint64_t pc, uint32_t offset)
{
  uint64_t signature = pc;
  signature = (signature << (REGION_SIZE_LOG - LOG2_BLOCK_SIZE));
  signature += static_cast<uint64_t>(offset);
  return signature;
}

// ============================================================================
// Prefetch generation
// ============================================================================
//
// Capability-backed path:
//   For each set bit `i` in the PHT bitmap, compute the target virtual
//   address within the object chunk, then:
//     1. Bounds check against [cap_base, cap_top).
//     2. Same-page check: the target must be on the same physical page
//        as the triggering demand (we can only derive PA within that page).
//     3. Translate VA -> PA using the same-page identity:
//        target_PA = demand_PA_page | (target_VA & PAGE_MASK)
//
// Page-fallback path:
//   Reconstruct the physical address directly (stock SMS).

std::size_t sms_cheri::generate_prefetch(uint64_t pc, uint64_t pa,
                                         const region_info& ri,
                                         std::vector<uint64_t>& pref_addr)
{
  uint64_t signature = create_signature(pc, ri.offset);
  uint32_t set = 0;
  auto it = search_pht(signature, set);
  if (it == pht[set].end())
    return 0;

  PHTEntry* entry = (*it);

  if (ri.has_cap) {
    // --- Capability-backed prefetch generation ---
    //
    // region_id is the VA of this object chunk's start:
    //   region_va_start = cap_base + (region_idx * REGION_SIZE)
    // which equals ri.region_id by construction in decompose().

    uint64_t region_va_start = ri.region_id;
    uint64_t page_mask = (1ULL << LOG2_PAGE_SIZE) - 1;

    for (uint32_t i = 0; i < region_cls(); ++i) {
      if (!entry->pattern[i] || i == ri.offset)
        continue;

      uint64_t target_va = region_va_start + (static_cast<uint64_t>(i) << LOG2_BLOCK_SIZE);

      // 1. Capability bounds check.
      if (target_va < ri.cap_base || target_va >= ri.cap_top) {
        stat_pref_bounds_clip++;
        continue;
      }

      // 2. Same virtual page as the demand?
      //    Within a page VA offset == PA offset, so we can only safely
      //    translate addresses on the demand's page.
      if ((target_va & ~page_mask) != ri.demand_va_page) {
        stat_pref_page_clip++;
        continue;
      }

      // 3. VA -> PA translation via same-page identity.
      uint64_t target_pa = ri.demand_pa_page | (target_va & page_mask);
      pref_addr.push_back(target_pa);
      stat_pref_cap++;
    }
  } else {
    // --- Page-fallback path (stock SMS) ---
    uint64_t region_pa_base = ri.region_id << REGION_SIZE_LOG;

    for (uint32_t i = 0; i < region_cls(); ++i) {
      if (!entry->pattern[i] || i == ri.offset)
        continue;

      uint64_t target_pa = region_pa_base + (static_cast<uint64_t>(i) << LOG2_BLOCK_SIZE);
      pref_addr.push_back(target_pa);
      stat_pref_nocap++;
    }
  }

  update_age_pht(set, it);
  stat_pref_generated += pref_addr.size();
  return pref_addr.size();
}

// ============================================================================
// Prefetch buffering and issuing
// ============================================================================

void sms_cheri::buffer_prefetch(std::vector<uint64_t> pref_addr)
{
  for (uint32_t i = 0; i < pref_addr.size(); ++i) {
    if (pref_buffer.size() >= PREF_BUFFER_SIZE)
      break;
    pref_buffer.push_back(pref_addr[i]);
  }
}

void sms_cheri::issue_prefetch()
{
  uint32_t count = 0;
  while (!pref_buffer.empty() && count < PREF_DEGREE) {
    champsim::address pf_addr{pref_buffer.front()};
    const bool success = prefetch_line(pf_addr, true, 0);
    if (!success)
      break;
    pref_buffer.pop_front();
    count++;
  }
}