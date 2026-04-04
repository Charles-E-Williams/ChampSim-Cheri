#include "ipcp_cheri.h"

#include <iostream>

#include "cache.h"

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
    ghb_l1[i] = {0, 0};
  for (int i = 0; i < NUM_IP_OBJECT_CTX_ENTRIES; i++)
    object_state_l1[i] = {};
  for (int i = 0; i < NUM_REGION_STREAM_ENTRIES; i++)
    region_stream_l1[i] = {};
}

uint32_t ipcp_cheri::prefetcher_cache_operate(champsim::address address, champsim::address ip_addr,
                                              uint8_t cache_hit, bool useful_prefetch,
                                              access_type type, uint32_t metadata_in)
{
  uint64_t addr = address.to<uint64_t>();
  uint64_t ip = ip_addr.to<uint64_t>();

  auto cap = intern_->get_authorizing_capability();
  if (!cheri::is_tag_valid(cap)) 
    return metadata_in;

  uint64_t cap_base_val = cap.base.to<uint64_t>();
  uint64_t cap_length_val = cap.length.to<uint64_t>();

  // Compute offset
  // Object-relative offset in cache-line units.
  int64_t cl_offset = static_cast<int64_t>((addr - cap_base_val) >> LOG2_BLOCK_SIZE);

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
      trackers_l1[index].last_cl_offset = cl_offset;
      trackers_l1[index].last_stride = 0;
      trackers_l1[index].ip_valid = 1;
      trackers_l1[index].signature = 0;
      trackers_l1[index].conf = 0;
      trackers_l1[index].str_valid = 0;
      trackers_l1[index].str_strength = 0;
      trackers_l1[index].str_dir = 0;

      // Store capability context
      trackers_l1[index].cap_base = cap_base_val;
      trackers_l1[index].cap_length = cap_length_val;
    } else {
      trackers_l1[index].ip_valid = 0;
    }

    // Next-line prefetch on new IP
    uint64_t pf_address = cap_base_val + (static_cast<uint64_t>(cl_offset + 1) << LOG2_BLOCK_SIZE);
    if (!cheri::prefetch_safe(champsim::address{pf_address}, cap)) {
      stat_pf_bounded_by_cap++;
      return 0;
    }

    metadata = encode_metadata(1, NL_TYPE, spec_nl);
    intern_->prefetch_line(champsim::address{pf_address}, true, metadata);
    stat_pf_issued_nl++;
    return 0;
  } else {
    trackers_l1[index].ip_valid = 1;
  }

  // When both accesses are within the same capability object, stride is the
  // difference in object-relative offsets. No page-boundary correction needed.
  int64_t stride = 0;
  bool same_object = (cap_base_val == trackers_l1[index].cap_base);

  if (same_object) {
    // Normal stride calculation within the same object
    stride = cl_offset - trackers_l1[index].last_cl_offset;
  } else {
    // Save outgoing object context and try restoring context for the incoming object.
    save_object_state(ip, trackers_l1[index]);
    bool restored = load_object_state(ip, cap_base_val, trackers_l1[index]);

    trackers_l1[index].cap_base = cap_base_val;
    trackers_l1[index].cap_length = cap_length_val;
    trackers_l1[index].last_cl_offset = cl_offset;

    if (!restored) {
      trackers_l1[index].last_stride = 0;
      trackers_l1[index].signature = 0;
      trackers_l1[index].conf = 0;
      trackers_l1[index].str_valid = 0;
      trackers_l1[index].str_strength = 0;
      trackers_l1[index].str_dir = 0;
    }

    int ghb_index = 0;
    for (ghb_index = 0; ghb_index < NUM_GHB_ENTRIES; ghb_index++)
      if (cl_offset == ghb_l1[ghb_index].cap_cl_offset)
        break;
    if (ghb_index == NUM_GHB_ENTRIES) {
      for (ghb_index = NUM_GHB_ENTRIES - 1; ghb_index > 0; ghb_index--)
        ghb_l1[ghb_index] = ghb_l1[ghb_index - 1];
      ghb_l1[0] = {cl_offset, cap_base_val};
    }

    check_for_region_stream_l1(index, cap_base_val, cl_offset);
    return 0;
  }

  int64_t MAX_STRIDE = static_cast<int64_t>(cap_length_val >> LOG2_BLOCK_SIZE);
  if (stride > MAX_STRIDE || stride < -MAX_STRIDE)  {
    // Update the offset for future accesses, but abort training for this large jump
    trackers_l1[index].last_cl_offset = cl_offset;
    trackers_l1[index].last_stride = 0; 
    
    // We optionally update the GHB here so stream detection still works on absolute addresses
    int ghb_index = 0;
    for (ghb_index = 0; ghb_index < NUM_GHB_ENTRIES; ghb_index++)
      if (cl_offset == ghb_l1[ghb_index].cap_cl_offset)
        break;
    if (ghb_index == NUM_GHB_ENTRIES) {
      for (ghb_index = NUM_GHB_ENTRIES - 1; ghb_index > 0; ghb_index--)
        ghb_l1[ghb_index] = ghb_l1[ghb_index - 1];
      ghb_l1[0] = {cl_offset, cap_base_val};
    }
    return 0;
  }

  if (stride == 0)
      return 0;

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

  // Check stream in GHB and region stream table.
  check_for_stream_l1(index, cap_base_val);
  check_for_region_stream_l1(index, cap_base_val, cl_offset);

  // Update stored state
  trackers_l1[index].last_cl_offset = cl_offset;
  trackers_l1[index].cap_base = cap_base_val;
  trackers_l1[index].cap_length = cap_length_val;
  save_object_state(ip, trackers_l1[index]);

  // Update GHB (dedup-then-shift, matching stock IPCP)
  int ghb_index = 0;
  for (ghb_index = 0; ghb_index < NUM_GHB_ENTRIES; ghb_index++)
    if (cl_offset == ghb_l1[ghb_index].cap_cl_offset)
      break;
  if (ghb_index == NUM_GHB_ENTRIES) {
    for (ghb_index = NUM_GHB_ENTRIES - 1; ghb_index > 0; ghb_index--)
      ghb_l1[ghb_index] = ghb_l1[ghb_index - 1];
    ghb_l1[0] = {cl_offset, cap_base_val};
  }

  // Prefetch boundary is always capability bounds.

  if (trackers_l1[index].str_valid == 1) {
    // Stream IP
    prefetch_degree = prefetch_degree * 2;
    for (int i = 0; i < prefetch_degree; i++) {
      int64_t stream_delta = (trackers_l1[index].str_dir == 1) ? (i + 1) : -(i + 1);
      int64_t pf_cl_offset = cl_offset + stream_delta;
  
      if (pf_cl_offset < 0) {
        stat_pf_bounded_by_cap++;
        break;
      }

      uint64_t pf_address = cap_base_val + (static_cast<uint64_t>(pf_cl_offset) << LOG2_BLOCK_SIZE);

      if (!cheri::prefetch_safe(champsim::address{pf_address}, cap)) {
        stat_pf_bounded_by_cap++;
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
      int64_t pf_cl_offset = cl_offset + (trackers_l1[index].last_stride * (i + 1));
      if (pf_cl_offset < 0) {
        stat_pf_bounded_by_cap++;
        break;
      }
      uint64_t pf_address = cap_base_val + (static_cast<uint64_t>(pf_cl_offset) << LOG2_BLOCK_SIZE);

      if (!cheri::prefetch_safe(champsim::address{pf_address}, cap)) {
        stat_pf_bounded_by_cap++;
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
      int64_t pf_cl_offset = cl_offset + pref_offset;

      if (pf_cl_offset < 0) {
        stat_pf_bounded_by_cap++;
        break;
      }

      uint64_t pf_address = cap_base_val + (static_cast<uint64_t>(pf_cl_offset) << LOG2_BLOCK_SIZE);


      if (DPT_l1[signature].conf == -1 || DPT_l1[signature].delta == 0)
        break;

      if (!cheri::prefetch_safe(champsim::address{pf_address}, cap)) {
        stat_pf_bounded_by_cap++;
        break;
      }

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
    uint64_t pf_address = cap_base_val + (static_cast<uint64_t>(cl_offset + 1) << LOG2_BLOCK_SIZE);

    bool nl_ok = true;
    if (!cheri::prefetch_safe(champsim::address{pf_address}, cap)) {
      stat_pf_bounded_by_cap++;
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


uint16_t ipcp_cheri::update_sig_l1(uint16_t old_sig, int delta)
{
  uint16_t new_sig = 0;
  constexpr int SIG_MAGNITUDE_MASK = 0x3F;
  constexpr int LARGE_STRIDE_FOLD_BIT = 0x20;
  constexpr int SIG_MAX_MAGNITUDE = 63;
  int abs_delta = (delta < 0) ? (-delta) : delta;
  int magnitude = abs_delta & SIG_MAGNITUDE_MASK;
  if (abs_delta > SIG_MAX_MAGNITUDE)
    magnitude |= LARGE_STRIDE_FOLD_BIT;
  int sig_delta = (delta < 0) ? (magnitude + (1 << 6)) : magnitude;
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

void ipcp_cheri::check_for_stream_l1(int index, uint64_t cap_base_val)
{
int64_t cur_offset = trackers_l1[index].last_cl_offset;

  int pos_count = 0, neg_count = 0, count = 0;
  int64_t check_off = cur_offset;

  for (int i = 0; i < NUM_GHB_ENTRIES; i++) {
    check_off--;
    for (int j = 0; j < NUM_GHB_ENTRIES; j++)
      if (check_off == ghb_l1[j].cap_cl_offset && cap_base_val == ghb_l1[j].cap_base) {
        pos_count++;
        break;
      }
  }

  check_off = cur_offset;
  for (int i = 0; i < NUM_GHB_ENTRIES; i++) {
    check_off++;
    for (int j = 0; j < NUM_GHB_ENTRIES; j++)
      if (check_off == ghb_l1[j].cap_cl_offset && cap_base_val == ghb_l1[j].cap_base) {
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

void ipcp_cheri::check_for_region_stream_l1(int index, uint64_t cap_base_val, int64_t cl_offset)
{
  int region_index = static_cast<int>((cap_base_val >> LOG2_BLOCK_SIZE) & (NUM_REGION_STREAM_ENTRIES - 1));
  auto& entry = region_stream_l1[region_index];

  if (!entry.valid || entry.cap_base != cap_base_val) {
    entry.valid = 1;
    entry.cap_base = cap_base_val;
    entry.last_cl_offset = cl_offset;
    entry.run_length = 0;
    entry.dir = 0;
    return;
  }

  int64_t delta = cl_offset - entry.last_cl_offset;
  if (delta == 1 || delta == -1) {
    uint16_t new_dir = (delta == 1) ? 1 : 0;
    if (entry.run_length == 0 || entry.dir == new_dir) {
      entry.run_length++;
    } else {
      entry.run_length = 1;
    }
    entry.dir = new_dir;
  } else {
    entry.run_length = 0;
  }

  entry.last_cl_offset = cl_offset;

  if (entry.run_length >= 4) {
    trackers_l1[index].str_valid = 1;
    trackers_l1[index].str_dir = entry.dir;
    if (entry.run_length >= 8)
      trackers_l1[index].str_strength = 1;
  }
}

uint32_t ipcp_cheri::object_state_index(uint64_t ip, uint64_t cap_base_val)
{
  // Knuth multiplicative hashing constant for 64-bit key mixing.
  constexpr uint64_t HASH_MULTIPLIER = 11400714819323198485ull;
  uint64_t mixed = (ip * HASH_MULTIPLIER) ^ (cap_base_val >> LOG2_BLOCK_SIZE);
  return static_cast<uint32_t>(mixed & (NUM_IP_OBJECT_CTX_ENTRIES - 1));
}

bool ipcp_cheri::load_object_state(uint64_t ip, uint64_t cap_base_val, IP_TABLE_L1_CHERI& tracker)
{
    uint32_t idx = object_state_index(ip, cap_base_val);
    auto& st = object_state_l1[idx];
    if (!(st.valid && st.ip == ip && st.cap_base == cap_base_val))
      return false;

    tracker.cap_base = st.cap_base;
    tracker.last_cl_offset = st.last_cl_offset;
    tracker.last_stride = st.last_stride;
    tracker.signature = st.signature;
    tracker.conf = st.conf;
    tracker.str_dir = st.str_dir;
    tracker.str_valid = st.str_valid;
    tracker.str_strength = st.str_strength;
    return true;
}

void ipcp_cheri::save_object_state(uint64_t ip, const IP_TABLE_L1_CHERI& tracker)
{
    uint32_t idx = object_state_index(ip, tracker.cap_base);
    auto& st = object_state_l1[idx];
    st.valid = 1;
    st.ip = ip;
    st.cap_base = tracker.cap_base;
    st.last_cl_offset = tracker.last_cl_offset;
    st.last_stride = tracker.last_stride;
    st.signature = tracker.signature;
    st.conf = tracker.conf;
    st.str_dir = tracker.str_dir;
    st.str_valid = tracker.str_valid;
    st.str_strength = tracker.str_strength;
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

void ipcp_cheri::prefetcher_final_stats()
{
  std::cout << std::endl;
  std::cout << "ipcp_cheri final stats" << std::endl;
  std::cout << "  Prefetches bounded by cap:       " << stat_pf_bounded_by_cap << std::endl;
  std::cout << "  Prefetches issued (CS):          " << stat_pf_issued_cs << std::endl;
  std::cout << "  Prefetches issued (CPLX):        " << stat_pf_issued_cplx << std::endl;
  std::cout << "  Prefetches issued (Stream):      " << stat_pf_issued_stream << std::endl;
  std::cout << "  Prefetches issued (NL):          " << stat_pf_issued_nl << std::endl;
}