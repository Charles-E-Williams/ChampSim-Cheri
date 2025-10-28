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

#include <string>
#include <iostream>
#include <unordered_map>
#include <vector>
#include "cheri.h"

namespace champsim {

class capability_memory {
private:
  std::unordered_map<uint64_t, capability> cap_map;

public:
  capability_memory() = default;

  void store_capability(champsim::address addr, const capability& cap);
  std::optional<capability> load_capability(champsim::address addr) const;
  bool has_capability(champsim::address addr) const;
  void clear_capability(champsim::address addr);
  size_t size() const { return cap_map.size(); }
  void clear() { cap_map.clear(); }
};

// Global capability memory - one per CPU
extern std::vector<capability_memory> global_capability_memory;

// Initialize global capability memory (call this before simulation)
void initialize_capability_memory(size_t num_cpus);

} // namespace champsim

#endif