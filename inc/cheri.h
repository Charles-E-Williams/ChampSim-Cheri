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
    struct capability {

        champsim::address offset{};
        champsim::address base{};
        champsim::address length{}; 
        uint32_t permissions = 0;
        bool tag = false;
        bool is_cap_instr = false;
    };
}
#endif
