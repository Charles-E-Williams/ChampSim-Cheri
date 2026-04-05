#include "sc.h"

#ifdef ENABLE_SC

// helper: initialize GEHL table arrays with alternating -1/0 pattern
static void init_gehl(int8_t** ptrs, int8_t* store, int nbr, int log_entries)
{
    int entries = 1 << log_entries;
    for (int i = 0; i < nbr; i++) {
        ptrs[i] = store + i * entries;
        for (int j = 0; j < entries; j++)
            if (!(j & 1)) ptrs[i][j] = -1;
    }
}

StatisticalCorrector::StatisticalCorrector()
{
    // BiasPCLMAP: alternating -1 init on even entries
    for (int j = 0; j < (1 << LOGBIAS); j++) {
        if (!(j & 1))
            bias_pclmap[j] = -1;
    }

#ifdef ENABLE_SC_GEHL
    init_gehl(ggehl, &ggehl_store[0][0], GNB, LOGGNB);
    init_gehl(agehl, &agehl_store[0][0], ANB, LOGGNB);
    init_gehl(bgehl, &bgehl_store[0][0], BNB, LOGBNB);
    init_gehl(fgehl, &fgehl_store[0][0], FNB, LOGFNB);
    init_gehl(pgehl, &pgehl_store[0][0], PNB, LOGPNB);
#endif

#ifdef ENABLE_SC_IMLI
    for (int j = 0; j < (1 << LOGINB); j++) {
        if (!(j & 1)) {
            ibias_br[j] = -1; ibias_ta[j] = -1;
            isbias_br[j] = -1; isbias_ta[j] = -1;
        }
    }
#endif

#ifdef ENABLE_SC_LOCAL
    init_gehl(lgehl, &lgehl_store[0][0], LNB, LOGLNB);
#endif
#ifdef ENABLE_SC_LOCALS
    init_gehl(sgehl, &sgehl_store[0][0], SNB, LOGSNB);
#endif
#ifdef ENABLE_SC_LOCALT
    init_gehl(tgehl, &tgehl_store[0][0], TNB, LOGTNB);
    init_gehl(qgehl, &qgehl_store[0][0], QNB, LOGQNB);
#endif
}

int StatisticalCorrector::gehl_predict(uint64_t pc, uint64_t hist, const int* lengths, int8_t** tab, int nbr, int logs) const
{
    int sum = 0;
    for (int i = 0; i < nbr; i++) {
        uint64_t bh  = hist & ((1ULL << lengths[i]) - 1);
        uint64_t idx = gehl_index(pc, bh, i, logs);
        sum += incval(tab[i][idx]);
    }
    return sum;
}

void StatisticalCorrector::gehl_update(uint64_t pc, bool taken, uint64_t hist, const int* lengths, int8_t** tab, int nbr, int logs)
{
    for (int i = 0; i < nbr; i++) {
        uint64_t bh  = hist & ((1ULL << lengths[i]) - 1);
        uint64_t idx = gehl_index(pc, bh, i, logs);
        ctrupdate(tab[i][idx], taken, PERCWIDTH);
    }
}

bool StatisticalCorrector::predict(uint64_t pc, const tage_prediction_info& tinfo,
                                   const history_state& hs, prediction_entry& pe)
{
    int sum = 0;

    // bias tables indexed by TAGE outputs
    pe.ind_bias_pc     = ind_bias_pc(pc);
    pe.ind_bias_lmap   = ind_bias_lmap(tinfo.longest_match_pred, tinfo.hc_pred);
    pe.ind_bias_pclmap = ind_bias_pclmap(pc, tinfo.longest_match_pred, tinfo.hc_pred);

    sum += incval(bias_lmap[pe.ind_bias_lmap]);
    sum += 2 * incval(bias_pc[pe.ind_bias_pc]);
    sum += incval(bias_pclmap[pe.ind_bias_pclmap]);

#ifdef ENABLE_SC_OTHERTABLES
    pe.ind_bias_alt     = ind_bias_alt(tinfo);
    pe.ind_bias_bim     = ind_bias_bim(tinfo);
    pe.ind_bias_lmhcalt = ind_bias_lmhcalt(tinfo);
    sum += incval(bias_alt[pe.ind_bias_alt]);
    sum += incval(bias_bim[pe.ind_bias_bim]);
    sum += incval(bias_lmhcalt[pe.ind_bias_lmhcalt]);
#endif

    pe.pred_tsc = (sum >= 0);

    // global history GEHL
    int sum_ghist = 0;
#ifdef ENABLE_SC_GEHL
    pe.gh_snap = hs.gh;
    pe.bhist_snap = hs.bhist;
    pe.fhist_snap = hs.fhist;
    pe.phist_block_snap = hs.phist_block;

    sum_ghist += gehl_predict(pc, hs.gh, gm, ggehl, GNB, LOGGNB);
    uint64_t pc_mixed = pc ^ ((uint64_t)tinfo.longest_match_pred
                              ^ ((uint64_t)tinfo.hc_pred << 1));
    sum_ghist += gehl_predict(pc_mixed, hs.gh, am, agehl, ANB, LOGGNB);

    pe.pred_tsc = (sum + sum_ghist >= 0);

    // forward and block-region GEHL
    sum_ghist += gehl_predict(pc ^ (pc >> 3), hs.fhist, fm, fgehl, FNB, LOGFNB);
    sum_ghist += gehl_predict(pc ^ (pc >> 3), hs.phist_block, pm, pgehl, PNB, LOGPNB);

    // backward GEHL: only when IMLI counters are low (not in a detected loop)
#ifdef ENABLE_SC_IMLI
    if (hs.ta_imli < 12 && hs.br_imli < 12
        && hs.s_ta_imli < 12 && hs.s_br_imli < 12)
#endif
        sum_ghist += gehl_predict(pc ^ (pc >> 6), hs.bhist, bm, bgehl, BNB, LOGBNB);
#endif // ENABLE_SC_GEHL

    // IMLI bias tables
    int sum_imli = 0;
#ifdef ENABLE_SC_IMLI
    pe.f_br_imli_snap   = hs.f_br_imli;
    pe.f_ta_imli_snap   = hs.f_ta_imli;
    pe.f_s_br_imli_snap = hs.f_s_br_imli;
    pe.f_s_ta_imli_snap = hs.f_s_ta_imli;
    pe.ta_imli_snap     = hs.ta_imli;
    pe.br_imli_snap     = hs.br_imli;
    pe.s_ta_imli_snap   = hs.s_ta_imli;
    pe.s_br_imli_snap   = hs.s_br_imli;

    // large-region IMLI
    sum_imli += incval(ibias_ta[ind_imli_ta(pc, hs.f_ta_imli)]);
    sum_imli += incval(ibias_br[ind_imli_br(pc, hs.f_br_imli)]);
    // small-region IMLI
    sum_imli += incval(isbias_ta[ind_s_imli_ta(pc, hs.f_s_ta_imli)]);
    sum_imli += incval(isbias_br[ind_s_imli_br(pc, hs.f_s_br_imli)]);
#endif

    // local history GEHL
    int sum_local = 0;
#ifdef ENABLE_SC_LOCAL
    pe.l_shist_snap = hs.l_shist[TAGE::get_local_index(pc)];
    sum_local += gehl_predict(pc, pe.l_shist_snap, lm, lgehl, LNB, LOGLNB);
#endif
#ifdef ENABLE_SC_LOCALS
    pe.s_slhist_snap = hs.s_slhist[TAGE::get_second_local_index(hs.pcblock)];
    sum_local += gehl_predict(pc, pe.s_slhist_snap, sm, sgehl, SNB, LOGSNB);
#endif
#ifdef ENABLE_SC_LOCALT
    pe.t_slhist_snap = hs.t_slhist[TAGE::get_third_local_index(pc)];
    sum_local += gehl_predict(pc, pe.t_slhist_snap, tm, tgehl, TNB, LOGTNB);
    pe.q_slhist_snap = hs.q_slhist[TAGE::get_fourth_local_index(pc)];
    sum_local += gehl_predict(pc, pe.q_slhist_snap, qm, qgehl, QNB, LOGQNB);
#endif

    // multiplicative weights: scale local and IMLI contributions
#ifdef ENABLE_SC_EXTRAW
    pe.ind_upd  = ind_upd(pc);
    pe.ind_upd0 = ind_upd0(pc);
    sum_local = (((1 << PERCWIDTH) + wl[pe.ind_upd]) *  ((1 << PERCWIDTH) + wl0[pe.ind_upd0]) * sum_local) >> (2 * PERCWIDTH - 1);
    sum_imli  = (((1 << PERCWIDTH) + wi[pe.ind_upd]) *  ((1 << PERCWIDTH) + wi0[pe.ind_upd0]) * sum_imli) >> (2 * PERCWIDTH - 1);
#else
    pe.ind_upd  = ind_upd(pc);
    pe.ind_upd0 = ind_upd0(pc);
#endif

    // store partial sums for EXTRAW update
    pe.sum_local     = sum_local;
    pe.sum_imli_full = sum_imli;
    pe.sum_ghist     = sum_ghist;

    sum += sum_ghist + sum_imli + sum_local;
    pe.sum_sc  = sum;
    pe.pred_sc = (sum >= 0);

    pe.threshold = p_update_threshold[pe.ind_upd] + s_update_threshold[pe.ind_upd0] + update_threshold;

    bool final_pred;
#ifdef FORCEONHIGHCONF
    if (tinfo.tage_conf == 3 && abs(sum) < pe.threshold / 2)
        final_pred = tinfo.longest_match_pred;
    else
        final_pred = pe.pred_sc;
#else
    final_pred = pe.pred_sc;
#endif
    return final_pred;
}

void StatisticalCorrector::update(const prediction_entry& pe, bool resolve_dir)
{
    bool sc_pred = pe.pred_sc;
    int  sum     = pe.sum_sc;
    int  thres   = pe.threshold;

    if (sc_pred != resolve_dir || abs(sum) < thres) {

        // multiplicative weight updates
#ifdef ENABLE_SC_EXTRAW
        if ((pe.sum_local >= 0) != (sum - pe.sum_local >= 0)) {
            ctrupdate(wl[pe.ind_upd], ((pe.sum_local >= 0) == resolve_dir), PERCWIDTH);
            ctrupdate(wl0[pe.ind_upd0], ((pe.sum_local >= 0) == resolve_dir), PERCWIDTH);
        }
        if ((pe.sum_imli_full >= 0) != (sum - pe.sum_imli_full >= 0)) {
            ctrupdate(wi[pe.ind_upd], ((pe.sum_imli_full >= 0) == resolve_dir), PERCWIDTH);
            ctrupdate(wi0[pe.ind_upd0], ((pe.sum_imli_full >= 0) == resolve_dir), PERCWIDTH);
        }
#endif

        // threshold adaptation
        if (abs(sum) >= thres / 2) {
            if (sc_pred != resolve_dir) {
                p_update_threshold[pe.ind_upd]  += 1;
                s_update_threshold[pe.ind_upd0] += 1;
                update_threshold += 1;
            } else {
                p_update_threshold[pe.ind_upd]  -= 1;
                s_update_threshold[pe.ind_upd0] -= 1;
                update_threshold -= 1;
            }
            auto clamp = [](int& v, int lo, int hi) {
                if (v < lo) v = lo; 
                if (v > hi) v = hi;
            };
            clamp(update_threshold, 0, (1 << WIDTHRES) - 1);
            clamp(p_update_threshold[pe.ind_upd], 0, (1 << WIDTHRESP) - 1);
            clamp(s_update_threshold[pe.ind_upd0], 0, (1 << WIDTHRESP) - 1);
        }

        // bias table updates
        ctrupdate(bias_lmap[pe.ind_bias_lmap], resolve_dir, PERCWIDTH);
        ctrupdate(bias_pc[pe.ind_bias_pc], resolve_dir, PERCWIDTH);
        ctrupdate(bias_pclmap[pe.ind_bias_pclmap], resolve_dir, PERCWIDTH);

#ifdef ENABLE_SC_OTHERTABLES
        ctrupdate(bias_alt[pe.ind_bias_alt], resolve_dir, PERCWIDTH);
        ctrupdate(bias_bim[pe.ind_bias_bim], resolve_dir, PERCWIDTH);
        ctrupdate(bias_lmhcalt[pe.ind_bias_lmhcalt], resolve_dir, PERCWIDTH);
#endif

        // GEHL table updates
#ifdef ENABLE_SC_GEHL
        uint64_t pc = pe.pc;
        gehl_update(pc, resolve_dir, pe.gh_snap, gm, ggehl, GNB, LOGGNB);
        uint64_t pc_mixed = pc ^ ((uint64_t)pe.tinfo.longest_match_pred
                                  ^ ((uint64_t)pe.tinfo.hc_pred << 1));
        gehl_update(pc_mixed, resolve_dir, pe.gh_snap, am, agehl, ANB, LOGGNB);

        gehl_update(pc ^ (pc >> 3), resolve_dir, pe.fhist_snap, fm, fgehl, FNB, LOGFNB);
        gehl_update(pc ^ (pc >> 3), resolve_dir, pe.phist_block_snap, pm, pgehl, PNB, LOGPNB);

#ifdef ENABLE_SC_IMLI
        if (pe.ta_imli_snap < 12 && pe.br_imli_snap < 12 && pe.s_ta_imli_snap < 12 && pe.s_br_imli_snap < 12)
#endif
            gehl_update(pc ^ (pc >> 6), resolve_dir, pe.bhist_snap, bm, bgehl, BNB, LOGBNB);
#endif // ENABLE_SC_GEHL

        // IMLI table updates
#ifdef ENABLE_SC_IMLI
        ctrupdate(ibias_ta[ind_imli_ta(pe.pc, pe.f_ta_imli_snap)],
                  resolve_dir, PERCWIDTH);
        ctrupdate(ibias_br[ind_imli_br(pe.pc, pe.f_br_imli_snap)],
                  resolve_dir, PERCWIDTH);
        ctrupdate(isbias_ta[ind_s_imli_ta(pe.pc, pe.f_s_ta_imli_snap)],
                  resolve_dir, PERCWIDTH);
        ctrupdate(isbias_br[ind_s_imli_br(pe.pc, pe.f_s_br_imli_snap)],
                  resolve_dir, PERCWIDTH);
#endif

        // local history GEHL updates
#ifdef ENABLE_SC_LOCAL
        gehl_update(pe.pc, resolve_dir, pe.l_shist_snap, lm, lgehl, LNB, LOGLNB);
#endif
#ifdef ENABLE_SC_LOCALS
        gehl_update(pe.pc, resolve_dir, pe.s_slhist_snap, sm, sgehl, SNB, LOGSNB);
#endif
#ifdef ENABLE_SC_LOCALT
        gehl_update(pe.pc, resolve_dir, pe.t_slhist_snap, tm, tgehl, TNB, LOGTNB);
        gehl_update(pe.pc, resolve_dir, pe.q_slhist_snap, qm, qgehl, QNB, LOGQNB);
#endif
    }
}

int StatisticalCorrector::compute_storage() const
{
    int bits = 0;

    // BiasPC + BiasPCLMAP
    bits += 2 * (1 << LOGBIAS) * PERCWIDTH;
    // BiasLMAP
    bits += 4 * PERCWIDTH;

#ifdef ENABLE_SC_OTHERTABLES
    bits += (1 << 7) * PERCWIDTH;
    bits += (1 << 6) * PERCWIDTH;
    bits += (1 << 9) * PERCWIDTH;
#endif

#ifdef ENABLE_SC_GEHL
    bits += GNB * (1 << LOGGNB) * PERCWIDTH;
    bits += ANB * (1 << LOGGNB) * PERCWIDTH;
    bits += gm[0];  // GH bits
    bits += BNB * (1 << LOGBNB) * PERCWIDTH;
    bits += bm[0];
    bits += FNB * (1 << LOGFNB) * PERCWIDTH;
    bits += fm[0];
    bits += PNB * (1 << LOGPNB) * PERCWIDTH;
    bits += pm[0];
#endif

#ifdef ENABLE_SC_IMLI
    // large + small IMLI: 4 tables
    bits += 4 * (1 << LOGINB) * PERCWIDTH;
    // IMLI counter storage: 2 large + 2 small region counters
    bits += 2 * LOGBIAS;  // large TaIMLI, BrIMLI
    bits += 2 * LOGBIAS;  // small TaSIMLI, BrSIMLI
    bits += 10 + 10;      // LastBackPC + LastBack (large)
    bits += 14 + 14;      // SLastBackPC + SLastBack (small, wider)
#endif

#ifdef ENABLE_SC_LOCAL
    bits += LNB * (1 << LOGLNB) * PERCWIDTH;
    bits += NLOCAL * lm[0];
#endif
#ifdef ENABLE_SC_LOCALS
    bits += SNB * (1 << LOGSNB) * PERCWIDTH;
    bits += NSECLOCAL * sm[0];
#endif
#ifdef ENABLE_SC_LOCALT
    bits += TNB * (1 << LOGTNB) * PERCWIDTH;
    bits += NTLOCAL * tm[0];
    bits += QNB * (1 << LOGQNB) * PERCWIDTH;
    bits += NQLOCAL * qm[0];
#endif

#ifdef ENABLE_SC_EXTRAW
    bits += 4 * PERCWIDTH * (1 << LOGSIZEUPS);
#endif

    // threshold management
    bits += WIDTHRES;
    bits += 2 * (1 << LOGSIZEUPS) * WIDTHRESP;

    return bits;
}

#endif // ENABLE_SC