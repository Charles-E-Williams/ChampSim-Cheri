#include "cheri_ptr_chase.h"

void cheri_ptr_chase::pct_train(champsim::address ip, bool found_pointer)
{
  auto found = pct.check_hit({ip, 0, 0});

  if (found.has_value()) {
    auto entry = *found;

    if (found_pointer) {
      if (entry.confidence < CONF_MAX)
        entry.confidence++;
      // Promote depth when confidence is high enough
      if (entry.confidence >= DEPTH_PROMOTE && entry.depth_limit < MAX_DEPTH)
        entry.depth_limit++;
    } else {
      if (entry.confidence > 0)
        entry.confidence--;
      // Contract depth when confidence drops
      if (entry.confidence <= DEPTH_DEMOTE && entry.depth_limit > 1)
        entry.depth_limit--;
    }

    if constexpr (DEBUG_PRINT) {
      fmt::print("[PCT-TRAIN] ip: {} {} conf: {} depth: {}\n",
                 ip, found_pointer ? "PTR_HIT" : "PTR_MISS",
                 entry.confidence, entry.depth_limit);
    }

    pct.fill(entry);
  } else {
    // Cold insert: if we found a pointer, start with confidence 1
    uint8_t init_conf = found_pointer ? 1 : 0;

    if constexpr (DEBUG_PRINT) {
      fmt::print("[PCT-TRAIN] ip: {} COLD INSERT conf: {}\n", ip, init_conf);
    }

    pct.fill({ip, init_conf, 1});
  }
}

void cheri_ptr_chase::pct_chase(champsim::address ip, uint64_t start_cursor)
{
  auto found = pct.check_hit({ip, 0, 0});

    if (!found.has_value() || found->confidence < CHASE_THRESH) {
        stat_conf_too_low++;
        if constexpr (DEBUG_PRINT) {
            if (found->confidence)
                fmt::print("[PCT-CHASE] ip: {} conf {} < thresh {}, skip\n",
                    ip, found->confidence, CHASE_THRESH);
        } 
        return;
    }

    stat_chase_started++;
    uint64_t cursor = start_cursor;
    uint8_t  hops   = found->depth_limit;

    for (uint8_t d = 0; d < hops; d++) {
        uint64_t cl = cursor >> LOG2_BLOCK_SIZE;
        uint64_t idx = filter.hash(cl) % PTR_MAP_SIZE;

        // Check if we have learned this access yet
        if (ptr_map[idx].cl_tag != cl) {
            stat_map_miss++;
            break; 
        }

        uint64_t next_target = ptr_map[idx].target;
        if (next_target == 0 || next_target == cursor) {
            stat_null_or_loop++;
            break;
        }

        champsim::address pf_addr{next_target};
        if (!filter.test(pf_addr)) {
            filter.add(pf_addr);
            prefetch_line(pf_addr, false, 0); // false = place deeper chases into the l2
            stat_pf_issued++;
        } else {
            stat_bloom_filtered++;
        }
        cursor = next_target;
    }
}