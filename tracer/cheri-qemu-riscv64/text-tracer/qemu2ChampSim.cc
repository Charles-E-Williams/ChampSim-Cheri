/*
 * Copyright 2023 The ChampSim Contributors
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

#include <cstring>
#include <vector>
#include <cassert>
#include <cstdint>
#include <unordered_map>
#include <set>
#include <iomanip>
#include <byteswap.h>
#include <limits.h>
#include <re2/re2.h>
#include <iostream>
#include <fstream>

#include "../../../inc/trace_instruction.h"
#include "disas.hh"
#include "stream.hh"
#include "/home/grads/c/charlesw2000/cheri-compressed-cap/cheri_compressed_cap.h"

using trace_instr_format = cheri_instr;
#define CHERI_CAP_MODE (RISCV_DIS_FLAG_CHERI | RISCV_DIS_FLAG_CAPMODE)

// Regular expressions for parsing
const re2::RE2 pc_opcode_pattern("\\]\\s+(0x[0-9a-fA-F]+):\\s+([0-9a-fA-F]+)");
const re2::RE2 mem_pattern("Memory (Read|Write) \\[([0-9A-Fa-f]+)\\] = ([0-9A-Fa-f]+)");
const re2::RE2 cap_reg_pattern(
    R"(Write\s+(c\d+)/(\w+)\|v:(\d+)\s+s:(\d+)\s+p:([0-9a-fA-F]+)\s+f:(\d+)\s+b:([0-9a-fA-F]+)\s+l:([0-9a-fA-F]+)\s+\|o:([0-9a-fA-F]+)\s+t:([0-9a-fA-F]+))"
);
const re2::RE2 cap_mem_pattern(
    R"(Cap Memory (Read|Write) \[([0-9a-fA-F]+)\] = v:(\d+) PESBT:([0-9a-fA-F]+) Cursor:([0-9a-fA-F]+))"
);

void print_usage(const char* program_name) {
    std::cerr << "CHERI-QEMU Trace to ChampSim Converter\n";
    std::cerr << "===========================================\n";
    std::cerr << "Usage: " << program_name << " <input_trace>\n\n";
    std::cerr << "Arguments:\n";
    std::cerr << "  <input_trace>   Path to the text CHERI-QEMU trace file\n\n";
    std::cerr << "Options:\n";
    std::cerr << "  -v              Enable verbose output to stderr\n\n";
    std::cerr << "Output: Binary trace data written to stdout\n";
    std::cerr << "Usage example: " << program_name << " trace.txt | xz -c > trace.champsimtrace.xz\n";
}

struct InstructionTrace {
    trace_instr_format curr_instr;
    rv_decode decoded_instr;
    inst_type_t type = INST_TYPE_UNKNOWN;
    BranchType branch_type = NOT_BRANCH;
    uint32_t opcode;
    bool is_ld = false;
    bool is_st = false;

    InstructionTrace() {
        memset(&curr_instr, 0, sizeof(trace_instr_format));
    }

    const char* RiscvBranchType_to_str() const {
        switch(branch_type) {
            case BRANCH_DIRECT_JUMP : return "Branch Direct Jump";
            case BRANCH_INDIRECT : return "Branch Indirect"; 
            case BRANCH_CONDITIONAL : return "Branch Conditional";
            case BRANCH_DIRECT_CALL : return "Branch Direct Call";
            case BRANCH_INDIRECT_CALL : return "Branch Indirect Call";
            case BRANCH_RETURN : return "Branch Return";
            case BRANCH_OTHER : return "Branch Other";
            case NOT_BRANCH : return "Not a Branch";
            case ERROR : assert(false);
        }
        return "";
    }

    void debug_print_instruction() const {
        std::cerr << "\n=== Instruction Debug ==="
         << "\nPC: 0x" << std::hex << curr_instr.ip
         << "\nInstruction: 0x" << std::hex << decoded_instr.inst 
         << "\nInstruction Type: " << inst_type_to_str(type);

        if (curr_instr.is_branch) {
            std::cerr << "\nBranch Type: " << RiscvBranchType_to_str()
             << "\nBranch Taken: " << (int)curr_instr.branch_taken;
        }

        std::cerr << "\n\nDestination Registers\n";
        for (const auto& regs : curr_instr.destination_registers) {
            if (regs)
                std::cerr << "Register ID " << std::dec << static_cast<int>(regs) <<  "\n";   
        }
        
        std::cerr << "\nSource Registers:\n";
        for (const auto& regs : curr_instr.source_registers) {
            if (regs)
                std::cerr << "Register ID " << std::dec << static_cast<int>(regs) << "\n";      
        }
        
        std::cerr << "\n\nMemory Operands";
        std::cerr << "\nDestination Memory:\n";
        for (const auto& mem : curr_instr.destination_memory) {
            if (mem)
                std::cerr << "Memory Address: 0x" << std::hex << mem << "\n";    
        }
        
        std::cerr << "\nSource Memory:\n";
        for (const auto& mem : curr_instr.source_memory) {
            if (mem)
                std::cerr << "Memory Address: 0x" << std::hex << mem << "\n";  
        }

        if (curr_instr.is_cap_instr) {
            std::cerr << "\nCapability Metadata: "
                      << " | Base 0x" << std::hex << curr_instr.base
                      << " | Length: 0x" << std::hex << curr_instr.length
                      << " | Offset: 0x" << std::hex << curr_instr.offset
                      << " | Perms: 0x" << std::hex << curr_instr.permissions
                      << " | Tag:" << (int)curr_instr.tag;
        }
        std::cerr << "\n=========================\n\n";
    }
};

uint8_t get_instruction_length(uint32_t inst) {
    if ((inst & 0x3) != 0x3) {
        return 2;  // Compressed instruction
    } else {
        return 4;  // Standard 32-bit instruction
    }
}

uint8_t remap_regid(uint8_t reg, InstructionTrace& trace) {
    const char TRANSLATED_REG_IP = 64;
    const char TRANSLATED_REG_SP = 65;
    const char TRANSLATED_REG_FLAGS = 66;
    const char TRANSLATED_REG_ZERO = 67;

    switch (reg) {
        case rv_ireg_zero: return (trace.type == INST_TYPE_FP) ? TRANSLATED_REG_ZERO : rv_ireg_zero;
        case rv_ireg_sp: return champsim::REG_STACK_POINTER;
        case champsim::REG_STACK_POINTER: return TRANSLATED_REG_SP;
        case champsim::REG_FLAGS: return TRANSLATED_REG_FLAGS;
        case champsim::REG_INSTRUCTION_POINTER: return TRANSLATED_REG_IP;
        default: return reg;
    }
}

bool extract_pc_and_opcode(const std::string& line, unsigned long long& pc, uint32_t& opcode) {
    std::string pc_str, opcode_str;
    
    if (RE2::PartialMatch(line, pc_opcode_pattern, &pc_str, &opcode_str)) {
        pc = std::stoull(pc_str, nullptr, 16);
        opcode = std::stoul(opcode_str, nullptr, 16);
        return true;
    }
    return false;   
}

void parse_memory_operation(const std::string& line, InstructionTrace& trace) {
    std::string op_type, address, value;
    
    if (RE2::PartialMatch(line, mem_pattern, &op_type, &address, &value)) {
        uint64_t addr = std::stoull(address, nullptr, 16);
        uint8_t access_size = get_memory_access_size(trace.decoded_instr);
        
        // Calculate cache line boundaries
        uint64_t cacheline_start = addr & ~63ULL;
        uint64_t cacheline_end = (addr + access_size - 1) & ~63ULL;
        
        if (op_type == "Write") {
            trace.curr_instr.destination_memory[0] = addr;
            if (cacheline_end != cacheline_start) {
                trace.curr_instr.destination_memory[1] = cacheline_end;
            }
        } else {
            trace.curr_instr.source_memory[0] = addr;
            if (cacheline_end != cacheline_start) {
                trace.curr_instr.source_memory[1] = cacheline_end;
            }
        }
    }
}

void parse_cap_memory_operation(const std::string& line, InstructionTrace& trace) {
    std::string op_type, address, valid, pesbt, cursor;
    
    if (RE2::PartialMatch(line, cap_mem_pattern, &op_type, &address, &valid, &pesbt, &cursor)) {
        uint64_t addr = std::stoull(address, nullptr, 16);
        uint64_t PESBT = std::stoull(pesbt, nullptr, 16);
        uint64_t CURSOR = std::stoull(cursor, nullptr, 16);
        uint8_t tag = std::stoi(valid, nullptr, 16);
        cc128_cap_t result;

        cc128_decompress_mem(PESBT, CURSOR, (bool)tag, &result);

        trace.curr_instr.base = result.base();
        trace.curr_instr.length = result.length();
        trace.curr_instr.offset = result.offset();
        trace.curr_instr.permissions = result.all_permissions();
        trace.curr_instr.tag = (unsigned char)result.cr_tag;
        
        // Calculate cache line boundaries for capability (16 bytes)
        uint64_t cacheline_start = addr & ~63ULL;
        uint64_t cacheline_end = (addr + 15) & ~63ULL;

        if (op_type == "Write") {
            trace.curr_instr.destination_memory[0] = addr;
            if (cacheline_end != cacheline_start) {
                trace.curr_instr.destination_memory[1] = cacheline_end;
            }
        } else {
            trace.curr_instr.source_memory[0] = addr;
            if (cacheline_end != cacheline_start) {
                trace.curr_instr.source_memory[1] = cacheline_end;
            }
        }
        
        trace.curr_instr.is_cap_instr = 1;
    }
}

void parse_cap_register_write(const std::string& line, InstructionTrace& trace) {
    std::string reg_num, reg_name, valid, sealed, perms, flags, base, length, offset, tag;
    
    if (RE2::PartialMatch(line, cap_reg_pattern, 
                          &reg_num, &reg_name, &valid, &sealed, 
                          &perms, &flags, &base, &length, &offset, &tag)) {
        
        trace.curr_instr.is_cap_instr = 1;
        trace.curr_instr.base = std::stoull(base, nullptr, 16);
        trace.curr_instr.length = std::stoull(length, nullptr, 16);
        trace.curr_instr.offset = std::stoull(offset, nullptr, 16);
        trace.curr_instr.permissions = std::stoul(perms, nullptr, 16);
        trace.curr_instr.tag = std::stoi(valid);
    }
}

BranchType get_branch_type(const rv_decode& dec) {
    switch (dec.op) {
        case rv_op_beq: case rv_op_bne: case rv_op_blt: case rv_op_bge:
        case rv_op_bltu: case rv_op_bgeu: case rv_op_beqz: case rv_op_bnez:
        case rv_op_blez: case rv_op_bgez: case rv_op_bltz: case rv_op_bgtz:
        case rv_op_ble: case rv_op_bleu: case rv_op_bgt: case rv_op_bgtu:
        case rv_op_c_beqz: case rv_op_c_bnez:
            return BRANCH_CONDITIONAL;

        case rv_op_j: case rv_op_c_j: case rv_op_jal: case rv_op_c_jal:
            return (dec.rd == rv_ireg_zero) ? BRANCH_DIRECT_JUMP : BRANCH_DIRECT_CALL;
        
        case rv_op_jalr: case rv_op_c_jalr:
            if (dec.rd == rv_ireg_zero)   
                return (dec.rs1 == rv_ireg_ra) ? BRANCH_RETURN : BRANCH_INDIRECT;
            else return BRANCH_INDIRECT_CALL;

        case rv_op_jr: case rv_op_c_jr:
            return (dec.rs1 == rv_ireg_ra) ? BRANCH_RETURN : BRANCH_INDIRECT;
        case rv_op_ret:
            return BRANCH_RETURN;

        case rv_op_cjalr:
            return (dec.rd == rv_ireg_zero) ? BRANCH_INDIRECT : BRANCH_INDIRECT_CALL;

        default: return ERROR;
    }
}

void set_branch_info(InstructionTrace& trace) {
    switch (trace.branch_type) {
        case BRANCH_DIRECT_JUMP:
            trace.curr_instr.destination_registers[0] = champsim::REG_INSTRUCTION_POINTER;
            trace.curr_instr.branch_taken = true;
            break;

        case BRANCH_INDIRECT:
            trace.curr_instr.branch_taken = true;
            trace.curr_instr.destination_registers[0] = champsim::REG_INSTRUCTION_POINTER;
            trace.curr_instr.source_registers[0] = remap_regid(trace.decoded_instr.rs1, trace);
            break;

        case BRANCH_CONDITIONAL:
            trace.curr_instr.destination_registers[0] = champsim::REG_INSTRUCTION_POINTER;
            trace.curr_instr.source_registers[0] = champsim::REG_INSTRUCTION_POINTER;
            trace.curr_instr.source_registers[1] = remap_regid(trace.decoded_instr.rs1, trace);
            if (trace.decoded_instr.rs2 != rv_ireg_zero)
                trace.curr_instr.source_registers[2] = remap_regid(trace.decoded_instr.rs2, trace);
            break;

        case BRANCH_DIRECT_CALL:
            trace.curr_instr.branch_taken = true;
            trace.curr_instr.destination_registers[0] = champsim::REG_INSTRUCTION_POINTER;
            trace.curr_instr.destination_registers[1] = remap_regid(trace.decoded_instr.rd, trace);
            trace.curr_instr.source_registers[0] = champsim::REG_INSTRUCTION_POINTER;
            break;

        case BRANCH_INDIRECT_CALL:
            trace.curr_instr.branch_taken = true;
            trace.curr_instr.destination_registers[0] = champsim::REG_INSTRUCTION_POINTER;
            trace.curr_instr.destination_registers[1] = remap_regid(trace.decoded_instr.rd, trace);
            trace.curr_instr.source_registers[0] = champsim::REG_INSTRUCTION_POINTER;
            trace.curr_instr.source_registers[1] = remap_regid(trace.decoded_instr.rs1, trace);
            break;
        
        case BRANCH_RETURN:
            trace.curr_instr.branch_taken = true;
            trace.curr_instr.destination_registers[0] = champsim::REG_INSTRUCTION_POINTER;
            trace.curr_instr.source_registers[0] = remap_regid(trace.decoded_instr.rs1, trace);
            break;

        case BRANCH_OTHER:
        case NOT_BRANCH:
        default: 
            assert(false && "Error: Unrecognized branch type\n");
    }
}

void set_load_store_info(InstructionTrace& trace) {
    switch (trace.type) {
        case INST_TYPE_LOAD:
            trace.is_ld = true;
            trace.curr_instr.destination_registers[0] = remap_regid(trace.decoded_instr.rd, trace);
            trace.curr_instr.source_registers[0] = remap_regid(trace.decoded_instr.rs1, trace);
            break;

        case INST_TYPE_STORE:
            trace.is_st = true;
            trace.curr_instr.source_registers[0] = remap_regid(trace.decoded_instr.rs1, trace);
            trace.curr_instr.source_registers[1] = remap_regid(trace.decoded_instr.rs2, trace);
            break;

        case INST_TYPE_CAP_LOAD:
            trace.is_ld = true;
            trace.curr_instr.is_cap_instr = 1;
            trace.curr_instr.destination_registers[0] = remap_regid(trace.decoded_instr.rd, trace);
            trace.curr_instr.source_registers[0] = remap_regid(trace.decoded_instr.rs1, trace);
            break;

        case INST_TYPE_CAP_STORE:
            trace.is_st = true;
            trace.curr_instr.is_cap_instr = 1;
            trace.curr_instr.source_registers[0] = remap_regid(trace.decoded_instr.rs1, trace);
            trace.curr_instr.source_registers[1] = remap_regid(trace.decoded_instr.rs2, trace);
            break;

        case INST_TYPE_FP_LOAD:
            trace.is_ld = true;
            trace.curr_instr.destination_registers[0] = remap_regid(trace.decoded_instr.rd, trace);
            trace.curr_instr.source_registers[0] = remap_regid(trace.decoded_instr.rs1, trace);
            break;

        case INST_TYPE_FP_STORE:
            trace.is_st = true;
            trace.curr_instr.source_registers[0] = remap_regid(trace.decoded_instr.rs1, trace);
            trace.curr_instr.source_registers[1] = remap_regid(trace.decoded_instr.rs2, trace);
            break;

        default:
            assert(false);
    }
}

void set_atomic_reg_info(InstructionTrace& trace) {
    switch (trace.type) {
        case INST_TYPE_AMO:
            trace.curr_instr.destination_registers[0] = remap_regid(trace.decoded_instr.rd, trace);
            trace.curr_instr.source_registers[0] = remap_regid(trace.decoded_instr.rs2, trace);
            trace.curr_instr.source_registers[1] = remap_regid(trace.decoded_instr.rs1, trace);
            break;

        case INST_TYPE_AMO_LOAD: 
            trace.curr_instr.destination_registers[0] = remap_regid(trace.decoded_instr.rd, trace);
            trace.curr_instr.source_registers[0] = remap_regid(trace.decoded_instr.rs1, trace);
            break;

        case INST_TYPE_AMO_STORE:
            trace.curr_instr.destination_registers[0] = remap_regid(trace.decoded_instr.rd, trace);
            trace.curr_instr.source_registers[0] = remap_regid(trace.decoded_instr.rs1, trace);
            trace.curr_instr.source_registers[1] = remap_regid(trace.decoded_instr.rs2, trace);
            break;

        default: 
            assert(false);
    }
}

void set_reg_data(InstructionTrace& trace) {
    int num_regs_used = count_register_operands(trace.decoded_instr.codec);
    assert(num_regs_used != 0 && num_regs_used != 55);

    switch (num_regs_used) {
        case 1:
            trace.curr_instr.destination_registers[0] = remap_regid(trace.decoded_instr.rd, trace);
            break;

        case 2:
            trace.curr_instr.destination_registers[0] = remap_regid(trace.decoded_instr.rd, trace);
            trace.curr_instr.source_registers[0] = remap_regid(trace.decoded_instr.rs1, trace);       
            break;

        case 3:
            trace.curr_instr.destination_registers[0] = remap_regid(trace.decoded_instr.rd, trace);
            trace.curr_instr.source_registers[0] = remap_regid(trace.decoded_instr.rs1, trace);
            trace.curr_instr.source_registers[1] = remap_regid(trace.decoded_instr.rs2, trace);
            break;

        case 4:
            trace.curr_instr.destination_registers[0] = remap_regid(trace.decoded_instr.rd, trace);
            trace.curr_instr.source_registers[0] = remap_regid(trace.decoded_instr.rs1, trace);
            trace.curr_instr.source_registers[1] = remap_regid(trace.decoded_instr.rs2, trace);
            trace.curr_instr.source_registers[2] = remap_regid(trace.decoded_instr.rs3, trace);
            break;

        default:
            assert(false);
    }
}

void convert_cheri_trace_entry(InstructionTrace& trace) {
    trace.decoded_instr = disasm_inst(rv64, trace.curr_instr.ip, trace.opcode, CHERI_CAP_MODE);

    assert(trace.decoded_instr.op != rv_op_illegal); 
    trace.type = classify_instruction(trace.decoded_instr);
    trace.curr_instr.is_branch = trace.type == INST_TYPE_BRANCH;

    if (trace.type == INST_TYPE_UNKNOWN) {
        std::cerr << "Error: Unknown instruction 0x" << std::hex << trace.decoded_instr.inst 
                  << " at PC 0x" << std::hex << trace.decoded_instr.pc << std::endl;
        assert(false);
    }

    switch (trace.type) {
        case INST_TYPE_BRANCH:
            trace.branch_type = get_branch_type(trace.decoded_instr);
            assert(trace.branch_type != ERROR);
            set_branch_info(trace);
            break;

        case INST_TYPE_LOAD:
        case INST_TYPE_CAP_LOAD:
        case INST_TYPE_FP_LOAD:
        case INST_TYPE_STORE:
        case INST_TYPE_CAP_STORE:
        case INST_TYPE_FP_STORE:
            set_load_store_info(trace);
            break;

        case INST_TYPE_CAP_OP:
            trace.curr_instr.is_cap_instr = 1;
            set_reg_data(trace);
            break;

        case INST_TYPE_ALU:       
        case INST_TYPE_CSR:
        case INST_TYPE_FP:
            set_reg_data(trace);
            break;

        case INST_TYPE_AMO:
        case INST_TYPE_AMO_LOAD:
        case INST_TYPE_AMO_STORE:
            set_atomic_reg_info(trace);
            break;

        case INST_TYPE_SYSTEM:
            break;

        default:
            assert(false);      
    }  
}

int main(int argc, char** argv) {
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }
    
    std::string input_file = argv[1];
    bool verbose = false;
    
    for (int i = 2; i < argc; i++) {
        if(strcmp(argv[i], "-v") == 0) {
            verbose = true;
        }
    }
    
    auto input = create_input_stream(input_file);
    if (!input || !input->is_open()) {
        std::cerr << "Error: Cannot open input file " << input_file << std::endl;
        return 1;
    }

    InstructionTrace trace, prev_trace;
    uint64_t instruction_count = 0;
    bool pending_instr = false;
    std::string line;
    
    while (input->getline(line)) {
        // Check if this line contains an instruction
        if (extract_pc_and_opcode(line, trace.curr_instr.ip, trace.opcode)) {
            // Process previous instruction if we have one
            if (pending_instr) {
                // Handle conditional branch taken/not taken
                if (prev_trace.branch_type == BRANCH_CONDITIONAL) {
                    uint64_t fall_through_pc = prev_trace.curr_instr.ip + get_instruction_length(prev_trace.decoded_instr.inst);
                    prev_trace.curr_instr.branch_taken = (trace.curr_instr.ip != fall_through_pc);
                }
                
                // Write previous instruction to stdout (unless verbose mode is enabled)
                if (!verbose) {
                    std::cout.write(reinterpret_cast<const char*>(&prev_trace.curr_instr), sizeof(trace_instr_format));
                }
                
                if (verbose) {
                    prev_trace.debug_print_instruction();
                }
            }
            
            // Setup new instruction
            InstructionTrace new_trace; // Use a temporary new trace object
            new_trace.curr_instr.ip = trace.curr_instr.ip;
            new_trace.opcode = trace.opcode;
            
            // Process the current instruction
            convert_cheri_trace_entry(new_trace);
            
            prev_trace = new_trace;
            pending_instr = true;
            instruction_count++;
            
        } else if (pending_instr) {

            parse_memory_operation(line, prev_trace);
            parse_cap_memory_operation(line, prev_trace);
            parse_cap_register_write(line, prev_trace);
        }
    }
    
    // Handle the last pending instruction
    if (pending_instr) {
        if (verbose) {
             prev_trace.debug_print_instruction();
        } else {
             std::cout.write(reinterpret_cast<const char*>(&prev_trace.curr_instr), sizeof(trace_instr_format));
        }
    }
    
    input->close();
    
    std::cerr << "Processed " << instruction_count << " total instructions." << std::endl;
    
    return 0;
}
