#ifndef SC_H
#define SC_H

#include "tage_helper.h"
#include "tage.h"

#ifdef ENABLE_SC

class StatisticalCorrector {
public:
    // bias tables indexed by TAGE outputs
    int8_t bias_pc[1 << LOGBIAS]     = {};
    int8_t bias_pclmap[1 << LOGBIAS] = {};
    int8_t bias_lmap[4]              = {};

#ifdef ENABLE_SC_OTHERTABLES
    int8_t bias_alt[1 << 7]     = {};
    int8_t bias_bim[1 << 6]     = {};
    int8_t bias_lmhcalt[1 << 9] = {};
#endif

    // global history GEHL
#ifdef ENABLE_SC_GEHL
    int gm[GNB] = {53, 39, 24, 11, 4};
    int am[ANB] = {31, 17, 7, 2};
    int8_t ggehl_store[GNB][1 << LOGGNB] = {};
    int8_t* ggehl[GNB] = {};
    int8_t agehl_store[ANB][1 << LOGGNB] = {};
    int8_t* agehl[ANB] = {};

    // backward history GEHL
    int bm[BNB] = {11};
    int8_t bgehl_store[BNB][1 << LOGBNB] = {};
    int8_t* bgehl[BNB] = {};

    // forward history GEHL
    int fm[FNB] = {10};
    int8_t fgehl_store[FNB][1 << LOGFNB] = {};
    int8_t* fgehl[FNB] = {};

    // block-region history GEHL
    int pm[PNB] = {12};
    int8_t pgehl_store[PNB][1 << LOGPNB] = {};
    int8_t* pgehl[PNB] = {};
#endif

    // IMLI bias tables - large-region (br/ta) and small-region (sbr/sta)
#ifdef ENABLE_SC_IMLI
    int8_t ibias_br[1 << LOGINB]  = {};
    int8_t ibias_ta[1 << LOGINB]  = {};
    int8_t isbias_br[1 << LOGINB] = {};
    int8_t isbias_ta[1 << LOGINB] = {};
#endif

    // local history GEHL tables
#ifdef ENABLE_SC_LOCAL
    int lm[LNB] = {25, 16, 10, 5};
    int8_t lgehl_store[LNB][1 << LOGLNB] = {};
    int8_t* lgehl[LNB] = {};
#endif
#ifdef ENABLE_SC_LOCALS
    int sm[SNB] = {30, 18, 9, 4};
    int8_t sgehl_store[SNB][1 << LOGSNB] = {};
    int8_t* sgehl[SNB] = {};
#endif
#ifdef ENABLE_SC_LOCALT
    int tm[TNB] = {17, 8, 3};
    int8_t tgehl_store[TNB][1 << LOGTNB] = {};
    int8_t* tgehl[TNB] = {};
    int qm[QNB] = {45, 24, 12};
    int8_t qgehl_store[QNB][1 << LOGQNB] = {};
    int8_t* qgehl[QNB] = {};
#endif

    // multiplicative weights (EXTRAW)
#ifdef ENABLE_SC_EXTRAW
    int8_t wl[1 << LOGSIZEUPS]  = {};
    int8_t wl0[1 << LOGSIZEUPS] = {};
    int8_t wi[1 << LOGSIZEUPS]  = {};
    int8_t wi0[1 << LOGSIZEUPS] = {};
#endif

    // adaptive threshold
    int update_threshold                    = 23;
    int p_update_threshold[1 << LOGSIZEUPS] = {};
    int s_update_threshold[1 << LOGSIZEUPS] = {};

    StatisticalCorrector();

    bool predict(uint64_t pc, const tage_prediction_info& tinfo,
                 const history_state& hs, prediction_entry& pe);
    void update(const prediction_entry& pe, bool resolve_dir);
    int compute_storage() const;

private:
    int gehl_predict(uint64_t pc, uint64_t hist, const int* lengths,
                     int8_t** tab, int nbr, int logs) const;
    void gehl_update(uint64_t pc, bool taken, uint64_t hist,
                     const int* lengths, int8_t** tab, int nbr, int logs);

    static uint64_t gehl_index(uint64_t pc, uint64_t bhist, int i, int logs) {
        uint64_t idx = pc ^ bhist;
        idx ^= (bhist >> (8  - i));
        idx ^= (bhist >> (16 - 2 * i));
        idx ^= (bhist >> (24 - 3 * i));
        idx ^= (bhist >> (32 - 3 * i));
        idx ^= (bhist >> (40 - 4 * i));
        idx ^= (bhist >> (48 - 4 * i));
        return idx & ((1ULL << logs) - 1);
    }

    // index functions
    static int ind_bias_pc(uint64_t pc) {
        return (int)((pc ^ (pc >> (LOGBIAS - 5))) & ((1 << LOGBIAS) - 1));
    }
    static int ind_bias_lmap(bool lmp, bool hcp) {
        return (int)lmp + ((int)hcp << 1);
    }
    static int ind_bias_pclmap(uint64_t pc, bool lmp, bool hcp) {
        int pi = ind_bias_pc(pc);
        int lb = ((int)lmp ^ ((int)hcp << 1)) << (LOGBIAS - 2);
        return (pi ^ lb) & ((1 << LOGBIAS) - 1);
    }
#ifdef ENABLE_SC_OTHERTABLES
    static int ind_bias_alt(const tage_prediction_info& t) {
        int diff = t.hit_bank - t.alt_bank;
        int bd = (diff > 4) + (diff > 9) + (diff > 13);
        return (int)(t.longest_match_pred == t.alt_taken)
             + (t.tage_conf << 1)
             + ((int)(t.alt_conf >= t.tage_conf) << 3)
             + ((int)t.longest_match_pred << 4)
             + (bd << 5);
    }
    static int ind_bias_bim(const tage_prediction_info& t) {
        int bh = (t.hit_bank > 0) + (t.hit_bank > 7) + (t.hit_bank > 13);
        return (int)(t.longest_match_pred == t.bim_pred)
             + ((int)(t.bim_conf >= t.tage_conf) << 1)
             + (bh << 2)
             + ((int)(t.alt_bank > 0) << 4)
             + ((int)t.longest_match_pred << 5);
    }
    static int ind_bias_lmhcalt(const tage_prediction_info& t) {
        return (int)t.longest_match_pred
             + ((int)t.hc_pred << 1)
             + ((int)t.alt_taken << 2)
             + (t.tage_conf << 3)
             + (t.alt_conf << 5)
             + (t.hc_conf << 7);
    }
#endif
#ifdef ENABLE_SC_IMLI
    // large-region IMLI indices
    static int ind_imli_br(uint64_t pc, uint64_t f) {
        return (int)((pc ^ f ^ (pc >> (LOGINB - 6))) & ((1 << LOGINB) - 1));
    }
    static int ind_imli_ta(uint64_t pc, uint64_t f) {
        return (int)(((pc >> 4) ^ f ^ (pc << (LOGINB - 4))) & ((1 << LOGINB) - 1));
    }
    // small-region IMLI indices (different hash to reduce aliasing)
    static int ind_s_imli_br(uint64_t pc, uint64_t f) {
        return (int)((pc ^ (pc >> 6) ^ f ^ (pc >> (LOGINB - 6))) & ((1 << LOGINB) - 1));
    }
    static int ind_s_imli_ta(uint64_t pc, uint64_t f) {
        return (int)(((pc >> 5) ^ f ^ (pc << (LOGINB - 5))) & ((1 << LOGINB) - 1));
    }
#endif
    static int ind_upd(uint64_t pc) {
        return (int)((pc ^ (pc >> 2)) & ((1 << LOGSIZEUPS) - 1));
    }
    static int ind_upd0(uint64_t pc) {
        return (int)((pc ^ (pc >> 5) ^ (pc >> 3)) & ((1 << LOGSIZEUPS) - 1));
    }
};

#endif // ENABLE_SC
#endif