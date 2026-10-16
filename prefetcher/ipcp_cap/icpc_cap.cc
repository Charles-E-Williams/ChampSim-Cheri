//=======================================================================================//
// File             : ipcp_cap/ipcp_cap.cc
// Description      : Capability-aware IPCP at the L1D (see ipcp_cap.h)
//=======================================================================================//

#include "ipcp_cap.h"

#include <cstdlib>
#include <iostream>

#include "cache.h"
#include "cheri_prefetch_utils.h"

namespace
{
// XOR-fold value into its low `bits` bits
uint16_t fold(uint64_t value, unsigned bits)
{
  uint64_t folded = 0;
  while (value != 0) {
    folded ^= value & ((1ull << bits) - 1);
    value >>= bits;
  }
  return static_cast<uint16_t>(folded);
}
} // namespace

void ipcp_cap::prefetcher_initialize()
{
  throttle[GS_TYPE].degree = GS_DEGREE;
  throttle[CS_TYPE].degree = CS_DEGREE;
  throttle[CPLX_TYPE].degree = CPLX_DEGREE;
  for (int index = 0; index < NUM_RST_ENTRIES; index++)
    rst[index].lru = index;

  std::cout << "IPCP_CAP (L1D)" << std::endl
            << "NUM_IP_TABLE_ENTRIES " << NUM_IP_TABLE_ENTRIES << std::endl
            << "NUM_CSPT_ENTRIES " << NUM_CSPT_ENTRIES << std::endl
            << "NUM_RST_ENTRIES " << NUM_RST_ENTRIES << std::endl
            << "NUM_RR_ENTRIES " << NUM_RR_ENTRIES << std::endl
            << "GS/CS/CPLX degree " << GS_DEGREE << "/" << CS_DEGREE << "/" << CPLX_DEGREE << std::endl
            << "CHERI-aware" << std::endl
            << std::endl;
}

void ipcp_cap::prefetcher_cycle_operate() { cycle++; }

uint32_t ipcp_cap::prefetcher_cache_operate(champsim::address address, champsim::address ip_addr, uint32_t cpu, champsim::capability cap, uint8_t cache_hit,
                                            bool useful_prefetch, access_type type, uint32_t metadata_in, uint32_t metadata_hit)
{
  uint64_t addr = address.to<uint64_t>();
  uint64_t ip = ip_addr.to<uint64_t>();
  uint64_t cl_addr = addr >> LOG2_BLOCK_SIZE;
  uint8_t vpage = static_cast<uint8_t>((addr >> LOG2_LEARN_PAGE) & 0x3);
  uint8_t cl_offset = static_cast<uint8_t>(cl_addr & ((1 << (LOG2_LEARN_PAGE - LOG2_BLOCK_SIZE)) - 1));
  uint16_t region_id = (vpage << 1) | (cl_offset >> LOG2_LINES_IN_REGION);
  unsigned region_offset = cl_offset & (NUM_LINES_IN_REGION - 1);

  // throttling feedback: first demand use of a prefetched line (class and out-of-object bit kept in its metadata)
  if (useful_prefetch) {
    uint16_t pf_type = (metadata_hit >> 8) & 0x3;
    if (pf_type != NONE_TYPE)
      throttle[pf_type].useful++;
    if ((metadata_hit >> 10) & 0x1)
      oob_useful++;
  }

  // tentative NL from misses per kilo-cycle
  if (cache_hit == 0 && ++num_misses == NL_MISS_WINDOW) {
    uint64_t elapsed = cycle - window_start_cycle;
    double mpkc = (elapsed == 0) ? 1e9 : 1000.0 * NL_MISS_WINDOW / (double)elapsed;
    spec_nl = (mpkc <= NL_MPKC_THRESHOLD) ? 1 : 0;
    window_start_cycle = cycle;
    num_misses = 0;
  }

  // the capability of this access
  TRIGGER trigger;
  trigger.cl_addr = cl_addr;
  trigger.tagged = cheri::is_tag_valid(cap) ? 1 : 0;
  if (trigger.tagged) {
    trigger.base = cap.base.to<uint64_t>();
    trigger.top = cheri::capability_top(cap).to<uint64_t>();
    trigger.large = (cap.length.to<uint64_t>() >= LARGE_OBJECT_BYTES) ? 1 : 0;
    stat_tagged++;
  } else {
    stat_untagged++;
  }
  uint8_t obj_tag = trigger.tagged ? static_cast<uint8_t>(fold(trigger.base >> cheri::CAP_ALIGNMENT_BITS, OBJ_TAG_BITS)) : 0;
  uint64_t base_cl_addr = trigger.base >> LOG2_BLOCK_SIZE;
  uint64_t obj_line = (trigger.tagged && cl_addr >= base_cl_addr) ? cl_addr - base_cl_addr : 0;

  rr_insert(cl_addr);

  int index = (ip >> NUM_IP_ALIGN_BITS) & ((1 << NUM_IP_INDEX_BITS) - 1);
  uint16_t ip_tag = (ip >> (NUM_IP_ALIGN_BITS + NUM_IP_INDEX_BITS)) & ((1 << NUM_IP_TAG_BITS) - 1);
  IP_TABLE& entry = trackers[index];

  if (entry.ip_tag != ip_tag) { // new/conflict IP
    if (entry.ip_valid == 0) {  // if valid bit is zero, update with latest IP info
      entry = IP_TABLE();
      entry.ip_tag = ip_tag;
      entry.ip_valid = 1;
      entry.last_vpage = vpage;
      entry.last_line_offset = cl_offset;
      entry.obj_valid = trigger.tagged;
      entry.obj_tag = obj_tag;
      entry.sim_obj_base = trigger.base;
      if (trigger.tagged)
        entry.signature = object_seed(ip, obj_line);
    } else { // otherwise, reset valid bit and leave the previous IP as it is
      entry.ip_valid = 0;
    }
    rst_update(region_id, region_offset, false, 0);
    if (trigger.tagged)
      stat_ip_miss_tagged++;
    else
      stat_ip_miss_untagged++;
    // an IP with no class: tentative NL
    bool oob;
    if (spec_nl && allowed(trigger, cl_addr + 1, NONE_TYPE, true, false, oob))
      issue_prefetch(cl_addr + 1, NONE_TYPE, 1, true, oob);
    return metadata_in;
  }
  entry.ip_valid = 1;

  // stride from last-vpage (2 lsbs) and last-line-offset (Sec. IV-A)
  unsigned pdiff = (vpage - entry.last_vpage) & 0x3;
  int page_adjust = (pdiff == 1) ? 64 : ((pdiff == 3) ? -64 : 0);
  int stride = page_adjust + (int)cl_offset - (int)entry.last_line_offset;
  bool stride_valid = (pdiff != 2) && (std::abs(stride) <= MAX_STRIDE);

  // GS: train the RST and classify the IP from it. The IP's previous region is its last-vpage and the msb of its
  // last-line-offset (Sec. IV-C).
  uint16_t prev_region_id = (entry.last_vpage << 1) | (entry.last_line_offset >> LOG2_LINES_IN_REGION);
  RST* region = rst_update(region_id, region_offset, entry.str_valid == 1, prev_region_id);
  entry.str_valid = (region->trained_dense || region->tentative_dense) ? 1 : 0;
  entry.str_dir = (region->pos_neg_count >= (1 << (POSNEG_BITS - 1))) ? 1 : 0;

  // don't do anything if same address is seen twice in a row
  if (stride_valid && stride == 0)
    return metadata_in;

  bool obj_changed = trigger.tagged && entry.obj_valid && entry.obj_tag != obj_tag;
  if (!trigger.tagged)
    stat_path_untagged++;
  else if (!entry.obj_valid)
    stat_path_first_tagged++;
  else if (obj_changed)
    stat_path_object_changed++;
  else if (entry.sim_obj_base != trigger.base)
    stat_path_alias++;
  else
    stat_path_same_object++;
  if (trigger.tagged && trigger.large)
    stat_path_large++;

  if (stride_valid) {
    // update constant stride (CS) confidence; update CS only if confidence is zero.
    // C1: a mismatch on an object change does not count against a stride learned inside an object.
    bool protect = obj_changed && entry.intra && entry.conf > 0 && stride != entry.last_stride;
    if (protect) {
      stat_cs_protected++;
    } else {
      if (stride == entry.last_stride) {
        entry.conf = (entry.conf < 3) ? entry.conf + 1 : 3;
        entry.intra = (trigger.tagged && !obj_changed) ? 1 : 0;
      } else {
        entry.conf = (entry.conf > 0) ? entry.conf - 1 : 0;
      }
      if (entry.conf == 0) {
        entry.last_stride = stride;
        entry.intra = 0;
      }
    }

    // update complex stride (CPLX) confidence; update CPLX only if confidence is zero.
    // C2: a cross-object delta can confirm or refute a prediction, but is never installed.
    CSPT& cspt_entry = cspt[entry.signature];
    if (stride == cspt_entry.stride)
      cspt_entry.conf = (cspt_entry.conf < 3) ? cspt_entry.conf + 1 : 3;
    else
      cspt_entry.conf = (cspt_entry.conf > 0) ? cspt_entry.conf - 1 : 0;
    if (cspt_entry.conf == 0 && !obj_changed)
      cspt_entry.stride = stride;
  }

  if (obj_changed) { // C2: the IP entered a new object
    entry.signature = object_seed(ip, obj_line);
    stat_cplx_seeded++;
  } else if (stride_valid) {
    entry.signature = update_sig(entry.signature, stride);
  }

  entry.obj_valid = trigger.tagged;
  entry.obj_tag = obj_tag;
  entry.sim_obj_base = trigger.base;
  entry.last_vpage = vpage;
  entry.last_line_offset = cl_offset;

  // prediction: GS > CS > CPLX > tentative NL
  int num_prefs = 0;

  bool gs_issued = false;
  if (entry.str_valid == 1) { // GS IP
    for (int step = 1; step <= throttle[GS_TYPE].degree; step++) {
      uint64_t pf_cl_addr = (entry.str_dir == 1) ? cl_addr + step : cl_addr - step;
      bool oob;
      if (!allowed(trigger, pf_cl_addr, GS_TYPE, false, false, oob))
        break;
      issue_prefetch(pf_cl_addr, GS_TYPE, (entry.str_dir == 1) ? 1 : -1, false, oob);
      num_prefs++;
    }
    gs_issued = true;
  }

  // with GS accuracy low, IPCP also explores the other classes (Sec. V)
  bool explore = !gs_issued || throttle[GS_TYPE].low;
  bool cs_issued = false;
  if (explore && entry.conf > 1 && entry.last_stride != 0) { // CS IP
    for (int step = 1; step <= throttle[CS_TYPE].degree; step++) {
      uint64_t pf_cl_addr = (uint64_t)((int64_t)cl_addr + (int64_t)entry.last_stride * step);
      bool oob;
      if (!allowed(trigger, pf_cl_addr, CS_TYPE, false, entry.intra, oob))
        break;
      issue_prefetch(pf_cl_addr, CS_TYPE, entry.last_stride, false, oob);
      num_prefs++;
    }
    cs_issued = true;
  }

  if (explore && !cs_issued) { // CPLX IP
    uint16_t signature = entry.signature;
    int64_t pref_offset = 0;
    for (int step = 0; step < throttle[CPLX_TYPE].degree; step++) {
      CSPT& cspt_entry = cspt[signature];
      if (cspt_entry.stride == 0)
        break;
      pref_offset += cspt_entry.stride;
      uint64_t pf_cl_addr = (uint64_t)((int64_t)cl_addr + pref_offset);
      bool oob;
      if (!allowed(trigger, pf_cl_addr, CPLX_TYPE, false, false, oob))
        break;
      if (cspt_entry.conf >= 1) { // prefetch only when conf > 0 for CPLX
        issue_prefetch(pf_cl_addr, CPLX_TYPE, cspt_entry.stride, false, oob);
        num_prefs++;
        if (obj_changed)
          stat_cplx_from_seed++;
      }
      signature = update_sig(signature, cspt_entry.stride);
    }
  }

  // if no prefetches are issued till now, speculatively issue a next-line prefetch
  bool oob;
  if (num_prefs == 0 && spec_nl && allowed(trigger, cl_addr + 1, NONE_TYPE, true, false, oob))
    issue_prefetch(cl_addr + 1, NONE_TYPE, 1, true, oob);

  return metadata_in;
}

uint32_t ipcp_cap::prefetcher_cache_fill(champsim::address addr, champsim::address ip, uint32_t cpu, champsim::capability cap, bool useless, long set, long way,
                                         bool prefetch, champsim::address evicted_addr, champsim::capability evicted_cap, uint32_t metadata_in,
                                         uint32_t metadata_evict, uint32_t cpu_evict)
{
  // the line keeps the 2-bit class of the prefetch that brought it in (Sec. V) and its out-of-object bit (C4);
  // demand fills carry none
  if (!prefetch)
    return 0;
  uint16_t pf_type = (metadata_in >> 8) & 0x3;
  if (pf_type != NONE_TYPE)
    count_fill(pf_type);

  bool oob = (metadata_in >> 10) & 0x1;
  if (oob && ++oob_fills == OOB_WINDOW) { // C4: re-decide once per window of leader-set out-of-object fills
    if ((double)oob_useful / (double)oob_fills < ACC_LOW)
      oob_trust = (oob_trust > 0) ? oob_trust - 1 : 0;
    else
      oob_trust = (oob_trust < 3) ? oob_trust + 1 : 3;
    oob_blocked = (oob_trust == 0);
    stat_oob_windows++;
    if (oob_blocked)
      stat_oob_blocked_windows++;
    oob_fills = 0;
    oob_useful = 0;
  }
  return ((uint32_t)pf_type << 8) | ((uint32_t)oob << 10);
}

void ipcp_cap::prefetcher_final_stats()
{
  std::cout << "ipcp_cap final stats" << std::endl
            << "  Issued GS/CS/CPLX/NL:        " << stat_issued[GS_TYPE] << " / " << stat_issued[CS_TYPE] << " / " << stat_issued[CPLX_TYPE] << " / "
            << stat_issued[NL_STAT] << std::endl
            << "  Dropped by RR filter:        " << stat_rr_dropped << std::endl
            << "  Stopped at page boundary:    " << stat_page_stopped << std::endl
            << "  Final degrees GS/CS/CPLX:    " << throttle[GS_TYPE].degree << " / " << throttle[CS_TYPE].degree << " / " << throttle[CPLX_TYPE].degree
            << std::endl
            << "  Tagged / untagged accesses:  " << stat_tagged << " / " << stat_untagged << std::endl
            << "  Path: IP-table miss tagged / untagged:     " << stat_ip_miss_tagged << " / " << stat_ip_miss_untagged << std::endl
            << "  Path: hit untagged (paper path):           " << stat_path_untagged << std::endl
            << "  Path: hit tagged, IP was untagged before:  " << stat_path_first_tagged << std::endl
            << "  Path: hit tagged, same object:             " << stat_path_same_object << std::endl
            << "  Path: hit tagged, object changed (C1/C2):  " << stat_path_object_changed << std::endl
            << "  Path: hit tagged, aliased object tag:      " << stat_path_alias << std::endl
            << "  Path: hit tagged in object >= 2 KB:        " << stat_path_large << std::endl
            << "  Stopped at object end (C3):  " << stat_cap_stopped << std::endl
            << "  CS strides protected (C1):   " << stat_cs_protected << std::endl
            << "  CPLX object reseeds (C2):    " << stat_cplx_seeded << std::endl
            << "  CPLX issued at object entry: " << stat_cplx_from_seed << std::endl
            << "  Out-of-object leader issues: " << stat_oob_issued << std::endl
            << "  Out-of-object filtered (C4): " << stat_oob_filtered << std::endl
            << "  C4 windows blocked / total:  " << stat_oob_blocked_windows << " / " << stat_oob_windows << std::endl;
}

/***************Updating the signature*************************************/
uint16_t ipcp_cap::update_sig(uint16_t old_sig, int stride)
{
  // 7-bit sign magnitude form, since we need to track strides from +63 to -63
  uint32_t sig_stride = (stride < 0) ? ((uint32_t)(-stride) | 0x40) : (uint32_t)stride;
  return ((old_sig << 1) ^ sig_stride) & (NUM_CSPT_ENTRIES - 1);
}

/***************C2: signature of an IP that just entered an object**********/
uint16_t ipcp_cap::object_seed(uint64_t ip, uint64_t obj_line)
{
  // The IP bits spread different IPs over the CSPT like ordinary signatures do; the line within the object is what
  // makes the following deltas object-relative.
  uint64_t line_field = (obj_line < 63) ? obj_line : 63;
  return (fold(ip >> NUM_IP_ALIGN_BITS, NUM_SIG_BITS) ^ line_field) & (NUM_CSPT_ENTRIES - 1);
}

/****************Encoding the metadata***********************************/
uint32_t ipcp_cap::encode_metadata(int stride, uint16_t type, bool oob)
{
  // stride in the low 7 bits (sign magnitude), class type in bits 8-9, out-of-object bit (C4) in bit 10
  uint32_t metadata = (stride >= 0) ? (uint32_t)stride : ((uint32_t)(-stride) | 0x40);
  return metadata | ((uint32_t)type << 8) | ((uint32_t)oob << 10);
}

/*********************Region stream table (GS class)***********************/
ipcp_cap::RST* ipcp_cap::rst_find(uint16_t region_id)
{
  for (int index = 0; index < NUM_RST_ENTRIES; index++)
    if (rst[index].valid && rst[index].region_id == region_id)
      return &rst[index];
  return nullptr;
}

ipcp_cap::RST* ipcp_cap::rst_update(uint16_t region_id, unsigned region_offset, bool gs_ip, uint16_t prev_region_id)
{
  RST* region = rst_find(region_id);
  if (region == nullptr) {
    // allocate the LRU entry
    region = &rst[0];
    for (int index = 0; index < NUM_RST_ENTRIES; index++)
      if (rst[index].lru > region->lru)
        region = &rst[index];
    // a GS IP entering a new region: if the region it came from was trained dense, assume this one is too (Sec. IV-C)
    uint16_t tentative = 0;
    if (gs_ip) {
      RST* prev = rst_find(prev_region_id);
      if (prev != nullptr && prev->trained_dense)
        tentative = 1;
    }
    int old_lru = region->lru;
    *region = RST();
    region->valid = 1;
    region->region_id = region_id;
    region->last_offset = static_cast<uint8_t>(region_offset);
    region->tentative_dense = tentative;
    region->lru = old_lru;
  } else if (region_offset != region->last_offset) {
    int max_count = (1 << POSNEG_BITS) - 1;
    if (region_offset > region->last_offset)
      region->pos_neg_count = (region->pos_neg_count < max_count) ? region->pos_neg_count + 1 : max_count;
    else
      region->pos_neg_count = (region->pos_neg_count > 0) ? region->pos_neg_count - 1 : 0;
    region->last_offset = static_cast<uint8_t>(region_offset);
  }
  region->line_access.set(region_offset);
  if (region->line_access.count() >= DENSE_THRESHOLD)
    region->trained_dense = 1;

  // LRU update
  for (int index = 0; index < NUM_RST_ENTRIES; index++)
    if (rst[index].lru < region->lru)
      rst[index].lru++;
  region->lru = 0;
  return region;
}

/*********************Recent request filter*******************************/
bool ipcp_cap::rr_hit(uint64_t cl_addr)
{
  uint16_t tag = cl_addr & ((1 << RR_TAG_BITS) - 1);
  for (int index = 0; index < NUM_RR_ENTRIES; index++)
    if (rr_valid[index] && rr_tags[index] == tag)
      return true;
  return false;
}

void ipcp_cap::rr_insert(uint64_t cl_addr)
{
  if (rr_hit(cl_addr))
    return;
  rr_tags[rr_next] = cl_addr & ((1 << RR_TAG_BITS) - 1);
  rr_valid[rr_next] = 1;
  rr_next = (rr_next + 1) % NUM_RR_ENTRIES;
}

/*********************Issuing prefetches**********************************/
bool ipcp_cap::allowed(const TRIGGER& trigger, uint64_t pf_cl_addr, uint16_t type, bool nl, bool stop_at_object_end, bool& oob)
{
  oob = false;
  // crossing the OS page needs another translation
  if ((pf_cl_addr >> (LOG2_PAGE_SIZE - LOG2_BLOCK_SIZE)) != (trigger.cl_addr >> (LOG2_PAGE_SIZE - LOG2_BLOCK_SIZE))) {
    stat_page_stopped++;
    return false;
  }
  if (trigger.tagged && !cheri::overlaps_bounds(champsim::block_number{pf_cl_addr}, champsim::address{trigger.base}, champsim::address{trigger.top})) {
    // C3: stop at the end of large objects. CS also stops at the end of an object its stride was learned inside, and
    // CPLX at the end of any object, since with C2 the CSPT only holds deltas learned inside objects.
    bool stop = trigger.large || (!nl && ((type == CS_TYPE && stop_at_object_end) || type == CPLX_TYPE));
    if (stop) {
      stat_cap_stopped++;
      return false;
    }
    // C4: leaving a small object. Leader sets always issue and measure; the others follow the decision.
    bool leader = (pf_cl_addr & OOB_LEADER_MASK) == 0;
    if (oob_blocked && !leader) {
      stat_oob_filtered++;
      return false;
    }
    oob = leader;
  }
  return true;
}

void ipcp_cap::issue_prefetch(uint64_t pf_cl_addr, uint16_t type, int stride, bool nl, bool oob)
{
  if (rr_hit(pf_cl_addr)) {
    stat_rr_dropped++;
    return;
  }
  rr_insert(pf_cl_addr);
  if (prefetch_line(champsim::address{pf_cl_addr << LOG2_BLOCK_SIZE}, true, encode_metadata(stride, type, oob))) {
    stat_issued[nl ? NL_STAT : type]++;
    if (oob)
      stat_oob_issued++; // leader sets only
  }
}

/**************************Coordinated throttling*************************/
void ipcp_cap::count_fill(uint16_t type)
{
  CLASS_THROTTLE& class_throttle = throttle[type];
  if (++class_throttle.fills < THROTTLE_WINDOW)
    return;
  int max_degree = (type == GS_TYPE) ? GS_DEGREE : ((type == CS_TYPE) ? CS_DEGREE : CPLX_DEGREE);
  double acc = (double)class_throttle.useful / (double)class_throttle.fills;
  if (acc > ACC_HIGH && class_throttle.degree < max_degree)
    class_throttle.degree++;
  else if (acc < ACC_LOW && class_throttle.degree > 1)
    class_throttle.degree--;
  class_throttle.low = (acc < ACC_LOW) ? 1 : 0;
  class_throttle.fills = 0;
  class_throttle.useful = 0;
}