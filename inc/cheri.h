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

#ifndef CHERI_H
#define CHERI_H

#include "champsim.h"

namespace champsim
{

// Bit positions and sizes
#define METADATA_SEALED_BIT     0
#define METADATA_PERMS_SHIFT    1
#define METADATA_PERMS_BITS     19
#define METADATA_IS_CAP_BIT     20    
#define METADATA_OTYPE_SHIFT    32
#define METADATA_OTYPE_BITS     31
#define METADATA_TAG_BIT        63

// Masks
#define METADATA_SEALED_MASK    (1ULL << METADATA_SEALED_BIT)
#define METADATA_PERMS_MASK     ((1ULL << METADATA_PERMS_BITS) - 1)
#define METADATA_IS_CAP_MASK    (1ULL << METADATA_IS_CAP_BIT)
#define METADATA_OTYPE_MASK     ((1ULL << METADATA_OTYPE_BITS) - 1)
#define METADATA_TAG_MASK       (1ULL << METADATA_TAG_BIT)

struct capability {
    bool is_cap = false;
    bool tagged = false;
    uint32_t perms = 0;
    champsim::address base{};
    champsim::address length{};
    champsim::address offset{};

    bool is_valid() const {return tagged != 0;}

    // Decoding functions
    static inline unsigned char get_tag(unsigned long long metadata) {
        return (metadata & METADATA_TAG_MASK) ? 1 : 0;
    }

    static inline uint64_t get_otype(unsigned long long metadata) {
        uint64_t otype = (metadata >> METADATA_OTYPE_SHIFT) & METADATA_OTYPE_MASK;
        // Sign extend from 31 bits if needed
        if (otype & (1 << 30)) {
            otype |= ~METADATA_OTYPE_MASK;
        }
        return otype;
    }

    static inline uint32_t get_perms(unsigned long long metadata) {
        return (metadata >> METADATA_PERMS_SHIFT) & METADATA_PERMS_MASK;
    }

    static inline bool get_is_sealed(unsigned long long metadata) {
        return (metadata & METADATA_SEALED_MASK) ? 1 : 0;
    }

    static inline bool get_is_cap(unsigned long long metadata) {
        return (metadata & METADATA_IS_CAP_MASK) ? 1 : 0;
    }
};
} // namespace champsim
#endif