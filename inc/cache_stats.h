#ifndef CACHE_STATS_H
#define CACHE_STATS_H

#include <cstdint>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>

#include "channel.h"
#include "event_counter.h"

enum class cap_size_coverage_events : uint8_t {
  B_0_128B = 0, // 0–128B
  B_128B_4KB,   // 128B–4KB
  B_4KB_64KB,   // 4KB–64KB
  B_64KB_1MB,   // 64KB–1MB
  B_1MB_1GB,    // 1MB–1GB
  B_GT_1GB,     // > 1GB
  UNTAGGED,      
  NUM_coverage_events
};

inline constexpr std::size_t NUM_CAP_SIZE_coverage_events = static_cast<std::size_t>(cap_size_coverage_events::UNTAGGED);

inline constexpr std::array<const char*, static_cast<std::size_t>(cap_size_coverage_events::NUM_coverage_events)> cap_size_coverage_events_names{{
    "0-128B", "128B-4KB", "4KB-64KB", "64KB-1MB", "1MB-1GB", ">1GB", "UNTAGGED"
}};

inline constexpr std::array<cap_size_coverage_events, 6> cap_size_coverage_events_all{{
    cap_size_coverage_events::B_0_128B, cap_size_coverage_events::B_128B_4KB, cap_size_coverage_events::B_4KB_64KB,
    cap_size_coverage_events::B_64KB_1MB, cap_size_coverage_events::B_1MB_1GB, cap_size_coverage_events::B_GT_1GB
}};

inline constexpr std::array<cap_size_coverage_events, 7> cap_size_coverage_events_with_untagged{{
    cap_size_coverage_events::B_0_128B, cap_size_coverage_events::B_128B_4KB, cap_size_coverage_events::B_4KB_64KB,
    cap_size_coverage_events::B_64KB_1MB, cap_size_coverage_events::B_1MB_1GB, cap_size_coverage_events::B_GT_1GB,
    cap_size_coverage_events::UNTAGGED
}};

inline cap_size_coverage_events classify_cap_size(uint64_t length)
{
  if (length <= 128)
    return cap_size_coverage_events::B_0_128B;
  if (length <= 4096)
    return cap_size_coverage_events::B_128B_4KB;
  if (length <= 65536)
    return cap_size_coverage_events::B_4KB_64KB;
  if (length <= 1048576)
    return cap_size_coverage_events::B_64KB_1MB;
  if (length <= 1073741824ULL)
    return cap_size_coverage_events::B_1MB_1GB;
  return cap_size_coverage_events::B_GT_1GB;
}

inline cap_size_coverage_events classify_capability(const champsim::capability& cap)
{
  if (!cap.tag)
    return cap_size_coverage_events::UNTAGGED;
  uint64_t len = cap.length.to<uint64_t>();
  if (len == 0)
    return cap_size_coverage_events::UNTAGGED;
  return classify_cap_size(len);
}

using cap_dist_key = std::tuple<cap_size_coverage_events, access_type, std::remove_cv_t<decltype(NUM_CPUS)>>;
using cl_cap_key = std::tuple<unsigned, access_type, std::remove_cv_t<decltype(NUM_CPUS)>>;

struct cache_stats {
  std::string name;
  // prefetch stats
  uint64_t pf_requested = 0;
  uint64_t pf_issued = 0;
  uint64_t pf_useful = 0;
  uint64_t pf_useless = 0;
  uint64_t pf_fill = 0;

  champsim::stats::event_counter<std::pair<access_type, std::remove_cv_t<decltype(NUM_CPUS)>>> hits = {};
  champsim::stats::event_counter<std::pair<access_type, std::remove_cv_t<decltype(NUM_CPUS)>>> misses = {};
  champsim::stats::event_counter<std::pair<access_type, std::remove_cv_t<decltype(NUM_CPUS)>>> miss_merge = {};
  champsim::stats::event_counter<std::pair<access_type, std::remove_cv_t<decltype(NUM_CPUS)>>> fill = {};

  long total_miss_latency_cycles{};

  champsim::stats::event_counter<cap_dist_key> cap_auth_hits = {};
  champsim::stats::event_counter<cap_dist_key> cap_auth_misses = {};
  champsim::stats::event_counter<cap_dist_key> cap_data_hits = {};
  champsim::stats::event_counter<cap_dist_key> cap_data_misses = {};

  champsim::stats::event_counter<cl_cap_key> capabilities_per_cl_hit = {};
  champsim::stats::event_counter<cl_cap_key> capabilities_per_cl_miss = {};
};

cache_stats operator-(cache_stats lhs, cache_stats rhs);

#endif