#include <fstream>
#include <iostream>
#include <regex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <cstdint>
#include <cstring>
#include <cassert>
#include <algorithm>


#define CHERI
#include "../../inc/trace_instruction.h"
using cap_data = capability_metadata;
using trace_instr_format = input_instr;

struct ProgramTrace
{

    std::unordered_map<uint64_t, cap_data> mem_caps;
    
    //current instruction context
    trace_instr_format curr_instr;
    bool pending_instr = false;
    bool pending_branch = false;
    uint64_t next_pc = 0;
    uint64_t branch_pc = 0;
    bool verbose;
    std::string mnemonic;


    void instr_trace_init()
    {

        mem_caps.clear();
        pending_instr = false;
        pending_branch = false;
        next_pc = 0;
        branch_pc = 0;
        std::fill_n(curr_instr.destination_registers, NUM_INSTR_DESTINATIONS, 0);
        std::fill_n(curr_instr.source_registers, NUM_INSTR_SOURCES, 0);
        std::memset(curr_instr.destination_memory, 0, sizeof(curr_instr.destination_memory));
        std::memset(curr_instr.source_memory, 0, sizeof(curr_instr.source_memory));
        std::memset(&curr_instr, 0, sizeof(trace_instr_format));
        mnemonic.clear();
    }


    void debug_print_instruction() const {

        std::cout << "\n=== Instruction Debug ==="
         << "\nInstruction: " << mnemonic 
         << "\nPC: 0x" << std::hex << curr_instr.ip
         << "\nIs Branch: " << std::dec << (int)curr_instr.is_branch
         << "\nBranch Taken: " << (int)curr_instr.branch_taken
         << "\nIs Capability: " << (int)curr_instr.is_cap
         << "\n\nDestination Registers: ";
        
        // Print destination registers
        for (std::size_t i = 0; i < NUM_INSTR_DESTINATIONS; i++) {
            if (curr_instr.destination_registers[i]) {
                std::cout << "Reg " << (int)curr_instr.destination_registers[i] << " ";
            }
        }
        
        std::cout << "\nSource Registers: ";
        // Print source registers
        for (std::size_t i = 0; i < NUM_INSTR_SOURCES; i++) {
            if (curr_instr.source_registers[i]) {
                std::cout << "Reg " << (int)curr_instr.source_registers[i] << " ";
            }
        }
        
        std::cout << "\n\nMemory Operands:";
        // Print destination memory
        std::cout << "\nDestination Memory:";
        for (const auto& mem : curr_instr.destination_memory) {
            if (mem.address) {
                std::cout << "\n Addr: 0x" << std::hex << mem.address
                << " | PESBT: 0x" << mem.cap.pesbt
                << " | Cursor: 0x" << mem.cap.cursor
                << " | Tag: " << (int)mem.cap.tag;
            }
        }
        
        // Print source memory
        std::cout << "\nSource Memory:";
        for (const auto& mem : curr_instr.source_memory) {
            if (mem.address) {
                std::cout << "\n Addr: 0x" << std::hex << mem.address
                << " | PESBT: 0x" << mem.cap.pesbt
                << " | Cursor: 0x" << mem.cap.cursor
                << " | Tag: " << (int)mem.cap.tag;
            }
        }
        
        std::cout << "\n=========================\n" << std::dec << std::endl;
    }

};




// Control flow instructions
const std::unordered_set<std::string> CONTROL_FLOW_INST = {
    "j", "jal", "beq", "bne", "blt", "bge", "bltu", "bgeu", "jalr",
    "beqz", "bnez", "cjal", "cjalr", "jalr.cap", "jr", "ret"
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


// const std::regex cap_tag_write_pattern(
//     R"(Cap Tag (Read|Write) \[([0-9a-fA-Fx]+)\/[0-9a-fA-Fx]+\] (\d+) -> (\d+))"
// );
// const std::regex cap_tag_read_pattern(
//     R"(Cap\s+Tag\s+Read\s+\[([0-9a-fA-Fx]+)\/([0-9a-fA-Fx]+)\]\s+->\s+(\d+))"
// );

const std::unordered_map<std::string, int> reg_name_to_id = {
    // Integer registers
    {"zero",0 },
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
    {"f28", 28}, {"f29", 29}, {"f30", 30}, {"f31", 31},


    {"ft0", 0}, {"ft1", 1}, {"ft2", 2}, {"ft3", 3}, {"ft4", 4}, {"ft5", 5}, {"ft6", 6}, {"ft7", 7},
    {"fs0", 8}, {"fs1", 9},
    {"fa0", 10}, {"fa1", 11}, {"fa2", 12}, {"fa3", 13}, {"fa4", 14}, {"fa5", 15},
    {"fa6", 16}, {"fa7", 17},
    {"fs2", 18}, {"fs3", 19}, {"fs4", 20}, {"fs5", 21}, {"fs6", 22}, {"fs7", 23},
    {"fs8", 24}, {"fs9", 25}, {"fs10", 26}, {"fs11", 27},
    {"ft8", 28}, {"ft9", 29}, {"ft10", 30}, {"ft11", 31},

    {"scr1", champsim::SCR1_REG}
};


void write_instr(ProgramTrace& trace, std::ofstream& out) {

    if (trace.curr_instr.is_branch)
        trace.curr_instr.branch_taken = (trace.curr_instr.ip + 4) != trace.next_pc;

    trace.curr_instr.is_cap = 0;
    for (const auto& dest : trace.curr_instr.destination_memory) {
        if (dest.address != 0) {
            trace.curr_instr.is_cap = 1;
            break;
        }   
    }

    if (!trace.curr_instr.is_cap) {
        for (const auto& src : trace.curr_instr.source_memory) {
            if (src.address != 0) {
                trace.curr_instr.is_cap = 1;
                break;
            }
        }
    }

    if (trace.verbose)
        trace.debug_print_instruction();

    out.write(reinterpret_cast<const char*>(&trace.curr_instr), sizeof(trace_instr_format));
   // trace.instr_trace_init();
}

void process_mem_access(ProgramTrace& trace, const std::smatch& match, bool is_store)
{
    const uint64_t addr = std::stoull(match[2].str(), nullptr, 0x10);
    const bool tag = std::stoi(match[3]) != 0;
    const uint64_t pesbt = std::stoull(match[4].str(), nullptr, 0x10);
    const uint64_t cursor = std::stoull(match[5].str(), nullptr, 0x10);

    // std::cout << "PESBT = 0x" << std::hex << pesbt << " Cursor = 0x" << std::hex << cursor << std::endl;
    // std::cout << "ADDR = 0x" << std::hex << addr << std::endl;
    // std::cout << "TAG IS " << (int)tag << std::endl;

    cap_data cap = {pesbt, cursor, static_cast<unsigned char>(tag)};
    trace.mem_caps[addr] = cap;


    const auto& ops = is_store ? trace.curr_instr.destination_memory : trace.curr_instr.source_memory;
    const std::size_t size = is_store ? NUM_INSTR_DESTINATIONS : NUM_INSTR_SOURCES;

    for (std::size_t i = 0; i < size; i++) {
        auto& op = ops[i];
        if (op.address == 0) {
            op.address = (unsigned long long)addr;
            op.cap = cap;
            break;
        }

    }
    
}

void process_reg_write(ProgramTrace& trace, const std::smatch& match)
{

    const std::string reg_name = match[1].str();
    const uint8_t reg_id  = reg_name_to_id.at(reg_name);
    for (auto& dest : trace.curr_instr.destination_registers) {
        if (dest ==0) {
            dest = (unsigned char)reg_id;
            return;
        }
    }

    if (trace.verbose)
        std::cerr << "Warning: All destination register slots full when trying to add" << static_cast<int>(reg_id) << std::endl;
}


void process_cap_reg_write(ProgramTrace& trace, const std::smatch& match)
{

    const std::string reg_name = match[1].str();
    const uint8_t reg_id  = reg_name_to_id.at(reg_name);

    for (auto& dest : trace.curr_instr.destination_registers) {
        if (dest == 0 ) {
            dest = (unsigned char)reg_id;
            return;
        }
    }
    if (trace.verbose)
        std::cerr << "Warning: All destination register slots full when trying to add" << static_cast<int>(reg_id) << std::endl;

}

void parse_trace(ProgramTrace& trace, const std::string& operands)
{
    std::regex registerPattern(
        // Match instruction label/name if present (optional)
        "(?:\\w+\\s+)?"
        // Match destination register
        "([a-z][0-9a-z]+)\\s*,\\s*"
        // Match different source patterns:
        "(?:"
            // Format 1: src1, src2, src3 (for FP instructions with 3 sources)
            "([a-z][0-9a-z]+)\\s*,\\s*([a-z][0-9a-z]+)\\s*,\\s*([a-z][0-9a-z]+)|"
            // Format 2: src1, src2 or src1, immediate
            "([a-z][0-9a-z]+)\\s*,\\s*(-?\\d+|[a-z][0-9a-z]+)|"
            // Format 3: immediate(src)
            "(-?\\d+)\\s*\\(\\s*([a-z][0-9a-z]+)\\s*\\)|"
            // Format 4: single src register (move instr)
            "([a-z][0-9a-z]+)"
        ")"
    );


    std::smatch match;
    if (std::regex_search(operands, match, registerPattern))
    {
        if(match[2].matched && match[3].matched && match[4].matched) {

            std::string src1, src2, src3;
            src1 = match[2].str(); src2 = match[3].str(); src3 = match[4].str();


            // std::cout << "src1 = " << src1 << std::endl;
            // std::cout << "src2 = " << src2 << std::endl;
            // std::cout << "src3 = " << src3 << std::endl;

            trace.curr_instr.source_registers[0] = reg_name_to_id.at(src1);             
            trace.curr_instr.source_registers[1] = reg_name_to_id.at(src2);
            trace.curr_instr.source_registers[2] = reg_name_to_id.at(src3);

        }

        else if (match[5].matched) {
            std::string src1 = match[5].str();
            // std::cout << "src1 = " << src1 << std::endl;
            trace.curr_instr.source_registers[0] = reg_name_to_id.at(src1);

            std::string src2 = match[6].str();
            if (!src2.empty() && (std::isalpha(static_cast<unsigned char>(src2[0])))) {
                // std::cout << "src2 = " << src2 << std::endl;
                trace.curr_instr.source_registers[1] = reg_name_to_id.at(src2);
  
            }
        }

        else if (match[8].matched) {
            // std::cout << "src1 = " << match[8].str() << std::endl;
            trace.curr_instr.source_registers[0] = reg_name_to_id.at(match[8].str());
        }  

        else if (match[9].matched) {
            //  std::cout << "src1 = " << match[9].str() << std::endl;
            trace.curr_instr.source_registers[0] = reg_name_to_id.at(match[9].str());
        }  
    }

    // else {

    //     std::cerr << operands << std::endl;

    // }



}

int main(int argc, char* argv[])
{

    if (argc < 3) {
        std::cerr << "Usage: " << argv[0] << "<trace_file> <output_trace> [-v] \n";
        exit(EXIT_FAILURE);
    }

    std::string fileName = argv[1];
    std::ifstream trace_file(fileName);
    std::string oFilename = std::string(argv[2]) + ".champsim.bin";

    if (!trace_file.is_open()) {
        std::cerr << "Error: Trace file at path" << std::string(argv[1]) << " could not be opened.\n";
        exit (EXIT_FAILURE);
    }
    std::ofstream trace_outFile(oFilename, std::ios::binary);
    if(!trace_outFile.is_open())
    {
        std::cerr << "Error: Could not open output file " << oFilename << std::endl;
        exit(EXIT_FAILURE);
    }


    ProgramTrace trace;
    trace.verbose = (argc > 3 && std::string(argv[3]) == "-v");
    std::string line;

    trace.instr_trace_init();
    while (std::getline(trace_file, line))
    {
        std::smatch match;
        
        if(line.find("Write c") != std::string::npos) {
            std::string next_line;
            auto old_post = trace_file.tellg();
            if(getline(trace_file,next_line) && next_line.find("|o:") != std::string::npos) 
                line += " " + next_line;

            else {
                std::cerr << "ERROR: Capability Register Write metadata not found in the next line\n";
                exit(EXIT_FAILURE);
            }
        }

        if (std::regex_match(line, match, instr_pattern)) {
            if (trace.verbose)
                trace.mnemonic = match[3].str();

            if (match[3].str() == "fence")  //maintain a list of "dont care instructions"
                continue;


            if (trace.pending_branch) {
                trace.next_pc = std::stoull(match[1].str(), nullptr, 0x10);
                trace.curr_instr.branch_taken = (trace.branch_pc + 4) != trace.next_pc;
                write_instr(trace,trace_outFile);
                trace.instr_trace_init();
            }


            trace.curr_instr.ip = std::stoull(match[1].str(), nullptr, 0x10);
            trace.curr_instr.is_branch = CONTROL_FLOW_INST.count(match[3].str());

            if (trace.curr_instr.is_branch) {
                trace.pending_branch = true;
                trace.branch_pc = trace.curr_instr.ip;
            }

            parse_trace(trace, match[4].str());
            trace.pending_instr = true;    
        }

        else if (trace.pending_instr && !trace.pending_branch) {

            if (std::regex_search(line,match, cap_reg_write_pattern)) {
                process_cap_reg_write(trace,match);

            }

            else if (std::regex_search(line,match, reg_write_pattern)) {
                process_reg_write(trace,match);
            }


            else if (std::regex_search(line,match, cap_mem_pattern)){
                process_mem_access(trace, match, match[1].str() == "WRITE");
            }
        }
    }
    

    if (trace.pending_instr) write_instr(trace,trace_outFile);

    return EXIT_SUCCESS;





}