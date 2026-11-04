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
// sit in: the size-offset key, a hash of the parent object's length and the field's offset from where the parent
// pointer points (16-byte units). A chained key prefetches only with its own confident deltas (no borrowing from the
// parent). Recursion's sightings are recorded in their own chained CHT, so they never evict first-hop history. Chains
// stop when no confident delta remains, or at max_depth.
//
// Per-host settings (the defaults are the depth-1 engine that ip_stride_cheri_tdp and ipcp_cap_tdp were built with):
//   max_depth      1 = no recursion; up to 7 (the depth field's maximum)
//   berti_ip_key   depth-0 key: false = 11-bit hash of the IP; true = Berti's 16-bit IP hash
//   recent_filter  true = skip targets among the last 32 issued; false = leave duplicates to the cache's tag and MSHR check
//   page_stats     statistics only: same-virtual-page counts of TDP targets (tdp_page_stats.h); the host must pass the
//                  evicted line to on_fill
//
// Host prefetcher (at a virtual_prefetch L1D):
//   cache_operate: engine.on_operate(...) for every access, and OR engine.tag_prefetch(ip) into the metadata of the
//                  host's own prefetches (so the lines they bring in are scanned under the trigger's key);
//   cache_fill:    engine.on_fill(...), and keep the bits in cheri_tdp::MD_MASK in the metadata the hook returns;
//   cycle_operate: engine.on_cycle(*this).
// Metadata of TDP's own prefetches:
//   max_depth 1:   [19] TDP prefetch, [20] key valid, [31:21] key. The host must leave bits 19-31 free.
//   max_depth > 1: [15:0] line key, [18:16] depth, [19] TDP prefetch. The host must leave bits 0-19 of TDP's
//                  prefetches' metadata alone.

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <iostream>
#include <optional>

#include "cache.h"
#include "capability_memory.h"
#include "cheri_prefetch_utils.h"
#include "modules.h"
#include "tdp_page_stats.h"

namespace cheri_tdp
{
constexpr std::size_t CHT_SETS = 8;           // Capability History Table sets, indexed by hash(base hash, cursor line >> CHT_REGION)
constexpr std::size_t CHT_WAYS = 16;          // CHT ways (FIFO within a set)
constexpr unsigned CHT_REGION = 4;            // log2 of the lines per cursor region (> log2(MAX_LINE_DELTA))
constexpr std::size_t CHAINED_CHT_SETS = 8;   // chained CHT (recursion's sightings, max_depth > 1) sets, indexed as the CHT
constexpr std::size_t CHAINED_CHT_WAYS = 8;   // chained CHT ways (FIFO within a set)
constexpr std::size_t PDT_SETS = 16;          // Pointer Delta Table sets (indexed by a hash of the key)
constexpr std::size_t PDT_WAYS = 4;           // PDT ways (LRU within a set)
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
constexpr unsigned MD_DEPTH_SHIFT = 16;
static_assert(MAX_LINE_DELTA < (1 << CHT_REGION), "a dereference's cursors must lie in at most two CHT regions");

struct settings {
  unsigned max_depth = 1;    // 1 = no recursion
  bool berti_ip_key = false; // depth-0 key: false = 11-bit IP hash, true = Berti's 16-bit IP hash
  bool recent_filter = true; // skip targets among the last RECENT_TARGETS issued
  bool page_stats = false;   // statistics only: same-page counts of TDP targets
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
  };

  settings config;
  std::array<cht_entry, CHT_SETS * CHT_WAYS> cht{};
  std::array<uint16_t, CHT_SETS> cht_next{};
  std::array<cht_entry, CHAINED_CHT_SETS * CHAINED_CHT_WAYS> chained_cht{};
  std::array<uint16_t, CHAINED_CHT_SETS> chained_cht_next{};
  std::array<pdt_entry, PDT_SETS * PDT_WAYS> pdt{};
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
    uint64_t depth_stop = 0;
    uint64_t by_depth_issued[8][2] = {}, by_depth_fills[8] = {}, by_depth_useful[8] = {}, by_depth_useless[8] = {}; // [.][0] L2, [.][1] L1D
  } st;
  bool in_warmup = true;
  tdp_page_stats::recorder page_stats;                         // statistics only (settings.page_stats)

  bool recursive() const { return config.max_depth > 1; }
  static unsigned md_depth(uint32_t metadata) { return (metadata >> MD_DEPTH_SHIFT) & 0x7; }
  static uint32_t md_make_chain(uint16_t line_key, unsigned depth) { return line_key | MD_TDP | (std::min(depth, DEPTH_FIELD_MAX) << MD_DEPTH_SHIFT); }

  // CHT and chained CHT: the set of a pointer's object (base hash) and the CHT_REGION-line region of its cursor
  static std::size_t cht_set(uint16_t base_hash, uint64_t line, std::size_t sets)
  {
    const uint64_t h = (static_cast<uint64_t>(base_hash) ^ ((line >> CHT_REGION) * 0x9E3779B97F4A7C15ULL)) * 0xff51afd7ed558ccdULL;
    return static_cast<std::size_t>(h >> 40) % sets;
  }
  // PDT: the ways of a key's set
  auto pdt_ways(uint16_t tag)
  {
    const auto first = pdt.begin() + static_cast<std::ptrdiff_t>(((tag * 0x9E3779B1u) >> 16) % PDT_SETS * PDT_WAYS);
    return std::pair{first, first + PDT_WAYS};
  }

  pdt_entry* pdt_find(uint16_t tag)
  {
    auto [first, last] = pdt_ways(tag);
    for (auto it = first; it != last; ++it)
      if (it->valid && it->tag == tag) {
        it->lru = ++pdt_clock;
        return &*it;
      }
    return nullptr;
  }

  pdt_entry* pdt_alloc(uint16_t tag)
  {
    if (auto* entry = pdt_find(tag))
      return entry;
    auto [first, last] = pdt_ways(tag);
    auto victim = std::min_element(first, last, [](const auto& lhs, const auto& rhs) { return lhs.valid != rhs.valid ? !lhs.valid : lhs.lru < rhs.lru; });
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

  // Issue one TDP prefetch. Returns whether it went out. pointer_line, depth: statistics only (page_stats).
  bool issue(const champsim::modules::prefetcher& host, uint64_t target_line, bool fill_l1d, uint32_t metadata, const champsim::capability& cap,
             uint64_t pointer_line, unsigned depth)
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
    if (config.page_stats)
      page_stats.on_issue(target_line, pointer_line, depth, fill_l1d);
    return true;
  }

  // CHT search (dedup) + record, PDT lookup, issue.
  void process(const champsim::modules::prefetcher& host, const sighting& sight)
  {
    const auto& cap = sight.cap;
    const uint64_t cursor_addr = cap.base.to<uint64_t>() + cap.offset.to<uint64_t>();
    const uint64_t cursor_line = cursor_addr >> LOG2_BLOCK_SIZE;
    const uint16_t base_hash = hash16(cap.base.to<uint64_t>());
    const std::size_t set = cht_set(base_hash, cursor_line, CHT_SETS);
    const std::size_t chained_set = cht_set(base_hash, cursor_line, CHAINED_CHT_SETS);
    auto recorded = [&](const cht_entry& entry) { return entry.valid && entry.base_hash == base_hash && entry.cursor_line == cursor_line; };
    if (std::any_of(cht.begin() + set * CHT_WAYS, cht.begin() + (set + 1) * CHT_WAYS, recorded)
        || (recursive()
            && std::any_of(chained_cht.begin() + chained_set * CHAINED_CHT_WAYS, chained_cht.begin() + (chained_set + 1) * CHAINED_CHT_WAYS, recorded))) {
      st.duplicate++;
      return; // already recorded: neither a new sighting nor a new issue
    }
    st.sightings[sight.source]++;
    if (sight.source == SRC_TDP_FILL) { // recursion's sightings: the chained CHT (FIFO within the set)
      chained_cht[chained_set * CHAINED_CHT_WAYS + chained_cht_next[chained_set]] = cht_entry{true, base_hash, cursor_line, sight.key, 0};
      chained_cht_next[chained_set] = static_cast<uint16_t>((chained_cht_next[chained_set] + 1) % CHAINED_CHT_WAYS);
    } else { // FIFO within the set
      cht[set * CHT_WAYS + cht_next[set]] = cht_entry{true, base_hash, cursor_line, sight.key, 0};
      cht_next[set] = static_cast<uint16_t>((cht_next[set] + 1) % CHT_WAYS);
    }

    const bool mshr_ok = host.intern_->get_mshr_occupancy_ratio() < MSHR_LIMIT;
    pdt_entry* own_entry = pdt_alloc(sight.key);
    for (const auto& delta : own_entry->deltas) { // the levels set at the end of the previous epoch
      if (!delta.valid || delta.status == 0)
        continue;
      const uint64_t target_line = cursor_line + static_cast<int64_t>(delta.delta);
      if (target_line == sight.own_line)
        continue;
      if (!recursive()) {
        issue(host, target_line, delta.status == 2 && mshr_ok, MD_TDP | MD_VALID | (static_cast<uint32_t>(sight.key & KEY_MASK) << MD_KEY_SHIFT), cap,
              sight.own_line, sight.depth + 1u);
        continue;
      }
      // the line's part of the size-offset key: the object's length (8-bit hash, [15:8]) and the line's offset from
      // the pointer's cursor in 16-byte units, biased by 128 ([7:0]; the target is within MAX_LINE_DELTA lines of the
      // cursor's line). The fill adds each found pointer's position in the line, giving the field's offset from where
      // this pointer points, whatever the object's bounds or alignment
      const int64_t line_off16 = (static_cast<int64_t>(target_line << LOG2_BLOCK_SIZE) - static_cast<int64_t>(cursor_addr)) >> 4;
      const uint16_t target_key = static_cast<uint16_t>((hash16(cap.length.to<uint64_t>() * 0x9E3779B97F4A7C15ULL) & 0xFF00)
                                                        | static_cast<uint16_t>((line_off16 + 128) & 0xFF));
      const bool fill_l1d = (delta.status == 2) && mshr_ok;
      if (issue(host, target_line, fill_l1d, md_make_chain(target_key, sight.depth + 1u), cap, sight.own_line, sight.depth + 1u))
        st.by_depth_issued[std::min<unsigned>(sight.depth + 1u, DEPTH_FIELD_MAX)][fill_l1d]++;
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

  // A TDP fill (recursion): its pointers, keyed by field (the line's part of the size-offset key with the slot index),
  // with the depth the prefetch carried.
  void scan_tdp_fill(const champsim::modules::prefetcher& host, uint64_t line, uint32_t fill_metadata, uint32_t cpu)
  {
    const unsigned depth = md_depth(fill_metadata);
    if (depth == 0)
      return;
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
      // the field: the object's length and the pointer's offset from where the parent pointer points (16-byte units)
      const uint16_t field_key = hash16((static_cast<uint64_t>(line_key >> 8) << 16) ^ static_cast<uint64_t>((line_key & 0xFF) + slot) ^ 0x9E3779B97F4A7C15ULL);
      sighting found{*pointer, field_key, line, SRC_TDP_FILL};
      found.depth = static_cast<uint8_t>(depth);
      enqueue(host, found);
    }
  }

  // A dereference: credit the line difference to the key of the sighting of the pointer it went through.
  void train(uint64_t line, const champsim::capability& cap, bool is_miss)
  {
    const uint16_t base_hash = hash16(cap.base.to<uint64_t>());
    cht_entry* best = nullptr;
    int best_delta = 0;
    auto search = [&](auto first, auto last) {
      for (auto it = first; it != last; ++it) {
        auto& entry = *it;
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
    };
    // the cursors within MAX_LINE_DELTA lines of this line lie in at most two regions: search both sets of each table
    auto search_table = [&](auto& table, std::size_t sets, std::size_t ways) {
      const std::size_t set_lo = cht_set(base_hash, line - MAX_LINE_DELTA, sets), set_hi = cht_set(base_hash, line + MAX_LINE_DELTA, sets);
      search(table.begin() + set_lo * ways, table.begin() + (set_lo + 1) * ways);
      if (set_hi != set_lo)
        search(table.begin() + set_hi * ways, table.begin() + (set_hi + 1) * ways);
    };
    search_table(cht, CHT_SETS, CHT_WAYS);
    if (recursive())
      search_table(chained_cht, CHAINED_CHT_SETS, CHAINED_CHT_WAYS);
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
      if (config.page_stats)
        page_stats.on_used(line);
    }
    if (!cache_hit || tdp_hit)
      train(line, cap, !cache_hit);
  }

  // Every fill. Scans the line's capability slots: what its tags and data carry. evicted_line: statistics only (page_stats).
  void on_fill(const champsim::modules::prefetcher& host, uint64_t line, uint64_t ip, uint32_t cpu, bool prefetch, uint32_t metadata_in, bool useless,
               uint32_t metadata_evict, uint64_t evicted_line = 0)
  {
    if (useless && (metadata_evict & MD_TDP)) {
      st.useless++;
      if (recursive())
        st.by_depth_useless[md_depth(metadata_evict)]++;
      if (config.page_stats)
        page_stats.on_unused_eviction(evicted_line);
    }
    if (!prefetch) {
      if (ip != 0)
        scan(host, line, config.berti_ip_key ? berti_ip_hash(ip) : ip_key(ip), SRC_DEMAND_FILL, cpu);
      return;
    }
    if (metadata_in & MD_TDP) {
      st.fills++;
      if (config.page_stats)
        page_stats.on_tdp_fill(line);
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
      page_stats.reset_counts();
    }
  }

  void print_stats(std::ostream& out) const
  {
    const char* source_name[NUM_SRC] = {"demand_fill", "host_prefetch_fill", "tdp_fill"};
    out << "TDP SETTINGS max_depth " << config.max_depth << " berti_ip_key " << config.berti_ip_key << " recent_filter " << config.recent_filter << " CHT "
        << CHT_SETS << "x" << CHT_WAYS << " chained_CHT " << CHAINED_CHT_SETS << "x" << CHAINED_CHT_WAYS << " PDT " << PDT_SETS << "x" << PDT_WAYS << std::endl;
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
    if (config.page_stats)
      page_stats.print(out, "tdp_engine");
    if (!recursive())
      return;
    out << "TDP RECUR depth_stop " << st.depth_stop << std::endl;
    for (unsigned depth = 1; depth <= DEPTH_FIELD_MAX; depth++)
      if (st.by_depth_issued[depth][0] + st.by_depth_issued[depth][1] + st.by_depth_fills[depth] + st.by_depth_useful[depth] + st.by_depth_useless[depth])
        out << "TDP RECUR depth " << depth << " issued_l1 " << st.by_depth_issued[depth][1] << " issued_l2 " << st.by_depth_issued[depth][0] << " fills "
            << st.by_depth_fills[depth] << " useful " << st.by_depth_useful[depth] << " useless " << st.by_depth_useless[depth] << std::endl;
  }
};
} // namespace cheri_tdp

#endif
