#ifndef DEREF_CLASS_H
#define DEREF_CLASS_H

#include <algorithm>
#include <array>
#include <cstdint>
#include <unordered_map>

#include "cheri.h"
#include "trace_instruction.h"

namespace champsim
{
// Set by --deref-stats. Off: the classifier never runs, every load stays NOT_PTR and the table is not printed.
inline bool deref_stats_enabled = false;

// How a load's address was produced, decided from capability values in program order: a load whose authorising
// capability was itself loaded from memory goes through a loaded pointer.
//   NOT_PTR   the authority was never loaded from memory (stack root, fresh allocation, register-only values)
//   CAPTAB    the authority is a capability-table entry (loaded by a load whose authority covers its own PC):
//             a global-variable access, not a pointer dereference
//   INDEXED   the authority is a loaded pointer, and this PC's cursor offset from the loaded value changed since its
//             last execution: a walk (a[i], p++) through a loaded base
//   D1_STACK  the authority is a pointer loaded from the stack (spill reload or pointer-typed local), fixed offset
//   D1_MEM    the authority is a pointer loaded from a heap object or a global variable, fixed offset
//   CHAIN     the authority is a pointer that was itself loaded by a dereference (p->next->f), fixed offset
enum class deref_class : uint8_t { NOT_PTR = 0, CAPTAB, INDEXED, D1_STACK, D1_MEM, CHAIN, NUM_CLASSES };

inline constexpr std::array<const char*, static_cast<std::size_t>(deref_class::NUM_CLASSES)> deref_class_names{
    {"NOT_PTR", "CAPTAB", "INDEXED", "D1_STACK", "D1_MEM", "CHAIN"}};

inline constexpr std::array<deref_class, static_cast<std::size_t>(deref_class::NUM_CLASSES)> deref_class_all{
    {deref_class::NOT_PTR, deref_class::CAPTAB, deref_class::INDEXED, deref_class::D1_STACK, deref_class::D1_MEM, deref_class::CHAIN}};

class deref_tracker
{
  // Where a loaded pointer was loaded from
  enum class pointer_source : uint8_t { CAPTAB_ENTRY, STACK, MEM, DEREF };

  struct loaded_pointer {
    uint64_t value; // base + offset when loaded
    pointer_source source;
  };

  using object_key = std::pair<uint64_t, uint64_t>; // (base, length) of a capability

  struct object_key_hash {
    std::size_t operator()(const object_key& key) const { return std::hash<uint64_t>{}(key.first * 0x9e3779b97f4a7c15ULL ^ key.second); }
  };

  std::unordered_map<object_key, loaded_pointer, object_key_hash> loaded_pointers{}; // every capability loaded from memory
  std::unordered_map<uint64_t, int64_t> last_offset_at_pc{};                          // cursor - loaded value at each PC's last execution
  uint64_t stack_begin = 0, stack_end = 0;                                            // bounds of authorities used through sp

  static object_key key_of(const capability& cap) { return {cap.base.to<uint64_t>(), cap.length.to<uint64_t>()}; }

  bool on_stack(const capability& cap) const
  {
    return stack_begin < stack_end && cap.base.to<uint64_t>() >= stack_begin && cap.base.to<uint64_t>() < stack_end;
  }

public:
  // Observe one instruction in program order, before any register-list rewriting. Returns the class of its loads
  // (NOT_PTR for instructions without loads). The register list is read only to learn the stack's bounds.
  template <typename INSTR>
  deref_class observe(const INSTR& instr)
  {
    const capability& authority = instr.auth_cap;
    const bool accesses_memory = !std::empty(instr.source_memory) || !std::empty(instr.destination_memory);
    const bool through_sp = std::any_of(std::begin(instr.source_registers), std::end(instr.source_registers),
                                        [](auto reg) { return reg == REG_STACK_POINTER; });
    if (accesses_memory && authority.tag && through_sp) {
      const uint64_t authority_begin = authority.base.to<uint64_t>();
      const uint64_t authority_end = authority_begin + authority.length.to<uint64_t>();
      stack_begin = (stack_begin < stack_end) ? std::min(stack_begin, authority_begin) : authority_begin;
      stack_end = std::max(stack_end, authority_end);
    }

    if (std::empty(instr.source_memory) || !authority.tag)
      return deref_class::NOT_PTR;

    const uint64_t pc = champsim::address{instr.ip}.to<uint64_t>();
    deref_class result = deref_class::NOT_PTR;
    if (auto pointer_entry = loaded_pointers.find(key_of(authority)); pointer_entry != loaded_pointers.end()) {
      const loaded_pointer& pointer = pointer_entry->second;
      const auto cursor = authority.base.to<uint64_t>() + authority.offset.to<uint64_t>();
      const auto offset = static_cast<int64_t>(cursor - pointer.value);
      auto [pc_entry, first_execution] = last_offset_at_pc.try_emplace(pc, offset);
      if (!first_execution && pc_entry->second != offset)
        result = deref_class::INDEXED;
      else if (pointer.source == pointer_source::CAPTAB_ENTRY)
        result = deref_class::CAPTAB;
      else if (pointer.source == pointer_source::STACK)
        result = deref_class::D1_STACK;
      else if (pointer.source == pointer_source::MEM)
        result = deref_class::D1_MEM;
      else
        result = deref_class::CHAIN;
      pc_entry->second = offset;
    }

    const capability& loaded_cap = instr.transferred_cap;
    if (has_transferred(instr.cap_op) && loaded_cap.tag) {
      pointer_source source = pointer_source::MEM; // from a heap object, a global variable (CAPTAB) or an array element (INDEXED)
      const uint64_t authority_begin = authority.base.to<uint64_t>();
      if (pc >= authority_begin && pc - authority_begin < authority.length.to<uint64_t>())
        source = pointer_source::CAPTAB_ENTRY; // the authority spans the code: derived from PCC, so this is the capability table
      else if (result == deref_class::D1_STACK || result == deref_class::D1_MEM || result == deref_class::CHAIN)
        source = pointer_source::DEREF;
      else if (result == deref_class::NOT_PTR && on_stack(authority))
        source = pointer_source::STACK;
      const uint64_t pointer_value = loaded_cap.base.to<uint64_t>() + loaded_cap.offset.to<uint64_t>();
      loaded_pointers.insert_or_assign(key_of(loaded_cap), loaded_pointer{pointer_value, source});
    }

    return result;
  }
};
} // namespace champsim

#endif
