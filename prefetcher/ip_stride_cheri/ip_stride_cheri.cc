#include "ip_stride_cheri.h"

#include <algorithm>
#include <cassert>
#include <iostream>

#include "cache.h"

std::optional<champsim::capability> ip_stride_cheri::get_auth_capability() const
{
  const auto& cap = intern_->auth_capability;
  if (!cap.tag || cap.cap_op != champsim::cap_op_type::AUTH_CAP)
    return std::nullopt;
  
  return cap;
}


int ip_stride_cheri::compute_adaptive_degree(champsim::block_number cl_addr,
                                             champsim::block_number::difference_type stride,
                                             const champsim::capability& cap) const
{
  // How many cache lines lie between the current position and the
  // capability bound in the direction we're striding?
  int direction = (stride > 0) ? 1 : -1;
  int remaining = cheri::remaining_lines(cl_addr, direction,
                                         cap.base, cheri::capability_top(cap));

  // Each prefetch covers |stride| cache lines of distance, so the
  // maximum useful degree is remaining / |stride|.
  int abs_stride = static_cast<int>(std::abs(stride));
  int max_useful = (abs_stride > 0) ? (remaining / abs_stride) : 0;

  // Clamp to [1, PREFETCH_DEGREE].  We always attempt at least 1 so the
  // bounds check in cycle_operate can decide; if even that one is out of
  // bounds it will be caught there.
  return std::clamp(max_useful, 1, PREFETCH_DEGREE);
}

void ip_stride_cheri::prefetcher_initialize()
{
  stride_prefetches_issued = 0;
  stride_prefetches_bounded = 0;
  cap_lookups = 0;
  cap_hits = 0;
  degree_adapted_count = 0;
  degree_full_count = 0;
}

uint32_t ip_stride_cheri::prefetcher_cache_operate(champsim::address addr, champsim::address ip, uint8_t cache_hit, bool useful_prefetch,
                                                   access_type type, uint32_t metadata_in)
{
  champsim::block_number cl_addr{addr};
  champsim::block_number::difference_type stride = 0;

  auto cap = get_auth_capability();

  cap_lookups++;
  if (cap.has_value())
    cap_hits++;

  auto found = table.check_hit({ip, cl_addr, stride});

  if (found.has_value()) {
    stride = champsim::offset(found->last_cl_addr, cl_addr);

    if (stride != 0 && stride == found->last_stride) {
      int degree = PREFETCH_DEGREE;

      if (cap.has_value()) {
        degree = compute_adaptive_degree(cl_addr, stride, *cap);

        if (degree < PREFETCH_DEGREE)
          degree_adapted_count++;
        else
          degree_full_count++;
      } else {
        degree_full_count++;
      }

      active_lookahead = {champsim::address{cl_addr}, stride, degree, cap};
    }
  }

  table.fill({ip, cl_addr, stride});

  return metadata_in;
}

void ip_stride_cheri::prefetcher_cycle_operate()
{
  if (!active_lookahead.has_value())
    return;

  auto [old_pf_address, stride, degree, cap] = active_lookahead.value();
  assert(degree > 0);

  champsim::address pf_address{champsim::block_number{old_pf_address} + stride};

  if (cap.has_value()) {
    if (!cheri::prefetch_safe(pf_address, *cap)) {
      stride_prefetches_bounded++;
      active_lookahead.reset();
      return;
    }
  }

  if (intern_->virtual_prefetch || champsim::page_number{pf_address} == champsim::page_number{old_pf_address}) {
    const bool mshr_under_light_load = intern_->get_mshr_occupancy_ratio() < 0.5;
    const bool success = prefetch_line(pf_address, mshr_under_light_load, 0);
    if (success) {
      stride_prefetches_issued++;
      active_lookahead = {pf_address, stride, degree - 1, cap};
    }

    if (active_lookahead->degree == 0)
      active_lookahead.reset();
  } else {
    active_lookahead.reset();
  }
}

uint32_t ip_stride_cheri::prefetcher_cache_fill(champsim::address addr, long set, long way, uint8_t prefetch, champsim::address evicted_addr,
                                                uint32_t metadata_in)
{
  return metadata_in;
}


void ip_stride_cheri::prefetcher_final_stats()
{
  std::cout << "\nip_stride_cheri final stats" << std::endl;
  std::cout << "  Stride prefetches issued:    " << stride_prefetches_issued << std::endl;
  std::cout << "  Stride prefetches bounded:   " << stride_prefetches_bounded << std::endl;
  std::cout << "  CHERI cap lookups:           " << cap_lookups << std::endl;
  std::cout << "  CHERI cap hits:              " << cap_hits << std::endl;
  if (cap_lookups > 0)
    std::cout << "  CHERI cap hit rate:          " << (100.0 * static_cast<double>(cap_hits) / static_cast<double>(cap_lookups)) << "%" << std::endl;
  std::cout << "  Degree full (== " << PREFETCH_DEGREE << "):          " << degree_full_count << std::endl;
  std::cout << "  Degree adapted (< " << PREFETCH_DEGREE << "):        " << degree_adapted_count << std::endl;
  if (degree_full_count + degree_adapted_count > 0) {
    double adapt_pct = 100.0 * static_cast<double>(degree_adapted_count)
                     / static_cast<double>(degree_full_count + degree_adapted_count);
    std::cout << "  Degree adaptation rate:      " << adapt_pct << "%" << std::endl;
  }
}