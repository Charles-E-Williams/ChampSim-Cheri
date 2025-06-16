#include <fstream>
#include <iostream>
#include <cstring>
#include <vector>
#include <cassert>
#include <cstdint>
#include <unordered_map>
#include <string>
#include <set>
#include <iomanip>
#include <byteswap.h>

#define CHERI
#include "../../inc/trace_instruction.h"
#include "disas.hh"
using cap_data = cap_metadata;
using trace_instr_format = input_instr;



#define CTE_NO_REG  0   /* No register is changed. */
#define CTE_GPR     1   /* GPR change (val2) */
#define CTE_LD_GPR  2   /* Load into GPR (val2) from address (val1) */
#define CTE_ST_GPR  3   /* Store from GPR (val2) to address (val1) */
#define CTE_CAP     11  /* Cap change (val2,val3,val4,val5) */
#define CTE_LD_CAP  12  /* Load Cap (val2,val3,val4,val5) from addr (val1) */
#define CTE_ST_CAP  13  /* Store Cap (val2,val3,val4,val5) to addr (val1) */
#define CTE_EXCEPTION_NONE 31
#define CHERI_CAP_MODE (RISCV_DIS_FLAG_CHERI | RISCV_DIS_FLAG_CAPMODE)


// RISC-V registers
#define RISCV_REG_ZERO 0
#define RISCV_REG_RA 1
#define RISCV_REG_SP 2

// Constants
namespace {
    constexpr char REG_AX = 56;
}

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
    {"s10", 26}, {"s11", 27}, {"t3", 28}, {"t4", 29}, {"t5", 30}, {"t6", 31}
};

uint64_t get_instruction_length(uint32_t inst) {
    // Check if it's a compressed instruction (16-bit)
    if ((inst & 0x3) != 0x3) {
        return 2;  // Compressed instruction
    } else {
        return 4;  // Standard 32-bit instruction
    }
}

typedef struct {
    uint8_t entry_type;
    uint8_t exception;  /* 0=none, 1=TLB Mod, 2=TLB Load, 3=TLB Store, etc. */
    uint16_t cycles;    /* Currently not used. */
    uint32_t inst;      /* Encoded instruction. */
    uint64_t pc;        /* PC value of instruction. */
    uint64_t val1;      /* val1 is used for memory address. */
    uint64_t val2;      /* val2, val3, val4, val5 are used for reg content. */
    uint64_t val3;
    uint64_t val4;
    uint64_t val5;
    uint8_t thread;     /* Hardware thread/CPU (i.e. cpu->cpu_index ) */
    uint8_t asid;       /* Address Space ID */
} __attribute__((packed)) cheri_trace_entry_t;

// Helper function for entry type names
const char* entry_type_to_string(uint8_t type) {
    switch(type) {
        case CTE_NO_REG:  return "CTE_NO_REG";
        case CTE_GPR:     return "CTE_GPR";
        case CTE_LD_GPR:  return "CTE_LD_GPR";
        case CTE_ST_GPR:  return "CTE_ST_GPR";
        case CTE_CAP:     return "CTE_CAP";
        case CTE_LD_CAP:  return "CTE_LD_CAP";
        case CTE_ST_CAP:  return "CTE_ST_CAP";
        default:          return "UNKNOWN";
    }
}

struct InstructionTrace {
    BranchType branch_type = NOT_BRANCH;
    trace_instr_format curr_instr;
    uint64_t branch_target_pc;
    rv_decode decoded_instr;
    inst_type_t type = INST_TYPE_UNKNOWN;


    InstructionTrace() {
        memset(&curr_instr, 0, sizeof(trace_instr_format));
    }


    const char* RiscvBranchType_string() const {
        switch(branch_type) {
            case BRANCH_DIRECT_JUMP : return "Branch Direct Jump"; //gg
            case BRANCH_INDIRECT : return "Branch Indirect"; //gg 
            case BRANCH_CONDITIONAL : return "Branch Conditional"; //gg
            case BRANCH_DIRECT_CALL : return "Branch Direct Call"; //??
            case BRANCH_INDIRECT_CALL : return "Branch Indirect Call"; //gg
            case BRANCH_RETURN : return "Branch Return"; //gg
            case BRANCH_OTHER : return "Branch Other";
            case NOT_BRANCH : return "Not a Branch";
            case ERROR : assert(false); //shouldn't end up here...
        }
        return "";
    }

    void debug_print_instruction() const {

        std::cout << "\n=== Instruction Debug ==="
         << "\nPC: 0x" << std::hex << curr_instr.ip
         << "\nIs Branch: " << std::dec << (int)curr_instr.is_branch
         << "\nBranch Type: " << RiscvBranchType_string()
         << "\nBranch Taken: " << (int)curr_instr.branch_taken
         << "\nIs Capability Instruction: " << (int)curr_instr.is_cap
         << "\nInstruction: 0x" << std::hex << decoded_instr.inst 
         << "\nInstruction Type: " << inst_type_to_str(type)
         << "\n\nDestination Registers: ";
        

        // // Print destination registers
        for (const auto& regs : curr_instr.destination_registers) {
            std::cout << "\n Register ID " << std::dec << static_cast<int>(regs) <<  "\n";
            
        }
        
        std::cout << "\nSource Registers: ";
        // Print source registers
        for (const auto& regs : curr_instr.source_registers) {
            std::cout << "\n Register ID " << std::dec << static_cast<int>(regs) << "\n";
            
        }
        
        std::cout << "\n\nMemory Operands:";
        // Print destination memory
        std::cout << "\nDestination Memory:";
        for (const auto& mem : curr_instr.destination_memory) {
            std::cout << "\n Memory Address: 0x" << std::hex << mem << "\n";
            
        }
        
        // Print source memory
        std::cout << "\nSource Memory:";
        for (const auto& mem : curr_instr.source_memory) {
            std::cout << "\n Memory Address: 0x" << std::hex << mem << "\n";
            
        }

        std::cout << "\nCapability Metadata: "
                << " | Base 0x" << std::hex << curr_instr.cap.base
                << " | Length: 0x" << std::hex << curr_instr.cap.length
                << " | Offset: 0x" << std::hex << curr_instr.cap.offset
                << " | Perms: 0x" << std::hex << curr_instr.cap.perms
                << " | Tag:" << (int)curr_instr.cap.tag;
            

        
        std::cout << "\n=========================\n" << std::dec << std::endl;
    }


};





// Print usage and help information
void print_usage(const char* program_name) {
    std::cerr << "CHERI-RISC-V 64 Trace to ChampSim Converter\n";
    std::cerr << "===========================================\n";
    std::cerr << "Usage: " << program_name << " <input_trace> <output_trace> [-v]\n\n";
    std::cerr << "Arguments:\n";
    std::cerr << "  <input_trace>    Path to the binary CHERI-QEMU trace file\n";
    std::cerr << "  <output_trace>   Path for the output ChampSim-compatible trace file\n";
    std::cerr << "  -v               Enable verbose output during conversion\n\n";
    std::cerr << "Example:\n";
    std::cerr << "  " << program_name << " cheri_trace.bin output.champsim.trace -v\n\n";
    std::cerr << "Note: This converter is specifically designed for CHERI-RISC-V traces\n";
    std::cerr << "      produced by CHERI-QEMU with capability tracing enabled.\n";
    std::cerr << "This program comes as is with no guarantee of covering every conversion case.\n";
}

void print_metadata(uint64_t metadata) {
    std::cout << "  Tag:      " << ((metadata >> 63) & 0x1) << "\n";
    std::cout << "  Perms:    0x" << ((metadata >> 1) &  0x7FFFFFFF) << "\n";
}

bool is_cap_instr(const cheri_trace_entry_t& entry) {

    switch(entry.entry_type) {
        case CTE_CAP:
        case CTE_LD_CAP:
        case CTE_ST_CAP:
            return true;
        default: return false;
    }
    assert(false);
}


bool is_branch_instruction(InstructionTrace& trace) {


    switch (trace.decoded_instr.op) {
        // Standard branch instructions
        case rv_op_beq:
        case rv_op_bne:
        case rv_op_blt:
        case rv_op_bge:
        case rv_op_bltu:
        case rv_op_bgeu:
        // Pseudo branch instructions
        case rv_op_beqz:
        case rv_op_bnez:
        case rv_op_blez:
        case rv_op_bgez:
        case rv_op_bltz:
        case rv_op_bgtz:
        case rv_op_ble:
        case rv_op_bleu:
        case rv_op_bgt:
        case rv_op_bgtu:
        // Compressed branches
        case rv_op_c_beqz:
        case rv_op_c_bnez:
        // Jump and link instructions
        case rv_op_jal:
        case rv_op_jalr:
        case rv_op_c_jal:
        case rv_op_c_jalr:
        // Pseudo jump instructions
        case rv_op_j:
        case rv_op_ret:
        case rv_op_jr:
        case rv_op_c_j:
        case rv_op_c_jr:
        case rv_op_cjalr:
            trace.type = INST_TYPE_BRANCH;
            return true;

        default: return false;
    }
    return false;
}


BranchType get_branch_type(const rv_decode& dec)
{
    switch (dec.op) {
        // Standard branch instructions
        case rv_op_beq:
        case rv_op_bne:
        case rv_op_blt:
        case rv_op_bge:
        case rv_op_bltu:
        case rv_op_bgeu:
        // Pseudo branch instructions
        case rv_op_beqz:
        case rv_op_bnez:
        case rv_op_blez:
        case rv_op_bgez:
        case rv_op_bltz:
        case rv_op_bgtz:
        case rv_op_ble:
        case rv_op_bleu:
        case rv_op_bgt:
        case rv_op_bgtu:
        // Compressed branches
        case rv_op_c_beqz:
        case rv_op_c_bnez:
            return BRANCH_CONDITIONAL;
        // Jump and link instructions
        case rv_op_j:
        case rv_op_c_j:
        case rv_op_jal:
        case rv_op_c_jal:
           // printf("J or jal instruction\n");
            return (dec.rd == RISCV_REG_ZERO) ? BRANCH_DIRECT_JUMP : BRANCH_DIRECT_CALL;
        
        case rv_op_jalr:
        case rv_op_c_jalr:
          //  printf("jalr instruction\n");
            if (dec.rd == RISCV_REG_ZERO)   
                return (dec.rs1 == RISCV_REG_RA) ? BRANCH_RETURN : BRANCH_INDIRECT;
            else return BRANCH_INDIRECT_CALL;

        // Pseudo jump instructions
        case rv_op_jr:
        case rv_op_c_jr:
            return BRANCH_INDIRECT;
        case rv_op_ret:
            return BRANCH_RETURN;

        case rv_op_cjalr:
            return (dec.rd == RISCV_REG_ZERO) ? BRANCH_INDIRECT : BRANCH_INDIRECT_CALL;

        default: return ERROR;
    }
    
    assert(false); //shouldn't end up here.

}

cap_data set_cap(cheri_trace_entry_t& entry) {
    cap_data c;

    /*
        Metadata (tag, perms, seal, flag) is in val2
        Cursor is stored in val3
        Base is in val4
        Length is in val5
        We can derive the offset from Cursor - Base 
    */

    c.tag = (entry.val2 >> 63) & 0x1;
    c.perms = (entry.val2 & 0xFFFF);
    c.length = entry.val5;
    c.base = entry.val4;
    c.offset = entry.val3 - entry.val4;
    return c;
}


void set_branch_info(InstructionTrace& trace){
    switch (trace.branch_type)
    {
    case BRANCH_DIRECT_JUMP:
        trace.curr_instr.destination_registers[0] = champsim::REG_INSTRUCTION_POINTER;
        trace.curr_instr.branch_taken = true;
        break;

    case BRANCH_INDIRECT:
        trace.curr_instr.branch_taken = true;
        trace.curr_instr.destination_registers[0] = champsim::REG_INSTRUCTION_POINTER;
        trace.curr_instr.source_registers[0] = ::REG_AX;
        break;

    case BRANCH_CONDITIONAL:
        trace.branch_target_pc = trace.curr_instr.ip + 4;
        trace.curr_instr.destination_registers[0] = champsim::REG_INSTRUCTION_POINTER;
        trace.curr_instr.source_registers[0] = champsim::REG_INSTRUCTION_POINTER;
        trace.curr_instr.source_registers[1] = ::REG_AX;
        break;

    case BRANCH_DIRECT_CALL:
        trace.curr_instr.branch_taken = true;
        trace.curr_instr.destination_registers[0] = champsim::REG_INSTRUCTION_POINTER;
        trace.curr_instr.destination_registers[1] = champsim::REG_STACK_POINTER;
        trace.curr_instr.source_registers[0] = champsim::REG_INSTRUCTION_POINTER;
        trace.curr_instr.source_registers[1] = champsim::REG_STACK_POINTER;
        break;

    case BRANCH_INDIRECT_CALL:
        trace.curr_instr.branch_taken = true;
        trace.curr_instr.destination_registers[0] = champsim::REG_INSTRUCTION_POINTER;
        trace.curr_instr.destination_registers[1] = champsim::REG_STACK_POINTER;
        trace.curr_instr.source_registers[0] = champsim::REG_INSTRUCTION_POINTER;
        trace.curr_instr.source_registers[1] = champsim::REG_STACK_POINTER;
        trace.curr_instr.source_registers[2] = ::REG_AX;
        break;
    
    case BRANCH_RETURN:
        trace.curr_instr.branch_taken = true;
        trace.curr_instr.source_registers[0] = champsim::REG_STACK_POINTER;
        trace.curr_instr.destination_registers[0] = champsim::REG_INSTRUCTION_POINTER;
        trace.curr_instr.destination_registers[1] = champsim::REG_STACK_POINTER;
        break;

    case BRANCH_OTHER:
    case NOT_BRANCH:
    default: 
        assert(false && "Error: Unrecognized branch type\n");
    }
}

void set_mem_info(InstructionTrace& trace, const cheri_trace_entry_t& entry) {

    if (entry.entry_type == CTE_LD_CAP || entry.entry_type == CTE_LD_GPR) {
        trace.curr_instr.destination_registers[0] = trace.decoded_instr.rd;
        trace.curr_instr.source_registers[0] = trace.decoded_instr.rs1;
        trace.curr_instr.source_memory[0] = entry.val1;
    }

    else if (entry.entry_type == CTE_ST_CAP || entry.entry_type == CTE_ST_GPR) {
        trace.curr_instr.source_registers[0]= trace.decoded_instr.rs1;
        trace.curr_instr.source_registers[1] = trace.decoded_instr.rs2;
        trace.curr_instr.destination_memory[0] = entry.val1;    
    }

    else assert(false);
}

void set_cap_info(InstructionTrace& trace, cheri_trace_entry_t& entry) {

    if (entry.entry_type == CTE_CAP) {
        trace.curr_instr.cap = set_cap(entry);
    }

    else if (entry.entry_type == CTE_LD_CAP || trace.type == INST_TYPE_CAP_LOAD) {
        trace.curr_instr.cap = set_cap(entry);
    }

    else if (entry.entry_type == CTE_ST_CAP || trace.type == INST_TYPE_CAP_STORE) {
        trace.curr_instr.cap = set_cap(entry);
    }

    else assert(false && "Invalid entry type\n");
}

void set_reg_info(InstructionTrace& trace)       
{
    int num_regs_used = count_register_operands(trace.decoded_instr.codec);

    switch (num_regs_used)
    {
    case 0:
    case 55:
        assert(false);
        break;

    case 1:
        trace.curr_instr.destination_registers[0] = trace.decoded_instr.rd;
        break;

    case 2:
        trace.curr_instr.destination_registers[0] = trace.decoded_instr.rd;
        trace.curr_instr.source_registers[0] = trace.decoded_instr.rs1;        
        break;

    case 3:
        trace.curr_instr.destination_registers[0] = trace.decoded_instr.rd;
        trace.curr_instr.source_registers[0] = trace.decoded_instr.rs1;
        trace.curr_instr.source_registers[1] = trace.decoded_instr.rs2;
        break;

    case 4:
        trace.curr_instr.destination_registers[0] = trace.decoded_instr.rd;
        trace.curr_instr.source_registers[0] = trace.decoded_instr.rs1;
        trace.curr_instr.source_registers[1] = trace.decoded_instr.rs2;
        trace.curr_instr.source_registers[2] = trace.decoded_instr.rs3;
        break;
    }

}

void set_info_atomic(InstructionTrace& trace, const cheri_trace_entry_t& entry)
{
    int num_regs_used = count_register_operands(trace.decoded_instr.codec);

    switch (num_regs_used)
    {
    case 2:
        trace.curr_instr.destination_registers[0] = trace.decoded_instr.rd;
        trace.curr_instr.source_registers[0] = trace.decoded_instr.rs1;
        trace.curr_instr.source_memory[0] = entry.val1;
        break;

    case 3: 
        trace.curr_instr.destination_registers[0] = trace.decoded_instr.rd;
        trace.curr_instr.source_registers[0] = trace.decoded_instr.rs2;
        trace.curr_instr.source_registers[1] = trace.decoded_instr.rs1;
        trace.curr_instr.destination_memory[0] = entry.val1;
        break;
    default:
        assert(false);
        break;
    }
}

void convert_cheri_trace_entry(cheri_trace_entry_t& entry, InstructionTrace& trace)
{
    trace.decoded_instr = disasm_inst(rv64, entry.pc, entry.inst, CHERI_CAP_MODE);
    assert(trace.decoded_instr.op != rv_op_illegal); 

    trace.curr_instr.is_branch = is_branch_instruction(trace);
    trace.curr_instr.ip = entry.pc;
    trace.type = classify_instruction(trace.decoded_instr);
    trace.curr_instr.is_cap = is_cap_instr(entry) || (trace.type == INST_TYPE_CAP_LOAD ) || (trace.type == INST_TYPE_CAP_STORE);

    if (trace.type == INST_TYPE_UNKNOWN) {
        printf("Unknown instruction: %08lx\n", trace.decoded_instr.inst);
        assert(false);
    }

    switch (trace.type)
    {
        case INST_TYPE_BRANCH:
            trace.branch_type = get_branch_type(trace.decoded_instr);
            assert(trace.branch_type != NOT_BRANCH && trace.branch_type != ERROR);
            set_branch_info(trace);
            break;

        case INST_TYPE_LOAD:
        case INST_TYPE_STORE:
        case INST_TYPE_CAP_LOAD:
        case INST_TYPE_CAP_STORE: 
        case INST_TYPE_FP_LOAD:
        case INST_TYPE_FP_STORE:
            set_mem_info(trace,entry);
            break;

        case INST_TYPE_CAP_OP:
        case INST_TYPE_ALU:        
        case INST_TYPE_FP:
            set_reg_info(trace);
            break;
        case INST_TYPE_ATOMIC:
            set_info_atomic(trace,entry);
            break;
        
        case INST_TYPE_CSR: 
        case INST_TYPE_SYSTEM:
            if (count_register_operands(trace.decoded_instr.codec) > 0) 
                set_reg_info(trace);
            break;

        default:
            assert(false);       
    }


    if (trace.curr_instr.is_cap)
        set_cap_info(trace,entry);
}



int main(int argc, char** argv) {
    if (argc < 3) {
        print_usage(argv[0]);
        return 1;
    }
    
    std::string input_file = argv[1];
    std::string output_file = argv[2];
    bool verbose = (argc > 3 && std::string(argv[3]) == "-v");
    
    // Open input trace file
    std::ifstream input(input_file, std::ios::binary);
    if (!input) {
        std::cerr << "Error: Cannot open input file " << input_file << std::endl;
        return 1;
    }
    
    // Open output trace file
    std::ofstream output(output_file, std::ios::binary);
    if (!output) {
        std::cerr << "Error: Cannot open output file " << output_file << std::endl;
        return 1;
    }
    
    // Process trace entries
    cheri_trace_entry_t entry;
    InstructionTrace trace;
    InstructionTrace prev_trace;
    uint64_t instruction_count = 0;
    bool pending_instr = false;

    while (input.read(reinterpret_cast<char*>(&entry), sizeof(cheri_trace_entry_t))) {
        entry.pc = bswap_64(entry.pc);
        entry.val1 = bswap_64(entry.val1);
        entry.val2 = bswap_64(entry.val2);
        entry.val3 = bswap_64(entry.val3);
        entry.val4 = bswap_64(entry.val4);
        entry.val5 = bswap_64(entry.val5);
        entry.inst = bswap_32(entry.inst);

        // Process current entry first
        trace = InstructionTrace();
        convert_cheri_trace_entry(entry, trace);

        if (pending_instr) {
            // Check if previous instruction was a conditional branch
            if (prev_trace.branch_type == BRANCH_CONDITIONAL) {
                uint64_t fall_through_pc = prev_trace.curr_instr.ip + 
                    get_instruction_length(prev_trace.decoded_instr.inst);
                
                // If current PC != fall-through PC, branch was taken
                if (entry.pc != fall_through_pc) {
                    prev_trace.curr_instr.branch_taken = true;
                } else {
                    prev_trace.curr_instr.branch_taken = false;
                }
                
                if (verbose) {
                    printf("Conditional branch at 0x%llx: %s (next PC: 0x%lx, expected fall-through: 0x%lx)\n",
                           prev_trace.curr_instr.ip,
                           prev_trace.curr_instr.branch_taken ? "TAKEN" : "NOT TAKEN",
                           entry.pc, fall_through_pc);
                }
            }

            output.write(reinterpret_cast<const char*>(&prev_trace.curr_instr), sizeof(trace_instr_format));
            if (verbose){
                prev_trace.debug_print_instruction();
            }

        }

        prev_trace = trace; // Save current as previous AFTER processing
        pending_instr = true;
        instruction_count++;
    }
    
    // Don't forget to write the last instruction
    if (pending_instr) {
        output.write(reinterpret_cast<const char*>(&trace.curr_instr), sizeof(trace_instr_format));
        if (verbose)
            trace.debug_print_instruction();
    }
    
    input.close();
    output.close();
    
    if (verbose) {
        std::cout << "Processed " << instruction_count << " instructions." << std::endl;
    }
    
    return 0;
}
