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
#include "cheri_utils.h"

#include <iostream>

namespace champsim {

std::vector<capability_memory> cap_mem;

void initialize_capability_memory(size_t num_cpus) {
  cap_mem.clear();
  cap_mem.resize(num_cpus);
}

void capability_memory::store_capability(champsim::address addr, const capability& cap) {  
  uint64_t index = addr.to<uint64_t>() >> CAP_ALIGNMENT_BITS;
  // std::cout << "store_cap addr=0x" << std::hex << addr.to<uint64_t>() << " ";
  // cheri::print_cap(cap);
  if (cap.tag) {
    cap_map[index] = cap;
  } else {
    cap_map.erase(index);
  }
}

std::optional<capability> capability_memory::load_capability(champsim::address addr) const {

  if (cap_map.empty())
    return std::nullopt;

  uint64_t index = addr.to<uint64_t>() >> CAP_ALIGNMENT_BITS;
  auto it = cap_map.find(index);
  if (it != cap_map.end() && it->second.tag) {
      return it->second;
  }
  return std::nullopt;
}

bool capability_memory::has_capability(champsim::address addr) const {
  if (cap_map.empty())
    return false;

  uint64_t index = addr.to<uint64_t>() >> CAP_ALIGNMENT_BITS;
  auto it = cap_map.find(index);
  return (it != cap_map.end() && it->second.tag);
}

void capability_memory::invalidate_tag(champsim::address addr) {
  if (cap_map.empty())
    return;

  uint64_t index = addr.to<uint64_t>() >> CAP_ALIGNMENT_BITS;
  cap_map.erase(index);
}


} // namespace champsim