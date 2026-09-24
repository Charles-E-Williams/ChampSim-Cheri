#include <catch.hpp>
#include <optional>

#include "cache.h"
#include "cheri_prefetch_utils.h"
#include "defaults.hpp"
#include "mocks.hpp"

namespace
{
// An object whose base is not line-aligned: [0x70000030, 0x70000130)
constexpr uint64_t obj_base = 0x70000030;
constexpr uint64_t obj_length = 0x100;

champsim::capability object_cap(uint64_t offset)
{
  return champsim::capability{champsim::address{offset}, champsim::address{obj_base}, champsim::address{obj_length}, cheri::PERM_LOAD, true};
}

// A virtual-prefetch cache (the prefetch address is the VA) that translates through a mock
struct virtual_cache {
  do_nothing_MRC mock_ll;
  do_nothing_MRC mock_lt;
  to_rq_MRP mock_ul;
  CACHE uut{champsim::cache_builder{champsim::defaults::default_l1d}
                .name("438-uut")
                .sets(1)
                .ways(8)
                .upper_levels({&mock_ul.queues})
                .lower_level(&mock_ll.queues)
                .lower_translate(&mock_lt.queues)
                .set_virtual_prefetch()};
  std::array<champsim::operable*, 4> elements{{&mock_ll, &mock_lt, &mock_ul, &uut}};

  virtual_cache()
  {
    for (auto elem : elements) {
      elem->initialize();
      elem->warmup = false;
      elem->begin_phase();
    }
  }

  // Issue a prefetch with an explicit capability, let it fill, and return the capability stored on the filled block
  champsim::capability prefetch_and_fill(uint64_t pf_va, champsim::capability cap)
  {
    REQUIRE(uut.prefetch_line(champsim::address{pf_va}, true, 0, cap));
    for (int i = 0; i < 200; ++i)
      for (auto elem : elements)
        elem->_operate();

    std::optional<champsim::capability> filled;
    for (const auto& blk : uut.block)
      if (blk.valid && champsim::block_number{blk.v_address} == champsim::block_number{champsim::address{pf_va}})
        filled = blk.auth_cap;
    REQUIRE(filled.has_value());
    return *filled;
  }
};

void check_unchanged_except_offset(const champsim::capability& got)
{
  CHECK(got.tag);
  CHECK(got.base == champsim::address{obj_base});
  CHECK(got.length == champsim::address{obj_length});
  CHECK(got.permissions == cheri::PERM_LOAD);
}
} // namespace

TEST_CASE("438-1: the first line of an object with an unaligned base re-points the cursor to base")
{
  virtual_cache c;
  const auto got = c.prefetch_and_fill(0x70000000, object_cap(0xc0)); // trigger cursor further into the object
  check_unchanged_except_offset(got);
  CHECK(got.offset == champsim::address{0}); // cursor == base, not the line start below it
}

TEST_CASE("438-2: a pointer-chase capability whose cursor is already inside the line is unchanged")
{
  virtual_cache c;
  const auto got = c.prefetch_and_fill(0x70000040, object_cap(0x20)); // cursor 0x70000050, inside line 0x70000040
  check_unchanged_except_offset(got);
  CHECK(got.offset == champsim::address{0x20});
}

TEST_CASE("438-3: a line entirely outside the object keeps the offset unchanged")
{
  virtual_cache c;
  const auto got = c.prefetch_and_fill(0x70001000, object_cap(0x40));
  check_unchanged_except_offset(got);
  CHECK(got.offset == champsim::address{0x40});
}

TEST_CASE("438-4: an explicit-capability prefetch carrying its trigger's cursor is re-pointed at the prefetched line")
{
  virtual_cache c;
  const auto got = c.prefetch_and_fill(0x70000100, object_cap(0x10)); // like ip_stride_cheri's lookahead: cursor at the trigger
  check_unchanged_except_offset(got);
  CHECK(got.offset == champsim::address{0x70000100 - obj_base});
}

TEST_CASE("438-5: prefetch_safe accepts the first and last partial lines of an unaligned object and rejects the lines just outside")
{
  const auto cap = object_cap(0);                                  // [0x70000030, 0x70000130)
  CHECK(cheri::prefetch_safe(champsim::address{0x70000000}, cap)); // first partial line
  CHECK(cheri::prefetch_safe(champsim::address{0x70000100}, cap)); // last partial line
  CHECK(cheri::prefetch_safe(champsim::address{0x70000080}, cap)); // interior line
  CHECK_FALSE(cheri::prefetch_safe(champsim::address{0x6fffffc0}, cap)); // line just below
  CHECK_FALSE(cheri::prefetch_safe(champsim::address{0x70000140}, cap)); // line just above

  auto no_load = cap;
  no_load.permissions = 0;
  CHECK_FALSE(cheri::prefetch_safe(champsim::address{0x70000080}, no_load));
}
