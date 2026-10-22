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
//                          installed. C2 applies to an IP only while its C4 confidence is low
//                          (lines past its objects go unused); otherwise the paper's address
//                          signature is kept, since cross-object deltas between packed
//                          neighbours are the pattern;
//                      C3  object bounds: GS/CS/CPLX/NL prefetches stop at the end of an object
//                          of >= 2 KB;
//                      C4  learned out-of-object confidence for small objects: a prefetch that
//                          leaves a small object is decided by a 4-bit counter per (object size
//                          class, IP-table index): high issues to the L1D, middle to the L2C
//                          only, low drops. Lines issued to the L1D carry the counter's index in
//                          their prefetch metadata; a first demand use adds 7 and an eviction
//                          without use subtracts 1 (break-even accuracy 1/8). Probe lines (1 in
//                          8) always issue to the L1D, so a dropping key can recover.
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
    uint16_t size_class;       // C4: size class of a small object
    uint16_t ip_index;         // C4: IP-table index

    TRIGGER()
    {
      cl_addr = 0;
      tagged = 0;
      base = 0;
      top = 0;
      large = 0;
      size_class = 0;
      ip_index = 0;
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
  int oob_ctr[OOB_CLASSES][NUM_IP_TABLE_ENTRIES]; // 4-bit counters
  uint64_t stat_oob_candidates[OOB_CLASSES] = {};
  uint64_t stat_oob_to_l1[OOB_CLASSES] = {};
  uint64_t stat_oob_to_l2[OOB_CLASSES] = {};
  uint64_t stat_oob_dropped[OOB_CLASSES] = {};
  uint64_t stat_oob_probes[OOB_CLASSES] = {};
  uint64_t stat_oob_used[OOB_CLASSES] = {};
  uint64_t stat_oob_unused[OOB_CLASSES] = {};

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
  uint64_t stat_c2_active = 0;      // object changes with C2 on / off for the IP
  uint64_t stat_c2_inactive = 0;
  uint64_t stat_class_fills[4] = {}; // by class type
  uint64_t stat_class_useful[4] = {};
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
  bool allowed(const TRIGGER& trigger, uint64_t pf_cl_addr, bool& oob, bool& to_l2);
  void issue_prefetch(uint64_t pf_cl_addr, uint16_t type, int stride, bool nl, bool oob, bool to_l2, const TRIGGER& trigger);
  void oob_feedback(uint32_t metadata, bool used);
  void count_fill(uint16_t type);
  uint16_t update_sig(uint16_t old_sig, int stride);
  uint16_t object_seed(uint64_t ip, uint64_t obj_line);
  uint32_t encode_metadata(int stride, uint16_t type, bool oob, const TRIGGER& trigger);

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