#include "ip_stride_cheri.h"

#include <algorithm>
#include <cassert>
#include <iostream>

#include "cache.h"

std::optional<champsim::capability> ip_stride_cheri::get_curr_capability() const
{
  // The cache stashes the current request's capability in last_access_cap
  // just before calling the prefetcher. This is the capability that the
  // instruction used to authorise its memory access — its bounds define
  // the legal address range for that pointer.
  const auto& cap = intern_->last_access_cap;
  if (!cap.tag)
    return std::nullopt;
  
  return cap;
}

void ip_stride_cheri::prefetcher_initialize()
{
  stride_prefetches_issued = 0;
  stride_prefetches_bounded = 0;
  cap_lookups = 0;
  cap_hits = 0;
}

uint32_t ip_stride_cheri::prefetcher_cache_operate(champsim::address addr, champsim::address ip, uint8_t cache_hit, bool useful_prefetch,
                                                   access_type type, uint32_t metadata_in)
{
  champsim::block_number cl_addr{addr};
  champsim::block_number::difference_type stride = 0;

  auto cap = get_curr_capability();
  cap_lookups++;
  if (cap.has_value())
    cap_hits++;

  auto found = table.check_hit({ip, cl_addr, stride});

  if (found.has_value()) {
    stride = champsim::offset(found->last_cl_addr, cl_addr);

    if (stride != 0 && stride == found->last_stride)
      active_lookahead = {champsim::address{cl_addr}, stride, PREFETCH_DEGREE, cap};
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
}