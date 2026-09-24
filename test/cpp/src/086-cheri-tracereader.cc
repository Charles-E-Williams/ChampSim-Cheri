#include <catch.hpp>
#include <cstring>
#include <sstream>

#include "capability_memory.h"
#include "tracereader.h"

namespace
{
constexpr uint64_t presimpoint_ip = 0xdead0000;
constexpr uint64_t cap_slot = 0x10000;

cheri_instr presimpoint_entry(uint64_t dmem, uint64_t base, uint64_t length)
{
  cheri_instr entry{};
  entry.ip = presimpoint_ip;
  entry.destination_memory[0] = dmem;
  entry.cap_base = base;
  entry.cap_length = length;
  entry.cap_offset = dmem - base;
  entry.cap_perms = 0x3;
  entry.cap_tag = 1;
  entry.cap_op = static_cast<unsigned char>(champsim::cap_op_type::PRESIMPOINT);
  return entry;
}

cheri_instr plain_entry(uint64_t ip)
{
  cheri_instr entry{};
  entry.ip = ip;
  entry.destination_registers[0] = 1;
  entry.source_memory[0] = cap_slot;
  entry.auth_base = cap_slot;
  entry.auth_length = 64;
  entry.auth_tag = 1;
  entry.cap_op = static_cast<unsigned char>(champsim::cap_op_type::AUTH);
  return entry;
}

std::string make_trace()
{
  std::vector<cheri_instr> entries{presimpoint_entry(cap_slot, cap_slot, 64), presimpoint_entry(cap_slot + 0x10, cap_slot, 64), plain_entry(0x400000),
                                   plain_entry(0x400004), plain_entry(0x400008), plain_entry(0x40000c)};
  std::string bytes(entries.size() * sizeof(cheri_instr), '\0');
  std::memcpy(bytes.data(), entries.data(), bytes.size());
  return bytes;
}
} // namespace

SCENARIO("The CHERI tracereader consumes presimpoint entries into capability memory")
{
  GIVEN("A CHERI trace that starts with presimpoint entries")
  {
    const auto trace = make_trace();
    champsim::bulk_tracereader<cheri_instr, std::istringstream> uut{0, std::istringstream{trace}};

    WHEN("The first instructions are read")
    {
      auto inst0 = uut();
      auto inst1 = uut();
      auto inst2 = uut();

      THEN("Presimpoint entries never become instructions")
      {
        CHECK(inst0.ip == champsim::address{0x400000});
        CHECK(inst1.ip == champsim::address{0x400004});
        CHECK(inst2.ip == champsim::address{0x400008});
        for (const auto& inst : {inst0, inst1, inst2}) {
          CHECK(inst.ip != champsim::address{presimpoint_ip});
          CHECK_FALSE(inst.is_presimpoint);
        }
      }

      THEN("The authorizing capability is carried on the instruction")
      {
        CHECK(inst0.auth_cap.tag);
        CHECK(inst0.auth_cap.base == champsim::address{cap_slot});
      }

      THEN("Capability memory is loaded and finalized")
      {
        REQUIRE(champsim::cap_mem[0].is_finalized());
        REQUIRE(champsim::cap_mem[0].size() == 2);
        auto cap = champsim::cap_mem[0].load_capability(champsim::address{cap_slot + 0x10});
        REQUIRE(cap.has_value());
        CHECK(cap->base == champsim::address{cap_slot});
        CHECK(cap->offset == champsim::address{0x10});
      }

      AND_WHEN("The trace wraps after a runtime store changed a presimpoint slot")
      {
        champsim::capability runtime_cap{champsim::address{0}, champsim::address{0x70000}, champsim::address{16}, 0x1, true};
        champsim::cap_mem[0].store_capability(champsim::address{cap_slot}, runtime_cap);
        champsim::cap_mem[0].invalidate_tag(champsim::address{cap_slot + 0x10});

        champsim::bulk_tracereader<cheri_instr, std::istringstream> wrapped{0, std::istringstream{trace}};
        auto winst0 = wrapped();

        THEN("The presimpoint entries are skipped again and do not reload capability memory")
        {
          CHECK(winst0.ip == champsim::address{0x400000});
          auto cap = champsim::cap_mem[0].load_capability(champsim::address{cap_slot});
          REQUIRE(cap.has_value());
          CHECK(cap->base == champsim::address{0x70000});
          CHECK_FALSE(champsim::cap_mem[0].load_capability(champsim::address{cap_slot + 0x10}).has_value());
        }
      }
    }
  }
}
