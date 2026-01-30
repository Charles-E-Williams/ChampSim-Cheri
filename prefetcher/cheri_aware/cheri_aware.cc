#include "cheri_aware.h"

#include <cassert>
#include <iostream>
#include <algorithm>

void cheri_aware::prefetcher_initialize() {
  cpu_id = static_cast<uint32_t>(this->intern_->cpu);
  std::cout << "[CHERI-Aware] Initialized for CPU " << cpu_id << std::endl;
  std::cout << "[CHERI-Aware] Stride degree: " << PREFETCH_DEGREE << std::endl;
  std::cout << "[CHERI-Aware] Pointer chase degree: " << POINTER_CHASE_DEGREE << std::endl;
  std::cout << "[CHERI-Aware] Max chase depth: " << MAX_CHASE_DEPTH << std::endl;
}

uint32_t cheri_aware::prefetcher_cache_operate(champsim::address addr, 
                                               champsim::address ip, 
                                               uint8_t cache_hit, 
                                               bool useful_prefetch, 
                                               access_type type, 
                                               uint32_t metadata_in) {
    champsim::block_number cl_addr{addr};
    champsim::block_number::difference_type stride = 0;
    
    auto found = table.check_hit({ip, cl_addr, stride});
    
    // Guard against non-CHERI mode
    bool has_cap = false;
    if (cpu_id < champsim::cap_mem.size()) {
        auto cap_opt = champsim::cap_mem[cpu_id].load_capability(addr);
        has_cap = cap_opt.has_value() && cap_opt->tag;
        
        // Only track capability LOADS (not stores)
        if (has_cap && type == access_type::LOAD) {
            stat_capability_hits++;
            
            // Only do pointer chasing for LOAD HITs (when we actually have the capability data)
            if (cache_hit) {
                auto cap = cap_opt.value();
                
                // Calculate where this capability points to
                champsim::address target = cap.base + cap.offset.to<int64_t>();
                champsim::address cap_bound = cap.base + cap.length.to<uint64_t>();
                

                int64_t distance = std::abs(target.to<int64_t>() - addr.to<int64_t>());
                
                if (target != addr && 
                    target >= cap.base && 
                    target < cap_bound &&
                    distance >= 64 &&        // At least 1 cache line away
                    distance < 16384) {      // Less than 256 cache lines away
                    
                    pointer_chase_queue.push_back({
                        target,
                        1,
                        cap.base,
                        cap_bound
                    });
                    stat_pointer_chases_initiated++;
                    
                    // std::cout << "[PREFETCHER] Capability load at 0x" << std::hex << addr.to<uint64_t>()
                    //           << " points to 0x" << target.to<uint64_t>()
                    //           << " (distance: " << std::dec << distance << " bytes)" << std::endl;
                }
            }
            
            // Stride detection for capability arrays
            if (found.has_value()) {
                stride = champsim::offset(found->last_cl_addr, cl_addr);
                
                if (stride != 0 && stride == found->last_stride) {
                    active_lookahead = {
                        champsim::address{cl_addr}, 
                        stride, 
                        PREFETCH_DEGREE,
                        champsim::address{0},
                        champsim::address{std::numeric_limits<uint64_t>::max()},
                        true
                    };
                }
            }
        }
    }
    
    // Regular stride prefetching for non-capability accesses
    if (!has_cap) {
        stat_prefetches_no_capability++;
        
        if (found.has_value()) {
            stride = champsim::offset(found->last_cl_addr, cl_addr);
            
            if (stride != 0 && stride == found->last_stride) {
                active_lookahead = {
                    champsim::address{cl_addr}, 
                    stride, 
                    PREFETCH_DEGREE,
                    champsim::address{0},
                    champsim::address{std::numeric_limits<uint64_t>::max()},
                    false
                };
            }
        }
    }
    
    table.fill({ip, cl_addr, stride});
    return metadata_in;
}

void cheri_aware::prefetcher_cycle_operate() {
    // Handle stride prefetching
    handle_stride_prefetch();
    
    // Handle pointer chasing (only if MSHR not too busy)
    if (!pointer_chase_queue.empty() && intern_->get_mshr_occupancy_ratio() < 0.4) {
        handle_pointer_chase();
    }
}

void cheri_aware::handle_stride_prefetch() {
    if (!active_lookahead.has_value()) {
        return;
    }
    
    auto [old_pf_address, stride, degree, cap_base, cap_bound, is_cap_stride] = active_lookahead.value();
    
    if (degree <= 0) {
        active_lookahead.reset();
        return;
    }
    
    champsim::address pf_address{champsim::block_number{old_pf_address} + stride};
    
    bool same_page = intern_->virtual_prefetch || 
                     (champsim::page_number{pf_address} == champsim::page_number{old_pf_address});
    
    if (!same_page) {
        active_lookahead.reset();
        return;
    }
    
    const bool mshr_ok = intern_->get_mshr_occupancy_ratio() < 0.4;
    const bool success = issue_prefetch(pf_address, mshr_ok);
    
    if (success) {
        stat_stride_prefetches++;
        active_lookahead = {
            pf_address, 
            stride, 
            degree - 1, 
            cap_base, 
            cap_bound,
            is_cap_stride
        };
        
        // For capability stride prefetching, check if we can recursively chase
        if (is_cap_stride && cpu_id < champsim::cap_mem.size()) {
            auto cap_opt = champsim::cap_mem[cpu_id].load_capability(pf_address);
            if (cap_opt.has_value() && cap_opt->tag) {
                auto prefetched_cap = cap_opt.value();
                champsim::address cursor = prefetched_cap.base + prefetched_cap.offset.to<int64_t>();
                champsim::address bound = prefetched_cap.base + prefetched_cap.length.to<uint64_t>();
                
                // Same distance checks as in cache_operate
                int64_t distance = std::abs(cursor.to<int64_t>() - pf_address.to<int64_t>());
                
                if (cursor != pf_address &&
                    cursor >= prefetched_cap.base && 
                    cursor < bound &&
                    distance >= 64 && 
                    distance < 16384) {
                    
                    pointer_chase_queue.push_back({cursor, 1, prefetched_cap.base, bound});
                }
            }
        }
    }
}

void cheri_aware::handle_pointer_chase() {
    if (pointer_chase_queue.empty()) {
        return;
    }
    
    auto chase = pointer_chase_queue.front();
    pointer_chase_queue.pop_front();
    
    // Bounds check
    if (chase.target_addr < chase.bounds_base || chase.target_addr >= chase.bounds_limit) {
        stat_prefetches_blocked_by_bounds++;
        return;
    }
    
    stat_prefetches_bounds_ok++;
    
    const bool mshr_ok = intern_->get_mshr_occupancy_ratio() < 0.3;
    
    // Issue prefetch for the target
    bool success = issue_prefetch(chase.target_addr, mshr_ok);
    if (success) {
        stat_pointer_prefetches++;
    }
    
    // Issue additional prefetches for nearby cache lines (but only 1, not multiple)
    // Reduced from POINTER_CHASE_DEGREE to just 1 additional line
    if (POINTER_CHASE_DEGREE > 1) {
        champsim::address next_line = chase.target_addr + 64;
        
        if (next_line < chase.bounds_limit) {
            if (issue_prefetch(next_line, mshr_ok)) {
                stat_pointer_prefetches++;
                stat_prefetches_bounds_ok++;
            }
        }
    }
    
    // Recursive chasing: follow the pointer at the target location
    if (chase.depth < MAX_CHASE_DEPTH && cpu_id < champsim::cap_mem.size()) {
        auto cap_opt = champsim::cap_mem[cpu_id].load_capability(chase.target_addr);
        
        if (cap_opt.has_value() && cap_opt->tag) {
            auto next_cap = cap_opt.value();
            champsim::address next_target = next_cap.base + next_cap.offset.to<int64_t>();
            champsim::address next_bound = next_cap.base + next_cap.length.to<uint64_t>();
            
            // Same filtering as before
            int64_t distance = std::abs(next_target.to<int64_t>() - chase.target_addr.to<int64_t>());
            
            if (next_target != chase.target_addr &&
                next_target >= next_cap.base && 
                next_target < next_bound &&
                distance >= 64 && 
                distance < 16384) {
                
                pointer_chase_queue.push_back({
                    next_target,
                    chase.depth + 1,
                    next_cap.base,
                    next_bound
                });
                stat_recursive_chases++;
            }
        }
    }
}

bool cheri_aware::issue_prefetch(champsim::address addr, bool fill_level) {
    bool success = prefetch_line(addr, fill_level, 0);
    if (success) {
        stat_prefetches_issued++;
    }
    return success;
}

uint32_t cheri_aware::prefetcher_cache_fill(champsim::address addr, long set, long way, 
                                            uint8_t prefetch, champsim::address evicted_addr, 
                                            uint32_t metadata_in) {
    return metadata_in;
}

void cheri_aware::prefetcher_final_stats() {
    std::cout << "\n=== CHERI-Aware Prefetcher Statistics ===" << std::endl;
    std::cout << "  Total prefetches issued: " << stat_prefetches_issued << std::endl;
    std::cout << "    Stride prefetches: " << stat_stride_prefetches << std::endl;
    std::cout << "    Pointer chase prefetches: " << stat_pointer_prefetches << std::endl;
    std::cout << std::endl;
    
    std::cout << "  Capability tracking:" << std::endl;
    std::cout << "    Capability loads detected: " << stat_capability_hits << std::endl;
    std::cout << "    Non-capability accesses: " << stat_prefetches_no_capability << std::endl;
    std::cout << "    Pointer chases initiated: " << stat_pointer_chases_initiated << std::endl;
    std::cout << "    Recursive chases: " << stat_recursive_chases << std::endl;
    std::cout << std::endl;
    
    std::cout << "  Bounds checking:" << std::endl;
    std::cout << "    Prefetches within bounds: " << stat_prefetches_bounds_ok << std::endl;
    std::cout << "    Prefetches blocked by bounds: " << stat_prefetches_blocked_by_bounds << std::endl;
    
    if (stat_prefetches_bounds_ok + stat_prefetches_blocked_by_bounds > 0) {
        double total_checked = static_cast<double>(stat_prefetches_bounds_ok + stat_prefetches_blocked_by_bounds);
        double bounds_respect_rate = (100.0 * static_cast<double>(stat_prefetches_bounds_ok)) / total_checked;
        std::cout << "    Bounds respect rate: " << bounds_respect_rate << "%" << std::endl;
    }
    
    if (stat_pointer_chases_initiated > 0) {
        double avg_depth = static_cast<double>(stat_recursive_chases) /
                       static_cast<double>(stat_pointer_chases_initiated);
        std::cout << "    Average chase depth: " << (1.0 + avg_depth) << std::endl;
    }
    
    std::cout << "======================================\n" << std::endl;
}