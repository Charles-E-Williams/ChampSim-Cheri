#ifndef TAGE_SC_H
#define TAGE_SC_H

#include "modules.h"
#include "tage_helper.h"
#include "tage_defines.h"
#include "tage.h"
#ifdef ENABLE_SC
#include "sc.h"
#endif

#include <algorithm>
#include <cstdio>
#include <deque>

struct tage_sc : champsim::modules::branch_predictor {
    using branch_predictor::branch_predictor;
    static constexpr uint8_t BR_CONDITIONAL = 2;

    TAGE tage;
#ifdef ENABLE_SC
    StatisticalCorrector sc;
#endif

    history_state hist;
    uint8_t ghist[HISTBUFFERLENGTH] = {};

    std::deque<prediction_entry> pred_buffer;
    bool initialized = false;

    void ensure_init();
    bool predict_branch(champsim::address ip);
    void last_branch_result(champsim::address ip, champsim::address branch_target,
                            bool taken, uint8_t branch_type);

    void print_storage_budget() const
    {
        int tage_bits = tage.compute_storage();
        int total     = tage_bits;

        int target_bits = (24 * 1024 * 8) << BUDGET_LEVEL;
        double target_kb = target_bits / 8192.0;

        std::fprintf(stderr, "\nTAGE-SC budget (BUDGET_LEVEL=%d, LOGSCALE=%d, NHIST=%d):\n", BUDGET_LEVEL, LOGSCALE, NHIST);
        std::fprintf(stderr, "  TAGE:  %7d bits  (%6.1f KB)\n", tage_bits, tage_bits / 8192.0);

    #ifdef ENABLE_SC
        int sc_bits = sc.compute_storage();
        total += sc_bits;
        std::fprintf(stderr, "  SC:    %7d bits  (%6.1f KB)\n", sc_bits, sc_bits / 8192.0);
    #endif

        std::fprintf(stderr, "  TOTAL: %7d bits  (%6.1f KB)  target=%3.0f KB  util=%.1f%%\n\n", total, total / 8192.0, target_kb, 100.0 * total / target_bits);

        if (total > target_bits)
            std::fprintf(stderr, "[TAGE-SC] WARNING: OVER BUDGET by %.1f KB\n\n", (total - target_bits) / 8192.0);
    }
};

#endif