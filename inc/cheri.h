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
#include "address.h"

namespace champsim 
{

    enum class cap_op_type : uint8_t {
        NONE         = 0x00,
        AUTH         = 0x01,
        TRANSFERRED  = 0x02,
        BOTH         = 0x03, 
        PRESIMPOINT  = 0x04
    };


    inline bool has_auth(cap_op_type op) {
        return (static_cast<uint8_t>(op) & static_cast<uint8_t>(cap_op_type::AUTH)) != 0;
    }
    inline bool has_transferred(cap_op_type op) {
        return (static_cast<uint8_t>(op) & static_cast<uint8_t>(cap_op_type::TRANSFERRED)) != 0;
    }
    inline bool is_presimpoint(cap_op_type op) {
        return op == cap_op_type::PRESIMPOINT;
    }
    
    struct capability {
        champsim::address offset{0};
        champsim::address base{0};
        champsim::address length{0}; 
        uint32_t permissions = 0;
        bool tag = false;
    };
}
#endif
