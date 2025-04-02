#include <unordered_set>
#include <regex>
#include "../../inc/trace_instruction.h"


// Control flow instructions
const std::unordered_set<std::string> CONTROL_FLOW_INST = {
    "j", "jal", "beq", "bne", "blt", "bge", "bltu", "bgeu", "jalr",
    "beqz", "bnez", "cjal", "cjalr", "jalr.cap"
};

// Regex patterns 
const std::regex instr_pattern(R"(\[\d+:\d+\]\s+(0x[0-9a-fA-F]+):\s+([0-9a-fA-F]+)\s+([\w\.]+)\s*(.*))");

const std::regex cap_mem_pattern(
    R"(Cap Memory (Read|Write) \[([0-9a-fA-Fx]+)\] = v:(\d+) PESBT:([0-9a-fA-F]+) Cursor:([0-9a-fA-F]+))"
);

const std::regex cap_reg_write_pattern(
    R"(Write (c\d+)\/\w+\|v:(\d+) s:(\d+) p:([0-9a-fA-F]+) f:(\d+) b:([0-9a-fA-F]+) l:([0-9a-fA-F]+).*\|o:([0-9a-fA-F]+) t:([0-9a-fA-F]+))"
);

const std::regex reg_write_pattern(R"(Write (x\d+)/\w+ = ([0-9a-fA-F]+))");

const std::regex cap_tag_pattern(
    R"(Cap Tag (Read|Write) \[([0-9a-fA-Fx]+)\/[0-9a-fA-Fx]+\] (\d+) -> (\d+))"
);



int main()

{


    
}