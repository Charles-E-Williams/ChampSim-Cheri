// tage.cc -- TAGE predictor implementation
#include "tage.h"
#include <cstdio>
#include <cstdlib>
#include <cmath>

TAGE::TAGE()
{
    memset(btable, 0, sizeof(btable));
    memset(gtable, 0, sizeof(gtable));
    for (int i = 0; i <= NHIST / 4; i++) {
        gcount50[i] = 7; gcount16_31[i] = 7; gcount8_15[i] = 7;
        for (int j = 0; j < (1 << LOGCOUNT); j++) {
            count50[j][i] = 7; count16_31[j][i] = 7; count8_15[j][i] = 7;
        }
    }
}

void TAGE::init_histories(history_state& hs)
{
    static constexpr int NNHIST = NHIST + 8;
    int mm[NNHIST + 1];
    mm[1] = MINHIST;
    for (int i = 2; i <= NNHIST; i++) {
        mm[i] = (int)(MINHIST * pow((double)MAXHIST / MINHIST, (double)(i - 1) / (NNHIST - 1)) + 0.5);
        if (mm[i] <= mm[i - 1] + 1)
            mm[i] = mm[i - 1] + 1;
    }

    int PT = 1;
    for (int i = 1; i <= 7 && PT <= NHIST; i += 2)
        m[PT++] = mm[i];
    for (int i = 9; i <= NNHIST - 8 && PT <= NHIST; i++)
        m[PT++] = mm[i];
    PT = NHIST;
    for (int i = NNHIST; i >= NNHIST - 6 && PT > 0; i -= 2)
        m[PT--] = mm[i];

    for (int i = 1; i <= NHIST; i++) {
        if (m[i] == 0)
            m[i] = (int)(MINHIST * pow((double)MAXHIST / MINHIST, (double)(i - 1) / (NHIST - 1)) + 0.5);
        if (i > 1 && m[i] <= m[i - 1])
            m[i] = m[i - 1] + 1;
    }

    for (int i = 1; i <= NHIST; i++)
        m[i] *= BITS_PER_BR;

    for (int i = 1; i <= NHIST; i++) {
        hs.ch_i[i].init(m[i], 23, i - 1);
#if TBITS <= 11
        hs.ch_t0[i].init(m[i], TBITS - 1, i);
        hs.ch_t1[i].init(m[i], TBITS, i + 2);
#elif TBITS <= 13
        hs.ch_t0[i].init(m[i], 13, i);
        hs.ch_t1[i].init(m[i], 11, i + 2);
#else
        hs.ch_t0[i].init(m[i], 13, i);
        hs.ch_t1[i].init(m[i], TBITS, i + 2);
#endif
    }
    hs.ptr   = 0;
    hs.phist = 0;
    hs.gh    = 0;
}

int TAGE::F(uint64_t A, int size, int bank) const
{
    A = A & ((1ULL << size) - 1);
    int A1 = (int)(A & ((1 << LOGG) - 1));
    int A2 = (int)(A >> LOGG);
    if (bank < LOGG)
        A2 = ((A2 << bank) & ((1 << LOGG) - 1)) ^ (A2 >> (LOGG - bank));
    A1 ^= A2;
    if (bank < LOGG)
        A1 = ((A1 << bank) & ((1 << LOGG) - 1)) ^ (A1 >> (LOGG - bank));
    return A1;
}

int TAGE::gindex(uint64_t pc, int bank, uint64_t phist, const std::array<folded_history, NHIST_MAX + 1>& ch_i) const
{
    int M = (m[bank] > PHISTWIDTH) ? PHISTWIDTH : m[bank];
    uint32_t idx = (uint32_t)pc ^ ((uint32_t)pc >> (abs(LOGG - bank) + 1));
    idx ^= ch_i[bank].comp ^ (ch_i[bank].comp >> LOGG);
    idx ^= F(phist, M, bank);
    idx = (idx ^ (idx >> LOGG) ^ (idx >> (2 * LOGG))) & ((1 << LOGG) - 1);
    return (int)idx;
}

uint16_t TAGE::gtag_hash(uint64_t pc, int bank, uint64_t phist, const std::array<folded_history, NHIST_MAX + 1>& ch_t0, const std::array<folded_history, NHIST_MAX + 1>& ch_t1) const
{
    int M = (m[bank] > PHISTWIDTH) ? PHISTWIDTH : m[bank];
    int tag = (int)pc ^ ((int)pc >> 2);
    tag = (tag >> 1) ^ ((tag & 1) << 10) ^ F(phist, M, bank);
    tag ^= ch_t0[bank].comp ^ (ch_t1[bank].comp << 1);
    tag ^= tag >> TBITS;
    tag ^= (tag >> (TBITS - 2));
    return (uint16_t)(tag & ((1 << TBITS) - 1));
}

bool TAGE::getbim(int bi, tage_prediction_info& t)
{
    bool pred_dir = (btable[bi].pred != 0);
    int8_t hyst_val = btable[bi >> HYSTSHIFT].hyst;
    t.bim      = static_cast<int8_t>(pred_dir ? hyst_val : (-1 - hyst_val));
    t.bim_pred = pred_dir;
    t.bim_conf  = hyst_val;
    t.tage_conf = hyst_val;
    t.alt_conf  = hyst_val;
    t.hc_conf   = hyst_val;
    return pred_dir;
}

void TAGE::baseupdate(int bi, bool taken)
{
    int8_t inter = static_cast<int8_t>(btable[bi].pred
        ? btable[bi >> HYSTSHIFT].hyst
        : (-1 - btable[bi >> HYSTSHIFT].hyst));
    ctrupdate(inter, taken, BIMWIDTH);
    btable[bi].pred = (inter >= 0) ? 1 : 0;
    btable[bi >> HYSTSHIFT].hyst = static_cast<int8_t>(
        (inter >= 0) ? inter : (-inter - 1));
}

//  mix in phist, ghist pointer, and GTAG[4] for entropy
uint32_t TAGE::myrandom()
{
    seed++;
    seed += (uint32_t)prng_phist;
    seed = (seed >> 21) + (seed << 11);
    seed += (uint32_t)prng_ptr;
    seed = (seed >> 10) + (seed << 22);
    seed += prng_gtag4;
    return seed;
}


bool TAGE::predict(uint64_t pc, const history_state& hs, prediction_entry& pe)
{
    pe.pc = pc;
    tage_prediction_info& t = pe.tinfo;

    for (int i = 1; i <= NHIST; i++) {
        uint32_t base = gindex(pc, i, hs.phist, hs.ch_i);
        pe.gtag[i] = gtag_hash(pc, i, hs.phist, hs.ch_t0, hs.ch_t1);

        int bank1 = (int)((pc ^ (pc >> 4) ^ (hs.phist & ((1ULL << m[1]) - 1))) % NBANK);
        base += ((bank1 + i) % NBANK) * (1 << LOGG);
        base *= ASSOC;

        pe.gi[0][i] = base;
#if PSK
        pe.gi[1][i] = base ^ ((pe.gtag[i] & 0xFF) << 1);
#else
        pe.gi[1][i] = base;
#endif
    }

    // store PRNG entropy for myrandom() calls during update
    prng_phist = hs.phist;
    prng_ptr   = hs.ptr;
    prng_gtag4 = (NHIST >= 4) ? pe.gtag[4] : 0;

    pe.bi = bindex(pc);
    bool bim_pred = getbim(pe.bi, t);

    t.longest_match_pred = bim_pred;
    t.hc_pred   = bim_pred;
    t.alt_taken = bim_pred;
    t.tage_pred = bim_pred;
    t.hit_bank  = 0;
    t.alt_bank  = 0;
    t.hc_bank   = 0;

    for (int i = NHIST; i > 0; i--) {
        for (int j = 0; j < ASSOC; j++) {
            int idx = pe.gi[j][i] + j;
            if (gtable[idx].tag == pe.gtag[i]) {
                t.hit_bank = i;
                t.hit_way  = j;
                t.longest_match_pred = (gtable[idx].ctr >= 0);
                t.tage_conf = abs(2 * gtable[idx].ctr + 1) >> 1;
                goto found_hit;
            }
        }
    }
    found_hit:

    for (int i = t.hit_bank - 1; i > 0; i--) {
        for (int j = 0; j < ASSOC; j++) {
            int idx = pe.gi[j][i] + j;
            if (gtable[idx].tag == pe.gtag[i]) {
                t.alt_bank = i;
                t.alt_way  = j;
                goto found_alt;
            }
        }
    }
    found_alt:

    if (t.hit_bank > 0 && t.alt_bank > 0) {
        int idx = pe.gi[t.alt_way][t.alt_bank] + t.alt_way;
        t.alt_taken = (gtable[idx].ctr >= 0);
        t.alt_conf  = abs(2 * gtable[idx].ctr + 1) >> 1;
    }

    if (t.hit_bank > 0) {
        if (t.tage_conf == 0) {
            for (int i = t.hit_bank - 1; i > 0; i--) {
                for (int j = 0; j < ASSOC; j++) {
                    int idx = pe.gi[j][i] + j;
                    if (gtable[idx].tag == pe.gtag[i]) {
                        if (abs(2 * gtable[idx].ctr + 1) != 1) {
                            t.hc_bank = i;
                            t.hc_way  = j;
                            t.hc_pred = (gtable[idx].ctr >= 0);
                            t.hc_conf = abs(2 * gtable[idx].ctr + 1) >> 1;
                            goto found_hc;
                        }
                        break;
                    }
                }
            }
            found_hc:;
        } else {
            t.hc_bank = t.hit_bank;
            t.hc_way  = t.hit_way;
            t.hc_pred = t.longest_match_pred;
            t.hc_conf = t.tage_conf;
        }
    }

    if (t.hit_bank > 0) {
#ifdef ENABLE_SC
        t.tage_pred = t.longest_match_pred;
#else
        bool weak = (abs(2 * gtable[pe.gi[t.hit_way][t.hit_bank] + t.hit_way].ctr + 1) == 1);
        if (weak && use_alt_on_na >= 0)
            t.tage_pred = t.hc_pred;
        else
            t.tage_pred = t.longest_match_pred;
#endif
    }

    return t.tage_pred;
}


void TAGE::update(const prediction_entry& pe, bool resolve_dir,
                  bool sc_pred, bool sc_pred_tsc)
{
    const tage_prediction_info& t = pe.tinfo;

    bool do_alloc = (t.hit_bank < NHIST);
    do_alloc &= (t.longest_match_pred != resolve_dir);
    do_alloc &= (sc_pred_tsc != resolve_dir);

    if (t.hit_bank > 0) {
        int hit_idx = pe.gi[t.hit_way][t.hit_bank] + t.hit_way;
        bool weak = (t.tage_conf == 0);

        if (weak && t.longest_match_pred != resolve_dir) {
            if (t.alt_bank > 0 && t.alt_bank != t.hc_bank)
                ctrupdate(gtable[pe.gi[t.alt_way][t.alt_bank] + t.alt_way].ctr,
                          resolve_dir, CWIDTH);
            if (t.hc_bank > 0)
                ctrupdate(gtable[pe.gi[t.hc_way][t.hc_bank] + t.hc_way].ctr,
                          resolve_dir, CWIDTH);
            else
                baseupdate(pe.bi, resolve_dir);
        }

#ifndef ENABLE_SC
        if (weak && t.longest_match_pred != t.hc_pred)
            ctrupdate(use_alt_on_na, (t.hc_pred == resolve_dir), ALTWIDTH);
#endif

        ctrupdate(gtable[hit_idx].ctr, resolve_dir, CWIDTH);
    } else {
        baseupdate(pe.bi, resolve_dir);
    }

    if (t.hit_bank > 0 && t.longest_match_pred != t.alt_taken) {
        int idx = pe.gi[t.hit_way][t.hit_bank] + t.hit_way;
        if (t.longest_match_pred == resolve_dir) {
            if (gtable[idx].u == 0)
                gtable[idx].u++;
            if (gtable[idx].u < (1 << UWIDTH) - 1)
                gtable[idx].u++;
        } else {
            if (gtable[idx].u > 0 && sc_pred == resolve_dir)
                gtable[idx].u--;
        }
    }

    if ((t.tage_pred != resolve_dir) || ((myrandom() & 31) < 4))
        ctrupdate(count_miss11, (t.tage_pred != resolve_dir), 8);

    int ic = indcount(pe.pc);

    if (t.hit_bank > 0 && t.tage_conf == 0) {
        for (int i = t.hit_bank / 4; i <= NHIST / 4; i++) {
            bool correct = (resolve_dir == t.longest_match_pred);
            ctrupdate(count50[ic][i], correct, 7);
            ctrupdate(gcount50[i], correct, 7);
            if (!correct || ((myrandom() & 31) > 1)) {
                ctrupdate(count16_31[ic][i], correct, 7);
                ctrupdate(gcount16_31[i], correct, 7);
            }
            if (!correct || ((myrandom() & 31) > 3)) {
                ctrupdate(count8_15[ic][i], correct, 7);
                ctrupdate(gcount8_15[i], correct, 7);
            }
        }
    }

#ifdef FILTERALLOCATION
    if (t.tage_conf < 2) {
        int q = (t.hit_bank + 1) / 4;
        if (count50[ic][q] < 0 && gcount50[q] < 0)
            do_alloc &= ((myrandom() & ((1 << (4 - t.tage_conf)) - 1)) == 0);
        else if (count16_31[ic][q] < 0 && gcount16_31[q] < 0)
            do_alloc &= ((myrandom() & ((1 << (2 - t.tage_conf)) - 1)) == 0);
        else if (count8_15[ic][q] < 0 && gcount8_15[q] < 0)
            do_alloc &= ((myrandom() & ((1 << (1 - t.tage_conf)) - 1)) == 0);
    }
#endif

    if (do_alloc)
        allocate(pe, resolve_dir);
}

void TAGE::allocate(const prediction_entry& pe, bool resolve_dir)
{
    int ic = indcount(pe.pc);
    int q  = (pe.tinfo.hit_bank + 1) / 4;

    int max_alloc = pe.tinfo.tage_conf
        + !(count50[ic][q] < 0 && gcount50[q] < 0)
        + !(count16_31[ic][q] < 0 && gcount16_31[q] < 0)
        + !(count8_15[ic][q] < 0 && gcount8_15[q] < 0);

    int NA = 0, penalty = 0;
    int dep = pe.tinfo.hit_bank + 1;
    dep += ((myrandom() & 1) == 0);
    dep += ((myrandom() & 3) == 0);
    if (dep <= pe.tinfo.hit_bank) dep = pe.tinfo.hit_bank + 1;
    if (dep > NHIST) return;

    bool first = true;

    for (int i = dep; i <= NHIST; i++) {
        bool done = false;
        int start_way = myrandom() % ASSOC;
        bool rep[ASSOC]  = {};
        int  irep[ASSOC] = {};
        bool move[ASSOC] = {};

        for (int jj = 0; jj < ASSOC; jj++) {
            int j = (start_way + 1 + jj) % ASSOC;
            int idx = pe.gi[j][i] + j;

            if (gtable[idx].u == 0) {
                rep[j]  = true;
                irep[j] = idx;
            }
#if REPSK
            else {
                int other_way = j ^ 1;
                int other_idx = (pe.gi[j][i] ^ ((gtable[idx].tag & 0xFF) << 1)) + other_way;
                if (other_idx >= 0 && other_idx < TAGGED_ENTRIES
                    && gtable[other_idx].u == 0) {
                    rep[j]  = true;
                    irep[j] = other_idx;
                    move[j] = true;
                }
            }
#endif
            if (rep[j]) {
                bool proceed = (UWIDTH == 2);
#if (UWIDTH == 1)
                int ctr_str = abs(2 * gtable[idx].ctr + 1) >> 1;
                int mask = (1 << ctr_str) - 1;
                proceed = ((myrandom() & mask) == 0) || (TICKH >= BORNTICK / 2);
#endif
                if (proceed) {
                    done = true;
                    if (move[j]) {
                        gtable[irep[j]].u   = gtable[idx].u;
                        gtable[irep[j]].tag = gtable[idx].tag;
                        gtable[irep[j]].ctr = gtable[idx].ctr;
                    }
                    gtable[idx].tag = pe.gtag[i];
                    gtable[idx].ctr = resolve_dir ? 0 : -1;
                    bool protect = ((UWIDTH >= 2) || (TICKH >= BORNTICK / 2)) && first;
                    gtable[idx].u = protect ? 1 : 0;
                    NA++;
                    if (i >= 3 || !first) max_alloc--;
                    first = false;
                    i += 2;
                    i -= ((myrandom() & 1) == 0);
                    i += ((myrandom() & 1) == 0);
                    i += ((myrandom() & 3) == 0);
                    break;
                }
            }
        }

        if (max_alloc < 0) break;

        if (!done) {
            for (int jj = 0; jj < ASSOC; jj++) {
                int j = jj;
                int idx = pe.gi[j][i] + j;
                if ((myrandom() & ((1 << (1 + LOGASSOC + REPSK)) - 1)) == 0)
                    if (abs(2 * gtable[idx].ctr + 1) == 1)
                        if (gtable[idx].u == 1)
                            gtable[idx].u--;
#if REPSK
                if (irep[j] > 0 && irep[j] < TAGGED_ENTRIES)
                    if ((myrandom() & ((1 << (1 + LOGASSOC + REPSK)) - 1)) == 0)
                        if (abs(2 * gtable[irep[j]].ctr + 1) == 1)
                            if (gtable[irep[j]].u == 1)
                                gtable[irep[j]].u--;
#endif
            }
            penalty++;
        }
        if (NA >= 3) break;
    }

    TICKH += 2 * penalty - 3 * NA;
    if (TICKH < 0) TICKH = 0;
    if (TICKH >= BORNTICK) TICKH = BORNTICK;

    TICK += penalty - (2 + 2 * (count_miss11 >= 0)) * NA;
    if (TICK < 0) TICK = 0;
    if (TICK >= BORNTICK) {
        for (int j = 0; j < TAGGED_ENTRIES; j++)
            if (gtable[j].u > 0) gtable[j].u--;
        TICK  = 0;
        TICKH = 0;
    }
}

void TAGE::update_history(uint64_t pc_raw, bool taken, uint64_t target,
                          uint8_t branch_type, history_state& hs, uint8_t* ghist)
{
    uint64_t pcbr = pc_raw >> 2;
    bool backward = (target != 0 && pc_raw > target);

    // path hash for SC GEHL
    if (taken || backward) {
        hs.gh = (hs.gh << 1) ^ pcbr;
        hs.gh <<= 1;
        hs.gh ^= (target >> 4) ^ (target >> 2);
    } else {
        hs.gh <<= 1;
    }

    // local history updates on conditional branches
    bool is_cond = (branch_type == 2);  // ChampSim: BRANCH_CONDITIONAL = 2
    if (is_cond) {
#ifdef ENABLE_SC_LOCAL
        uint64_t li = get_local_index(pcbr);
        hs.l_shist[li] = (hs.l_shist[li] << 1) + (int)taken;
#endif
#ifdef ENABLE_SC_LOCALS
        uint64_t si = get_second_local_index(hs.pcblock);
        hs.s_slhist[si] = (hs.s_slhist[si] << 1) + (int)taken;
#endif
#ifdef ENABLE_SC_LOCALT
        uint64_t ti = get_third_local_index(pcbr);
        hs.t_slhist[ti] = ((hs.t_slhist[ti] << 1) + (int)taken) ^ ((long long)pcbr & 15);
        uint64_t qi = get_fourth_local_index(pcbr);
        hs.q_slhist[qi] = (hs.q_slhist[qi] << 3)
            + (((long long)(hs.gh >> 2) & 3) << 1) + (int)taken;
#endif
    }

    // PCBLOCK: used as index source for second local history
    if (taken) hs.pcblock = target >> 2;

#ifdef ENABLE_SC_GEHL
    // PHIST: 256-byte block region history
    if ((target >> 8) != hs.prev_region) {
        hs.phist_block = (hs.phist_block << 1) ^ (target >> 8);
        hs.prev_region = (target >> 8);
    }
#endif

    // FHIST: forward taken branch path
#ifdef ENABLE_SC_IMLI
    if (taken && target > pc_raw && target != 0)
        hs.fhist = (hs.fhist << 3) ^ (target >> 2) ^ (pc_raw >> 1);
#endif

    // IMLI counter maintenance
#ifdef ENABLE_SC_IMLI
    // 1=INDIRECT, 4=INDIRECT_CALL, 5=RETURN
    bool is_indirect_or_ret = (branch_type == 1 || branch_type == 4
                               || branch_type == 5);
    if (is_indirect_or_ret) {
        hs.ta_imli = 0;
        hs.br_imli = 0;
        hs.s_ta_imli = 0;
        hs.s_br_imli = 0;
        goto derive_imli;
    }

    if (taken && backward) {
        // large-region IMLI (64-byte regions)
        uint64_t tgt_reg = (target & 0xFFFF) >> LOGREGSIZE;
        uint64_t pc_reg  = (pc_raw & 0xFFFF) >> LOGREGSIZE;

        if (tgt_reg == hs.last_back_tgt) {
            if (hs.ta_imli < (uint64_t)((1 << LOGBIAS) - 1)) hs.ta_imli++;
        } else {
            hs.ta_imli = 0;
        }
        if (pc_reg == hs.last_back_pc) {
            if (hs.br_imli < (uint64_t)((1 << LOGBIAS) - 1)) hs.br_imli++;
        } else {
            hs.br_imli = 0;
        }
        hs.last_back_tgt = tgt_reg;
        hs.last_back_pc  = pc_reg;

        // small-region IMLI (4-byte regions)
        uint64_t s_tgt = target & 0xFFFF;
        uint64_t s_pc  = pc_raw & 0xFFFF;

        if (s_tgt == hs.s_last_back_tgt) {
            if (hs.s_ta_imli < (uint64_t)((1 << LOGBIAS) - 1)) hs.s_ta_imli++;
        } else {
            // BHIST update on small-region target change
            hs.bhist = (hs.bhist << 1) ^ hs.s_last_back_tgt;
            hs.s_ta_imli = 0;
        }
        if (s_pc == hs.s_last_back_pc) {
            if (hs.s_br_imli < (uint64_t)((1 << LOGBIAS) - 1)) hs.s_br_imli++;
        } else {
            // BHIST update on small-region PC change
            hs.bhist = (hs.bhist << 1) ^ hs.s_last_back_pc;
            hs.s_br_imli = 0;
        }
        hs.s_last_back_tgt = s_tgt;
        hs.s_last_back_pc  = s_pc;
    }

    derive_imli:
    // large-region derived IMLI
    hs.f_ta_imli = (hs.ta_imli == 0 || hs.br_imli == hs.ta_imli)
                   ? (hs.bhist & 63) : hs.ta_imli;
    hs.f_br_imli = (hs.br_imli == 0)
                   ? (hs.fhist & 63) : hs.br_imli;
    // small-region derived IMLI
    hs.f_s_ta_imli = (hs.s_ta_imli == 0 || hs.s_br_imli == hs.s_ta_imli)
                     ? (hs.bhist & 7) : hs.s_ta_imli;
    hs.f_s_br_imli = (hs.s_br_imli == 0)
                     ? (hs.fhist & 7) : hs.s_br_imli;
#endif

    // T and PATH for TAGE folded history
    int T    = (int)(pcbr ^ (pcbr >> 2) ^ (pcbr >> 4) ^ (target >> 1) ^ taken);
    int PATH = (int)((pcbr ^ (pcbr >> 2)) ^ (target >> 3));

    hs.phist = (hs.phist << 5) ^ PATH;
    hs.phist &= ((1ULL << PHISTWIDTH) - 1);

    for (int t = 0; t < BITS_PER_BR; t++) {
        int dir = T & 1;
        T >>= 1;
        hs.ptr--;
        ghist[hs.ptr & (HISTBUFFERLENGTH - 1)] = (uint8_t)dir;
        for (int i = 1; i <= NHIST; i++) {
            hs.ch_i[i].update(ghist, hs.ptr);
            hs.ch_t0[i].update(ghist, hs.ptr);
            hs.ch_t1[i].update(ghist, hs.ptr);
        }
    }
}

int TAGE::compute_storage() const
{
    int bits = 0;
    bits += NBANK * (1 << LOGG) * ASSOC * (CWIDTH + UWIDTH + TBITS);
    bits += (1 << LOGB);
    bits += (BIMWIDTH - 1) * (1 << (LOGB - HYSTSHIFT));
    bits += m[NHIST];
    bits += PHISTWIDTH;
    bits += 12;
    if (UWIDTH == 1) bits += 12;
    bits += 36;  // seed (32) + misc (4)
    bits += 8;
    bits += 3 * 7 * ((NHIST / 4 + 1) * (1 << LOGCOUNT) + (NHIST / 4 + 1));
#ifndef ENABLE_SC
    bits += ALTWIDTH;
#endif
    return bits;
}