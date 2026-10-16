//=======================================================================================//
// File             : ipcp_isca/ipcp_isca.cc
// Description      : IPCP at the L1D, ISCA'20 (see ipcp_isca.h)
//=======================================================================================//

#include "ipcp_isca.h"

#include <cstdlib>
#include <iostream>

#include "cache.h"

void ipcp_isca::prefetcher_initialize()
{
  throttle[GS_TYPE].degree = GS_DEGREE;
  throttle[CS_TYPE].degree = CS_DEGREE;
  throttle[CPLX_TYPE].degree = CPLX_DEGREE;
  for (int index = 0; index < NUM_RST_ENTRIES; index++)
    rst[index].lru = index;

  std::cout << "IPCP_ISCA (L1D)" << std::endl
            << "NUM_IP_TABLE_ENTRIES " << NUM_IP_TABLE_ENTRIES << std::endl
            << "NUM_CSPT_ENTRIES " << NUM_CSPT_ENTRIES << std::endl
            << "NUM_RST_ENTRIES " << NUM_RST_ENTRIES << std::endl
            << "NUM_RR_ENTRIES " << NUM_RR_ENTRIES << std::endl
            << "GS/CS/CPLX degree " << GS_DEGREE << "/" << CS_DEGREE << "/" << CPLX_DEGREE << std::endl
            << std::endl;
}

void ipcp_isca::prefetcher_cycle_operate() { cycle++; }

uint32_t ipcp_isca::prefetcher_cache_operate(champsim::address address, champsim::address ip_addr, uint32_t cpu, champsim::capability cap, uint8_t cache_hit,
                                             bool useful_prefetch, access_type type, uint32_t metadata_in, uint32_t metadata_hit)
{
  uint64_t addr = address.to<uint64_t>();
  uint64_t ip = ip_addr.to<uint64_t>();
  uint64_t cl_addr = addr >> LOG2_BLOCK_SIZE;
  uint8_t vpage = (addr >> LOG2_LEARN_PAGE) & 0x3;
  uint8_t cl_offset = static_cast<uint8_t>(cl_addr & ((1 << (LOG2_LEARN_PAGE - LOG2_BLOCK_SIZE)) - 1));
  uint16_t region_id = (vpage << 1) | (cl_offset >> LOG2_LINES_IN_REGION);
  unsigned region_offset = cl_offset & (NUM_LINES_IN_REGION - 1);

  // throttling feedback: first demand use of a prefetched line (class kept in the line's metadata)
  if (useful_prefetch) {
    uint16_t pf_type = (metadata_hit >> 8) & 0x3;
    if (pf_type != NONE_TYPE)
      throttle[pf_type].useful++;
  }

  // tentative NL from misses per kilo-cycle
  if (cache_hit == 0 && ++num_misses == NL_MISS_WINDOW) {
    uint64_t elapsed = cycle - window_start_cycle;
    double mpkc = (elapsed == 0) ? 1e9 : 1000.0 * NL_MISS_WINDOW / (double)elapsed;
    spec_nl = (mpkc <= NL_MPKC_THRESHOLD) ? 1 : 0;
    window_start_cycle = cycle;
    num_misses = 0;
  }

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
    } else { // otherwise, reset valid bit and leave the previous IP as it is
      entry.ip_valid = 0;
    }
    rst_update(region_id, region_offset, false, 0);
    // an IP with no class: tentative NL
    if (spec_nl && same_page(cl_addr, cl_addr + 1))
      issue_prefetch(cl_addr + 1, NONE_TYPE, 1, true);
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

  if (stride_valid) {
    // update constant stride (CS) confidence; update CS only if confidence is zero
    if (stride == entry.last_stride)
      entry.conf = (entry.conf < 3) ? entry.conf + 1 : 3;
    else
      entry.conf = (entry.conf > 0) ? entry.conf - 1 : 0;
    if (entry.conf == 0)
      entry.last_stride = stride;

    // update complex stride (CPLX) confidence; update CPLX only if confidence is zero
    CSPT& cspt_entry = cspt[entry.signature];
    if (stride == cspt_entry.stride)
      cspt_entry.conf = (cspt_entry.conf < 3) ? cspt_entry.conf + 1 : 3;
    else
      cspt_entry.conf = (cspt_entry.conf > 0) ? cspt_entry.conf - 1 : 0;
    if (cspt_entry.conf == 0)
      cspt_entry.stride = stride;

    entry.signature = update_sig(entry.signature, stride);
  }

  entry.last_vpage = vpage;
  entry.last_line_offset = cl_offset;

  // prediction: GS > CS > CPLX > tentative NL
  int num_prefs = 0;

  bool gs_issued = false;
  if (entry.str_valid == 1) { // GS IP
    for (int step = 1; step <= throttle[GS_TYPE].degree; step++) {
      uint64_t pf_cl_addr = (entry.str_dir == 1) ? cl_addr + step : cl_addr - step;
      if (!same_page(cl_addr, pf_cl_addr))
        break;
      issue_prefetch(pf_cl_addr, GS_TYPE, (entry.str_dir == 1) ? 1 : -1, false);
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
      if (!same_page(cl_addr, pf_cl_addr))
        break;
      issue_prefetch(pf_cl_addr, CS_TYPE, entry.last_stride, false);
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
      if (!same_page(cl_addr, pf_cl_addr))
        break;
      if (cspt_entry.conf >= 1) { // prefetch only when conf > 0 for CPLX
        issue_prefetch(pf_cl_addr, CPLX_TYPE, cspt_entry.stride, false);
        num_prefs++;
      }
      signature = update_sig(signature, cspt_entry.stride);
    }
  }

  // if no prefetches are issued till now, speculatively issue a next-line prefetch
  if (num_prefs == 0 && spec_nl && same_page(cl_addr, cl_addr + 1))
    issue_prefetch(cl_addr + 1, NONE_TYPE, 1, true);

  return metadata_in;
}

uint32_t ipcp_isca::prefetcher_cache_fill(champsim::address addr, champsim::address ip, uint32_t cpu, champsim::capability cap, bool useless, long set, long way,
                                          bool prefetch, champsim::address evicted_addr, champsim::capability evicted_cap, uint32_t metadata_in,
                                          uint32_t metadata_evict, uint32_t cpu_evict)
{
  // the line keeps the 2-bit class of the prefetch that brought it in (Sec. V); demand fills carry none
  if (!prefetch)
    return 0;
  uint16_t pf_type = (metadata_in >> 8) & 0x3;
  if (pf_type != NONE_TYPE)
    count_fill(pf_type);
  return (uint32_t)pf_type << 8;
}

void ipcp_isca::prefetcher_final_stats()
{
  std::cout << "ipcp_isca final stats" << std::endl
            << "  Issued GS/CS/CPLX/NL:        " << stat_issued[GS_TYPE] << " / " << stat_issued[CS_TYPE] << " / " << stat_issued[CPLX_TYPE] << " / "
            << stat_issued[NL_STAT] << std::endl
            << "  Dropped by RR filter:        " << stat_rr_dropped << std::endl
            << "  Stopped at page boundary:    " << stat_page_stopped << std::endl
            << "  Final degrees GS/CS/CPLX:    " << throttle[GS_TYPE].degree << " / " << throttle[CS_TYPE].degree << " / " << throttle[CPLX_TYPE].degree
            << std::endl;
}

/***************Updating the signature*************************************/
uint16_t ipcp_isca::update_sig(uint16_t old_sig, int stride)
{
  // 7-bit sign magnitude form, since we need to track strides from +63 to -63
  uint32_t sig_stride = (stride < 0) ? ((uint32_t)(-stride) | 0x40) : (uint32_t)stride;
  return ((old_sig << 1) ^ sig_stride) & (NUM_CSPT_ENTRIES - 1);
}

/****************Encoding the metadata***********************************/
uint32_t ipcp_isca::encode_metadata(int stride, uint16_t type)
{
  // stride in the low 7 bits (sign magnitude), class type in bits 8-9
  uint32_t metadata = (stride >= 0) ? (uint32_t)stride : ((uint32_t)(-stride) | 0x40);
  return metadata | ((uint32_t)type << 8);
}

/*********************Region stream table (GS class)***********************/
ipcp_isca::RST* ipcp_isca::rst_find(uint16_t region_id)
{
  for (int index = 0; index < NUM_RST_ENTRIES; index++)
    if (rst[index].valid && rst[index].region_id == region_id)
      return &rst[index];
  return nullptr;
}

ipcp_isca::RST* ipcp_isca::rst_update(uint16_t region_id, unsigned region_offset, bool gs_ip, uint16_t prev_region_id)
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
bool ipcp_isca::rr_hit(uint64_t cl_addr)
{
  uint16_t tag = cl_addr & ((1 << RR_TAG_BITS) - 1);
  for (int index = 0; index < NUM_RR_ENTRIES; index++)
    if (rr_valid[index] && rr_tags[index] == tag)
      return true;
  return false;
}

void ipcp_isca::rr_insert(uint64_t cl_addr)
{
  if (rr_hit(cl_addr))
    return;
  rr_tags[rr_next] = cl_addr & ((1 << RR_TAG_BITS) - 1);
  rr_valid[rr_next] = 1;
  rr_next = (rr_next + 1) % NUM_RR_ENTRIES;
}

/*********************Issuing prefetches**********************************/
bool ipcp_isca::same_page(uint64_t cl_addr, uint64_t pf_cl_addr)
{
  // crossing the OS page needs another translation
  if ((pf_cl_addr >> (LOG2_PAGE_SIZE - LOG2_BLOCK_SIZE)) != (cl_addr >> (LOG2_PAGE_SIZE - LOG2_BLOCK_SIZE))) {
    stat_page_stopped++;
    return false;
  }
  return true;
}

void ipcp_isca::issue_prefetch(uint64_t pf_cl_addr, uint16_t type, int stride, bool nl)
{
  if (rr_hit(pf_cl_addr)) {
    stat_rr_dropped++;
    return;
  }
  rr_insert(pf_cl_addr);
  if (prefetch_line(champsim::address{pf_cl_addr << LOG2_BLOCK_SIZE}, true, encode_metadata(stride, type)))
    stat_issued[nl ? NL_STAT : type]++;
}

/**************************Coordinated throttling*************************/
void ipcp_isca::count_fill(uint16_t type)
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