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
#include <cstdint>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "cheri.h"

namespace champsim {

// Per-CPU shadow of the tagged 16-byte capability slots in memory, keyed by (virtual address >> 4).
//
// Layout:
//  - Every slot is a 16-byte entry {key, offset, cap_id}. cap_id indexes an interned descriptor holding every
//    capability field except the offset (base, length, permissions, tag).
//  - cap_id == TOMBSTONE marks an invalidated slot. Otherwise, BIG_OFFSET_FLAG in cap_id means the offset does not
//    fit in 32 bits and is kept in big_offsets_. Always test for TOMBSTONE before testing the flag.
//  - big_offsets_ holds exactly the keys whose current live value carries BIG_OFFSET_FLAG. It is updated on every
//    store and invalidation of that key.
//
// Before finalize() (presimpoint loading): entries_ is an append-only log. The last record for a key wins, and a
// TOMBSTONE record erases the key. The log is compacted (stable sort by key, keep the last record per key, drop
// tombstones) when it reaches max(2 x the size after the previous compaction, MIN_COMPACT_RECORDS), and before
// any query.
//
// After finalize(): entries_ is "main", sorted by key with unique keys; delta_ holds keys absent from main.
// Invariant: every key lives in exactly one of main or delta. A store to a key present in main (including a
// tombstoned one) updates it in place; only keys absent from main go to delta, and delta never holds tombstones.
// Lookups may skip the delta probe when delta_ is empty. delta_ is merged into main (dropping tombstones) when
// it grows past max(MIN_DELTA_MERGE, main / 8).
class capability_memory {
private:
  static constexpr uint64_t CAP_ALIGNMENT_BITS = 4;

  static constexpr uint32_t TOMBSTONE = std::numeric_limits<uint32_t>::max();
  static constexpr uint32_t BIG_OFFSET_FLAG = uint32_t{1} << 31;
  static constexpr uint32_t MAX_DESC_ID = BIG_OFFSET_FLAG - 2; // 2^31 - 2; with the flag set it never equals TOMBSTONE
  static constexpr std::size_t MIN_COMPACT_RECORDS = 1'000'000;
  static constexpr std::size_t MIN_DELTA_MERGE = 1'000'000;

  struct cap_descriptor {
    uint64_t base;
    uint64_t length;
    uint32_t permissions;
    bool tag;

    bool operator==(const cap_descriptor& o) const
    {
      return base == o.base && length == o.length && permissions == o.permissions && tag == o.tag;
    }
  };

  struct cap_descriptor_hash {
    size_t operator()(const cap_descriptor& o) const
    {
      uint64_t h = o.base;
      h ^= o.length + 0x9e3779b9 + (h << 6) + (h >> 2);
      h ^= (static_cast<uint64_t>(o.permissions) * 0xff51afd7ed558ccdULL);
      h ^= static_cast<uint64_t>(o.tag) << 63;
      h ^= (h >> 33);
      return static_cast<size_t>(h);
    }
  };

  struct packed_value {
    uint32_t offset;
    uint32_t cap_id;
  };

  struct entry {
    uint64_t key;
    uint32_t offset;
    uint32_t cap_id;
  };
  static_assert(sizeof(entry) == 16);

  mutable std::vector<entry> entries_;     // the log before finalize(), main after
  mutable std::size_t compacted_size_ = 0; // entries_.size() after the last compaction
  mutable bool log_dirty_ = false;         // the log has records since the last compaction

  std::unordered_map<uint64_t, packed_value> delta_;
  std::unordered_map<uint64_t, uint64_t> big_offsets_;

  std::vector<cap_descriptor> cap_table_;
  std::unordered_map<cap_descriptor, uint32_t, cap_descriptor_hash> intern_;

  std::size_t live_ = 0; // live (non-tombstone) keys across main and delta, after finalize()
  bool finalized_ = false;

  static uint64_t addr_to_key(champsim::address addr)
  {
    return addr.to<uint64_t>() >> CAP_ALIGNMENT_BITS;
  }

  uint32_t intern(const capability& cap);
  packed_value encode(uint64_t key, const capability& cap);
  std::optional<capability> decode(uint64_t key, uint32_t offset, uint32_t cap_id) const;

  void compact_log() const;
  void maybe_compact();
  void maybe_merge();

  const entry* find_main(uint64_t key) const;
  entry* find_main(uint64_t key);
  std::optional<packed_value> find(uint64_t key) const;

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
