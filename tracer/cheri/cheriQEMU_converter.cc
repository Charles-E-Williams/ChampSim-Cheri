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
using trace_instr_format = input_instr;


struct ProgramTrace
{

    std::unordered_map<int, cap_data> cap_regs;
    std::unordered_map<uint64_t, cap_data> mem_caps;
    trace_instr_format curr_instr;
    std::string partial_line;
    bool is_cap;
    int src_mem_idx = 0; int dest_mem_idx = 0;
};

void reset_instr(ProgramTrace& trace)
{
    memset(&trace.curr_instr, 0, sizeof(trace_instr_format));
    trace.is_cap = false;
    trace.partial_line = "";
    trace.src_mem_idx = trace.dest_mem_idx = 0;
}



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

const std::unordered_map<std::string, int> reg_name_to_id = {
    // Integer registers
    {"ra", 1}, {"sp", 2}, {"gp", 3}, {"tp", 4},
    {"t0", 5}, {"t1", 6}, {"t2", 7},
    {"s0", 8}, {"fp", 8}, {"s1", 9},
    {"a0", 10}, {"a1", 11}, {"a2", 12}, {"a3", 13},
    {"a4", 14}, {"a5", 15}, {"a6", 16}, {"a7", 17},
    {"s2", 18}, {"s3", 19}, {"s4", 20}, {"s5", 21},
    {"s6", 22}, {"s7", 23}, {"s8", 24}, {"s9", 25},
    {"s10", 26}, {"s11", 27}, {"t3", 28}, {"t4", 29},
    {"t5", 30}, {"t6", 31},

    // Explicit integer registers
    {"x0", 0}, {"x1", 1}, {"x2", 2}, {"x3", 3}, {"x4", 4},
    {"x5", 5}, {"x6", 6}, {"x7", 7}, {"x8", 8}, {"x9", 9},
    {"x10", 10}, {"x11", 11}, {"x12", 12}, {"x13", 13}, {"x14", 14},
    {"x15", 15}, {"x16", 16}, {"x17", 17}, {"x18", 18}, {"x19", 19},
    {"x20", 20}, {"x21", 21}, {"x22", 22}, {"x23", 23}, {"x24", 24},
    {"x25", 25}, {"x26", 26}, {"x27", 27}, {"x28", 28}, {"x29", 29},
    {"x30", 30}, {"x31", 31},

    // CHERI capability registers
    {"c0", 0},  {"c1", 1},  {"c2", 2},  {"c3", 3},
    {"c4", 4},  {"c5", 5},  {"c6", 6},  {"c7", 7},
    {"c8", 8},  {"c9", 9},  {"c10", 10}, {"c11", 11},
    {"c12", 12}, {"c13", 13}, {"c14", 14}, {"c15", 15},
    {"c16", 16}, {"c17", 17}, {"c18", 18}, {"c19", 19},
    {"c20", 20}, {"c21", 21}, {"c22", 22}, {"c23", 23},
    {"c24", 24}, {"c25", 25}, {"c26", 26}, {"c27", 27},
    {"c28", 28}, {"c29", 29}, {"c30", 30}, {"c31", 31},

    {"cnull", 0}, //null capability
    {"cra", 1},  // capability return address
    {"csp", 2},  // capability stack pointer
    {"cgp", 3},  // capability global pointer
    {"ctp", 4},  // capability thread pointer
    {"ct0", 5},  {"ct1", 6},  {"ct2", 7},
    {"cs0", 8},  {"cs1", 9},
    {"ca0", 10}, {"ca1", 11}, {"ca2", 12}, {"ca3", 13},
    {"ca4", 14}, {"ca5", 15}, {"ca6", 16}, {"ca7", 17},
    {"cs2", 18}, {"cs3", 19}, {"cs4", 20}, {"cs5", 21},
    {"cs6", 22}, {"cs7", 23}, {"cs8", 24}, {"cs9", 25},
    {"cs10", 26}, {"cs11", 27}, {"ct3", 28}, {"ct4", 29},
    {"ct5", 30}, {"ct6", 31},

    // fp registers
    {"f0", 0}, {"f1", 1}, {"f2", 2}, {"f3", 3},
    {"f4", 4}, {"f5", 5}, {"f6", 6}, {"f7", 7},
    {"f8", 8}, {"f9", 9}, {"f10", 10}, {"f11", 11},
    {"f12", 12}, {"f13", 13}, {"f14", 14}, {"f15", 15},
    {"f16", 16}, {"f17", 17}, {"f18", 18}, {"f19", 19},
    {"f20", 20}, {"f21", 21}, {"f22", 22}, {"f23", 23},
    {"f24", 24}, {"f25", 25}, {"f26", 26}, {"f27", 27},
    {"f28", 28}, {"f29", 29}, {"f30", 30}, {"f31", 31}

};


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



void parse_trace(ProgramTrace& trace, std::string& line)
{
    std::smatch match;

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



    ProgramTrace trace;
    std::string line;
    unsigned long long line_number = 0;

    try
    {
        while (std::getline(trace_file, line))
        {
            line_number++;

            if (!trace.partial_line.empty())
            {
                line = trace.partial_line + " " + line;
                trace.partial_line.clear();
            }


        }


    }
    catch (const std::exception& e) {
        std::cerr << "Error at line " << line_number << ": " << e.what() << std::endl;
        return 1;
    }
    




    return 1;





}