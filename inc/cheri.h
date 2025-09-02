#ifndef CHERI_H
#define CHERI_H
#include "champsim.h"

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
