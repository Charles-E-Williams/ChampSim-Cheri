#ifndef IPCP_CHERI_H
#define IPCP_CHERI_H

#include "champsim.h"
#include "ipcp_cheri_vars.h"
#include "modules.h"
#include "cheri_prefetch_utils.h"

class IP_TABLE_L1_CHERI
{
public:
  uint64_t ip_tag;
  int64_t last_cl_offset;    // last cl offset relative to capability
  int64_t last_stride;       // last delta observed
  uint16_t ip_valid;         // valid IP or not
  int conf;                  // CS confidence
  uint16_t signature;        // CPLX signature
  uint16_t str_dir;          // stream direction
  uint16_t str_valid;        // stream valid
  uint16_t str_strength;     // stream strength

  // CHERI capability context for this IP
  uint64_t cap_base;
  uint64_t cap_length;

  IP_TABLE_L1_CHERI()
  {
    ip_tag = 0;
    last_cl_offset = 0;
    last_stride = 0;
    ip_valid = 0;
    signature = 0;
    conf = 0;
    str_dir = 0;
    str_valid = 0;
    str_strength = 0;
    cap_base = 0;
    cap_length = 0;
  };
};

class DELTA_PRED_TABLE_CHERI
{
public:
  int delta;
  int conf;

  DELTA_PRED_TABLE_CHERI()
  {
    delta = 0;
    conf = 0;
  };
};

struct ghb_entry {
  int64_t cap_cl_offset;
  uint64_t cap_base;
};

class IP_OBJECT_STATE_CHERI
{
public:
  uint64_t ip;
  uint64_t cap_base;
  uint16_t valid;
  int64_t last_cl_offset;
  int64_t last_stride;
  uint16_t signature;
  int conf;
  uint16_t str_dir;
  uint16_t str_valid;
  uint16_t str_strength;

  IP_OBJECT_STATE_CHERI()
  {
    ip = 0;
    cap_base = 0;
    valid = 0;
    last_cl_offset = 0;
    last_stride = 0;
    signature = 0;
    conf = 0;
    str_dir = 0;
    str_valid = 0;
    str_strength = 0;
  };
};
 
class REGION_STREAM_TABLE_CHERI
{
public:
  uint64_t cap_base;
  int64_t last_cl_offset;
  uint16_t valid;
  uint16_t dir;
  uint16_t run_length;

  REGION_STREAM_TABLE_CHERI()
  {
    cap_base = 0;
    last_cl_offset = 0;
    valid = 0;
    dir = 0;
    run_length = 0;
  };
};

struct ipcp_cheri : public champsim::modules::prefetcher {
private:
  static constexpr int NUM_IP_OBJECT_CTX_ENTRIES = 2048;
  static constexpr int NUM_REGION_STREAM_ENTRIES = 64;
  IP_TABLE_L1_CHERI trackers_l1[NUM_IP_TABLE_L1_ENTRIES];
  IP_OBJECT_STATE_CHERI object_state_l1[NUM_IP_OBJECT_CTX_ENTRIES];
  DELTA_PRED_TABLE_CHERI DPT_l1[4096];
  ghb_entry ghb_l1[NUM_GHB_ENTRIES];
  REGION_STREAM_TABLE_CHERI region_stream_l1[NUM_REGION_STREAM_ENTRIES];
  int64_t prev_cpu_cycle;
  uint64_t num_misses;
  float mpkc = {0};
  int spec_nl = {0};

  // CHERI statistics
  uint64_t stat_pf_bounded_by_cap = 0;   // prefetches clipped by capability bounds
  uint64_t stat_pf_issued_cs = 0;
  uint64_t stat_pf_issued_cplx = 0;
  uint64_t stat_pf_issued_stream = 0;
  uint64_t stat_pf_issued_nl = 0;

  uint16_t update_sig_l1(uint16_t old_sig, int delta);
  uint32_t encode_metadata(int stride, uint16_t type, int spec_nl);
  void check_for_stream_l1(int index, uint64_t cap_base_val);
  void check_for_region_stream_l1(int index, uint64_t cap_base_val, int64_t cl_offset);
  uint32_t object_state_index(uint64_t ip, uint64_t cap_base_val);
  bool load_object_state(uint64_t ip, uint64_t cap_base_val, IP_TABLE_L1_CHERI& tracker);
  void save_object_state(uint64_t ip, const IP_TABLE_L1_CHERI& tracker);
  int update_conf(int stride, int pred_stride, int conf);


public:
  using champsim::modules::prefetcher::prefetcher;

  void prefetcher_initialize();
  uint32_t prefetcher_cache_operate(champsim::address addr, champsim::address ip, uint8_t cache_hit,
                                    bool useful_prefetch, access_type type, uint32_t metadata_in);
  uint32_t prefetcher_cache_fill(champsim::address addr, long set, long way, uint8_t prefetch,
                                 champsim::address evicted_addr, uint32_t metadata_in);
  void prefetcher_cycle_operate();
  void prefetcher_final_stats();
};

#endif