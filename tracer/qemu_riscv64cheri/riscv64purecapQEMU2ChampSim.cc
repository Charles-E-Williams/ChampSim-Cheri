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
using trace_instr_format = input_instr;


#define CTE_QEMU_VERSION (0x80U + 3)
#define CTE_QEMU_MAGIC "CheriTraceV03"

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

const char TRANSLATED_REG_IP = 64;
const char TRANSLATED_REG_SP = 65;
const char TRANSLATED_REG_FLAGS = 66;
const char TRANSLATED_REG_ZERO = 67;

std::vector<uint16_t>  simpoints;

// Print usage and help datarmation
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

uint8_t remap_regid(uint8_t reg) {
    switch (reg) {
        case RISCV_REG_ZERO: return TRANSLATED_REG_ZERO;
        case champsim::REG_STACK_POINTER: return TRANSLATED_REG_IP;
        case champsim::REG_FLAGS: return TRANSLATED_REG_SP;
        case champsim::REG_INSTRUCTION_POINTER: return TRANSLATED_REG_FLAGS;
        default: return reg;
    }
}

uint8_t get_instruction_length(uint32_t inst) {
    // Check if it's a compressed instruction (16-bit)
    if ((inst & 0x3) != 0x3) {
        return 2;  // Compressed instruction
    } else {
        return 4;  // Standard 32-bit instruction
    }
}

#pragma pack(push, 1)
typedef struct {
    uint8_t entry_type;
    uint8_t exception;
    uint16_t cycles;
    uint32_t inst;
    uint64_t pc;
    uint64_t val1;
    uint64_t val2;
    uint64_t val3;
    uint64_t val4;
    uint64_t val5;
    uint8_t thread;
    uint8_t asid;
} cheri_trace_entry_t;
#pragma pack(pop)

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
    rv_decode decoded_instr;
    inst_type_t type = INST_TYPE_UNKNOWN;
    bool is_ld = false;
    bool is_st = false;

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

        std::cerr << "\n=== Instruction Debug ==="
         << "\nPC: 0x" << std::hex << curr_instr.ip
         << "\nIs Branch: " << std::dec << (int)curr_instr.is_branch
         << "\nBranch Type: " << RiscvBranchType_string()
         << "\nBranch Taken: " << (int)curr_instr.branch_taken
         #ifdef CHERI
         << "\nIs Capability Instruction: " << (int)curr_instr.is_cap
         #endif
         << "\nInstruction: 0x" << std::hex << decoded_instr.inst 
         << "\nInstruction Type: " << inst_type_to_str(type)
         << "\n\nDestination Registers ";
        

        // // Print destination registers
        for (const auto& regs : curr_instr.destination_registers) {
            if (regs)
                std::cerr << "\n Register ID " << std::dec << static_cast<int>(regs) <<  "\n";
            
        }
        
        std::cerr << "\nSource Registers: ";
        // Print source registers
        for (const auto& regs : curr_instr.source_registers) {
            if (regs)
                std::cerr << "\n Register ID " << std::dec << static_cast<int>(regs) << "\n";
                
        }
        
        std::cerr << "\n\nMemory Operands";
        // Print destination memory
        std::cerr << "\nDestination Memory:";
        for (const auto& mem : curr_instr.destination_memory) {
            if (mem)
                std::cerr << "\n Memory Address: 0x" << std::hex << mem << "\n";
            
        }
        
        // Print source memory
        std::cerr << "\nSource Memory:";
        for (const auto& mem : curr_instr.source_memory) {
            if (mem)
                std::cerr << "\n Memory Address: 0x" << std::hex << mem << "\n";
            
        }

    #ifdef CHERI
        if (curr_instr.is_cap) {
            std::cerr << "\nCapability Metadata: "
                    << " | Base 0x" << std::hex << curr_instr.base
                    << " | Length: 0x" << std::hex << curr_instr.length
                    << " | Offset: 0x" << std::hex << curr_instr.offset
                    << " | Perms: 0x" << std::hex << curr_instr.perms
                    << " | Tag:" << (int)curr_instr.tag;
        }
        std::cerr << "\n=========================\n" << std::dec << std::endl;
    }
    #endif
};


#ifdef CHERI
bool is_cap_instr(const cheri_trace_entry_t& entry) {

    switch(entry.entry_type) {
        // case CTE_CAP:
        case CTE_LD_CAP:
        case CTE_ST_CAP:
            return true;
        default: return false;
    }
    assert(false);
}
#endif


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
            return (dec.rd == RISCV_REG_ZERO) ? BRANCH_DIRECT_JUMP : BRANCH_DIRECT_CALL;
        
        case rv_op_jalr:
        case rv_op_c_jalr:
            if (dec.rd == RISCV_REG_ZERO)   
                return (dec.rs1 == RISCV_REG_RA) ? BRANCH_RETURN : BRANCH_INDIRECT;
            else return BRANCH_INDIRECT_CALL;

        // Pseudo jump instructions
        case rv_op_jr:
        case rv_op_c_jr:
            return (dec.rs1 == RISCV_REG_RA) ? BRANCH_RETURN : BRANCH_INDIRECT;
        case rv_op_ret:
            return BRANCH_RETURN;

        case rv_op_cjalr:
            return (dec.rd == RISCV_REG_ZERO) ? BRANCH_INDIRECT : BRANCH_INDIRECT_CALL;

        default: return ERROR;
    }
    
    assert(false); //shouldn't end up here.

}


void set_branch_data(InstructionTrace& trace, cheri_trace_entry_t& entry){

    switch (trace.branch_type)
    {

        case BRANCH_DIRECT_JUMP: //writes ip only
            trace.curr_instr.destination_registers[0] = champsim::REG_INSTRUCTION_POINTER;
            trace.curr_instr.branch_taken = true;
            break;

        case BRANCH_INDIRECT: //writes ip and reads other
            trace.curr_instr.branch_taken = true;
            trace.curr_instr.destination_registers[0] = champsim::REG_INSTRUCTION_POINTER;
            trace.curr_instr.source_registers[0] = remap_regid(trace.decoded_instr.rs1);
            break;

        case BRANCH_CONDITIONAL: //writes ip, reads ip and reads other
            trace.curr_instr.destination_registers[0] = champsim::REG_INSTRUCTION_POINTER;
            trace.curr_instr.source_registers[0] = champsim::REG_INSTRUCTION_POINTER;
            trace.curr_instr.source_registers[1] = remap_regid(trace.decoded_instr.rs1);
            if (trace.decoded_instr.rs2 != RISCV_REG_ZERO)
                trace.curr_instr.source_registers[2] = remap_regid(trace.decoded_instr.rs2);
            break;

        case BRANCH_DIRECT_CALL: //reads ip, writes ip: jal ra, offset
            trace.curr_instr.branch_taken = true;
            trace.curr_instr.destination_registers[0] = champsim::REG_INSTRUCTION_POINTER;
            trace.curr_instr.destination_registers[1] = remap_regid(trace.decoded_instr.rd);
            trace.curr_instr.source_registers[0] = champsim::REG_INSTRUCTION_POINTER;
            // trace.curr_instr.destination_registers[1] = champsim::REG_STACK_POINTER;
            // trace.curr_instr.source_registers[1] = champsim::REG_STACK_POINTER;
            break;

        case BRANCH_INDIRECT_CALL: //reads ip, writes ip and reads other jalr ra, rs1, imm
            trace.curr_instr.branch_taken = true;
            trace.curr_instr.destination_registers[0] = champsim::REG_INSTRUCTION_POINTER;
            trace.curr_instr.destination_registers[1] = remap_regid(trace.decoded_instr.rd);
            trace.curr_instr.source_registers[0] = champsim::REG_INSTRUCTION_POINTER;
            trace.curr_instr.source_registers[1] = remap_regid(trace.decoded_instr.rs1);
            // trace.curr_instr.destination_registers[1] = champsim::REG_STACK_POINTER;
            // trace.curr_instr.source_registers[1] = champsim::REG_STACK_POINTER;
            break;
        
        case BRANCH_RETURN: // writes ip, reads other
            trace.curr_instr.branch_taken = true;
            trace.curr_instr.destination_registers[0] = champsim::REG_INSTRUCTION_POINTER;
            trace.curr_instr.source_registers[0] = remap_regid(trace.decoded_instr.rs1);
            // trace.curr_instr.source_registers[0] = champsim::REG_STACK_POINTER;
            // trace.curr_instr.destination_registers[1] = champsim::REG_STACK_POINTER;
            break;

        case BRANCH_OTHER:
        case NOT_BRANCH:
        default: 
            assert(false && "Error: Unrecognized branch type\n");
    }

    #ifdef CHERI
    if (trace.curr_instr.is_cap) {
        if (entry.entry_type == CTE_LD_CAP) {
            trace.curr_instr.source_memory[0] = entry.val1;
        }

        else if (entry.entry_type == CTE_ST_CAP) {
            trace.curr_instr.destination_memory[0] = entry.val1;

        }


        trace.curr_instr.tag = (entry.val2 >> 63) & 0x1;
        trace.curr_instr.perms = (entry.val2 >> 1) & 0x7FFFFFFF;
        trace.curr_instr.length = entry.val5;
        trace.curr_instr.base = entry.val4;
        trace.curr_instr.offset = entry.val3 - entry.val4;
    }  
    #endif 
}

void set_mem_data(InstructionTrace& trace, const cheri_trace_entry_t& entry) {

    assert(entry.val1 != 0);
    uint8_t access_size = get_memory_access_size(trace.decoded_instr);
    uint64_t address = entry.val1;

    uint64_t cacheline_access_ini = address & ~63lu; //0xffffffffffffffc0
    uint64_t cacheline_access_end = (address + access_size -1) & ~63lu;
    
    

    if (trace.is_ld) {
        trace.curr_instr.destination_registers[0] = remap_regid(trace.decoded_instr.rd);
        trace.curr_instr.source_registers[0] = remap_regid(trace.decoded_instr.rs1);
        trace.curr_instr.source_memory[0] = address;
        if (cacheline_access_end != cacheline_access_ini)
            trace.curr_instr.source_memory[1] = cacheline_access_end;
    } else if (trace.is_st) {
        trace.curr_instr.source_registers[0]= remap_regid(trace.decoded_instr.rs1);
        trace.curr_instr.source_registers[1] = remap_regid(trace.decoded_instr.rs2);
        trace.curr_instr.destination_memory[0] = address;    
        if (cacheline_access_end != cacheline_access_ini)
            trace.curr_instr.destination_memory[1] = cacheline_access_end;
    }

#ifdef CHERI
    if (trace.curr_instr.is_cap ) {
        trace.curr_instr.tag = (entry.val2 >> 63) & 0x1;
        trace.curr_instr.perms = (entry.val2 >> 1) & 0x7FFFFFFF;
        trace.curr_instr.length = entry.val5;
        trace.curr_instr.base = entry.val4;
        trace.curr_instr.offset = entry.val3 - entry.val4;
    } 
#endif
}


void set_reg_data(InstructionTrace& trace)       
{
    int num_regs_used = count_register_operands(trace.decoded_instr.codec);
    assert(num_regs_used != 0 &&  num_regs_used != 55);
    if (trace.decoded_instr.rd == RISCV_REG_ZERO && trace.type != INST_TYPE_FP) {
        std::cerr << "Error: Instruction of type " << inst_type_to_str(trace.type) 
        << "is using the zero register as a destination\n";
        assert(false);
    }

    switch (num_regs_used)
    {

        case 1:
            trace.curr_instr.destination_registers[0] = remap_regid(trace.decoded_instr.rd);
            break;

        case 2:
            trace.curr_instr.destination_registers[0] = remap_regid(trace.decoded_instr.rd);
            trace.curr_instr.source_registers[0] = remap_regid(trace.decoded_instr.rs1);        
            break;

        case 3:
            trace.curr_instr.destination_registers[0] = remap_regid(trace.decoded_instr.rd);
            trace.curr_instr.source_registers[0] = remap_regid(trace.decoded_instr.rs1);
            trace.curr_instr.source_registers[1] = remap_regid(trace.decoded_instr.rs2);
            break;

        case 4:
            trace.curr_instr.destination_registers[0] = remap_regid(trace.decoded_instr.rd);
            trace.curr_instr.source_registers[0] = remap_regid(trace.decoded_instr.rs1);
            trace.curr_instr.source_registers[1] = remap_regid(trace.decoded_instr.rs2);
            trace.curr_instr.source_registers[2] = remap_regid(trace.decoded_instr.rs3);
            break;

        default:
            assert(false);
    }
}

void set_atomic_data(InstructionTrace& trace, cheri_trace_entry_t& entry)
{
    int num_regs_used = count_register_operands(trace.decoded_instr.codec);
    assert(num_regs_used != 0 && num_regs_used != 55);
    
    if (trace.decoded_instr.rd == 0) {
        std::cerr << "Error: Instruction of type " << inst_type_to_str(trace.type) 
        << "is using the zero register as a destination\n";
        assert(false);    
    }
    
    uint8_t access_size = get_memory_access_size(trace.decoded_instr);
    uint64_t address = entry.val1;

    uint64_t cacheline_access_ini = address & ~63lu; //0xffffffffffffffc0
    uint64_t cacheline_access_end = (address + access_size -1) & ~63lu;

    switch (num_regs_used)
    {
    case 2:
        trace.curr_instr.destination_registers[0] = remap_regid(trace.decoded_instr.rd);
        trace.curr_instr.source_registers[0] = remap_regid(trace.decoded_instr.rs1);
        trace.curr_instr.source_memory[0] = address;
        if (cacheline_access_end != cacheline_access_ini)
            trace.curr_instr.source_memory[1] = cacheline_access_end;
        break;

    case 3: 
        trace.curr_instr.destination_registers[0] = remap_regid(trace.decoded_instr.rd);
        trace.curr_instr.source_registers[0] = remap_regid(trace.decoded_instr.rs2);
        trace.curr_instr.source_registers[1] = remap_regid(trace.decoded_instr.rs1);
        trace.curr_instr.destination_memory[0] = address;
        if (cacheline_access_end != cacheline_access_ini)
            trace.curr_instr.destination_memory[1] = cacheline_access_end;
        break;

    default: 
        assert(false);
    }
}

void convert_cheri_trace_entry(cheri_trace_entry_t& entry, InstructionTrace& trace)
{
    trace.decoded_instr = disasm_inst(rv64, entry.pc, entry.inst, CHERI_CAP_MODE);

    assert(trace.decoded_instr.op != rv_op_illegal); 

    trace.curr_instr.is_branch = is_branch_instruction(trace);
    trace.curr_instr.ip = entry.pc;
    trace.type = classify_instruction(trace.decoded_instr);



    #ifdef CHERI
    trace.curr_instr.is_cap = is_cap_instr(entry) || (trace.type == INST_TYPE_CAP_LOAD ) || (trace.type == INST_TYPE_CAP_STORE);
    #endif

    if (trace.type == INST_TYPE_UNKNOWN) {
        printf("Error: Unknown instruction 0x%08lx detected at ip 0x%08lx\n", trace.decoded_instr.inst, trace.decoded_instr.pc);
        assert(false);
    }

    switch (trace.type)
    {

        case INST_TYPE_BRANCH:
            trace.branch_type = get_branch_type(trace.decoded_instr);
            assert(trace.branch_type != ERROR);
            set_branch_data(trace, entry);
            break;

        case INST_TYPE_LOAD:
        case INST_TYPE_CAP_LOAD:
        case INST_TYPE_FP_LOAD:
            trace.is_ld = true;
            set_mem_data(trace,entry);
            break;
            
        case INST_TYPE_STORE:
        case INST_TYPE_CAP_STORE: 
        case INST_TYPE_FP_STORE:
            trace.is_st = true;
            set_mem_data(trace,entry);
            break;

        case INST_TYPE_CAP_OP:
        case INST_TYPE_ALU:        
        case INST_TYPE_CSR:
        case INST_TYPE_FP:
            set_reg_data(trace);
            break;

        case INST_TYPE_ATOMIC:
            set_atomic_data(trace,entry);
            break;

        case INST_TYPE_SYSTEM: 
            break;
        case INST_TYPE_UNKNOWN:
        default:
            assert(false);       
    }

}

int main(int argc, char** argv) {
    if (argc < 3) {
        print_usage(argv[0]);
        return 1;
    }
    
    std::string input_file = argv[1];
    std::string base_output_file = argv[2];
    std::string sp_file;
    bool verbose = false, simpoint = false;
    uint64_t interval = 0;
    std::vector<uint64_t> simpoints;
    
    // Parse command line arguments
    for (int i = 1; i < argc; i++) {
        if(strcmp(argv[i], "-v") == 0) {
            verbose = true;
        } else if (strcmp(argv[i],"-s") == 0) {
            if (i + 2 < argc) {  // Need both file and interval
                simpoint = true;
                sp_file = argv[i+1];
                interval = std::strtoull(argv[i+2], nullptr, 10);
                i += 2;  // Skip the next two arguments
            } else {
                std::cerr << "Error: -s flag requires file path and interval\n";
                return 1;
            }
        }
    }
    
    // Load simpoints if specified
    if (simpoint) {
        std::ifstream sp(sp_file);
        if (sp) {
            std::string line;
            while(std::getline(sp, line)) {
                simpoints.push_back(std::stoul(line));
            }
            sp.close();
        } else {
            std::cerr << "Error: cannot open simpoints file at path " << sp_file << std::endl;
            return 1;
        }
        
        if (verbose) {
            std::cerr << "Simpoints loaded: ";
            for (auto v : simpoints) {
                std::cerr << v << " ";
            }
            std::cerr << std::endl;
        }
    }
    
    // Open input trace file
    std::ifstream input(input_file, std::ios::binary);
    if (!input) {
        std::cerr << "Error: Cannot open input file " << input_file << std::endl;
        return 1;
    }
    
    // Extract base name for output files (remove .champsimtrace or similar extension)
    std::string base_name = base_output_file;
    size_t dot_pos = base_name.rfind('.');
    if (dot_pos != std::string::npos) {
        base_name = base_name.substr(0, dot_pos);
    }
    

    cheri_trace_entry_t entry;
    InstructionTrace trace, prev_trace;
    uint64_t instruction_count = 0;
    uint64_t current_file_instructions = 0;
    bool pending_instr = false;
    size_t current_simpoint_idx = 0;
    
    // Open first output file
    std::ofstream output;
    std::string current_output_file;
    
    if (simpoint && !simpoints.empty()) {
        current_output_file = base_name + "-" + std::to_string(simpoints[0]) + ".champsimtrace";
        output.open(current_output_file, std::ios::binary);
        if (!output) {
            std::cerr << "Error: Cannot open output file " << current_output_file << std::endl;
            return 1;
        }
        if (verbose) {
            std::cerr << "Starting file: " << current_output_file << std::endl;
        }
    } else {
        output.open(base_output_file, std::ios::binary);
        if (!output) {
            std::cerr << "Error: Cannot open output file " << base_output_file << std::endl;
            return 1;
        }
    }
    
    while (input.read(reinterpret_cast<char*>(&entry), sizeof(cheri_trace_entry_t))) {
        // Byte swap
        entry.pc = bswap_64(entry.pc);
        entry.val1 = bswap_64(entry.val1);
        entry.val2 = bswap_64(entry.val2);
        entry.val3 = bswap_64(entry.val3);
        entry.val4 = bswap_64(entry.val4);
        entry.val5 = bswap_64(entry.val5);
        entry.inst = bswap_32(entry.inst);
        
        // Process current entry
        trace = InstructionTrace();
        convert_cheri_trace_entry(entry, trace);
        
        if (pending_instr) {
            // Handle conditional branch taken/not taken
            if (prev_trace.branch_type == BRANCH_CONDITIONAL) {
                uint64_t fall_through_pc = prev_trace.curr_instr.ip + 
                    444;
                prev_trace.curr_instr.branch_taken = (entry.pc != fall_through_pc);
            }
            
            // Check if we need to switch files BEFORE writing
            if (simpoint && current_file_instructions >= interval && current_simpoint_idx < simpoints.size()) {
                output.close();
                
                if (verbose) {
                    std::cerr << "Completed simpoint " << simpoints[current_simpoint_idx] 
                              << " with " << current_file_instructions << " instructions" << std::endl;
                }
                
                current_simpoint_idx++;
                
                if (current_simpoint_idx < simpoints.size()) {
                    current_output_file = base_name + "-" + std::to_string(simpoints[current_simpoint_idx]) + ".champsimtrace";
                    output.open(current_output_file, std::ios::binary);
                    
                    if (!output) {
                        std::cerr << "Error: Cannot open output file " << current_output_file << std::endl;
                        input.close();
                        return 1;
                    }
                    
                    if (verbose) {
                        std::cerr << "Starting file: " << current_output_file << std::endl;
                    }
                    
                    current_file_instructions = 0;
                } else {
                    // No more simpoints
                    if (verbose) {
                        std::cerr << "All simpoints processed" << std::endl;
                    }
                    break;
                }
            }
            
            // Write instruction if we still have an open file
            if (output.is_open()) {
                output.write(reinterpret_cast<const char*>(&prev_trace.curr_instr), sizeof(trace_instr_format));
                current_file_instructions++;
                
                if (verbose) {
                    prev_trace.debug_print_instruction();
                }
            }
        }
        
        prev_trace = trace;
        pending_instr = true;
        instruction_count++;
    }
    
    // Handle the last pending instruction
    if (pending_instr && output.is_open() && (!simpoint || current_file_instructions < interval)) {
        output.write(reinterpret_cast<const char*>(&trace.curr_instr), sizeof(trace_instr_format));
        current_file_instructions++;
    }
    
    input.close();
    if (output.is_open()) {
        output.close();
        if (verbose && simpoint) {
            std::cerr << "Final simpoint " << simpoints[current_simpoint_idx] 
                      << " with " << current_file_instructions << " instructions" << std::endl;
        }
    }
    
    std::cerr << "Processed " << instruction_count << " total instructions." << std::endl;
    if (simpoint) {
        std::cerr << "Created " << std::min(current_simpoint_idx + 1, simpoints.size()) << " simpoint trace files." << std::endl;
    }
    
    return 0;
}