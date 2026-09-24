/*
 *    Copyright 2023 The ChampSim Contributors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <algorithm>
#include <utility>
#include <nlohmann/json.hpp>

#include "stats_printer.h"

void to_json(nlohmann::json& j, const O3_CPU::stats_type& stats)
{
  constexpr std::array types{branch_type::BRANCH_DIRECT_JUMP, branch_type::BRANCH_INDIRECT,      branch_type::BRANCH_CONDITIONAL,
                             branch_type::BRANCH_DIRECT_CALL, branch_type::BRANCH_INDIRECT_CALL, branch_type::BRANCH_RETURN};

  auto total_mispredictions = std::ceil(
      std::accumulate(std::begin(types), std::end(types), 0LL, [btm = stats.branch_type_misses](auto acc, auto next) { return acc + btm.value_or(next, 0); }));

  std::map<std::string, std::size_t> mpki{};
  for (auto type : types) {
    mpki.emplace(branch_type_names.at(champsim::to_underlying(type)), stats.branch_type_misses.value_or(type, 0));
  }

  j = nlohmann::json{{"instructions", stats.instrs()},
                     {"cycles", stats.cycles()},
                     {"Avg ROB occupancy at mispredict", std::ceil(stats.total_rob_occupancy_at_branch_mispredict) / std::ceil(total_mispredictions)},
                     {"mispredict", mpki},
                     {"untagged authority loads", stats.untagged_auth_loads},
                     {"untagged authority stores", stats.untagged_auth_stores},
                     {"authority capability does not cover access", stats.auth_cap_va_out_of_bounds}};
}

void to_json(nlohmann::json& j, const CACHE::stats_type& stats)
{
  using hits_value_type = typename decltype(stats.hits)::value_type;
  using misses_value_type = typename decltype(stats.misses)::value_type;
  using miss_merge_value_type = typename decltype(stats.miss_merge)::value_type;
  using fill_value_type = typename decltype(stats.fill)::value_type;

  std::map<std::string, nlohmann::json> statsmap;
  statsmap.emplace("prefetch requested", stats.pf_requested);
  statsmap.emplace("prefetch issued", stats.pf_issued);
  statsmap.emplace("useful prefetch", stats.pf_useful);
  statsmap.emplace("useless prefetch", stats.pf_useless);
  statsmap.emplace("prefetch cap offset unadjusted", stats.pf_cap_offset_unadjusted);

  // Prefetch outcomes by issuing capability size class: {counter: {class: [per-cpu raw counts]}}
  {
    const std::array<std::pair<const char*, const champsim::stats::event_counter<pf_cap_key>*>, 10> by_size{{
        {"issued", &stats.pf_issued_by_cap_size},
        {"issued skip fill", &stats.pf_issued_skip_fill_by_cap_size},
        {"redundant", &stats.pf_redundant_by_cap_size},
        {"fill own", &stats.pf_fill_own_by_cap_size},
        {"useful timely demand", &stats.pf_useful_timely_demand_by_cap_size},
        {"useful timely upper-level prefetch", &stats.pf_useful_timely_upper_pf_by_cap_size},
        {"useful late", &stats.pf_useful_late_by_cap_size},
        {"useless", &stats.pf_useless_by_cap_size},
        {"useful same object", &stats.pf_useful_same_object_by_cap_size},
        {"useful demand untagged", &stats.pf_useful_demand_untagged_by_cap_size},
    }};
    std::map<std::string, nlohmann::json> by_size_json;
    for (const auto& [counter_name, counter] : by_size) {
      std::map<std::string, std::vector<long>> per_class;
      for (auto cls : cap_size_coverage_events_with_untagged) {
        std::vector<long> per_cpu;
        for (std::size_t cpu = 0; cpu < NUM_CPUS; ++cpu)
          per_cpu.push_back(counter->value_or(pf_cap_key{cls, cpu}, 0L));
        per_class.emplace(cap_size_coverage_events_names.at(static_cast<std::size_t>(cls)), per_cpu);
      }
      by_size_json.emplace(counter_name, per_class);
    }
    statsmap.emplace("prefetch by capability size", by_size_json);
  }

  uint64_t total_downstream_demands = stats.fill.total();
  for (std::size_t cpu = 0; cpu < NUM_CPUS; ++cpu)
    total_downstream_demands -= stats.fill.value_or(std::pair{access_type::PREFETCH, cpu}, fill_value_type{});

  statsmap.emplace("miss latency", std::ceil(stats.total_miss_latency_cycles) / std::ceil(total_downstream_demands));
  for (const auto type : {access_type::LOAD, access_type::RFO, access_type::PREFETCH, access_type::WRITE, access_type::TRANSLATION}) {
    std::vector<hits_value_type> hits;
    std::vector<misses_value_type> misses;
    std::vector<miss_merge_value_type> miss_merges;

    for (std::size_t cpu = 0; cpu < NUM_CPUS; ++cpu) {
      hits.push_back(stats.hits.value_or(std::pair{type, cpu}, hits_value_type{}));
      misses.push_back(stats.misses.value_or(std::pair{type, cpu}, misses_value_type{}));
      miss_merges.push_back(stats.miss_merge.value_or(std::pair{type, cpu}, miss_merge_value_type{}));
    }

    statsmap.emplace(access_type_names.at(champsim::to_underlying(type)), nlohmann::json{{"hit", hits}, {"miss", misses}, {"miss_merge", miss_merges}});
  }

  j = statsmap;
}

void to_json(nlohmann::json& j, const DRAM_CHANNEL::stats_type stats)
{
  j = nlohmann::json{{"RQ ROW_BUFFER_HIT", stats.RQ_ROW_BUFFER_HIT},
                     {"RQ ROW_BUFFER_MISS", stats.RQ_ROW_BUFFER_MISS},
                     {"WQ ROW_BUFFER_HIT", stats.WQ_ROW_BUFFER_HIT},
                     {"WQ ROW_BUFFER_MISS", stats.WQ_ROW_BUFFER_MISS},
                     {"AVG DBUS CONGESTED CYCLE", (std::ceil(stats.dbus_cycle_congested) / std::ceil(stats.dbus_count_congested))},
                     {"REFRESHES ISSUED", stats.refresh_cycles}};
}

namespace champsim
{
void to_json(nlohmann::json& j, const champsim::phase_stats stats)
{
  std::map<std::string, nlohmann::json> roi_stats;
  roi_stats.emplace("cores", stats.roi_cpu_stats);
  roi_stats.emplace("DRAM", stats.roi_dram_stats);
  for (auto x : stats.roi_cache_stats) {
    roi_stats.emplace(x.name, x);
  }

  std::map<std::string, nlohmann::json> sim_stats;
  sim_stats.emplace("cores", stats.sim_cpu_stats);
  sim_stats.emplace("DRAM", stats.sim_dram_stats);
  for (auto x : stats.sim_cache_stats) {
    sim_stats.emplace(x.name, x);
  }

  std::map<std::string, nlohmann::json> statsmap{{"name", stats.name}, {"traces", stats.trace_names}};
  statsmap.emplace("roi", roi_stats);
  statsmap.emplace("sim", sim_stats);
  j = statsmap;
}
} // namespace champsim

void champsim::json_printer::print(std::vector<phase_stats>& stats) { stream << nlohmann::json::array_t{std::begin(stats), std::end(stats)}; }
