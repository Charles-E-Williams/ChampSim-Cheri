#include <catch.hpp>
#include <map>
#include <optional>

#include "cache.h"
#include "defaults.hpp"
#include "mocks.hpp"

namespace
{
constexpr uint64_t trigger_addr = 0xdeadbe40;
const champsim::address in_hook_target{champsim::block_number{champsim::address{trigger_addr}} + 1};
const champsim::address cycle_target{champsim::block_number{champsim::address{trigger_addr}} + 2};

std::map<champsim::address, champsim::capability> prefetch_caps_seen_below;
std::optional<champsim::address> pending_cycle_prefetch;

// Issues one no-cap prefetch from inside cache_operate and one from cycle_operate.
struct legacy_issuer : champsim::modules::prefetcher {
  using prefetcher::prefetcher;

  uint32_t prefetcher_cache_operate(champsim::address, champsim::address, uint32_t, champsim::capability, bool, bool, access_type type, uint32_t metadata_in,
                                    uint32_t)
  {
    if (type == access_type::LOAD) {
      prefetch_line(in_hook_target, true, 0);
      pending_cycle_prefetch = cycle_target;
    }
    return metadata_in;
  }

  void prefetcher_cycle_operate()
  {
    if (pending_cycle_prefetch.has_value() && prefetch_line(*pending_cycle_prefetch, true, 0))
      pending_cycle_prefetch.reset();
  }
};

// Records the capability carried by each prefetch request that reaches the lower cache.
struct prefetch_cap_recorder : champsim::modules::prefetcher {
  using prefetcher::prefetcher;

  uint32_t prefetcher_cache_operate(champsim::address addr, champsim::address, uint32_t, champsim::capability cap, bool, bool, access_type type,
                                    uint32_t metadata_in, uint32_t)
  {
    if (type == access_type::PREFETCH)
      prefetch_caps_seen_below.insert_or_assign(addr, cap);
    return metadata_in;
  }
};

void run_scenario(bool inherit)
{
  prefetch_caps_seen_below.clear();
  pending_cycle_prefetch.reset();

  do_nothing_MRC mock_ll;
  to_rq_MRP mock_ul;
  champsim::channel upper_to_lower{};
  auto upper_builder = champsim::cache_builder{champsim::defaults::default_l2c}
                           .name(inherit ? "435-upper-inherit" : "435-upper-no-inherit")
                           .upper_levels({&mock_ul.queues})
                           .lower_level(&upper_to_lower)
                           .prefetcher<::legacy_issuer>();
  if (inherit)
    upper_builder.set_inherit_trigger_cap();
  else
    upper_builder.reset_inherit_trigger_cap();
  CACHE upper{upper_builder};
  CACHE lower{champsim::cache_builder{champsim::defaults::default_llc}
                  .name("435-lower")
                  .upper_levels({&upper_to_lower})
                  .lower_level(&mock_ll.queues)
                  .prefetcher<::prefetch_cap_recorder>()};

  std::array<champsim::operable*, 4> elements{{&mock_ll, &mock_ul, &upper, &lower}};
  for (auto elem : elements) {
    elem->initialize();
    elem->warmup = false;
    elem->begin_phase();
  }

  const champsim::capability trigger_cap{champsim::address{0}, champsim::address{0xdeadbe00}, champsim::address{4096}, 0x3, true};

  decltype(mock_ul)::request_type load;
  load.address = champsim::address{trigger_addr};
  load.v_address = load.address;
  load.cpu = 0;
  load.cap = trigger_cap;
  REQUIRE(mock_ul.issue(load));

  for (auto i = 0; i < 200; ++i)
    for (auto elem : elements)
      elem->_operate();

  REQUIRE(prefetch_caps_seen_below.count(in_hook_target) == 1);
  REQUIRE(prefetch_caps_seen_below.count(cycle_target) == 1);

  const auto& in_hook_cap = prefetch_caps_seen_below.at(in_hook_target);
  if (inherit) {
    CHECK(in_hook_cap.tag);
    CHECK(in_hook_cap.base == trigger_cap.base);
    CHECK(in_hook_cap.length == trigger_cap.length);
  } else {
    CHECK_FALSE(in_hook_cap.tag);
  }

  // Outside cache_operate there is no trigger, so a no-cap prefetch stays untagged either way.
  CHECK_FALSE(prefetch_caps_seen_below.at(cycle_target).tag);
}
} // namespace

TEST_CASE("With inherit_trigger_cap, a no-cap prefetch issued inside cache_operate carries the trigger's capability") { run_scenario(true); }

TEST_CASE("Without inherit_trigger_cap, a no-cap prefetch issued inside cache_operate stays untagged") { run_scenario(false); }
