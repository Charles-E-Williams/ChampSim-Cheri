#include <iostream>
#include <iomanip>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <cmath>
#include "cache.h"
#include "spp_ppf_cheri.h"

// =============================================================================
// Initialization
// =============================================================================

void spp_ppf_cheri::PPF_Module::init(CACHE* cache)
{
    cache_ = cache;
    for (int a = 0; a < 30; a++)
        depth_track[a] = 0;

    ST.parent_ = this;
    PT.parent_ = this;
    FILTER.parent_ = this;
    GHR.parent_ = this;
    PERC.parent_ = this;

    stat_pf_bounded_by_cap = 0;
}

void spp_ppf_cheri::prefetcher_initialize()
{
    module_.init(intern_);
}

// =============================================================================
// prefetcher_cache_operate — grabs auth_cap, then delegates to do_prefetch
// =============================================================================

uint32_t spp_ppf_cheri::prefetcher_cache_operate(champsim::address addr, champsim::address ip, uint8_t cache_hit, bool useful_prefetch, access_type type,
                                                  uint32_t metadata_in)
{
    // Grab the authorizing capability (guaranteed tagged under DPC4)
    module_.current_cap_ = intern_->get_authorizing_capability();
    module_.do_prefetch(addr, ip, cache_hit, useful_prefetch, type, metadata_in);
    return metadata_in;
}

// =============================================================================
// do_prefetch — stock SPP lookahead with CHERI-aware perceptron filtering
// =============================================================================

void spp_ppf_cheri::PPF_Module::do_prefetch(champsim::address addr, champsim::address ip,
                                             uint8_t cache_hit, bool useful_prefetch, access_type type,
                                             uint32_t metadata_in, double confidence_modifier)
{
    champsim::page_number page{addr};
    offset_type page_offset{addr};
    std::vector<uint32_t> confidence_q(100 * cache_->get_mshr_size(), 0);
    uint32_t last_sig = 0,
             curr_sig = 0,
             depth = 0;

    std::vector<typename offset_type::difference_type> delta_q(100 * cache_->get_mshr_size(), 0);
    std::vector<int32_t> perc_sum_q(100 * cache_->get_mshr_size(), 0);
    typename offset_type::difference_type delta = 0;

    double bounded_modifier = confidence_modifier;
    if (bounded_modifier < 0.0) bounded_modifier = 0.0;
    if (bounded_modifier > 1.0) bounded_modifier = 1.0;
    confidence_q[0] = std::max(1u, static_cast<uint32_t>(std::round(100.0 * bounded_modifier)));
    GHR.global_accuracy = GHR.pf_issued ? ((100 * GHR.pf_useful) / GHR.pf_issued) : 0;

    for (int i = PAGES_TRACKED - 1; i > 0; i--) {
        GHR.page_tracker[i] = GHR.page_tracker[i - 1];
    }
    GHR.page_tracker[0] = page;

    int distinct_pages = 0;
    uint8_t num_pf = 0;
    for (int i = 0; i < PAGES_TRACKED; i++) {
        int j;
        for (j = 0; j < i; j++) {
            if (GHR.page_tracker[i] == GHR.page_tracker[j])
                break;
        }
        if (i == j)
            distinct_pages++;
    }

    if (SPP_DEBUG_PRINT) {
        fmt::print("\n[ChampSim] prefetcher_cache_operate addr: {0:x} cache_line: {0:x}", addr, champsim::block_number(addr));
        fmt::print(" page: {0:x} page_offset: {}\n", page, page_offset);
    }

    // Stage 1: Read and update signature — completely stock
    ST.read_and_update_sig(page, page_offset, last_sig, curr_sig, delta);

    FILTER.train_neg = 1;

    // Check the prefetch filter for demand hits (training feedback)
    // CHERI values are zero for L2C_DEMAND — they're not used for training index reconstruction
    // because L2C_DEMAND replays stored features from the accept/reject filter entries
    FILTER.check(addr, champsim::address{}, champsim::address{}, L2C_DEMAND, 0, 0, 0, 0, 0, 0, 0, 0, 0);

    // Stage 2: Update delta patterns — completely stock
    if (last_sig) PT.update_pattern(last_sig, delta);

    // =========================================================================
    // Extract CHERI metadata for perceptron features
    // =========================================================================
    const auto& cap = current_cap_;
    uint64_t cap_base_val = cap.base.to<uint64_t>();
    uint64_t cap_length_val = cap.length.to<uint64_t>();
    uint64_t cap_offset_val = cap.offset.to<uint64_t>();
    uint64_t demand_va = cheri::capability_cursor(cap).to<uint64_t>();
    uint64_t cap_top = cheri::capability_top(cap).to<uint64_t>();

    // Feature 9: object_size_class — log2 of capability length
    uint64_t cheri_size_class = (cap_length_val > 0) ? champsim::lg2(cap_length_val) : 0;
    // Feature 10: cap_offset_cl — CL offset of access within the object
    uint64_t cheri_offset_cl = cap_offset_val >> LOG2_BLOCK_SIZE;

    // Feature 11 (remaining_cls_in_dir) is computed per-candidate in read_pattern
    // because it depends on the delta direction. We pass demand_va, cap_base, cap_top.

    // Stage 3: Start prefetching — stock lookahead with CHERI-aware perceptron + bounds gate
    champsim::address base_addr = addr;
    champsim::address curr_ip = ip;
    uint32_t lookahead_conf = 100,
             pf_q_head = 0,
             pf_q_tail = 0;
    uint8_t do_lookahead = 0;
    int32_t prev_delta = 0;

    champsim::address train_addr = addr;
    int32_t train_delta = 0;

    GHR.ip_3 = GHR.ip_2;
    GHR.ip_2 = GHR.ip_1;
    GHR.ip_1 = GHR.ip_0;
    GHR.ip_0 = ip;

    if (LOOKAHEAD_ON) {
        do {
            uint32_t lookahead_way = PT_WAY;

            train_addr = addr;
            train_delta = prev_delta;

            // Read PT — passes CHERI metadata for per-candidate remaining_cls computation
            PT.read_pattern(curr_sig, delta_q, confidence_q, perc_sum_q, lookahead_way, lookahead_conf,
                            pf_q_tail, depth, addr, base_addr, train_addr, curr_ip, train_delta, last_sig,
                            cache_->get_pq_occupancy().back(), cache_->get_pq_size().back(),
                            cache_->get_mshr_occupancy(), cache_->get_mshr_size(),
                            cheri_size_class, cheri_offset_cl,
                            demand_va, cap_base_val, cap_top);

            do_lookahead = 0;
            for (uint32_t i = pf_q_head; i < pf_q_tail; i++) {
                champsim::address pf_addr{(champsim::block_number{base_addr} + delta_q[i])};
                int32_t perc_sum = perc_sum_q[i];

                if (SPP_DEBUG_PRINT)
                    std::cout << "[ChampSim] State of features: \nTrain addr: " << train_addr
                              << "\tCurr IP: " << curr_ip
                              << "\tIP_1: " << GHR.ip_1 << "\tIP_2: " << GHR.ip_2 << "\tIP_3: " << GHR.ip_3
                              << "\tDelta: " << train_delta + delta_q[i]
                              << "\t:LastSig " << last_sig << "\t:CurrSig " << curr_sig
                              << "\t:Conf " << confidence_q[i] << "\t:Depth " << depth
                              << "\tSUM: " << perc_sum << std::endl;

                FILTER_REQUEST fill_level = (perc_sum >= PERC_THRESHOLD_HI) ? SPP_L2C_PREFETCH : SPP_LLC_PREFETCH;

                if (champsim::page_number{addr} == champsim::page_number{pf_addr}) { // Same physical page

                    // Pre-filter: optional region map check
                    if (region_map_check_ && region_map_check_(pf_addr)) {
                        do_lookahead = 1;
                        pf_q_head++;
                        continue;
                    }

                    // =========================================================
                    // CHERI hard bounds gate: check if prefetch VA is in-bounds
                    // =========================================================
                    uint64_t pf_va = demand_va + (static_cast<int64_t>(delta_q[i]) << LOG2_BLOCK_SIZE);
                    if (!cheri::prefetch_safe(champsim::address{pf_va}, cap)) {
                        stat_pf_bounded_by_cap++;
                        do_lookahead = 1;
                        pf_q_head++;
                        continue; // Suppress out-of-bounds prefetch
                    }

                    // Compute remaining_cls for this candidate's direction
                    // Must match the direction used in read_pattern's perc_predict call
                    int64_t total_delta_for_dir = train_delta + delta_q[i];
                    int direction = (total_delta_for_dir > 0) ? 1 : ((total_delta_for_dir < 0) ? -1 : 0);
                    champsim::block_number demand_va_block{champsim::address{demand_va}};
                    uint64_t cheri_remaining_cls = static_cast<uint64_t>(
                        cheri::remaining_lines(demand_va_block, direction,
                                               champsim::address{cap_base_val}, champsim::address{cap_top}));

                    if (num_pf < ceil(((cache_->get_pq_size().back()) / distinct_pages))) {
                        if (FILTER.check(pf_addr, train_addr, curr_ip, fill_level,
                                         train_delta + delta_q[i], last_sig, curr_sig,
                                         confidence_q[i], perc_sum, (depth - 1),
                                         cheri_size_class, cheri_offset_cl, cheri_remaining_cls)) {

                            // Histogramming
                            int32_t perc_sum_shifted = perc_sum + (PERC_COUNTER_MAX + 1) * PERC_FEATURES;
                            int32_t hist_index = perc_sum_shifted / 10;
                            FILTER.hist_tots[hist_index]++;

                            // Issue prefetch
                            cache_->prefetch_line(pf_addr, (fill_level == SPP_L2C_PREFETCH), 0, cap);

                            if (fill_level == SPP_L2C_PREFETCH) {
                                GHR.pf_issued++;
                                GHR.pf_l2c++;
                                if (GHR.pf_issued > GLOBAL_COUNTER_MAX) {
                                    GHR.pf_issued >>= 1;
                                    GHR.pf_useful >>= 1;
                                }
                            }
                            GHR.pf_total++;
                            if (fill_level == SPP_LLC_PREFETCH) GHR.pf_llc++;

                            GHR.perc_pass++;
                            GHR.depth_val = 1;
                            num_pf++;

                            if (SPP_DEBUG_PRINT) {
                                std::cout << "[" << cache_->NAME << "] " << __func__
                                          << " prefetch addr: " << std::hex << pf_addr << std::dec
                                          << " confidence: " << confidence_q[i]
                                          << " depth: " << depth << std::endl;
                            }
                        }
                    }
                } else {
                    // Cross-page: update GHR for cross-page tracking (stock behavior)
                    if (GHR_ON) {
                        if (SPP_SANITY_CHECK && (pf_q_head != pf_q_tail)) {
                            if (SPP_DEBUG_PRINT) {
                                std::cout << "[" << cache_->NAME << "] " << __func__
                                          << " twice: page boundary crossing detected for GHR update" << std::endl;
                            }
                        }
                        GHR.update_entry(curr_sig, confidence_q[i], offset_type{pf_addr}, delta_q[i]);
                    }
                }
                do_lookahead = 1;
                pf_q_head++;
            }

            // Lookahead (stock SPP behavior): once a PT way is selected, update path state.
            // Loop continuation is still controlled by do_lookahead in the surrounding do/while.
            if (lookahead_way < PT_WAY) {
                uint32_t set = get_hash(curr_sig) % PT_SET;
                base_addr += (PT.delta[set][lookahead_way] << LOG2_BLOCK_SIZE);
                prev_delta += PT.delta[set][lookahead_way];

                int sig_delta = (PT.delta[set][lookahead_way] < 0)
                    ? (((-1) * PT.delta[set][lookahead_way]) + (1 << (SIG_DELTA_BIT - 1)))
                    : PT.delta[set][lookahead_way];
                curr_sig = ((curr_sig << SIG_SHIFT) ^ sig_delta) & SIG_MASK;
            }

            if (SPP_DEBUG_PRINT) {
                std::cout << "Looping curr_sig: " << std::hex << curr_sig << " base_addr: " << base_addr << std::dec;
                std::cout << " pf_q_head: " << pf_q_head << " pf_q_tail: " << pf_q_tail << " depth: " << depth << std::endl;
            }

        } while (do_lookahead);
    }

    // Stats
    if (GHR.depth_val) {
        GHR.depth_num++;
        GHR.depth_sum += depth;
    }
    if (depth < 30)
        depth_track[depth]++;
}

// =============================================================================
// prefetcher_cache_fill — stock training feedback via FILTER
// =============================================================================

void spp_ppf_cheri::PPF_Module::handle_fill(champsim::address addr, long set, long way,
                                             uint8_t prefetch, champsim::address evicted_addr, uint32_t metadata_in)
{
    // Prefetch dropped
    if (way == cache_->NUM_WAY && evicted_addr == champsim::address{}) {
        FILTER.check(addr, champsim::address{}, champsim::address{}, L2C_EVICT, 0, 0, 0, 0, 0, 0, 0, 0, 0);
        return;
    }

    if (FILTER_ON) {
        if (SPP_DEBUG_PRINT) {
            fmt::print("\n");
        }
        FILTER.check(evicted_addr, champsim::address{}, champsim::address{}, L2C_EVICT, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    }
}

uint32_t spp_ppf_cheri::prefetcher_cache_fill(champsim::address addr,long set, long way,
                                               uint8_t prefetch, champsim::address evicted_addr, uint32_t metadata_in)
{
    module_.handle_fill(addr,set, way, prefetch, evicted_addr, metadata_in);
    return metadata_in;
}

// =============================================================================
// Hash function — stock
// =============================================================================

uint64_t spp_ppf_cheri::get_hash(uint64_t key)
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

// =============================================================================
// SIGNATURE_TABLE — completely stock
// =============================================================================

void spp_ppf_cheri::PPF_Module::SIGNATURE_TABLE::read_and_update_sig(champsim::page_number page, offset_type page_offset,
                                                                      uint32_t &last_sig, uint32_t &curr_sig,
                                                                      typename offset_type::difference_type &delta)
{
    uint32_t set = spp_ppf_cheri::get_hash(page.to<uint64_t>()) % ST_SET, match = ST_WAY;
    tag_type partial_page{champsim::address{page}};
    uint8_t ST_hit = 0;
    long sig_delta{0};

    if (SPP_DEBUG_PRINT) {
        fmt::print("[ST] read_and_update_sig page: {} partial_page: {}\n", page, partial_page);
    }

    // Case 1: Hit
    for (match = 0; match < ST_WAY; match++) {
        if (valid[set][match] && (tag[set][match] == partial_page)) {
            last_sig = sig[set][match];
            delta = champsim::offset(last_offset[set][match], page_offset);

            if (delta) {
                sig_delta = (delta < 0) ? (((-1) * delta) + (1 << (SIG_DELTA_BIT - 1))) : delta;
                curr_sig = ((last_sig << SIG_SHIFT) ^ sig_delta) & SIG_MASK;
                last_offset[set][match] = page_offset;
                sig[set][match] = curr_sig;
            } else {
                // Hitting the same cache line, delta is zero.
                last_sig = 0;
            }

            if (SPP_DEBUG_PRINT) {
                std::cout << "[ST] " << __func__ << " HIT set: " << set << " way: " << match;
                std::cout << " page: " << std::hex << page << std::dec;
                std::cout << " old_sig: " << std::hex << last_sig << " new_sig: " << curr_sig << std::dec;
                std::cout << " delta: " << delta << std::endl;
            }

            ST_hit = 1;
            break;
        }
    }

    // Case 2: Miss
    if (ST_hit == 0) {
        uint32_t victim_way = ST_WAY;
        for (uint32_t way = 0; way < ST_WAY; way++) {
            if (valid[set][way] == 0) {
                victim_way = way;
                break;
            }
        }

        if (victim_way == ST_WAY) {
            for (uint32_t way = 0; way < ST_WAY; way++) {
                if (lru[set][way] == (ST_WAY - 1)) {
                    victim_way = way;
                    break;
                }
            }
        }

        if (SPP_DEBUG_PRINT) {
            std::cout << "[ST] " << __func__ << " MISS set: " << set << " way: " << victim_way;
            std::cout << " page: " << std::hex << page << std::dec;
            std::cout << " old_sig: " << std::hex << sig[set][victim_way] << std::dec << std::endl;
        }

        valid[set][victim_way] = 1;
        tag[set][victim_way] = partial_page;
        sig[set][victim_way] = 0;
        last_offset[set][victim_way] = page_offset;

        // GHR bootstrap
        if (GHR_ON) {
            uint32_t GHR_entry = parent_->GHR.check_entry(page_offset);
            if (GHR_entry < MAX_GHR_ENTRY) {
                sig[set][victim_way] = parent_->GHR.sig[GHR_entry];
                last_sig = sig[set][victim_way];

                auto ghr_delta = parent_->GHR.delta[GHR_entry];
                if (ghr_delta) {
                    sig_delta = (ghr_delta < 0) ? (((-1) * ghr_delta) + (1 << (SIG_DELTA_BIT - 1))) : ghr_delta;
                    curr_sig = ((last_sig << SIG_SHIFT) ^ sig_delta) & SIG_MASK;
                    last_offset[set][victim_way] = page_offset;
                    sig[set][victim_way] = curr_sig;
                    delta = ghr_delta;
                }

                if (SPP_DEBUG_PRINT) {
                    std::cout << "[ST] " << __func__ << " GHR bootstrap sig: " << std::hex << sig[set][victim_way] << std::dec;
                    std::cout << " delta: " << delta << std::endl;
                }
            }
        }

        match = victim_way;
    }

    // LRU update
    int position = lru[set][match];
    for (uint32_t way = 0; way < ST_WAY; way++) {
        if (lru[set][way] < (unsigned)position)
            lru[set][way]++;
    }
    lru[set][match] = 0;
}

// =============================================================================
// PATTERN_TABLE::update_pattern — completely stock
// =============================================================================

void spp_ppf_cheri::PPF_Module::PATTERN_TABLE::update_pattern(uint32_t last_sig, typename offset_type::difference_type curr_delta)
{
    // Update (sig, delta) correlation
    uint32_t set = spp_ppf_cheri::get_hash(last_sig) % PT_SET,
             match = 0;

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

            if (SPP_DEBUG_PRINT) {
                std::cout << "[PT] " << __func__ << " hit sig: " << std::hex << last_sig << std::dec << " set: " << set << " way: " << match;
                std::cout << " delta: " << delta[set][match] << " c_delta: " << c_delta[set][match] << " c_sig: " << c_sig[set] << std::endl;
            }

            break;
        }
    }

    // Case 2: Miss
    if (match == PT_WAY) {
        uint32_t victim_way = PT_WAY,
                 min_counter = C_SIG_MAX;

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

        if (SPP_DEBUG_PRINT) {
            std::cout << "[PT] " << __func__ << " miss sig: " << std::hex << last_sig << std::dec << " set: " << set << " way: " << victim_way;
            std::cout << " delta: " << delta[set][victim_way] << " c_delta: " << c_delta[set][victim_way] << " c_sig: " << c_sig[set] << std::endl;
        }

        if (SPP_SANITY_CHECK) {
            // Assertion
            if (victim_way == PT_WAY) {
                std::cout << "[PT] Cannot find a replacement victim!" << std::endl;
                assert(0);
            }
        }
    }
}

// =============================================================================
// PATTERN_TABLE::read_pattern — stock logic + CHERI feature threading
// =============================================================================

void spp_ppf_cheri::PPF_Module::PATTERN_TABLE::read_pattern(uint32_t curr_sig,
    std::vector<typename offset_type::difference_type>& delta_q, std::vector<uint32_t>& confidence_q,
    std::vector<int32_t>& perc_sum_q, uint32_t &lookahead_way, uint32_t &lookahead_conf, uint32_t &pf_q_tail,
    uint32_t &depth, champsim::address addr, champsim::address base_addr, champsim::address train_addr,
    champsim::address curr_ip, typename offset_type::difference_type train_delta, uint32_t last_sig,
    uint32_t pq_occupancy, uint32_t pq_SIZE, uint32_t mshr_occupancy, uint32_t mshr_SIZE,
    uint64_t cheri_size_class, uint64_t cheri_offset_cl,
    uint64_t demand_va, uint64_t cap_base, uint64_t cap_top)
{
    uint32_t set = spp_ppf_cheri::get_hash(curr_sig) % PT_SET,
             local_conf = 0,
             pf_conf = 0,
             max_conf = 0;

    bool found_candidate = false;

    if (c_sig[set]) {

        for (uint32_t way = 0; way < PT_WAY; way++) {

            local_conf = (100 * c_delta[set][way]) / c_sig[set];
            pf_conf = depth ? (parent_->GHR.global_accuracy * c_delta[set][way] / c_sig[set] * lookahead_conf / 100) : local_conf;

            // Compute remaining_cls for this candidate's delta direction
            typename offset_type::difference_type total_delta = train_delta + delta[set][way];
            int direction = (total_delta > 0) ? 1 : ((total_delta < 0) ? -1 : 0);
            champsim::block_number demand_va_block{champsim::address{demand_va}};
            uint64_t cheri_remaining_cls = static_cast<uint64_t>(
                cheri::remaining_lines(demand_va_block, direction,
                                       champsim::address{cap_base}, champsim::address{cap_top}));

            int32_t perc_sum = parent_->PERC.perc_predict(train_addr, curr_ip,
                parent_->GHR.ip_1, parent_->GHR.ip_2, parent_->GHR.ip_3,
                train_delta + delta[set][way], last_sig, curr_sig, pf_conf, depth,
                cheri_size_class, cheri_offset_cl, cheri_remaining_cls);

            bool do_pf = (perc_sum >= PERC_THRESHOLD_LO) ? 1 : 0;
            bool fill_l2 = (perc_sum >= PERC_THRESHOLD_HI) ? 1 : 0;

            if (fill_l2 && (mshr_occupancy >= mshr_SIZE || pq_occupancy >= pq_SIZE))
                continue;

            if (pf_conf && do_pf && pf_q_tail < 100) {

                confidence_q[pf_q_tail] = pf_conf;
                delta_q[pf_q_tail] = delta[set][way];
                perc_sum_q[pf_q_tail] = perc_sum;

                if (SPP_DEBUG_PRINT) {
                    std::cout << "[PT] State of Features: \nTrain addr: " << train_addr
                              << "\tCurr IP: " << curr_ip
                              << "\tIP_1: " << parent_->GHR.ip_1 << "\tIP_2: " << parent_->GHR.ip_2
                              << "\tIP_3: " << parent_->GHR.ip_3
                              << "\tDelta: " << train_delta + delta[set][way]
                              << "\tLastSig: " << last_sig << "\tCurrSig: " << curr_sig
                              << "\tConf: " << pf_conf << "\tDepth: " << depth
                              << "\tSUM: " << perc_sum << std::endl;
                }

                if (pf_conf > max_conf) {
                    lookahead_way = way;
                    max_conf = pf_conf;
                }
                pf_q_tail++;
                found_candidate = true;

                if (SPP_DEBUG_PRINT) {
                    std::cout << "[PT] " << __func__ << " HIGH CONF: " << pf_conf
                              << " sig: " << std::hex << curr_sig << std::dec
                              << " set: " << set << " way: " << way
                              << " delta: " << delta[set][way] << " c_delta: " << c_delta[set][way]
                              << " c_sig: " << c_sig[set] << std::endl;
                }
            } else {
                if (SPP_DEBUG_PRINT) {
                    std::cout << "[PT] " << __func__ << " LOW CONF: " << pf_conf
                              << " sig: " << std::hex << curr_sig << std::dec
                              << " set: " << set << " way: " << way
                              << " delta: " << delta[set][way] << " c_delta: " << c_delta[set][way]
                              << " c_sig: " << c_sig[set] << std::endl;
                }
            }

            // Recording Perc negatives
            if (pf_conf && pf_q_tail < parent_->cache_->get_mshr_size() && (perc_sum < PERC_THRESHOLD_HI)) {
                // Note: Using PERC_THRESHOLD_HI as the deciding factor for negative case
                // Because 'trueness' of a prefetch is decided based on the feedback from L2C
                // So even though LLC prefetches go through, they are treated as false wrt L2C in this case
                champsim::address pf_addr{champsim::block_number{champsim::address{base_addr}} + delta[set][way]};

                if (champsim::page_number{champsim::address{addr}} == champsim::page_number{pf_addr}) { // Prefetch request is in the same physical page
                    parent_->FILTER.check(pf_addr, train_addr, curr_ip, SPP_PERC_REJECT,
                                          train_delta + delta[set][way], last_sig, curr_sig, pf_conf, perc_sum, depth,
                                          cheri_size_class, cheri_offset_cl, cheri_remaining_cls);
                    parent_->GHR.perc_reject++;
                }
            }
        }
        lookahead_conf = max_conf;
        if (found_candidate) depth++;

        if (SPP_DEBUG_PRINT)
            std::cout << "global_accuracy: " << parent_->GHR.global_accuracy
                      << " lookahead_conf: " << lookahead_conf << std::endl;
    } else confidence_q[pf_q_tail] = 0;
}

// =============================================================================
// PREFETCH_FILTER::check — logs CHERI features alongside stock features
// =============================================================================

bool spp_ppf_cheri::PPF_Module::PREFETCH_FILTER::check(champsim::address check_addr, champsim::address base_addr,
    champsim::address ip, FILTER_REQUEST filter_request,
    typename offset_type::difference_type cur_delta, uint32_t last_sig, uint32_t curr_sig,
    uint32_t conf, int32_t sum, uint32_t depth,
    uint64_t cheri_sc, uint64_t cheri_oc, uint64_t cheri_rem)
{
    champsim::block_number cache_line{check_addr};
    uint64_t hash = spp_ppf_cheri::get_hash(cache_line.to<uint64_t>());

    // Main filter
    uint64_t quotient = (hash >> REMAINDER_BIT) & ((1 << QUOTIENT_BIT) - 1),
             remainder = hash % (1 << REMAINDER_BIT);

    // Reject filter
    uint64_t quotient_reject = (hash >> REMAINDER_BIT_REJ) & ((1 << QUOTIENT_BIT_REJ) - 1),
             remainder_reject = hash % (1 << REMAINDER_BIT_REJ);

    if (SPP_DEBUG_PRINT) {
        std::cout << "[FILTER] check_addr: " << std::hex << check_addr << " check_cache_line: " << champsim::block_number{check_addr};
        std::cout << " request type: " << filter_request;
        std::cout << " hash: " << hash << std::dec << " quotient: " << quotient << " remainder: " << remainder << std::endl;
    }

    switch (filter_request) {

        case SPP_PERC_REJECT:
            if ((valid[quotient] || useful[quotient]) && remainder_tag[quotient] == remainder) {
                if (SPP_DEBUG_PRINT) {
                    std::cout << "[FILTER] " << __func__ << " line is already in the filter check_addr: " << std::hex << check_addr << " cache_line: " << cache_line << std::dec;
                    std::cout << " quotient: " << quotient << " valid: " << valid[quotient] << " useful: " << useful[quotient] << std::endl;
                }
                return false;
            } else {
                if (train_neg) {
                    valid_reject[quotient_reject] = 1;
                    remainder_tag_reject[quotient_reject] = remainder_reject;

                    // Log stock perc features
                    address_reject[quotient_reject] = base_addr;
                    pc_reject[quotient_reject] = ip;
                    pc_1_reject[quotient_reject] = parent_->GHR.ip_1;
                    pc_2_reject[quotient_reject] = parent_->GHR.ip_2;
                    pc_3_reject[quotient_reject] = parent_->GHR.ip_3;
                    delta_reject[quotient_reject] = cur_delta;
                    perc_sum_reject[quotient_reject] = sum;
                    last_signature_reject[quotient_reject] = last_sig;
                    cur_signature_reject[quotient_reject] = curr_sig;
                    confidence_reject[quotient_reject] = conf;
                    la_depth_reject[quotient_reject] = depth;

                    // Log CHERI features
                    cheri_size_class_rej[quotient_reject] = cheri_sc;
                    cheri_offset_cl_rej[quotient_reject] = cheri_oc;
                    cheri_remaining_cls_rej[quotient_reject] = cheri_rem;
                }

                if (SPP_DEBUG_PRINT) {
                    std::cout << "[FILTER] " << __func__ << " PF rejected by perceptron. Set valid_reject for check_addr: " << std::hex << check_addr << " cache_line: " << cache_line << std::dec;
                    std::cout << " quotient: " << quotient << " remainder_tag: " << remainder_tag_reject[quotient_reject] << std::endl;
                }
            }
            break;

        case SPP_L2C_PREFETCH:
            if ((valid[quotient] || useful[quotient]) && remainder_tag[quotient] == remainder) {
                if (SPP_DEBUG_PRINT) {
                    std::cout << "[FILTER] " << __func__ << " line is already in the filter check_addr: " << std::hex << check_addr << " cache_line: " << cache_line << std::dec;
                    std::cout << " quotient: " << quotient << " valid: " << valid[quotient] << " useful: " << useful[quotient] << std::endl;
                }
                return false;
            } else {
                valid[quotient] = 1;
                useful[quotient] = 0;
                remainder_tag[quotient] = remainder;

                // Log stock perc features
                delta[quotient] = cur_delta;
                pc[quotient] = ip;
                pc_1[quotient] = parent_->GHR.ip_1;
                pc_2[quotient] = parent_->GHR.ip_2;
                pc_3[quotient] = parent_->GHR.ip_3;
                last_signature[quotient] = last_sig;
                cur_signature[quotient] = curr_sig;
                confidence[quotient] = conf;
                address[quotient] = base_addr;
                perc_sum[quotient] = sum;
                la_depth[quotient] = depth;

                // Log CHERI features
                cheri_size_class_log[quotient] = cheri_sc;
                cheri_offset_cl_log[quotient] = cheri_oc;
                cheri_remaining_cls_log[quotient] = cheri_rem;

                if (SPP_DEBUG_PRINT) {
                    std::cout << "[FILTER] " << __func__ << " set valid for check_addr: " << std::hex << check_addr << " cache_line: " << cache_line << std::dec;
                    std::cout << " quotient: " << quotient << " remainder_tag: " << remainder_tag[quotient] << " valid: " << valid[quotient] << " useful: " << useful[quotient] << std::endl;
                }
            }
            break;

        case SPP_LLC_PREFETCH:
            if ((valid[quotient] || useful[quotient]) && remainder_tag[quotient] == remainder) {
                if (SPP_DEBUG_PRINT) {
                    std::cout << "[FILTER] " << __func__ << " line is already in the filter check_addr: " << std::hex << check_addr << " cache_line: " << cache_line << std::dec;
                    std::cout << " quotient: " << quotient << " valid: " << valid[quotient] << " useful: " << useful[quotient] << std::endl;
                }
                return false;
            } else {
                // NOTE: SPP_LLC_PREFETCH has relatively low confidence
                // Therefore, it is safe to prefetch this cache line in the large LLC and save precious L2C capacity
                // If this prefetch request becomes more confident and SPP eventually issues SPP_L2C_PREFETCH,
                // we can get this cache line immediately from the LLC (not from DRAM)
                // To allow this fast prefetch from LLC, SPP does not set the valid bit for SPP_LLC_PREFETCH

                if (SPP_DEBUG_PRINT) {
                    std::cout << "[FILTER] " << __func__ << " don't set valid for check_addr: " << std::hex << check_addr << " cache_line: " << cache_line << std::dec;
                    std::cout << " quotient: " << quotient << " valid: " << valid[quotient] << " useful: " << useful[quotient] << std::endl;
                }
            }
            break;

        case L2C_DEMAND:
            if ((remainder_tag[quotient] == remainder) && (useful[quotient] == 0)) {
                useful[quotient] = 1;
                if (valid[quotient]) {
                    parent_->GHR.pf_useful++;
                    parent_->GHR.pf_l2c_good++;
                }

                if (SPP_DEBUG_PRINT) {
                    std::cout << "[FILTER] " << __func__ << " set useful for check_addr: " << std::hex << check_addr << " cache_line: " << cache_line << std::dec;
                    std::cout << " quotient: " << quotient << " valid: " << valid[quotient] << " useful: " << useful[quotient];
                    std::cout << " GHR.pf_issued: " << parent_->GHR.pf_issued << " GHR.pf_useful: " << parent_->GHR.pf_useful << std::endl;
                    if (valid[quotient])
                        std::cout << " Calling Perceptron Update (INC) as L2C_DEMAND was useful" << std::endl;
                }

                if (valid[quotient]) {
                    // Useful prefetch → increment weights (replay stored features including CHERI)
                    parent_->PERC.perc_update(address[quotient], pc[quotient],
                        pc_1[quotient], pc_2[quotient], pc_3[quotient],
                        delta[quotient], last_signature[quotient], cur_signature[quotient],
                        confidence[quotient], la_depth[quotient], 1, perc_sum[quotient],
                        cheri_size_class_log[quotient], cheri_offset_cl_log[quotient], cheri_remaining_cls_log[quotient]);

                    int32_t perc_sum_shifted = perc_sum[quotient] + (PERC_COUNTER_MAX + 1) * PERC_FEATURES;
                    int32_t hist_index = perc_sum_shifted / 10;
                    hist_hits[hist_index]++;
                }
            }
            // If NOT prefetched, check if it was rejected by perceptron
            if (!(valid[quotient] && remainder_tag[quotient] == remainder)) {
                if (valid_reject[quotient_reject] && remainder_tag_reject[quotient_reject] == remainder_reject) {
                    if (SPP_DEBUG_PRINT) {
                        std::cout << "[FILTER] " << __func__ << " demand hit on rejected line check_addr: " << std::hex << check_addr << std::dec;
                        std::cout << " Calling Perceptron Update (DEC) — missed opportunity" << std::endl;
                    }
                    if (train_neg) {
                        // Missed opportunity → decrement weights that blocked it (replay stored CHERI features)
                        parent_->PERC.perc_update(address_reject[quotient_reject], pc_reject[quotient_reject],
                            pc_1_reject[quotient_reject], pc_2_reject[quotient_reject], pc_3_reject[quotient_reject],
                            delta_reject[quotient_reject], last_signature_reject[quotient_reject],
                            cur_signature_reject[quotient_reject], confidence_reject[quotient_reject],
                            la_depth_reject[quotient_reject], 0, perc_sum_reject[quotient_reject],
                            cheri_size_class_rej[quotient_reject], cheri_offset_cl_rej[quotient_reject],
                            cheri_remaining_cls_rej[quotient_reject]);
                        valid_reject[quotient_reject] = 0;
                        remainder_tag_reject[quotient_reject] = 0;
                        parent_->GHR.reject_update++;
                    }
                }
            }
            break;

        case L2C_EVICT:
            if (valid[quotient] && !useful[quotient]) {
                if (parent_->GHR.pf_useful)
                    parent_->GHR.pf_useful--;

                if (SPP_DEBUG_PRINT) {
                    std::cout << "[FILTER] " << __func__ << " eviction for check_addr: " << std::hex << check_addr << " cache_line: " << cache_line << std::dec;
                    std::cout << " quotient: " << quotient << " valid: " << valid[quotient] << " useful: " << useful[quotient] << std::endl;
                    std::cout << " Calling Perceptron Update (DEC) as L2C_DEMAND was not useful" << std::endl;
                }

                // Useless prefetch evicted → decrement weights (replay stored CHERI features)
                parent_->PERC.perc_update(address[quotient], pc[quotient],
                    pc_1[quotient], pc_2[quotient], pc_3[quotient],
                    delta[quotient], last_signature[quotient], cur_signature[quotient],
                    confidence[quotient], la_depth[quotient], 0, perc_sum[quotient],
                    cheri_size_class_log[quotient], cheri_offset_cl_log[quotient], cheri_remaining_cls_log[quotient]);
            }

            // Reset filter entry
            valid[quotient] = 0;
            useful[quotient] = 0;
            remainder_tag[quotient] = 0;

            // Reset reject filter too
            valid_reject[quotient_reject] = 0;
            remainder_tag_reject[quotient_reject] = 0;

            break;

        default:
            std::cout << "[FILTER] Invalid filter request type: " << filter_request << std::endl;
            assert(0);
    }

    return true;
}

// =============================================================================
// GLOBAL_REGISTER — completely stock
// =============================================================================

void spp_ppf_cheri::PPF_Module::GLOBAL_REGISTER::update_entry(uint32_t pf_sig, uint32_t pf_confidence,
    offset_type pf_offset, typename offset_type::difference_type pf_delta)
{
    bool min_conf_set = false; 
    uint32_t min_conf = 100,
             victim_way = 0;

    if (SPP_DEBUG_PRINT) {
        std::cout << "[GHR] Crossing the page boundary pf_sig: " << std::hex << pf_sig << std::dec;
        std::cout << " confidence: " << pf_confidence << " pf_offset: " << pf_offset << " pf_delta: " << pf_delta << std::endl;
    }

    for (uint32_t i = 0; i < MAX_GHR_ENTRY; i++) {
        if (valid[i] && (offset[i] == pf_offset)) {
            sig[i] = pf_sig;
            confidence[i] = pf_confidence;
            delta[i] = pf_delta;

            if (SPP_DEBUG_PRINT)
                std::cout << "[GHR] Found a matching index: " << i << std::endl;
            return;
        }

        if (confidence[i] < min_conf) {
            min_conf = confidence[i];
            victim_way = i;
        }
    }

    if (victim_way >= MAX_GHR_ENTRY) {
        std::cout << "[GHR] Cannot find a replacement victim!" << std::endl;
        assert(0);
    }

    if (SPP_DEBUG_PRINT) {
        std::cout << "[GHR] Replace index: " << victim_way << " pf_sig: " << std::hex << sig[victim_way] << std::dec;
        std::cout << " confidence: " << confidence[victim_way] << " pf_offset: " << offset[victim_way] << " pf_delta: " << delta[victim_way] << std::endl;
    }

    valid[victim_way] = 1;
    sig[victim_way] = pf_sig;
    confidence[victim_way] = pf_confidence;
    offset[victim_way] = pf_offset;
    delta[victim_way] = pf_delta;
}

uint32_t spp_ppf_cheri::PPF_Module::GLOBAL_REGISTER::check_entry(offset_type page_offset)
{
    uint32_t max_conf = 0,
             max_conf_way = MAX_GHR_ENTRY;

    for (uint32_t i = 0; i < MAX_GHR_ENTRY; i++) {
        if ((offset[i] == page_offset) && (max_conf < confidence[i])) {
            max_conf = confidence[i];
            max_conf_way = i;
        }
    }

    return max_conf_way;
}

// =============================================================================
// get_perc_index — 12 features (9 stock + 3 CHERI)
// =============================================================================

void spp_ppf_cheri::PPF_Module::get_perc_index(champsim::address base_addr, champsim::address ip,
    champsim::address ip_1, champsim::address ip_2, champsim::address ip_3,
    typename offset_type::difference_type cur_delta,
    uint32_t last_sig, uint32_t curr_sig, uint32_t confidence, uint32_t depth,
    uint64_t cheri_sc, uint64_t cheri_oc, uint64_t cheri_rem,
    uint64_t* perc_set)
{
    champsim::block_number cache_line{base_addr};
    champsim::page_number page_addr{base_addr};

    int sig_delta = (cur_delta < 0) ? (((-1) * cur_delta) + (1 << (SIG_DELTA_BIT - 1))) : cur_delta;
    uint64_t pre_hash[PERC_FEATURES];

    // Stock features [0..8]
    pre_hash[0] = base_addr.to<uint64_t>();
    pre_hash[1] = cache_line.to<uint64_t>();
    pre_hash[2] = page_addr.to<uint64_t>();
    pre_hash[3] = confidence ^ page_addr.to<uint64_t>();
    pre_hash[4] = curr_sig ^ sig_delta;
    pre_hash[5] = ip_1.to<uint64_t>() ^ (ip_2.to<uint64_t>() >> 1) ^ (ip_3.to<uint64_t>() >> 2);
    pre_hash[6] = ip.to<uint64_t>() ^ depth;
    pre_hash[7] = ip.to<uint64_t>() ^ sig_delta;
    pre_hash[8] = confidence;

    // CHERI features [9..11]
    pre_hash[9]  = cheri_sc;                  // object_size_class
    pre_hash[10] = cheri_oc;                  // cap_offset_cl
    pre_hash[11] = cheri_rem;                 // remaining_cls_in_dir

    for (int i = 0; i < PERC_FEATURES; i++) {
        perc_set[i] = (pre_hash[i]) % PERC.PERC_DEPTH[i];
        if (SPP_DEBUG_PRINT) std::cout << "  Perceptron Set Index#: " << i << " = " << perc_set[i];
    }
    if (SPP_DEBUG_PRINT) std::cout << std::endl;
}

// =============================================================================
// PERCEPTRON::perc_predict — stock logic over 12-feature vector
// =============================================================================

int32_t spp_ppf_cheri::PPF_Module::PERCEPTRON::perc_predict(champsim::address base_addr, champsim::address ip,
    champsim::address ip_1, champsim::address ip_2, champsim::address ip_3,
    typename offset_type::difference_type cur_delta,
    uint32_t last_sig, uint32_t curr_sig, uint32_t confidence, uint32_t depth,
    uint64_t cheri_sc, uint64_t cheri_oc, uint64_t cheri_rem)
{
    if (SPP_DEBUG_PRINT) {
        int sig_delta = (cur_delta < 0) ? (((-1) * cur_delta) + (1 << (SIG_DELTA_BIT - 1))) : cur_delta;
        std::cout << "[PERC_PRED] Current IP: " << ip << "  and  Memory Adress: " << std::hex << base_addr << std::endl;
        std::cout << " Last Sig: " << last_sig << " Curr Sig: " << curr_sig << std::dec << std::endl;
        std::cout << " Cur Delta: " << cur_delta << " Sign Delta: " << sig_delta << " Confidence: " << confidence << std::endl;
        std::cout << " CHERI sc: " << cheri_sc << " oc: " << cheri_oc << " rem: " << cheri_rem << std::endl;
        std::cout << " ";
    }

    uint64_t perc_set[PERC_FEATURES];
    parent_->get_perc_index(base_addr, ip, ip_1, ip_2, ip_3, cur_delta, last_sig, curr_sig, confidence, depth,
                            cheri_sc, cheri_oc, cheri_rem, perc_set);

    int32_t sum = 0;
    for (int i = 0; i < PERC_FEATURES; i++) {
        sum += perc_weights[perc_set[i]][i];
    }

    if (SPP_DEBUG_PRINT)
        std::cout << " Sum of perceptrons: " << sum << " Prediction made: "
                  << ((sum >= PERC_THRESHOLD_LO) ? (sum >= PERC_THRESHOLD_HI) : false) << std::endl;
    return sum;
}

// =============================================================================
// PERCEPTRON::perc_update — stock logic over 12-feature vector
// =============================================================================

void spp_ppf_cheri::PPF_Module::PERCEPTRON::perc_update(champsim::address base_addr, champsim::address ip,
    champsim::address ip_1, champsim::address ip_2, champsim::address ip_3,
    typename offset_type::difference_type cur_delta,
    uint32_t last_sig, uint32_t curr_sig, uint32_t confidence, uint32_t depth,
    bool direction, int32_t perc_sum,
    uint64_t cheri_sc, uint64_t cheri_oc, uint64_t cheri_rem)
{
    if (SPP_DEBUG_PRINT) {
        int sig_delta = (cur_delta < 0) ? (((-1) * cur_delta) + (1 << (SIG_DELTA_BIT - 1))) : cur_delta;
        std::cout << "[PERC_UPD] (Recorded) IP: " << ip << "  and  Memory Adress: " << std::hex << base_addr << std::endl;
        std::cout << " Last Sig: " << last_sig << " Curr Sig: " << curr_sig << std::dec << std::endl;
        std::cout << " Cur Delta: " << cur_delta << " Sign Delta: " << sig_delta << " Confidence: " << confidence << " Update Direction: " << direction << std::endl;
        std::cout << " CHERI sc: " << cheri_sc << " oc: " << cheri_oc << " rem: " << cheri_rem << std::endl;
        std::cout << " ";
    }

    uint64_t perc_set[PERC_FEATURES];
    parent_->get_perc_index(base_addr, ip, ip_1, ip_2, ip_3, cur_delta, last_sig, curr_sig, confidence, depth,
                            cheri_sc, cheri_oc, cheri_rem, perc_set);

    int32_t sum = 0;
    for (int i = 0; i < PERC_FEATURES; i++) {
        perc_touched[perc_set[i]][i] = 1;
    }

    sum = perc_sum;

    if (!direction) {
        // Prediction wrong
        for (int i = 0; i < PERC_FEATURES; i++) {
            if (sum >= PERC_THRESHOLD_HI) {
                if (perc_weights[perc_set[i]][i] > -1 * (PERC_COUNTER_MAX + 1))
                    perc_weights[perc_set[i]][i]--;
            }
            if (sum < PERC_THRESHOLD_HI) {
                if (perc_weights[perc_set[i]][i] < PERC_COUNTER_MAX)
                    perc_weights[perc_set[i]][i]++;
            }
        }
        if (SPP_DEBUG_PRINT) {
            int differential = (sum >= PERC_THRESHOLD_HI) ? -1 : 1;
            std::cout << " Direction is: " << direction << " and target is: " << PERC_THRESHOLD_HI
                      << " and diff is: " << differential << std::endl;
        }
    }
    if (direction && sum > NEG_UPDT_THRESHOLD && sum < POS_UPDT_THRESHOLD) {
        // Prediction correct with moderate confidence: reinforce only when sum is between
        // NEG_UPDT_THRESHOLD and POS_UPDT_THRESHOLD (i.e., not strongly saturated).
        for (int i = 0; i < PERC_FEATURES; i++) {
            if (sum >= PERC_THRESHOLD_HI) {
                if (perc_weights[perc_set[i]][i] < PERC_COUNTER_MAX)
                    perc_weights[perc_set[i]][i]++;
            }
            if (sum < PERC_THRESHOLD_HI) {
                if (perc_weights[perc_set[i]][i] > -1 * (PERC_COUNTER_MAX + 1))
                    perc_weights[perc_set[i]][i]--;
            }
        }
        if (SPP_DEBUG_PRINT) {
            int differential = (sum >= PERC_THRESHOLD_HI) ? 1 : -1;
            std::cout << " Direction is: " << direction << " and target is: " << PERC_THRESHOLD_HI
                      << " and diff is: " << differential << std::endl;
        }
    }
}

// =============================================================================
// Final stats
// =============================================================================

void spp_ppf_cheri::PPF_Module::final_stats()
{
    if (SPP_DEBUG_PRINT) {
        // fmt::print("\nAvg Lookahead Depth: {}\t\n",GHR.depth_sum / GHR.depth_num);
        // fmt::print("TOTAL: {}\tL2C: {}\tLLC: {} GOOD_L2C: {}\n",GHR.pf_total, GHR.pf_l2c, GHR.pf_llc, GHR.pf_l2c_good);
        // fmt::print("PERC PASS: {}\tPERC REJECT: {}\tREJECT_UPDATE: {}\n",GHR.perc_pass,GHR.perc_reject,GHR.reject_update);
    }

    if (SPP_PERC_WGHT) {
        ofstream myfile;
        char fname[] = "perc_weights_cheri_0.csv";
        myfile.open(fname, std::ofstream::app);
        fmt::print("Printing all the perceptron weights to: {}\n", fname);

        std::string row = "base_addr,cache_line,page_addr,confidence^page_addr,curr_sig^sig_delta,"
                          "ip_1^ip_2^ip_3,ip^depth,ip^sig_delta,confidence,"
                          "object_size_class,cap_offset_cl,remaining_cls_in_dir,\n";
        for (int i = 0; i < PERC_ENTRIES; i++) {
            for (int j = 0; j < PERC_FEATURES; j++) {
                if (PERC.perc_touched[i][j]) {
                    row = row + std::to_string(PERC.perc_weights[i][j]) + ",";
                } else {
                    row = row + ",";
                }
            }
            row = row + "\n";
        }
        myfile << row;
        myfile.close();
    }

    // CHERI stats
    fmt::print("\n=== SPP+PPF CHERI Stats ===\n");
    fmt::print("  Prefetches bounded by cap: {}\n", stat_pf_bounded_by_cap);

    int tot = 0;
    fmt::print("------------------\n");
    fmt::print("{} Depth Distribution\n", cache_->NAME);
    fmt::print("------------------\n");

    for (int a = 0; a < 30; a++) {
        fmt::print("depth {}: {}\n", a, depth_track[a]);
        tot += depth_track[a];
    }
    fmt::print("Total: {}\n", tot);
    fmt::print("------------------\n");
}

void spp_ppf_cheri::prefetcher_final_stats()
{
    module_.final_stats();
}
