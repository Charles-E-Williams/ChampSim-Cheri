/*
 * Berti-TDP: Berti with tag-directed prefetching (TDP). Design, results and how to run it: DESIGN.md in this directory.
 *
 * Berti's code (Berti's structures below, and the Berti statements in the hooks) is prefetcher/berti/berti.{h,cc}
 * unchanged; with TDP_ENABLE 0 (berti_tdp_params.h) this module is bit-identical to Berti. TDP: CHERI tags
 * mark the pointer slots of every line arriving in L1D; each pointer is recorded in the Capability History Table (CHT)
 * and looked up in the Pointer Delta Table (PDT), which holds per IP Berti-style epoch confidences of "dereferenced
 * line - pointer's cursor line"; confident deltas are prefetched. A tagged demand miss whose authorizing capability is
 * a recorded pointer trains its delta.
 */

#ifndef BERTI_TDP_H_
#define BERTI_TDP_H_

/*
 * Berti: an Accurate Local-Delta Data Prefetcher
 *  
 * 55th ACM/IEEE International Conference on Microarchitecture (MICRO 2022),
 * October 1-5, 2022, Chicago, Illinois, USA.
 * 
 * @Authors: Agustín Navarro-Torres, Biswabandan Panda, J. Alastruey-Benedé, 
 *           Pablo Ibáñez, Víctor Viñals-Yúfera, and Alberto Ros
 * @Manteiners: Agustín Navarro -Torres
 * @Email: agusnt@unizar.es
 * @Date: 22/11/2022
 * 
 * This code is an update from the original code to work with the new version
 * of ChampSim: https://github.com/agusnt/Berti-Artifact
 * 
 * Maybe fine-tuning is required to get the optimal performance/accuracy.
 * 
 * Please note that this version of ChampSim has noticeable differences with 
 * the used for the paper, so results can varies.
 * 
 * Cite this:
 * 
 * A. Navarro-Torres, B. Panda, J. Alastruey-Benedé, P. Ibáñez, 
 * V. Viñals-Yúfera and A. Ros, 
 * "Berti: an Accurate Local-Delta Data Prefetcher," 
 * 2022 55th IEEE/ACM International Symposium on Microarchitecture (MICRO), 
 * 2022, pp. 975-991, doi: 10.1109/MICRO56248.2022.00072.
 * 
 * @INPROCEEDINGS{9923806,  author={Navarro-Torres, Agustín and Panda, 
 * Biswabandan and Alastruey-Benedé, Jesús and Ibáñez, Pablo and Viñals-Yúfera,
 * Víctor and Ros, Alberto},  booktitle={2022 55th IEEE/ACM International 
 * Symposium on Microarchitecture (MICRO)},   title={Berti: an Accurate 
 * Local-Delta Data Prefetcher},   year={2022},  volume={},  number={},  
 * pages={975-991},  doi={10.1109/MICRO56248.2022.00072}}
 */

#include "berti_tdp_params.h"
#include "cache.h"
#include "capability_memory.h"
#include "cheri_prefetch_utils.h"

#include <algorithm>
#include <array>
#include <deque>
#include <iostream>
#include <stdlib.h>
#include <cassert>
#include <vector>
#include <time.h>
#include <cstdio>
#include <tuple>
#include <queue>
#include <cmath>
#include <map>
#include <unordered_map>

class berti_tdp : public champsim::modules::prefetcher {

    public:
    /*****************************************************************************
     *                              Stats                                        *
     *****************************************************************************/
    // Get average latency: Welford's method
    typedef struct welford
    {
        uint64_t num = 0; 
        float average = 0.0;
    } welford_t;
    
    welford_t average_latency;
    
    // Get more info
    uint64_t pf_to_l1 = 0;
    uint64_t pf_to_l2 = 0;
    uint64_t pf_to_l2_bc_mshr = 0;
    uint64_t cant_track_latency = 0;
    uint64_t cross_page = 0;
    uint64_t no_cross_page = 0;
    uint64_t no_found_berti = 0;
    uint64_t found_berti = 0;
    uint64_t average_issued = 0;
    uint64_t average_num = 0;

    
    /*****************************************************************************
     *                      General Structs                                      *
     *****************************************************************************/
    
    typedef struct Delta {
        uint64_t conf;
        int64_t  delta;
        uint8_t  rpl;
        Delta(): conf(0), delta(0), rpl(BERTI_R) {};
    } delta_t; 
    
    /*****************************************************************************
     *                      Berti structures                                     *
     *****************************************************************************/
    class LatencyTable
    {
        /* Latency table simulate the modified PQ and MSHR */
        private:
        struct latency_table {
            uint64_t addr = 0; // Addr
            uint64_t tag  = 0; // IP-Tag 
            uint64_t time = 0; // Event cycle
            bool     pf   = false;   // Is the entry accessed by a demand miss
        };
        int size;
        
        latency_table *latencyt;
    
        public:
        LatencyTable(const int size) : size(size)
        {
            latencyt = new latency_table[size];
        }
        ~LatencyTable() { delete []latencyt;}
    
        uint8_t  add(uint64_t addr, uint64_t tag, bool pf, uint64_t cycle);
        uint64_t get(uint64_t addr);
        uint64_t del(uint64_t addr);
        uint64_t get_tag(uint64_t addr);
    };
    
    class ShadowCache
    {
        /* Shadow cache: mirrors L1D to track prefetch status and latency. */
        private:
        struct shadow_cache {
            uint64_t addr = 0;
            uint64_t lat  = 0;
            bool     pf   = false;
        };
    
        int sets;
        int ways;
        shadow_cache **scache;
    
        public:
        uint64_t aliased_cache_hits = 0;
        ShadowCache(const int sets, const int ways)
        {
            this->sets = sets;
            this->ways = ways;
            scache = new shadow_cache*[sets];
            for (int i = 0; i < sets; i++)
                scache[i] = new shadow_cache[ways];
        }
        ~ShadowCache()
        {
            for (int i = 0; i < sets; i++)
                delete[] scache[i];
            delete[] scache;
        }
    
        bool add(long set, long way, uint64_t addr, bool pf, uint64_t lat);
        bool is_pf(uint64_t addr);
        void set_pf(uint64_t addr, bool pf);
        bool get(uint64_t addr);
        uint64_t get_latency(uint64_t addr);
    };
    
    class HistoryTable
    {
        /* History Table */
        private:
        struct history_table {
            uint64_t tag  = 0; // IP Tag
            uint64_t addr = 0; // IP @ accessed
            uint64_t time = 0; // Time where the line is accessed
        }; // This struct is the history table
    
        const int sets = HISTORY_TABLE_SETS;
        const int ways = HISTORY_TABLE_WAYS;
    
        history_table **historyt;
        history_table **history_pointers;
    
        uint16_t get_aux(uint32_t latency, uint64_t tag, uint64_t act_addr,
            uint64_t *tags, uint64_t *addr, uint64_t cycle);
        public:
        HistoryTable()
        {
            history_pointers = new history_table*[sets];
            historyt = new history_table*[sets];
    
            for (int i = 0; i < sets; i++) historyt[i] = new history_table[ways];
            for (int i = 0; i < sets; i++) history_pointers[i] = historyt[i];
        }
    
        ~HistoryTable()
        {
            for (int i = 0; i < sets; i++) delete []historyt[i];
            delete []historyt;
    
            delete []history_pointers;
        }
    
        int get_ways();
        void add(uint64_t tag, uint64_t addr, uint64_t cycle);
        uint16_t get(uint32_t latency, uint64_t tag, uint64_t act_addr, 
            uint64_t *tags, uint64_t *addr, uint64_t cycle);
    };
    
    /* Berti Table */
    private:
    struct berti_table {
        std::array<delta_t, BERTI_TABLE_DELTA_SIZE> deltas;
        uint64_t conf = 0;
        uint64_t total_used = 0;
    };
    
    std::map<uint64_t, berti_table*> bertit;
    std::queue<uint64_t> bertit_queue;
        
    uint64_t size = 0;

    bool static compare_greater_delta(delta_t a, delta_t b);
    bool static compare_rpl(delta_t a, delta_t b);

    void increase_conf_tag(uint64_t tag);
    void conf_tag(uint64_t tag);
    void add(uint64_t tag, int64_t delta);

    public:
    //Berti(uint64_t p_size) : size(p_size) {};
    void find_and_update(uint64_t latency, uint64_t tag, uint64_t cycle, 
        uint64_t line_addr);
    uint8_t get(uint64_t tag, std::vector<delta_t> &res);
    uint64_t ip_hash(uint64_t ip);
    
    uint64_t me = 0;
    static uint64_t others;
    // This is structure is an adaption of Berti for multicore simulations
    static std::vector<LatencyTable*> latencyt;
    static std::vector<ShadowCache*> scache;
    static std::vector<HistoryTable*> historyt;

    using prefetcher::prefetcher;
    uint32_t prefetcher_cache_operate(champsim::address addr, champsim::address ip, uint32_t cpu, champsim::capability cap, uint8_t cache_hit,
                                      bool useful_prefetch, access_type type, uint32_t metadata_in, uint32_t metadata_hit);
    uint32_t prefetcher_cache_fill(champsim::address addr, champsim::address ip, uint32_t cpu, champsim::capability cap, bool useless, long set, long way,
                                   bool prefetch, champsim::address evicted_addr, champsim::capability evicted_cap, uint32_t metadata_in,
                                   uint32_t metadata_evict, uint32_t cpu_evict);

    void prefetcher_initialize();
    void prefetcher_cycle_operate();
    void prefetcher_final_stats();

    /*****************************************************************************
     *                    Tag-directed prefetching (TDP) (Berti-TDP)                              *
     *****************************************************************************/
    // Metadata on this prefetcher's prefetches (carried by the request to the fill, kept per L1D block):
    //   [15:0] trigger IP hash (Berti's 16-bit IP tag), [16] TDP prefetch, [17] IP field valid.
    static constexpr uint32_t MD_IP_MASK = 0xFFFF;
    static constexpr uint32_t MD_TDP = 1u << 16;
    static constexpr uint32_t MD_VALID = 1u << 17;
    static uint32_t md_make(uint64_t ip16, bool is_tdp) { return static_cast<uint32_t>(ip16 & MD_IP_MASK) | (is_tdp ? MD_TDP : 0u) | MD_VALID; }
    // Recursion (TDP_RECUR != 0 only): [20:18] depth of the TDP prefetch (1 = first hop), [25:21] probability that its
    // line is used, in 1/16 (0-16), [26] control prefetch (TDP_RECUR_CTRL_NL; never scanned)
    static constexpr uint32_t MD_CTRL = 1u << 26;
    static uint32_t md_depth(uint32_t metadata) { return (metadata >> 18) & 0x7; }
    static uint32_t md_prob(uint32_t metadata) { return (metadata >> 21) & 0x1F; }
    static uint32_t md_make_chain(uint64_t key, unsigned depth, unsigned prob)
    {
      return md_make(key, true) | (std::min(depth, 7u) << 18) | (std::min(prob, 16u) << 21);
    }

    enum { SRC_DEMAND_FILL, SRC_PF_FILL, SRC_LOAD_HIT, SRC_TDP_FILL, SRC_TDP_HIT, NUM_SRC };

    // Capability History Table: recent pointer sightings
    struct cht_entry {
        bool     valid = false;
        uint16_t base_hash = 0;   // hash of the pointer's base: names the pointed-to object
        uint64_t cursor_line = 0; // the pointer's cursor line
        uint16_t key = 0;         // PDT key
        uint16_t trained = 0;     // deltas already credited for this sighting, one bit per delta
        uint64_t time = 0;        // cycle of the sighting (statistics; TDP_TIMELY)
        uint8_t  source = 0;      // statistics only
    };
    std::array<cht_entry, TDP_CHT_SIZE> cht{};
    std::size_t cht_ptr = 0;
    std::array<cht_entry, (TDP_RECUR_CHT > 0) ? TDP_RECUR_CHT : 1> cht2{}; // TDP_RECUR_CHT: recursion's own sightings
    std::size_t cht2_ptr = 0;

    // Pointer Delta Table: per key, epoch confidence of (dereferenced line - cursor line)
    struct pdt_delta {
        bool    valid = false;
        int8_t  delta = 0;
        uint8_t conf = 0;
        uint8_t status = 0; // 0 none, 1 prefetch to L2, 2 prefetch to L1D
        uint8_t last = 0;   // TDP_RECUR: dereferences in the previous epoch (the confidence status was set from)
    };
    struct pdt_entry {
        bool     valid = false;
        uint16_t tag = 0;
        uint8_t  sightings = 0;
        uint64_t lru = 0;
        std::array<pdt_delta, TDP_PDT_DELTAS> d{};
    };
    std::array<pdt_entry, TDP_PDT_SIZE> pdt{};
    uint64_t pdt_clock = 0;

    // Sighting queue: pointers waiting for their CHT search and PDT lookup
    struct sighting {
        champsim::capability cap;
        uint64_t ip16;
        uint64_t line_va; // the line the pointer was read from
        uint8_t  source;
        uint8_t  depth = 0;  // TDP_RECUR: 0 = an IP-keyed sighting; d >= 1 = found in a TDP fill of depth d (ip16 holds its key)
        uint8_t  prob = 16;  // TDP_RECUR: probability (1/16) that the line holding it is used
        bool     chained = false; // TDP_RECUR: produced by recursion (TDP_RECUR_CHT places it)
        uint16_t fallback = 0;   // TDP_RECUR_KEY 4: the parent sighting's key, used while this sighting's key is untrained
    };
    std::deque<sighting> sq;
    uint64_t budget_cycle = ~uint64_t{0};
    unsigned budget_used = 0;

    // TDP_TIMELY: dereference misses waiting until their line fills and the fetch latency is known
    struct pending_deref {
        uint64_t line;
        uint16_t key;
        int8_t   delta;
        uint64_t gap;
    };
    std::deque<pending_deref> pending;

    // Statistics (not hardware)
    uint64_t st_sight[NUM_SRC] = {}, st_lines_scanned[NUM_SRC] = {};
    uint64_t st_sight_dup = 0, st_sq_dropped = 0, st_sq_delayed = 0;
    uint64_t st_tagged_load_miss = 0, st_match = 0, st_match_timely = 0;
    uint64_t st_train_miss = 0, st_train_tdphit = 0, st_epochs = 0, st_status_l1 = 0, st_status_l2 = 0;
    uint64_t st_tdp_l1 = 0, st_tdp_l2 = 0, st_tdp_pq_full = 0, st_tdp_inflight = 0, st_tdp_oob = 0;
    uint64_t st_tdp_fills = 0, st_tdp_useful = 0, st_tdp_useless = 0;
    uint64_t st_timely_credit = 0, st_late_dropped = 0, st_nolat_dropped = 0, st_pending_overflow = 0;
    uint64_t st_gap[NUM_SRC][16] = {};
    uint64_t st_delta_hist[2 * TDP_MAX_LINE_DELTA + 1] = {};
    static constexpr int NUM_SRC_PRINT = TDP_RECUR ? NUM_SRC : SRC_TDP_FILL; // the design prints its 3 sources only
    // Recursion statistics (TDP_RECUR)
    uint64_t st_rc_issued[8][2] = {}, st_rc_useful[8] = {}, st_rc_useless[8] = {}, st_rc_fills[8] = {}; // by depth; [.][0] L2, [.][1] L1D
    uint64_t st_rc_scanned_fill = 0, st_rc_scanned_hit = 0, st_rc_depth_stop = 0, st_rc_low_prob = 0, st_rc_ctrl = 0, st_rc_fallback = 0;
    bool rc_in_warmup = true;
    // TDP_RECUR_CREDIT / TDP_RECUR_FEEDBACK (hardware state)
    uint32_t rc_outstanding = 0;             // lines fetched by recursion, in L1D, not yet used
    unsigned rc_limit = (TDP_RECUR_DEPTH < 4) ? TDP_RECUR_DEPTH : 4; // learned depth limit
    uint8_t  fb_used[8] = {}, fb_unused[8] = {};
    uint32_t fb_events = 0;
    uint64_t st_rc_credit_stop = 0, st_rc_fb_up = 0, st_rc_fb_down = 0, st_rc_limit_epochs[8] = {};
    static bool fb_sampled(uint64_t line) { return (line & 7) == 0; } // 8 of the 64 L1D sets
    void rc_line_used(uint32_t line_metadata, uint64_t line);
    void rc_line_unused(uint32_t line_metadata, uint64_t line);
    void fb_tick();

#if TDP_CHAIN_STATS
    // Chain measurement (not hardware): the pointers in every L1D fill, by the full base of their target.
    struct ch_ptr { uint64_t cursor_line = 0, line = 0, fill_cycle = 0; uint8_t kind = 0; bool valid = false; };
    enum { CH_DEMAND, CH_MERGED, CH_BERTI, CH_TDP, CH_KINDS };
    struct ch_line { uint8_t depth = 1; uint64_t first_use = 0; bool pending = false; };
    std::unordered_map<uint64_t, std::array<ch_ptr, 4>> ch_ptrs;
    std::unordered_map<uint64_t, ch_line> ch_lines;
    uint64_t ch_next_prune = 0;
    uint64_t ch_miss_depth[9] = {}, ch_cov_depth[9] = {};        // would-miss demands by chain depth: missed / hit a prefetched line
    uint64_t ch_miss_parent[CH_KINDS] = {}, ch_cov_parent[CH_KINDS] = {};
    uint64_t ch_lead_fill[CH_KINDS][16] = {};                    // misses: cycles from the parent line's fill (log2)
    uint64_t ch_stop_used = 0, ch_stop_unused = 0;               // misses whose pointer sat in an unscanned TDP fill: used before / not
    uint64_t ch_stop_lead_fill[16] = {}, ch_stop_lead_use[16] = {}; // ... cycles from that line's fill / its first demand use
    void ch_fill(uint64_t line, uint8_t kind, uint32_t cpu);
    void ch_demand(uint64_t line, const champsim::capability& cap, bool miss);
#endif

    static uint16_t hash16(uint64_t x);
    static uint16_t make_key(uint64_t ip16, const champsim::capability& c);
    pdt_entry* pdt_find(uint16_t tag);
    pdt_entry* pdt_alloc(uint16_t tag);
    void pdt_count_sighting(pdt_entry* e);
    void pdt_train(uint16_t key, int delta);
    bool issue_tdp(uint64_t line, bool fill_l1, uint64_t ip16, const champsim::capability& c, uint32_t chain_metadata);
    void process_sighting(const sighting& sight);
    bool take_budget();
    void drain_sightings();
    void scan_line(uint64_t line_va, uint64_t ip16, uint8_t source, uint32_t cpu, unsigned slot_mask = (1u << cheri::CAPS_PER_CL) - 1);
    void scan_tdp_fill(uint64_t line_va, uint32_t fill_metadata, uint32_t cpu);
    void enqueue_sighting(const sighting& sight);
    bool pf_line(champsim::address addr, bool fill_l1, uint32_t metadata);
    bool pf_line(champsim::address addr, bool fill_l1, uint32_t metadata, const champsim::capability& cap);
    void train_deref(uint64_t line, const champsim::capability& cap, bool is_miss);
};
#endif