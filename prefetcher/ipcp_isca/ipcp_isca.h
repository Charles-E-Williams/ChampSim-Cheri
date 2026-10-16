//=======================================================================================//
// File             : ipcp_isca/ipcp_isca.h
// Description      : IPCP at the L1D as specified in Pakalapati & Panda, "Bouquet of
//                    Instruction Pointers", ISCA'20 (Sec. IV-V, Fig. 5, Table I), at the
//                    paper's sizes. Baseline for ipcp_cap.
//
//                    Deviations from the paper:
//                      - the IP-table index skips PC bit 0 (RISC-V: 2-byte instruction alignment);
//                      - tentative NL uses misses per kilo-cycle (threshold 15, 256-miss window),
//                        as in the authors' DPC-3 code;
//                      - prefetches stop at the OS page; strides are learned at 4 KB granularity;
//                      - the L2 half of IPCP is not modelled.
//=======================================================================================//

#ifndef IPCP_ISCA_H
#define IPCP_ISCA_H

#include <bitset>
#include <cstdint>

#include "champsim.h"
#include "ipcp_isca_vars.h"
#include "modules.h"

struct ipcp_isca : public champsim::modules::prefetcher {
  friend struct ipcp_test_access; // unit tests (test/cpp/src/460-*)

  class IP_TABLE
  {
  public:
    uint16_t ip_tag;          // IP tag
    uint16_t ip_valid;        // hysteresis bit
    uint8_t last_vpage;       // 2 lsbs of the 4 KB virtual page
    uint8_t last_line_offset; // 6-bit line offset in the 4 KB page
    int last_stride;          // 7-bit sign-magnitude
    int conf;                 // 2-bit CS confidence
    uint16_t str_valid;       // GS: stream valid
    uint16_t str_dir;         // GS: stream direction, 1 = positive
    uint16_t signature;       // 7-bit CPLX signature

    IP_TABLE()
    {
      ip_tag = 0;
      ip_valid = 0;
      last_vpage = 0;
      last_line_offset = 0;
      last_stride = 0;
      conf = 0;
      str_valid = 0;
      str_dir = 0;
      signature = 0;
    };
  };

  class CSPT
  {
  public:
    int stride;
    int conf;

    CSPT()
    {
      stride = 0;
      conf = 0;
    };
  };

  class RST
  {
  public:
    uint16_t valid;
    uint16_t region_id;  // 3-bit region-id (Fig. 5): 2 vpage lsbs + region msb within the 4 KB page
    uint8_t last_offset; // 5-bit line offset in the region
    std::bitset<NUM_LINES_IN_REGION> line_access;
    int pos_neg_count; // 6-bit stream direction counter
    uint16_t trained_dense;
    uint16_t tentative_dense;
    int lru; // 3 bits

    RST()
    {
      valid = 0;
      region_id = 0;
      last_offset = 0;
      line_access.reset();
      pos_neg_count = 1 << (POSNEG_BITS - 1);
      trained_dense = 0;
      tentative_dense = 0;
      lru = 0;
    };
  };

  class CLASS_THROTTLE
  {
  public:
    unsigned fills;  // prefetch fills in the current window
    unsigned useful; // first demand uses in the current window
    int degree;
    uint16_t low; // accuracy below ACC_LOW in the last window

    CLASS_THROTTLE()
    {
      fills = 0;
      useful = 0;
      degree = 0;
      low = 0;
    };
  };

private:
  IP_TABLE trackers[NUM_IP_TABLE_ENTRIES];
  CSPT cspt[NUM_CSPT_ENTRIES];
  RST rst[NUM_RST_ENTRIES];
  uint16_t rr_tags[NUM_RR_ENTRIES] = {};
  uint16_t rr_valid[NUM_RR_ENTRIES] = {};
  int rr_next = 0;
  CLASS_THROTTLE throttle[4]; // indexed by class type; NONE_TYPE unused

  uint64_t cycle = 0;
  uint64_t window_start_cycle = 0;
  unsigned num_misses = 0;
  int spec_nl = 1;

  // statistics
  uint64_t stat_issued[5] = {}; // by class type, [NL_STAT] = NL
  uint64_t stat_rr_dropped = 0;
  uint64_t stat_page_stopped = 0;

  RST* rst_find(uint16_t region_id);
  RST* rst_update(uint16_t region_id, unsigned region_offset, bool gs_ip, uint16_t prev_region_id);
  bool rr_hit(uint64_t cl_addr);
  void rr_insert(uint64_t cl_addr);
  bool same_page(uint64_t cl_addr, uint64_t pf_cl_addr);
  void issue_prefetch(uint64_t pf_cl_addr, uint16_t type, int stride, bool nl);
  void count_fill(uint16_t type);
  uint16_t update_sig(uint16_t old_sig, int stride);
  uint32_t encode_metadata(int stride, uint16_t type);

public:
  using champsim::modules::prefetcher::prefetcher;

  // champsim interface prototypes
  void prefetcher_initialize();
  uint32_t prefetcher_cache_operate(champsim::address addr, champsim::address ip, uint32_t cpu, champsim::capability cap, uint8_t cache_hit,
                                    bool useful_prefetch, access_type type, uint32_t metadata_in, uint32_t metadata_hit);
  uint32_t prefetcher_cache_fill(champsim::address addr, champsim::address ip, uint32_t cpu, champsim::capability cap, bool useless, long set, long way,
                                 bool prefetch, champsim::address evicted_addr, champsim::capability evicted_cap, uint32_t metadata_in,
                                 uint32_t metadata_evict, uint32_t cpu_evict);
  void prefetcher_cycle_operate();
  void prefetcher_final_stats();
};

#endif /* IPCP_ISCA_H */