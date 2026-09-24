#include <catch.hpp>
#include <map>
#include <vector>

#include "cache.h"
#include "defaults.hpp"
#include "mocks.hpp"

namespace
{
std::map<CACHE*, std::vector<champsim::capability>> operate_caps;
std::map<CACHE*, std::vector<champsim::capability>> evicted_caps;

struct cap_recorder : champsim::modules::prefetcher {
  using prefetcher::prefetcher;

  uint32_t prefetcher_cache_operate(champsim::address, champsim::address, uint32_t, champsim::capability cap, bool, bool, access_type, uint32_t metadata_in,
                                    uint32_t)
  {
    ::operate_caps[intern_].push_back(cap);
    return metadata_in;
  }

  uint32_t prefetcher_cache_fill(champsim::address, champsim::address, uint32_t, champsim::capability, bool, long, long, bool, champsim::address evicted_addr,
                                 champsim::capability evicted_cap, uint32_t metadata_in, uint32_t, uint32_t)
  {
    if (evicted_addr != champsim::address{})
      ::evicted_caps[intern_].push_back(evicted_cap);
    return metadata_in;
  }
};

champsim::capability make_cap(uint64_t base, uint64_t length)
{
  return champsim::capability{champsim::address{0}, champsim::address{base}, champsim::address{length}, 0x3, true};
}

bool same_cap(const champsim::capability& lhs, const champsim::capability& rhs)
{
  return lhs.tag == rhs.tag && lhs.base == rhs.base && lhs.length == rhs.length && lhs.permissions == rhs.permissions;
}
} // namespace

SCENARIO("The authorizing capability reaches prefetchers at every level, and the victim's capability reaches the fill hook")
{
  GIVEN("Two single-block caches chained above a memory")
  {
    do_nothing_MRC mock_ll;
    to_rq_MRP mock_ul;
    champsim::channel l1_to_l2{};
    CACHE uut_l1{champsim::cache_builder{champsim::defaults::default_l1d}
                     .name("433-uut-l1")
                     .sets(1)
                     .ways(1)
                     .upper_levels({&mock_ul.queues})
                     .lower_level(&l1_to_l2)
                     .prefetcher<::cap_recorder>()};
    CACHE uut_l2{champsim::cache_builder{champsim::defaults::default_l2c}
                     .name("433-uut-l2")
                     .sets(1)
                     .ways(1)
                     .upper_levels({&l1_to_l2})
                     .lower_level(&mock_ll.queues)
                     .prefetcher<::cap_recorder>()};

    std::array<champsim::operable*, 4> elements{{&mock_ll, &mock_ul, &uut_l1, &uut_l2}};

    for (auto elem : elements) {
      elem->initialize();
      elem->warmup = false;
      elem->begin_phase();
    }

    for (auto* c : {&uut_l1, &uut_l2}) {
      ::operate_caps[c].clear();
      ::evicted_caps[c].clear();
    }

    const auto cap_a = make_cap(0xdeadbe00, 256);
    const auto cap_b = make_cap(0xcafeba00, 4096);

    WHEN("Two loads with different capabilities map to the same block")
    {
      decltype(mock_ul)::request_type test_a;
      test_a.address = champsim::address{0xdeadbeef};
      test_a.v_address = test_a.address;
      test_a.cpu = 0;
      test_a.cap = cap_a;
      auto result_a = mock_ul.issue(test_a);

      for (auto i = 0; i < 100; ++i)
        for (auto elem : elements)
          elem->_operate();

      decltype(mock_ul)::request_type test_b;
      test_b.address = champsim::address{0xcafebabe};
      test_b.v_address = test_b.address;
      test_b.cpu = 0;
      test_b.cap = cap_b;
      auto result_b = mock_ul.issue(test_b);

      for (auto i = 0; i < 100; ++i)
        for (auto elem : elements)
          elem->_operate();

      THEN("Both issues are received")
      {
        CHECK(result_a);
        CHECK(result_b);
      }

      THEN("Each level's cache_operate sees the demand's capability")
      {
        for (auto* c : {&uut_l1, &uut_l2}) {
          REQUIRE(std::size(::operate_caps[c]) >= 2);
          CHECK(same_cap(::operate_caps[c].front(), cap_a));
          CHECK(same_cap(::operate_caps[c].back(), cap_b));
        }
      }

      THEN("Each level's cache_fill sees the victim's capability as evicted_cap")
      {
        for (auto* c : {&uut_l1, &uut_l2}) {
          REQUIRE(std::size(::evicted_caps[c]) == 1);
          CHECK(same_cap(::evicted_caps[c].front(), cap_a));
        }
      }
    }
  }
}
