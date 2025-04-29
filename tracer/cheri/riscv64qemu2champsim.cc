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
#include <vector>
#include <re2/re2.h>

#include "../../inc/trace_instruction.h"

using trace_instr_format = input_instr;
namespace 
{
  constexpr char REG_AX = 56;  
} 

// Regex patterns 
// const re2::RE2 instr_pattern(R"(\[\d+:\d+\]\s+(0x[0-9a-fA-F]+):\s+([0-9a-fA-F]+)\s+([\w\.]+)\s*(.*))");
const re2::RE2 instr_pattern("\\[(\\d+:\\d+)\\]\\s+(0x[0-9a-fA-F]+):\\s+([0-9a-fA-F]+)\\s+([\\w\\.]+)\\s+(.*?)(?:\\s+#(0x[0-9a-fA-F]+))?$");
const re2::RE2 reg_write_pattern(R"(Write (x\d+)/\w+ = ([0-9a-fA-F]+))");
    // Patterns for different instruction formats
const re2::RE2 load_store_pattern("([a-z][0-9a-z]+),\\s*(-?\\d+)\\(([a-z][0-9a-z]+)\\)"); // lw x1, 8(x2)
const re2::RE2 mem_pattern("Memory (Read|Write) \\[([0-9A-Fa-f]+)\\] = ([0-9A-Fa-f]+)");
// All standard RISC-V load instructions (excluding capability loads)
const std::unordered_set<std::string> load_instructions = {
    // Standard integer loads
    "lb", "lh", "lw", "ld",
    // Unsigned loads
    "lbu", "lhu", "lwu",

    "cld",
    // Atomic load-reserve instructions with various memory orderings
    "lr.b", "lr.h", "lr.w", "lr.d",
    "lr.b.acq", "lr.h.acq", "lr.w.acq", "lr.d.acq",
    "lr.b.rel", "lr.h.rel", "lr.w.rel", "lr.d.rel",
    "lr.b.aqrl", "lr.h.aqrl", "lr.w.aqrl", "lr.d.aqrl",
    // Floating-point loads
    "flw", "flh", "fld", "flq"
};

// All standard RISC-V store instructions (excluding capability stores)
const std::unordered_set<std::string> store_instructions = {
    // Standard integer stores
    "sb", "sh", "sw", "sd","csd",
    // Atomic store-conditional instructions with various memory orderings
    "sc.b", "sc.h", "sc.w", "sc.d",
    "sc.b.rel", "sc.h.rel", "sc.w.rel", "sc.d.rel",
    "sc.b.aqrl", "sc.h.aqrl", "sc.w.aqrl", "sc.d.aqrl",
    // Floating-point stores
    "fsw", "fsh", "fsd", "fsq"
};

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
   "([a-z][0-9a-z]+)\\s*,\\s*(-?\\d+|[a-z][0-9a-z]+)(?:$|\\s)|"
    // Format 3: immediate(src) - supports store/load instructions like sw rs2, offset(rs1)
   "(-?\\d+)\\s*\\(\\s*([a-z][0-9a-z]+)\\s*\\)|"
    // Format 4: single src register (move instr)
   "([a-z][0-9a-z]+)|"
    // Format 5: src1, src2, immediate (for jalr-like instructions and alternative store format)
   "([a-z][0-9a-z]+)\\s*,\\s*([a-z][0-9a-z]+)\\s*,\\s*(-?\\d+)|"
    // Format 6: single register, immediate
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
    BRANCH_JALR,             // JALR is context dependent
    BRANCH_OTHER,            // Other types of branches
    NOT_BRANCH,              // Not a branch instruction
    ERROR
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
    
    // Indirect jumps and calls (I-type/JALR)
    // {"jalr", BRANCH_INDIRECT_CALL},    // Usually a call, but context-dependent
    {"jr", BRANCH_INDIRECT},           // Pseudo-instruction (jalr x0, rs, 0)
    {"jalr", BRANCH_JALR}, // CHERI capability call
  
    // Return instructions
    {"ret", BRANCH_RETURN}             // Pseudo-instruction (jalr x0, ra, 0)
};

const std::unordered_map<std::string, int> REG_MAP = {
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


    //compressed registers
    {"ca0", 0}, {"ca1", 1}, {"ca2", 2}, {"ca3", 3},
    {"ca4", 4}, {"ca5", 5}, {"ca6", 6}, {"ca7", 7},
    {"cs0", 16}, {"cs1", 17}, {"cs2", 18}, {"cs3", 19},
    {"cs4", 20}, {"cs5", 21}, {"cs6", 22}, {"cs7", 23},
    {"cs8", 24}, {"cs9", 25}, {"cs10", 26}, {"cs11", 27},
    {"cs12", 28}, {"cs13", 29}, {"cs14", 30}, {"cs15", 31},


    {"cnull", 0}, 
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
    {"cs10", 26}, {"cs11", 27}, {"ct3", 28}, {"ct4", 29}, {"ct5", 30}, {"ct6", 31}
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

struct ProgramTrace {

    trace_instr_format curr_instr;
    bool pending_instr = false;
    uint64_t target_addr = 0;
    std::string mnemonic, curr_mnemonic;
    RiscvBranchType branchType = NOT_BRANCH;
    bool verbose;    

    void clear() {

        pending_instr = false;
        target_addr = 0;
        branchType  = NOT_BRANCH;
        
        curr_instr = trace_instr_format();
    }

    void debug_print_instruction() const {

        std::cout << "\n=== Instruction Debug ==="
         << "\nInstruction: " << curr_mnemonic 
         << "\nPC: 0x" << std::hex << curr_instr.ip
         << "\nIs Branch: " << std::dec << (int)curr_instr.is_branch
         << "\nBranch Type: " << RiscvBranchType_string()
         << "\nBranch Taken: " << (int)curr_instr.branch_taken
         << "\n\nDestination Registers: ";
        
        // // Print destination registers
        for (const auto& regs : curr_instr.destination_registers)
            if (regs)
                std::cout << "\n Register Number " << std::dec << static_cast<int>(regs);
        
        
        std::cout << "\nSource Registers: ";
        // Print source registers
        for (const auto& regs : curr_instr.source_registers) 
            if (regs)
                std::cout << "\n Register Number " << std::dec << static_cast<int>(regs); 
                
        
        
        std::cout << "\n\nMemory Operands:";
        // Print destination memory
        std::cout << "\nDestination Memory:";
        for (const auto& addr : curr_instr.destination_memory) 
                std::cout << "\n Memory Address: 0x" << std::hex << addr;

        
        // Print source memory
        std::cout << "\nSource Memory:";
        for (const auto& addr : curr_instr.source_memory) 
                std::cout << "\n Memory Address: 0x" << std::hex << addr;
            
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
            case BRANCH_JALR : return "Branch jalr";
            case BRANCH_OTHER : return "Branch Other";
            case NOT_BRANCH : return "Not a Branch";
            case ERROR : assert(false); //shouldn't end up here...
        }
        return "";
    }

};


void process_branch(ProgramTrace& trace, std::string register_ops)
{

    
    RiscvBranchType bType = (CONTROL_FLOW_INST.count(trace.mnemonic)  > 0) ? CONTROL_FLOW_INST.at(trace.mnemonic) : NOT_BRANCH;            
    trace.curr_instr.is_branch = bType != NOT_BRANCH;
    trace.branchType = bType;


    switch (trace.branchType) {
        //writes IP only

        case BRANCH_DIRECT_JUMP: {//gg
            trace.curr_instr.destination_registers[0] = champsim::REG_INSTRUCTION_POINTER;
            trace.curr_instr.branch_taken = true;
            break;
        }
    
        //read something else, write ip
        case BRANCH_INDIRECT: { //gg
            trace.curr_instr.branch_taken = true;
            trace.curr_instr.destination_registers[0] = champsim::REG_INSTRUCTION_POINTER;
            trace.curr_instr.source_registers[0] = ::REG_AX;
            break;
        }

        // reads other, writes and reads ip
        case BRANCH_CONDITIONAL: { //gg
            trace.curr_instr.destination_registers[0] = champsim::REG_INSTRUCTION_POINTER;
            trace.curr_instr.source_registers[0] = champsim::REG_INSTRUCTION_POINTER;
            trace.curr_instr.source_registers[1] = ::REG_AX;
            break;
        }
    
        //reads ip, sp, writes sp,ip
        case BRANCH_DIRECT_CALL: { //gg
            trace.curr_instr.branch_taken = true;
            trace.curr_instr.destination_registers[0]= champsim::REG_INSTRUCTION_POINTER;
            trace.curr_instr.destination_registers[1] = champsim::REG_STACK_POINTER;
            trace.curr_instr.source_registers[0]= champsim::REG_INSTRUCTION_POINTER;
            trace.curr_instr.source_registers[1] = champsim::REG_STACK_POINTER;
            break;
        }
        // reads other, ip, sp, writes sp and ip
        case BRANCH_INDIRECT_CALL: { //this will never reach
            
            trace.curr_instr.branch_taken = true;
            trace.curr_instr.destination_registers[0] = champsim::REG_INSTRUCTION_POINTER;
            trace.curr_instr.destination_registers[1] = champsim::REG_STACK_POINTER;
            trace.curr_instr.source_registers[0] = champsim::REG_INSTRUCTION_POINTER;
            trace.curr_instr.source_registers[1] = champsim::REG_STACK_POINTER;
            trace.curr_instr.source_registers[2] = ::REG_AX;
            break;
        }
        // reads sp, writes sp,ip
        case BRANCH_RETURN:  { //gg
            trace.curr_instr.branch_taken = true;
            trace.curr_instr.source_registers[0] = champsim::REG_STACK_POINTER;
            trace.curr_instr.destination_registers[0] = champsim::REG_INSTRUCTION_POINTER;
            trace.curr_instr.destination_registers[1] = champsim::REG_STACK_POINTER;

            break;
        }

        case BRANCH_JALR: { //JALR is context dependent, gg
            trace.curr_instr.branch_taken = true;
            std::size_t first_comma = register_ops.find(',');
            std::string first_reg = register_ops.substr(0,first_comma);
            if(REG_MAP.at(first_reg) == 0) {  
                std::size_t second_comma = register_ops.find(',', first_comma + 1);
                std::string second_reg = register_ops.substr(first_comma+1, second_comma - (first_comma + 1));

                if (REG_MAP.at(second_reg) != 1) { //indirect jmp 
                    trace.curr_instr.destination_registers[0] = champsim::REG_INSTRUCTION_POINTER;
                    trace.curr_instr.source_registers[0] = ::REG_AX;
                    trace.branchType = BRANCH_INDIRECT;
                    break;
                }

                else { //return
                    trace.curr_instr.source_registers[0] = champsim::REG_STACK_POINTER;
                    trace.curr_instr.destination_registers[0] = champsim::REG_INSTRUCTION_POINTER;
                    trace.curr_instr.destination_registers[1] = champsim::REG_STACK_POINTER;
                    trace.branchType = BRANCH_RETURN;
                    break;
                }
            } else  { //indirect call       
                trace.curr_instr.destination_registers[0] = champsim::REG_INSTRUCTION_POINTER;
                trace.curr_instr.destination_registers[1] = champsim::REG_STACK_POINTER;
                trace.curr_instr.source_registers[0] = champsim::REG_INSTRUCTION_POINTER;
                trace.curr_instr.source_registers[1] = champsim::REG_STACK_POINTER;
                trace.curr_instr.source_registers[2] = ::REG_AX;
                trace.branchType = BRANCH_INDIRECT_CALL;
                break;
            }
            break;
        }
        default: assert(false);
    }
}

void process_store(ProgramTrace& trace, std::string& operands)
{
    std::string r1,r2,offset;
    try { 
        if(RE2::PartialMatch(operands,load_store_pattern,&r1,&offset,&r2)) {

            trace.curr_instr.source_registers[0] = remap_regid(REG_MAP.at(r1));
            trace.curr_instr.source_registers[1] = remap_regid(REG_MAP.at(r2));
        }
    }
    catch (const std::out_of_range&) {
        std::cerr << "process_store: out of range error: " << r1 << " " << r2 << std::endl;
    }
}

void process_load(ProgramTrace& trace, std::string& operands)
{

    std::string r1,r2,offset;
    try {
        if(RE2::PartialMatch(operands,load_store_pattern,&r1,&offset,&r2)) {
            trace.curr_instr.destination_registers[0] = remap_regid(REG_MAP.at(r1));
            trace.curr_instr.source_registers[0] = remap_regid(REG_MAP.at(r2));
        }
    } 
    catch (const std::out_of_range&) {
        std::cerr << "process_load: out of range error: " << r1 << " " << r2 << std::endl;
    } 
}


void process_int_fp_instr(ProgramTrace& trace, std::string& operands) 
{
    boost::smatch m;

    if(boost::regex_search(operands, m, registerPattern)) {
        if(m[2].matched && m[3].matched && m[4].matched) {
            std::string src1, src2, src3;
            src1 = m[2].str(); src2 = m[3].str(); src3 = m[4].str();

            trace.curr_instr.source_registers[0] = remap_regid(REG_MAP.at(src1));             
            trace.curr_instr.source_registers[1] = remap_regid(REG_MAP.at(src2));
            trace.curr_instr.source_registers[2] = remap_regid(REG_MAP.at(src3));
        }

        else if (m[5].matched) {
            std::string src1 = m[5].str();
            trace.curr_instr.source_registers[0] = remap_regid(REG_MAP.at(src1));

            std::string src2 = m[6].str();
            if (!src2.empty() && (std::isalpha(static_cast<unsigned char>(src2[0])))) {
                trace.curr_instr.source_registers[1] = remap_regid(REG_MAP.at(src2));
            }

        }

        else if (m[8].matched) {
            trace.curr_instr.source_registers[0] = remap_regid(REG_MAP.at(m[8].str()));
        }  

        else if (m[9].matched) {
            trace.curr_instr.source_registers[0] = remap_regid(REG_MAP.at(m[9].str()));
        }  
    }
}


void write_instr(ProgramTrace& trace, std::ofstream& out) {
    

    if (trace.branchType == BRANCH_CONDITIONAL) 
        trace.curr_instr.branch_taken = (trace.curr_instr.ip +4 ) != trace.target_addr;

    
    if (trace.verbose) trace.debug_print_instruction();

    out.write(reinterpret_cast<const char*>(&trace.curr_instr),sizeof(trace_instr_format));
    trace.clear();
}


bool parse_trace(std::string& filepath, std::string& ofile, ProgramTrace& trace)
{
    trace.clear();
    std::string line;
    std::ifstream trace_file(filepath);
    if (!trace_file.is_open()) {
        std::cerr << "Error: Trace file at path " << filepath << " could not be opened.\n";
        return false;
    }

    std::ofstream trace_outFile(ofile, std::ios::binary);
    if(!trace_outFile.is_open())
    {
        std::cerr << "Error: Could not open output file " << ofile << std::endl;
        exit(EXIT_FAILURE);
    }

    
    while (std::getline(trace_file,line)) {
        std::string cpu, pc, opcode, mnemonic, reg_operands;
        std::string mem_op, mem_addr, val;

  
        if (RE2::PartialMatch(line, instr_pattern, &cpu, &pc, &opcode, &mnemonic, &reg_operands)) {
            trace.mnemonic = mnemonic;
            if (trace.mnemonic == "fence" || trace.mnemonic == "csrrs" || trace.mnemonic == "illegal") continue;

            trace.curr_instr.ip = std::stoull(pc,nullptr,0x10);

            if (trace.pending_instr) {
                write_instr(trace,trace_outFile);
            }

            trace.pending_instr = true;
            trace.curr_mnemonic = trace.mnemonic;
            trace.mnemonic = mnemonic;
            if (CONTROL_FLOW_INST.count(mnemonic) > 0) { //branch instruction 
                std::size_t pos = reg_operands.find('#');

                if (pos != std::string::npos) 
                    trace.target_addr = std::stoull(reg_operands.substr(pos+2),nullptr,0x10);
            
                process_branch(trace,reg_operands);
            } 
            else if (store_instructions.count(mnemonic)){

                process_store(trace, reg_operands);

            }

            else if (load_instructions.count(mnemonic)) {

                process_load(trace, reg_operands);
            }

            else {
                process_int_fp_instr(trace,reg_operands);
            }      
        }

        else if (RE2::PartialMatch(line, mem_pattern, &mem_op, &mem_addr, &val)) {
            if (mem_op == "Write")
                trace.curr_instr.destination_memory[0] = std::stoull(mem_addr,nullptr,0x10);

            else if (mem_op == "Read")
                trace.curr_instr.source_memory[0] = std::stoull(mem_addr,nullptr,0x10);
        }
    }
    return true;
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



    ProgramTrace trace;
    trace.verbose = (argc > 3 && std::string(argv[3]) == "-v");

    parse_trace(fileName,oFilename, trace);
}