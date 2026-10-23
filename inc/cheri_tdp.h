#ifndef CHERI_TDP_H
#define CHERI_TDP_H

// Tag-directed prefetching (TDP): the pointer-prefetching layer of Berti-TDP (prefetcher/berti_tdp/DESIGN.md), packaged so any L1D
// prefetcher can add it. CHERI tags mark the pointer slots of every line arriving in L1D; each pointer is a sighting,
// recorded in the Capability History Table (CHT) and looked up in the Pointer Delta Table (PDT), which holds per IP
// Berti-style epoch confidences of "dereferenced line - pointer's cursor line"; confident deltas are prefetched. A
// tagged demand load that misses (or hits a TDP-prefetched line) and whose authorizing capability is a recorded pointer
// trains its delta.
//
// Differences from berti_tdp: no Berti latency table to test "already in flight", so a 32-entry filter of recently
// issued TDP targets is used instead; the key is an 11-bit IP hash, so that it fits in prefetch metadata bits 21-31.
//
// Host prefetcher (at a virtual_prefetch L1D):
//   cache_operate: tdp.on_operate(...) for every access, and OR tdp.tag_prefetch(ip) into the metadata of the host's own
//                  prefetches (so the lines they bring in are scanned under the trigger's key);
//   cache_fill:    tdp.on_fill(...), and keep the bits in cheri_tdp::MD_MASK in the metadata the hook returns;
//   cycle_operate: tdp.on_cycle(*this).
// The host must leave metadata bits 19-31 free.
//
// Experiment switches are read once from the environment, so one binary runs every variant (unset = the design):
//   TDP_TH_L2=n        dereferences per epoch for an L2-level delta (default 4; 8 = THRESHOLD_L1 leaves no
//                      low-confidence L2 prefetches)
//   TDP_DEMOTE=0       an L1D-level prefetch is dropped when the L1D MSHR is busy, instead of being sent to L2
//   TDP_FEEDBACK=1     per-key accuracy feedback (FEEDBACK_* below) gates issue
//   TDP_BERTI_TRAIN=0  Berti-TDP only: Berti does not treat TDP-prefetched lines as its own prefetches
//   TDP_INTERVAL=n     print a "TDP INTERVAL" line of per-interval counters every n cycles (default 0: none)
// The values in effect are printed with the final stats ("TDP CONFIG").

#include <algorithm>
#include <array>
#include <bitset>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <iostream>
#include <unordered_map>
#include <utility>

#include "cache.h"
#include "capability_memory.h"
#include "cheri_prefetch_utils.h"
#include "modules.h"

namespace cheri_tdp
{
constexpr std::size_t CHT_SIZE = 128;            // Capability History Table entries (FIFO)
constexpr std::size_t PDT_SIZE = 64;             // Pointer Delta Table entries (fully associative, LRU)
constexpr std::size_t PDT_DELTAS = 4;            // deltas per PDT entry
constexpr std::size_t SIGHTING_QUEUE_SIZE = 8;   // sighting queue entries (oldest dropped when full)
constexpr unsigned SIGHTINGS_PER_CYCLE = 2;      // sightings processed per cycle
constexpr unsigned EPOCH = 16;                   // sightings per confidence epoch
constexpr unsigned THRESHOLD_L1 = 8;             // dereferences per epoch for an L1D-level delta
constexpr unsigned THRESHOLD_L2 = 4;             // for an L2-level delta
constexpr int MAX_LINE_DELTA = 7;                // |dereferenced line - cursor line| learned
constexpr std::size_t RECENT_FILTER_SIZE = 32;   // recently issued TDP targets (duplicate filter)
constexpr double MSHR_LIMIT = 0.70;              // L1D-level TDP prefetches only below this MSHR occupancy (as Berti)

// Accuracy feedback (TDP_FEEDBACK=1). Each CHT entry records which deltas were prefetched for its sighting; a
// prefetched delta whose target is dereferenced while the sighting is in the CHT is a hit (+1 to the key's counter),
// one still unmatched when the entry is overwritten is a miss (-1). This sees L2-level prefetches as well as L1D ones.
constexpr uint8_t FEEDBACK_MAX = 15;      // 4-bit counter per PDT entry
constexpr uint8_t FEEDBACK_INIT = 8;
constexpr uint8_t FEEDBACK_MIN_L2 = 8;    // L2-level issue needs at least this
constexpr uint8_t FEEDBACK_MIN_ISSUE = 4; // any issue needs at least this; below, +1 per epoch so the key is tried again

// Prefetch metadata: [19] TDP prefetch, [20] key valid, [31:21] key (11-bit IP hash)
constexpr uint32_t MD_TDP = 1u << 19;
constexpr uint32_t MD_VALID = 1u << 20;
constexpr unsigned MD_KEY_SHIFT = 21;
constexpr uint32_t KEY_MASK = 0x7FF;
constexpr uint32_t MD_MASK = MD_TDP | MD_VALID | (KEY_MASK << MD_KEY_SHIFT);

inline uint16_t hash16(uint64_t value)
{
  value ^= value >> 33;
  value *= 0xff51afd7ed558ccdULL;
  value ^= value >> 29;
  return static_cast<uint16_t>(value ^ (value >> 16) ^ (value >> 32) ^ (value >> 48));
}

inline uint16_t ip_key(uint64_t ip) { return static_cast<uint16_t>(hash16(ip) & KEY_MASK); }

// Experiment switches (see the top of this file)
struct knobs {
  unsigned threshold_l2 = 0; // 0: the module's default
  bool demote = true;
  bool feedback = false;
  bool berti_train = true;
  uint64_t interval = 0;
};

inline const knobs& get_knobs()
{
  static const knobs settings = [] {
    auto read_env = [](const char* name, unsigned long long default_value) {
      const char* text = std::getenv(name);
      return (text == nullptr || *text == '\0') ? default_value : std::strtoull(text, nullptr, 10);
    };
    knobs result;
    result.threshold_l2 = static_cast<unsigned>(read_env("TDP_TH_L2", 0));
    result.demote = read_env("TDP_DEMOTE", 1) != 0;
    result.feedback = read_env("TDP_FEEDBACK", 0) != 0;
    result.berti_train = read_env("TDP_BERTI_TRAIN", 1) != 0;
    result.interval = read_env("TDP_INTERVAL", 0);
    return result;
  }();
  return settings;
}

inline void print_config(std::ostream& out, unsigned threshold_l2)
{
  const knobs& settings = get_knobs();
  out << "TDP CONFIG TH_L2 " << threshold_l2 << " DEMOTE " << settings.demote << " FEEDBACK " << settings.feedback << " BERTI_TRAIN "
      << settings.berti_train << " INTERVAL " << settings.interval << std::endl;
}

// Statistics only (not hardware): how many TDP prefetches sent to L2 were later requested by an L1D demand miss. An
// upper bound on their usefulness: a target counts as used if a demand miss asks for it within the last TRACKED_PREFETCHES
// TDP L2 prefetches (about 1.6x the L2's lines), whether or not it was still in L2.
class l2_usage
{
  static constexpr std::size_t TRACKED_PREFETCHES = 32768;
  std::unordered_map<uint64_t, std::pair<uint64_t, uint8_t>> unused_by_line; // line -> (sequence number, source)
  std::deque<std::pair<uint64_t, uint64_t>> issue_order;                     // (line, sequence number), oldest first
  uint64_t sequence = 0;

public:
  enum { LOWCONF, DEMOTED, NUM_SOURCES }; // status-1 delta; status-2 delta sent down because the L1D MSHR was busy
  uint64_t issued[NUM_SOURCES] = {}, used[NUM_SOURCES] = {};

  void issue(uint64_t line, int source)
  {
    issued[source]++;
    unused_by_line[line] = {++sequence, static_cast<uint8_t>(source)};
    issue_order.emplace_back(line, sequence);
    if (issue_order.size() > TRACKED_PREFETCHES) {
      auto [oldest_line, oldest_sequence] = issue_order.front();
      issue_order.pop_front();
      auto tracked = unused_by_line.find(oldest_line);
      if (tracked != unused_by_line.end() && tracked->second.first == oldest_sequence)
        unused_by_line.erase(tracked);
    }
  }

  void demand_miss(uint64_t line)
  {
    auto tracked = unused_by_line.find(line);
    if (tracked == unused_by_line.end())
      return;
    used[tracked->second.second]++;
    unused_by_line.erase(tracked);
  }

  void print(std::ostream& out) const
  {
    out << "TDP L2_USE lowconf issued " << issued[LOWCONF] << " used " << used[LOWCONF] << " demoted issued " << issued[DEMOTED] << " used "
        << used[DEMOTED] << std::endl;
  }
};

class engine
{
  struct history_entry {          // CHT entry: one pointer sighting
    bool valid = false;
    uint16_t base_hash = 0;       // hash of the pointer's base: names the pointed-to object
    uint64_t cursor_line = 0;     // the pointer's cursor line
    uint16_t key = 0;
    uint16_t trained_deltas = 0;  // deltas already credited for this sighting, one bit per delta
    uint16_t pending_deltas = 0;  // deltas prefetched for this sighting and not yet dereferenced, one bit per delta
  };
  struct delta_slot {
    bool valid = false;
    int8_t delta = 0;
    uint8_t confidence = 0;
    uint8_t status = 0; // 0 none, 1 prefetch to L2, 2 prefetch to L1D
  };
  struct delta_entry {            // PDT entry: one key
    bool valid = false;
    uint16_t tag = 0;
    uint8_t sightings = 0;
    uint8_t accuracy = FEEDBACK_INIT; // accuracy counter (TDP_FEEDBACK)
    uint64_t lru_stamp = 0;
    std::array<delta_slot, PDT_DELTAS> slots{};
  };
  struct sighting {
    champsim::capability pointer;
    uint16_t key;
    uint64_t own_line; // the line the pointer was read from
  };

  std::array<history_entry, CHT_SIZE> history{};
  std::size_t history_next = 0;
  std::array<delta_entry, PDT_SIZE> delta_table{};
  uint64_t lru_clock = 0;
  std::deque<sighting> sighting_queue;
  uint64_t budget_cycle = ~uint64_t{0};
  unsigned budget_used = 0;
  std::array<uint64_t, RECENT_FILTER_SIZE> recent_targets{};
  std::size_t recent_next = 0;

  const knobs& settings = get_knobs();
  const unsigned threshold_l2 = settings.threshold_l2 ? settings.threshold_l2 : THRESHOLD_L2;

  // statistics (not hardware)
  uint64_t stat_sightings = 0, stat_duplicates = 0, stat_queued = 0, stat_queue_dropped = 0;
  uint64_t stat_trained = 0, stat_issued_l1 = 0, stat_issued_l2 = 0, stat_pq_full = 0, stat_recent_skipped = 0;
  uint64_t stat_fills = 0, stat_useful = 0, stat_useless = 0;
  uint64_t stat_busy_dropped = 0, stat_feedback_skipped_all = 0, stat_feedback_skipped_l2 = 0;
  uint64_t stat_predictions_dereferenced = 0, stat_predictions_not_dereferenced = 0;
  l2_usage l2_tracker;
  struct interval_counters {
    uint64_t issued_l1, l2_lowconf, l2_demoted, busy_dropped, fills, useful, useless, l2_used, predictions_dereferenced,
        predictions_not_dereferenced, feedback_skipped;
  };
  interval_counters last_interval{};
  uint64_t next_interval = 0;

  interval_counters current_counters() const
  {
    return {stat_issued_l1,
            l2_tracker.issued[l2_usage::LOWCONF],
            l2_tracker.issued[l2_usage::DEMOTED],
            stat_busy_dropped,
            stat_fills,
            stat_useful,
            stat_useless,
            l2_tracker.used[l2_usage::LOWCONF] + l2_tracker.used[l2_usage::DEMOTED],
            stat_predictions_dereferenced,
            stat_predictions_not_dereferenced,
            stat_feedback_skipped_all + stat_feedback_skipped_l2};
  }

  delta_entry* peek_delta_entry(uint16_t tag) // lookup without an LRU update (feedback)
  {
    for (auto& entry : delta_table)
      if (entry.valid && entry.tag == tag)
        return &entry;
    return nullptr;
  }

  void feedback(uint16_t key, int change)
  {
    if (!settings.feedback)
      return;
    if (delta_entry* entry = peek_delta_entry(key))
      entry->accuracy = static_cast<uint8_t>(std::clamp<int>(entry->accuracy + change, 0, FEEDBACK_MAX));
  }

  delta_entry* find_delta_entry(uint16_t tag)
  {
    for (auto& entry : delta_table)
      if (entry.valid && entry.tag == tag) {
        entry.lru_stamp = ++lru_clock;
        return &entry;
      }
    return nullptr;
  }

  delta_entry* find_or_allocate_delta_entry(uint16_t tag)
  {
    if (auto* entry = find_delta_entry(tag))
      return entry;
    auto victim = std::min_element(delta_table.begin(), delta_table.end(), [](const auto& lhs, const auto& rhs) {
      return lhs.valid != rhs.valid ? !lhs.valid : lhs.lru_stamp < rhs.lru_stamp;
    });
    *victim = delta_entry{};
    victim->valid = true;
    victim->tag = tag;
    victim->lru_stamp = ++lru_clock;
    return &*victim;
  }

  void count_sighting(delta_entry* entry)
  {
    if (++entry->sightings < EPOCH)
      return;
    for (auto& slot : entry->slots) {
      if (!slot.valid)
        continue;
      slot.status = (slot.confidence >= THRESHOLD_L1) ? 2 : (slot.confidence >= threshold_l2) ? 1 : 0;
      slot.confidence = 0;
    }
    entry->sightings = 0;
    if (settings.feedback && entry->accuracy < FEEDBACK_MIN_ISSUE)
      entry->accuracy++;
  }

  void train_delta(uint16_t key, int delta)
  {
    delta_entry* entry = find_delta_entry(key);
    if (entry == nullptr)
      return;
    for (auto& slot : entry->slots)
      if (slot.valid && slot.delta == delta) {
        slot.confidence = static_cast<uint8_t>(std::min<unsigned>(slot.confidence + 1u, EPOCH));
        return;
      }
    delta_slot* victim = nullptr; // a free slot, else the least confident slot without a prefetch level
    for (auto& slot : entry->slots)
      if (!slot.valid) {
        victim = &slot;
        break;
      }
    if (victim == nullptr)
      for (auto& slot : entry->slots)
        if (slot.status == 0 && (victim == nullptr || slot.confidence < victim->confidence))
          victim = &slot;
    if (victim == nullptr)
      return;
    *victim = delta_slot{};
    victim->valid = true;
    victim->delta = static_cast<int8_t>(delta);
    victim->confidence = 1;
  }

  bool issue(const champsim::modules::prefetcher& host, uint64_t line, bool fill_l1, uint16_t key, const champsim::capability& pointer)
  {
    if (std::find(recent_targets.begin(), recent_targets.end(), line) != recent_targets.end()) {
      stat_recent_skipped++;
      return false;
    }
    const uint32_t metadata = MD_TDP | MD_VALID | (static_cast<uint32_t>(key & KEY_MASK) << MD_KEY_SHIFT);
    if (!host.prefetch_line(champsim::address{line << LOG2_BLOCK_SIZE}, fill_l1, metadata, pointer)) {
      stat_pq_full++;
      return false;
    }
    recent_targets[recent_next] = line;
    recent_next = (recent_next + 1) % RECENT_FILTER_SIZE;
    (fill_l1 ? stat_issued_l1 : stat_issued_l2)++;
    return true;
  }

  void process(const champsim::modules::prefetcher& host, const sighting& seen)
  {
    const auto& pointer = seen.pointer;
    const uint64_t cursor_line = (pointer.base.to<uint64_t>() + pointer.offset.to<uint64_t>()) >> LOG2_BLOCK_SIZE;
    const uint16_t base_hash = hash16(pointer.base.to<uint64_t>());
    if (std::any_of(history.begin(), history.end(),
                    [&](const history_entry& recorded) { return recorded.valid && recorded.base_hash == base_hash && recorded.cursor_line == cursor_line; })) {
      stat_duplicates++;
      return;
    }
    stat_sightings++;
    history_entry& record = history[history_next];
    history_next = (history_next + 1) % CHT_SIZE;
    if (record.valid && record.pending_deltas) { // prefetched deltas of the overwritten sighting that were never dereferenced
      const int never_dereferenced = static_cast<int>(std::bitset<16>(record.pending_deltas).count());
      stat_predictions_not_dereferenced += static_cast<uint64_t>(never_dereferenced);
      feedback(record.key, -never_dereferenced);
    }
    record = history_entry{true, base_hash, cursor_line, seen.key, 0, 0};

    const bool mshr_ok = host.intern_->get_mshr_occupancy_ratio() < MSHR_LIMIT;
    delta_entry* entry = find_or_allocate_delta_entry(seen.key);
    for (auto& slot : entry->slots) { // the levels set at the end of the previous epoch
      if (!slot.valid || slot.status == 0)
        continue;
      const uint64_t target_line = cursor_line + static_cast<int64_t>(slot.delta);
      if (target_line == seen.own_line)
        continue;
      const bool fill_l1 = slot.status == 2 && mshr_ok;
      if (slot.status == 2 && !mshr_ok && !settings.demote) {
        stat_busy_dropped++;
        continue;
      }
      if (settings.feedback && (entry->accuracy < FEEDBACK_MIN_ISSUE || (!fill_l1 && entry->accuracy < FEEDBACK_MIN_L2))) {
        (entry->accuracy < FEEDBACK_MIN_ISSUE ? stat_feedback_skipped_all : stat_feedback_skipped_l2)++;
        continue;
      }
      if (issue(host, target_line, fill_l1, seen.key, pointer)) {
        record.pending_deltas |= static_cast<uint16_t>(1u << (slot.delta + MAX_LINE_DELTA));
        if (!fill_l1)
          l2_tracker.issue(target_line, slot.status == 1 ? l2_usage::LOWCONF : l2_usage::DEMOTED);
      }
    }
    count_sighting(entry);
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

  void train(uint64_t line, const champsim::capability& authority)
  {
    const uint16_t base_hash = hash16(authority.base.to<uint64_t>());
    history_entry* best_match = nullptr;
    int best_delta = 0;
    for (auto& recorded : history) {
      if (!recorded.valid || recorded.base_hash != base_hash)
        continue;
      const int64_t delta = static_cast<int64_t>(line) - static_cast<int64_t>(recorded.cursor_line);
      if (delta < -MAX_LINE_DELTA || delta > MAX_LINE_DELTA)
        continue;
      if (best_match == nullptr || std::abs(delta) < std::abs(best_delta)) {
        best_match = &recorded;
        best_delta = static_cast<int>(delta);
      }
    }
    if (best_match == nullptr)
      return;
    const uint16_t delta_bit = static_cast<uint16_t>(1u << (best_delta + MAX_LINE_DELTA));
    if (best_match->pending_deltas & delta_bit) { // a prefetched target was dereferenced
      best_match->pending_deltas = static_cast<uint16_t>(best_match->pending_deltas & ~delta_bit);
      stat_predictions_dereferenced++;
      feedback(best_match->key, +1);
    }
    if (best_match->trained_deltas & delta_bit)
      return;
    best_match->trained_deltas |= delta_bit;
    stat_trained++;
    train_delta(best_match->key, best_delta);
  }

public:
  // Metadata bits for the host's own prefetches triggered by an access of this IP
  static uint32_t tag_prefetch(uint64_t ip) { return MD_VALID | (static_cast<uint32_t>(ip_key(ip)) << MD_KEY_SHIFT); }

  // Every access the host sees. line is the virtual line address; cap the access's authorizing capability.
  void on_operate(uint64_t line, const champsim::capability& cap, bool cache_hit, bool useful_prefetch, access_type type, uint32_t metadata_hit)
  {
    if (type == access_type::LOAD && !cache_hit)
      l2_tracker.demand_miss(line);
    if (type != access_type::LOAD || !cap.tag)
      return;
    const bool tdp_hit = cache_hit && useful_prefetch && (metadata_hit & MD_TDP);
    stat_useful += tdp_hit;
    if (!cache_hit || tdp_hit)
      train(line, cap);
  }

  // Every fill. Scans the line's capability slots: what its tags and data carry.
  void on_fill(const champsim::modules::prefetcher& host, uint64_t line, uint64_t ip, uint32_t cpu, bool prefetch, uint32_t metadata_in, bool useless,
               uint32_t metadata_evict)
  {
    stat_useless += (useless && (metadata_evict & MD_TDP));
    uint16_t key;
    if (!prefetch) {
      if (ip == 0)
        return;
      key = ip_key(ip);
    } else {
      if (metadata_in & MD_TDP)
        stat_fills++;
      if (!(metadata_in & MD_VALID) || (metadata_in & MD_TDP))
        return; // TDP-prefetched lines are not scanned again (depth 1)
      key = static_cast<uint16_t>((metadata_in >> MD_KEY_SHIFT) & KEY_MASK);
    }
    const uint64_t line_va = line << LOG2_BLOCK_SIZE;
    for (unsigned slot = 0; slot < cheri::CAPS_PER_CL; slot++) {
      auto pointer = champsim::cap_mem[cpu].load_capability(champsim::address{line_va + slot * cheri::CAP_ALIGNMENT_BYTES});
      if (!pointer.has_value() || !pointer->tag || !cheri::has_load_permissions(pointer->permissions) || cheri::has_execute_permissions(pointer->permissions))
        continue;
      if (pointer->base.to<uint64_t>() + pointer->offset.to<uint64_t>() == 0)
        continue;
      if (sighting_queue.empty() && take_budget(host)) {
        process(host, sighting{*pointer, key, line});
        continue;
      }
      if (sighting_queue.size() >= SIGHTING_QUEUE_SIZE) {
        sighting_queue.pop_front();
        stat_queue_dropped++;
      }
      sighting_queue.push_back(sighting{*pointer, key, line});
      stat_queued++;
    }
  }

  void on_cycle(const champsim::modules::prefetcher& host)
  {
    while (!sighting_queue.empty() && take_budget(host)) {
      process(host, sighting_queue.front());
      sighting_queue.pop_front();
    }
    if (settings.interval) {
      const uint64_t now = host.intern_->current_cycle();
      if (now >= next_interval) {
        if (next_interval != 0) {
          const interval_counters current = current_counters();
          std::cout << "TDP INTERVAL cycle " << now << " issued_l1 " << current.issued_l1 - last_interval.issued_l1 << " l2_lowconf "
                    << current.l2_lowconf - last_interval.l2_lowconf << " l2_demoted " << current.l2_demoted - last_interval.l2_demoted << " busy_drop "
                    << current.busy_dropped - last_interval.busy_dropped << " fills " << current.fills - last_interval.fills << " useful "
                    << current.useful - last_interval.useful << " useless " << current.useless - last_interval.useless << " l2_used "
                    << current.l2_used - last_interval.l2_used << " pred_hit " << current.predictions_dereferenced - last_interval.predictions_dereferenced
                    << " pred_miss " << current.predictions_not_dereferenced - last_interval.predictions_not_dereferenced << " fb_skip "
                    << current.feedback_skipped - last_interval.feedback_skipped << std::endl;
          last_interval = current;
        }
        next_interval = now + settings.interval;
      }
    }
  }

  void print_stats(std::ostream& out) const
  {
    print_config(out, threshold_l2);
    out << "TDP SIGHTINGS " << stat_sightings << " DUPLICATE " << stat_duplicates << " QUEUED " << stat_queued << " QUEUE_DROPPED " << stat_queue_dropped
        << std::endl;
    out << "TDP TRAIN " << stat_trained << std::endl;
    out << "TDP ISSUED L1 " << stat_issued_l1 << " L2 " << stat_issued_l2 << " PQ_FULL " << stat_pq_full << " RECENT_SKIP " << stat_recent_skipped
        << std::endl;
    out << "TDP FILLS " << stat_fills << " USEFUL " << stat_useful << " USELESS " << stat_useless << std::endl;
    out << "TDP PREDICTIONS dereferenced " << stat_predictions_dereferenced << " not_dereferenced " << stat_predictions_not_dereferenced << " BUSY_DROP "
        << stat_busy_dropped << " FB_SKIP all " << stat_feedback_skipped_all << " l2 " << stat_feedback_skipped_l2 << std::endl;
    l2_tracker.print(out);
  }
};
} // namespace cheri_tdp

#endif
