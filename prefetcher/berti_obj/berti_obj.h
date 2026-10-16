#ifndef BERTI_OBJ_H_
#define BERTI_OBJ_H_

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

#include "berti_obj_params.h"
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

class berti_obj : public champsim::modules::prefetcher {

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
    /*****************************************************************************
     *      Capability layer: object-local timely deltas across IPs              *
     *****************************************************************************/
    // Berti's local context is the IP. The capability adds a second one, the object: a timely delta from the line
    // IP b touched in an object to a line another IP later missed on in the same object is learned for IP b, and
    // replayed from IP b's later accesses to other objects, inside their bounds only.
    // Object History Table (OHT): Berti's training events (miss, prefetch hit) on tagged multi-line objects.
    // Object Delta Table (ODT): per-IP in-object deltas, with Berti's epoch confidence (ODT_EPOCH object touches).
    // Scope: struct-sized objects (2..OBJ_MAX_LINES lines). Large arrays are left to Berti's per-IP deltas; their
    // streams would also flood the OHT.
    static constexpr bool OBJ_DELTAS = true;
    static constexpr int OHT_SIZE = 32;
    static constexpr int ODT_SIZE = 16;
    static constexpr int ODT_DELTAS = 2;
    static constexpr unsigned OBJ_TAG_BITS = 10;
    static constexpr unsigned OBJ_IP_BITS = 10;   // partial IP tag in the OHT and ODT
    static constexpr unsigned OBJ_LINE_BITS = 6;
    static constexpr int64_t OBJ_MAX_LINES = 64;  // 4 KB
    static constexpr uint32_t ODT_EPOCH = 16;     // object touches of an IP per confidence epoch

    struct obj_ref {
        bool valid = false;
        uint64_t tag = 0;    // OBJ_TAG_BITS hash of (base, length)
        int64_t line = 0;    // line within the object
        int64_t nlines = 0;  // lines overlapping the object
    };
    struct oht_entry {
        bool valid = false;
        uint64_t obj = 0;
        uint64_t ip = 0;
        uint64_t line = 0;   // line within the object (OBJ_LINE_BITS)
        uint64_t time = 0;
    };
    struct odt_delta {
        int64_t delta = 0;
        uint64_t conf = 0;
        uint8_t rpl = BERTI_R;
    };
    struct odt_entry {
        bool valid = false;
        uint64_t ip = 0;
        uint32_t touches = 0;
        uint64_t lru = 0;
        std::array<odt_delta, ODT_DELTAS> deltas{};
    };

    std::array<oht_entry, OHT_SIZE> oht{};
    unsigned oht_head = 0;
    std::array<odt_entry, ODT_SIZE> odt{};
    uint64_t odt_clock = 0;

    static obj_ref object_of(champsim::address addr, const champsim::capability& cap);
    odt_entry* odt_find(uint64_t ip);
    void oht_add(uint64_t ip, const obj_ref& o, uint64_t cycle);
    void odt_train(uint64_t ip, int64_t delta);
    void obj_find_and_update(uint64_t latency, uint64_t ip, const obj_ref& o, uint64_t cycle);

    uint64_t obj_oht_adds = 0;
    uint64_t obj_train_events = 0;
    uint64_t obj_deltas_trained = 0;
    uint64_t obj_epochs = 0;
    uint64_t obj_pf_l1 = 0;
    uint64_t obj_pf_l2 = 0;
    uint64_t obj_pf_out_of_object = 0; // candidates dropped by the bounds check

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
};
#endif