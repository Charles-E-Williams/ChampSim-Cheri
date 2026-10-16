//=======================================================================================//
// File             : ipcp_cap/ipcp_cap.h
// Description      : Capability-aware IPCP at the L1D: the paper's L1 IPCP (ipcp_isca) plus
//                      C1  object-change-aware CS training: a stride mismatch that coincides
//                          with a change of object does not count against a stride that was
//                          learned inside an object;
//                      C2  object-entry CPLX signatures: when an IP enters a new object, its
//                          signature is reseeded from (IP, line within the object), so the
//                          CSPT learns per-IP in-object footprints; a cross-object delta
//                          updates the confidence of the prediction it tests but is never
//                          installed;
//                      C3  object bounds: GS/CS/CPLX/NL prefetches stop at the end of an object
//                          of >= 2 KB; CS also stops at the end of any object whose stride was
//                          learned inside one object, and CPLX at the end of any object (its
//                          deltas are in-object deltas, C2). Small objects are otherwise not
//                          clipped: GS/NL and cross-object strides keep reaching neighbours;
//                      C4  learned bounds filter for small objects: prefetches that leave a
//                          small object are measured in 8 leader sets of the L1D (1 bit per
//                          leader line, next to the class bits); a 2-bit trust counter moves
//                          down on each window whose accuracy is below the paper's low
//                          watermark and up otherwise, and at zero they are blocked in the
//                          other sets.
//                    Untagged accesses take the paper's path. Apart from C1-C4 the logic is
//                    the same as ipcp_isca (test 460-6 checks this on untagged input).
//=======================================================================================//

#ifndef IPCP_CAP_H
#define IPCP_CAP_H

#include <bitset>
#include <cstdint>

#include "champsim.h"
#include "cheri.h"
#include "ipcp_cap_vars.h"
#include "modules.h"

struct ipcp_cap : public champsim::modules::prefetcher {
  friend struct ipcp_test_access; // unit tests (test/cpp/src/460-*)

  class IP_TABLE
  {
  public:
    uint16_t ip_tag;           // IP tag
    uint16_t ip_valid;         // hysteresis bit
    uint8_t last_vpage;        // 2 lsbs of the 4 KB virtual page
    uint8_t last_line_offset;  // 6-bit line offset in the 4 KB page
    int last_stride;           // 7-bit sign-magnitude
    int conf;                  // 2-bit CS confidence
    uint16_t str_valid;        // GS: stream valid
    uint16_t str_dir;          // GS: stream direction, 1 = positive
    uint16_t signature;        // 7-bit CPLX signature

    // capability layer
    uint8_t obj_tag;           // C1/C2: OBJ_TAG_BITS hash of the last object's base
    uint16_t obj_valid;        // the last access of this IP was tagged
    uint16_t intra;            // C1: the stride was last confirmed between two accesses to the same object
    uint64_t sim_obj_base;     // simulation only (not hardware): full base of the last object, to count tag aliasing

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
      obj_tag = 0;
      obj_valid = 0;
      intra = 0;
      sim_obj_base = 0;
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
    uint16_t region_id;        // 3-bit region-id (Fig. 5): 2 vpage lsbs + region msb within the 4 KB page
    uint8_t last_offset;       // 5-bit line offset in the region
    std::bitset<NUM_LINES_IN_REGION> line_access;
    int pos_neg_count;         // 6-bit stream direction counter
    uint16_t trained_dense;
    uint16_t tentative_dense;
    int lru;                   // 3 bits

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
    unsigned fills;            // prefetch fills in the current window
    unsigned useful;           // first demand uses in the current window
    int degree;
    uint16_t low;              // accuracy below ACC_LOW in the last window

    CLASS_THROTTLE()
    {
      fills = 0;
      useful = 0;
      degree = 0;
      low = 0;
    };
  };

  // Capability of one demand access, as the prefetcher uses it
  class TRIGGER
  {
  public:
    uint64_t cl_addr;
    uint16_t tagged;
    uint64_t base;
    uint64_t top;
    uint16_t large;            // object of at least LARGE_OBJECT_BYTES

    TRIGGER()
    {
      cl_addr = 0;
      tagged = 0;
      base = 0;
      top = 0;
      large = 0;
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

  // C4 state
  unsigned oob_fills = 0;
  unsigned oob_useful = 0;
  int oob_trust = 3; // 2-bit; blocked at 0
  bool oob_blocked = false;

  uint64_t cycle = 0;
  uint64_t window_start_cycle = 0;
  unsigned num_misses = 0;
  int spec_nl = 1;

  // statistics
  uint64_t stat_issued[5] = {}; // by class type, [4] = NL
  uint64_t stat_rr_dropped = 0;
  uint64_t stat_page_stopped = 0;
  uint64_t stat_cap_stopped = 0;
  uint64_t stat_cs_protected = 0;
  uint64_t stat_cplx_seeded = 0;
  uint64_t stat_cplx_from_seed = 0; // CPLX prefetches issued on the access that entered an object
  uint64_t stat_oob_issued = 0;
  uint64_t stat_oob_filtered = 0;
  uint64_t stat_oob_windows = 0;
  uint64_t stat_oob_blocked_windows = 0;
  uint64_t stat_tagged = 0;
  uint64_t stat_untagged = 0;
  // path breakdown of IP-table hits: see prefetcher_final_stats
  uint64_t stat_path_untagged = 0;
  uint64_t stat_path_same_object = 0;
  uint64_t stat_path_object_changed = 0;
  uint64_t stat_path_alias = 0;        // different object, same object tag: handled as the same object
  uint64_t stat_path_first_tagged = 0; // previous access of this IP was untagged
  uint64_t stat_path_large = 0;        // subset of the above: trigger in an object >= 2 KB
  uint64_t stat_ip_miss_tagged = 0;    // IP-table misses (no IPCP state; NL only)
  uint64_t stat_ip_miss_untagged = 0;

  RST* rst_find(uint16_t region_id);
  RST* rst_update(uint16_t region_id, unsigned region_offset, bool gs_ip, uint16_t prev_region_id);
  bool rr_hit(uint64_t cl_addr);
  void rr_insert(uint64_t cl_addr);
  // Returns false (and counts why) when the candidate must not be issued and prefetching in this direction stops.
  bool allowed(const TRIGGER& trigger, uint64_t pf_cl_addr, uint16_t type, bool nl, bool stop_at_object_end, bool& oob);
  void issue_prefetch(uint64_t pf_cl_addr, uint16_t type, int stride, bool nl, bool oob);
  void count_fill(uint16_t type);
  uint16_t update_sig(uint16_t old_sig, int stride);
  uint16_t object_seed(uint64_t ip, uint64_t obj_line);
  uint32_t encode_metadata(int stride, uint16_t type, bool oob);

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

#endif /* IPCP_CAP_H */