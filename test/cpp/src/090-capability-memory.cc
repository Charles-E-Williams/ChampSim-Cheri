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
