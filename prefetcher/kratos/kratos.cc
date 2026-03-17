//=======================================================================================//
// File             : kratos/kratos.cc
// Description      : KRATOS v5 – ChampSim prefetcher interface implementation.
//                    Routes accesses by capability size, drives the SMS pipeline,
//                    stride engine, and pointer chasing logic.
//=======================================================================================//

#include "kratos.h"

#include <algorithm>
#include <cassert>
#include <iostream>
#include <iomanip>

#include "cache.h"

// =========================================================================
// prefetcher_initialize
// =========================================================================

void kratos::prefetcher_initialize()
{
  // Initialize PHT as vector of sets (same pattern as SMS)
  std::deque<kratos_helper::PHTEntry*> empty_set;
  pht.resize(PHT_SETS, empty_set);

  // Zero all statistics
  stat_tlb_lookups = stat_tlb_hits = 0;
  stat_route_atlas = stat_route_stride = stat_route_nocap = 0;
  stat_ft_inserts = stat_at_promotions = 0;
  stat_pht_hits_primary = stat_pht_hits_type = stat_pht_misses = 0;
  stat_pf_issued = stat_pf_bounded = stat_pf_no_xlat = 0;
  stat_pf_buffered = stat_pf_useful = 0;
  stat_ptr_discovered = stat_ptr_confirmed = stat_ptr_pf_issued = 0;
  stat_stride_issued = stat_stride_bounded = 0;

  std::cout << "KRATOS v5 L2 prefetcher initialized" << std::endl;
  std::cout << "  TLB_ENTRIES  = " << TLB_ENTRIES << std::endl;
  std::cout << "  FT_SIZE      = " << FT_SIZE << std::endl;
  std::cout << "  AT_SIZE      = " << AT_SIZE << std::endl;
  std::cout << "  PHT_SIZE     = " << PHT_SIZE << " (" << PHT_SETS << " sets x " << PHT_ASSOC << " ways)" << std::endl;
  std::cout << "  STRIDE       = " << STRIDE_SETS << " sets x " << STRIDE_WAYS << " ways" << std::endl;
  std::cout << "  PTR_BUF_SIZE = " << PTR_BUF_SIZE << std::endl;
  std::cout << "  PREF_BUF     = " << PREF_BUF_SIZE << " (degree " << PREF_DEGREE << "/cyc)" << std::endl;
  std::cout << "  Routing: small/med <= " << CAP_SMALL_MAX << "B -> Atlas, "
            << "> " << CAP_LARGE_MIN << "B -> Stride" << std::endl;
}

// =========================================================================
// prefetcher_cache_operate — main routing and learning entry point
// =========================================================================

uint32_t kratos::prefetcher_cache_operate(champsim::address addr, champsim::address ip,
                                          uint8_t cache_hit, bool useful_prefetch,
                                          access_type type, uint32_t metadata_in)
{
  uint64_t pa     = addr.to<uint64_t>();
  uint64_t va     = get_current_v_address();
  uint64_t ip_val = ip.to<uint64_t>();

  uint64_t v_page = va >> 12;
  uint64_t p_page = pa >> 12;

  // Track useful prefetches for stats
  if (useful_prefetch)
    stat_pf_useful++;

  // Always update the translation cache from demand traffic
  if (va != 0)
    tlb_update(v_page, p_page);

  // -----------------------------------------------------------------------
  // Capability-based routing
  // -----------------------------------------------------------------------
  auto cap = get_auth_capability();

  if (!cap.has_value()) {
    // No valid capability → IP-stride fallback (page-bounded)
    stat_route_nocap++;
    stride_operate(addr, va, ip, std::nullopt);
    return metadata_in;
  }

  uint64_t cap_length = cap->length.to<uint64_t>();

  if (cap_length > CAP_LARGE_MIN) {
    // Large capability → stride engine with bounds clipping
    stat_route_stride++;
    stride_operate(addr, va, ip, cap);
    return metadata_in;
  }

  // -----------------------------------------------------------------------
  // Small / medium capability → SMS spatial pipeline
  // -----------------------------------------------------------------------
  stat_route_atlas++;

  uint8_t cl_offset = static_cast<uint8_t>((pa >> LOG2_BLOCK_SIZE) & (BITMAP_MAX_SIZE - 1));
  bool is_miss = (cache_hit == 0);

  // Check pointer buffer for confirmation on every demand access
  ptr_check_confirmation(pa, va, ip);

  // --- Accumulation Table hit: update the pattern ---
  auto at_it = at_lookup(p_page);
  if (at_it != acc_table.end()) {
    (*at_it)->pattern[cl_offset] = 1;
    at_update_lru(at_it);

    // Pointer discovery on demand miss to small pointer-bearing object
    if (is_miss && cap_length <= CAP_SMALL_MAX
        && cheri::has_load_cap(cap->permissions)) {
      ptr_discover(va, ip_val, p_page, cl_offset, *cap);
    }
    return metadata_in;
  }

  // --- Filter Table hit: promote to Accumulation Table ---
  auto ft_it = ft_lookup(p_page);
  if (ft_it != filter_table.end()) {
    stat_at_promotions++;
    at_insert(*ft_it, cl_offset);
    ft_evict(ft_it);

    if (is_miss && cap_length <= CAP_SMALL_MAX
        && cheri::has_load_cap(cap->permissions)) {
      ptr_discover(va, ip_val, p_page, cl_offset, *cap);
    }
    return metadata_in;
  }

  // --- New region: consult PHT for prediction, then insert into FT ---

  // Primary PHT lookup: hash(IP, trigger_offset)
  uint64_t sig = pht_signature(ip_val, cl_offset);
  uint32_t set = 0;
  auto pht_it = pht_lookup(sig, set);

  if (pht_it != pht[set].end() && (*pht_it)->confidence >= CONF_PREDICT) {
    // PHT hit — replay the learned footprint
    stat_pht_hits_primary++;
    Bitmap bounds = cap->tag
                        ? compute_bounds_mask(v_page << 12, *cap)
                        : ~Bitmap(0);
    pht_replay((*pht_it)->pattern, bounds, v_page << 12, p_page, cl_offset);
    pht_update_lru(set, pht_it);
  } else {
    // TODO Phase 3: type-based secondary PHT lookup here
    //   uint64_t tsig = pht_type_signature(cap_length, cl_offset);
    //   uint32_t tset = 0;
    //   auto type_it = pht_lookup_type(tsig, tset);
    //   if (type_it != pht[tset].end() && (*type_it)->type_confidence >= CONF_TYPE) { ... }
    stat_pht_misses++;
  }

  // Insert into filter table (always, even if PHT predicted)
  stat_ft_inserts++;
  ft_insert(p_page, v_page, ip_val, cl_offset, *cap);

  // Pointer discovery
  if (is_miss && cap_length <= CAP_SMALL_MAX
      && cheri::has_load_cap(cap->permissions)) {
    ptr_discover(va, ip_val, p_page, cl_offset, *cap);
  }

  return metadata_in;
}

// =========================================================================
// prefetcher_cache_fill — called when a miss is filled in the L2
// =========================================================================

uint32_t kratos::prefetcher_cache_fill(champsim::address addr, long set, long way,
                                       uint8_t prefetch, champsim::address evicted_addr,
                                       uint32_t metadata_in)
{
  // Currently minimal — feedback is handled via useful_prefetch in cache_operate.
  // Future: track eviction of prefetched lines for negative feedback.
  return metadata_in;
}

// =========================================================================
// prefetcher_cycle_operate — progressive stride issuance + buffer drain
// =========================================================================

void kratos::prefetcher_cycle_operate()
{
  // ----- Stride lookahead: issue one prefetch per cycle -----
  if (active_stride_lookahead.has_value()) {
    auto [old_pf_address, stride, degree, cap, last_va] = *active_stride_lookahead;
    assert(degree > 0);

    champsim::address pf_address{champsim::block_number{old_pf_address} + stride};

    // Bounds check in VA space using capability
    if (cap.has_value()) {
      int64_t stride_bytes = static_cast<int64_t>(stride) << LOG2_BLOCK_SIZE;
      uint64_t pf_va = last_va + static_cast<uint64_t>(stride_bytes);
      if (!cheri::in_bounds(champsim::address{pf_va},
                            cap->base, cheri::capability_top(*cap))) {
        stat_stride_bounded++;
        active_stride_lookahead.reset();
        issue_prefetches();
        return;
      }
    }

    // Page boundary check in PA space — stop at boundary for PIPT
    if (intern_->virtual_prefetch
        || champsim::page_number{pf_address} == champsim::page_number{old_pf_address}) {
      bool mshr_light = intern_->get_mshr_occupancy_ratio() < MSHR_SUPPRESS_ALL;
      bool success = prefetch_line(pf_address, mshr_light, 0);
      if (success) {
        stat_stride_issued++;
        int64_t stride_bytes = static_cast<int64_t>(stride) << LOG2_BLOCK_SIZE;
        uint64_t new_va = last_va + static_cast<uint64_t>(stride_bytes);
        active_stride_lookahead = {pf_address, stride, degree - 1, cap, new_va};
      }
      if (active_stride_lookahead.has_value() && active_stride_lookahead->degree == 0)
        active_stride_lookahead.reset();
    } else {
      // Cross-page: stop lookahead, next demand will restart stride detection
      active_stride_lookahead.reset();
    }
  }

  // ----- Drain prefetch buffer (spatial + pointer replay) -----
  issue_prefetches();
}

// =========================================================================
// prefetcher_final_stats
// =========================================================================

void kratos::prefetcher_final_stats()
{
  std::cout << std::endl;
  std::cout << "KRATOS v5 final stats" << std::endl;
  std::cout << "  --- Translation Cache ---" << std::endl;
  std::cout << "  TLB lookups:           " << stat_tlb_lookups << std::endl;
  std::cout << "  TLB hits:              " << stat_tlb_hits << std::endl;
  if (stat_tlb_lookups > 0)
    std::cout << "  TLB hit rate:          "
              << std::fixed << std::setprecision(2)
              << (100.0 * stat_tlb_hits / stat_tlb_lookups) << "%" << std::endl;

  std::cout << "  --- Capability Routing ---" << std::endl;
  std::cout << "  Route atlas (small/med): " << stat_route_atlas << std::endl;
  std::cout << "  Route stride (large):    " << stat_route_stride << std::endl;
  std::cout << "  Route nocap (fallback):  " << stat_route_nocap << std::endl;

  std::cout << "  --- SMS Pipeline ---" << std::endl;
  std::cout << "  FT inserts:            " << stat_ft_inserts << std::endl;
  std::cout << "  AT promotions:         " << stat_at_promotions << std::endl;
  std::cout << "  PHT hits (primary):    " << stat_pht_hits_primary << std::endl;
  std::cout << "  PHT hits (type):       " << stat_pht_hits_type << std::endl;
  std::cout << "  PHT misses:            " << stat_pht_misses << std::endl;

  std::cout << "  --- Prefetching ---" << std::endl;
  std::cout << "  PF buffered:           " << stat_pf_buffered << std::endl;
  std::cout << "  PF issued:             " << stat_pf_issued << std::endl;
  std::cout << "  PF bounded (Aegis):    " << stat_pf_bounded << std::endl;
  std::cout << "  PF no translation:     " << stat_pf_no_xlat << std::endl;
  std::cout << "  PF useful:             " << stat_pf_useful << std::endl;

  std::cout << "  --- Pointer Chasing ---" << std::endl;
  std::cout << "  Ptrs discovered:       " << stat_ptr_discovered << std::endl;
  std::cout << "  Ptrs confirmed:        " << stat_ptr_confirmed << std::endl;
  std::cout << "  Ptr PF issued:         " << stat_ptr_pf_issued << std::endl;

  std::cout << "  --- Stride Engine ---" << std::endl;
  std::cout << "  Stride PF issued:      " << stat_stride_issued << std::endl;
  std::cout << "  Stride PF bounded:     " << stat_stride_bounded << std::endl;
}