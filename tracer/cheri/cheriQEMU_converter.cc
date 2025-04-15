#include <fstream>
#include <iostream>
#include <boost/regex.hpp>
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
#include "/home/grads/c/charlesw2000/QEMU-to-ChampSim/cheri-compressed-cap/cheri_compressed_cap.h"


using cap_data = cap_metadata;
using trace_instr_format = input_instr;

namespace 
{
  constexpr char REG_AX = 56;  
} 


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

std::unordered_set<std::string> load_store_instructions = {
    "LBU",    // Load Byte Unsigned
    "LBS",    // Load Byte Signed
    "LHU",    // Load Halfword Unsigned
    "LHS",    // Load Halfword Signed
    "LW",     // Load Word
    "LD",     // Load Doubleword
    "FLW",    // Load Single-Precision Floating-Point
    "FLD",    // Load Double-Precision Floating-Point
    "SW",     // Store Word
    "SH",     // Store Halfword
    "SB",     // Store Byte
    "SD",     // Store Doubleword
    "FSW",    // Store Single-Precision Floating-Point
    "FSD"     // Store Double-Precision Floating-Point
};

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
//    {"jalr", BRANCH_INDIRECT_CALL},    // Usually a call, but context-dependent
    {"jr", BRANCH_INDIRECT},           // Pseudo-instruction (jalr x0, rs, 0)
    {"cjalr", BRANCH_INDIRECT_CALL},   // CHERI indirect call
    {"jalr.cap", BRANCH_INDIRECT_CALL}, // CHERI capability call
    {"jalr", BRANCH_OTHER}, // CHERI capability call
  
    // Return instructions
    {"ret", BRANCH_RETURN}             // Pseudo-instruction (jalr x0, ra, 0)
};

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
    struct cap {
        uint64_t base, length, offset;
        uint16_t perms;
        uint32_t otype;
        uint8_t flag, tag;
        bool sealed;
    };
    
    //current instruction context
    trace_instr_format curr_instr;
    bool pending_instr = false;
    bool pending_branch = false;
    uint64_t next_pc = 0;
    uint64_t branch_pc = 0;
    bool verbose;
    std::string mnemonic;
    RiscvBranchType branchType = NOT_BRANCH;

    void instr_trace_init()
    {

        pending_instr = false;
        pending_branch = false;
        next_pc = 0;
        branch_pc = 0;
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
         << "\nIs Capability: " << (int)curr_instr.is_cap
         << "\n\nDestination Registers: ";
        

        // // Print destination registers
        // for (std::size_t i = 0; i < NUM_INSTR_DESTINATIONS; i++) {
        //     if (curr_instr.destination_registers[i].) {
        //         std::cout << "Reg " << (int)curr_instr.destination_registers[i] << " ";
        //     }
        // }
        
        // std::cout << "\nSource Registers: ";
        // // Print source registers
        // for (std::size_t i = 0; i < NUM_INSTR_SOURCES; i++) {
        //     if (curr_instr.source_registers[i]) {
        //         std::cout << "Reg " << (int)curr_instr.source_registers[i] << " ";
        //     }
        // }
        
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


    private:
        const char* RiscvBranchType_string() const {
            switch(branchType) {
                case BRANCH_DIRECT_JUMP : return "Branch Direct Jump";
                case BRANCH_INDIRECT : return "Branch Indirect";
                case BRANCH_CONDITIONAL : return "Branch Unconditional";
                case BRANCH_DIRECT_CALL : return "Branch Direct Call";
                case BRANCH_INDIRECT_CALL : return "Branch Indirect Call";
                case BRANCH_RETURN : return "Branch Return";
                case BRANCH_OTHER : return "Branch Other";
                case NOT_BRANCH : return "Not a Branch";
                case ERROR : assert(false); //shouldn't end up here...
            }
    }
};


// Regex patterns 
const boost::regex instr_pattern(R"(\[\d+:\d+\]\s+(0x[0-9a-fA-F]+):\s+([0-9a-fA-F]+)\s+([\w\.]+)\s*(.*))");
const boost::regex cap_mem_pattern(
    R"(Cap Memory (Read|Write) \[([0-9a-fA-Fx]+)\] = v:(\d+) PESBT:([0-9a-fA-F]+) Cursor:([0-9a-fA-F]+))"
);
const boost::regex cap_reg_write_pattern(
    R"(Write (c\d+)\/\w+\|v:(\d+) s:(\d+) p:([0-9a-fA-F]+) f:(\d+) b:([0-9a-fA-F]+) l:([0-9a-fA-F]+).*\|o:([0-9a-fA-F]+) t:([0-9a-fA-F]+))"
);
const boost::regex reg_write_pattern(R"(Write (x\d+)/\w+ = ([0-9a-fA-F]+))");

const boost::regex registerPattern(
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


const boost::regex conditionalDirectPattern (
    "\\b(beq|bne|blt|bge|bltu|bgeu|beqz|bnez|bgtu|ble|bleu|bgez|blez|bgtz|bltz|bgt)\\b"
    "\\s+([xsafct][0-9]+|ra|sp|gp|tp|zero)\\s*,?\\s*"  // First register with flexible comma
    "(([xsafct][0-9]+|ra|sp|gp|tp|zero)\\s*,\\s*)?"    // Second register is optional for pseudo-instructions
    "([a-zA-Z0-9_\\.]+|[+-]?[0-9]+)"                   // Label or offset
);

// Unconditional direct jump pattern (J-type)
// Matches instructions like: jal label or jal x1, label
const boost::regex unconditionalDirectPattern (
    "\\b(j|jal|cjal)\\b"
    "(\\s+([xsafct][0-9]+|ra|sp|gp|tp|zero)\\s*,)?"  // Optional destination register
    "\\s+([a-zA-Z0-9_\\.]+|[+-]?[0-9]+)"             // Label or offset
);

// Unconditional indirect jump pattern (I-type/JALR)
// Matches instructions like: jalr x1, x2, 0 or jr x1 or ret
const boost::regex unconditionalIndirectPattern (
    "\\b(jalr|jr|ret|cjalr|jalr\\.cap)\\b"
    "(\\s+([xsafct][0-9]+|ra|sp|gp|tp|zero)"     // Optional destination register
    "(\\s*,\\s*([0-9]+\\s*\\()?(\\s*[xsafct][0-9]+|ra|sp|gp|tp|zero)(\\s*\\))?"  // Source register with optional offset
    "(\\s*,\\s*([+-]?[0-9]+))?)?)?"             // Optional immediate offset
);


// const boost::regex cap_tag_write_pattern(
//     R"(Cap Tag (Read|Write) \[([0-9a-fA-Fx]+)\/[0-9a-fA-Fx]+\] (\d+) -> (\d+))"
// );
// const boost::regex cap_tag_read_pattern(
//     R"(Cap\s+Tag\s+Read\s+\[([0-9a-fA-Fx]+)\/([0-9a-fA-Fx]+)\]\s+->\s+(\d+))"
// );

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
    {"ft8", 28}, {"ft9", 29}, {"ft10", 30}, {"ft11", 31}
};


void write_instr(ProgramTrace& trace, std::ofstream& out) {


    // if (trace.curr_instr.is_branch) {

    //     switch(trace.RiscvBranchType) {

    //         case CONDITONAL_DIRECT : 
    //                 trace.curr_instr.branch_taken = trace.next_pc != trace.curr_instr.ip + 4;
    //                 break;

    //         case UNCONDITIONAL_DIRECT : 
    //         case UNCONDITIONAL_INDIRECT : 
    //             trace.curr_instr.branch_taken = true;
    //             break;

    //         default: break;
    //     }
    //     trace.pending_branch = false;
    // }

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


    if (trace.verbose) trace.debug_print_instruction();
    out.write(reinterpret_cast<const char*>(&trace.curr_instr), sizeof(trace_instr_format));
    trace.instr_trace_init();
}

void process_mem_access(ProgramTrace& trace, const boost::smatch& match, bool is_store)
{
    const uint64_t addr = std::stoull(match[2].str(), nullptr, 0x10);
    const bool tag = std::stoi(match[3]) != 0;
    const uint64_t pesbt = std::stoull(match[4].str(), nullptr, 0x10);
    const uint64_t cursor = std::stoull(match[5].str(), nullptr, 0x10);

    // std::cout << "PESBT = 0x" << std::hex << pesbt << " Cursor = 0x" << std::hex << cursor << std::endl;
    // std::cout << "ADDR = 0x" << std::hex << addr << std::endl;
    // std::cout << "TAG IS " << (int)tag << std::endl;

    cap_data cap = {pesbt, cursor, static_cast<unsigned char>(tag)};


    const auto& ops = is_store ? trace.curr_instr.destination_memory : trace.curr_instr.source_memory;
    const std::size_t size = is_store ? NUM_INSTR_DESTINATIONS : NUM_INSTR_SOURCES;

    for (std::size_t i = 0; i < size; i++) {
        if (ops[i].address == 0) {
            ops[i].address = (unsigned long long)addr;
            ops[i].cap = cap;
            break;
        }

    }
    
}

void process_reg_write(ProgramTrace& trace, const boost::smatch& match)
{

    const std::string reg_name = match[1].str();

    try {
        const uint8_t reg_id  = reg_map.at(reg_name);

        if (trace.verbose) 
            std::cout << "[DEBUG] Processing destination register: " << reg_name << " (ID " << static_cast<int>(reg_id) << ")\n";
        

        // for (auto& dest : trace.curr_instr.destination_registers) {
        //     if (dest ==0) {
        //         dest = (unsigned char)reg_id;
        //         return;
        //     }
        // }

        // if (trace.verbose) {
        //     std::cerr << "Warning: All destination register slots full when trying to add " << static_cast<int>(reg_id) << std::endl;
        //     std::cerr << "Current destination register: ";

        //     for (const auto& r : trace.curr_instr.destination_registers) {
        //         std::cerr << static_cast<int>(r) << " ";
        //     }
        //     std::cerr << std::endl;
        // }

    } catch (const  std::out_of_range&) {
        std::cerr << "Unknown register " << reg_name << std::endl;
    }
  
}

//add this back in if gratz wants it 
// void process_cap_reg_write(ProgramTrace& trace, const boost::smatch& match)
// {

//     const std::string reg_name = match[1].str();
//     const uint8_t reg_id  = reg_map.at(reg_name);

//     for (auto& dest : trace.curr_instr.destination_registers) {
//         if (dest == 0 ) {
//             dest = (unsigned char)reg_id;
//             return;
//         }
//     }
//     if (trace.verbose)
//         std::cerr << "Warning: All destination register slots full when trying to add" << static_cast<int>(reg_id) << std::endl;
// }




void parse_trace(ProgramTrace& trace, const std::string& operands)
{

// /* 

/*
    BRANCH_DIRECT_JUMP = 0,  // Unconditional direct jumps (j)
    BRANCH_INDIRECT,         // Unconditional indirect jumps (jalr without link)
    BRANCH_CONDITIONAL,      // Conditional branches (beq, bne, etc.)
    BRANCH_DIRECT_CALL,      // Direct calls (jal)
    BRANCH_INDIRECT_CALL,    // Indirect calls (jalr with link)
    BRANCH_RETURN,           // Return instructions (ret/jalr used as return)
    BRANCH_OTHER,            // Other types of branches
    NOT_BRANCH,              // Not a branch instruction
    ERROR

*/

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

            // reads flags, writes and reads ip
            case BRANCH_CONDITIONAL: 
                trace.curr_instr.destination_registers[0].reg_id = champsim::REG_INSTRUCTION_POINTER;
                trace.curr_instr.source_registers[0].reg_id = champsim::REG_INSTRUCTION_POINTER;
                trace.curr_instr.source_registers[1].reg_id = champsim::REG_FLAGS;
                break;
        
            //reads ip, sp, writes sp,ip
            case BRANCH_DIRECT_CALL: 
                trace.curr_instr.branch_taken = true;
                trace.curr_instr.destination_registers[0].reg_id = champsim::REG_INSTRUCTION_POINTER;
                trace.curr_instr.destination_registers[1].reg_id = champsim::REG_STACK_POINTER;
                trace.curr_instr.source_registers[0].reg_id = champsim::REG_INSTRUCTION_POINTER;
                trace.curr_instr.source_registers[1].reg_id = champsim::REG_STACK_POINTER;
                break;

            //reads other, ip, sp, writes sp and ip
            case BRANCH_INDIRECT_CALL: 
                trace.curr_instr.branch_taken = true;
                trace.curr_instr.destination_registers[0].reg_id = champsim::REG_INSTRUCTION_POINTER;
                trace.curr_instr.destination_registers[1].reg_id = champsim::REG_STACK_POINTER;
                trace.curr_instr.source_registers[0].reg_id = champsim::REG_INSTRUCTION_POINTER;
                trace.curr_instr.source_registers[1].reg_id = champsim::REG_STACK_POINTER;
                trace.curr_instr.source_registers[0].reg_id = ::REG_AX;
                break;

            //
            case BRANCH_RETURN: 
                trace.curr_instr.branch_taken = true;
                trace.curr_instr.source_registers[0].reg_id = champsim::REG_STACK_POINTER;
                trace.curr_instr.destination_registers[0].reg_id = champsim::REG_INSTRUCTION_POINTER;
                trace.curr_instr.destination_registers[1].reg_id = champsim::REG_STACK_POINTER;
                break;


            case BRANCH_OTHER: //JALR is context dependent
                trace.curr_instr.branch_taken = true;
                trace.curr_instr.destination_registers[0].reg_id = champsim::REG_STACK_POINTER;


                if(reg_map.at(match[2].str()) == 0) { //return

                } 

                else if (reg_map.at(match[2].str()) == 1) //indirect call
                {

                }


                else if (reg_map.at(match[2].str()) == 6) { //direct call

                }


                else {
                    std::cerr << "ERROR: Unknown destination register\n";
                }



            default:
                assert(false);
            }



        }





        // if(match[2].matched && match[3].matched && match[4].matched) {

        //     std::string src1, src2, src3;
        //     src1 = match[2].str(); src2 = match[3].str(); src3 = match[4].str();

        //     trace.curr_instr.source_registers[0].reg_id = reg_map.at(src1);             
        //     trace.curr_instr.source_registers[1].reg_id = reg_map.at(src2);
        //     trace.curr_instr.source_registers[2].reg_id = reg_map.at(src3);

        // }

        // else if (match[5].matched) {
        //     std::string src1 = match[5].str();
        //     trace.curr_instr.source_registers[0] = reg_map.at(src1);

        //     std::string src2 = match[6].str();
        //     if (!src2.empty() && (std::isalpha(static_cast<unsigned char>(src2[0])))) {
        //         trace.curr_instr.source_registers[0].reg_id = reg_map.at(src2);
        //     }
        // }

        // else if (match[8].matched) {
        //     trace.curr_instr.source_registers->reg_id = reg_map.at(match[8].str());
        // }  

        // else if (match[9].matched) {
        //     trace.curr_instr.source_registers[0] = reg_map.at(match[9].str());
        // }  
    }

    else {
        if (trace.verbose) 
            std::cerr << "Error: Unknown operands found: " << operands << std::endl;
    }



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
            auto RiscvBranchType = CONTROL_FLOW_INST.count(trace.mnemonic) ? CONTROL_FLOW_INST.at(trace.mnemonic) : NOT_BRANCH;
            trace.curr_instr.is_branch = RiscvBranchType != NOT_BRANCH;

            if (trace.curr_instr.is_branch) {
                trace.pending_branch = true;
                trace.branch_pc = trace.curr_instr.ip;
            }

            trace.pending_instr = true;    
            parse_trace(trace, match[4].str());

        }

        else if (trace.pending_instr && !trace.pending_branch) {

            if (boost::regex_search(line,match, cap_reg_write_pattern)) {
                process_reg_write(trace,match);

            }

            else if (boost::regex_search(line,match, reg_write_pattern)) {
                process_reg_write(trace,match);
            }


            else if (boost::regex_search(line,match, cap_mem_pattern)){
                process_mem_access(trace, match, match[1].str() == "WRITE");
            }
        }
    }
    

    if (trace.pending_instr) write_instr(trace,trace_outFile);


    trace_file.close();
    trace_outFile.close();

    return EXIT_SUCCESS;





}