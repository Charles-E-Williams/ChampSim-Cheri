#include <catch.hpp>

#include "capability_memory.h"

namespace
{
champsim::capability make_cap(uint64_t base, uint64_t length, uint64_t offset, uint32_t perms = 0x3)
{
  return champsim::capability{champsim::address{offset}, champsim::address{base}, champsim::address{length}, perms, true};
}
} // namespace

SCENARIO("Capability memory keeps presimpoint capabilities across finalize")
{
  GIVEN("A capability memory loaded with presimpoint stores")
  {
    champsim::capability_memory uut;
    uut.store_capability(champsim::address{0x1000}, make_cap(0x1000, 64, 0));
    uut.store_capability(champsim::address{0x1010}, make_cap(0x1000, 64, 0x10));
    uut.store_capability(champsim::address{0x2000}, make_cap(0x2000, 4096, 0));
    REQUIRE_FALSE(uut.is_finalized());
    REQUIRE(uut.size() == 3);

    WHEN("It is finalized")
    {
      uut.finalize();

      THEN("Every capability is still visible with its fields intact")
      {
        REQUIRE(uut.is_finalized());
        REQUIRE(uut.size() == 3);
        auto cap = uut.load_capability(champsim::address{0x1010});
        REQUIRE(cap.has_value());
        CHECK(cap->tag);
        CHECK(cap->base == champsim::address{0x1000});
        CHECK(cap->length == champsim::address{64});
        CHECK(cap->offset == champsim::address{0x10});
        CHECK(cap->permissions == 0x3);
      }

      THEN("Addresses in the same 16-byte slot map to the same capability")
      {
        auto cap = uut.load_capability(champsim::address{0x101f});
        REQUIRE(cap.has_value());
        CHECK(cap->offset == champsim::address{0x10});
      }

      THEN("Unstored slots have no capability") { REQUIRE_FALSE(uut.load_capability(champsim::address{0x1020}).has_value()); }

      AND_WHEN("It is finalized a second time (trace wrap)")
      {
        uut.finalize();

        THEN("Nothing changes")
        {
          REQUIRE(uut.size() == 3);
          REQUIRE(uut.load_capability(champsim::address{0x2000}).has_value());
        }
      }

      AND_WHEN("A presimpoint slot is invalidated")
      {
        uut.invalidate_tag(champsim::address{0x1000});

        THEN("It no longer loads, and the others are untouched")
        {
          REQUIRE_FALSE(uut.load_capability(champsim::address{0x1000}).has_value());
          REQUIRE_FALSE(uut.has_capability(champsim::address{0x1000}));
          REQUIRE(uut.load_capability(champsim::address{0x1010}).has_value());
          REQUIRE(uut.size() == 2);
        }

        AND_WHEN("A new capability is stored there")
        {
          uut.store_capability(champsim::address{0x1000}, make_cap(0x8000, 128, 0x20));

          THEN("The new capability is returned")
          {
            auto cap = uut.load_capability(champsim::address{0x1000});
            REQUIRE(cap.has_value());
            CHECK(cap->base == champsim::address{0x8000});
            CHECK(cap->offset == champsim::address{0x20});
          }
        }
      }

      AND_WHEN("A presimpoint slot is overwritten by a store")
      {
        uut.store_capability(champsim::address{0x2000}, make_cap(0x9000, 256, 0));

        THEN("The store takes priority over the presimpoint entry")
        {
          auto cap = uut.load_capability(champsim::address{0x2000});
          REQUIRE(cap.has_value());
          CHECK(cap->base == champsim::address{0x9000});
        }

        AND_WHEN("It is then invalidated")
        {
          uut.invalidate_tag(champsim::address{0x2000});
          THEN("Neither the store nor the presimpoint entry is visible") { REQUIRE_FALSE(uut.load_capability(champsim::address{0x2000}).has_value()); }
        }
      }

      AND_WHEN("An untagged capability is stored")
      {
        auto untagged = make_cap(0x1000, 64, 0);
        untagged.tag = false;
        uut.store_capability(champsim::address{0x1000}, untagged);

        THEN("The slot is invalidated") { REQUIRE_FALSE(uut.load_capability(champsim::address{0x1000}).has_value()); }
      }

      AND_WHEN("A new slot is stored after finalize")
      {
        uut.store_capability(champsim::address{0x3000}, make_cap(0x3000, 32, 0));
        THEN("It is visible") { REQUIRE(uut.load_capability(champsim::address{0x3000}).has_value()); }
      }
    }
  }
}

TEST_CASE("090-size: a capability store over a presimpoint key, invalidate and re-store count the key once")
{
  champsim::capability_memory uut;
  uut.store_capability(champsim::address{0x1000}, make_cap(0x1000, 64, 0));
  uut.store_capability(champsim::address{0x1010}, make_cap(0x1000, 64, 0x10));
  uut.store_capability(champsim::address{0x2000}, make_cap(0x2000, 4096, 0));
  uut.finalize();
  CHECK(uut.size() == 3);

  uut.store_capability(champsim::address{0x2000}, make_cap(0x9000, 256, 0));
  CHECK(uut.size() == 3);
  CHECK(uut.has_capability(champsim::address{0x2000}));
  CHECK(uut.load_capability(champsim::address{0x2000}).value_or(champsim::capability{}).base == champsim::address{0x9000});

  uut.invalidate_tag(champsim::address{0x2000});
  CHECK(uut.size() == 2);
  CHECK_FALSE(uut.has_capability(champsim::address{0x2000}));
  CHECK_FALSE(uut.load_capability(champsim::address{0x2000}).has_value());

  uut.store_capability(champsim::address{0x2000}, make_cap(0x9000, 256, 0x10));
  CHECK(uut.size() == 3);
  CHECK(uut.has_capability(champsim::address{0x2000}));
  CHECK(uut.load_capability(champsim::address{0x2000}).value_or(champsim::capability{}).offset == champsim::address{0x10});
}

TEST_CASE("090-size: a new key stored and invalidated after finalize counts once, then zero")
{
  champsim::capability_memory uut;
  uut.store_capability(champsim::address{0x1000}, make_cap(0x1000, 64, 0));
  uut.store_capability(champsim::address{0x1010}, make_cap(0x1000, 64, 0x10));
  uut.store_capability(champsim::address{0x2000}, make_cap(0x2000, 4096, 0));
  uut.finalize();
  CHECK(uut.size() == 3);

  uut.store_capability(champsim::address{0x3000}, make_cap(0x3000, 32, 0));
  CHECK(uut.size() == 4);
  CHECK(uut.has_capability(champsim::address{0x3000}));

  uut.invalidate_tag(champsim::address{0x3000});
  CHECK(uut.size() == 3);
  CHECK_FALSE(uut.has_capability(champsim::address{0x3000}));
  CHECK_FALSE(uut.load_capability(champsim::address{0x3000}).has_value());
}

namespace
{
constexpr uint64_t big_offset = (uint64_t{1} << 32) + 0x40; // does not fit in 32 bits

champsim::capability big_cap(uint64_t base) { return make_cap(base, uint64_t{1} << 40, big_offset); }

uint64_t loaded_offset(const champsim::capability_memory& m, uint64_t addr)
{
  return m.load_capability(champsim::address{addr}).value_or(champsim::capability{}).offset.to<uint64_t>();
}
} // namespace

TEST_CASE("090-loading: repeated overwrites and invalidations before finalize keep the last record")
{
  champsim::capability_memory uut;
  uut.store_capability(champsim::address{0x1000}, make_cap(0x1000, 64, 0));
  CHECK(uut.size() == 1);
  uut.store_capability(champsim::address{0x1000}, make_cap(0x1000, 64, 0x8));
  CHECK(uut.size() == 1);
  uut.invalidate_tag(champsim::address{0x1000});
  CHECK(uut.size() == 0);
  CHECK_FALSE(uut.has_capability(champsim::address{0x1000}));
  uut.store_capability(champsim::address{0x1000}, make_cap(0x5000, 128, 0x20));
  CHECK(uut.size() == 1);
  uut.invalidate_tag(champsim::address{0x7000}); // a key that exists nowhere
  CHECK(uut.size() == 1);

  uut.finalize();
  CHECK(uut.size() == 1);
  auto cap = uut.load_capability(champsim::address{0x1000});
  REQUIRE(cap.has_value());
  CHECK(cap->base == champsim::address{0x5000});
  CHECK(cap->offset == champsim::address{0x20});
}

TEST_CASE("090-big-offset: a big offset stored before finalize and overwritten keeps the last record")
{
  champsim::capability_memory uut;
  uut.store_capability(champsim::address{0x1000}, big_cap(0x1000));
  CHECK(uut.size() == 1);
  CHECK(loaded_offset(uut, 0x1000) == big_offset);

  uut.store_capability(champsim::address{0x1000}, make_cap(0x1000, 64, 0x10)); // small wins
  CHECK(uut.size() == 1);
  uut.store_capability(champsim::address{0x2000}, make_cap(0x2000, 64, 0x10));
  uut.store_capability(champsim::address{0x2000}, big_cap(0x2000)); // big wins
  CHECK(uut.size() == 2);

  uut.finalize();
  CHECK(uut.size() == 2);
  CHECK(loaded_offset(uut, 0x1000) == 0x10);
  CHECK(loaded_offset(uut, 0x2000) == big_offset);
}

TEST_CASE("090-big-offset: after finalize, a big offset round-trips, is overwritten by a small one, and is invalidated")
{
  champsim::capability_memory uut;
  uut.store_capability(champsim::address{0x1000}, make_cap(0x1000, 64, 0));
  uut.finalize();
  CHECK(uut.size() == 1);

  // In main (a presimpoint key)
  uut.store_capability(champsim::address{0x1000}, big_cap(0x1000));
  CHECK(uut.size() == 1);
  CHECK(loaded_offset(uut, 0x1000) == big_offset);
  uut.store_capability(champsim::address{0x1000}, make_cap(0x1000, 64, 0x30));
  CHECK(uut.size() == 1);
  CHECK(loaded_offset(uut, 0x1000) == 0x30);
  uut.store_capability(champsim::address{0x1000}, big_cap(0x1000));
  CHECK(loaded_offset(uut, 0x1000) == big_offset);
  uut.invalidate_tag(champsim::address{0x1000});
  CHECK(uut.size() == 0);
  CHECK_FALSE(uut.load_capability(champsim::address{0x1000}).has_value());
  uut.store_capability(champsim::address{0x1000}, make_cap(0x1000, 64, 0x40)); // re-store a tombstoned main key
  CHECK(uut.size() == 1);
  CHECK(loaded_offset(uut, 0x1000) == 0x40);

  // In delta (a new key)
  uut.store_capability(champsim::address{0x9000}, big_cap(0x9000));
  CHECK(uut.size() == 2);
  CHECK(loaded_offset(uut, 0x9000) == big_offset);
  uut.store_capability(champsim::address{0x9000}, make_cap(0x9000, 64, 0x50));
  CHECK(uut.size() == 2);
  CHECK(loaded_offset(uut, 0x9000) == 0x50);
  uut.invalidate_tag(champsim::address{0x9000});
  CHECK(uut.size() == 1);
  CHECK_FALSE(uut.load_capability(champsim::address{0x9000}).has_value());
}

TEST_CASE("090-no-op: invalidating a key that exists nowhere after finalize changes nothing")
{
  champsim::capability_memory uut;
  uut.store_capability(champsim::address{0x1000}, make_cap(0x1000, 64, 0));
  uut.finalize();
  CHECK(uut.size() == 1);
  uut.invalidate_tag(champsim::address{0x7000});
  CHECK(uut.size() == 1);
  CHECK_FALSE(uut.has_capability(champsim::address{0x7000}));
  CHECK(uut.has_capability(champsim::address{0x1000}));
}

TEST_CASE("090-merge: keys stored after finalize survive a delta merge, then are overwritten and invalidated")
{
  champsim::capability_memory uut;
  uut.store_capability(champsim::address{0x1000}, make_cap(0x1000, 64, 0));
  uut.store_capability(champsim::address{0x1010}, make_cap(0x1000, 64, 0x10));
  uut.store_capability(champsim::address{0x2000}, make_cap(0x2000, 64, 0));
  uut.finalize();
  uut.invalidate_tag(champsim::address{0x2000}); // a tombstone in main, dropped by the merge
  CHECK(uut.size() == 2);

  // Two keys that go to delta first: one with a small and one with a big offset
  constexpr uint64_t k_small = 0x10000;
  constexpr uint64_t k_big = 0x10010;
  uut.store_capability(champsim::address{k_small}, make_cap(k_small, 64, 0x8));
  uut.store_capability(champsim::address{k_big}, big_cap(k_big));
  CHECK(uut.size() == 4);

  // More than 1M further new slots cross the merge threshold
  constexpr uint64_t fill_base = uint64_t{1} << 40;
  constexpr uint64_t fill_count = 1'000'001;
  for (uint64_t i = 0; i < fill_count; ++i)
    uut.store_capability(champsim::address{fill_base + (i << 4)}, make_cap(fill_base, 64, 0));
  CHECK(uut.size() == 4 + fill_count);

  // Everything still loads correctly after the merge
  CHECK(loaded_offset(uut, k_small) == 0x8);
  CHECK(loaded_offset(uut, k_big) == big_offset);
  CHECK(uut.has_capability(champsim::address{0x1000}));
  CHECK_FALSE(uut.has_capability(champsim::address{0x2000}));
  CHECK(uut.has_capability(champsim::address{fill_base + ((fill_count - 1) << 4)}));

  // Overwrite and invalidate the merged keys
  uut.store_capability(champsim::address{k_small}, make_cap(k_small, 64, 0x18));
  CHECK(uut.size() == 4 + fill_count);
  CHECK(loaded_offset(uut, k_small) == 0x18);
  uut.store_capability(champsim::address{k_big}, make_cap(k_big, 64, 0x28)); // big -> small
  CHECK(uut.size() == 4 + fill_count);
  CHECK(loaded_offset(uut, k_big) == 0x28);
  uut.invalidate_tag(champsim::address{k_small});
  CHECK(uut.size() == 3 + fill_count);
  CHECK_FALSE(uut.load_capability(champsim::address{k_small}).has_value());
  uut.invalidate_tag(champsim::address{k_big});
  CHECK(uut.size() == 2 + fill_count);
  CHECK_FALSE(uut.load_capability(champsim::address{k_big}).has_value());

  // The tombstoned presimpoint key can still be restored
  uut.store_capability(champsim::address{0x2000}, make_cap(0x2000, 64, 0x4));
  CHECK(uut.size() == 3 + fill_count);
  CHECK(loaded_offset(uut, 0x2000) == 0x4);
}
