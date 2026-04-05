#ifndef TAGE_HELPER_H
#define TAGE_HELPER_H

#include "tage_defines.h"
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>

inline void ctrupdate(int8_t& ctr, bool taken, int nbits) // tage counters
{
    int max_val = (1 << (nbits - 1)) - 1;
    int min_val = -(1 << (nbits - 1));
    if (taken) { if (ctr < max_val) ctr++; }
    else       { if (ctr > min_val) ctr--; }
}

inline int incval(int8_t ctr) { return 2 * ctr + 1; } // sc update counter

class folded_history {
public:
    unsigned comp = 0;
    int CLENGTH = 0, OLENGTH = 0, OUTPOINT = 0;

    void init(int original_length, int compressed_length, int /*seed*/)
    {
        comp = 0;
        OLENGTH  = original_length;
        CLENGTH  = compressed_length;
        OUTPOINT = OLENGTH % CLENGTH;
    }
    void update(const uint8_t* h, int PT)
    {
        comp  = (comp << 1) ^ h[PT & (HISTBUFFERLENGTH - 1)];
        comp ^= h[(PT + OLENGTH) & (HISTBUFFERLENGTH - 1)] << OUTPOINT;
        comp ^= (comp >> CLENGTH);
        comp &= ((1u << CLENGTH) - 1);
    }
};
 
struct bentry { // TAGE bimodal entry
    int8_t pred = 0;
    int8_t hyst = 1;
};

struct gentry { // TAGE global table entry
    int8_t   ctr = 0;
    uint16_t tag = 0;
    int8_t   u   = 0;
};

struct tage_prediction_info {
    bool longest_match_pred = false; // alternate   TAGE prediction if the longest match was not hitting: needed for updating the u bit
    bool hc_pred            = false; // longest not low confident match or base prediction if no confident match
    bool alt_taken          = false;
    bool tage_pred          = false;  // tage prediction
    bool bim_pred           = false;
    int tage_conf = 0, alt_conf = 0, hc_conf = 0, bim_conf = 0;
    int hit_bank = 0, hit_way = 0;
    int alt_bank = 0, alt_way = 0;
    int hc_bank  = 0, hc_way  = 0;
    int8_t bim = 0;
};

// history state  updated only at commit time
struct history_state {
    int      ptr   = 0;
    uint64_t phist = 0;
    std::array<folded_history, NHIST_MAX + 1> ch_i;
    std::array<folded_history, NHIST_MAX + 1> ch_t0;
    std::array<folded_history, NHIST_MAX + 1> ch_t1;
    // path hash used by SC GEHL tables
    uint64_t gh = 0;

#ifdef ENABLE_SC_IMLI
    // large-region IMLI 
    uint64_t br_imli = 0, ta_imli = 0;
    uint64_t last_back_pc = 0, last_back_tgt = 0;
    uint64_t f_br_imli = 0, f_ta_imli = 0;

    // small-region IMLI 
    uint64_t s_br_imli = 0, s_ta_imli = 0;
    uint64_t s_last_back_pc = 0, s_last_back_tgt = 0;
    uint64_t f_s_br_imli = 0, f_s_ta_imli = 0;

    //  backward-branch path history 
    uint64_t bhist = 0;
    //  forward-taken-branch path history
    uint64_t fhist = 0;
#endif

#ifdef ENABLE_SC_GEHL
    //  256-byte block region history
    uint64_t phist_block = 0;
    uint64_t prev_region = 0;
#endif

    //target >> 2 of last taken branch 
    uint64_t pcblock = 0;

#ifdef ENABLE_SC_LOCAL
    std::array<long long, NLOCAL> l_shist = {};
#endif
#ifdef ENABLE_SC_LOCALS
    std::array<long long, NSECLOCAL> s_slhist = {};
#endif
#ifdef ENABLE_SC_LOCALT
    std::array<long long, NTLOCAL> t_slhist = {};
    std::array<long long, NQLOCAL> q_slhist = {};
#endif
};

struct prediction_entry {
    uint64_t pc = 0;
    bool prediction = false;

    int      bi = 0;
    uint32_t gi[ASSOC][NHIST_MAX + 1]   = {};
    uint16_t gtag[NHIST_MAX + 1] = {};
    tage_prediction_info tinfo;

    int  sum_sc   = 0;
    bool pred_sc  = false;
    bool pred_tsc = false;

#ifdef ENABLE_SC
    int ind_bias_pc = 0, ind_bias_lmap = 0, ind_bias_pclmap = 0;
#ifdef ENABLE_SC_OTHERTABLES
    int ind_bias_alt = 0, ind_bias_bim = 0, ind_bias_lmhcalt = 0;
#endif
    uint64_t gh_snap = 0;
#ifdef ENABLE_SC_IMLI
    uint64_t f_br_imli_snap = 0, f_ta_imli_snap = 0;
    uint64_t f_s_br_imli_snap = 0, f_s_ta_imli_snap = 0;
    // raw IMLI counters 
    uint64_t ta_imli_snap = 0, br_imli_snap = 0;
    uint64_t s_ta_imli_snap = 0, s_br_imli_snap = 0;
#endif
#ifdef ENABLE_SC_GEHL
    uint64_t bhist_snap = 0, fhist_snap = 0, phist_block_snap = 0;
#endif
#ifdef ENABLE_SC_LOCAL
    long long l_shist_snap = 0;
#endif
#ifdef ENABLE_SC_LOCALS
    long long s_slhist_snap = 0;
#endif
#ifdef ENABLE_SC_LOCALT
    long long t_slhist_snap = 0;
    long long q_slhist_snap = 0;
#endif
    int sum_local = 0, sum_imli_full = 0, sum_ghist = 0;
    int threshold = 0, ind_upd = 0, ind_upd0 = 0;
#endif
};

#endif