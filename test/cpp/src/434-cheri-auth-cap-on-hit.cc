#include <catch.hpp>

#include "cache.h"
#include "defaults.hpp"
#include "mocks.hpp"

namespace
{
champsim::capability make_cap(uint64_t base, uint64_t length)
{
  return champsim::capability{champsim::address{0}, champsim::address{base}, champsim::address{length}, 0x3, true};
}

bool same_cap(const champsim::capability& lhs, const champsim::capability& rhs)
{
  return lhs.tag == rhs.tag && lhs.base == rhs.base && lhs.length == rhs.length && lhs.permissions == rhs.permissions;
}
} // namespace

SCENARIO("Only tagged demand hits update a block's authorizing capability")
{
  GIVEN("A single-block cache")
  {
    do_nothing_MRC mock_ll;
    to_rq_MRP mock_ul_rq;
    to_pq_MRP mock_ul_pq;
    CACHE uut{champsim::cache_builder{champsim::defaults::default_l2c}
                  .name("434-uut")
                  .sets(1)
                  .ways(1)
                  .upper_levels({{&mock_ul_rq.queues, &mock_ul_pq.queues}})
                  .lower_level(&mock_ll.queues)};

    std::array<champsim::operable*, 4> elements{{&mock_ll, &mock_ul_rq, &mock_ul_pq, &uut}};

    for (auto elem : elements) {
      elem->initialize();
      elem->warmup = false;
      elem->begin_phase();
    }

    auto run = [&] {
      for (auto i = 0; i < 100; ++i)
        for (auto elem : elements)
          elem->_operate();
    };

    const champsim::address addr{0xdeadbeef};
    const auto fill_cap = make_cap(0xdeadbe00, 256);
    const auto demand_cap = make_cap(0xdead0000, 65536);
    const auto prefetch_cap = make_cap(0xcafe0000, 4096);

    auto make_packet = [&](access_type type, champsim::capability cap) {
      decltype(mock_ul_rq)::request_type pkt;
      pkt.address = addr;
      pkt.v_address = addr;
      pkt.cpu = 0;
      pkt.type = type;
      pkt.cap = cap;
      return pkt;
    };

    auto block_cap = [&] {
      REQUIRE(uut.block.front().valid);
      return uut.block.front().auth_cap;
    };

    WHEN("A block is filled, then hit by a tagged demand, a tagged prefetch, and an untagged demand")
    {
      REQUIRE(mock_ul_rq.issue(make_packet(access_type::LOAD, fill_cap)));
      run();
      THEN("The fill sets the block's capability") { REQUIRE(same_cap(block_cap(), fill_cap)); }

      REQUIRE(mock_ul_rq.issue(make_packet(access_type::LOAD, demand_cap)));
      run();
      REQUIRE(mock_ll.packet_count() == 1); // the demand hit
      THEN("A tagged demand hit sets the block's capability") { REQUIRE(same_cap(block_cap(), demand_cap)); }

      REQUIRE(mock_ul_pq.issue(make_packet(access_type::PREFETCH, prefetch_cap)));
      run();
      REQUIRE(mock_ll.packet_count() == 1); // the prefetch hit
      THEN("A tagged prefetch hit leaves it unchanged") { REQUIRE(same_cap(block_cap(), demand_cap)); }

      REQUIRE(mock_ul_rq.issue(make_packet(access_type::LOAD, champsim::capability{})));
      run();
      REQUIRE(mock_ll.packet_count() == 1); // the untagged demand hit
      THEN("An untagged demand hit leaves it unchanged") { REQUIRE(same_cap(block_cap(), demand_cap)); }
    }
  }
}
