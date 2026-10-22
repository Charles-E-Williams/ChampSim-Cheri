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

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <iostream>

#include "cache.h"
#include "capability_memory.h"
#include "cheri_prefetch_utils.h"
#include "modules.h"

namespace cheri_tdp
{
constexpr std::size_t CHT_SIZE = 128;   // Capability History Table entries (FIFO)
constexpr std::size_t PDT_SIZE = 64;    // Pointer Delta Table entries (fully associative, LRU)
constexpr std::size_t PDT_DELTAS = 4;   // deltas per PDT entry
constexpr std::size_t SIGHTQ = 8;       // sighting queue entries (oldest dropped when full)
constexpr unsigned SIGHT_PER_CYCLE = 2; // sightings processed per cycle
constexpr unsigned EPOCH = 16;          // sightings per confidence epoch
constexpr unsigned TH_L1 = 8;           // dereferences per epoch for an L1D-level delta
constexpr unsigned TH_L2 = 4;           // for an L2-level delta
constexpr int MAX_LINE_DELTA = 7;       // |dereferenced line - cursor line| learned
constexpr std::size_t RECENT = 32;      // recently issued TDP targets (duplicate filter)
constexpr double MSHR_LIMIT = 0.70;     // L1D-level TDP prefetches only below this MSHR occupancy (as Berti)

// Prefetch metadata: [19] TDP prefetch, [20] key valid, [31:21] key (11-bit IP hash)
constexpr uint32_t MD_TDP = 1u << 19;
constexpr uint32_t MD_VALID = 1u << 20;
constexpr unsigned MD_KEY_SHIFT = 21;
constexpr uint32_t KEY_MASK = 0x7FF;
constexpr uint32_t MD_MASK = MD_TDP | MD_VALID | (KEY_MASK << MD_KEY_SHIFT);

inline uint16_t hash16(uint64_t x)
{
  x ^= x >> 33;
  x *= 0xff51afd7ed558ccdULL;
  x ^= x >> 29;
  return static_cast<uint16_t>(x ^ (x >> 16) ^ (x >> 32) ^ (x >> 48));
}

inline uint16_t ip_key(uint64_t ip) { return static_cast<uint16_t>(hash16(ip) & KEY_MASK); }

class engine
{
  struct cht_entry {
    bool valid = false;
    uint16_t base_hash = 0;   // hash of the pointer's base: names the pointed-to object
    uint64_t cursor_line = 0; // the pointer's cursor line
    uint16_t key = 0;
    uint16_t trained = 0;     // deltas already credited for this sighting, one bit per delta
  };
  struct pdt_delta {
    bool valid = false;
    int8_t delta = 0;
    uint8_t conf = 0;
    uint8_t status = 0; // 0 none, 1 prefetch to L2, 2 prefetch to L1D
  };
  struct pdt_entry {
    bool valid = false;
    uint16_t tag = 0;
    uint8_t sightings = 0;
    uint64_t lru = 0;
    std::array<pdt_delta, PDT_DELTAS> d{};
  };
  struct sighting {
    champsim::capability cap;
    uint16_t key;
    uint64_t own_line; // the line the pointer was read from
  };

  std::array<cht_entry, CHT_SIZE> cht{};
  std::size_t cht_ptr = 0;
  std::array<pdt_entry, PDT_SIZE> pdt{};
  uint64_t pdt_clock = 0;
  std::deque<sighting> sq;
  uint64_t budget_cycle = ~uint64_t{0};
  unsigned budget_used = 0;
  std::array<uint64_t, RECENT> recent{};
  std::size_t recent_ptr = 0;

  // statistics (not hardware)
  uint64_t st_sightings = 0, st_dup = 0, st_queued = 0, st_q_dropped = 0;
  uint64_t st_train = 0, st_issue_l1 = 0, st_issue_l2 = 0, st_pq_full = 0, st_recent_skip = 0;
  uint64_t st_fills = 0, st_useful = 0, st_useless = 0;

  pdt_entry* pdt_find(uint16_t tag)
  {
    for (auto& e : pdt)
      if (e.valid && e.tag == tag) {
        e.lru = ++pdt_clock;
        return &e;
      }
    return nullptr;
  }

  pdt_entry* pdt_alloc(uint16_t tag)
  {
    if (auto* e = pdt_find(tag))
      return e;
    auto victim = std::min_element(pdt.begin(), pdt.end(), [](const auto& a, const auto& b) { return a.valid != b.valid ? !a.valid : a.lru < b.lru; });
    *victim = pdt_entry{};
    victim->valid = true;
    victim->tag = tag;
    victim->lru = ++pdt_clock;
    return &*victim;
  }

  void pdt_count_sighting(pdt_entry* e)
  {
    if (++e->sightings < EPOCH)
      return;
    for (auto& d : e->d) {
      if (!d.valid)
        continue;
      d.status = (d.conf >= TH_L1) ? 2 : (d.conf >= TH_L2) ? 1 : 0;
      d.conf = 0;
    }
    e->sightings = 0;
  }

  void pdt_train(uint16_t key, int delta)
  {
    pdt_entry* e = pdt_find(key);
    if (e == nullptr)
      return;
    for (auto& d : e->d)
      if (d.valid && d.delta == delta) {
        d.conf = static_cast<uint8_t>(std::min<unsigned>(d.conf + 1u, EPOCH));
        return;
      }
    pdt_delta* victim = nullptr; // a free slot, else the least confident slot without a prefetch level
    for (auto& d : e->d)
      if (!d.valid) {
        victim = &d;
        break;
      }
    if (victim == nullptr)
      for (auto& d : e->d)
        if (d.status == 0 && (victim == nullptr || d.conf < victim->conf))
          victim = &d;
    if (victim == nullptr)
      return;
    *victim = pdt_delta{};
    victim->valid = true;
    victim->delta = static_cast<int8_t>(delta);
    victim->conf = 1;
  }

  void issue(const champsim::modules::prefetcher& host, uint64_t line, bool fill_l1, uint16_t key, const champsim::capability& c)
  {
    if (std::find(recent.begin(), recent.end(), line) != recent.end()) {
      st_recent_skip++;
      return;
    }
    const uint32_t md = MD_TDP | MD_VALID | (static_cast<uint32_t>(key & KEY_MASK) << MD_KEY_SHIFT);
    if (!host.prefetch_line(champsim::address{line << LOG2_BLOCK_SIZE}, fill_l1, md, c)) {
      st_pq_full++;
      return;
    }
    recent[recent_ptr] = line;
    recent_ptr = (recent_ptr + 1) % RECENT;
    (fill_l1 ? st_issue_l1 : st_issue_l2)++;
  }

  void process(const champsim::modules::prefetcher& host, const sighting& s)
  {
    const auto& c = s.cap;
    const uint64_t cline = (c.base.to<uint64_t>() + c.offset.to<uint64_t>()) >> LOG2_BLOCK_SIZE;
    const uint16_t bh = hash16(c.base.to<uint64_t>());
    if (std::any_of(cht.begin(), cht.end(), [&](const cht_entry& x) { return x.valid && x.base_hash == bh && x.cursor_line == cline; })) {
      st_dup++;
      return;
    }
    st_sightings++;
    cht[cht_ptr] = cht_entry{true, bh, cline, s.key, 0};
    cht_ptr = (cht_ptr + 1) % CHT_SIZE;

    const bool mshr_ok = host.intern_->get_mshr_occupancy_ratio() < MSHR_LIMIT;
    pdt_entry* e = pdt_alloc(s.key);
    for (auto& d : e->d) { // the levels set at the end of the previous epoch
      if (!d.valid || d.status == 0)
        continue;
      const uint64_t tline = cline + static_cast<int64_t>(d.delta);
      if (tline != s.own_line)
        issue(host, tline, d.status == 2 && mshr_ok, s.key, c);
    }
    pdt_count_sighting(e);
  }

  bool take_budget(const champsim::modules::prefetcher& host)
  {
    const uint64_t now = host.intern_->current_cycle();
    if (now != budget_cycle) {
      budget_cycle = now;
      budget_used = 0;
    }
    if (budget_used >= SIGHT_PER_CYCLE)
      return false;
    budget_used++;
    return true;
  }

  void train(uint64_t line, const champsim::capability& cap)
  {
    const uint16_t bh = hash16(cap.base.to<uint64_t>());
    cht_entry* best = nullptr;
    int best_d = 0;
    for (auto& ce : cht) {
      if (!ce.valid || ce.base_hash != bh)
        continue;
      const int64_t d = static_cast<int64_t>(line) - static_cast<int64_t>(ce.cursor_line);
      if (d < -MAX_LINE_DELTA || d > MAX_LINE_DELTA)
        continue;
      if (best == nullptr || std::abs(d) < std::abs(best_d)) {
        best = &ce;
        best_d = static_cast<int>(d);
      }
    }
    if (best == nullptr)
      return;
    const uint16_t bit = static_cast<uint16_t>(1u << (best_d + MAX_LINE_DELTA));
    if (best->trained & bit)
      return;
    best->trained |= bit;
    st_train++;
    pdt_train(best->key, best_d);
  }

public:
  // Metadata bits for the host's own prefetches triggered by an access of this IP
  static uint32_t tag_prefetch(uint64_t ip) { return MD_VALID | (static_cast<uint32_t>(ip_key(ip)) << MD_KEY_SHIFT); }

  // Every access the host sees. line is the virtual line address; cap the access's authorizing capability.
  void on_operate(uint64_t line, const champsim::capability& cap, bool cache_hit, bool useful_prefetch, access_type type, uint32_t metadata_hit)
  {
    if (type != access_type::LOAD || !cap.tag)
      return;
    const bool tdp_hit = cache_hit && useful_prefetch && (metadata_hit & MD_TDP);
    st_useful += tdp_hit;
    if (!cache_hit || tdp_hit)
      train(line, cap);
  }

  // Every fill. Scans the line's capability slots: what its tags and data carry.
  void on_fill(const champsim::modules::prefetcher& host, uint64_t line, uint64_t ip, uint32_t cpu, bool prefetch, uint32_t metadata_in, bool useless,
               uint32_t metadata_evict)
  {
    st_useless += (useless && (metadata_evict & MD_TDP));
    uint16_t key;
    if (!prefetch) {
      if (ip == 0)
        return;
      key = ip_key(ip);
    } else {
      if (metadata_in & MD_TDP)
        st_fills++;
      if (!(metadata_in & MD_VALID) || (metadata_in & MD_TDP))
        return; // TDP-prefetched lines are not scanned again (depth 1)
      key = static_cast<uint16_t>((metadata_in >> MD_KEY_SHIFT) & KEY_MASK);
    }
    const uint64_t line_va = line << LOG2_BLOCK_SIZE;
    for (unsigned slot = 0; slot < cheri::CAPS_PER_CL; slot++) {
      auto c = champsim::cap_mem[cpu].load_capability(champsim::address{line_va + slot * cheri::CAP_ALIGNMENT_BYTES});
      if (!c.has_value() || !c->tag || !cheri::has_load_permissions(c->permissions) || cheri::has_execute_permissions(c->permissions))
        continue;
      if (c->base.to<uint64_t>() + c->offset.to<uint64_t>() == 0)
        continue;
      if (sq.empty() && take_budget(host)) {
        process(host, sighting{*c, key, line});
        continue;
      }
      if (sq.size() >= SIGHTQ) {
        sq.pop_front();
        st_q_dropped++;
      }
      sq.push_back(sighting{*c, key, line});
      st_queued++;
    }
  }

  void on_cycle(const champsim::modules::prefetcher& host)
  {
    while (!sq.empty() && take_budget(host)) {
      process(host, sq.front());
      sq.pop_front();
    }
  }

  void print_stats(std::ostream& os) const
  {
    os << "TDP SIGHTINGS " << st_sightings << " DUPLICATE " << st_dup << " QUEUED " << st_queued << " QUEUE_DROPPED " << st_q_dropped << std::endl;
    os << "TDP TRAIN " << st_train << std::endl;
    os << "TDP ISSUED L1 " << st_issue_l1 << " L2 " << st_issue_l2 << " PQ_FULL " << st_pq_full << " RECENT_SKIP " << st_recent_skip << std::endl;
    os << "TDP FILLS " << st_fills << " USEFUL " << st_useful << " USELESS " << st_useless << std::endl;
  }
};
} // namespace cheri_tdp

#endif
