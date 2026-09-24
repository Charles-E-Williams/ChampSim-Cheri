#include "cache_stats.h"

cache_stats operator-(cache_stats lhs, cache_stats rhs)
{
  cache_stats result;
  result.pf_requested = lhs.pf_requested - rhs.pf_requested;
  result.pf_issued = lhs.pf_issued - rhs.pf_issued;
  result.pf_useful = lhs.pf_useful - rhs.pf_useful;
  result.pf_useless = lhs.pf_useless - rhs.pf_useless;
  result.pf_fill = lhs.pf_fill - rhs.pf_fill;

  result.hits = lhs.hits - rhs.hits;
  result.misses = lhs.misses - rhs.misses;

  result.cap_auth_hits = lhs.cap_auth_hits - rhs.cap_auth_hits;
  result.cap_auth_misses = lhs.cap_auth_misses - rhs.cap_auth_misses;
  result.cap_data_hits = lhs.cap_data_hits - rhs.cap_data_hits;
  result.cap_data_misses = lhs.cap_data_misses - rhs.cap_data_misses;
  
  result.capabilities_per_cl_hit = lhs.capabilities_per_cl_hit - rhs.capabilities_per_cl_hit;
  result.capabilities_per_cl_miss = lhs.capabilities_per_cl_miss - rhs.capabilities_per_cl_miss;

  result.pf_issued_by_cap_size = lhs.pf_issued_by_cap_size - rhs.pf_issued_by_cap_size;
  result.pf_issued_skip_fill_by_cap_size = lhs.pf_issued_skip_fill_by_cap_size - rhs.pf_issued_skip_fill_by_cap_size;
  result.pf_redundant_by_cap_size = lhs.pf_redundant_by_cap_size - rhs.pf_redundant_by_cap_size;
  result.pf_fill_own_by_cap_size = lhs.pf_fill_own_by_cap_size - rhs.pf_fill_own_by_cap_size;
  result.pf_useful_timely_demand_by_cap_size = lhs.pf_useful_timely_demand_by_cap_size - rhs.pf_useful_timely_demand_by_cap_size;
  result.pf_useful_timely_upper_pf_by_cap_size = lhs.pf_useful_timely_upper_pf_by_cap_size - rhs.pf_useful_timely_upper_pf_by_cap_size;
  result.pf_useful_late_by_cap_size = lhs.pf_useful_late_by_cap_size - rhs.pf_useful_late_by_cap_size;
  result.pf_useless_by_cap_size = lhs.pf_useless_by_cap_size - rhs.pf_useless_by_cap_size;
  result.pf_useful_same_object_by_cap_size = lhs.pf_useful_same_object_by_cap_size - rhs.pf_useful_same_object_by_cap_size;
  result.pf_useful_demand_untagged_by_cap_size = lhs.pf_useful_demand_untagged_by_cap_size - rhs.pf_useful_demand_untagged_by_cap_size;
  
  result.total_miss_latency_cycles = lhs.total_miss_latency_cycles - rhs.total_miss_latency_cycles;

  return result;
}
