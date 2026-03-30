#include "spp_cheri.h"


#include <cassert>
#include <iostream>


void spp_cheri::prefetcher_initialize()
{
  std::cout << "Initialize SPP-CHERI SIGNATURE TABLE" << std::endl;
  std::cout << "ST_SET: " << ST_SET << " ST_WAY: " << ST_WAY << " ST_TAG_BIT: " << ST_TAG_BIT << std::endl;
  std::cout << "Initialize SPP-CHERI PATTERN TABLE" << std::endl;
  std::cout << "PT_SET: " << PT_SET << " PT_WAY: " << PT_WAY << std::endl;
  std::cout << "SIG_DELTA_BIT: " << SIG_DELTA_BIT << " C_SIG_BIT: " << C_SIG_BIT << " C_DELTA_BIT: " << C_DELTA_BIT << std::endl;
  std::cout << "Initialize SPP-CHERI PREFETCH FILTER" << std::endl;
  std::cout << "FILTER_SET: " << FILTER_SET << std::endl;
  std::cout << "CHERI-aware" << std::endl;

  ST._parent = this;
  PT._parent = this;
  FILTER._parent = this;
  GHR._parent = this;
}

void spp_cheri::prefetcher_cycle_operate() {}


uint32_t spp_cheri::prefetcher_cache_operate(champsim::address addr, champsim::address ip,
                                             uint8_t cache_hit, bool useful_prefetch,
                                             access_type type, uint32_t metadata_in)
{
  champsim::page_number page{addr};
  uint32_t last_sig = 0, curr_sig = 0, depth = 0;
  std::vector<uint32_t> confidence_q(intern_->MSHR_SIZE);
 
  int64_t delta = 0;
  std::vector<int64_t> delta_q(intern_->MSHR_SIZE);
 
  for (uint32_t i = 0; i < intern_->MSHR_SIZE; i++) {
    confidence_q[i] = 0;
    delta_q[i] = 0;
  }
 
  confidence_q[0] = 100;
  GHR.global_accuracy = GHR.pf_issued ? ((100 * GHR.pf_useful) / GHR.pf_issued) : 0;
 
  auto cap = intern_->get_authorizing_capability();
 
  uint64_t cap_base_val   = cap.base.to<uint64_t>();
  uint64_t cap_length_val = cap.length.to<uint64_t>();
  uint64_t cap_offset_val = cap.offset.to<uint64_t>();
 
  // Demand virtual address from capability cursor
  uint64_t demand_va = cheri::capability_cursor(cap).to<uint64_t>();
  uint64_t cap_top   = cheri::capability_top(cap).to<uint64_t>();
 
  // Seed TLB clone with this demand's VA → PA mapping
  tlb.fill(demand_va, addr.to<uint64_t>());
 
  if constexpr (SPP_DEBUG_PRINT) {
    std::cout << std::endl << "[SPP-CHERI] " << __func__ << " addr: " << addr
              << " page: " << page
              << " demand_va: 0x" << std::hex << demand_va << std::dec << std::endl;
  }
 
  // Stage 1: Read and update signature stored in ST
  ST.read_and_update_sig(addr, last_sig, curr_sig, delta, cap_base_val, cap_offset_val, cap_length_val);
 
  // Also check the prefetch filter to update global accuracy
  FILTER.check(addr, spp_cheri::L2C_DEMAND);
 
  // Stage 2: Update delta patterns stored in PT
  if (last_sig)
    PT.update_pattern(last_sig, delta);
 
  // Stage 3: Start prefetching
  //   base_addr = PA cursor for same-page arithmetic
  //   base_va   = VA cursor for cross-page capability arithmetic
  auto base_addr = addr;
  uint64_t base_va = demand_va;
  int64_t cap_cl_cursor = static_cast<int64_t>(cap_offset_val >> LOG2_BLOCK_SIZE);  
  uint32_t lookahead_conf = 100, pf_q_head = 0, pf_q_tail = 0;
  uint8_t do_lookahead = 0;
 
  do {
    uint32_t lookahead_way = PT_WAY;
    PT.read_pattern(curr_sig, delta_q, confidence_q, lookahead_way,
                    lookahead_conf, pf_q_tail, depth);
 
    do_lookahead = 0;
    for (uint32_t i = pf_q_head; i < pf_q_tail; i++) {
      if (confidence_q[i] >= PF_THRESHOLD) {
 
        // Compute prefetch VA from the VA cursor + cap-relative delta
        uint64_t pf_va = base_va + (static_cast<int64_t>(delta_q[i]) << LOG2_BLOCK_SIZE);
 
        // CHERI bounds check (in VA space against the authorizing capability)
        if (!cheri::in_bounds(champsim::address{pf_va}, champsim::address{cap_base_val}, champsim::address{cap_top})) {
          stat_pf_bounded_by_cap++;
          // Still allow lookahead to continue past bounded-out prefetches
          do_lookahead = 1;
          pf_q_head++;
          continue;
        }
 
        // Compute PA-space prefetch address for same-page check
        champsim::address pf_addr{champsim::block_number{base_addr} + delta_q[i]};
 
        if (champsim::page_number{pf_addr} == page) {
          //  Same physical page as demand: issue directly (no translation needed) 
          if (FILTER.check(pf_addr, ((confidence_q[i] >= FILL_THRESHOLD)
                                         ? spp_cheri::SPP_L2C_PREFETCH
                                         : spp_cheri::SPP_LLC_PREFETCH))) {
            prefetch_line(pf_addr, (confidence_q[i] >= FILL_THRESHOLD), 0);
 
            if (confidence_q[i] >= FILL_THRESHOLD) {
              GHR.pf_issued++;
              if (GHR.pf_issued > GLOBAL_COUNTER_MAX) {
                GHR.pf_issued >>= 1;
                GHR.pf_useful >>= 1;
              }
            }
 
            if constexpr (SPP_DEBUG_PRINT) {
              std::cout << "[SPP-CHERI] same-page pf_addr: " << pf_addr
                        << " delta: " << delta_q[i]
                        << " conf: " << confidence_q[i]
                        << " depth: " << depth << std::endl;
            }
          }
        } else {
          //  Cross-page access
          auto translated = tlb.translate(pf_va);
 
          if (translated.has_value()) {
            champsim::address cross_pf_addr{*translated};
 
            if (FILTER.check(cross_pf_addr, ((confidence_q[i] >= FILL_THRESHOLD)
                                                 ? spp_cheri::SPP_L2C_PREFETCH
                                                 : spp_cheri::SPP_LLC_PREFETCH))) {
              prefetch_line(cross_pf_addr, (confidence_q[i] >= FILL_THRESHOLD), 0);
 
              if (confidence_q[i] >= FILL_THRESHOLD) {
                GHR.pf_issued++;
                if (GHR.pf_issued > GLOBAL_COUNTER_MAX) {
                  GHR.pf_issued >>= 1;
                  GHR.pf_useful >>= 1;
                }
              }
              stat_cross_page_pf++;
              if constexpr (SPP_DEBUG_PRINT) {
                std::cout << "[SPP-CHERI] cross-page pf_va: 0x" << std::hex << pf_va
                          << " pf_pa: " << cross_pf_addr << std::dec
                          << " delta: " << delta_q[i]
                          << " conf: " << confidence_q[i]
                          << " depth: " << depth << std::endl;
              }
            }
          } else {
            // tlb miss 
            if constexpr (GHR_ON) {
              GHR.update_entry(curr_sig, confidence_q[i], cap_cl_cursor + delta_q[i], delta_q[i]);
            }
 
            if constexpr (SPP_DEBUG_PRINT) {
              std::cout << "[SPP-CHERI] cross-page TLB miss pf_va: 0x" << std::hex
                        << pf_va << std::dec
                        << " delta: " << delta_q[i] << std::endl;
            }
          }
        }
 
        do_lookahead = 1;
        pf_q_head++;
      }
    }
 
    // Update PA base, VA base, and signature for next lookahead iteration
    if (lookahead_way < PT_WAY) {
      uint32_t set = get_hash(curr_sig) % PT_SET;
      int64_t la_delta = PT.delta[set][lookahead_way];
 
      base_addr += (la_delta << LOG2_BLOCK_SIZE);
      base_va   += (static_cast<int64_t>(la_delta) << LOG2_BLOCK_SIZE);
      cap_cl_cursor += la_delta;
 
      uint64_t abs_delta = (la_delta < 0) ? (-la_delta) : la_delta;
      uint64_t folded_magnitude;
      if (abs_delta < (1ULL << (SIG_DELTA_BIT - 1))) {
        folded_magnitude = abs_delta;
      } else {
        folded_magnitude = (abs_delta ^ (abs_delta >> 6) ^ (abs_delta >> 12)) & ((1ULL << (SIG_DELTA_BIT - 1)) - 1);
        folded_magnitude |= (1ULL << (SIG_DELTA_BIT - 2)); 
      }
      
      auto sig_delta = (la_delta < 0) ? (folded_magnitude + (1 << (SIG_DELTA_BIT - 1))) : folded_magnitude;
      curr_sig = ((curr_sig << SIG_SHIFT) ^ sig_delta) & SIG_MASK;
    }
 
    if constexpr (SPP_DEBUG_PRINT) {
      std::cout << "Looping curr_sig: " << std::hex << curr_sig
                << " base_addr: " << base_addr << std::dec
                << " base_va: 0x" << std::hex << base_va << std::dec
                << " pf_q_head: " << pf_q_head
                << " pf_q_tail: " << pf_q_tail
                << " depth: " << depth << std::endl;
    }
  } while (LOOKAHEAD_ON && do_lookahead);
 
  return metadata_in;
}

uint32_t spp_cheri::prefetcher_cache_fill(champsim::address addr, long set, long way,
                                          uint8_t prefetch, champsim::address evicted_addr,
                                          uint32_t metadata_in)
{
  if constexpr (FILTER_ON) {
    FILTER.check(evicted_addr, spp_cheri::L2C_EVICT);
  }
  return metadata_in;
}

uint64_t spp_cheri::get_hash(uint64_t key)
{
  key += (key << 12);
  key ^= (key >> 22);
  key += (key << 4);
  key ^= (key >> 9);
  key += (key << 10);
  key ^= (key >> 2);
  key += (key << 7);
  key ^= (key >> 12);
  key = (key >> 3) * 2654435761;
  return key;
}


void spp_cheri::SIGNATURE_TABLE::read_and_update_sig(champsim::address addr,
                                                     uint32_t& last_sig,
                                                     uint32_t& curr_sig,
                                                     int64_t& out_delta,
                                                     uint64_t cap_base_val,
                                                     uint64_t cap_offset_val,
                                                     uint64_t cap_length_val)
{
  uint32_t set = static_cast<uint32_t>(get_hash(cap_base_val) % ST_SET);
 
  auto match = ST_WAY;
  uint8_t ST_hit = 0;
  long sig_delta = 0;
 
  int64_t cap_cl_offset = static_cast<int64_t>(cap_offset_val >> LOG2_BLOCK_SIZE);
 
  //  Case 1: ST hit (match on cap_base) 
  for (match = 0; match < ST_WAY; match++) {
    if (!valid[set][match])
      continue;
 
    if (cap_base[set][match] == cap_base_val) {
      last_sig = sig[set][match];
 
      // Delta in cap-relative cache-line units
      out_delta = cap_cl_offset - last_cap_cl_offset[set][match];
 
      if (out_delta) {
        // Track cross-page strides within the same capability (VA pages)
        champsim::page_number last_page{champsim::address{cap_base_val + (static_cast<uint64_t>(last_cap_cl_offset[set][match]) << LOG2_BLOCK_SIZE)}};
        champsim::page_number curr_page{champsim::address{cap_base_val + cap_offset_val}};
        if (last_page != curr_page)
          _parent->stat_cross_page_in_cap++;
 
        uint64_t abs_delta = (out_delta < 0) ? (-out_delta) : out_delta;
        
        uint64_t folded_magnitude;
        if (abs_delta < (1ULL << (SIG_DELTA_BIT - 1))) {
            folded_magnitude = abs_delta;
        } else {

          folded_magnitude = (abs_delta ^ (abs_delta >> 6) ^ (abs_delta >> 12)) & ((1ULL << (SIG_DELTA_BIT - 1)) - 1);
          folded_magnitude |= (1ULL << (SIG_DELTA_BIT - 2)); 
        }

        sig_delta = (out_delta < 0) ? (folded_magnitude + (1 << (SIG_DELTA_BIT - 1))) : folded_magnitude;
        sig[set][match] = ((last_sig << SIG_SHIFT) ^ sig_delta) & SIG_MASK;
        curr_sig = sig[set][match];
        last_cap_cl_offset[set][match] = cap_cl_offset;
      } else {
        last_sig = 0;  // same cache line, no delta
      }
 
      ST_hit = 1;
      break;
    }
  }
 
  //  Case 2: ST miss — find invalid entry 
  if (match == ST_WAY) {
    for (match = 0; match < ST_WAY; match++) {
      if (!valid[set][match]) {
        valid[set][match] = 1;
        sig[set][match] = 0;
        curr_sig = 0;
        cap_base[set][match] = cap_base_val;
        cap_length[set][match] = cap_length_val;
        last_cap_cl_offset[set][match] = cap_cl_offset;
        break;
      }
    }
  }
 
  //  Case 3: ST miss, no invalid — LRU eviction 
  if (match == ST_WAY) {
    for (match = 0; match < ST_WAY; match++) {
      if (lru[set][match] == ST_WAY - 1) {
        sig[set][match] = 0;
        curr_sig = 0;
        cap_base[set][match] = cap_base_val;
        cap_length[set][match] = cap_length_val;
        last_cap_cl_offset[set][match] = cap_cl_offset;
        break;
      }
    }
  }
 
  if constexpr (SPP_SANITY_CHECK) {
    if (match == ST_WAY) {
      std::cout << "[ST-CHERI] Cannot find a replacement victim!" << std::endl;
      assert(0);
    }
  }
 
  //  GHR bootstrap for new pages 
  if constexpr (GHR_ON) {
    if (ST_hit == 0) {
      uint32_t GHR_found = _parent->GHR.check_entry(cap_cl_offset);
      if (GHR_found < MAX_GHR_ENTRY) {
        auto ghr_delta = _parent->GHR.delta[GHR_found];
        uint64_t abs_delta = (ghr_delta < 0) ? (-ghr_delta) : ghr_delta;
        uint64_t folded_magnitude;
        if (abs_delta < (1ULL << (SIG_DELTA_BIT - 1))) {
          folded_magnitude = abs_delta;
        } else {
          folded_magnitude = (abs_delta ^ (abs_delta >> 6) ^ (abs_delta >> 12)) & ((1ULL << (SIG_DELTA_BIT - 1)) - 1);
          folded_magnitude |= (1ULL << (SIG_DELTA_BIT - 2)); 
        }
        
        sig_delta = (ghr_delta < 0) ? (folded_magnitude + (1 << (SIG_DELTA_BIT - 1))) : folded_magnitude;
        sig[set][match] = ((_parent->GHR.sig[GHR_found] << SIG_SHIFT) ^ sig_delta) & SIG_MASK;
        curr_sig = sig[set][match];
      }
    }
  }
 
  //  LRU update 
  for (uint32_t way = 0; way < ST_WAY; way++) {
    if (lru[set][way] < lru[set][match])
      lru[set][way]++;
  }
  lru[set][match] = 0;
}
 


void spp_cheri::PATTERN_TABLE::update_pattern(uint32_t last_sig, int64_t curr_delta)
{
  uint32_t set = get_hash(last_sig) % PT_SET, match = 0;

  for (match = 0; match < PT_WAY; match++) {
    if (delta[set][match] == curr_delta) {
      c_delta[set][match]++;
      c_sig[set]++;
      if (c_sig[set] > C_SIG_MAX) {
        for (uint32_t way = 0; way < PT_WAY; way++)
          c_delta[set][way] >>= 1;
        c_sig[set] >>= 1;
      }
      break;
    }
  }

  if (match == PT_WAY) {
    uint32_t victim_way = PT_WAY, min_counter = C_SIG_MAX;
    for (match = 0; match < PT_WAY; match++) {
      if (c_delta[set][match] < min_counter) {
        victim_way = match;
        min_counter = c_delta[set][match];
      }
    }

    if constexpr (SPP_SANITY_CHECK) {
      if (victim_way == PT_WAY) {
        std::cout << "[PT-CHERI] Cannot find a replacement victim!" << std::endl;
        assert(0);
      }
    }

    delta[set][victim_way] = curr_delta;
    c_delta[set][victim_way] = 0;
    c_sig[set]++;
    if (c_sig[set] > C_SIG_MAX) {
      for (uint32_t way = 0; way < PT_WAY; way++)
        c_delta[set][way] >>= 1;
      c_sig[set] >>= 1;
    }
  }
}

void spp_cheri::PATTERN_TABLE::read_pattern(uint32_t curr_sig, std::vector<int64_t>& delta_q,
                                            std::vector<uint32_t>& confidence_q,
                                            uint32_t& lookahead_way, uint32_t& lookahead_conf,
                                            uint32_t& pf_q_tail, uint32_t& depth)
{
  uint32_t set = get_hash(curr_sig) % PT_SET, local_conf = 0, pf_conf = 0, max_conf = 0;

  if (c_sig[set]) {
    for (uint32_t way = 0; way < PT_WAY; way++) {
      local_conf = (100 * c_delta[set][way]) / c_sig[set];
      pf_conf = depth ? (_parent->GHR.global_accuracy * c_delta[set][way] / c_sig[set] * lookahead_conf / 100)
                      : local_conf;

      if (pf_conf >= PF_THRESHOLD) {
        confidence_q[pf_q_tail] = pf_conf;
        delta_q[pf_q_tail] = delta[set][way];

        if (pf_conf > max_conf) {
          lookahead_way = way;
          max_conf = pf_conf;
        }
        pf_q_tail++;
      }
    }
    pf_q_tail++;
    lookahead_conf = max_conf;
    if (lookahead_conf >= PF_THRESHOLD)
      depth++;
  } else {
    confidence_q[pf_q_tail] = 0;
  }
}


bool spp_cheri::PREFETCH_FILTER::check(champsim::address check_addr, FILTER_REQUEST filter_request)
{
  champsim::block_number cache_line{check_addr};
  auto hash = get_hash(cache_line.to<uint64_t>());
  auto quotient = (hash >> REMAINDER_BIT) & ((1 << QUOTIENT_BIT) - 1);
  auto remainder = hash % (1 << REMAINDER_BIT);

  switch (filter_request) {
  case spp_cheri::SPP_L2C_PREFETCH:
    if ((valid[quotient] || useful[quotient]) && remainder_tag[quotient] == remainder)
      return false;
    else {
      valid[quotient] = 1;
      useful[quotient] = 0;
      remainder_tag[quotient] = remainder;
    }
    break;

  case spp_cheri::SPP_LLC_PREFETCH:
    if ((valid[quotient] || useful[quotient]) && remainder_tag[quotient] == remainder)
      return false;
    break;

  case spp_cheri::L2C_DEMAND:
    if ((remainder_tag[quotient] == remainder) && (useful[quotient] == 0)) {
      useful[quotient] = 1;
      if (valid[quotient])
        _parent->GHR.pf_useful++;
    }
    break;

  case spp_cheri::L2C_EVICT:
    if (valid[quotient] && !useful[quotient] && _parent->GHR.pf_useful)
      _parent->GHR.pf_useful--;
    valid[quotient] = 0;
    useful[quotient] = 0;
    remainder_tag[quotient] = 0;
    break;

  default:
    std::cout << "[FILTER-CHERI] Invalid request type: " << filter_request << std::endl;
    assert(0);
  }

  return true;
}


void spp_cheri::GLOBAL_REGISTER::update_entry(uint32_t pf_sig, uint32_t pf_confidence,
                                               int64_t pf_cap_cl_off, int64_t pf_delta)
{                                              
  uint32_t min_conf = 100, victim_way = MAX_GHR_ENTRY;

  for (uint32_t i = 0; i < MAX_GHR_ENTRY; i++) {
    if (valid[i] && (cap_cl_offset[i] == pf_cap_cl_off)) {
      sig[i] = pf_sig;
      confidence[i] = pf_confidence;
      delta[i] = pf_delta;
      return;
    }
    if (confidence[i] <= min_conf) {
      min_conf = confidence[i];
      victim_way = i;
    }
  }

  if (victim_way >= MAX_GHR_ENTRY) {
    std::cout << "[GHR] Cannot find a replacement victim!" << std::endl;
    assert(0);
  }

  valid[victim_way] = 1;
  sig[victim_way] = pf_sig;
  confidence[victim_way] = pf_confidence;
  cap_cl_offset[victim_way] = pf_cap_cl_off;
  delta[victim_way] = pf_delta;
}

uint32_t spp_cheri::GLOBAL_REGISTER::check_entry(int64_t demand_cap_cl_off)
{
  uint32_t max_conf = 0, max_conf_way = MAX_GHR_ENTRY;
  for (uint32_t i = 0; i < MAX_GHR_ENTRY; i++) {
    if ((cap_cl_offset[i] == demand_cap_cl_off) && (max_conf < confidence[i])) {
      max_conf = confidence[i];
      max_conf_way = i;
    }
  }
  return max_conf_way;
}


void spp_cheri::prefetcher_final_stats()
{
  std::cout << std::endl;
  std::cout << "spp_cheri final stats" << std::endl;
  std::cout << "  Cross-page deltas in cap:        " << stat_cross_page_in_cap << std::endl;
  std::cout << "  Prefetches bounded by cap:       " << stat_pf_bounded_by_cap << std::endl;
  std::cout << "  Cross-page prefetches issued: " << stat_cross_page_pf << std::endl;
  tlb.print_stats();
}