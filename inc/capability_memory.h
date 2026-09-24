/*
 *    Copyright 2023 The ChampSim Contributors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef CAPABILITY_MEMORY_H
#define CAPABILITY_MEMORY_H

#include <algorithm>
#include <iostream>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "cheri.h"

namespace champsim {

class capability_memory {
private:
  static constexpr uint64_t CAP_ALIGNMENT_BITS = 4;

  struct cap_descriptor {
    uint64_t base;
    uint64_t length;
    uint32_t permissions;

    bool operator==(const cap_descriptor& o) const
    {
      return base == o.base && length == o.length && permissions == o.permissions;
    }
  };

  struct cap_descriptor_hash {
    size_t operator()(const cap_descriptor& o) const
    {
      uint64_t h = o.base;
      h ^= o.length + 0x9e3779b9 + (h << 6) + (h >> 2);
      h ^= (static_cast<uint64_t>(o.permissions) * 0xff51afd7ed558ccdULL);
      h ^= (h >> 33);
      return static_cast<size_t>(h);
    }
  };

  struct presimpoint_entry {
    uint64_t va_key;
    uint64_t offset;
    uint32_t cap_id;

    bool operator<(const presimpoint_entry& o) const { return va_key < o.va_key; }
  };

  std::unordered_map<uint64_t, capability> presimpoint_map_;

  std::vector<presimpoint_entry> presimpoint_entries_;
  std::vector<cap_descriptor> cap_table_;
  bool finalized_ = false;

  std::unordered_map<uint64_t, capability> simpoint_region_map_;
  std::unordered_set<uint64_t> invalidated_keys_;

  static uint64_t addr_to_key(champsim::address addr)
  {
    return addr.to<uint64_t>() >> CAP_ALIGNMENT_BITS;
  }

  capability rebuild(const presimpoint_entry& e) const;
  const presimpoint_entry* find_in_presimpoint(uint64_t key) const;

public:
  capability_memory() = default;

  void finalize();

  void store_capability(champsim::address addr, const capability& cap);
  std::optional<capability> load_capability(champsim::address addr) const;
  bool has_capability(champsim::address addr) const;
  void invalidate_tag(champsim::address addr);

  size_t size() const;
  void clear();
  bool is_finalized() const { return finalized_; }
};

extern std::vector<capability_memory> cap_mem;

void initialize_capability_memory(size_t num_cpus);

} // namespace champsim

#endif