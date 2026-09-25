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

#include "capability_memory.h"

#include <cassert>
#include <utility>

namespace champsim {

std::vector<capability_memory> cap_mem;

void initialize_capability_memory(size_t num_cpus)
{
  cap_mem.clear();
  cap_mem.resize(num_cpus);
}

// Intern every capability field except the offset. Descriptors are never freed.
uint32_t capability_memory::intern(const capability& cap)
{
  cap_descriptor desc{cap.base.to<uint64_t>(), cap.length.to<uint64_t>(), cap.permissions, cap.tag};
  auto [it, inserted] = intern_.try_emplace(desc, static_cast<uint32_t>(cap_table_.size()));
  if (inserted) {
    assert(cap_table_.size() <= MAX_DESC_ID);
    cap_table_.push_back(desc);
  }
  return it->second;
}

// Encode cap as the value for key. Offsets that don't fit in 32 bits go to big_offsets_; any previous big offset for
// key is erased otherwise, so big_offsets_ always matches the key's current value.
capability_memory::packed_value capability_memory::encode(uint64_t key, const capability& cap)
{
  const uint32_t id = intern(cap);
  const uint64_t offset = cap.offset.to<uint64_t>();
  if (offset <= std::numeric_limits<uint32_t>::max()) {
    big_offsets_.erase(key);
    return {static_cast<uint32_t>(offset), id};
  }
  big_offsets_.insert_or_assign(key, offset);
  return {0, id | BIG_OFFSET_FLAG};
}

std::optional<capability> capability_memory::decode(uint64_t key, uint32_t offset, uint32_t cap_id) const
{
  if (cap_id == TOMBSTONE)
    return std::nullopt;

  uint64_t full_offset = offset;
  if (cap_id & BIG_OFFSET_FLAG) {
    full_offset = big_offsets_.at(key);
    cap_id &= ~BIG_OFFSET_FLAG;
  }

  const auto& desc = cap_table_[cap_id];
  if (!desc.tag)
    return std::nullopt;
  return capability{champsim::address{full_offset}, champsim::address{desc.base}, champsim::address{desc.length}, desc.permissions, desc.tag};
}

// Sort the log by key (stable, so records for a key stay in arrival order), keep the last record per key and drop
// tombstones. big_offsets_ needs no change: it already matches each key's last record.
void capability_memory::compact_log() const
{
  std::stable_sort(entries_.begin(), entries_.end(), [](const entry& a, const entry& b) { return a.key < b.key; });

  auto out = entries_.begin();
  for (auto it = entries_.begin(); it != entries_.end();) {
    auto last = it;
    while (std::next(last) != entries_.end() && std::next(last)->key == it->key)
      ++last;
    if (last->cap_id != TOMBSTONE)
      *out++ = *last;
    it = std::next(last);
  }
  entries_.erase(out, entries_.end());

  compacted_size_ = entries_.size();
  log_dirty_ = false;
}

void capability_memory::maybe_compact()
{
  if (entries_.size() >= std::max(2 * compacted_size_, MIN_COMPACT_RECORDS))
    compact_log();
}

// Merge delta into main, dropping main's tombstones. Keys are disjoint between the two (see the header invariant).
void capability_memory::maybe_merge()
{
  if (delta_.size() <= std::max(MIN_DELTA_MERGE, entries_.size() / 8))
    return;

  std::vector<entry> delta_sorted;
  delta_sorted.reserve(delta_.size());
  for (const auto& [key, value] : delta_)
    delta_sorted.push_back({key, value.offset, value.cap_id});
  std::sort(delta_sorted.begin(), delta_sorted.end(), [](const entry& a, const entry& b) { return a.key < b.key; });

  std::vector<entry> merged;
  merged.reserve(entries_.size() + delta_sorted.size());
  auto main_it = entries_.begin();
  auto delta_it = delta_sorted.begin();
  while (main_it != entries_.end() || delta_it != delta_sorted.end()) {
    if (delta_it == delta_sorted.end() || (main_it != entries_.end() && main_it->key < delta_it->key)) {
      if (main_it->cap_id != TOMBSTONE)
        merged.push_back(*main_it);
      ++main_it;
    } else {
      merged.push_back(*delta_it);
      ++delta_it;
    }
  }

  entries_.swap(merged);
  std::unordered_map<uint64_t, packed_value>().swap(delta_);
}

const capability_memory::entry* capability_memory::find_main(uint64_t key) const
{
  auto it = std::lower_bound(entries_.begin(), entries_.end(), key, [](const entry& e, uint64_t k) { return e.key < k; });
  if (it != entries_.end() && it->key == key)
    return &(*it);
  return nullptr;
}

capability_memory::entry* capability_memory::find_main(uint64_t key)
{
  return const_cast<entry*>(std::as_const(*this).find_main(key));
}

// The key's current value (a TOMBSTONE value if it is invalidated in main), or nullopt if the key is unknown.
// Before finalize() this compacts the log first, so entries_ is sorted with unique keys and no tombstones.
std::optional<capability_memory::packed_value> capability_memory::find(uint64_t key) const
{
  if (!finalized_ && log_dirty_)
    compact_log();

  if (finalized_ && !delta_.empty()) {
    if (auto it = delta_.find(key); it != delta_.end())
      return it->second;
  }

  if (const entry* e = find_main(key))
    return packed_value{e->offset, e->cap_id};
  return std::nullopt;
}

void capability_memory::finalize()
{
  if (finalized_)
    return;

  compact_log();
  entries_.shrink_to_fit();
  live_ = entries_.size();
  finalized_ = true;

  double entry_mb = static_cast<double>(entries_.size() * sizeof(entry)) / (1024.0 * 1024.0);
  double table_mb = static_cast<double>(cap_table_.size() * sizeof(cap_descriptor)) / (1024.0 * 1024.0);

  fmt::print("[CAP_MEM] finalize: {} entries, {} unique caps "
             "(entries: {:.1f} MB, caps: {:.1f} MB, total: {:.1f} MB)\n",
             entries_.size(), cap_table_.size(),
             entry_mb, table_mb, entry_mb + table_mb);
}

void capability_memory::store_capability(champsim::address addr, const capability& cap)
{
  if (!cap.tag) {
    invalidate_tag(addr);
    return;
  }

  const uint64_t key = addr_to_key(addr);
  const packed_value value = encode(key, cap);

  if (!finalized_) {
    entries_.push_back({key, value.offset, value.cap_id});
    log_dirty_ = true;
    maybe_compact();
    return;
  }

  if (entry* e = find_main(key)) {
    if (e->cap_id == TOMBSTONE)
      ++live_;
    e->offset = value.offset;
    e->cap_id = value.cap_id;
    return;
  }

  if (delta_.insert_or_assign(key, value).second)
    ++live_;
  maybe_merge();
}

std::optional<capability> capability_memory::load_capability(champsim::address addr) const
{
  const uint64_t key = addr_to_key(addr);
  if (auto value = find(key); value.has_value())
    return decode(key, value->offset, value->cap_id);
  return std::nullopt;
}

bool capability_memory::has_capability(champsim::address addr) const
{
  return load_capability(addr).has_value();
}

void capability_memory::invalidate_tag(champsim::address addr)
{
  const uint64_t key = addr_to_key(addr);
  big_offsets_.erase(key);

  if (!finalized_) {
    entries_.push_back({key, 0, TOMBSTONE});
    log_dirty_ = true;
    maybe_compact();
    return;
  }

  if (entry* e = find_main(key)) {
    if (e->cap_id != TOMBSTONE) {
      e->offset = 0;
      e->cap_id = TOMBSTONE;
      --live_;
    }
    return;
  }

  if (delta_.erase(key) > 0)
    --live_;
}

size_t capability_memory::size() const
{
  if (!finalized_) {
    if (log_dirty_)
      compact_log();
    return entries_.size();
  }
  return live_;
}

void capability_memory::clear()
{
  std::vector<entry>().swap(entries_);
  compacted_size_ = 0;
  log_dirty_ = false;
  std::unordered_map<uint64_t, packed_value>().swap(delta_);
  std::unordered_map<uint64_t, uint64_t>().swap(big_offsets_);
  std::vector<cap_descriptor>().swap(cap_table_);
  std::unordered_map<cap_descriptor, uint32_t, cap_descriptor_hash>().swap(intern_);
  live_ = 0;
  finalized_ = false;
}

} // namespace champsim
