#include <catch.hpp>
#include <map>
#include <optional>
#include <vector>

#include "../../../prefetcher/ampm_cheri/ampm_cheri.h"
#include "../../../prefetcher/sms_cheri/sms_cheri.h"
#include "cache.h"
#include "cheri_prefetch_utils.h"
#include "defaults.hpp"
#include "mocks.hpp"

// Access to sms_cheri internals (declared a friend in sms_cheri.h)
struct sms_cheri_test_access {
  static auto decompose(const sms_cheri& p, uint64_t pa, const champsim::capability& cap) { return p.decompose(pa, cap); }
  static void buffer(sms_cheri& p, std::vector<uint64_t> pref_addr, const champsim::capability& cap, uint64_t demand_pa)
  {
    p.buffer_prefetch(pref_addr, cap, p.decompose(demand_pa, cap));
  }
  static const auto& pref_buffer(const sms_cheri& p) { return p.pref_buffer; }
};

namespace
{
champsim::capability make_cap(uint64_t base, uint64_t length, uint64_t offset, bool tag = true)
{
  return champsim::capability{champsim::address{offset}, champsim::address{base}, champsim::address{length}, cheri::PERM_LOAD, tag};
}

// A physical cache with no prefetcher, only used to construct standalone prefetcher modules
struct host_cache {
  do_nothing_MRC mock_ll;
  to_rq_MRP mock_ul;
  CACHE uut{champsim::cache_builder{champsim::defaults::default_l2c}.name("439-host").upper_levels({&mock_ul.queues}).lower_level(&mock_ll.queues)};
};
} // namespace

TEST_CASE("439-1: line_va_from_cursor accepts a cursor in the accessed line and rejects a different line or an untagged cap")
{
  // Cursor VA 0x7fff12345e48, physical line 0xdeadbe40: same line position (0xe40 >> 6) within the page
  const auto cap = make_cap(0x7fff12340000, 0x10000, 0x5e48);
  const auto va = cheri::line_va_from_cursor(cap, champsim::address{0xdeadbe40});
  REQUIRE(va.has_value());
  CHECK(*va == champsim::address{0x7fff12345e40});

  CHECK_FALSE(cheri::line_va_from_cursor(cap, champsim::address{0xdeadbe80}).has_value()); // different line in the page
  CHECK_FALSE(cheri::line_va_from_cursor(make_cap(0x7fff12340000, 0x10000, 0x5e48, false), champsim::address{0xdeadbe40}).has_value()); // untagged
}

TEST_CASE("439-2: sms_cheri buffers each target with the capability re-pointed at the target line")
{
  host_cache h;
  sms_cheri p{&h.uut};
  // Object [0x40000, 0x50000); demand cursor 0x41100, demand physical line 0x7100 (same page offset)
  const auto cap = make_cap(0x40000, 0x10000, 0x1100);
  const auto ri = sms_cheri_test_access::decompose(p, 0x7100, cap);
  CHECK(ri.demand_pa_line == 0x7100);
  CHECK(ri.demand_obj_line == 0x44);

  sms_cheri_test_access::buffer(p, {0x7140, 0x7000}, cap, 0x7100);
  const auto& buffered = sms_cheri_test_access::pref_buffer(p);
  REQUIRE(buffered.size() == 2);
  CHECK(buffered[0].first == 0x7140);
  CHECK(buffered[0].second.offset == champsim::address{0x1140}); // cursor 0x41140
  CHECK(buffered[1].first == 0x7000);
  CHECK(buffered[1].second.offset == champsim::address{0x1000}); // cursor 0x41000
  for (const auto& [pa, c] : buffered) {
    CHECK(c.base == cap.base);
    CHECK(c.length == cap.length);
    CHECK(c.tag);
  }
}

TEST_CASE("439-3: AMPM-CHERI eviction cleanup works from evicted_cap, and is skipped when the cursor is not in the evicted line")
{
  host_cache h;
  ampm_cheri p{&h.uut};
  // A large object (routed to the capability path); the prefetched line has VA base + 0x2040 and PA 0x9040
  const uint64_t base = 0x100000;
  const champsim::address line_va{base + 0x2040};
  const champsim::address line_pa{0x9040};
  const auto line_cap = make_cap(base, 1 << 20, 0x2040); // e.g. an own prefetch's re-pointed capability

  p.add_to_map(line_va, line_cap, true);
  REQUIRE(p.check_map(line_va, line_cap, true));

  SECTION("cursor in the evicted line: the zone bit is cleared")
  {
    p.prefetcher_cache_fill(champsim::address{0xa000}, {}, 0, {}, true, 0, 0, false, line_pa, line_cap, 0, 0, 0);
    CHECK_FALSE(p.check_map(line_va, line_cap, true));
    CHECK(p.evict_cursor_check_failed == 0);
  }

  SECTION("cursor in a different line: cleanup skipped and counted")
  {
    const auto wrong = make_cap(base, 1 << 20, 0x2100);
    p.prefetcher_cache_fill(champsim::address{0xa000}, {}, 0, {}, true, 0, 0, false, line_pa, wrong, 0, 0, 0);
    CHECK(p.check_map(line_va, line_cap, true));
    CHECK(p.evict_cursor_check_failed == 1);
  }
}

namespace
{
std::map<CACHE*, std::vector<champsim::capability>> fill_evicted_caps;

struct next_line_no_cap : champsim::modules::prefetcher {
  using prefetcher::prefetcher;
  uint32_t prefetcher_cache_operate(champsim::address addr, champsim::address, uint32_t, champsim::capability, bool, bool, access_type type,
                                    uint32_t metadata_in, uint32_t)
  {
    if (type == access_type::LOAD)
      prefetch_line(champsim::address{champsim::block_number{addr} + 1}, true, 0);
    return metadata_in;
  }
};

struct evicted_cap_recorder : champsim::modules::prefetcher {
  using prefetcher::prefetcher;
  uint32_t prefetcher_cache_fill(champsim::address, champsim::address, uint32_t, champsim::capability, bool, long, long, bool, champsim::address,
                                 champsim::capability evicted_cap, uint32_t metadata_in, uint32_t, uint32_t)
  {
    ::fill_evicted_caps[intern_].push_back(evicted_cap);
    return metadata_in;
  }
};

template <typename P>
struct physical_rig {
  do_nothing_MRC mock_ll;
  to_rq_MRP mock_ul{[](auto x, auto y) { return x.v_address == y.v_address; }};
  CACHE uut;
  std::array<champsim::operable*, 3> elements{{&mock_ll, &mock_ul, &uut}};

  physical_rig(long ways, bool inherit)
      : uut{[&] {
          auto b = champsim::cache_builder{champsim::defaults::default_l2c}
                       .name("439-rig")
                       .sets(1)
                       .ways(ways)
                       .upper_levels({&mock_ul.queues})
                       .lower_level(&mock_ll.queues)
                       .reset_virtual_prefetch()
                       .template prefetcher<P>();
          if (inherit)
            b.set_inherit_trigger_cap();
          return b;
        }()}
  {
    for (auto elem : elements) {
      elem->initialize();
      elem->warmup = false;
      elem->begin_phase();
    }
  }

  void load(uint64_t pa, uint64_t va, champsim::capability cap)
  {
    to_rq_MRP::request_type pkt;
    pkt.address = champsim::address{pa};
    pkt.v_address = champsim::address{va};
    pkt.cpu = 0;
    pkt.cap = cap;
    REQUIRE(mock_ul.issue(pkt));
    for (int i = 0; i < 200; ++i)
      for (auto elem : elements)
        elem->_operate();
  }
};
} // namespace

TEST_CASE("439-4: an own-prefetched line at a physical cache keeps a cursor that yields its VA for eviction cleanup")
{
  // Before the side channel was removed, such a block's v_address was 0, so AMPM-CHERI's cleanup skipped it
  physical_rig<next_line_no_cap> r{4, true};
  const uint64_t base = 0x7fff12340000;
  r.load(0xdeadbe40, 0x7fff12345e40, make_cap(base, 0x10000, 0x5e40)); // demand; prefetches the next line 0xdeadbe80

  std::optional<champsim::capability> prefetched_cap;
  for (const auto& blk : r.uut.block)
    if (blk.valid && blk.prefetch && blk.address == champsim::address{0xdeadbe80})
      prefetched_cap = blk.auth_cap; // becomes evicted_cap when this block is evicted
  REQUIRE(prefetched_cap.has_value());
  CHECK(prefetched_cap->tag);
  const auto evicted_va = cheri::line_va_from_cursor(*prefetched_cap, champsim::address{0xdeadbe80});
  REQUIRE(evicted_va.has_value());
  CHECK(*evicted_va == champsim::address{0x7fff12345e80});
}

TEST_CASE("439-5: a fill into an invalid way passes an untagged evicted_cap, even after an invalidation")
{
  physical_rig<evicted_cap_recorder> r{1, false};
  ::fill_evicted_caps[&r.uut].clear();
  const auto cap = make_cap(0x40000, 0x10000, 0x100);

  r.load(0x1000, 0x40100, cap); // fills the only (invalid) way
  REQUIRE(std::size(::fill_evicted_caps[&r.uut]) == 1);
  CHECK_FALSE(::fill_evicted_caps[&r.uut].back().tag);

  r.uut.invalidate_entry(champsim::address{0x1000}); // the way keeps a stale, tagged auth_cap but is invalid
  r.load(0x2000, 0x40200, cap);
  REQUIRE(std::size(::fill_evicted_caps[&r.uut]) == 2);
  CHECK_FALSE(::fill_evicted_caps[&r.uut].back().tag);
}
