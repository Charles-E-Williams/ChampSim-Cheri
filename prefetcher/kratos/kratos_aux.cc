//=======================================================================================//
// File             : kratos/kratos_aux.cc
// Description      : KRATOS v5 – internal helper function implementations.
//                    Translation cache, Aegis bounds enforcement, SMS pipeline tables,
//                    stride engine, pointer chasing, prefetch buffer management.
//=======================================================================================//

#include "kratos.h"

#include <algorithm>
#include <cassert>
#include <cmath>

#include "cache.h"

// ============================================================================
// Translation Cache
// ============================================================================

uint64_t kratos::get_current_v_address() const
{
  return 0xFEEDBEEF;
}

std::optional<champsim::capability> kratos::get_auth_capability() const
{
  const auto& cap = intern_->auth_capability;
  if (!cap.tag)
    return std::nullopt;
  return cap;
}

void kratos::tlb_update(uint64_t v_page, uint64_t p_page)
{
  // Set-associative insertion with LRU replacement
  uint32_t set_idx = static_cast<uint32_t>(v_page % TLB_SETS);
  uint32_t base    = set_idx * TLB_WAYS;

  // Check if already present — update age
  for (uint32_t w = 0; w < TLB_WAYS; w++) {
    auto& e = tlb_cache[base + w];
    if (e.valid && e.v_page == v_page) {
      e.p_page = p_page;   // mapping could change
      // Age all others in set, set this to youngest
      for (uint32_t j = 0; j < TLB_WAYS; j++) {
        if (tlb_cache[base + j].valid && tlb_cache[base + j].age < e.age)
          tlb_cache[base + j].age++;
      }
      e.age = 0;
      return;
    }
  }

  // Find victim: invalid first, then highest age
  uint32_t victim = base;
  uint8_t  max_age = 0;
  for (uint32_t w = 0; w < TLB_WAYS; w++) {
    if (!tlb_cache[base + w].valid) {
      victim = base + w;
      break;
    }
    if (tlb_cache[base + w].age >= max_age) {
      max_age = tlb_cache[base + w].age;
      victim  = base + w;
    }
  }

  // Age all valid entries in set
  for (uint32_t w = 0; w < TLB_WAYS; w++) {
    if (tlb_cache[base + w].valid)
      tlb_cache[base + w].age++;
  }

  auto& ve  = tlb_cache[victim];
  ve.v_page = v_page;
  ve.p_page = p_page;
  ve.age    = 0;
  ve.valid  = true;
}

std::optional<uint64_t> kratos::tlb_lookup(uint64_t va)
{
  stat_tlb_lookups++;

  uint64_t v_page  = va >> 12;
  uint32_t set_idx = static_cast<uint32_t>(v_page % TLB_SETS);
  uint32_t base    = set_idx * TLB_WAYS;

  for (uint32_t w = 0; w < TLB_WAYS; w++) {
    const auto& e = tlb_cache[base + w];
    if (e.valid && e.v_page == v_page) {
      stat_tlb_hits++;
      uint64_t pa = (e.p_page << 12) | (va & 0xFFF);
      return pa;
    }
  }
  return std::nullopt;
}

// ============================================================================
// Aegis — Bounds Enforcement
// ============================================================================

bool kratos::aegis_check(uint64_t pf_va, const champsim::capability& cap)
{
  uint64_t base = cap.base.to<uint64_t>();
  uint64_t top  = base + cap.length.to<uint64_t>();
  return (pf_va >= base) && (pf_va < top);
}

Bitmap kratos::compute_bounds_mask(uint64_t region_base_va,
                                   const champsim::capability& cap)
{
  Bitmap mask;
  uint64_t cap_base = cap.base.to<uint64_t>();
  uint64_t cap_top  = cap_base + cap.length.to<uint64_t>();

  for (int i = 0; i < BITMAP_MAX_SIZE; i++) {
    uint64_t cl_addr = region_base_va + (static_cast<uint64_t>(i) << LOG2_BLOCK_SIZE);
    if (cl_addr >= cap_base && cl_addr < cap_top)
      mask[i] = 1;
  }
  return mask;
}

// ============================================================================
// Filter Table
// ============================================================================

std::deque<kratos_helper::FilterEntry*>::iterator
kratos::ft_lookup(uint64_t p_page)
{
  return std::find_if(filter_table.begin(), filter_table.end(),
                      [p_page](const kratos_helper::FilterEntry* e) {
                        return e->p_page == p_page;
                      });
}

void kratos::ft_insert(uint64_t p_page, uint64_t v_page, uint64_t ip,
                       uint8_t trigger_offset, const champsim::capability& cap)
{
  // Evict LRU if full (FIFO: front is oldest)
  if (filter_table.size() >= FT_SIZE) {
    auto victim = filter_table.begin();
    ft_evict(victim);
  }

  auto* entry          = new kratos_helper::FilterEntry();
  entry->p_page        = p_page;
  entry->v_page        = v_page;
  entry->ip            = ip;
  entry->trigger_offset = trigger_offset;
  entry->cap           = cap;
  filter_table.push_back(entry);
}

void kratos::ft_evict(std::deque<kratos_helper::FilterEntry*>::iterator victim)
{
  delete *victim;
  filter_table.erase(victim);
}

// ============================================================================
// Accumulation Table
// ============================================================================

std::deque<kratos_helper::AccEntry*>::iterator
kratos::at_lookup(uint64_t p_page)
{
  return std::find_if(acc_table.begin(), acc_table.end(),
                      [p_page](const kratos_helper::AccEntry* e) {
                        return e->p_page == p_page;
                      });
}

void kratos::at_insert(kratos_helper::FilterEntry* ft_entry, uint8_t second_offset)
{
  // Evict LRU if full
  if (acc_table.size() >= AT_SIZE) {
    // Find max-age entry
    uint32_t max_age = 0;
    auto victim = acc_table.begin();
    for (auto it = acc_table.begin(); it != acc_table.end(); ++it) {
      if ((*it)->age >= max_age) {
        max_age = (*it)->age;
        victim  = it;
      }
    }
    at_evict(victim);
  }

  auto* entry          = new kratos_helper::AccEntry();
  entry->p_page        = ft_entry->p_page;
  entry->v_page        = ft_entry->v_page;
  entry->ip            = ft_entry->ip;
  entry->trigger_offset = ft_entry->trigger_offset;
  entry->cap           = ft_entry->cap;

  // Compute bounds mask from capability
  if (entry->cap.tag) {
    entry->bounds_mask = compute_bounds_mask(entry->v_page << 12, entry->cap);
  } else {
    entry->bounds_mask.set();   // all 1s — no capability constraint
  }

  // Record both the trigger and the second access
  entry->pattern[ft_entry->trigger_offset] = 1;
  entry->pattern[second_offset]            = 1;

  entry->age = 0;
  // Age all existing entries
  for (auto& e : acc_table)
    e->age++;

  acc_table.push_back(entry);
}

void kratos::at_evict(std::deque<kratos_helper::AccEntry*>::iterator victim)
{
  // Before eviction, train the PHT with the learned pattern
  pht_insert(*victim);
  delete *victim;
  acc_table.erase(victim);
}

void kratos::at_update_lru(std::deque<kratos_helper::AccEntry*>::iterator current)
{
  for (auto it = acc_table.begin(); it != acc_table.end(); ++it)
    (*it)->age++;
  (*current)->age = 0;
}

// ============================================================================
// Pattern History Table
// ============================================================================

uint64_t kratos::pht_signature(uint64_t ip, uint8_t trigger_offset)
{
  // Same scheme as SMS: concatenate IP bits with offset
  uint64_t sig = ip;
  sig = (sig << 6) + static_cast<uint64_t>(trigger_offset);
  return sig;
}

uint64_t kratos::pht_type_signature(uint64_t cap_length, uint8_t trigger_offset)
{
  // Mix capability length with offset — different hash to avoid collisions
  // with the primary signature space
  uint64_t sig = cap_length * 2654435761ULL;  // Knuth multiplicative hash
  sig = (sig << 6) + static_cast<uint64_t>(trigger_offset);
  return sig;
}

std::deque<kratos_helper::PHTEntry*>::iterator
kratos::pht_lookup(uint64_t signature, uint32_t& set)
{
  set = static_cast<uint32_t>(signature % PHT_SETS);
  return std::find_if(pht[set].begin(), pht[set].end(),
                      [signature](const kratos_helper::PHTEntry* e) {
                        return e->signature == signature;
                      });
}

std::deque<kratos_helper::PHTEntry*>::iterator
kratos::pht_lookup_type(uint64_t type_sig, uint32_t& set)
{
  // Type-based lookup uses a separate set computation
  set = static_cast<uint32_t>(type_sig % PHT_SETS);
  return std::find_if(pht[set].begin(), pht[set].end(),
                      [type_sig](const kratos_helper::PHTEntry* e) {
                        return e->type_signature == type_sig;
                      });
}

void kratos::pht_insert(kratos_helper::AccEntry* at_entry)
{
  uint64_t sig      = pht_signature(at_entry->ip, at_entry->trigger_offset);
  uint64_t type_sig = pht_type_signature(
      at_entry->cap.length.to<uint64_t>(), at_entry->trigger_offset);

  uint32_t set = 0;
  auto it = pht_lookup(sig, set);

  if (it != pht[set].end()) {
    // PHT hit — merge patterns
    kratos_helper::PHTEntry* existing = *it;

    // If the new pattern agrees with the stored one, increase confidence
    uint32_t same = BitmapHelper::count_bits_same(existing->pattern, at_entry->pattern);
    uint32_t diff = BitmapHelper::count_bits_diff(existing->pattern, at_entry->pattern);

    // Merge: OR the patterns together (accumulate observed accesses)
    existing->pattern = BitmapHelper::bitwise_or(existing->pattern, at_entry->pattern);
    existing->pointer_map = BitmapHelper::bitwise_or(existing->pointer_map,
                                                      at_entry->pointer_map);

    // Update confidence: up if patterns mostly agree, down if they diverge
    if (same >= diff) {
      if (existing->confidence < CONF_MAX)
        existing->confidence++;
    } else {
      if (existing->confidence > 0)
        existing->confidence--;
    }

    // Also update type confidence
    existing->type_signature = type_sig;
    if (same >= diff) {
      if (existing->type_confidence < CONF_MAX)
        existing->type_confidence++;
    } else {
      if (existing->type_confidence > 0)
        existing->type_confidence--;
    }

    pht_update_lru(set, it);
  } else {
    // PHT miss — allocate new entry
    // Evict LRU if set is full
    if (pht[set].size() >= PHT_ASSOC) {
      uint32_t max_age = 0;
      auto victim = pht[set].begin();
      for (auto jt = pht[set].begin(); jt != pht[set].end(); ++jt) {
        if ((*jt)->age >= max_age) {
          max_age = (*jt)->age;
          victim  = jt;
        }
      }
      delete *victim;
      pht[set].erase(victim);
    }

    auto* entry            = new kratos_helper::PHTEntry();
    entry->signature       = sig;
    entry->type_signature  = type_sig;
    entry->pattern         = at_entry->pattern;
    entry->pointer_map     = at_entry->pointer_map;
    entry->confidence      = 2;   // initial confidence (training needed)
    entry->type_confidence = 1;   // type path starts lower
    entry->age             = 0;

    // Age all entries in this set
    for (auto& e : pht[set])
      e->age++;

    pht[set].push_back(entry);
  }
}

void kratos::pht_update_lru(uint32_t set,
                            std::deque<kratos_helper::PHTEntry*>::iterator current)
{
  for (auto it = pht[set].begin(); it != pht[set].end(); ++it)
    (*it)->age++;
  (*current)->age = 0;
}

uint32_t kratos::pht_replay(const Bitmap& pattern, const Bitmap& bounds_mask,
                            uint64_t region_base_va, uint64_t p_page,
                            uint8_t trigger_offset)
{
  // Apply bounds mask and exclude the trigger position
  Bitmap to_prefetch = BitmapHelper::bitwise_and(pattern, bounds_mask);
  to_prefetch[trigger_offset] = 0;

  uint32_t count = 0;

  for (int i = 0; i < BITMAP_MAX_SIZE; i++) {
    if (!to_prefetch[i])
      continue;

    // Since the region IS a 4 KB page and we have the p_page, we can
    // directly construct the PA without a TLB lookup — all cachelines
    // in this region map to the same physical page.
    uint64_t pf_pa = (p_page << 12)
                   | (static_cast<uint64_t>(i) << LOG2_BLOCK_SIZE);

    buffer_prefetch(pf_pa);
    count++;
    stat_pf_buffered++;
  }

  return count;
}

// ============================================================================
// Stride Engine
// ============================================================================

void kratos::stride_operate(champsim::address addr_pa, uint64_t addr_va,
                            champsim::address ip,
                            const std::optional<champsim::capability>& cap)
{
  champsim::block_number cl_addr{addr_pa};
  champsim::block_number::difference_type stride{};

  // Probe the stride table by IP
  kratos_helper::StrideEntry probe;
  probe.ip = ip;
  auto found = stride_table.check_hit(probe);

  if (found.has_value()) {
    stride = champsim::offset(found->last_cl_addr, cl_addr);

    if (stride != 0 && stride == found->last_stride) {
      // Stride confirmed — initiate lookahead
      int degree = STRIDE_DEGREE;

      if (cap.has_value()) {
        // Adaptive degree: clip to capability bounds
        int direction = (stride > 0) ? 1 : -1;
        int remaining = cheri::remaining_lines(cl_addr, direction,
                                               cap->base,
                                               cheri::capability_top(*cap));
        int abs_stride = static_cast<int>(std::abs(stride));
        int max_useful = (abs_stride > 0) ? (remaining / abs_stride) : 0;
        degree = std::clamp(max_useful, 1, STRIDE_DEGREE);
      }

      active_stride_lookahead = {champsim::address{cl_addr}, stride,
                                 degree, cap, addr_va};
    }
  }

  // Update stride table with current access
  kratos_helper::StrideEntry updated;
  updated.ip          = ip;
  updated.last_cl_addr = cl_addr;
  updated.last_va     = addr_va;
  updated.last_stride = stride;
  updated.cap         = cap.value_or(champsim::capability{});
  updated.confidence  = (found.has_value() && stride != 0
                         && stride == found->last_stride)
                            ? std::min<uint8_t>(found->confidence + 1, CONF_MAX)
                            : 0;
  stride_table.fill(updated);
}

// ============================================================================
// Pointer Chasing
// ============================================================================

void kratos::ptr_discover(uint64_t cl_va, uint64_t ip,
                          uint64_t p_page, uint8_t cl_offset,
                          const champsim::capability& cap)
{
  // Align VA to cacheline boundary
  uint64_t cl_base_va = (cl_va >> LOG2_BLOCK_SIZE) << LOG2_BLOCK_SIZE;

  for (uint32_t slot = 0; slot < CAP_SLOTS_PER_CL; slot++) {
    // Each capability slot is 16 bytes (CAP_ALIGNMENT_BITS = 4)
    uint64_t slot_va = cl_base_va + (static_cast<uint64_t>(slot) << 4);

    auto target_cap = champsim::cap_mem[intern_->cpu]
                          .load_capability(champsim::address{slot_va});
    if (!target_cap.has_value() || !target_cap->tag)
      continue;

    // Found a live pointer — compute its virtual address target
    uint64_t target_va = cheri::capability_cursor(*target_cap).to<uint64_t>();
    if (target_va == 0)
      continue;

    // Translate target VA to PA; skip if translation unavailable
    auto target_pa_opt = tlb_lookup(target_va);
    if (!target_pa_opt.has_value()) {
      stat_pf_no_xlat++;
      continue;
    }

    // Evict oldest non-confirmed entry if buffer is full
    if (ptr_buffer.size() >= PTR_BUF_SIZE) {
      auto victim = std::find_if(ptr_buffer.begin(), ptr_buffer.end(),
                                 [](const kratos_helper::PtrBufEntry* e) {
                                   return e->valid && !e->confirmed;
                                 });
      if (victim != ptr_buffer.end()) {
        delete *victim;
        ptr_buffer.erase(victim);
      } else {
        // All entries confirmed — can't evict, skip this pointer
        continue;
      }
    }

    auto* entry         = new kratos_helper::PtrBufEntry();
    entry->source_v_page = cl_va >> 12;
    entry->source_cl_off = cl_offset;
    entry->slot_index   = static_cast<uint8_t>(slot);
    entry->source_ip    = ip;
    entry->target_va    = target_va;
    entry->target_pa    = target_pa_opt.value();
    entry->target_cap   = *target_cap;
    entry->confirmed    = false;
    entry->valid        = true;
    ptr_buffer.push_back(entry);

    stat_ptr_discovered++;
  }
}

void kratos::ptr_check_confirmation(uint64_t demand_pa, uint64_t demand_va,
                                    champsim::address ip)
{
  uint64_t demand_cl = demand_pa >> LOG2_BLOCK_SIZE;

  for (auto it = ptr_buffer.begin(); it != ptr_buffer.end(); ++it) {
    kratos_helper::PtrBufEntry* entry = *it;
    if (!entry->valid || entry->confirmed)
      continue;

    uint64_t target_cl = entry->target_pa >> LOG2_BLOCK_SIZE;
    if (demand_cl != target_cl)
      continue;

    // --- Pointer confirmed! ---
    entry->confirmed = true;
    stat_ptr_confirmed++;

    // Trigger depth-1 recursive prefetch for the target region,
    // but only if MSHR pressure allows it.
    if (intern_->get_mshr_occupancy_ratio() >= MSHR_SUPPRESS_RECUR)
      break;

    uint64_t target_v_page = entry->target_va >> 12;
    uint64_t target_p_page = entry->target_pa >> 12;
    uint8_t  target_offset = static_cast<uint8_t>(
        (entry->target_va >> LOG2_BLOCK_SIZE) & (BITMAP_MAX_SIZE - 1));

    // Try PHT lookup for the target region using the source IP as context
    uint64_t sig = pht_signature(entry->source_ip, target_offset);
    uint32_t set = 0;
    auto pht_it = pht_lookup(sig, set);

    if (pht_it != pht[set].end() && (*pht_it)->confidence >= CONF_PREDICT) {
      Bitmap bounds = entry->target_cap.tag
                          ? compute_bounds_mask(target_v_page << 12,
                                               entry->target_cap)
                          : ~Bitmap(0);
      pht_replay((*pht_it)->pattern, bounds,
                 target_v_page << 12, target_p_page, target_offset);
      stat_ptr_pf_issued++;
      pht_update_lru(set, pht_it);
    }
    // TODO Phase 3: try type-based lookup as fallback

    break;   // one confirmation per demand access
  }

  for (auto it = ptr_buffer.begin(); it != ptr_buffer.end(); ) {
    if ((*it)->confirmed) {
      delete *it;
      it = ptr_buffer.erase(it);
    } else {
      ++it;
    }
  }
}

// ============================================================================
// Prefetch Buffer — rate-limited issuance
// ============================================================================

void kratos::buffer_prefetch(uint64_t pa)
{
  if (pref_buffer.size() < PREF_BUF_SIZE)
    pref_buffer.push_back(pa);
  // Silently drop if buffer is full — back-pressure throttling
}

void kratos::buffer_prefetch(const std::vector<uint64_t>& addrs)
{
  for (uint64_t pa : addrs)
    buffer_prefetch(pa);
}

void kratos::issue_prefetches()
{
  uint32_t count = 0;

  while (!pref_buffer.empty() && count < PREF_DEGREE) {
    // Check MSHR pressure before each prefetch
    if (intern_->get_mshr_occupancy_ratio() >= MSHR_SUPPRESS_ALL)
      break;

    champsim::address pf_addr{pref_buffer.front()};
    bool mshr_light = intern_->get_mshr_occupancy_ratio() < MSHR_SUPPRESS_RECUR;
    bool success = prefetch_line(pf_addr, mshr_light, 0);

    if (!success)
      break;

    pref_buffer.pop_front();
    count++;
    stat_pf_issued++;
  }
}
