/*
 *    Copyright 2023 The ChampSim Contributors
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

#ifndef TRACE_INSTRUCTION_H
#define TRACE_INSTRUCTION_H
#include <limits>


#ifdef CHERI
#include "/home/charles-williams/Documents/CHERI/CheriTrace/cheri-compressed-cap/cheri_compressed_cap.h"
#endif
// special registers that help us identify branches
namespace champsim
{
constexpr char REG_STACK_POINTER = 6;
constexpr char REG_FLAGS = 25;
constexpr char REG_INSTRUCTION_POINTER = 26;

#ifdef CHERI
constexpr char SCR1_REG = 70;
#endif
} // namespace champsim

// instruction format
constexpr std::size_t NUM_INSTR_DESTINATIONS_SPARC = 4;
constexpr std::size_t NUM_INSTR_DESTINATIONS = 2;
constexpr std::size_t NUM_INSTR_SOURCES = 4;


#ifdef CHERI

constexpr std::size_t CAP_INSTR_MASK = 0x1;
constexpr std::size_t CAP_SRC_MASK = 0x2;
constexpr std::size_t CAP_DEST_MASK = 0x4;
constexpr std::size_t CAP_MEM_MASK = 0x8;

struct capability_metadata
{
  unsigned long long pesbt, cursor;
  unsigned char tag;
};
#endif

// NOLINTBEGIN(cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays): These classes are deliberately trivial
struct input_instr {
  // instruction pointer or PC (Program Counter)
  unsigned long long ip;

  // branch info
  unsigned char is_branch;
  unsigned char branch_taken;

  unsigned char destination_registers[NUM_INSTR_DESTINATIONS]; // output registers
  unsigned char source_registers[NUM_INSTR_SOURCES];           // input registers
#ifdef CHERI
  unsigned char is_cap;
  // Memory operands with capability metadata (address = cap.base + cap.offset)
  struct {
    unsigned long long address;
    struct capability_metadata cap;
  } destination_memory[NUM_INSTR_DESTINATIONS], source_memory[NUM_INSTR_SOURCES];
  
#else
  unsigned long long destination_memory[NUM_INSTR_DESTINATIONS]; // output memory
  unsigned long long source_memory[NUM_INSTR_SOURCES];           // input memory

#endif
};

struct cloudsuite_instr {
  // instruction pointer or PC (Program Counter)
  unsigned long long ip;

  // branch info
  unsigned char is_branch;
  unsigned char branch_taken;
  unsigned char asid[2];


  unsigned char destination_registers[NUM_INSTR_DESTINATIONS_SPARC]; // output registers
  unsigned char source_registers[NUM_INSTR_SOURCES];                 // input registers

#ifdef CHERI



  // Memory operands with capability metadata (address = cap.base + cap.offset)
  struct {
    capability_metadata cap;
    unsigned long long address;
  } destination_memory[NUM_INSTR_DESTINATIONS_SPARC], source_memory[NUM_INSTR_SOURCES];
#else

  unsigned long long destination_memory[NUM_INSTR_DESTINATIONS_SPARC]; // output memory
  unsigned long long source_memory[NUM_INSTR_SOURCES];                 // input memory

#endif
};
// NOLINTEND(cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays)

#endif
