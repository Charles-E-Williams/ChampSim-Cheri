#include <fstream>
#include <iostream>
#include <regex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <cstdint>
#include <cstring>
#include "../../inc/trace_instruction.h"
#include "../../../cheri-compressed-cap/cheri_compressed_cap.h"

using cap_data = capability_metadata;

void print_cap(cap_data cap)
{
    std::cout << 
        "Tag = " << static_cast<int>(cap.tag) << "\n"
        "Sealed = " << static_cast<int>(cap.sealed) << "\n"
        "Base = " << std::hex << "0x" << cap.base << "\n"
        "Length = " << std::hex << "0x" << cap.length << "\n"
        "Top = " << std::hex << "0x" << cap.get_top() << "\n"
        "Cursor = " << std::hex << "0x" << cap.get_cursor() << "\n"
        "Offset = " << std::hex << "0x" << cap.offset << "\n"
        "Permission = " << std::hex << "0x" << cap.perms << "\n";
}
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



cap_data* decode_capability(uint64_t pesbt, uint64_t cursor, bool valid, cap_data* cap )
{
    assert(cap);

    //decoding capabilities in memory for the risc-v standard format
    cc128r_cap_t result;
    cc128r_decompress_mem(pesbt, cursor, valid, &result);
    cap->tag = valid;
    cap->sealed = result.is_sealed(); 
    cap->base = result.cr_base;
    cap->length = result.length();
    cap->offset = result.offset();
    cap->perms = result.all_permissions();
    
    
    assert(cc128r_is_representable_cap_exact(&result));
    return cap;
}

int main(int argc, char* argv[])
{

    if (argc != 3) {
        std::cerr << "Usage: " << argv[0] << "<trace_file> <output_trace>\n";
        exit(EXIT_FAILURE);
    }

    std::ifstream trace_file(argv[1]);
    if (!trace_file.is_open()) {
        std::cerr << "Trace file does not exist\n";
        exit (EXIT_FAILURE);
    }

    std::string oFilename = argv[2];
    oFilename += + ".champsim.bin";



    std::string line;


    while (std::getline(trace_file, line))
    {
        std::cout << line << std::endl;
    }



    return 1;





}