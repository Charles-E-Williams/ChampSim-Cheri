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
#include <iostream>

namespace champsim {

// Define the global capability memory vector
std::vector<capability_memory> global_capability_memory;

void initialize_capability_memory(size_t num_cpus) {
  global_capability_memory.clear();
  global_capability_memory.resize(num_cpus);
}

void capability_memory::store_capability(champsim::address addr, const capability& cap) {
  if (cap.tag) {
    // Align to 16-byte boundary (capability size)
    // Capabilities are 16 bytes, so we need 16-byte granularity
    constexpr uint64_t CAP_ALIGN_BITS = 4; // log2(16) = 4
    auto cap_aligned_addr = addr.to<uint64_t>() >> CAP_ALIGN_BITS;
    cap_map[cap_aligned_addr] = cap;
  } else {
    clear_capability(addr);
  }
}

std::optional<capability> capability_memory::load_capability(champsim::address addr) const {
  // Align to 16-byte boundary
  constexpr uint64_t CAP_ALIGN_BITS = 4;
  auto cap_aligned_addr = addr.to<uint64_t>() >> CAP_ALIGN_BITS;
  auto it = cap_map.find(cap_aligned_addr);
  if (it != cap_map.end() && it->second.tag) {
    return it->second;
  }
  return std::nullopt;
}

bool capability_memory::has_capability(champsim::address addr) const {
  constexpr uint64_t CAP_ALIGN_BITS = 4;
  auto cap_aligned_addr = addr.to<uint64_t>() >> CAP_ALIGN_BITS;
  auto it = cap_map.find(cap_aligned_addr);
  return (it != cap_map.end() && it->second.tag);
}

void capability_memory::clear_capability(champsim::address addr) {
  constexpr uint64_t CAP_ALIGN_BITS = 4;
  auto cap_aligned_addr = addr.to<uint64_t>() >> CAP_ALIGN_BITS;
  cap_map.erase(cap_aligned_addr);
}


} // namespace champsim