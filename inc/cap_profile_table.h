// ============================================================================
// Capability Access Profile Table
// ============================================================================
// Per-object access pattern classifier and feedback table indexed by
// capability identity: hash(base, length).  Any CHERI-aware prefetcher
// can call update() from its cache_operate and query predict() / classify()
// to steer prefetch strategy.
//
// Includes ASD-derived stream depth histograms (Hur & Lin, MICRO'06 /
// HPCA'09) for probabilistic prefetch depth decisions.
//
// Three actionable patterns:
//   STRIDED       — stable stride, predictable (IP-Stride / Next-Line)
//   POINTER_CHASE — tagged caps + irregular access (pointer chaser)
//   IRREGULAR     — everything else (AMPM / SMS territory)
//
// Hardware budget:
//   Entry: 23 bytes
//   Table: 256 sets × 4 ways × 23B = 23 KB
//   Histogram: 2 × 64 × 8B = 1 KB
//   Total: ~24 KB
// ============================================================================

#ifndef CAP_PROFILE_TABLE_H
#define CAP_PROFILE_TABLE_H

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <optional>

#include "address.h"
#include "champsim.h"
#include "msl/lru_table.h"
#include "cheri.h"
#include "capability_memory.h"

namespace cheri {

// ============================================================================
// Access pattern classification
// ============================================================================

enum class AccessPattern : uint8_t {
  UNKNOWN       = 0,  // insufficient observations
  STRIDED       = 1,  // stable stride (unit or non-unit) — predictable
  POINTER_CHASE = 2,  // tagged caps in data AND irregular access
  IRREGULAR     = 3,  // everything else — spatial prefetcher territory
  NUM_PATTERNS
};

inline constexpr std::array<const char*, static_cast<std::size_t>(AccessPattern::NUM_PATTERNS)>
    access_pattern_names{{
        "UNKNOWN", "STRIDED", "POINTER_CHASE", "IRREGULAR"
    }};

// ============================================================================
// StreamDepthHistogram — from ASD (Hur & Lin, MICRO'06 / HPCA'09)
// ============================================================================
// bins[i] = count of observed streams reaching depth >= (i+1).
// Decision: prefetch k more lines from position i iff
//   lht(i) < factor * lht(i + k)
// ============================================================================

struct StreamDepthHistogram {
  static constexpr std::size_t MAX_BINS = 64;

  uint64_t bins[MAX_BINS]{};
  uint64_t total_streams{};
  double   factor{2.0};

  void record_stream(std::size_t depth) {
    if (depth == 0) return;
    total_streams++;
    std::size_t cap = std::min(depth, MAX_BINS);
    for (std::size_t i = 0; i < cap; i++)
      bins[i]++;
  }

  std::size_t get_depth_from(std::size_t i, std::size_t max_prefetch) const {
    if (i == 0 || i > MAX_BINS) return 0;
    std::size_t depth = 0;
    while (depth < max_prefetch
           && (i + depth) < MAX_BINS
           && bins[i - 1] < static_cast<uint64_t>(factor * static_cast<double>(bins[i -1 + depth])))
      depth++;
    return depth;
  }

  void clear() {
    std::memset(bins, 0, sizeof(bins));
    total_streams = 0;
  }

  double compare(const StreamDepthHistogram& other) const {
    if (total_streams == 0 && other.total_streams == 0) return 0.0;
    if (total_streams == 0 || other.total_streams == 0) return 1.0;
    double norm = static_cast<double>(other.total_streams)
                  / static_cast<double>(total_streams);
    double diff = 0;
    for (std::size_t i = 0; i < MAX_BINS; i++)
      diff += std::abs(static_cast<double>(other.bins[i])
                       - static_cast<double>(bins[i]) * norm);
    return diff / static_cast<double>(other.total_streams);
  }
};

// ============================================================================
// EpochStateMachine — from Hur & Lin HPCA'09 Fig 4
// ============================================================================

struct EpochStateMachine {
  enum State : uint8_t { SAME1, SAME2, HALF1, HALF2, DOUBLE1, DOUBLE2 };
  State state{SAME1};

  uint64_t update(bool dissimilar, uint64_t epoch) {
    switch (state) {
      case SAME1:
        state = dissimilar ? DOUBLE1 : SAME1;
        return dissimilar ? (epoch << 1) : epoch;
      case SAME2:
        state = dissimilar ? HALF1   : SAME2;
        return dissimilar ? (epoch >> 1) : epoch;
      case HALF1:
        state = dissimilar ? DOUBLE1 : HALF2;
        return dissimilar ? (epoch << 1) : (epoch >> 1);
      case HALF2:
        state = dissimilar ? DOUBLE1 : SAME1;
        return dissimilar ? (epoch << 1) : epoch;
      case DOUBLE1:
        state = dissimilar ? HALF1   : DOUBLE2;
        return dissimilar ? (epoch >> 1) : (epoch << 1);
      case DOUBLE2:
        state = dissimilar ? HALF1   : SAME2;
        return dissimilar ? (epoch >> 1) : epoch;
    }
    return epoch;
  }
};

// ============================================================================
// ObjectPredictability — returned by predict()
// ============================================================================

struct ObjectPredictability {
  bool          stride_predictable{};
  int16_t       predicted_stride{};
  bool          depth_predictable{};
  std::size_t   advised_depth{};
  double        miss_rate{-1.0};   // [0.0, 1.0] or -1 if insufficient data
  AccessPattern pattern{AccessPattern::UNKNOWN};
};

// ============================================================================
// Profile entry — 23 bytes
// ============================================================================

struct CapProfileEntry {

  // === Key ===
  uint64_t cap_hash{};          // 8B — object identity hash(base, length)

  // === Stride computation ===
  int32_t  last_cl_offset{};    // 4B — last capability-relative CL offset
  int16_t  dominant_stride{};   // 2B — confirmed stride (CL units)

  // === Stream depth (histogram feed) ===
  uint16_t stream_depth{};      // 2B — consecutive same-stride accesses

  // === Stride confidence [0, 7] saturating ===
  uint8_t  stride_stability{};  // 1B — up on match, down on mismatch
  uint8_t  stride_variation{};  // 1B — up on mismatch, down on match

  // === Temporal hotspot detection ===
  uint8_t  zero_stride_count{}; // 1B — consecutive zero-stride (sat 255)

  // === Pointer scan ===
  bool     has_tagged_caps{};   // 1B — found tagged caps in cacheline data
  uint8_t  ptr_scan_count{};    // 1B — scans performed (gates further scans)

  // === Observation + feedback ===
  uint8_t  access_count{};      // 1B — saturating; both counters shift
  uint8_t  miss_count{};        // 1B — right together at 255 to preserve ratio

  // Total: 23 bytes

  // === lru_table interface ===
  auto index() const
  {
    using namespace champsim::data::data_literals;
    return champsim::address{cap_hash}.slice_upper<2_b>();
  }
  auto tag() const
  {
    using namespace champsim::data::data_literals;
    return champsim::address{cap_hash}.slice_upper<2_b>();
  }
};


// ============================================================================
// CapProfileTable
// ============================================================================

class CapProfileTable {
public:
  // --- Table geometry ---
  static constexpr std::size_t SETS = 32;
  static constexpr std::size_t WAYS = 4;

  // --- Pointer scan limits ---
  static constexpr uint8_t  MAX_PTR_SCANS     = 4;
  static constexpr unsigned CAP_SLOTS_PER_CL   = 4;  // 64B / 16B per cap

  // --- Classification thresholds ---
  static constexpr uint8_t  MIN_OBSERVATIONS     = 4;
  static constexpr uint8_t  STRIDE_CONF_THRESH   = 3;

  // --- ASD epoch bounds ---
  static constexpr uint64_t EPOCH_MAX        = 8192;
  static constexpr uint64_t EPOCH_MIN        = 256;
  static constexpr double   EPOCH_DIFF_THRESH = 0.1;


  static constexpr uint8_t ZERO_STRIDE_TIMEOUT = 8;

private:
  champsim::msl::lru_table<CapProfileEntry> table_{SETS, WAYS};

  // =========================================================================
  // ASD histogram machinery
  // =========================================================================

  StreamDepthHistogram active_hist_asc_{};
  StreamDepthHistogram active_hist_desc_{};
  StreamDepthHistogram build_hist_asc_{};
  StreamDepthHistogram build_hist_desc_{};
  EpochStateMachine    epoch_sm_asc_{};
  EpochStateMachine    epoch_sm_desc_{};

  uint64_t epoch_length_asc_{EPOCH_MAX};
  uint64_t epoch_length_desc_{EPOCH_MAX};
  uint64_t epoch_counter_asc_{EPOCH_MAX};
  uint64_t epoch_counter_desc_{EPOCH_MAX};

  // =========================================================================
  // Statistics (sanity checking only — not part of hardware budget)
  // =========================================================================

  uint64_t stat_updates      = 0;
  uint64_t stat_insertions   = 0;
  uint64_t stat_queries      = 0;
  uint64_t stat_predictions  = 0;

  uint64_t stat_ptr_scans    = 0;
  uint64_t stat_ptr_positive = 0;

  uint64_t stat_pattern[static_cast<std::size_t>(AccessPattern::NUM_PATTERNS)] = {};

  uint64_t stat_stride_confirmed   = 0;
  uint64_t stat_stride_changed     = 0;
  uint64_t stat_zero_stride        = 0;
  uint64_t stat_dir_ascending      = 0;
  uint64_t stat_dir_descending     = 0;

  uint64_t stat_accesses_total     = 0;
  uint64_t stat_misses_total       = 0;

  uint64_t stat_streams_terminated = 0;
  uint64_t stat_epoch_transitions  = 0;
  uint64_t stat_depth_queries      = 0;

  static constexpr std::size_t DEPTH_DIST_BINS = 16;
  uint64_t stat_depth_dist[DEPTH_DIST_BINS] = {};

public:

  // =========================================================================
  // set_histogram_factor()
  // =========================================================================
  void set_histogram_factor(double factor) {
    active_hist_asc_.factor  = factor;
    active_hist_desc_.factor = factor;
    build_hist_asc_.factor   = factor;
    build_hist_desc_.factor  = factor;
  }

  // =========================================================================
  // update() — called from any prefetcher's cache_operate
  // =========================================================================
  void update(uint64_t addr_va, const champsim::capability& cap,
              uint8_t cache_hit, uint8_t cpu)
  {
    if (!cap.tag) return;

    uint64_t obj_hash   = hash_capability(cap);
    uint64_t cap_base   = cap.base.to<uint64_t>();

    int64_t  current_cl = static_cast<int64_t>(
        (addr_va - cap_base) >> LOG2_BLOCK_SIZE);

    int32_t current_cl32 = static_cast<int32_t>(
        std::clamp(current_cl,
                   static_cast<int64_t>(INT32_MIN),
                   static_cast<int64_t>(INT32_MAX)));

    CapProfileEntry probe{};
    probe.cap_hash = obj_hash;

    auto found = table_.check_hit(probe);

    if (found.has_value()) {
      CapProfileEntry entry = *found;
      update_existing(entry, current_cl32, cache_hit, cpu, addr_va);
      table_.fill(entry);
    } else {
      insert_new(obj_hash, current_cl32, cache_hit, cpu, addr_va);
    }
  }

  // =========================================================================
  // classify() — returns access pattern for the object
  // =========================================================================
  AccessPattern classify(const champsim::capability& cap)
  {
    if (!cap.tag) return AccessPattern::UNKNOWN;

    CapProfileEntry probe{};
    probe.cap_hash = hash_capability(cap);
    auto found = table_.check_hit(probe);
    if (!found.has_value()) return AccessPattern::UNKNOWN;

    stat_queries++;
    AccessPattern result = classify_entry(*found);
    stat_pattern[static_cast<std::size_t>(result)]++;
    return result;
  }

  // =========================================================================
  // query() — raw entry for fine-grained prefetcher decisions
  // =========================================================================
  std::optional<CapProfileEntry> query(const champsim::capability& cap)
  {
    if (!cap.tag) return std::nullopt;
    CapProfileEntry probe{};
    probe.cap_hash = hash_capability(cap);
    return table_.check_hit(probe);
  }

  // =========================================================================
  // predict() — unified predictability query
  // =========================================================================
  ObjectPredictability predict(const champsim::capability& cap,
                               std::size_t max_depth = 6)
  {
    ObjectPredictability p{};

    if (!cap.tag) return p;

    CapProfileEntry probe{};
    probe.cap_hash = hash_capability(cap);
    auto found = table_.check_hit(probe);
    if (!found.has_value() || found->access_count < MIN_OBSERVATIONS)
      return p;

    stat_predictions++;
    const auto& e = *found;

    p.pattern = classify_entry(e);
    stat_queries++;
    stat_pattern[static_cast<std::size_t>(p.pattern)]++;

    // Stride predictability
    p.stride_predictable = e.stride_stability >= STRIDE_CONF_THRESH
                        && e.stride_variation <= 2
                        && e.dominant_stride != 0;
    p.predicted_stride = e.dominant_stride;

    // Depth predictability — pick histogram matching stride direction
    if (e.stream_depth > 0) {
      const auto& hist = (e.dominant_stride >= 0)
                         ? active_hist_asc_
                         : active_hist_desc_;
      std::size_t advised = hist.get_depth_from(e.stream_depth, max_depth);
      p.depth_predictable = advised > 0;
      p.advised_depth = advised;

      stat_depth_queries++;
      if (advised < DEPTH_DIST_BINS) stat_depth_dist[advised]++;
    }

    // Feedback: per-object miss rate
    if (e.access_count > 0) {
      p.miss_rate = static_cast<double>(e.miss_count)
                  / static_cast<double>(e.access_count);
    }

    return p;
  }

  // =========================================================================
  // get_prefetch_depth() — standalone histogram query
  // =========================================================================
  std::size_t get_prefetch_depth(const champsim::capability& cap,
                                 std::size_t max_prefetch = 6)
  {
    if (!cap.tag) return 0;

    CapProfileEntry probe{};
    probe.cap_hash = hash_capability(cap);
    auto found = table_.check_hit(probe);
    if (!found.has_value()) {
      stat_depth_queries++;
      if (1 < DEPTH_DIST_BINS) stat_depth_dist[1]++;
      return 1;
    }

    std::size_t pos = found->stream_depth;
    if (pos == 0) {
      stat_depth_queries++;
      if (1 < DEPTH_DIST_BINS) stat_depth_dist[1]++;
      return 1;
    }

    const auto& hist = (found->dominant_stride >= 0)
                       ? active_hist_asc_
                       : active_hist_desc_;
    std::size_t depth = hist.get_depth_from(pos, max_prefetch);
    depth = std::max(std::size_t{1}, depth);

    stat_depth_queries++;
    if (depth < DEPTH_DIST_BINS) stat_depth_dist[depth]++;

    return depth;
  }

  // =========================================================================
  // Accessors
  // =========================================================================
  const StreamDepthHistogram& get_active_histogram_asc()  const { return active_hist_asc_;  }
  const StreamDepthHistogram& get_active_histogram_desc() const { return active_hist_desc_; }
  uint64_t get_epoch_length_asc()  const { return epoch_length_asc_;  }
  uint64_t get_epoch_length_desc() const { return epoch_length_desc_; }

  // =========================================================================
  // print_stats()
  // =========================================================================
  void print_stats(const std::string& prefix = "CAP_PROFILE") const
  {
    std::cout << "\n"
              << "========================================\n"
              << prefix << " — Capability Access Profile Table\n"
              << "========================================\n"
              << "\n"

              << "--- Table Activity ---\n"
              << prefix << "_updates "              << stat_updates        << "\n"
              << prefix << "_insertions "            << stat_insertions     << "\n"
              << prefix << "_queries "               << stat_queries        << "\n"
              << prefix << "_predictions "           << stat_predictions    << "\n"
              << prefix << "_accesses_observed "     << stat_accesses_total << "\n"
              << prefix << "_misses_observed "       << stat_misses_total   << "\n";

    if (stat_updates + stat_insertions > 0) {
      std::cout << prefix << "_table_hit_rate "
                << (100.0 * static_cast<double>(stat_updates)
                    / static_cast<double>(stat_updates + stat_insertions))
                << "%\n";
    }
    if (stat_accesses_total > 0) {
      std::cout << prefix << "_cache_miss_rate "
                << (100.0 * static_cast<double>(stat_misses_total)
                    / static_cast<double>(stat_accesses_total))
                << "%\n";
    }

    std::cout << "\n"
              << "--- Pointer Scan ---\n"
              << prefix << "_ptr_scans "             << stat_ptr_scans     << "\n"
              << prefix << "_ptr_scans_positive "    << stat_ptr_positive  << "\n";

    if (stat_ptr_scans > 0) {
      std::cout << prefix << "_ptr_positive_rate "
                << (100.0 * static_cast<double>(stat_ptr_positive)
                    / static_cast<double>(stat_ptr_scans))
                << "%\n";
    }

    std::cout << "\n"
              << "--- Stride / Direction ---\n"
              << prefix << "_stride_confirmed "      << stat_stride_confirmed  << "\n"
              << prefix << "_stride_changed "        << stat_stride_changed    << "\n"
              << prefix << "_zero_stride "           << stat_zero_stride       << "\n"
              << prefix << "_dir_ascending "         << stat_dir_ascending     << "\n"
              << prefix << "_dir_descending "        << stat_dir_descending    << "\n";

    uint64_t total_stride_events = stat_stride_confirmed + stat_stride_changed;
    if (total_stride_events > 0) {
      std::cout << prefix << "_stride_stability_rate "
                << (100.0 * static_cast<double>(stat_stride_confirmed)
                    / static_cast<double>(total_stride_events))
                << "%\n";
    }

    std::cout << "\n"
              << "--- Classification Distribution ---\n";
    for (std::size_t i = 0; i < static_cast<std::size_t>(AccessPattern::NUM_PATTERNS); i++) {
      std::cout << prefix << "_class_" << access_pattern_names[i]
                << " " << stat_pattern[i];
      if (stat_queries > 0) {
        std::cout << " ("
                  << (100.0 * static_cast<double>(stat_pattern[i])
                      / static_cast<double>(stat_queries))
                  << "%)";
      }
      std::cout << "\n";
    }

    // --- ASD Histogram Stats ---
    std::cout << "\n"
              << "--- ASD Stream Depth Histogram ---\n"
              << prefix << "_streams_terminated "    << stat_streams_terminated << "\n"
              << prefix << "_epoch_transitions "     << stat_epoch_transitions  << "\n"
              << prefix << "_epoch_length_asc "   << epoch_length_asc_  << "\n"
              << prefix << "_epoch_length_desc "  << epoch_length_desc_ << "\n"
              << prefix << "_depth_queries "         << stat_depth_queries      << "\n";

    std::cout << prefix << "_depth_distribution";
    for (std::size_t i = 0; i < DEPTH_DIST_BINS; i++) {
      if (stat_depth_dist[i] > 0)
        std::cout << " [" << i << "]=" << stat_depth_dist[i];
    }
    std::cout << "\n";

    auto print_hist = [&](const std::string& label, const StreamDepthHistogram& h) {
      std::cout << prefix << "_" << label;
      for (std::size_t i = 0; i < StreamDepthHistogram::MAX_BINS; i++) {
        if (h.bins[i] > 0)
          std::cout << " [" << (i + 1) << "]=" << h.bins[i];
        else
          break;
      }
      std::cout << "\n";

      if (h.total_streams > 0) {
        std::cout << prefix << "_" << label << "_advice";
        for (std::size_t pos = 1; pos <= 16 && pos <= StreamDepthHistogram::MAX_BINS; pos++) {
          std::size_t advice = h.get_depth_from(pos, 6);
          std::cout << " [" << pos << "]->" << advice;
        }
        std::cout << "\n";
      }
    };

    print_hist("hist_asc", active_hist_asc_);
    print_hist("hist_desc", active_hist_desc_);
  }

private:

  // =========================================================================
  // tick_epoch()
  // =========================================================================
  void tick_epoch_asc() {
    if (epoch_counter_asc_ == 0) {
      double diff = active_hist_asc_.compare(build_hist_asc_);
      epoch_length_asc_ = epoch_sm_asc_.update(diff > EPOCH_DIFF_THRESH, epoch_length_asc_);
      epoch_length_asc_ = std::clamp(epoch_length_asc_, EPOCH_MIN, EPOCH_MAX);

      active_hist_asc_ = build_hist_asc_;
      build_hist_asc_.clear();
      build_hist_asc_.factor = active_hist_asc_.factor;

      epoch_counter_asc_ = epoch_length_asc_;
      stat_epoch_transitions++;
    } else {
      epoch_counter_asc_--;
    }
  }

  void tick_epoch_desc() {
    if (epoch_counter_desc_ == 0) {
      double diff = active_hist_desc_.compare(build_hist_desc_);
      epoch_length_desc_ = epoch_sm_desc_.update(diff > EPOCH_DIFF_THRESH, epoch_length_desc_);
      epoch_length_desc_ = std::clamp(epoch_length_desc_, EPOCH_MIN, EPOCH_MAX);

      active_hist_desc_ = build_hist_desc_;
      build_hist_desc_.clear();
      build_hist_desc_.factor = active_hist_desc_.factor;

      epoch_counter_desc_ = epoch_length_desc_;
      stat_epoch_transitions++;
    } else {
      epoch_counter_desc_--;
    }
  }

  // =========================================================================
  // terminate_stream()
  // =========================================================================
  void terminate_stream(uint16_t depth, bool ascending) {
    if (depth > 0) {
      if (ascending)
        build_hist_asc_.record_stream(depth);
      else
        build_hist_desc_.record_stream(depth);
      stat_streams_terminated++;
    }
  }

  // =========================================================================
  // update_existing()
  // =========================================================================
  void update_existing(CapProfileEntry& entry, int32_t current_cl,
                       uint8_t cache_hit, uint8_t cpu, uint64_t addr_va)
  {
    stat_updates++;
    stat_accesses_total++;
    if (cache_hit == 0) stat_misses_total++;

    int64_t stride = static_cast<int64_t>(current_cl)
                   - static_cast<int64_t>(entry.last_cl_offset);

    if (stride == 0) {
      stat_zero_stride++;
      if (entry.zero_stride_count < 255) entry.zero_stride_count++;

      // Temporal timeout — terminate stalled stream
      if (entry.zero_stride_count >= ZERO_STRIDE_TIMEOUT && entry.stream_depth > 0) {
        bool asc = entry.dominant_stride >= 0;
        terminate_stream(entry.stream_depth, asc);
        entry.stream_depth = 0;
      }

    } else {
      entry.zero_stride_count = 0;
      bool ascending = stride > 0;

      // Tick the appropriate directional epoch
      if (ascending) tick_epoch_asc();
      else           tick_epoch_desc();

      if (stride == entry.dominant_stride) {
        stat_stride_confirmed++;
        if (entry.stride_stability < 7) entry.stride_stability++;
        if (entry.stride_variation > 0) entry.stride_variation--;

        if (entry.stream_depth < UINT16_MAX) entry.stream_depth++;

      } else {
        stat_stride_changed++;
        if (entry.stride_variation < 7) entry.stride_variation++;
        if (entry.stride_stability > 0) entry.stride_stability--;

        // Terminate using the OLD stride's direction
        bool old_asc = entry.dominant_stride >= 0;
        terminate_stream(entry.stream_depth, old_asc);

        if (entry.stride_stability == 0) {
          entry.dominant_stride = clamp_stride(stride);
          entry.stride_stability = 1;
        }

        entry.stream_depth = 1;
      }

      // Direction stats
      if (ascending) stat_dir_ascending++;
      else           stat_dir_descending++;
    }

    entry.last_cl_offset = current_cl;

    // Saturating counters — shift both right together to preserve ratio
    if (entry.access_count == 255) {
      entry.access_count = static_cast<uint8_t>(entry.access_count >> 1);
      entry.miss_count   = static_cast<uint8_t>(entry.miss_count >> 1);
    }
    entry.access_count++;
    if (cache_hit == 0) entry.miss_count++;

    // Pointer scan on miss (bounded)
    if (cache_hit == 0 && entry.ptr_scan_count < MAX_PTR_SCANS) {
      run_pointer_scan(entry, addr_va, cpu);
    }
  }

  // =========================================================================
  // insert_new()
  // =========================================================================
  void insert_new(uint64_t obj_hash, int32_t current_cl,
                  uint8_t cache_hit, uint8_t cpu, uint64_t addr_va)
  {
    stat_insertions++;
    stat_accesses_total++;
    if (cache_hit == 0) stat_misses_total++;

    CapProfileEntry entry{};
    entry.cap_hash          = obj_hash;
    entry.last_cl_offset    = current_cl;
    entry.dominant_stride   = 0;
    entry.stream_depth      = 1;
    entry.stride_stability  = 0;
    entry.stride_variation  = 0;
    entry.zero_stride_count = 0;
    entry.has_tagged_caps   = false;
    entry.ptr_scan_count    = 0;
    entry.access_count      = 1;
    entry.miss_count        = (cache_hit == 0) ? 1 : 0;

    if (cache_hit == 0) {
      run_pointer_scan(entry, addr_va, cpu);
    }

    table_.fill(entry);
  }

  // =========================================================================
  // classify_entry()
  // =========================================================================
  AccessPattern classify_entry(const CapProfileEntry& e)
  {
    if (e.access_count < MIN_OBSERVATIONS)
      return AccessPattern::UNKNOWN;

    // Pointer chasing — tagged caps AND irregular access
    if (e.has_tagged_caps && e.stride_stability < STRIDE_CONF_THRESH)
      return AccessPattern::POINTER_CHASE;

    // Strided — stable stride (unit or non-unit), predictable
    if (e.stride_stability >= STRIDE_CONF_THRESH
        && e.dominant_stride != 0
        && e.stride_variation <= 2)
      return AccessPattern::STRIDED;

    // Everything else — spatial prefetcher territory
    return AccessPattern::IRREGULAR;
  }

  // =========================================================================
  // Pointer scan
  // =========================================================================
  void run_pointer_scan(CapProfileEntry& entry, uint64_t addr_va, uint8_t cpu)
  {
    stat_ptr_scans++;
    entry.ptr_scan_count++;

    uint64_t cl_base = addr_va & ~(uint64_t)(BLOCK_SIZE - 1);
    bool found = false;

    for (unsigned slot = 0; slot < CAP_SLOTS_PER_CL; slot++) {
      auto cap_opt = champsim::cap_mem[cpu].load_capability(
          champsim::address{cl_base + slot * 16});
      if (cap_opt.has_value() && cap_opt->tag) {
        found = true;
        break;
      }
    }

    if (found) {
      entry.has_tagged_caps = true;
      stat_ptr_positive++;
    }
  }

  // =========================================================================
  // Utility
  // =========================================================================
  static int16_t clamp_stride(int64_t stride)
  {
    return static_cast<int16_t>(
        std::clamp(stride, static_cast<int64_t>(INT16_MIN),
                           static_cast<int64_t>(INT16_MAX)));
  }

  static uint64_t hash_capability(const champsim::capability& cap)
  {
    uint64_t b = cap.base.to<uint64_t>();
    uint64_t l = cap.length.to<uint64_t>();
    uint64_t h = b ^ (l + 0x9e3779b9 + (b << 6) + (b >> 2));
    h ^= (h >> 33);
    h *= 0xff51afd7ed558ccd;
    h ^= (h >> 33);
    return h;
  }
};

} // namespace cheri

#endif // CAP_PROFILE_TABLE_H