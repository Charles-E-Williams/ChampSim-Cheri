#ifndef TAGE_H
#define TAGE_H

#include "tage_helper.h"

class TAGE {
public:
    bentry btable[1 << LOGB];
    static constexpr int TAGGED_ENTRIES = NBANK * (1 << LOGG) * ASSOC;
    gentry gtable[TAGGED_ENTRIES];
    int m[NHIST + 1] = {};

    // allocation/replacement state
    int TICK = 0, TICKH = 0;
    int8_t use_alt_on_na = 0;
    int8_t count_miss11 = -64; // more or less than 11% of misspredictions
    uint32_t seed = 0;

    // PRNG entropy sources 
    uint64_t prng_phist = 0;
    int      prng_ptr   = 0;
    uint16_t prng_gtag4 = 0;

    // allocation filtering
    int8_t count50     [1 << LOGCOUNT][(NHIST_MAX / 4) + 1] = {};
    int8_t count16_31  [1 << LOGCOUNT][(NHIST_MAX / 4) + 1] = {};
    int8_t count8_15   [1 << LOGCOUNT][(NHIST_MAX / 4) + 1] = {};
    int8_t gcount50    [(NHIST_MAX / 4) + 1] = {};
    int8_t gcount16_31 [(NHIST_MAX / 4) + 1] = {};
    int8_t gcount8_15  [(NHIST_MAX / 4) + 1] = {};

    TAGE();
    
    void init_histories(history_state& hs);
    bool predict(uint64_t pc, const history_state& hs, prediction_entry& pe);
    void update(const prediction_entry& pe, bool resolve_dir, bool sc_pred, bool sc_pred_tsc);
    void update_history(uint64_t pc_raw, bool taken, uint64_t target, uint8_t branch_type, history_state& hs, uint8_t* ghist);
    int compute_storage() const;

private:

    int bindex(uint64_t pc) const { return (int)((pc ^ (pc >> LOGB)) & ((1 << LOGB) - 1));}
    int indcount(uint64_t pc) const { return (int)((pc ^ (pc >> LOGCOUNT)) & ((1 << LOGCOUNT) - 1));}

    int F(uint64_t A, int size, int bank) const;
    int gindex(uint64_t pc, int bank, uint64_t phist, const std::array<folded_history, NHIST_MAX + 1>& ch_i) const;
    uint16_t gtag_hash(uint64_t pc, int bank, uint64_t phist, const std::array<folded_history, NHIST_MAX + 1>& ch_t0, const std::array<folded_history, NHIST_MAX + 1>& ch_t1) const;
    bool getbim(int bi, tage_prediction_info& tinfo);
    void baseupdate(int bi, bool taken);
    void allocate(const prediction_entry& pe, bool resolve_dir);
    uint32_t myrandom();

    // local history index helpers 
public:
    static uint64_t get_local_index(uint64_t pc) {
#ifdef ENABLE_SC_LOCAL
        return (pc ^ (pc >> 2)) & (NLOCAL - 1);
#else
        (void)pc; return 0;
#endif
    }
#ifdef ENABLE_SC_LOCALS
    static uint64_t get_second_local_index(uint64_t pcblock) {
        return (pcblock ^ (pcblock >> 5)) & (NSECLOCAL - 1);
    }
#endif
#ifdef ENABLE_SC_LOCALT
    static uint64_t get_third_local_index(uint64_t pc) {
        return (pc ^ (pc >> 5)) & (NTLOCAL - 1);
    }
    static uint64_t get_fourth_local_index(uint64_t pc) {
        return (pc ^ (pc >> 3)) & (NQLOCAL - 1);
    }
#endif
};

#endif