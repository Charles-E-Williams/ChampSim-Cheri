#include "spp_obj.h"

#include <cassert>
#include <cstdlib>
#include <iostream>

void spp_obj::prefetcher_initialize()
{
  std::cout << "Initialize SIGNATURE TABLE" << std::endl;
  std::cout << "ST_SET: " << ST_SET << std::endl;
  std::cout << "ST_WAY: " << ST_WAY << std::endl;
  std::cout << "ST_TAG_BIT: " << ST_TAG_BIT << std::endl;

  std::cout << std::endl << "Initialize PATTERN TABLE" << std::endl;
  std::cout << "PT_SET: " << PT_SET << std::endl;
  std::cout << "PT_WAY: " << PT_WAY << std::endl;
  std::cout << "SIG_DELTA_BIT: " << SIG_DELTA_BIT << std::endl;
  std::cout << "C_SIG_BIT: " << C_SIG_BIT << std::endl;
  std::cout << "C_DELTA_BIT: " << C_DELTA_BIT << std::endl;

  std::cout << std::endl << "Initialize PREFETCH FILTER" << std::endl;
  std::cout << "FILTER_SET: " << FILTER_SET << std::endl;

  // pass pointers
  ST._parent = this;
  PT._parent = this;
  FILTER._parent = this;
  GHR._parent = this;
}

void spp_obj::prefetcher_cycle_operate() {}

uint32_t spp_obj::prefetcher_cache_operate(champsim::address addr, champsim::address ip, uint32_t cpu, champsim::capability cap, uint8_t cache_hit,
                                           bool useful_prefetch, access_type type, uint32_t metadata_in, uint32_t metadata_hit)
{
  if (!cap.tag)
    return metadata_in;

  // A demand access to a tracked OOB candidate credits its confidence counter
  if constexpr (OOB_FILTER)
    if (type == access_type::LOAD || type == access_type::RFO)
      oob.on_demand(champsim::block_number{addr}.to<uint64_t>());

  // Object context: the demand's virtual line from the capability cursor. Without it, behave as unmodified SPP.
  const std::optional<champsim::address> va_line = cheri::line_va_from_cursor(cap, addr);
  if (cap.tag && !va_line.has_value())
    cursor_check_failed++;
  const bool obj = cap.tag && va_line.has_value();
  if (obj)
    tagged_accesses++;
  // OBJECT_KEYED applies to large objects only: small ones keep page keys, where neighbouring objects share a signature
  const bool keyed = OBJECT_KEYED && obj && cap.length.to<uint64_t>() >= OBJ_KEY_MIN_BYTES;

  bool promote = false;
  if constexpr (PROMOTE) {
    if (obj && cap.length.to<uint64_t>() >= PROMOTE_MIN_BYTES && intern_->get_mshr_occupancy_ratio() * 100 < PROMOTE_MSHR_LIMIT) {
      promote_eligible++;
      promote = !PROMOTE_GATE || prom_issued < PROMOTE_WARMUP || prom_useful * 100 >= PROMOTE_MIN_ACC * prom_issued || (promote_eligible & 15) == 0;
      if (!promote)
        promote_gated++;
    }
  }
  const uint32_t pf_threshold = promote ? PF_THRESHOLD_PROMOTE : PF_THRESHOLD;
  const uint32_t fill_threshold = promote ? FILL_THRESHOLD_PROMOTE : FILL_THRESHOLD;
  const cheri_policy::OobConfidence::key oob_key =
      cheri_policy::OobConfidence::make_key(cheri_policy::size_class(cap.length.to<uint64_t>()), ip.to<uint64_t>());

  page_type page{addr};
  uint32_t last_sig = 0, curr_sig = 0, depth = 0;
  std::vector<uint32_t> confidence_q(intern_->MSHR_SIZE);

  typename spp_obj::offset_type::difference_type delta = 0;
  std::vector<typename spp_obj::offset_type::difference_type> delta_q(intern_->MSHR_SIZE);

  for (uint32_t i = 0; i < intern_->MSHR_SIZE; i++) {
    confidence_q[i] = 0;
    delta_q[i] = 0;
  }
  confidence_q[0] = 100;
  GHR.global_accuracy = GHR.pf_issued ? ((100 * GHR.pf_useful) / GHR.pf_issued) : 0;

  if constexpr (SPP_DEBUG_PRINT) {
    std::cout << std::endl << "[ChampSim] " << __func__ << " addr: " << addr;
    std::cout << " page: " << page << std::endl;
  }

  // OBJECT_KEYED: index the ST by object-relative region instead of physical page (tagged accesses to objects >= OBJ_KEY_MIN_BYTES)
  champsim::address st_addr = addr;
  int64_t obj_line = 0;
  champsim::address neighbor_key[2];
  int64_t neighbor_first_line[2];
  int n_neighbors = 0;
  if constexpr (OBJECT_KEYED) {
    if (keyed) {
      obj_line = cheri::lines_from_cap_base(cap);
      st_addr = object_key_address(cap, obj_line);
      obj_keyed_accesses++;
      if constexpr (CARRY_SIG) {
        const int64_t region_lines = int64_t{1} << (OBJ_LOG2_REGION - LOG2_BLOCK_SIZE);
        const int64_t region_first = (obj_line / region_lines) * region_lines;
        for (int64_t first : {region_first - region_lines, region_first + region_lines}) {
          if (first < 0 || static_cast<uint64_t>(first) << LOG2_BLOCK_SIZE >= cap.length.to<uint64_t>())
            continue;
          neighbor_key[n_neighbors] = object_key_address(cap, first);
          neighbor_first_line[n_neighbors] = first;
          n_neighbors++;
        }
      }
    }
  }

  // Stage 1: Read and update a sig stored in ST
  // last_sig and delta are used to update (sig, delta) correlation in PT
  // curr_sig is used to read prefetch candidates in PT
  const uint64_t carried_before = ST.carried;
  ST.read_and_update_sig(st_addr, keyed, last_sig, curr_sig, delta, neighbor_key, neighbor_first_line, n_neighbors, obj_line);
  carried_sigs += ST.carried - carried_before;

  // Also check the prefetch filter in parallel to update global accuracy counters
  FILTER.check(addr, spp_obj::L2C_DEMAND);

  // Stage 2: Update delta patterns stored in PT
  if (last_sig)
    PT.update_pattern(last_sig, delta);

  // Stage 3: Start prefetching
  auto base_addr = addr;
  uint32_t lookahead_conf = 100, pf_q_head = 0, pf_q_tail = 0;
  uint8_t do_lookahead = 0;

  do {
    uint32_t lookahead_way = PT_WAY;
    PT.read_pattern(curr_sig, delta_q, confidence_q, lookahead_way, lookahead_conf, pf_q_tail, depth, pf_threshold);

    do_lookahead = 0;
    for (uint32_t i = pf_q_head; i < pf_q_tail; i++) {
      if (confidence_q[i] >= pf_threshold) {
        champsim::address pf_addr{champsim::block_number{base_addr} + delta_q[i]};
        bool fill_l2 = (confidence_q[i] >= fill_threshold);
        bool issue = true;

        // Out-of-object candidate: END_STOP drops it for large objects, else the learned OOB filter decides
        bool oob_cand = false;
        int64_t line_delta = 0;
        if (obj) {
          line_delta = champsim::offset(champsim::block_number{addr}, champsim::block_number{pf_addr});
          const champsim::block_number pf_va_block = champsim::block_number{*va_line} + line_delta;
          oob_cand = !cheri::overlaps_bounds(pf_va_block, cap.base, cheri::capability_top(cap));
          if (oob_cand) { // promotion applies to in-object candidates only
            issue = (confidence_q[i] >= PF_THRESHOLD);
            fill_l2 = (confidence_q[i] >= FILL_THRESHOLD);
          }
        }
        if (END_STOP && oob_cand && cap.length.to<uint64_t>() >= OBJ_KEY_MIN_BYTES) {
          issue = false;
          oob_cand = false; // not an OOB-filter candidate
          end_stopped++;
        }

        if (page_type{pf_addr} == page) { // Prefetch request is in the same physical page
          // OBJECT_KEYED: a candidate in the next object-relative region bootstraps that region's signature via the GHR
          if constexpr (OBJECT_KEYED && GHR_ON) {
            const int64_t pf_obj_line = obj_line + line_delta;
            if (keyed && pf_obj_line >= 0 && (pf_obj_line >> (OBJ_LOG2_REGION - LOG2_BLOCK_SIZE)) != (obj_line >> (OBJ_LOG2_REGION - LOG2_BLOCK_SIZE)))
              GHR.update_entry(curr_sig, confidence_q[i], spp_obj::offset_type{object_key_address(cap, pf_obj_line)}, delta_q[i], true);
          }

          auto act = cheri_policy::oob_action::ISSUE_HERE;
          if (OOB_FILTER && issue && oob_cand) {
            act = oob.decide(oob_key);
            if (act == cheri_policy::oob_action::DROP) {
              oob.record(champsim::block_number{pf_addr}.to<uint64_t>(), oob_key, act);
              issue = false;
            } else if (act == cheri_policy::oob_action::ISSUE_LOWER) {
              fill_l2 = false;
            }
          }

          const bool promoted_fill = promote && fill_l2 && !oob_cand && (confidence_q[i] < PF_THRESHOLD || confidence_q[i] < FILL_THRESHOLD);
          if (issue && FILTER.check(pf_addr, (fill_l2 ? spp_obj::SPP_L2C_PREFETCH : spp_obj::SPP_LLC_PREFETCH), PROMOTE_GATE && promoted_fill)) {
            const bool issued = prefetch_line(pf_addr, fill_l2, 0); // Use addr (not base_addr) to obey the same physical page boundary
            if (promoted_fill) { // counted at the filter, as SPP's own pf_issued
              promoted_fills++;
              if (++prom_issued > GLOBAL_COUNTER_MAX) {
                prom_issued >>= 1;
                prom_useful >>= 1;
              }
            }
            // OOB outcome is recorded once the issue result is known; a failed issue counts as a drop (shadow entry)
            if (OOB_FILTER && oob_cand)
              oob.record(champsim::block_number{pf_addr}.to<uint64_t>(), oob_key, issued ? act : cheri_policy::oob_action::DROP);

            if (fill_l2) {
              GHR.pf_issued++;
              if (GHR.pf_issued > GLOBAL_COUNTER_MAX) {
                GHR.pf_issued >>= 1;
                GHR.pf_useful >>= 1;
              }
              if constexpr (SPP_DEBUG_PRINT) {
                std::cout << "[ChampSim] SPP L2 prefetch issued GHR.pf_issued: " << GHR.pf_issued << " GHR.pf_useful: " << GHR.pf_useful << std::endl;
              }
            }

            if constexpr (SPP_DEBUG_PRINT) {
              std::cout << "[ChampSim] " << __func__ << " base_addr: " << base_addr << " pf_addr: " << pf_addr;
              std::cout << " prefetch_delta: " << delta_q[i] << " confidence: " << confidence_q[i];
              std::cout << " depth: " << i << std::endl;
            }
          }
        } else { // Prefetch request is crossing the physical page boundary
          if constexpr (GHR_ON) {
            // Store this prefetch request in GHR to bootstrap SPP learning when
            // we see a ST miss (i.e., accessing a new page)
            // A physical page offset: page kind, even for an object-keyed access
            GHR.update_entry(curr_sig, confidence_q[i], spp_obj::offset_type{pf_addr}, delta_q[i], false);
          }
        }

        do_lookahead = 1;
        pf_q_head++;
      }
    }

    // Update base_addr and curr_sig
    if (lookahead_way < PT_WAY) {
      uint32_t set = get_hash(curr_sig) % PT_SET;
      base_addr += (PT.delta[set][lookahead_way] << LOG2_BLOCK_SIZE);

      // PT.delta uses a 7-bit sign magnitude representation to generate
      // sig_delta
      // int sig_delta = (PT.delta[set][lookahead_way] < 0) ? ((((-1) *
      // PT.delta[set][lookahead_way]) & 0x3F) + 0x40) :
      // PT.delta[set][lookahead_way];
      auto sig_delta = (PT.delta[set][lookahead_way] < 0) ? (((-1) * PT.delta[set][lookahead_way]) + (1 << (SIG_DELTA_BIT - 1))) : PT.delta[set][lookahead_way];
      curr_sig = ((curr_sig << SIG_SHIFT) ^ sig_delta) & SIG_MASK;
    }

    if constexpr (SPP_DEBUG_PRINT) {
      std::cout << "Looping curr_sig: " << std::hex << curr_sig << " base_addr: " << base_addr << std::dec;
      std::cout << " pf_q_head: " << pf_q_head << " pf_q_tail: " << pf_q_tail << " depth: " << depth << std::endl;
    }
    // Stop before the candidate queue (sized MSHR_SIZE) could overflow
  } while (LOOKAHEAD_ON && do_lookahead && (pf_q_tail + PT_WAY + 1 <= confidence_q.size()));

  return metadata_in;
}

uint32_t spp_obj::prefetcher_cache_fill(champsim::address addr, champsim::address ip, uint32_t cpu, champsim::capability cap, bool useless, long set, long way,
                                        bool prefetch, champsim::address evicted_addr, champsim::capability evicted_cap, uint32_t metadata_in,
                                        uint32_t metadata_evict, uint32_t cpu_evict)
{
  if constexpr (FILTER_ON) {
    if constexpr (SPP_DEBUG_PRINT) {
      std::cout << std::endl;
    }
    FILTER.check(evicted_addr, spp_obj::L2C_EVICT);
  }

  return metadata_in;
}

void spp_obj::prefetcher_final_stats()
{
  std::cout << "\nSPP_OBJ configuration\n";
  if constexpr (OOB_FILTER)
    cheri_policy::OobConfidence::print_config(std::cout);
  std::cout << "  SPP_LOG2_PAGE: " << log2_page() << "  OBJECT_KEYED: " << OBJECT_KEYED << "  OBJ_LOG2_REGION: " << OBJ_LOG2_REGION
            << "  OBJ_KEY_MIN_BYTES: " << OBJ_KEY_MIN_BYTES << "  OOB_FILTER: " << OOB_FILTER << "  CARRY_SIG: " << CARRY_SIG << "  END_STOP: " << END_STOP
            << "\n";
  std::cout << "SPP_OBJ CURSOR_CHECK_FAILED: " << cursor_check_failed << " TAGGED_ACCESSES: " << tagged_accesses << " OBJ_KEYED_ACCESSES: " << obj_keyed_accesses
            << " CARRIED_SIGS: " << carried_sigs << " END_STOPPED: " << end_stopped << std::endl;
  std::cout << "SPP_OBJ PROMOTE: " << PROMOTE << " PROMOTE_GATE: " << PROMOTE_GATE << " ELIGIBLE: " << promote_eligible << " GATED: " << promote_gated
            << " PROMOTED_FILLS: " << promoted_fills << " PROM_ACC: " << (prom_issued ? 100 * prom_useful / prom_issued : 0) << std::endl;
  std::cout << "SPP_OBJ ST_WAY: " << ST_WAY << " PAGE_KEY ST_HITS: " << ST.hits[0] << " ST_MISSES: " << ST.misses[0] << " GHR_BOOTS: " << ST.ghr_boots[0]
            << " OBJ_KEY ST_HITS: " << ST.hits[1] << " ST_MISSES: " << ST.misses[1] << " GHR_BOOTS: " << ST.ghr_boots[1] << std::endl;
  if constexpr (OOB_FILTER)
    oob.print_stats(std::cout);
}

champsim::address spp_obj::object_key_address(const champsim::capability& cap, int64_t obj_line)
{
  assert(OBJ_LOG2_REGION <= log2_page()); // the in-region line must fit in the ST offset bits
  const unsigned region_line_bits = OBJ_LOG2_REGION - LOG2_BLOCK_SIZE;
  const uint64_t region = static_cast<uint64_t>(obj_line) >> region_line_bits;
  const uint64_t line_in_region = static_cast<uint64_t>(obj_line) & ((uint64_t{1} << region_line_bits) - 1);
  const uint64_t h = cheri::hash_capability(cap) ^ (region * 0x9e3779b97f4a7c15ULL);
  return champsim::address{(h << log2_page()) | (line_in_region << LOG2_BLOCK_SIZE)};
}

// TODO: Find a good 64-bit hash function
uint64_t spp_obj::get_hash(uint64_t key)
{
  // Robert Jenkins' 32 bit mix function
  key += (key << 12);
  key ^= (key >> 22);
  key += (key << 4);
  key ^= (key >> 9);
  key += (key << 10);
  key ^= (key >> 2);
  key += (key << 7);
  key ^= (key >> 12);

  // Knuth's multiplicative method
  key = (key >> 3) * 2654435761;

  return key;
}

void spp_obj::SIGNATURE_TABLE::read_and_update_sig(champsim::address addr, bool obj_key_in, uint32_t& last_sig, uint32_t& curr_sig,
                                                   typename offset_type::difference_type& delta, const champsim::address* neighbor_key,
                                                   const int64_t* neighbor_first_line, int n_neighbors, int64_t obj_line)
{
  auto set = get_hash(page_type{addr}.to<uint64_t>()) % ST_SET;
  auto match = ST_WAY;
  tag_type partial_page{addr};
  offset_type page_offset{addr};
  uint8_t ST_hit = 0;
  long sig_delta = 0;

  if constexpr (SPP_DEBUG_PRINT) {
    std::cout << "[ST] " << __func__ << " page: " << page_type{addr} << " partial_page: " << std::hex << partial_page << std::dec << std::endl;
  }

  // Case 2: Invalid
  if (match == ST_WAY) {
    for (match = 0; match < ST_WAY; match++) {
      if (valid[set][match] && (tag[set][match] == partial_page) && (obj_key[set][match] == obj_key_in)) {
        last_sig = sig[set][match];
        delta = champsim::offset(last_offset[set][match], page_offset);

        if (delta) {
          // Build a new sig based on 7-bit sign magnitude representation of delta
          // sig_delta = (delta < 0) ? ((((-1) * delta) & 0x3F) + 0x40) : delta;
          sig_delta = (delta < 0) ? (((-1) * delta) + (1 << (SIG_DELTA_BIT - 1))) : delta;
          sig[set][match] = ((last_sig << SIG_SHIFT) ^ sig_delta) & SIG_MASK;
          curr_sig = sig[set][match];
          last_offset[set][match] = page_offset;

          if constexpr (SPP_DEBUG_PRINT) {
            std::cout << "[ST] " << __func__ << " hit set: " << set << " way: " << match;
            std::cout << " valid: " << valid[set][match] << " tag: " << std::hex << tag[set][match];
            std::cout << " last_sig: " << last_sig << " curr_sig: " << curr_sig;
            std::cout << " delta: " << std::dec << delta << " last_offset: " << page_offset << std::endl;
          }
        } else
          last_sig = 0; // Hitting the same cache line, delta is zero

        ST_hit = 1;
        hits[obj_key_in]++;
        break;
      }
    }
  }

  // Case 2: Invalid
  if (match == ST_WAY) {
    for (match = 0; match < ST_WAY; match++) {
      if (valid[set][match] == 0) {
        valid[set][match] = 1;
        tag[set][match] = partial_page;
        obj_key[set][match] = obj_key_in;
        sig[set][match] = 0;
        curr_sig = sig[set][match];
        last_offset[set][match] = page_offset;

        if constexpr (SPP_DEBUG_PRINT) {
          std::cout << "[ST] " << __func__ << " invalid set: " << set << " way: " << match;
          std::cout << " valid: " << valid[set][match] << " tag: " << std::hex << partial_page;
          std::cout << " sig: " << sig[set][match] << " last_offset: " << std::dec << page_offset << std::endl;
        }

        break;
      }
    }
  }

  if constexpr (SPP_SANITY_CHECK) {
    // Assertion
    if (match == ST_WAY) {
      for (match = 0; match < ST_WAY; match++) {
        if (lru[set][match] == ST_WAY - 1) { // Find replacement victim
          tag[set][match] = partial_page;
          obj_key[set][match] = obj_key_in;
          sig[set][match] = 0;
          curr_sig = sig[set][match];
          last_offset[set][match] = page_offset;

          if constexpr (SPP_DEBUG_PRINT) {
            std::cout << "[ST] " << __func__ << " miss set: " << set << " way: " << match;
            std::cout << " valid: " << valid[set][match] << " victim tag: " << std::hex << tag[set][match] << " new tag: " << partial_page;
            std::cout << " sig: " << sig[set][match] << " last_offset: " << std::dec << page_offset << std::endl;
          }

          break;
        }
      }

      // Assertion
      if (match == ST_WAY) {
        std::cout << "[ST] Cannot find a replacement victim!" << std::endl;
        assert(0);
      }
    }
  }

  // CARRY_SIG: an object region is contiguous with its neighbours, so a new region continues the signature of the
  // adjacent region whose last line is nearest, when the delta fits SPP's 7-bit signature delta. This replaces the GHR
  // bootstrap for that access.
  if (ST_hit == 0)
    misses[obj_key_in]++;
  bool carried_now = false;
  if (ST_hit == 0 && n_neighbors > 0) {
    bool found = false;
    int64_t best_delta = 0;
    uint32_t best_sig = 0;
    for (int n = 0; n < n_neighbors; n++) {
      const tag_type ntag{neighbor_key[n]};
      for (uint32_t way = 0; way < ST_WAY; way++) {
        if (way == match || !valid[set][way] || !obj_key[set][way] || !(tag[set][way] == ntag))
          continue;
        const int64_t d = obj_line - (neighbor_first_line[n] + last_offset[set][way].template to<int64_t>());
        if (d != 0 && std::abs(d) < (1 << (SIG_DELTA_BIT - 1)) && (!found || std::abs(d) < std::abs(best_delta))) {
          found = true;
          best_delta = d;
          best_sig = sig[set][way];
        }
      }
    }
    if (found) {
      last_sig = best_sig;
      delta = best_delta;
      sig_delta = (best_delta < 0) ? (((-1) * best_delta) + (1 << (SIG_DELTA_BIT - 1))) : best_delta;
      sig[set][match] = ((best_sig << SIG_SHIFT) ^ sig_delta) & SIG_MASK;
      curr_sig = sig[set][match];
      carried++;
      carried_now = true;
    }
  }

  if constexpr (GHR_ON) {
    if (ST_hit == 0 && !carried_now) {
      uint32_t GHR_found = _parent->GHR.check_entry(page_offset, obj_key_in);
      if (GHR_found < MAX_GHR_ENTRY) {
        ghr_boots[obj_key_in]++;
        sig_delta = (_parent->GHR.delta[GHR_found] < 0) ? (((-1) * _parent->GHR.delta[GHR_found]) + (1 << (SIG_DELTA_BIT - 1))) : _parent->GHR.delta[GHR_found];
        sig[set][match] = ((_parent->GHR.sig[GHR_found] << SIG_SHIFT) ^ sig_delta) & SIG_MASK;
        curr_sig = sig[set][match];
      }
    }
  }

  // Update LRU
  for (uint32_t way = 0; way < ST_WAY; way++) {
    if (lru[set][way] < lru[set][match]) {
      lru[set][way]++;

      if constexpr (SPP_SANITY_CHECK) {
        // Assertion
        if (lru[set][way] >= ST_WAY) {
          std::cout << "[ST] LRU value is wrong! set: " << set << " way: " << way << " lru: " << lru[set][way] << std::endl;
          assert(0);
        }
      }
    }
  }

  lru[set][match] = 0; // Promote to the MRU position
}

void spp_obj::PATTERN_TABLE::update_pattern(uint32_t last_sig, typename offset_type::difference_type curr_delta)
{
  // Update (sig, delta) correlation
  uint32_t set = get_hash(last_sig) % PT_SET, match = 0;

  // Case 1: Hit
  for (match = 0; match < PT_WAY; match++) {
    if (delta[set][match] == curr_delta) {
      c_delta[set][match]++;
      c_sig[set]++;
      if (c_sig[set] > C_SIG_MAX) {
        for (uint32_t way = 0; way < PT_WAY; way++)
          c_delta[set][way] >>= 1;
        c_sig[set] >>= 1;
      }

      if constexpr (SPP_DEBUG_PRINT) {
        std::cout << "[PT] " << __func__ << " hit sig: " << std::hex << last_sig << std::dec << " set: " << set << " way: " << match;
        std::cout << " delta: " << delta[set][match] << " c_delta: " << c_delta[set][match] << " c_sig: " << c_sig[set] << std::endl;
      }

      break;
    }
  }

  // Case 2: Miss
  if (match == PT_WAY) {
    uint32_t victim_way = PT_WAY, min_counter = C_SIG_MAX;

    for (match = 0; match < PT_WAY; match++) {
      if (c_delta[set][match] < min_counter) { // Select an entry with the minimum c_delta
        victim_way = match;
        min_counter = c_delta[set][match];
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

    if constexpr (SPP_DEBUG_PRINT) {
      std::cout << "[PT] " << __func__ << " miss sig: " << std::hex << last_sig << std::dec << " set: " << set << " way: " << victim_way;
      std::cout << " delta: " << delta[set][victim_way] << " c_delta: " << c_delta[set][victim_way] << " c_sig: " << c_sig[set] << std::endl;
    }

    if constexpr (SPP_SANITY_CHECK) {
      // Assertion
      if (victim_way == PT_WAY) {
        std::cout << "[PT] Cannot find a replacement victim!" << std::endl;
        assert(0);
      }
    }
  }
}

void spp_obj::PATTERN_TABLE::read_pattern(uint32_t curr_sig, std::vector<typename offset_type::difference_type>& delta_q, std::vector<uint32_t>& confidence_q,
                                          uint32_t& lookahead_way, uint32_t& lookahead_conf, uint32_t& pf_q_tail, uint32_t& depth, uint32_t pf_threshold)
{
  // Update (sig, delta) correlation
  uint32_t set = get_hash(curr_sig) % PT_SET, local_conf = 0, pf_conf = 0, max_conf = 0;

  if (c_sig[set]) {
    for (uint32_t way = 0; way < PT_WAY; way++) {
      local_conf = (100 * c_delta[set][way]) / c_sig[set];
      pf_conf = depth ? (_parent->GHR.global_accuracy * c_delta[set][way] / c_sig[set] * lookahead_conf / 100) : local_conf;

      if (pf_conf >= pf_threshold) {
        confidence_q[pf_q_tail] = pf_conf;
        delta_q[pf_q_tail] = delta[set][way];

        // Lookahead path follows the most confident entry
        if (pf_conf > max_conf) {
          lookahead_way = way;
          max_conf = pf_conf;
        }
        pf_q_tail++;

        if constexpr (SPP_DEBUG_PRINT) {
          std::cout << "[PT] " << __func__ << " HIGH CONF: " << pf_conf << " sig: " << std::hex << curr_sig << std::dec << " set: " << set << " way: " << way;
          std::cout << " delta: " << delta[set][way] << " c_delta: " << c_delta[set][way] << " c_sig: " << c_sig[set];
          std::cout << " conf: " << local_conf << " depth: " << depth << std::endl;
        }
      } else {
        if constexpr (SPP_DEBUG_PRINT) {
          std::cout << "[PT] " << __func__ << "  LOW CONF: " << pf_conf << " sig: " << std::hex << curr_sig << std::dec << " set: " << set << " way: " << way;
          std::cout << " delta: " << delta[set][way] << " c_delta: " << c_delta[set][way] << " c_sig: " << c_sig[set];
          std::cout << " conf: " << local_conf << " depth: " << depth << std::endl;
        }
      }
    }
    pf_q_tail++;

    lookahead_conf = max_conf;
    if (lookahead_conf >= pf_threshold)
      depth++;

    if constexpr (SPP_DEBUG_PRINT) {
      std::cout << "global_accuracy: " << _parent->GHR.global_accuracy << " lookahead_conf: " << lookahead_conf << std::endl;
    }
  } else {
    confidence_q[pf_q_tail] = 0;
  }
}

bool spp_obj::PREFETCH_FILTER::check(champsim::address check_addr, FILTER_REQUEST filter_request, bool promoted_pf)
{
  champsim::block_number cache_line{check_addr};
  auto hash = get_hash(cache_line.to<uint64_t>());
  auto quotient = (hash >> REMAINDER_BIT) & ((1 << QUOTIENT_BIT) - 1);
  auto remainder = hash % (1 << REMAINDER_BIT);

  if constexpr (SPP_DEBUG_PRINT) {
    std::cout << "[FILTER] check_addr: " << check_addr << " hash: " << hash << " quotient: " << quotient << " remainder: " << remainder << std::endl;
  }

  switch (filter_request) {
  case spp_obj::SPP_L2C_PREFETCH:
    if ((valid[quotient] || useful[quotient]) && remainder_tag[quotient] == remainder) {
      if constexpr (SPP_DEBUG_PRINT) {
        std::cout << "[FILTER] " << __func__ << " line is already in the filter check_addr: " << check_addr << " cache_line: " << cache_line;
        std::cout << " quotient: " << quotient << " valid: " << valid[quotient] << " useful: " << useful[quotient] << std::endl;
      }

      return false; // False return indicates "Do not prefetch"
    } else {
      valid[quotient] = 1;  // Mark as prefetched
      useful[quotient] = 0; // Reset useful bit
      remainder_tag[quotient] = remainder;
      promoted[quotient] = promoted_pf;

      if constexpr (SPP_DEBUG_PRINT) {
        std::cout << "[FILTER] " << __func__ << " set valid for check_addr: " << check_addr << " cache_line: " << cache_line;
        std::cout << " quotient: " << quotient << " remainder_tag: " << remainder_tag[quotient] << " valid: " << valid[quotient]
                  << " useful: " << useful[quotient] << std::endl;
      }
    }
    break;

  case spp_obj::SPP_LLC_PREFETCH:
    if ((valid[quotient] || useful[quotient]) && remainder_tag[quotient] == remainder) {
      if constexpr (SPP_DEBUG_PRINT) {
        std::cout << "[FILTER] " << __func__ << " line is already in the filter check_addr: " << check_addr << " cache_line: " << cache_line;
        std::cout << " quotient: " << quotient << " valid: " << valid[quotient] << " useful: " << useful[quotient] << std::endl;
      }

      return false; // False return indicates "Do not prefetch"
    } else {
      // NOTE: SPP_LLC_PREFETCH has relatively low confidence (FILL_THRESHOLD <= SPP_LLC_PREFETCH < PF_THRESHOLD)
      // Therefore, it is safe to prefetch this cache line in the large LLC and save precious L2C capacity
      // If this prefetch request becomes more confident and SPP eventually issues SPP_L2C_PREFETCH,
      // we can get this cache line immediately from the LLC (not from DRAM)
      // To allow this fast prefetch from LLC, SPP does not set the valid bit for SPP_LLC_PREFETCH

      // valid[quotient] = 1;
      // useful[quotient] = 0;

      if constexpr (SPP_DEBUG_PRINT) {
        std::cout << "[FILTER] " << __func__ << " don't set valid for check_addr: " << check_addr << " cache_line: " << cache_line;
        std::cout << " quotient: " << quotient << " valid: " << valid[quotient] << " useful: " << useful[quotient] << std::endl;
      }
    }
    break;

  case spp_obj::L2C_DEMAND:
    if ((remainder_tag[quotient] == remainder) && (useful[quotient] == 0)) {
      useful[quotient] = 1;
      if (valid[quotient])
        _parent->GHR.pf_useful++; // This cache line was prefetched by SPP and actually used in the program
      if (valid[quotient] && promoted[quotient])
        _parent->prom_useful++;

      if constexpr (SPP_DEBUG_PRINT) {
        std::cout << "[FILTER] " << __func__ << " set useful for check_addr: " << check_addr << " cache_line: " << cache_line;
        std::cout << " quotient: " << quotient << " valid: " << valid[quotient] << " useful: " << useful[quotient];
        std::cout << " GHR.pf_issued: " << _parent->GHR.pf_issued << " GHR.pf_useful: " << _parent->GHR.pf_useful << std::endl;
      }
    }
    break;

  case spp_obj::L2C_EVICT:
    // Decrease global pf_useful counter when there is a useless prefetch (prefetched but not used)
    if (valid[quotient] && !useful[quotient] && _parent->GHR.pf_useful)
      _parent->GHR.pf_useful--;

    // Reset filter entry
    valid[quotient] = 0;
    useful[quotient] = 0;
    remainder_tag[quotient] = 0;
    promoted[quotient] = 0;
    break;

  default:
    // Assertion
    std::cout << "[FILTER] Invalid filter request type: " << filter_request << std::endl;
    assert(0);
  }

  return true;
}

void spp_obj::GLOBAL_REGISTER::update_entry(uint32_t pf_sig, uint32_t pf_confidence, offset_type pf_offset, typename offset_type::difference_type pf_delta,
                                            bool kind)
{
  // NOTE: GHR implementation is slightly different from the original paper
  // Instead of matching (last_offset + delta), GHR simply stores and matches the pf_offset
  uint32_t min_conf = 100, victim_way = 0;

  if constexpr (SPP_DEBUG_PRINT) {
    std::cout << "[GHR] Crossing the page boundary pf_sig: " << std::hex << pf_sig << std::dec;
    std::cout << " confidence: " << pf_confidence << " pf_offset: " << pf_offset << " pf_delta: " << pf_delta << std::endl;
  }

  for (uint32_t i = 0; i < MAX_GHR_ENTRY; i++) {
    // if (sig[i] == pf_sig) { // TODO: Which one is better and consistent?
    //  If GHR already holds the same pf_sig, update the GHR entry with the latest info
    if (valid[i] && (offset[i] == pf_offset) && (obj_key[i] == kind)) {
      // If GHR already holds the same pf_offset, update the GHR entry with the latest info
      sig[i] = pf_sig;
      confidence[i] = pf_confidence;
      // offset[i] = pf_offset;
      delta[i] = pf_delta;

      if constexpr (SPP_DEBUG_PRINT) {
        std::cout << "[GHR] Found a matching index: " << i << std::endl;
      }

      return;
    }

    // GHR replacement policy is based on the stored confidence value
    // An entry with the lowest confidence is selected as a victim
    if (confidence[i] <= min_conf) {
      min_conf = confidence[i];
      victim_way = i;
    }
  }

  // Assertion
  if (victim_way >= MAX_GHR_ENTRY) {
    std::cout << "[GHR] Cannot find a replacement victim!" << std::endl;
    assert(0);
  }

  if constexpr (SPP_DEBUG_PRINT) {
    std::cout << "[GHR] Replace index: " << victim_way << " pf_sig: " << std::hex << sig[victim_way] << std::dec;
    std::cout << " confidence: " << confidence[victim_way] << " pf_offset: " << offset[victim_way] << " pf_delta: " << delta[victim_way] << std::endl;
  }

  valid[victim_way] = 1;
  sig[victim_way] = pf_sig;
  confidence[victim_way] = pf_confidence;
  offset[victim_way] = pf_offset;
  delta[victim_way] = pf_delta;
  obj_key[victim_way] = kind;
}

uint32_t spp_obj::GLOBAL_REGISTER::check_entry(offset_type page_offset, bool kind)
{
  uint32_t max_conf = 0, max_conf_way = MAX_GHR_ENTRY;

  for (uint32_t i = 0; i < MAX_GHR_ENTRY; i++) {
    if ((offset[i] == page_offset) && (obj_key[i] == kind) && (max_conf < confidence[i])) {
      max_conf = confidence[i];
      max_conf_way = i;
    }
  }

  return max_conf_way;
}