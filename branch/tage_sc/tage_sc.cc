#include "tage_sc.h"

void tage_sc::ensure_init()
{
    if (initialized) return;
    tage.init_histories(hist);
    memset(ghist, 0, sizeof(ghist));
    print_storage_budget();
    initialized = true;
}

bool tage_sc::predict_branch(champsim::address ip)
{
    ensure_init();

    uint64_t pc = ip.to<uint64_t>() >> 2;

    if (pred_buffer.size() >= MAX_PRED_ENTRIES)
        pred_buffer.pop_front();

    pred_buffer.emplace_back();
    prediction_entry& pe = pred_buffer.back();
    pe.pc = pc;

    bool prediction = tage.predict(pc, hist, pe);

#ifdef ENABLE_SC
    prediction = sc.predict(pc, pe.tinfo, hist, pe);
#else
    pe.pred_sc  = prediction;
    pe.pred_tsc = prediction;
#endif

    pe.prediction = prediction;
    return prediction;
}

void tage_sc::last_branch_result(champsim::address ip,champsim::address branch_target, bool taken, uint8_t branch_type)
{
    ensure_init();

    uint64_t pc     = ip.to<uint64_t>() >> 2;
    uint64_t target = branch_target.to<uint64_t>();

    // search from the back to find the most recent prediction for this PC
    auto it = pred_buffer.end();
    for (auto rit = pred_buffer.rbegin(); rit != pred_buffer.rend(); ++rit) {
        if (rit->pc == pc) {
            it = rit.base() - 1;
            break;
        }
    }

    if (it != pred_buffer.end() && branch_type == BR_CONDITIONAL) {
        const prediction_entry& pe = *it;

#ifdef ENABLE_SC
        sc.update(pe, taken);
#endif
        tage.update(pe, taken, pe.pred_sc, pe.pred_tsc);

        pred_buffer.erase(it);
    } else if (it != pred_buffer.end()) {
        pred_buffer.erase(it);
    }

    // update history for al branch types
    tage.update_history(ip.to<uint64_t>(), taken, target, branch_type, hist, ghist);
}