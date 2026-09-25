#include <catch.hpp>
#include <optional>

#include "cache.h"
#include "defaults.hpp"
#include "mocks.hpp"

namespace
{
struct cap_stat_totals {
  long data_hits = 0;
  long data_misses = 0;
  long per_line_hits = 0;
  long per_line_misses = 0;
};

cap_stat_totals totals(const CACHE& c)
{
  return {c.sim_stats.cap_data_hits.total(), c.sim_stats.cap_data_misses.total(), c.sim_stats.capabilities_per_cl_hit.total(),
          c.sim_stats.capabilities_per_cl_miss.total()};
}

long count(const champsim::stats::event_counter<std::pair<access_type, std::size_t>>& counter, access_type type)
{
  return counter.value_or(std::pair{type, std::size_t{0}}, 0L);
}

const champsim::capability test_cap{champsim::address{0}, champsim::address{0x7000}, champsim::address{0x1000}, 0x3, true};

// Case A: a prefetch issued outside cache_operate at a cache without virtual_prefetch has no known VA. It fills a block,
// a store with a real VA dirties that block, and a load with a real VA evicts it, so its writeback carries the empty VA.
// single_block_lower: the lower cache holds one block, so the load evicts the prefetched line there first and the
// writeback misses (handle_write); otherwise the lower cache keeps the line and the writeback hits (try_hit).
void run_case_a(bool single_block_lower)
{
  do_nothing_MRC mock_ll;
  to_rq_MRP mock_rq;
  to_wq_MRP mock_wq;
  champsim::channel upper_to_lower{};
  CACHE upper{champsim::cache_builder{champsim::defaults::default_l1d} // matches offset bits, so a store is looked up like a read
                  .name("440-upper")
                  .sets(1)
                  .ways(1)
                  .upper_levels({{&mock_rq.queues, &mock_wq.queues}})
                  .lower_level(&upper_to_lower)
                  .reset_virtual_prefetch()};
  auto lower_builder = champsim::cache_builder{champsim::defaults::default_l2c}.name("440-lower").upper_levels({&upper_to_lower}).lower_level(&mock_ll.queues);
  if (single_block_lower)
    lower_builder.sets(1).ways(1);
  CACHE lower{lower_builder};

  std::array<champsim::operable*, 5> elements{{&mock_ll, &mock_rq, &mock_wq, &upper, &lower}};
  for (auto elem : elements) {
    elem->initialize();
    elem->warmup = false;
    elem->begin_phase();
  }
  auto run = [&] {
    for (int i = 0; i < 200; ++i)
      for (auto elem : elements)
        elem->_operate();
  };

  REQUIRE_FALSE(upper.virtual_prefetch);

  // 1. The prefetch misses at both levels with an empty v_address
  REQUIRE(upper.prefetch_line(champsim::address{0x1000}, true, 0, test_cap));
  run();
  REQUIRE(count(upper.sim_stats.misses, access_type::PREFETCH) == 1);
  REQUIRE(count(lower.sim_stats.misses, access_type::PREFETCH) == 1);
  const auto upper_after_prefetch = totals(upper);
  const auto lower_after_prefetch = totals(lower);
  CHECK(upper_after_prefetch.data_misses == 0);
  CHECK(upper_after_prefetch.per_line_misses == 0);
  CHECK(lower_after_prefetch.data_misses == 0);
  CHECK(lower_after_prefetch.per_line_misses == 0);

  // 2. A store with a real VA hits the prefetched block and dirties it (the block keeps its empty v_address)
  to_wq_MRP::request_type store;
  store.address = champsim::address{0x1000};
  store.v_address = champsim::address{0x7010};
  store.cpu = 0;
  store.type = access_type::WRITE;
  store.cap = test_cap;
  REQUIRE(mock_wq.issue(store));
  run();

  // 3. A load with a real VA evicts the dirty block; its writeback carries the empty v_address to the lower level
  to_rq_MRP::request_type load;
  load.address = champsim::address{0x2000};
  load.v_address = champsim::address{0x7040};
  load.cpu = 0;
  load.type = access_type::LOAD;
  load.cap = test_cap;
  REQUIRE(mock_rq.issue(load));
  run();

  if (single_block_lower)
    REQUIRE(count(lower.sim_stats.misses, access_type::WRITE) == 1); // the writeback went through handle_write
  else
    REQUIRE(count(lower.sim_stats.hits, access_type::WRITE) == 1); // the writeback went through try_hit

  const auto upper_end = totals(upper);
  const auto lower_end = totals(lower);

  // The demands with a real VA are recorded at the upper level: the store's hit and the load's miss
  CHECK(upper_end.data_hits == 1);
  CHECK(upper_end.per_line_hits == 1);
  CHECK(upper_end.data_misses == 1);
  CHECK(upper_end.per_line_misses == 1);

  // At the lower level only the load is recorded, neither the prefetch nor the writeback
  CHECK(lower_end.data_hits == 0);
  CHECK(lower_end.per_line_hits == 0);
  CHECK(lower_end.data_misses == 1);
  CHECK(lower_end.per_line_misses == 1);
}

// Case B: issues a no-cap prefetch to the next line from inside cache_operate
struct next_line_in_hook : champsim::modules::prefetcher {
  using prefetcher::prefetcher;
  uint32_t prefetcher_cache_operate(champsim::address addr, champsim::address, uint32_t, champsim::capability, bool, bool, access_type type,
                                    uint32_t metadata_in, uint32_t)
  {
    if (type == access_type::LOAD)
      prefetch_line(champsim::address{champsim::block_number{addr} + 1}, true, 0);
    return metadata_in;
  }
};
} // namespace

TEST_CASE("440-A1: with an unknown VA, a prefetch and the writeback of its block (a hit below) record no cap_mem-based statistics")
{
  run_case_a(false);
}

TEST_CASE("440-A2: with an unknown VA, a prefetch and the writeback of its block (a miss below) record no cap_mem-based statistics")
{
  run_case_a(true);
}

TEST_CASE("440-B: a prefetch on the trigger's page carries the recovered VA, and its statistics read the right cap_mem slot")
{
  // Trigger: PA 0xdeadbe40, VA 0x7fff12345e40 (same page offset). The prefetch goes to PA 0xdeadbe80 -> VA 0x7fff12345e80.
  constexpr uint64_t trigger_pa = 0xdeadbe40;
  constexpr uint64_t trigger_va = 0x7fff12345e40;
  const champsim::address pf_pa{trigger_pa + 0x40};
  const champsim::address pf_va{trigger_va + 0x40};

  // A 64-byte (0-128B class) capability stored in memory at the prefetched line's VA
  champsim::cap_mem[0].store_capability(pf_va, champsim::capability{champsim::address{0}, pf_va, champsim::address{64}, 0x3, true});

  do_nothing_MRC mock_ll;
  to_rq_MRP mock_rq{[](auto x, auto y) { return x.v_address == y.v_address; }};
  champsim::channel upper_to_lower{};
  CACHE upper{champsim::cache_builder{champsim::defaults::default_l2c}
                  .name("440-B-upper")
                  .upper_levels({&mock_rq.queues})
                  .lower_level(&upper_to_lower)
                  .reset_virtual_prefetch()
                  .prefetcher<next_line_in_hook>()};
  CACHE lower{champsim::cache_builder{champsim::defaults::default_llc}.name("440-B-lower").upper_levels({&upper_to_lower}).lower_level(&mock_ll.queues)};

  std::array<champsim::operable*, 4> elements{{&mock_ll, &mock_rq, &upper, &lower}};
  for (auto elem : elements) {
    elem->initialize();
    elem->warmup = false;
    elem->begin_phase();
  }
  REQUIRE_FALSE(upper.virtual_prefetch);

  to_rq_MRP::request_type load;
  load.address = champsim::address{trigger_pa};
  load.v_address = champsim::address{trigger_va};
  load.cpu = 0;
  load.type = access_type::LOAD;
  load.cap = test_cap;
  REQUIRE(mock_rq.issue(load));
  for (int i = 0; i < 300; ++i)
    for (auto elem : elements)
      elem->_operate();

  const cap_dist_key pf_key{cap_size_coverage_events::B_0_128B, access_type::PREFETCH, 0};
  CHECK(upper.sim_stats.cap_data_misses.value_or(pf_key, 0L) == 1); // the prefetch's own miss, looked up at its VA
  CHECK(lower.sim_stats.cap_data_misses.value_or(pf_key, 0L) == 1); // the forwarded prefetch carries the VA down

  std::optional<champsim::address> filled_va;
  for (const auto& blk : upper.block)
    if (blk.valid && blk.address == pf_pa)
      filled_va = blk.v_address;
  REQUIRE(filled_va.has_value());
  CHECK(*filled_va == pf_va); // the block keeps a real VA for a later writeback
}
