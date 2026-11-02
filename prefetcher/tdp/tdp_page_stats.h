#ifndef TDP_PAGE_STATS_H
#define TDP_PAGE_STATS_H

// Same-page statistics for tag-directed prefetching (statistics only, not hardware; no effect on timing). For every
// TDP prefetch issued: is its target line on the virtual page (4 KB, 2 MB) of the line the pointer was read from?
// Split by depth (1 = first hop from a demand or host-prefetch fill; 2+ = chained by recursion) and level (L1D-filling
// vs L2-only); and, for L1D-filling prefetches, the same split at their outcome (first demand use, or eviction unused).
// The flags of each issued line live in maps keyed by line (not in the prefetch metadata, which the hosts use in full):
// recorded at issue, moved at the TDP fill, consumed at first use or unused eviction. A later issue to the same line
// overwrites a pending entry; an issue the cache drops (line already present) cannot touch a resident line's entry.
// Hosts: on_issue, on_tdp_fill, on_used, on_unused_eviction at the matching sites; reset_counts when warmup ends (the
// maps are kept, so lines issued in warmup are still classified at their outcome); print at the end.

#include <cstdint>
#include <ostream>
#include <string>
#include <unordered_map>

#include "champsim.h"

namespace tdp_page_stats
{
constexpr unsigned LOG2_PAGE_4K = 12, LOG2_PAGE_2M = 21;

struct page_count {
  uint64_t prefetches = 0, same_4k = 0, same_2m = 0;
  void add(bool on_same_4k, bool on_same_2m)
  {
    prefetches++;
    same_4k += on_same_4k;
    same_2m += on_same_2m;
  }
  page_count& operator+=(const page_count& other)
  {
    prefetches += other.prefetches;
    same_4k += other.same_4k;
    same_2m += other.same_2m;
    return *this;
  }
};

class recorder
{
  enum { DEPTH_FIRST, DEPTH_CHAINED, NUM_DEPTH_CLASSES };
  enum { LEVEL_L2_ONLY, LEVEL_L1D, NUM_LEVELS };
  struct line_flags {
    bool same_4k = false, same_2m = false;
    unsigned depth_class = DEPTH_FIRST;
  };
  std::unordered_map<uint64_t, line_flags> issued_to_l1d; // L1D-filling prefetches issued, not yet filled
  std::unordered_map<uint64_t, line_flags> resident;      // filled into L1D, not yet used or evicted
  page_count issued[NUM_DEPTH_CLASSES][NUM_LEVELS] = {}, used[NUM_DEPTH_CLASSES] = {}, unused[NUM_DEPTH_CLASSES] = {};
  uint64_t untracked_used = 0, untracked_unused = 0, fills_without_issue = 0;

  static bool same_page(uint64_t target_line, uint64_t pointer_line, unsigned log2_page)
  {
    return ((target_line ^ pointer_line) >> (log2_page - LOG2_BLOCK_SIZE)) == 0;
  }
  void consume(uint64_t line, page_count* outcome, uint64_t& untracked)
  {
    auto entry = resident.find(line);
    if (entry == resident.end()) {
      untracked++;
      return;
    }
    outcome[entry->second.depth_class].add(entry->second.same_4k, entry->second.same_2m);
    resident.erase(entry);
  }
  static void print_line(std::ostream& out, const char* what, const page_count& count)
  {
    out << "TDP_PAGE_STATS " << what << " prefetches " << count.prefetches << " same_4KB " << count.same_4k << " same_2MB " << count.same_2m << std::endl;
  }

public:
  // target_line, pointer_line: virtual line addresses; depth: 1 = first hop
  void on_issue(uint64_t target_line, uint64_t pointer_line, unsigned depth, bool fill_l1d)
  {
    line_flags flags{same_page(target_line, pointer_line, LOG2_PAGE_4K), same_page(target_line, pointer_line, LOG2_PAGE_2M),
                     depth >= 2 ? unsigned{DEPTH_CHAINED} : unsigned{DEPTH_FIRST}};
    issued[flags.depth_class][fill_l1d ? LEVEL_L1D : LEVEL_L2_ONLY].add(flags.same_4k, flags.same_2m);
    if (fill_l1d)
      issued_to_l1d[target_line] = flags;
  }
  // A TDP prefetch filled this line into L1D
  void on_tdp_fill(uint64_t line)
  {
    auto entry = issued_to_l1d.find(line);
    if (entry == issued_to_l1d.end()) {
      fills_without_issue++;
      return;
    }
    resident[line] = entry->second;
    issued_to_l1d.erase(entry);
  }
  void on_used(uint64_t line) { consume(line, used, untracked_used); }
  void on_unused_eviction(uint64_t line) { consume(line, unused, untracked_unused); }

  void reset_counts()
  {
    for (auto& by_level : issued)
      for (auto& count : by_level)
        count = page_count{};
    for (auto& count : used)
      count = page_count{};
    for (auto& count : unused)
      count = page_count{};
    untracked_used = untracked_unused = fills_without_issue = 0;
  }

  void print(std::ostream& out, const char* host_name) const
  {
    out << "TDP_PAGE_STATS begin " << host_name
        << " (region of interest; target line on the same virtual page as the line holding the pointer; depth1 = first hop, depth2+ = recursion)"
        << std::endl;
    const char* depth_name[NUM_DEPTH_CLASSES] = {"depth1", "depth2+"};
    const char* level_name[NUM_LEVELS] = {"l2_only", "l1d"};
    page_count issued_all, used_all, unused_all;
    for (unsigned depth_class = 0; depth_class < NUM_DEPTH_CLASSES; depth_class++)
      for (unsigned level = 0; level < NUM_LEVELS; level++) {
        print_line(out, (std::string("issued ") + depth_name[depth_class] + " " + level_name[level]).c_str(), issued[depth_class][level]);
        issued_all += issued[depth_class][level];
      }
    print_line(out, "issued all all", issued_all);
    for (unsigned depth_class = 0; depth_class < NUM_DEPTH_CLASSES; depth_class++) {
      print_line(out, (std::string("used ") + depth_name[depth_class] + " l1d").c_str(), used[depth_class]);
      used_all += used[depth_class];
    }
    print_line(out, "used all l1d", used_all);
    for (unsigned depth_class = 0; depth_class < NUM_DEPTH_CLASSES; depth_class++) {
      print_line(out, (std::string("unused ") + depth_name[depth_class] + " l1d").c_str(), unused[depth_class]);
      unused_all += unused[depth_class];
    }
    print_line(out, "unused all l1d", unused_all);
    out << "TDP_PAGE_STATS untracked used " << untracked_used << " unused " << untracked_unused << " fills_without_issue " << fills_without_issue
        << std::endl;
    out << "TDP_PAGE_STATS end" << std::endl;
  }
};
} // namespace tdp_page_stats

#endif
