#ifndef CHERI_OOB_FILTER_H
#define CHERI_OOB_FILTER_H

// Learned confidence for out-of-bounds (OOB) prefetch candidates, keyed by (object size class, IP), shared by berti_cap,
// spp_cap and ampm_cap. Each prefetcher owns its own OobConfidence instance.

#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <string>

#include "cheri.h"
#include "cheri_large_object.h" // print_config also reports BOOST_ENABLED and BOOST_MIN_OBJ_BYTES
#include "cheri_prefetch_utils.h"

namespace cheri_policy {

// Ablation switch
constexpr bool OOB_GATE_ENABLED = true; // false: OOB candidates are issued as the unmodified prefetcher would (still tracked)

// OOB confidence
constexpr bool OOB_KEY_BY_IP = true; // false: key by size class only
constexpr unsigned NUM_CLASSES = 4;  // <=128B, <=4KB, <=64KB, >64KB
constexpr unsigned IP_SLOTS = 64;
constexpr int OOB_CTR_MAX = 31; // 5-bit counters
constexpr int OOB_CTR_INIT = 16;
constexpr int OOB_HIGH = 20; // >= HIGH: issue at this level
constexpr int OOB_LOW = 12;  // >= LOW: issue to the next level down; below: drop
constexpr int OOB_INC = 2;   // demand touched a tracked line
constexpr int OOB_DEC = 1;   // tracked line expired untouched (issued or shadow alike)

// Tracker of OOB candidates
constexpr std::size_t TRACKER_SETS = 256;
constexpr std::size_t TRACKER_WAYS = 4;
constexpr unsigned TRACKER_TAG_BITS = 16;

inline unsigned size_class(uint64_t length)
{
  if (length <= (uint64_t{1} << 7))
    return 0;
  if (length <= (uint64_t{1} << 12))
    return 1;
  if (length <= (uint64_t{1} << 16))
    return 2;
  return 3;
}

inline const char* size_class_name(unsigned cls)
{
  static const char* names[NUM_CLASSES] = {"0-128B", "128B-4KB", "4KB-64KB", ">64KB"};
  return names[cls];
}

enum class oob_action { ISSUE_HERE, ISSUE_LOWER, DROP };

class OobConfidence
{
public:
  struct key {
    unsigned cls = 0;
    unsigned slot = 0;
    bool class_only = true;
  };

  // IP 0 (e.g. an L1D prefetch arriving at L2C) always uses the class-only counters.
  static key make_key(unsigned cls, uint64_t ip)
  {
    if (!OOB_KEY_BY_IP || ip == 0)
      return {cls, 0, true};
    uint64_t h = ip ^ (ip >> 6) ^ (ip >> 12) ^ (ip >> 18);
    return {cls, static_cast<unsigned>(h % IP_SLOTS), false};
  }

  oob_action decide(key k) const
  {
    if (!OOB_GATE_ENABLED)
      return oob_action::ISSUE_HERE;
    int c = counter(k);
    if (c >= OOB_HIGH)
      return oob_action::ISSUE_HERE;
    if (c >= OOB_LOW)
      return oob_action::ISSUE_LOWER;
    return oob_action::DROP;
  }

  // Record an OOB candidate and the action taken for it. A DROP is tracked as a shadow entry.
  // A line that is already tracked is only upgraded shadow -> issued and counted as a repeat: re-proposals must not
  // extend its lifetime or inflate the candidate/decision stats.
  void record(uint64_t line, key k, oob_action act)
  {
    auto& s = stats[k.cls][k.class_only];
    const bool shadow = (act == oob_action::DROP);
    auto& set = tracker[line % TRACKER_SETS];
    const uint16_t tag = make_tag(line);

    for (auto& e : set) {
      if (e.valid && e.tag == tag) {
        e.shadow = e.shadow && shadow;
        s.repeat++;
        return;
      }
    }

    s.candidates++;
    if (act == oob_action::ISSUE_HERE)
      s.issued_here++;
    else if (act == oob_action::ISSUE_LOWER)
      s.issued_lower++;
    else
      s.dropped++;
    ++lru_clock;

    auto victim = &set[0];
    for (auto& e : set) {
      if (!e.valid) {
        victim = &e;
        break;
      }
      if (e.lru < victim->lru)
        victim = &e;
    }
    if (victim->valid) {
      auto& vs = stats[victim->cls][victim->class_only];
      if (victim->shadow)
        vs.expired_shadow++;
      else
        vs.expired_issued++;
      update(entry_key(*victim), -OOB_DEC);
    }
    *victim = entry{tag, static_cast<uint8_t>(k.cls), static_cast<uint8_t>(k.slot), k.class_only, shadow, true, lru_clock};
  }

  // A demand (LOAD/RFO) access to line: credit and retire a tracked candidate.
  void on_demand(uint64_t line)
  {
    auto& set = tracker[line % TRACKER_SETS];
    const uint16_t tag = make_tag(line);
    for (auto& e : set) {
      if (e.valid && e.tag == tag) {
        auto& s = stats[e.cls][e.class_only];
        if (e.shadow)
          s.used_shadow++;
        else
          s.used_issued++;
        update(entry_key(e), OOB_INC);
        e.valid = false;
        return;
      }
    }
  }

  static void print_config(std::ostream& os)
  {
    os << "  OOB_GATE_ENABLED: " << OOB_GATE_ENABLED << "  BOOST_ENABLED: " << BOOST_ENABLED << "\n"
       << "  OOB_KEY_BY_IP: " << OOB_KEY_BY_IP << "  IP_SLOTS: " << IP_SLOTS << "  CTR_MAX: " << OOB_CTR_MAX << "  CTR_INIT: " << OOB_CTR_INIT
       << "  HIGH: " << OOB_HIGH << "  LOW: " << OOB_LOW << "  INC: " << OOB_INC << "  DEC: " << OOB_DEC << "\n"
       << "  TRACKER_SETS: " << TRACKER_SETS << "  TRACKER_WAYS: " << TRACKER_WAYS << "  TRACKER_TAG_BITS: " << TRACKER_TAG_BITS << "\n"
       << "  BOOST_MIN_OBJ_BYTES: " << BOOST_MIN_OBJ_BYTES << "\n";
  }

  void print_stats(std::ostream& os) const
  {
    os << "  OOB confidence (key: IP = (class, IP hash), CLASS = class only)\n"
       << "  Class      Key    Candidates   IssuedHere  IssuedLower      Dropped   UsedIssued   UsedShadow ExpiredIssued ExpiredShadow       Repeat\n";
    for (unsigned c = 0; c < NUM_CLASSES; c++) {
      for (unsigned m = 0; m < 2; m++) {
        const auto& s = stats[c][m];
        if (s.candidates == 0 && s.repeat == 0)
          continue;
        os << "  " << size_class_name(c) << std::string(10 - std::string(size_class_name(c)).size(), ' ') << (m ? " CLASS" : " IP   ") << " " << pad(s.candidates)
           << pad(s.issued_here) << pad(s.issued_lower) << pad(s.dropped) << pad(s.used_issued) << pad(s.used_shadow) << pad(s.expired_issued, 14)
           << pad(s.expired_shadow, 14) << pad(s.repeat) << "\n";
      }
    }
    os << "  Class-only counters:";
    for (unsigned c = 0; c < NUM_CLASSES; c++)
      os << " " << size_class_name(c) << "=" << class_ctr[c];
    os << "\n";
  }

private:
  struct entry {
    uint16_t tag = 0;
    uint8_t cls = 0;
    uint8_t slot = 0;
    bool class_only = true;
    bool shadow = false;
    bool valid = false;
    uint64_t lru = 0;
  };

  struct class_stats {
    uint64_t candidates = 0, issued_here = 0, issued_lower = 0, dropped = 0;
    uint64_t used_issued = 0, used_shadow = 0, expired_issued = 0, expired_shadow = 0;
    uint64_t repeat = 0; // re-records of an already-tracked line
  };

  static uint16_t make_tag(uint64_t line) { return static_cast<uint16_t>((line / TRACKER_SETS) & ((uint64_t{1} << TRACKER_TAG_BITS) - 1)); }
  static key entry_key(const entry& e) { return {e.cls, e.slot, e.class_only}; }
  static std::string pad(uint64_t v, std::size_t w = 13)
  {
    auto s = std::to_string(v);
    return std::string(s.size() < w ? w - s.size() : 1, ' ') + s;
  }

  int counter(key k) const { return k.class_only ? class_ctr[k.cls] : ip_ctr[k.cls][k.slot]; }
  void update(key k, int d)
  {
    int& c = k.class_only ? class_ctr[k.cls] : ip_ctr[k.cls][k.slot];
    c = std::min(OOB_CTR_MAX, std::max(0, c + d));
  }

  std::array<std::array<int, IP_SLOTS>, NUM_CLASSES> ip_ctr = [] {
    std::array<std::array<int, IP_SLOTS>, NUM_CLASSES> a{};
    for (auto& row : a)
      row.fill(OOB_CTR_INIT);
    return a;
  }();
  std::array<int, NUM_CLASSES> class_ctr = {OOB_CTR_INIT, OOB_CTR_INIT, OOB_CTR_INIT, OOB_CTR_INIT};
  std::array<std::array<entry, TRACKER_WAYS>, TRACKER_SETS> tracker{};
  uint64_t lru_clock = 0;
  std::array<std::array<class_stats, 2>, NUM_CLASSES> stats{};
};

} // namespace cheri_policy

#endif