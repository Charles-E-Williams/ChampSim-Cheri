#include <catch.hpp>
#include <map>
#include <vector>

#include "cache.h"
#include "defaults.hpp"
#include "mocks.hpp"

namespace
{
struct operate_call {
  champsim::address addr;
  access_type type;
  bool cap_tagged;
};

std::map<CACHE*, std::vector<operate_call>> operate_calls;
std::map<CACHE*, std::vector<champsim::address>> fill_calls;

struct counting_prefetcher : champsim::modules::prefetcher {
  using prefetcher::prefetcher;

  uint32_t prefetcher_cache_operate(champsim::address addr, champsim::address, uint32_t, champsim::capability cap, bool, bool, access_type type,
                                    uint32_t metadata_in, uint32_t)
  {
    ::operate_calls[intern_].push_back({addr, type, cap.tag});
    return metadata_in;
  }

  uint32_t prefetcher_cache_fill(champsim::address addr, champsim::address, uint32_t, champsim::capability, bool, long, long, bool, champsim::address,
                                 champsim::capability, uint32_t metadata_in, uint32_t, uint32_t)
  {
    ::fill_calls[intern_].push_back(addr);
    return metadata_in;
  }
};
} // namespace

SCENARIO("An instruction-fetch miss at L1I activates the L2C prefetcher when prefetch_activate includes LOAD")
{
  GIVEN("An L1I above an L2C whose prefetcher is activated by LOAD")
  {
    do_nothing_MRC mock_ll;
    to_rq_MRP mock_fetch;
    champsim::channel l1i_to_l2c{};
    CACHE l1i{champsim::cache_builder{champsim::defaults::default_l1i}
                  .name("436-L1I")
                  .upper_levels({&mock_fetch.queues})
                  .lower_level(&l1i_to_l2c)};
    CACHE l2c{champsim::cache_builder{champsim::defaults::default_l2c}
                  .name("436-L2C")
                  .upper_levels({&l1i_to_l2c})
                  .lower_level(&mock_ll.queues)
                  .prefetch_activate(access_type::LOAD, access_type::PREFETCH)
                  .prefetcher<::counting_prefetcher>()};

    std::array<champsim::operable*, 4> elements{{&mock_ll, &mock_fetch, &l1i, &l2c}};
    for (auto elem : elements) {
      elem->initialize();
      elem->warmup = false;
      elem->begin_phase();
    }
    ::operate_calls[&l2c].clear();
    ::fill_calls[&l2c].clear();

    WHEN("The core fetches an instruction block that misses in L1I")
    {
      // A fetch as O3_CPU::do_fetch_instruction issues it: a LOAD with no authorizing capability
      decltype(mock_fetch)::request_type fetch;
      fetch.address = champsim::address{0x400000};
      fetch.v_address = fetch.address;
      fetch.ip = fetch.address;
      fetch.cpu = 0;
      fetch.type = access_type::LOAD;
      REQUIRE(mock_fetch.issue(fetch));

      for (auto i = 0; i < 100; ++i)
        for (auto elem : elements)
          elem->_operate();

      THEN("The miss reaches L2C") { REQUIRE(mock_ll.packet_count() == 1); }

      THEN("The L2C prefetcher's cache_operate is called for it, with an untagged capability")
      {
        REQUIRE(std::size(::operate_calls[&l2c]) == 1);
        CHECK(::operate_calls[&l2c].front().type == access_type::LOAD);
        CHECK(champsim::block_number{::operate_calls[&l2c].front().addr} == champsim::block_number{fetch.address});
        CHECK_FALSE(::operate_calls[&l2c].front().cap_tagged);
      }

      THEN("The L2C prefetcher's cache_fill is called for the instruction line") { REQUIRE(std::size(::fill_calls[&l2c]) == 1); }
    }
  }
}
