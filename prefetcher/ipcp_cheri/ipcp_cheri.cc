#include "ipcp_cheri.h"

#include <iostream>

#include "cache.h"

std::optional<champsim::capability> ipcp_cheri::get_auth_capability() const
{
  const auto& cap = intern_->auth_capability;
  if (!cap.tag)
    return std::nullopt;
  return cap;
}

bool ipcp_cheri::is_valid_auth_cap(const champsim::capability& cap) const
{
  return cap.tag;
}

void ipcp_cheri::prefetcher_initialize()
{
  std::cout << "IPCP_CHERI_AT_L1_CONFIG" << std::endl
            << "NUM_IP_TABLE_L1_ENTRIES " << NUM_IP_TABLE_L1_ENTRIES << std::endl
            << "NUM_GHB_ENTRIES " << NUM_GHB_ENTRIES << std::endl
            << "NUM_IP_INDEX_BITS " << NUM_IP_INDEX_BITS << std::endl
            << "NUM_IP_TAG_BITS " << NUM_IP_TAG_BITS << std::endl
            << "S_TYPE " << S_TYPE << std::endl
            << "CS_TYPE " << CS_TYPE << std::endl
            << "CPLX_TYPE " << CPLX_TYPE << std::endl
            << "NL_TYPE " << NL_TYPE << std::endl
            << "CHERI-aware" << std::endl
            << std::endl;

  prev_cpu_cycle = 0;
  num_misses = 0;

  for (int i = 0; i < NUM_GHB_ENTRIES; i++)
    ghb_l1[i] = 0;
}

uint32_t ipcp_cheri::prefetcher_cache_operate(champsim::address address, champsim::address ip_addr,
                                              uint8_t cache_hit, bool useful_prefetch,
                                              access_type type, uint32_t metadata_in)
{
  uint64_t addr = address.to<uint64_t>();
  uint64_t ip = ip_addr.to<uint64_t>();
  uint64_t curr_page = addr >> LOG2_PAGE_SIZE;
  uint64_t cl_addr = addr >> LOG2_BLOCK_SIZE;

  auto cap_opt = get_auth_capability();
  stat_cap_lookups++;

  bool use_cap = false;
  uint64_t cap_base_val = 0;
  uint64_t cap_length_val = 0;

  if (cap_opt.has_value() && is_valid_auth_cap(*cap_opt)) {
    stat_cap_hits++;
    cap_base_val = cap_opt->base.to<uint64_t>();
    cap_length_val = cap_opt->length.to<uint64_t>();
    use_cap = true;
  }

  // Compute offset: capability-relative or page-relative 
  // When capability is valid, the offset is relative to the object base.
  // This eliminates the need for page-crossing stride adjustments.
  int64_t cl_offset = 0;
  if (use_cap) {
    // Object-relative offset in cache-line units
    cl_offset = static_cast<int64_t>((addr - cap_base_val) >> LOG2_BLOCK_SIZE);
    stat_cap_offset_used++;
  } else {
    // Fallback: page-relative offset (original IPCP behavior)
    cl_offset = static_cast<int64_t>((addr >> LOG2_BLOCK_SIZE) & 0x3F);
    stat_page_offset_fallback++;
  }

  uint16_t signature = 0, last_signature = 0;
  int prefetch_degree = 0;
  int spec_nl_threshold = 0;
  int num_prefs = 0;
  uint32_t metadata = 0;
  uint16_t ip_tag = (ip >> NUM_IP_INDEX_BITS) & ((1 << NUM_IP_TAG_BITS) - 1);

  prefetch_degree = 3;
  spec_nl_threshold = 15;

  if (cache_hit == 0)
    num_misses += 1;

  auto ct = intern_->current_time.time_since_epoch() / intern_->clock_period;

  if (num_misses == 256) {
    mpkc = ((float)num_misses / (float)(ct - prev_cpu_cycle)) * 1000;
    prev_cpu_cycle = ct;
    if (mpkc > (float)spec_nl_threshold)
      spec_nl = 0;
    else
      spec_nl = 1;
    num_misses = 0;
  }

  int index = ip & ((1 << NUM_IP_INDEX_BITS) - 1);

  if (trackers_l1[index].ip_tag != ip_tag) {
    // New or conflicting IP
    if (trackers_l1[index].ip_valid == 0) {
      trackers_l1[index].ip_tag = ip_tag;
      trackers_l1[index].last_page = curr_page;
      trackers_l1[index].last_cl_offset = cl_offset;
      trackers_l1[index].last_stride = 0;
      trackers_l1[index].ip_valid = 1;
      trackers_l1[index].signature = 0;
      trackers_l1[index].conf = 0;
      trackers_l1[index].str_valid = 0;
      trackers_l1[index].str_strength = 0;
      trackers_l1[index].str_dir = 0;

      // Store capability context
      trackers_l1[index].cap_valid = use_cap;
      trackers_l1[index].cap_base = cap_base_val;
      trackers_l1[index].cap_length = cap_length_val;
    } else {
      trackers_l1[index].ip_valid = 0;
    }

    // Next-line prefetch on new IP
    uint64_t pf_address = ((addr >> LOG2_BLOCK_SIZE) + 1) << LOG2_BLOCK_SIZE;
    // CHERI bounds check for NL prefetch
    if (use_cap) {
      if (!cheri::prefetch_safe(champsim::address{pf_address}, *cap_opt)) {
        stat_pf_bounded_by_cap++;
        return 0;
      }
    }

    metadata = encode_metadata(1, NL_TYPE, spec_nl);
    intern_->prefetch_line(champsim::address{pf_address}, true, metadata);
    stat_pf_issued_nl++;
    return 0;
  } else {
    trackers_l1[index].ip_valid = 1;
  }

  // when both current and stored access use capabilities
  // from the same object, the stride is computed directly from object-relative
  // offsets.  No page-crossing adjustment is needed.
  int64_t stride = 0;
  bool same_cap_context = use_cap && trackers_l1[index].cap_valid
                          && (cap_base_val == trackers_l1[index].cap_base);

  if (same_cap_context) {
    // Both accesses are within the same capability object.
    // Stride is simply the difference in object-relative offsets.
    stride = cl_offset - trackers_l1[index].last_cl_offset;
    // No page-boundary correction needed -- this is the key win.

    if (curr_page != trackers_l1[index].last_page) {
      // We crossed a page but stayed in the same object.
      // In vanilla IPCP this would have triggered stride += 64.
      // With CHERI-relative offsets, we get the correct stride for free.
      stat_page_cross_eliminated++;
    }
  } else {
    // Fallback: vanilla IPCP stride computation
    if (cl_offset > trackers_l1[index].last_cl_offset)
      stride = cl_offset - trackers_l1[index].last_cl_offset;
    else {
      stride = trackers_l1[index].last_cl_offset - cl_offset;
      stride *= -1;
    }

    if (stride == 0)
        return 0;

    // Page boundary learning 
    if (curr_page != trackers_l1[index].last_page) {
      if (stride < 0)
        stride += 64;
      else
        stride -= 64;
    }
  }

  // update constant stride(CS) confidence
  trackers_l1[index].conf = update_conf((int)stride, (int)trackers_l1[index].last_stride, trackers_l1[index].conf);

  if (trackers_l1[index].conf == 0)
    trackers_l1[index].last_stride = stride;

  last_signature = trackers_l1[index].signature;
  DPT_l1[last_signature].conf = update_conf((int)stride, DPT_l1[last_signature].delta, DPT_l1[last_signature].conf);

  if (DPT_l1[last_signature].conf == 0)
    DPT_l1[last_signature].delta = (int)stride;

  signature = update_sig_l1(last_signature, (int)stride);
  trackers_l1[index].signature = signature;

  // Check stream in GHB
  check_for_stream_l1(index, cl_addr);

  // Update stored state
  trackers_l1[index].last_cl_offset = cl_offset;
  trackers_l1[index].last_page = curr_page;
  trackers_l1[index].cap_valid = use_cap;
  trackers_l1[index].cap_base = cap_base_val;
  trackers_l1[index].cap_length = cap_length_val;

  // Update GHB (dedup-then-shift, matching stock IPCP)
  int ghb_index = 0;
  for (ghb_index = 0; ghb_index < NUM_GHB_ENTRIES; ghb_index++)
    if (cl_addr == ghb_l1[ghb_index])
      break;
  if (ghb_index == NUM_GHB_ENTRIES) {
    for (ghb_index = NUM_GHB_ENTRIES - 1; ghb_index > 0; ghb_index--)
      ghb_l1[ghb_index] = ghb_l1[ghb_index - 1];
    ghb_l1[0] = cl_addr;
  }

  // ---- Prefetch issuance ----
  // For each class (Stream, CS, CPLX, NL), the prefetch boundary is
  // either capability bounds (when available) or same-page (fallback).

  if (trackers_l1[index].str_valid == 1) {
    // Stream IP
    prefetch_degree = prefetch_degree * 2;
    for (int i = 0; i < prefetch_degree; i++) {
      uint64_t pf_address = 0;

      if (trackers_l1[index].str_dir == 1)
        pf_address = (cl_addr + i + 1) << LOG2_BLOCK_SIZE;
      else
        pf_address = (cl_addr - i - 1) << LOG2_BLOCK_SIZE;

      // Boundary check: CHERI bounds or same-page
      if (use_cap) {
        if (!cheri::prefetch_safe(champsim::address{pf_address}, *cap_opt)) {
          stat_pf_bounded_by_cap++;
          break;
        }
      } else {
        if ((pf_address >> LOG2_PAGE_SIZE) != (addr >> LOG2_PAGE_SIZE))
          break;
      }

      metadata = encode_metadata(trackers_l1[index].str_dir == 1 ? 1 : -1, S_TYPE, spec_nl);
      intern_->prefetch_line(champsim::address{pf_address}, true, metadata);
      stat_pf_issued_stream++;
      num_prefs++;
    }

  } else if (trackers_l1[index].conf > 1 && trackers_l1[index].last_stride != 0) {
    // Constant Stride IP
    for (int i = 0; i < prefetch_degree; i++) {
      uint64_t pf_address = (cl_addr + (trackers_l1[index].last_stride * (i + 1))) << LOG2_BLOCK_SIZE;

      if (use_cap) {
        if (!cheri::prefetch_safe(champsim::address{pf_address}, *cap_opt)) {
          stat_pf_bounded_by_cap++;
          break;
        }
      } else {
        if ((pf_address >> LOG2_PAGE_SIZE) != (addr >> LOG2_PAGE_SIZE))
          break;
      }

      metadata = encode_metadata((int)trackers_l1[index].last_stride, CS_TYPE, spec_nl);
      intern_->prefetch_line(champsim::address{pf_address}, true, metadata);
      stat_pf_issued_cs++;
      num_prefs++;
    }

  } else if (DPT_l1[signature].conf >= 0 && DPT_l1[signature].delta != 0) {
    // Complex Stride IP
    int pref_offset = 0;
    for (int i = 0; i < prefetch_degree; i++) {
      pref_offset += DPT_l1[signature].delta;
      uint64_t pf_address = ((cl_addr + pref_offset) << LOG2_BLOCK_SIZE);

      // Boundary check: CHERI bounds or same-page
      if (use_cap) {
        if (!cheri::prefetch_safe(champsim::address{pf_address}, *cap_opt)) {
          stat_pf_bounded_by_cap++;
          break;
        }
      } else {
        if ((pf_address >> LOG2_PAGE_SIZE) != (addr >> LOG2_PAGE_SIZE))
          break;
      }

      if (DPT_l1[signature].conf == -1 || DPT_l1[signature].delta == 0)
        break;

      // Encode delta as 0 for CPLX (stock IPCP convention: don't propagate
      // complex deltas to downstream prefetchers via metadata)
      metadata = encode_metadata(0, CPLX_TYPE, spec_nl);
      if (DPT_l1[signature].conf > 0) {
        intern_->prefetch_line(champsim::address{pf_address}, true, metadata);
        stat_pf_issued_cplx++;
        num_prefs++;
      }

      signature = update_sig_l1(signature, DPT_l1[signature].delta);
    }
  } 

  if (num_prefs == 0 && spec_nl == 1) {
    uint64_t pf_address = ((cl_addr + 1) << LOG2_BLOCK_SIZE);
 
    bool nl_ok = true;
    if (use_cap) {
      if (!cheri::prefetch_safe(champsim::address{pf_address}, *cap_opt)) {
        stat_pf_bounded_by_cap++;
        nl_ok = false;
      }
    } else {
      if ((pf_address >> LOG2_PAGE_SIZE) != (addr >> LOG2_PAGE_SIZE))
        nl_ok = false;
    }
 
    if (nl_ok) {
      metadata = encode_metadata(1, NL_TYPE, spec_nl);
      intern_->prefetch_line(champsim::address{pf_address}, true, metadata);
      stat_pf_issued_nl++;
    }
  }
  
  return 0;
}

uint32_t ipcp_cheri::prefetcher_cache_fill(champsim::address addr, long set, long way,
                                           uint8_t prefetch, champsim::address evicted_addr,
                                           uint32_t metadata_in)
{
  return 0;
}

void ipcp_cheri::prefetcher_cycle_operate() {}

// ============================================================================
// Signature and metadata helpers (structurally unchanged from vanilla IPCP)
// ============================================================================

uint16_t ipcp_cheri::update_sig_l1(uint16_t old_sig, int delta)
{
  uint16_t new_sig = 0;
  int sig_delta = (delta < 0) ? (((-1) * delta) + (1 << 6)) : delta;
  new_sig = ((old_sig << 1) ^ sig_delta) & 0xFFF;
  return new_sig;
}

uint32_t ipcp_cheri::encode_metadata(int stride, uint16_t type, int _spec_nl)
{
  uint32_t metadata = 0;
  if (stride > 0)
    metadata = stride;
  else
    metadata = ((-1 * stride) | 0b1000000);

  metadata = metadata | (type << 8);
  metadata = metadata | (_spec_nl << 12);
  return metadata;
}

void ipcp_cheri::check_for_stream_l1(int index, uint64_t cl_addr)
{
  int pos_count = 0, neg_count = 0, count = 0;
  uint64_t check_addr = cl_addr;

  for (int i = 0; i < NUM_GHB_ENTRIES; i++) {
    check_addr--;
    for (int j = 0; j < NUM_GHB_ENTRIES; j++)
      if (check_addr == ghb_l1[j]) {
        pos_count++;
        break;
      }
  }

  check_addr = cl_addr;
  for (int i = 0; i < NUM_GHB_ENTRIES; i++) {
    check_addr++;
    for (int j = 0; j < NUM_GHB_ENTRIES; j++)
      if (check_addr == ghb_l1[j]) {
        neg_count++;
        break;
      }
  }

  if (pos_count > neg_count) {
    trackers_l1[index].str_dir = 1;
    count = pos_count;
  } else {
    trackers_l1[index].str_dir = 0;
    count = neg_count;
  }

  if (count > NUM_GHB_ENTRIES / 2) {
    trackers_l1[index].str_valid = 1;
    if (count >= (NUM_GHB_ENTRIES * 3) / 4)
      trackers_l1[index].str_strength = 1;
  } else {
    if (trackers_l1[index].str_strength == 0)
      trackers_l1[index].str_valid = 0;
  }
}

int ipcp_cheri::update_conf(int stride, int pred_stride, int conf)
{
  if (stride == pred_stride) {
    conf++;
    if (conf > 3)
      conf = 3;
  } else {
    conf--;
    if (conf < 0)
      conf = 0;
  }
  return conf;
}

// ============================================================================
// Statistics
// ============================================================================

void ipcp_cheri::prefetcher_final_stats()
{
  std::cout << std::endl;
  std::cout << "ipcp_cheri final stats" << std::endl;
  std::cout << "  CHERI cap lookups:               " << stat_cap_lookups << std::endl;
  std::cout << "  CHERI cap hits:                  " << stat_cap_hits << std::endl;
  if (stat_cap_lookups > 0)
    std::cout << "  CHERI cap hit rate:              "
              << (100.0 * static_cast<double>(stat_cap_hits) / static_cast<double>(stat_cap_lookups))
              << "%" << std::endl;
  std::cout << "  Cap-relative offset used:        " << stat_cap_offset_used << std::endl;
  std::cout << "  Page-relative offset fallback:   " << stat_page_offset_fallback << std::endl;
  std::cout << "  Page-cross adjustments avoided:  " << stat_page_cross_eliminated << std::endl;
  std::cout << "  Prefetches bounded by cap:       " << stat_pf_bounded_by_cap << std::endl;
  std::cout << "  Prefetches issued (CS):          " << stat_pf_issued_cs << std::endl;
  std::cout << "  Prefetches issued (CPLX):        " << stat_pf_issued_cplx << std::endl;
  std::cout << "  Prefetches issued (Stream):      " << stat_pf_issued_stream << std::endl;
  std::cout << "  Prefetches issued (NL):          " << stat_pf_issued_nl << std::endl;
}