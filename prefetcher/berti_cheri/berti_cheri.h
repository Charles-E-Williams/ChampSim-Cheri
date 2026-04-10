#ifndef VBERTI_CHERI_H_
#define VBERTI_CHERI_H_

/*
 * Berti-CHERI: CHERI-capability-aware Berti local-delta prefetcher.
 *
 * Based on:
 *   Berti: an Accurate Local-Delta Data Prefetcher
 *   55th ACM/IEEE International Symposium on Microarchitecture (MICRO 2022)
 *   A. Navarro-Torres, B. Panda, J. Alastruey-Benedé, P. Ibáñez,
 *   V. Viñals-Yúfera, and A. Ros
 *
 * CHERI adaptations:
 *  - History table stores capability-relative cache-line offsets instead of
 *    raw virtual addresses.
 *  - Delta table and history table are keyed by hash(IP, capability) so that
 *    the same IP touching different objects gets separate tracking.
 *  - Prefetch candidates are bounds-checked against the authorizing capability
 *    instead of checking page boundaries — enabling cross-page prefetching
 *    within an object while preventing out-of-bounds speculation.
 *  - Latency table carries the capability base so that fill-time learning can
 *    reconstruct the cap-relative offset.
 */

#include "berti_cheri_params.h"
#include "cache.h"
#include "cheri_prefetch_utils.h"

#include <algorithm>
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

class berti_cheri : public champsim::modules::prefetcher {

    public:
    /*****************************************************************************
     *                              Stats                                        *
     *****************************************************************************/
    typedef struct welford
    {
        uint64_t num = 0; 
        float average = 0.0;
    } welford_t;
    
    welford_t average_latency;
    
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

    // CHERI stats
    uint64_t stat_too_small_cap = 0;       // capability too small to prefetch
    uint64_t stat_pf_bounded_by_cap = 0;   // prefetches clipped by capability bounds
    uint64_t stat_cross_page_in_cap = 0;   // cross-page prefetches within capability

    
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
        /* Latency table tracks outstanding misses and prefetch requests.
         * Extended with cap_base to reconstruct offsets at fill time. */
        private:
        struct latency_table {
            uint64_t addr     = 0; // Cache-line-granularity VA
            uint64_t tag      = 0; // Combined IP+cap tag
            uint64_t time     = 0; // Event cycle
            uint64_t cap_base = 0; // Capability base address (byte addr)
            bool     pf       = false;
        };
        int size;
        
        latency_table *latencyt;
    
        public:
        LatencyTable(const int size) : size(size)
        {
            latencyt = new latency_table[size];
        }
        ~LatencyTable() { delete[] latencyt; }
    
        uint8_t  add(uint64_t addr, uint64_t tag, bool pf, uint64_t cycle,
                     uint64_t cap_base);
        uint64_t get(uint64_t addr);
        uint64_t del(uint64_t addr);
        uint64_t get_tag(uint64_t addr);
        uint64_t get_cap_base(uint64_t addr);
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
        /* stores recent accesses per IP+cap.
          In the CHERI version, the addr field stores the
         capability-relative cache-line offset rather than a raw VA. */
        private:
        static constexpr int sets = HISTORY_TABLE_SETS;
        static constexpr int ways = HISTORY_TABLE_WAYS;
    
        struct history_table {
            uint64_t tag  = 0; // Combined IP+cap tag
            uint64_t time = 0; // Timestamp
            uint64_t addr = 0; // Cap-relative cache-line offset
        }; // This struct is the history table
    
        history_table historyt[sets][ways];
        history_table *history_pointers[sets];
    
        uint16_t get_aux(uint32_t latency, uint64_t tag, uint64_t act_addr,
            uint64_t *tags, uint64_t *addr, uint64_t cycle);
    
        public:
        HistoryTable()
        {
            for (int i = 0; i < sets; i++)
                history_pointers[i] = &historyt[i][0];
        }
    
        void add(uint64_t tag, uint64_t addr, uint64_t cycle);
        uint16_t get(uint32_t latency, uint64_t tag, uint64_t act_addr, 
            uint64_t *tags, uint64_t *addr, uint64_t cycle);
    };
    
    /* Berti Table (delta confidence tracking) */
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
    /* find_and_update now takes a cap-relative cache-line offset instead of a raw line address. */
    void find_and_update(uint64_t latency, uint64_t tag, uint64_t cycle, int64_t cap_cl_offset);
    uint8_t get(uint64_t tag, std::vector<delta_t> &res);
    uint64_t ip_hash(uint64_t ip);

    /* Compute combined tag from IP and capability id. */
    uint64_t combined_tag(uint64_t ip_val, const champsim::capability& cap);
    
    uint64_t me = 0;
    static uint64_t others;
    static std::vector<LatencyTable*> latencyt;
    static std::vector<ShadowCache*> scache;
    static std::vector<HistoryTable*> historyt;

    using prefetcher::prefetcher;
    uint32_t prefetcher_cache_operate(champsim::address addr, champsim::address ip, uint8_t cache_hit, bool useful_prefetch, access_type type,
                                      uint32_t metadata_in);
    uint32_t prefetcher_cache_fill(champsim::address addr, long set, long way, uint8_t prefetch, champsim::address evicted_addr, uint32_t metadata_in);

    void prefetcher_initialize();
    void prefetcher_cycle_operate();
    void prefetcher_final_stats();
};
#endif