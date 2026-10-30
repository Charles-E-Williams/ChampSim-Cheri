#ifndef CHERI_TDP_H
#define CHERI_TDP_H

// Tag-directed prefetching (TDP) as an engine any L1D prefetcher can embed, and the core of the standalone `tdp`
// module in this directory. CHERI tags mark the pointer slots of every line arriving in L1D; each pointer is a
// sighting, recorded in the Capability History Table (CHT) and looked up in the Pointer Delta Table (PDT), which holds
// per key Berti-style epoch confidences of "dereferenced line - pointer's cursor line"; confident deltas are
// prefetched. A tagged demand load that misses (or hits a TDP-prefetched line) and whose authorizing capability is a
// recorded pointer trains its delta.
//
// Recursion (max_depth > 1): the pointers in a line TDP prefetched are scanned when it fills, keyed by the field they
// sit in and the path that led there (a hash of the parent's key, the pointed-to object's length and the line's
// offset in it, and the slot). Until that key has a confident delta, the parent's deltas stand in for it, discounted
// by the path probability; a trained key uses its own rate. Chains stop when confidence runs out, or at max_depth.
//
// Per-host settings (the defaults are the depth-1 engine that ip_stride_cheri_tdp and ipcp_cap_tdp were built with):
//   max_depth      1 = no recursion; up to 7 (the depth field's maximum)
//   berti_ip_key   depth-0 key: false = 11-bit hash of the IP; true = Berti's 16-bit IP hash
//   recent_filter  true = skip targets among the last 32 issued; false = leave duplicates to the cache's tag and MSHR check
//
// Host prefetcher (at a virtual_prefetch L1D):
//   cache_operate: engine.on_operate(...) for every access, and OR engine.tag_prefetch(ip) into the metadata of the
//                  host's own prefetches (so the lines they bring in are scanned under the trigger's key);
//   cache_fill:    engine.on_fill(...), and keep the bits in cheri_tdp::MD_MASK in the metadata the hook returns;
//   cycle_operate: engine.on_cycle(*this).
// Metadata of TDP's own prefetches:
//   max_depth 1:   [19] TDP prefetch, [20] key valid, [31:21] key. The host must leave bits 19-31 free.
//   max_depth > 1: [15:0] line key, [18:16] depth, [19] TDP prefetch, [24:20] probability that the line is used
//                  (1/16, 0-16), [30:25] parent's PDT slot, [31] parent slot valid. The host must leave TDP's
//                  prefetches' metadata alone (all 32 bits).

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <iostream>
#include <optional>
#include <unordered_map>

#include "cache.h"
#include "capability_memory.h"
#include "cheri_prefetch_utils.h"
#include "modules.h"

namespace cheri_tdp
{
constexpr std::size_t CHT_SIZE = 128;          // Capability History Table entries (FIFO)
constexpr std::size_t PDT_SIZE = 64;           // Pointer Delta Table entries (fully associative, LRU)
constexpr std::size_t PDT_DELTAS = 4;          // deltas per PDT entry
constexpr std::size_t SIGHTING_QUEUE = 8;      // sighting queue entries (oldest dropped when full)
constexpr unsigned SIGHTINGS_PER_CYCLE = 2;    // sightings processed per cycle
constexpr unsigned EPOCH = 16;                 // sightings per confidence epoch
constexpr unsigned THRESHOLD_L1D = 8;          // dereferences per epoch for an L1D-level delta
constexpr unsigned THRESHOLD_L2 = 4;           // for an L2-level delta
constexpr int MAX_LINE_DELTA = 7;              // |dereferenced line - cursor line| learned
constexpr std::size_t RECENT_TARGETS = 32;     // recently issued TDP targets (recent_filter)
constexpr double MSHR_LIMIT = 0.70;            // L1D-level TDP prefetches only below this MSHR occupancy (as Berti)
constexpr unsigned DEPTH_FIELD_MAX = 7;        // largest depth the 3-bit field holds

// Metadata shared by both layouts: [19] TDP prefetch. Depth-1 layout and host prefetch tags: [20] key valid, [31:21] key.
constexpr uint32_t MD_TDP = 1u << 19;
constexpr uint32_t MD_VALID = 1u << 20;
constexpr unsigned MD_KEY_SHIFT = 21;
constexpr uint32_t KEY_MASK = 0x7FF;
constexpr uint32_t MD_MASK = MD_TDP | MD_VALID | (KEY_MASK << MD_KEY_SHIFT);
// Recursion layout of TDP's own prefetches (max_depth > 1)
constexpr uint32_t MD_LINE_KEY_MASK = 0xFFFF;
constexpr unsigned MD_DEPTH_SHIFT = 16, MD_PROB_SHIFT = 20, MD_PARENT_SHIFT = 25;
constexpr uint32_t MD_PARENT_VALID = 1u << 31;
static_assert(PDT_SIZE <= 64, "the parent's PDT slot has 6 metadata bits");

struct settings {
  unsigned max_depth = 1;    // 1 = no recursion
  bool berti_ip_key = false; // depth-0 key: false = 11-bit IP hash, true = Berti's 16-bit IP hash
  bool recent_filter = true; // skip targets among the last RECENT_TARGETS issued
};

inline uint16_t hash16(uint64_t value)
{
  value ^= value >> 33;
  value *= 0xff51afd7ed558ccdULL;
  value ^= value >> 29;
  return static_cast<uint16_t>(value ^ (value >> 16) ^ (value >> 32) ^ (value >> 48));
}
inline uint16_t ip_key(uint64_t ip) { return static_cast<uint16_t>(hash16(ip) & KEY_MASK); }
inline uint16_t berti_ip_hash(uint64_t ip) { return static_cast<uint16_t>(ip ^ (ip >> 2) ^ (ip >> 5)); } // Berti's ENTANGLING_HASH, 16 bits

class engine
{
  enum { SRC_DEMAND_FILL, SRC_HOST_PREFETCH_FILL, SRC_TDP_FILL, NUM_SRC };

  struct cht_entry {
    bool valid = false;
    uint16_t base_hash = 0;   // hash of the pointer's base: names the pointed-to object
    uint64_t cursor_line = 0; // the pointer's cursor line
    uint16_t key = 0;         // PDT key
    uint16_t trained = 0;     // deltas already credited for this sighting, one bit per delta
  };
  struct pdt_delta {
    bool valid = false;
    int8_t delta = 0;
    uint8_t conf = 0;      // dereferences this epoch
    uint8_t status = 0;    // 0 none, 1 prefetch to L2, 2 prefetch to L1D (set at the end of the previous epoch)
    uint8_t last_conf = 0; // dereferences in the previous epoch (recursion: the delta's rate)
  };
  struct pdt_entry {
    bool valid = false;
    uint16_t tag = 0;
    uint8_t sightings = 0;
    uint64_t lru = 0;
    std::array<pdt_delta, PDT_DELTAS> deltas{};
  };
  struct sighting {
    champsim::capability cap;
    uint16_t key;
    uint64_t own_line;            // the line the pointer was read from
    uint8_t source;
    uint8_t depth = 0;            // 0 = in a demand or host-prefetch fill; d >= 1 = in a TDP fill of depth d
    uint8_t prob = 16;            // probability (1/16) that the line holding it is used
    int parent_slot = -1;         // depth >= 1: the PDT slot of the sighting the line was fetched for (fallback deltas)
    int parent_tag_at_issue = -1; // statistics only: that slot's tag when the line was prefetched
  };

  settings config;
  std::array<cht_entry, CHT_SIZE> cht{};
  std::size_t cht_next = 0;
  std::array<pdt_entry, PDT_SIZE> pdt{};
  uint64_t pdt_clock = 0;
  std::deque<sighting> sighting_queue;
  uint64_t budget_cycle = ~uint64_t{0};
  unsigned budget_used = 0;
  std::array<uint64_t, RECENT_TARGETS> recent_targets{};
  std::size_t recent_next = 0;

  // Statistics (not hardware), region of interest only
  struct stats {
    uint64_t lines_scanned[NUM_SRC] = {}, sightings[NUM_SRC] = {};
    uint64_t duplicate = 0, queued = 0, queue_dropped = 0;
    uint64_t train = 0, tagged_load_miss = 0;
    uint64_t issued_l1d = 0, issued_l2 = 0, pq_full = 0, recent_skip = 0;
    uint64_t fills = 0, useful = 0, useless = 0;
    uint64_t depth_stop = 0, low_prob = 0, fallback = 0, fallback_reused_slot = 0;
    uint64_t by_depth_issued[8][2] = {}, by_depth_fills[8] = {}, by_depth_useful[8] = {}, by_depth_useless[8] = {}; // [.][0] L2, [.][1] L1D
  } st;
  bool in_warmup = true;
  std::unordered_map<uint64_t, uint16_t> parent_tag_at_issue; // statistics only: parent slot's tag when each line was prefetched

  bool recursive() const { return config.max_depth > 1; }
  static unsigned md_depth(uint32_t metadata) { return (metadata >> MD_DEPTH_SHIFT) & 0x7; }
  static unsigned md_prob(uint32_t metadata) { return (metadata >> MD_PROB_SHIFT) & 0x1F; }
  static int md_parent_slot(uint32_t metadata) { return (metadata & MD_PARENT_VALID) ? static_cast<int>((metadata >> MD_PARENT_SHIFT) & 0x3F) : -1; }
  static uint32_t md_make_chain(uint16_t line_key, unsigned depth, unsigned prob, int parent_slot)
  {
    uint32_t metadata = line_key | MD_TDP | (std::min(depth, DEPTH_FIELD_MAX) << MD_DEPTH_SHIFT) | (std::min(prob, 16u) << MD_PROB_SHIFT);
    if (parent_slot >= 0)
      metadata |= MD_PARENT_VALID | (static_cast<uint32_t>(parent_slot) << MD_PARENT_SHIFT);
    return metadata;
  }

  pdt_entry* pdt_find(uint16_t tag)
  {
    for (auto& entry : pdt)
      if (entry.valid && entry.tag == tag) {
        entry.lru = ++pdt_clock;
        return &entry;
      }
    return nullptr;
  }

  pdt_entry* pdt_alloc(uint16_t tag)
  {
    if (auto* entry = pdt_find(tag))
      return entry;
    auto victim = std::min_element(pdt.begin(), pdt.end(), [](const auto& lhs, const auto& rhs) { return lhs.valid != rhs.valid ? !lhs.valid : lhs.lru < rhs.lru; });
    *victim = pdt_entry{};
    victim->valid = true;
    victim->tag = tag;
    victim->lru = ++pdt_clock;
    return &*victim;
  }

  // One more sighting under this key; at the end of an epoch every delta gets its prefetch level from its confidence.
  void pdt_count_sighting(pdt_entry& entry)
  {
    if (++entry.sightings < EPOCH)
      return;
    for (auto& delta : entry.deltas) {
      if (!delta.valid)
        continue;
      delta.status = (delta.conf >= THRESHOLD_L1D) ? 2 : (delta.conf >= THRESHOLD_L2) ? 1 : 0;
      delta.last_conf = delta.conf;
      delta.conf = 0;
    }
    entry.sightings = 0;
  }

  void pdt_train(uint16_t key, int line_delta)
  {
    pdt_entry* entry = pdt_find(key);
    if (entry == nullptr)
      return;
    for (auto& delta : entry->deltas)
      if (delta.valid && delta.delta == line_delta) {
        delta.conf = static_cast<uint8_t>(std::min<unsigned>(delta.conf + 1u, EPOCH));
        return;
      }
    pdt_delta* victim = nullptr; // a free slot, else the least confident slot without a prefetch level
    for (auto& delta : entry->deltas)
      if (!delta.valid) {
        victim = &delta;
        break;
      }
    if (victim == nullptr)
      for (auto& delta : entry->deltas)
        if (delta.status == 0 && (victim == nullptr || delta.conf < victim->conf))
          victim = &delta;
    if (victim == nullptr)
      return;
    *victim = pdt_delta{};
    victim->valid = true;
    victim->delta = static_cast<int8_t>(line_delta);
    victim->conf = 1;
  }

  // Issue one TDP prefetch. Returns whether it went out.
  bool issue(const champsim::modules::prefetcher& host, uint64_t target_line, bool fill_l1d, uint32_t metadata, const champsim::capability& cap)
  {
    if (config.recent_filter && std::find(recent_targets.begin(), recent_targets.end(), target_line) != recent_targets.end()) {
      st.recent_skip++;
      return false;
    }
    if (!host.prefetch_line(champsim::address{target_line << LOG2_BLOCK_SIZE}, fill_l1d, metadata, cap)) {
      st.pq_full++;
      return false;
    }
    if (config.recent_filter) {
      recent_targets[recent_next] = target_line;
      recent_next = (recent_next + 1) % RECENT_TARGETS;
    }
    (fill_l1d ? st.issued_l1d : st.issued_l2)++;
    return true;
  }

  // CHT search (dedup) + record, PDT lookup, issue.
  void process(const champsim::modules::prefetcher& host, const sighting& sight)
  {
    const auto& cap = sight.cap;
    const uint64_t cursor_line = (cap.base.to<uint64_t>() + cap.offset.to<uint64_t>()) >> LOG2_BLOCK_SIZE;
    const uint16_t base_hash = hash16(cap.base.to<uint64_t>());
    if (std::any_of(cht.begin(), cht.end(),
                    [&](const cht_entry& entry) { return entry.valid && entry.base_hash == base_hash && entry.cursor_line == cursor_line; })) {
      st.duplicate++;
      return; // already recorded: neither a new sighting nor a new issue
    }
    st.sightings[sight.source]++;
    cht[cht_next] = cht_entry{true, base_hash, cursor_line, sight.key, 0};
    cht_next = (cht_next + 1) % CHT_SIZE;

    const bool mshr_ok = host.intern_->get_mshr_occupancy_ratio() < MSHR_LIMIT;
    pdt_entry* own_entry = pdt_alloc(sight.key);
    const int own_slot = static_cast<int>(own_entry - pdt.data());
    pdt_entry* delta_source = own_entry; // the entry whose deltas are used
    // a field with no confident delta yet borrows the deltas of the sighting its line was fetched for
    if (sight.parent_slot >= 0
        && std::none_of(own_entry->deltas.begin(), own_entry->deltas.end(), [](const pdt_delta& delta) { return delta.valid && delta.status != 0; })) {
      pdt_entry& parent_entry = pdt[static_cast<std::size_t>(sight.parent_slot)];
      if (parent_entry.valid && &parent_entry != own_entry) {
        parent_entry.lru = ++pdt_clock;
        delta_source = &parent_entry;
        st.fallback++;
        st.fallback_reused_slot += (parent_entry.tag != sight.parent_tag_at_issue);
      }
    }
    for (const auto& delta : delta_source->deltas) { // the levels set at the end of the previous epoch
      if (!delta.valid || delta.status == 0)
        continue;
      const uint64_t target_line = cursor_line + static_cast<int64_t>(delta.delta);
      if (target_line == sight.own_line)
        continue;
      if (!recursive()) {
        issue(host, target_line, delta.status == 2 && mshr_ok, MD_TDP | MD_VALID | (static_cast<uint32_t>(sight.key & KEY_MASK) << MD_KEY_SHIFT), cap);
        continue;
      }
      // the probability that the target line is used: this key's own rate (depth 0, or a trained field key, whose rate
      // is already conditioned on the path that led to it); borrowed deltas are discounted by the path probability
      const bool use_own_rate = (sight.depth == 0) || (delta_source == own_entry);
      const unsigned prob = use_own_rate ? std::min<unsigned>(delta.last_conf, 16) : (static_cast<unsigned>(sight.prob) * delta.last_conf) / 16;
      const int level = use_own_rate ? delta.status : (prob >= THRESHOLD_L1D) ? 2 : (prob >= THRESHOLD_L2) ? 1 : 0;
      if (level == 0) {
        st.low_prob++;
        continue;
      }
      // the type of the line being fetched (which line of which length of object, from the pointer's bounds) within
      // its path (the key of the sighting it is fetched for)
      const uint16_t target_key = hash16((static_cast<uint64_t>(sight.key) << 40) ^ (cap.length.to<uint64_t>() * 0x9E3779B97F4A7C15ULL)
                                         ^ ((target_line << LOG2_BLOCK_SIZE) - cap.base.to<uint64_t>()) ^ 0x5bd1e995ULL);
      const bool fill_l1d = (level == 2) && mshr_ok;
      if (!issue(host, target_line, fill_l1d, md_make_chain(target_key, sight.depth + 1u, prob, own_slot), cap))
        continue;
      st.by_depth_issued[std::min<unsigned>(sight.depth + 1u, DEPTH_FIELD_MAX)][fill_l1d]++;
      if (fill_l1d)
        parent_tag_at_issue[target_line] = own_entry->tag;
    }
    pdt_count_sighting(*own_entry);
  }

  bool take_budget(const champsim::modules::prefetcher& host)
  {
    const uint64_t now = host.intern_->current_cycle();
    if (now != budget_cycle) {
      budget_cycle = now;
      budget_used = 0;
    }
    if (budget_used >= SIGHTINGS_PER_CYCLE)
      return false;
    budget_used++;
    return true;
  }

  void enqueue(const champsim::modules::prefetcher& host, const sighting& sight)
  {
    if (sighting_queue.empty() && take_budget(host)) {
      process(host, sight);
      return;
    }
    if (sighting_queue.size() >= SIGHTING_QUEUE) {
      sighting_queue.pop_front();
      st.queue_dropped++;
    }
    sighting_queue.push_back(sight);
    st.queued++;
  }

  // A data pointer in the slot at slot_va, or nothing (no tag, not loadable, executable, or null cursor)
  static std::optional<champsim::capability> data_pointer_at(uint64_t slot_va, uint32_t cpu)
  {
    auto pointer = champsim::cap_mem[cpu].load_capability(champsim::address{slot_va});
    if (!pointer.has_value() || !pointer->tag || !cheri::has_load_permissions(pointer->permissions) || cheri::has_execute_permissions(pointer->permissions))
      return std::nullopt; // not a data pointer (code pointers are never prefetched into L1D)
    if (pointer->base.to<uint64_t>() + pointer->offset.to<uint64_t>() == 0)
      return std::nullopt;
    return pointer;
  }

  // A demand or host-prefetch fill: its pointers, all under one key.
  void scan(const champsim::modules::prefetcher& host, uint64_t line, uint16_t key, uint8_t source, uint32_t cpu)
  {
    st.lines_scanned[source]++;
    const uint64_t line_va = line << LOG2_BLOCK_SIZE;
    for (unsigned slot = 0; slot < cheri::CAPS_PER_CL; slot++)
      if (auto pointer = data_pointer_at(line_va + slot * cheri::CAP_ALIGNMENT_BYTES, cpu))
        enqueue(host, sighting{*pointer, key, line, source});
  }

  // A TDP fill (recursion): its pointers, keyed by field (the line's key with the slot index), with the depth, path
  // probability and parent's PDT slot the prefetch carried.
  void scan_tdp_fill(const champsim::modules::prefetcher& host, uint64_t line, uint32_t fill_metadata, uint32_t cpu)
  {
    const unsigned depth = md_depth(fill_metadata);
    if (depth == 0)
      return;
    int issue_tag = -1; // statistics only
    if (auto issued = parent_tag_at_issue.find(line); issued != parent_tag_at_issue.end()) {
      issue_tag = issued->second;
      parent_tag_at_issue.erase(issued);
    }
    if (depth >= config.max_depth) {
      st.depth_stop++;
      return;
    }
    st.lines_scanned[SRC_TDP_FILL]++;
    const uint64_t line_va = line << LOG2_BLOCK_SIZE;
    const uint16_t line_key = static_cast<uint16_t>(fill_metadata & MD_LINE_KEY_MASK);
    for (unsigned slot = 0; slot < cheri::CAPS_PER_CL; slot++) {
      auto pointer = data_pointer_at(line_va + slot * cheri::CAP_ALIGNMENT_BYTES, cpu);
      if (!pointer)
        continue;
      // the field: the line's type key (the pointed-to object's length and this line's offset in it, from the pointer
      // it was fetched through, and that pointer's key) with the slot index
      const uint16_t field_key = hash16((static_cast<uint64_t>(line_key) << 8) ^ (slot + 1) ^ 0x9E3779B97F4A7C15ULL);
      sighting found{*pointer, field_key, line, SRC_TDP_FILL};
      found.depth = static_cast<uint8_t>(depth);
      found.prob = static_cast<uint8_t>(md_prob(fill_metadata));
      found.parent_slot = md_parent_slot(fill_metadata);
      found.parent_tag_at_issue = issue_tag;
      enqueue(host, found);
    }
  }

  // A dereference: credit the line difference to the key of the sighting of the pointer it went through.
  void train(uint64_t line, const champsim::capability& cap, bool is_miss)
  {
    const uint16_t base_hash = hash16(cap.base.to<uint64_t>());
    cht_entry* best = nullptr;
    int best_delta = 0;
    for (auto& entry : cht) {
      if (!entry.valid || entry.base_hash != base_hash)
        continue;
      const int64_t line_delta = static_cast<int64_t>(line) - static_cast<int64_t>(entry.cursor_line);
      if (line_delta < -MAX_LINE_DELTA || line_delta > MAX_LINE_DELTA)
        continue;
      if (best == nullptr || std::abs(line_delta) < std::abs(best_delta)) { // nearest cursor; on a tie, the oldest
        best = &entry;
        best_delta = static_cast<int>(line_delta);
      }
    }
    st.tagged_load_miss += is_miss;
    if (best == nullptr)
      return;
    const uint16_t delta_bit = static_cast<uint16_t>(1u << (best_delta + MAX_LINE_DELTA));
    if (best->trained & delta_bit)
      return;
    best->trained |= delta_bit;
    st.train++;
    pdt_train(best->key, best_delta);
  }

public:
  explicit engine(settings host_settings = {}) : config(host_settings) { config.max_depth = std::clamp(config.max_depth, 1u, DEPTH_FIELD_MAX); }

  // Metadata bits for the host's own prefetches triggered by an access of this IP
  static uint32_t tag_prefetch(uint64_t ip) { return MD_VALID | (static_cast<uint32_t>(ip_key(ip)) << MD_KEY_SHIFT); }

  // Every access the host sees. line is the virtual line address; cap the access's authorizing capability.
  void on_operate(uint64_t line, const champsim::capability& cap, bool cache_hit, bool useful_prefetch, access_type type, uint32_t metadata_hit)
  {
    if (type != access_type::LOAD || !cap.tag)
      return;
    const bool tdp_hit = cache_hit && useful_prefetch && (metadata_hit & MD_TDP);
    if (tdp_hit) {
      st.useful++;
      if (recursive())
        st.by_depth_useful[md_depth(metadata_hit)]++;
    }
    if (!cache_hit || tdp_hit)
      train(line, cap, !cache_hit);
  }

  // Every fill. Scans the line's capability slots: what its tags and data carry.
  void on_fill(const champsim::modules::prefetcher& host, uint64_t line, uint64_t ip, uint32_t cpu, bool prefetch, uint32_t metadata_in, bool useless,
               uint32_t metadata_evict)
  {
    if (useless && (metadata_evict & MD_TDP)) {
      st.useless++;
      if (recursive())
        st.by_depth_useless[md_depth(metadata_evict)]++;
    }
    if (!prefetch) {
      if (ip != 0)
        scan(host, line, config.berti_ip_key ? berti_ip_hash(ip) : ip_key(ip), SRC_DEMAND_FILL, cpu);
      return;
    }
    if (metadata_in & MD_TDP) {
      st.fills++;
      if (recursive()) {
        st.by_depth_fills[md_depth(metadata_in)]++;
        scan_tdp_fill(host, line, metadata_in, cpu);
      }
      return; // without recursion, TDP-prefetched lines are not scanned again (depth 1)
    }
    if (metadata_in & MD_VALID) // the host's own prefetch, tagged with its trigger's key
      scan(host, line, static_cast<uint16_t>((metadata_in >> MD_KEY_SHIFT) & KEY_MASK), SRC_HOST_PREFETCH_FILL, cpu);
  }

  void on_cycle(const champsim::modules::prefetcher& host)
  {
    while (!sighting_queue.empty() && take_budget(host)) {
      process(host, sighting_queue.front());
      sighting_queue.pop_front();
    }
    if (in_warmup && !host.intern_->warmup) { // statistics cover the region of interest only
      in_warmup = false;
      st = stats{};
    }
  }

  void print_stats(std::ostream& out) const
  {
    const char* source_name[NUM_SRC] = {"demand_fill", "host_prefetch_fill", "tdp_fill"};
    out << "TDP SETTINGS max_depth " << config.max_depth << " berti_ip_key " << config.berti_ip_key << " recent_filter " << config.recent_filter << std::endl;
    out << "TDP LINES_SCANNED";
    for (int source = 0; source < NUM_SRC; source++)
      out << " " << source_name[source] << " " << st.lines_scanned[source];
    out << std::endl << "TDP SIGHTINGS";
    for (int source = 0; source < NUM_SRC; source++)
      out << " " << source_name[source] << " " << st.sightings[source];
    out << " DUPLICATE " << st.duplicate << " QUEUED " << st.queued << " QUEUE_DROPPED " << st.queue_dropped << std::endl;
    out << "TDP TRAIN " << st.train << " TAGGED_LOAD_MISSES " << st.tagged_load_miss << std::endl;
    out << "TDP ISSUED L1 " << st.issued_l1d << " L2 " << st.issued_l2 << " PQ_FULL " << st.pq_full << " RECENT_SKIP " << st.recent_skip << std::endl;
    out << "TDP FILLS " << st.fills << " USEFUL " << st.useful << " USELESS " << st.useless << std::endl;
    if (!recursive())
      return;
    out << "TDP RECUR depth_stop " << st.depth_stop << " low_prob_skip " << st.low_prob << " fallback " << st.fallback << " fallback_reused_slot "
        << st.fallback_reused_slot << std::endl;
    for (unsigned depth = 1; depth <= DEPTH_FIELD_MAX; depth++)
      if (st.by_depth_issued[depth][0] + st.by_depth_issued[depth][1] + st.by_depth_fills[depth] + st.by_depth_useful[depth] + st.by_depth_useless[depth])
        out << "TDP RECUR depth " << depth << " issued_l1 " << st.by_depth_issued[depth][1] << " issued_l2 " << st.by_depth_issued[depth][0] << " fills "
            << st.by_depth_fills[depth] << " useful " << st.by_depth_useful[depth] << " useless " << st.by_depth_useless[depth] << std::endl;
  }
};
} // namespace cheri_tdp

#endif
