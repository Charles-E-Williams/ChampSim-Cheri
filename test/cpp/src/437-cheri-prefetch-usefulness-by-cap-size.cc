#include <array>
#include <catch.hpp>
#include <type_traits>

#include "cache.h"
#include "defaults.hpp"
#include "mocks.hpp"

namespace
{
using cls = cap_size_coverage_events;

champsim::capability make_cap(uint64_t base, uint64_t length)
{
  return champsim::capability{champsim::address{0}, champsim::address{base}, champsim::address{length}, 0x3, true};
}

// Size classes used below
const auto cap_64B_a = make_cap(0x10000, 64);    // 0-128B
const auto cap_64B_b = make_cap(0x90000, 64);    // 0-128B, different object
const auto cap_4KB = make_cap(0x20000, 4096);    // 128B-4KB
const auto cap_1MB = make_cap(0x100000, 1 << 20); // 64KB-1MB

constexpr std::array<champsim::stats::event_counter<pf_cap_key> cache_stats::*, 10> all_by_size{
    &cache_stats::pf_issued_by_cap_size,          &cache_stats::pf_issued_skip_fill_by_cap_size,
    &cache_stats::pf_redundant_by_cap_size,       &cache_stats::pf_fill_own_by_cap_size,
    &cache_stats::pf_useful_timely_demand_by_cap_size, &cache_stats::pf_useful_timely_upper_pf_by_cap_size,
    &cache_stats::pf_useful_late_by_cap_size,
    &cache_stats::pf_useless_by_cap_size,         &cache_stats::pf_useful_same_object_by_cap_size,
    &cache_stats::pf_useful_demand_untagged_by_cap_size};

long count(const champsim::stats::event_counter<pf_cap_key>& counter, cls c) { return counter.value_or(pf_cap_key{c, 0}, 0L); }

long sum(const champsim::stats::event_counter<pf_cap_key>& counter)
{
  long total = 0;
  for (auto c : cap_size_coverage_events_with_untagged)
    total += count(counter, c);
  return total;
}

// Prefetcher used by the untagged-trigger test: issues a no-cap prefetch to the next line on every LOAD
struct next_line_legacy : champsim::modules::prefetcher {
  using prefetcher::prefetcher;
  uint32_t prefetcher_cache_operate(champsim::address addr, champsim::address, uint32_t, champsim::capability, bool, bool, access_type type,
                                    uint32_t metadata_in, uint32_t)
  {
    if (type == access_type::LOAD)
      prefetch_line(champsim::address{champsim::block_number{addr} + 1}, true, 0);
    return metadata_in;
  }
};

struct no_prefetcher_tag {
};

// A single cache between a demand source (RQ), an upper-level prefetch source (PQ), and a memory with a fixed latency
template <typename P = no_prefetcher_tag>
struct rig {
  do_nothing_MRC mock_ll;
  do_nothing_MRC mock_lt;
  to_rq_MRP ul_rq;
  to_pq_MRP ul_pq;
  CACHE uut;
  std::array<champsim::operable*, 5> elements;

  static auto builder(long sets, long ways, bool virtual_prefetch, do_nothing_MRC& ll, do_nothing_MRC& lt, to_rq_MRP& rq, to_pq_MRP& pq)
  {
    auto b = champsim::cache_builder{champsim::defaults::default_l2c}
                 .name("437-uut")
                 .sets(sets)
                 .ways(ways)
                 .hit_latency(1)
                 .fill_latency(1)
                 .upper_levels({{&rq.queues, &pq.queues}})
                 .lower_level(&ll.queues);
    if (virtual_prefetch)
      b.lower_translate(&lt.queues).set_virtual_prefetch();
    else
      b.reset_virtual_prefetch();
    if constexpr (std::is_same_v<P, no_prefetcher_tag>)
      return b;
    else
      return b.template prefetcher<P>();
  }

  rig(long sets, long ways, int memory_latency = 0, bool virtual_prefetch = false)
      : mock_ll(memory_latency), uut{builder(sets, ways, virtual_prefetch, mock_ll, mock_lt, ul_rq, ul_pq)},
        elements{{&mock_ll, &mock_lt, &ul_rq, &ul_pq, &uut}}
  {
    for (auto elem : elements) {
      elem->initialize();
      elem->warmup = false;
      elem->begin_phase();
    }
  }

  void run(int cycles)
  {
    for (int i = 0; i < cycles; ++i)
      for (auto elem : elements)
        elem->_operate();
  }

  void demand(uint64_t addr, champsim::capability cap)
  {
    to_rq_MRP::request_type pkt;
    pkt.address = champsim::address{addr};
    pkt.v_address = pkt.address;
    pkt.cpu = 0;
    pkt.type = access_type::LOAD;
    pkt.cap = cap;
    REQUIRE(ul_rq.issue(pkt));
  }

  void upper_prefetch(uint64_t addr, champsim::capability cap)
  {
    to_pq_MRP::request_type pkt;
    pkt.address = champsim::address{addr};
    pkt.v_address = pkt.address;
    pkt.cpu = 0;
    pkt.type = access_type::PREFETCH;
    pkt.cap = cap;
    REQUIRE(ul_pq.issue(pkt));
  }

  void own_prefetch(uint64_t addr, champsim::capability cap, bool fill_this_level = true)
  {
    REQUIRE(uut.prefetch_line(champsim::address{addr}, fill_this_level, 0, cap));
  }

  const cache_stats& stats() const { return uut.sim_stats; }
};
} // namespace

TEST_CASE("437-1: per-class counters sum to the existing prefetch counters")
{
  rig<> r{1, 2, 20};
  // timely: prefetch fills, then a demand hits it
  r.own_prefetch(0x1000, cap_64B_a);
  r.run(100);
  r.demand(0x1000, cap_1MB);
  r.run(100);
  // useless: two unused prefetches, the second evicts the first
  r.own_prefetch(0x2000, cap_4KB);
  r.run(100);
  r.own_prefetch(0x3000, cap_1MB);
  r.run(100);
  r.own_prefetch(0x4000, champsim::capability{});
  r.run(100);
  // late: a demand joins an in-flight prefetch
  r.own_prefetch(0x5000, cap_4KB);
  r.run(5);
  r.demand(0x5000, cap_4KB);
  r.run(100);
  // an upper-level prefetch hits this cache's prefetched block
  r.own_prefetch(0x8000, cap_1MB);
  r.run(100);
  r.upper_prefetch(0x8000, cap_1MB);
  r.run(100);
  // upper-level prefetch and a skip-fill prefetch
  r.upper_prefetch(0x6000, cap_4KB);
  r.run(100);
  r.own_prefetch(0x7000, cap_64B_a, false);
  r.run(100);

  const auto& s = r.stats();
  REQUIRE(s.pf_useful > 0);
  REQUIRE(s.pf_useless > 0);
  CHECK(sum(s.pf_issued_by_cap_size) == static_cast<long>(s.pf_issued));
  REQUIRE(sum(s.pf_useful_timely_upper_pf_by_cap_size) > 0);
  CHECK(sum(s.pf_useful_timely_demand_by_cap_size) + sum(s.pf_useful_timely_upper_pf_by_cap_size) + sum(s.pf_useful_late_by_cap_size)
        == static_cast<long>(s.pf_useful));
  CHECK(sum(s.pf_useless_by_cap_size) == static_cast<long>(s.pf_useless));
  CHECK(sum(s.pf_fill_own_by_cap_size) <= static_cast<long>(s.pf_fill));
}

TEST_CASE("437-2: a timely prefetch is credited to the prefetch's class, not the demand's")
{
  rig<> r{1, 4};
  r.own_prefetch(0x1000, cap_64B_a);
  r.run(100);
  REQUIRE(count(r.stats().pf_fill_own_by_cap_size, cls::B_0_128B) == 1);

  r.demand(0x1000, cap_1MB);
  r.run(100);
  CHECK(r.stats().pf_useful == 1);
  CHECK(count(r.stats().pf_useful_timely_demand_by_cap_size, cls::B_0_128B) == 1);
  CHECK(count(r.stats().pf_useful_timely_demand_by_cap_size, cls::B_64KB_1MB) == 0);
  CHECK(sum(r.stats().pf_useful_timely_upper_pf_by_cap_size) == 0);
  CHECK(sum(r.stats().pf_useful_late_by_cap_size) == 0);
}

TEST_CASE("437-3: a demand that joins an in-flight prefetch is late, in the prefetch's class, and never counted again")
{
  rig<> r{1, 1, 30};
  r.own_prefetch(0x1000, cap_64B_a);
  r.run(5); // the prefetch is in the MSHR, waiting on memory
  REQUIRE(r.uut.get_mshr_occupancy() == 1);
  r.demand(0x1000, cap_1MB);
  r.run(100);

  CHECK(r.stats().pf_useful == 1);
  CHECK(count(r.stats().pf_useful_late_by_cap_size, cls::B_0_128B) == 1);
  CHECK(count(r.stats().pf_useful_late_by_cap_size, cls::B_64KB_1MB) == 0);
  CHECK((sum(r.stats().pf_useful_timely_demand_by_cap_size) + sum(r.stats().pf_useful_timely_upper_pf_by_cap_size)) == 0);
  CHECK(sum(r.stats().pf_fill_own_by_cap_size) == 0); // the merged fill carries the demand's identity

  AND_WHEN("the line is hit again and then evicted")
  {
    r.demand(0x1000, cap_1MB);
    r.run(100);
    r.demand(0x2000, cap_1MB); // single-block cache: evicts 0x1000
    r.run(100);
    CHECK(r.stats().pf_useful == 1);
    CHECK((sum(r.stats().pf_useful_timely_demand_by_cap_size) + sum(r.stats().pf_useful_timely_upper_pf_by_cap_size)) == 0);
    CHECK(r.stats().pf_useless == 0);
    CHECK(sum(r.stats().pf_useless_by_cap_size) == 0);
  }
}

TEST_CASE("437-4: an unused prefetched block evicted by a later fill is useless in its issue class")
{
  rig<> r{1, 1};
  r.own_prefetch(0x1000, cap_4KB);
  r.run(100);
  r.demand(0x2000, cap_64B_a); // evicts the prefetched block
  r.run(100);

  CHECK(r.stats().pf_useless == 1);
  CHECK(count(r.stats().pf_useless_by_cap_size, cls::B_128B_4KB) == 1);
  CHECK(count(r.stats().pf_useless_by_cap_size, cls::B_0_128B) == 0);
}

TEST_CASE("437-5: a prefetch from the upper level fills without setting the prefetch bit and is never credited here")
{
  rig<> r{1, 1};
  r.upper_prefetch(0x1000, cap_4KB);
  r.run(100);
  CHECK(r.stats().pf_fill == 1);
  CHECK(sum(r.stats().pf_fill_own_by_cap_size) == 0);
  CHECK(sum(r.stats().pf_issued_by_cap_size) == 0);

  r.demand(0x1000, cap_4KB); // hit
  r.run(100);
  r.demand(0x2000, cap_4KB); // evicts it
  r.run(100);
  CHECK(r.stats().pf_useful == 0);
  CHECK(r.stats().pf_useless == 0);
  CHECK((sum(r.stats().pf_useful_timely_demand_by_cap_size) + sum(r.stats().pf_useful_timely_upper_pf_by_cap_size)) == 0);
  CHECK(sum(r.stats().pf_useless_by_cap_size) == 0);
}

TEST_CASE("437-6: same-object and untagged-demand counters on useful prefetches")
{
  rig<> r{1, 4};
  r.own_prefetch(0x1000, cap_64B_a);
  r.own_prefetch(0x2000, cap_64B_a);
  r.own_prefetch(0x3000, cap_64B_a);
  r.run(100);
  REQUIRE(count(r.stats().pf_fill_own_by_cap_size, cls::B_0_128B) == 3);

  r.demand(0x1000, make_cap(cap_64B_a.base.to<uint64_t>(), 256)); // same base, different length
  r.demand(0x2000, cap_64B_b);                                     // different base
  r.demand(0x3000, champsim::capability{});                        // untagged
  r.run(100);

  CHECK(count(r.stats().pf_useful_timely_demand_by_cap_size, cls::B_0_128B) == 3);
  CHECK(count(r.stats().pf_useful_same_object_by_cap_size, cls::B_0_128B) == 1);
  CHECK(count(r.stats().pf_useful_demand_untagged_by_cap_size, cls::B_0_128B) == 1);
}

TEST_CASE("437-7: a prefetch to a resident line is redundant and does not fill")
{
  rig<> r{1, 4};
  r.demand(0x1000, cap_4KB);
  r.run(100);
  const auto fills_before = r.stats().pf_fill;

  r.own_prefetch(0x1000, cap_64B_a);
  r.run(100);
  CHECK(count(r.stats().pf_redundant_by_cap_size, cls::B_0_128B) == 1);
  CHECK(r.stats().pf_fill == fills_before);
  CHECK(sum(r.stats().pf_fill_own_by_cap_size) == 0);
}

TEST_CASE("437-8: a prefetch issued with fill_this_level == false is counted as skip-fill")
{
  rig<> r{1, 4};
  r.own_prefetch(0x1000, cap_4KB, false);
  r.own_prefetch(0x2000, cap_4KB, true);
  CHECK(count(r.stats().pf_issued_by_cap_size, cls::B_128B_4KB) == 2);
  CHECK(count(r.stats().pf_issued_skip_fill_by_cap_size, cls::B_128B_4KB) == 1);
  r.run(100);
  CHECK(count(r.stats().pf_fill_own_by_cap_size, cls::B_128B_4KB) == 1); // only the filling one
}

TEST_CASE("437-10: begin_phase clears the counters, end_phase copies them to roi_stats, and operator- subtracts them")
{
  rig<> r{1, 4};
  const pf_cap_key key{cls::B_4KB_64KB, 0};
  for (auto member : all_by_size)
    (r.uut.sim_stats.*member).increment(key);

  r.uut.begin_phase();
  for (auto member : all_by_size)
    CHECK(count(r.uut.sim_stats.*member, cls::B_4KB_64KB) == 0);

  for (auto member : all_by_size)
    (r.uut.sim_stats.*member).increment(key);
  r.uut.end_phase(0);
  for (auto member : all_by_size)
    CHECK(count(r.uut.roi_stats.*member, cls::B_4KB_64KB) == 1);

  const auto before = r.uut.sim_stats;
  for (auto member : all_by_size) {
    (r.uut.sim_stats.*member).increment(key);
    (r.uut.sim_stats.*member).increment(key);
  }
  const auto diff = r.uut.sim_stats - before;
  for (auto member : all_by_size)
    CHECK(count(diff.*member, cls::B_4KB_64KB) == 2);
}

TEST_CASE("437-11: no-cap prefetches triggered by untagged accesses land in UNTAGGED")
{
  rig<next_line_legacy> r{1, 4};
  r.demand(0x1000, champsim::capability{}); // miss; the prefetcher issues 0x1040 without a capability and inherits the untagged trigger
  r.run(100);
  r.demand(0x1040, champsim::capability{}); // hits the prefetched line
  r.run(100);

  REQUIRE(r.stats().pf_issued > 0);
  REQUIRE(r.stats().pf_useful == 1);
  CHECK(count(r.stats().pf_issued_by_cap_size, cls::UNTAGGED) == static_cast<long>(r.stats().pf_issued));
  CHECK(count(r.stats().pf_useful_timely_demand_by_cap_size, cls::UNTAGGED) == 1);
  CHECK(sum(r.stats().pf_useful_same_object_by_cap_size) == 0); // issuing capability unknown
  CHECK(count(r.stats().pf_useful_demand_untagged_by_cap_size, cls::UNTAGGED) == 1);
  for (auto member : all_by_size)
    for (auto c : cap_size_coverage_events_all)
      CHECK(count(r.stats().*member, c) == 0);
}

TEST_CASE("437-12: a PREFETCH from the upper level that hits a prefetched block is counted apart from demand hits")
{
  rig<> r{1, 4};
  r.own_prefetch(0x1000, cap_64B_a);
  r.run(100);
  REQUIRE(count(r.stats().pf_fill_own_by_cap_size, cls::B_0_128B) == 1);

  r.upper_prefetch(0x1000, cap_64B_a); // same object, but a prefetch, not a demand
  r.run(100);
  CHECK(r.stats().pf_useful == 1);
  CHECK(count(r.stats().pf_useful_timely_upper_pf_by_cap_size, cls::B_0_128B) == 1);
  CHECK(sum(r.stats().pf_useful_timely_demand_by_cap_size) == 0);
  CHECK(sum(r.stats().pf_useful_same_object_by_cap_size) == 0); // same-object counts demand uses only
  CHECK(sum(r.stats().pf_useful_demand_untagged_by_cap_size) == 0);

  // The prefetch bit is cleared by that hit, so a later demand hit is not counted again
  r.demand(0x1000, cap_64B_a);
  r.run(100);
  CHECK(r.stats().pf_useful == 1);
  CHECK(sum(r.stats().pf_useful_timely_demand_by_cap_size) == 0);
}
