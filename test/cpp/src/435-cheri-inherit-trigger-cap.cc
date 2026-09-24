#include <algorithm>
#include <catch.hpp>
#include <optional>
#include <vector>

#include "cache.h"
#include "defaults.hpp"
#include "mocks.hpp"

namespace
{
// Trigger: VA 0x7fff12345e40 inside a capability based at 0x7fff12345000 (offset 0xe40).
// On the physical cache the same access has PA 0xdeadbe40, i.e. the same page offset.
constexpr uint64_t trigger_va = 0x7fff12345e40;
constexpr uint64_t trigger_pa = 0xdeadbe40;
constexpr uint64_t cap_base = 0x7fff12345000;
constexpr uint64_t cap_length = 0x4000;

std::vector<champsim::address> in_hook_targets;
std::optional<champsim::address> cycle_target;
std::vector<champsim::capability> prefetch_caps_seen_below;

// Issues no-cap prefetches to in_hook_targets from inside cache_operate, and one to cycle_target from cycle_operate.
struct legacy_issuer : champsim::modules::prefetcher {
  using prefetcher::prefetcher;
  std::optional<champsim::address> pending_cycle;

  uint32_t prefetcher_cache_operate(champsim::address, champsim::address, uint32_t, champsim::capability, bool, bool, access_type type, uint32_t metadata_in,
                                    uint32_t)
  {
    if (type == access_type::LOAD) {
      for (auto target : in_hook_targets)
        prefetch_line(target, true, 0);
      pending_cycle = cycle_target;
    }
    return metadata_in;
  }

  void prefetcher_cycle_operate()
  {
    if (pending_cycle.has_value() && prefetch_line(*pending_cycle, true, 0))
      pending_cycle.reset();
  }
};

// Records the capability carried by each prefetch request that reaches the lower cache.
struct prefetch_cap_recorder : champsim::modules::prefetcher {
  using prefetcher::prefetcher;

  uint32_t prefetcher_cache_operate(champsim::address, champsim::address, uint32_t, champsim::capability cap, bool, bool, access_type type,
                                    uint32_t metadata_in, uint32_t)
  {
    if (type == access_type::PREFETCH)
      prefetch_caps_seen_below.push_back(cap);
    return metadata_in;
  }
};

struct result {
  std::vector<uint64_t> tagged_offsets{};
  std::size_t untagged = 0;
};

// virtual_upper: the upper cache prefetches in the virtual address space and translates through a mock.
// (The test defaults do not set virtual_prefetch on default_l1d, so it is set explicitly.)
result run_scenario(bool inherit, bool virtual_upper, std::vector<champsim::address> targets, champsim::address cycle)
{
  in_hook_targets = std::move(targets);
  cycle_target = cycle;
  prefetch_caps_seen_below.clear();

  do_nothing_MRC mock_ll;
  do_nothing_MRC mock_lt;
  to_rq_MRP mock_ul{[](auto x, auto y) { return x.v_address == y.v_address; }};
  champsim::channel upper_to_lower{};
  auto upper_builder = champsim::cache_builder{virtual_upper ? champsim::defaults::default_l1d : champsim::defaults::default_l2c}
                           .name("435-upper")
                           .upper_levels({&mock_ul.queues})
                           .lower_level(&upper_to_lower)
                           .prefetcher<::legacy_issuer>();
  if (virtual_upper)
    upper_builder.lower_translate(&mock_lt.queues).set_virtual_prefetch();
  else
    upper_builder.reset_virtual_prefetch();
  if (inherit)
    upper_builder.set_inherit_trigger_cap();
  else
    upper_builder.reset_inherit_trigger_cap();
  CACHE upper{upper_builder};
  REQUIRE(upper.virtual_prefetch == virtual_upper);
  CACHE lower{champsim::cache_builder{champsim::defaults::default_llc}
                  .name("435-lower")
                  .upper_levels({&upper_to_lower})
                  .lower_level(&mock_ll.queues)
                  .prefetcher<::prefetch_cap_recorder>()};

  std::array<champsim::operable*, 5> elements{{&mock_ll, &mock_lt, &mock_ul, &upper, &lower}};
  for (auto elem : elements) {
    elem->initialize();
    elem->warmup = false;
    elem->begin_phase();
  }

  decltype(mock_ul)::request_type load;
  load.address = champsim::address{virtual_upper ? trigger_va : trigger_pa};
  load.v_address = champsim::address{trigger_va};
  load.is_translated = !virtual_upper;
  load.cpu = 0;
  load.cap = champsim::capability{champsim::address{trigger_va - cap_base}, champsim::address{cap_base}, champsim::address{cap_length}, 0x3, true};
  REQUIRE(mock_ul.issue(load));

  for (auto i = 0; i < 300; ++i)
    for (auto elem : elements)
      elem->_operate();

  REQUIRE(std::size(prefetch_caps_seen_below) == std::size(in_hook_targets) + 1);

  result retval{};
  for (const auto& cap : prefetch_caps_seen_below) {
    if (cap.tag) {
      CHECK(cap.base == champsim::address{cap_base});
      CHECK(cap.length == champsim::address{cap_length});
      CHECK(cap.permissions == 0x3);
      retval.tagged_offsets.push_back(cap.offset.to<uint64_t>());
    } else {
      ++retval.untagged;
    }
  }
  std::sort(std::begin(retval.tagged_offsets), std::end(retval.tagged_offsets));
  return retval;
}
} // namespace

TEST_CASE("On a physical cache, an inherited capability is re-pointed at the prefetched line when it is on the trigger's page")
{
  const champsim::address same_page{trigger_pa + 0x40};                // VA 0x7fff12345e80 -> offset 0xe80
  const champsim::address other_page{((trigger_pa >> 12) + 1) << 12}; // VA unknown -> trigger's offset, unchanged
  auto res = run_scenario(true, false, {same_page, other_page}, champsim::address{trigger_pa + 0x80});

  CHECK(res.tagged_offsets == std::vector<uint64_t>{0xe40, 0xe80});
  CHECK(res.untagged == 1); // the cycle_operate prefetch has no trigger
}

TEST_CASE("On a virtual-prefetch cache, an inherited capability is re-pointed at the prefetch VA, even across pages")
{
  const champsim::address next_line{trigger_va + 0x40};   // offset 0xe80
  const champsim::address next_page{cap_base + 0x1040};   // offset 0x1040
  auto res = run_scenario(true, true, {next_line, next_page}, champsim::address{trigger_va + 0x80});

  CHECK(res.tagged_offsets == std::vector<uint64_t>{0xe80, 0x1040});
  CHECK(res.untagged == 1);
}

TEST_CASE("Without inherit_trigger_cap, a no-cap prefetch issued inside cache_operate stays untagged")
{
  auto res = run_scenario(false, false, {champsim::address{trigger_pa + 0x40}}, champsim::address{trigger_pa + 0x80});

  CHECK(res.tagged_offsets.empty());
  CHECK(res.untagged == 2);
}
