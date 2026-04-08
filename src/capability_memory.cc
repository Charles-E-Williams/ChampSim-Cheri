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

namespace champsim {

std::vector<capability_memory> cap_mem;

void initialize_capability_memory(size_t num_cpus)
{
  cap_mem.clear();
  cap_mem.resize(num_cpus);
}


capability capability_memory::rebuild(const presimpoint_entry& e) const
{
  const auto& cap_ = cap_table_[e.cap_id];
  return capability{
      champsim::address{e.offset},
      champsim::address{cap_.base},
      champsim::address{cap_.length},
      cap_.permissions,
      true
  };
}

const capability_memory::presimpoint_entry* capability_memory::find_in_presimpoint(uint64_t key) const
{
  auto it = std::lower_bound(
      presimpoint_entries_.begin(), presimpoint_entries_.end(), key,
      [](const presimpoint_entry& entry, uint64_t k) { return entry.va_key < k; });

  if (it != presimpoint_entries_.end() && it->va_key == key)
    return &(*it);
  return nullptr;
}

void capability_memory::finalize()
{
  if (finalized_)
    return;

  std::unordered_map<cap_descriptor, uint32_t, cap_descriptor_hash> prune;
  prune.reserve(128 * 1024);
  cap_table_.reserve(128 * 1024);

  presimpoint_entries_.reserve(presimpoint_map_.size());

  for (const auto& [key, cap] : presimpoint_map_) {
    cap_descriptor desc{cap.base.to<uint64_t>(),
                           cap.length.to<uint64_t>(),
                           cap.permissions};

    auto [it, inserted] = prune.try_emplace(desc, static_cast<uint32_t>(cap_table_.size()));
    if (inserted)
      cap_table_.push_back(desc);

    presimpoint_entries_.push_back({key, cap.offset.to<uint64_t>(), it->second});
  }

  std::unordered_map<uint64_t, capability>().swap(presimpoint_map_);
  std::unordered_map<cap_descriptor, uint32_t, cap_descriptor_hash>().swap(prune);

  std::sort(presimpoint_entries_.begin(), presimpoint_entries_.end());
  presimpoint_entries_.shrink_to_fit();
  cap_table_.shrink_to_fit();

  finalized_ = true;

  double entry_mb = static_cast<double>(presimpoint_entries_.size() * sizeof(presimpoint_entry) / (1024.0 * 1024.0));
  double table_mb = static_cast<double>(cap_table_.size() * sizeof(cap_descriptor) / (1024.0 * 1024.0));

  fmt::print("[CAP_MEM] finalize: {} entries, {} unique caps "
             "(entries: {:.1f} MB, caps: {:.1f} MB, total: {:.1f} MB)\n",
             presimpoint_entries_.size(), cap_table_.size(),
             entry_mb, table_mb, entry_mb + table_mb);
}


void capability_memory::store_capability(champsim::address addr, const capability& cap)
{
  uint64_t key = addr_to_key(addr);

  if (!finalized_) {
    if (cap.tag)
      presimpoint_map_[key] = cap;
    else
      presimpoint_map_.erase(key);
    return;
  }

  if (cap.tag) {
    simpoint_region_map_[key] = cap;
    invalidated_keys_.erase(key);
  } else {
    invalidate_tag(addr);
  }
}


std::optional<capability> capability_memory::load_capability(champsim::address addr) const
{
  uint64_t key = addr_to_key(addr);

  if (!finalized_) {
    auto it = presimpoint_map_.find(key);
    if (it != presimpoint_map_.end() && it->second.tag)
      return it->second;
    return std::nullopt;
  }

  // simpoint region writes take priority.
  auto rt = simpoint_region_map_.find(key);
  if (rt != simpoint_region_map_.end()) {
    if (rt->second.tag)
      return rt->second;
    return std::nullopt;
  }

  if (!invalidated_keys_.empty() && invalidated_keys_.count(key))
    return std::nullopt;

  const presimpoint_entry* entry = find_in_presimpoint(key);
  if (entry)
    return rebuild(*entry);

  return std::nullopt;
}

bool capability_memory::has_capability(champsim::address addr) const
{
  uint64_t key = addr_to_key(addr);

  if (!finalized_) {
    auto it = presimpoint_map_.find(key);
    return it != presimpoint_map_.end() && it->second.tag;
  }

  auto rt = simpoint_region_map_.find(key);
  if (rt != simpoint_region_map_.end())
    return rt->second.tag;

  if (!invalidated_keys_.empty() && invalidated_keys_.count(key))
    return false;

  return find_in_presimpoint(key) != nullptr;
}

void capability_memory::invalidate_tag(champsim::address addr)
{
  uint64_t key = addr_to_key(addr);

  if (!finalized_) {
    presimpoint_map_.erase(key);
    return;
  }

  simpoint_region_map_.erase(key);

  if (find_in_presimpoint(key))
    invalidated_keys_.insert(key);
}


size_t capability_memory::size() const
{
  if (!finalized_)
    return presimpoint_map_.size();

  return presimpoint_entries_.size() - invalidated_keys_.size() + simpoint_region_map_.size();
}

void capability_memory::clear()
{
  std::unordered_map<uint64_t, capability>().swap(presimpoint_map_);
  std::vector<presimpoint_entry>().swap(presimpoint_entries_);
  std::vector<cap_descriptor>().swap(cap_table_);
  std::unordered_map<uint64_t, capability>().swap(simpoint_region_map_);
  std::unordered_set<uint64_t>().swap(invalidated_keys_);
  finalized_ = false;
}

} // namespace champsim