#include <fstream>
#include <iostream>
#include <boost/regex.hpp>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <array>
#include <cstdint>
#include <cstring>
#include <cassert>
#include <re2/re2.h>
#include "/home/charles-williams/Documents/CHERI/CheriTrace/cheri-compressed-cap/cheri_compressed_cap.h"

#define CHERI
#include "../../inc/trace_instruction.h"

using cap_data = cap_metadata;
using trace_instr_format = input_instr;

namespace 
{
  constexpr char REG_AX = 56;  
} 

// Regex patterns 
const boost::regex instr_pattern(R"(\[\d+:\d+\]\s+(0x[0-9a-fA-F]+):\s+([0-9a-fA-F]+)\s+([\w\.]+)\s*(.*))");
const boost::regex cap_mem_pattern(
    R"(Cap Memory (Read|Write) \[([0-9a-fA-Fx]+)\] = v:(\d+) PESBT:([0-9a-fA-F]+) Cursor:([0-9a-fA-F]+))"
);
const boost::regex cap_reg_write_pattern(
    R"(Write (c\d+)\/(\w+)\|v:(\d+) s:(\d+) p:([0-9a-fA-F]+) f:(\d+) b:([0-9a-fA-F]+) l:([0-9a-fA-F]+).*\|o:([0-9a-fA-F]+) t:([0-9a-fA-F]+))"
);
const boost::regex reg_write_pattern(R"(Write (x\d+)/\w+ = ([0-9a-fA-F]+))");

const boost::regex tag_wr_pattern("Cap Tag Write \\[([0-9a-f]+)/([0-9a-f]+)\\] (\\d+) -> (\\d+)");

// Define regex pattern for RISC-V load instructions
const boost::regex load_pattern(
    "^(?:"
    // Standard RISC-V load instructions
    "l[bhwd](?:[uw])?|lc|"
    // CHERI load instructions
    "cl[bhwd](?:[uw])?|clc|"
    // Floating-point loads
    "fl[hwdq]|cfl[hwdq]|"
    // Load-reserve with optional memory ordering suffixes
    "lr\\.[bhwdc](?:\\.(acq|rel|aqrl))?|"
    "clr\\.[bhwdc](?:\\.(acq|rel|aqrl))?"
    ")$"
);

// Define regex pattern for RISC-V store instructions
const boost::regex store_pattern(
    "^(?:"
    // Standard RISC-V store instructions
    "s[bhwd]|sc|"
    // CHERI store instructions
    "cs[bhwd]|csc|"
    // Floating-point stores
    "fs[hwdq]|cfs[hwdq]|"
    // Store-conditional with optional memory ordering suffixes
    "sc\\.[bhwdc](?:\\.(rel|aqrl))?|"
    "csc\\.[bhwdc](?:\\.(rel|aqrl))?"
    ")$"
);

const boost::regex registerPattern(
    // Match instruction label/name if present (optional)
    "(?:\\w+\\s+)?"
    // Match destination register
    "([a-z][0-9a-z]+)\\s*,\\s*"
    // Match different source patterns:
    "(?:"
        // Format 1: src1, src2, src3 (for FP instructions with 3 sources)
        "([a-z][0-9a-z]+)\\s*,\\s*([a-z][0-9a-z]+)\\s*,\\s*([a-z][0-9a-z]+)|"
        // Format 2: src1, src2 or src1, immediate - with negative lookahead to prevent matching jalr format
        "([a-z][0-9a-z]+)\\s*,\\s*(-?\\d+|[a-z][0-9a-z]+)(?!\\s*,)|"
        // Format 3: immediate(src) - supports store/load instructions like sw rs2, offset(rs1)
        "(-?\\d+)\\s*\\(\\s*([a-z][0-9a-z]+)\\s*\\)|"
        // Format 4: single src register (move instr)
        "([a-z][0-9a-z]+)|"
        // Format 5: src1, src2, immediate (for jalr-like instructions and alternative store format)
        "([a-z][0-9a-z]+)\\s*,\\s*([a-z][0-9a-z]+)\\s*,\\s*(-?\\d+)|"
        // Format 6: single registerm immediate
        "(-?\\d+)"
    ")"
);



// Branch type classification enum
enum RiscvBranchType {
    BRANCH_DIRECT_JUMP = 0,  // Unconditional direct jumps (j)
    BRANCH_INDIRECT,         // Unconditional indirect jumps (jalr without link)
    BRANCH_CONDITIONAL,      // Conditional branches (beq, bne, etc.)
    BRANCH_DIRECT_CALL,      // Direct calls (jal)
    BRANCH_INDIRECT_CALL,    // Indirect calls (jalr with link)
    BRANCH_RETURN,           // Return instructions (ret/jalr used as return)
    BRANCH_OTHER,            // Other types of branches
    NOT_BRANCH,              // Not a branch instruction
    ERROR
  };

typedef enum {
    aluInstClass = 0,
    loadInstClass = 1,
    storeInstClass = 2,
    Branch = 3,
    fpInstClass = 4,
    undefInstClass = 5
  } InstClass;

const std::unordered_map<std::string, RiscvBranchType> CONTROL_FLOW_INST = {
    // Conditional branches (B-type)
    {"beq", BRANCH_CONDITIONAL}, {"bne", BRANCH_CONDITIONAL}, {"blt", BRANCH_CONDITIONAL}, 
    {"bge", BRANCH_CONDITIONAL}, {"bltu", BRANCH_CONDITIONAL}, {"bgeu", BRANCH_CONDITIONAL},
    {"beqz", BRANCH_CONDITIONAL}, {"bnez", BRANCH_CONDITIONAL}, {"bgtu", BRANCH_CONDITIONAL},      
    {"ble", BRANCH_CONDITIONAL},  {"bleu", BRANCH_CONDITIONAL}, {"bgez", BRANCH_CONDITIONAL},      
    {"blez", BRANCH_CONDITIONAL},  {"bgtz", BRANCH_CONDITIONAL},  
    {"bltz", BRANCH_CONDITIONAL},  {"bgt", BRANCH_CONDITIONAL},       
    
    // Direct jumps (J-type without link)
    {"j", BRANCH_DIRECT_JUMP},         // Pseudo-instruction (alias for jal zero)
    
    // Direct calls (J-type with link)
    {"jal", BRANCH_DIRECT_CALL},       // When used with ra or other registers
    {"cjal", BRANCH_DIRECT_CALL},      // CHERI direct call
    
    // Indirect jumps and calls (I-type/JALR)
    // {"jalr", BRANCH_INDIRECT_CALL},    // Usually a call, but context-dependent
    {"jr", BRANCH_INDIRECT},           // Pseudo-instruction (jalr x0, rs, 0)
    {"cjalr", BRANCH_INDIRECT_CALL},   // CHERI indirect call
    {"jalr.cap", BRANCH_INDIRECT_CALL}, // CHERI capability call
    {"jalr", BRANCH_OTHER}, // CHERI capability call
  
    // Return instructions
    {"ret", BRANCH_RETURN}             // Pseudo-instruction (jalr x0, ra, 0)
};

const std::unordered_map<std::string, int> reg_map = {
    // Integer registers
    {"zero",0 },
    {"ra", 1}, {"sp", 2}, {"gp", 3}, {"tp", 4},
    {"t0", 5}, {"t1", 6}, {"t2", 7},
    {"s0", 8}, {"fp", 8}, {"s1", 9},
    {"a0", 10}, {"a1", 11}, {"a2", 12}, {"a3", 13},
    {"a4", 14}, {"a5", 15}, {"a6", 16}, {"a7", 17},
    {"s2", 18}, {"s3", 19}, {"s4", 20}, {"s5", 21},
    {"s6", 22}, {"s7", 23}, {"s8", 24}, {"s9", 25},
    {"s10", 26}, {"s11", 27}, {"t3", 28}, {"t4", 29}, {"t5", 30}, {"t6", 31},

    // Explicit integer registers
    {"x0", 0}, {"x1", 1}, {"x2", 2}, {"x3", 3}, {"x4", 4},
    {"x5", 5}, {"x6", 6}, {"x7", 7}, {"x8", 8}, {"x9", 9},
    {"x10", 10}, {"x11", 11}, {"x12", 12}, {"x13", 13}, {"x14", 14},
    {"x15", 15}, {"x16", 16}, {"x17", 17}, {"x18", 18}, {"x19", 19},
    {"x20", 20}, {"x21", 21}, {"x22", 22}, {"x23", 23}, {"x24", 24},
    {"x25", 25}, {"x26", 26}, {"x27", 27}, {"x28", 28}, {"x29", 29}, {"x30", 30}, {"x31", 31},

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
    {"cs10", 26}, {"cs11", 27}, {"ct3", 28}, {"ct4", 29}, {"ct5", 30}, {"ct6", 31},

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
    {"ft8", 28}, {"ft9", 29}, {"ft10", 30}, {"ft11", 31}
};


uint8_t remap_regid(uint8_t reg) {

    switch (reg) {
        case 2: return 64;
        case 25: return 65;
        case 26: return 66;
        default: return reg;
    }
    return 0;
}

uint64_t get_addr(uint64_t base, uint64_t offset) {return (base+offset); }

struct ProgramTrace
{

/*
  CAP REGISTERS
  v: tag
  s: sealed with type
  p: combined perms
  f: flags
  b: base
  l: length
  o: offset
  t: object type
*/
    
    //current instruction context
    trace_instr_format curr_instr;
    std::array<uint64_t, 32> gpr = {0};
    std::array<cap_data, 32> cap_regs;
    std::array<bool, 32> has_cap = {false};
    bool pending_instr = false;
    bool pending_branch = false;
    bool is_ld_store = false;
    uint64_t next_pc = 0;
    std::string mnemonic;
    RiscvBranchType branchType = NOT_BRANCH;
    InstClass inst;


    double bytes_written = 0.0;
    bool verbose;    

    void instr_trace_init()
    {

        pending_instr = false;
        pending_branch = false;
        is_ld_store = false;
        next_pc = 0;
        branchType  = NOT_BRANCH;
        
        std::memset(curr_instr.destination_registers, 0, sizeof(curr_instr.destination_registers));
        std::memset(curr_instr.source_registers, 0, sizeof(curr_instr.source_registers));    
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
         << "\nBranch Type: " << RiscvBranchType_string()
         << "\nBranch Taken: " << (int)curr_instr.branch_taken
         << "\nIs Capability Instruction: " << (int)curr_instr.is_cap
         << "\n\nDestination Registers: ";
        

        // // Print destination registers
        for (const auto& regs : curr_instr.destination_registers) {
            if (regs.reg_id) {
                std::cout << "\n Register Number " << std::dec << static_cast<int>(regs.reg_id)
                << " | Base 0x" << std::hex << regs.cap.base
                << " | Length: 0x" << std::hex << regs.cap.length
                << " | Offset: 0x" << std::hex << regs.cap.offset
                << " | Perms: 0x" << std::hex << regs.cap.perms
                << " | Tag:" << (int)regs.cap.tag;
            }
        }
        
        std::cout << "\nSource Registers: ";
        // Print source registers
        for (const auto& regs : curr_instr.source_registers) {
            if (regs.reg_id) {
                std::cout << "\n Register Number " << std::dec << static_cast<int>(regs.reg_id) 
                << " | Base 0x" << std::hex << regs.cap.base
                << " | Length: 0x" << std::hex << regs.cap.length
                << " | Offset: 0x" <<  std::hex << regs.cap.offset
                << " | Perms: 0x" << std::hex << regs.cap.perms
                << " | Tag:" << (int)regs.cap.tag;
            }
        }
        
        std::cout << "\n\nMemory Operands:";
        // Print destination memory
        std::cout << "\nDestination Memory:";
        for (const auto& mem : curr_instr.destination_memory) {
            if (mem.address) {
                std::cout << "\n Memory Address: 0x" << std::hex << mem.address
                << " | Base 0x" << std::hex << mem.cap.base
                << " | Length: 0x" << std::hex << mem.cap.length
                << " | Offset: 0x" <<  std::hex << mem.cap.offset
                << " | Perms: 0x" << std::hex << mem.cap.perms
                << " | Tag:" << (int)mem.cap.tag;
            }
        }
        
        // Print source memory
        std::cout << "\nSource Memory:";
        for (const auto& mem : curr_instr.source_memory) {
            if (mem.address) {
                std::cout << "\n Memory Address: 0x" << std::hex << mem.address
                << " | Base 0x" << std::hex << mem.cap.base
                << " | Length: 0x" << std::hex << mem.cap.length
                << " | Offset: 0x" <<  std::hex << mem.cap.offset
                << " | Perms: 0x" << std::hex << mem.cap.perms
                << " | Tag:" << (int)mem.cap.tag;
            }
        }

        
        std::cout << "\n=========================\n" << std::dec << std::endl;
    }

    const char* RiscvBranchType_string() const {
            switch(branchType) {
                case BRANCH_DIRECT_JUMP : return "Branch Direct Jump";
                case BRANCH_INDIRECT : return "Branch Indirect";
                case BRANCH_CONDITIONAL : return "Branch Conditional";
                case BRANCH_DIRECT_CALL : return "Branch Direct Call";
                case BRANCH_INDIRECT_CALL : return "Branch Indirect Call";
                case BRANCH_RETURN : return "Branch Return";
                case BRANCH_OTHER : return "Branch Other";
                case NOT_BRANCH : return "Not a Branch";
                case ERROR : assert(false); //shouldn't end up here...
            }
            return "";
    }

    void pre_process_trace(std::ifstream& trace_file) {

        std::string line;
        while (std::getline(trace_file, line)) {
            boost::smatch m;

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
              
            if (boost::regex_search(line,m,cap_reg_write_pattern)) {
                const std::string reg_name = m[1].str();
                const uint8_t reg_id  = reg_map.at(reg_name);
            
                std::string v,p,b,l,o;
            

                v = m[3].str(); p = m[5].str(); 
                b = m[7].str(); l = m[8].str();
                o = m[9].str(); 
            
                cap_data cap;
                cap.tag = std::stoi(v);
                cap.perms = std::stoul(p,nullptr, 0x10);
                cap.base = std::stoull(b,nullptr, 0x10);
                cap.length = std::stoull(l,nullptr, 0x10);
                cap.offset = std::stoul(o,nullptr, 0x10);
                cap_regs[reg_id] = cap;
                has_cap[reg_id] = true;     
            }


            else if (boost::regex_search(line,m,reg_write_pattern)) {
                std::string reg_name = m[1].str();
                const uint8_t reg_id  = reg_map.at(reg_name);
                const std::string reg_value = m[2].str();
                gpr[reg_id] = std::stoull(reg_value,nullptr,0x10);

            }


            else if (line.find("restore_state_to_opc") != std::string::npos)
                break;
        }

        for (std::size_t i = 0; i < gpr.size(); i++)
            printf("GPR[%lu]: %lu\n", i, gpr[i]);

        int i = 0;
        for (const auto&  cap : cap_regs) {
            if(has_cap[i])
                std::cout << "\n Register Number " << std::dec << i
                << " | Base 0x" << std::hex << cap.base
                << " | Length: 0x" << std::hex << cap.length
                << " | Offset: 0x" <<  std::hex << cap.offset
                << " | Perms: 0x" << std::hex << cap.perms
                << " | Tag:" << std::dec << (int)cap.tag;
            i++;
        }
        trace_file.clear();
        exit(1);
    }

};


void set_reg_cap(ProgramTrace& trace, std::size_t src_idx, uint8_t reg_id, const std::string& reg_name) {

    const bool is_cap_reg = !reg_name.empty() && (reg_name[0] == 'c'); 
    if (is_cap_reg) {
        if (trace.has_cap[reg_id]) 
            trace.curr_instr.source_registers[src_idx].cap = trace.cap_regs[reg_id];

        trace.curr_instr.is_cap = true;
    }
}


void write_instr(ProgramTrace& trace, std::ofstream& out) {

    if (trace.inst == loadInstClass)
        assert(trace.curr_instr.source_memory->address);


    if (trace.branchType == BRANCH_CONDITIONAL) {
        trace.curr_instr.branch_taken = (trace.curr_instr.ip + 4) != trace.next_pc ;
        trace.pending_branch = false;
    }

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

    if (trace.verbose) trace.debug_print_instruction();

    double bytes_written = static_cast<double>(sizeof(trace_instr_format));
    out.write(reinterpret_cast<const char*>(&trace.curr_instr), bytes_written);
    trace.bytes_written += bytes_written; 

    if (trace.verbose) 
        std::cout << "Wrote " << bytes_written << " bytes. Total bytes: " << trace.bytes_written << " bytes\n";
    
    trace.instr_trace_init();
}



cap_data decode_cap(uint64_t& pesbt, uint64_t& cursor, bool tag)
{
    cc128r_cap_t result;
    cap_data c;
    cc128r_decompress_mem(pesbt, cursor, tag, &result);

    c.base = result.base();
    c.length = result.length();
    c.offset = result.offset();
    c.perms = result.all_permissions();
    c.tag = (unsigned char)result.cr_tag;

    return c;
}

void process_mem_access(ProgramTrace& trace, const boost::smatch& match, bool is_store)
{
    const uint64_t addr = std::stoull(match[2].str(), nullptr, 0x10);
    const bool tag = std::stoi(match[3]) != 0;
    uint64_t pesbt = std::stoull(match[4].str(), nullptr, 0x10);
    uint64_t cursor = std::stoull(match[5].str(), nullptr, 0x10);

    auto*ops = is_store ? &trace.curr_instr.destination_memory[0] : &trace.curr_instr.source_memory[0];
    const std::size_t size = is_store ? NUM_INSTR_DESTINATIONS : NUM_INSTR_SOURCES;


    for (std::size_t i = 0; i < size; i++) {
        if (ops[i].address == 0) {
            ops[i].address = (unsigned long long)addr;
            ops[i].cap = decode_cap(pesbt, cursor, tag);
            break;
        }
    }
}

void process_reg_write(ProgramTrace& trace, const boost::smatch& match)
{

    const std::string reg_name = match[1].str();

    try {
        const uint8_t reg_id  = reg_map.at(reg_name);
        const std::string reg_value = match[2].str();
        trace.gpr[reg_id] = std::stoull(reg_value,nullptr,0x10);

        if (trace.verbose) 
            std::cout << "[DEBUG] Processing destination register: " << reg_name << " (ID " << static_cast<int>(reg_id) << ")\n";
        

        for (auto& dest : trace.curr_instr.destination_registers) {
            if (dest.reg_id == 0) {
                dest.reg_id = (unsigned char)reg_id;
                return;
            }
        }

        if (trace.verbose) {
            std::cerr << "Warning: All destination register slots full when trying to add " << static_cast<int>(reg_id) << std::endl;
            std::cerr << "Current destination register: ";

            for (const auto& r : trace.curr_instr.destination_registers) {
                std::cerr << static_cast<int>(r.reg_id) << " ";
            }
            std::cerr << std::endl;
        }

    } catch (const  std::out_of_range&) {
        std::cerr << "Unknown register " << reg_name << std::endl;
    }
  
}

void process_cap_reg_write(ProgramTrace& trace, const boost::smatch& match)
{

    const std::string reg_name = match[1].str();
    trace.curr_instr.is_cap =  1;
    const uint8_t reg_id  = reg_map.at(reg_name);

    std::string v,p,b,l,o;
    // std::string f,t;
    v = match[2].str(); 
    p = match[4].str(); 
    b = match[6].str(); l = match[7].str();
    o = match[8].str(); 
    // f = match[5].str();
    // t = match[9].str();


    cap_data cap;
    cap.tag = std::stoi(v);
    cap.perms = std::stoul(p,nullptr, 0x10);
    cap.base = std::stoull(b,nullptr, 0x10);
    cap.length = std::stoull(l,nullptr, 0x10);
    cap.offset = std::stoul(o,nullptr, 0x10);


    trace.cap_regs[reg_id] = cap;
    trace.has_cap[reg_id] = true;
    for (auto& dest : trace.curr_instr.destination_registers) {
        if (dest.reg_id == 0 ) {
            dest.reg_id = (unsigned char)reg_id;
            dest.cap = cap;
            return;
        }
    }

    if (trace.verbose)
        std::cerr << "Warning: All destination register slots full when trying to add" << static_cast<int>(reg_id) << std::endl;
}



void parse_trace(ProgramTrace& trace, const std::string& operands)
{

    boost::smatch match;
    if (boost::regex_search(operands, match, registerPattern)) {

        if (trace.curr_instr.is_branch) {

            switch (trace.branchType) {
            //writes IP only
            case BRANCH_DIRECT_JUMP: 
                trace.curr_instr.destination_registers[0].reg_id = champsim::REG_INSTRUCTION_POINTER;
                trace.curr_instr.branch_taken = true;
                break;
        
            //read something else, write ip
            case BRANCH_INDIRECT: 
                trace.curr_instr.branch_taken = true;
                trace.curr_instr.destination_registers[0].reg_id = champsim::REG_INSTRUCTION_POINTER;
                trace.curr_instr.source_registers[0].reg_id = ::REG_AX;
                break;

            // reads other, writes and reads ip
            case BRANCH_CONDITIONAL: 
                trace.curr_instr.destination_registers[0].reg_id = champsim::REG_INSTRUCTION_POINTER;
                trace.curr_instr.source_registers[0].reg_id = champsim::REG_INSTRUCTION_POINTER;
                trace.curr_instr.source_registers[1].reg_id = ::REG_AX;
                break;
        
            //reads ip, sp, writes sp,ip
            case BRANCH_DIRECT_CALL: 
                trace.curr_instr.branch_taken = true;
                trace.curr_instr.destination_registers[0].reg_id = champsim::REG_INSTRUCTION_POINTER;
                trace.curr_instr.destination_registers[1].reg_id = champsim::REG_STACK_POINTER;
                trace.curr_instr.source_registers[0].reg_id = champsim::REG_INSTRUCTION_POINTER;
                trace.curr_instr.source_registers[1].reg_id = champsim::REG_STACK_POINTER;
                break;

            // reads other, ip, sp, writes sp and ip
            case BRANCH_INDIRECT_CALL: 
                trace.curr_instr.branch_taken = true;
                trace.curr_instr.destination_registers[0].reg_id = champsim::REG_INSTRUCTION_POINTER;
                trace.curr_instr.destination_registers[1].reg_id = champsim::REG_STACK_POINTER;
                trace.curr_instr.source_registers[0].reg_id = champsim::REG_INSTRUCTION_POINTER;
                trace.curr_instr.source_registers[1].reg_id = champsim::REG_STACK_POINTER;
                trace.curr_instr.source_registers[2].reg_id = ::REG_AX;
                break;

            // reads sp, writes sp,ip
            case BRANCH_RETURN: 
                trace.curr_instr.branch_taken = true;
                trace.curr_instr.source_registers[0].reg_id = champsim::REG_STACK_POINTER;
                trace.curr_instr.destination_registers[0].reg_id = champsim::REG_INSTRUCTION_POINTER;
                trace.curr_instr.destination_registers[1].reg_id = champsim::REG_STACK_POINTER;
                break;

            case BRANCH_OTHER: //JALR is context dependent
                trace.curr_instr.branch_taken = true;

                if(reg_map.at(match[1].str()) == 0) {  
                    if (reg_map.at(match[5].str()) != 1) { //indirect jmp 
                        trace.curr_instr.destination_registers[0].reg_id = champsim::REG_INSTRUCTION_POINTER;
                        trace.curr_instr.source_registers[0].reg_id = ::REG_AX;
                        trace.branchType = BRANCH_INDIRECT;
                        break;
                    }

                    else { //return
                        trace.curr_instr.source_registers[0].reg_id = champsim::REG_STACK_POINTER;
                        trace.curr_instr.destination_registers[0].reg_id = champsim::REG_INSTRUCTION_POINTER;
                        trace.curr_instr.destination_registers[1].reg_id = champsim::REG_STACK_POINTER;
                        trace.branchType = BRANCH_RETURN;
                        break;
                    }
                } else  { //indirect call       
                    trace.curr_instr.destination_registers[0].reg_id = champsim::REG_INSTRUCTION_POINTER;
                    trace.curr_instr.destination_registers[1].reg_id = champsim::REG_STACK_POINTER;
                    trace.curr_instr.source_registers[0].reg_id = champsim::REG_INSTRUCTION_POINTER;
                    trace.curr_instr.source_registers[1].reg_id = champsim::REG_STACK_POINTER;
                    trace.curr_instr.source_registers[2].reg_id = ::REG_AX;
                    trace.branchType = BRANCH_INDIRECT_CALL;
                    break;
                }
                break;

            default: assert(false);
            }
        }

        else { 

            boost::smatch m;
            if (boost::regex_search(trace.mnemonic,m,store_pattern)) {
                //extract src registers for store instruction
                trace.inst = storeInstClass;
                std::string src1, src2;
                src1 = match[1].str();
                assert(!src1.empty());

                trace.curr_instr.source_registers[0].reg_id = reg_map.at(src1);
                set_reg_cap(trace,0,reg_map.at(src1),src1);

                if (!match[8].str().empty()) {
                    src2 = match[8].str();

                    set_reg_cap(trace,1,reg_map.at(src2),src2);  
                }

                else if (!match[9].str().empty()) {
                    src2 = match[9].str();
                    set_reg_cap(trace,1,reg_map.at(src2),src2);
                }

                assert(!src2.empty());
                trace.curr_instr.source_registers[1].reg_id = reg_map.at(src2);

            }


            else if (boost::regex_search(trace.mnemonic,m,load_pattern))  {   
               
               trace.inst = loadInstClass;
            }
 
            else {

                if(match[2].matched && match[3].matched && match[4].matched) {
                    trace.inst = fpInstClass;
                    std::string src1, src2, src3;
                    src1 = match[2].str(); src2 = match[3].str(); src3 = match[4].str();

                    trace.curr_instr.source_registers[0].reg_id = reg_map.at(src1);             
                    trace.curr_instr.source_registers[1].reg_id = reg_map.at(src2);
                    trace.curr_instr.source_registers[2].reg_id = reg_map.at(src3);

                    set_reg_cap(trace,0,reg_map.at(src1),src1);
                    set_reg_cap(trace,1,reg_map.at(src2),src2);
                    set_reg_cap(trace,2,reg_map.at(src3),src3);

                }

                else if (match[5].matched) {
                    trace.inst = aluInstClass;
                    std::string src1 = match[5].str();
                    trace.curr_instr.source_registers[0].reg_id = reg_map.at(src1);
                    set_reg_cap(trace,0,reg_map.at(src1),src1);


                    std::string src2 = match[6].str();
                    if (!src2.empty() && (std::isalpha(static_cast<unsigned char>(src2[0])))) {
                        trace.curr_instr.source_registers[1].reg_id = reg_map.at(src2);
                        set_reg_cap(trace,1,reg_map.at(src2),src2);

                    }
                }

                else if (match[8].matched) {
                    trace.inst = aluInstClass;
                    trace.curr_instr.source_registers[0].reg_id = reg_map.at(match[8].str());
                    set_reg_cap(trace,0,reg_map.at(match[8].str()),match[8].str());
                }  

                else if (match[9].matched) {
                    trace.inst = aluInstClass;
                    trace.curr_instr.source_registers[0].reg_id = reg_map.at(match[9].str());
                    set_reg_cap(trace,0,reg_map.at(match[9].str()),match[9].str());
                }  
            }
        }
    }

    else { 
        if (trace.verbose) {
            std::cerr << "No register operands found: " << operands << std::endl;
        }
    }
}

void process_source_mem(ProgramTrace& trace, const std::string& addr)
{
    if (trace.inst != storeInstClass) 
        std::cerr << "Hmmmm. Strange\n";
    
    else 
        trace.curr_instr.destination_memory[0].address = std::stoull(addr, NULL, 0x10);
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

    trace.instr_trace_init(); trace.pre_process_trace(trace_file);

    while (std::getline(trace_file, line)) {
        boost::smatch match;


        if (trace.pending_branch) {
            if (boost::regex_match(line, match, instr_pattern)) {
                trace.next_pc = std::stoull(match[1].str(), nullptr, 0x10);
                write_instr(trace,trace_outFile);
                trace.pending_branch = false;
            }
            else continue;
        }
        
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

        if (boost::regex_match(line, match, instr_pattern)) {
 
            if (match[3].str() == "fence" || match[3].str() == "cspecialrw")  //maintain a list of "dont care instructions"?
                continue;


            if (trace.pending_instr) 
                write_instr(trace, trace_outFile);

            trace.curr_instr.ip = std::stoull(match[1].str(), nullptr, 0x10);
            trace.mnemonic = match[3].str();
            RiscvBranchType bType = (CONTROL_FLOW_INST.count(trace.mnemonic)  > 0) ? CONTROL_FLOW_INST.at(trace.mnemonic) : NOT_BRANCH;            
            trace.curr_instr.is_branch = bType != NOT_BRANCH;
            trace.branchType = bType;


            if (trace.curr_instr.is_branch) {
                trace.pending_branch = true;
                trace.inst = Branch;
            }
            

            trace.pending_instr = true;    
            parse_trace(trace, match[4].str());

        } else if (trace.pending_instr && !trace.pending_branch) {

            if (boost::regex_search(line,match, cap_reg_write_pattern)) {
                process_cap_reg_write(trace,match);

            }

            else if (boost::regex_search(line,match, reg_write_pattern)) {
                process_reg_write(trace,match);
            }


            else if (boost::regex_search(line,match, cap_mem_pattern)) {
                process_mem_access(trace, match, match[1].str() == "Write");
            }

            else if (boost::regex_search(line,match,tag_wr_pattern)) {
                process_source_mem(trace,match[1].str());
            }
        }
    }
    
    if (trace.pending_instr) write_instr(trace,trace_outFile);

    trace_file.close();
    trace_outFile.close();

    std::cout << "Size of raw trace file: " << trace.bytes_written / static_cast<double>(1 << 30) << " GB\n";

    return EXIT_SUCCESS;

}