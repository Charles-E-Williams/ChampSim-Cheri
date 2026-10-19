#include "tage_sc.h"
#include "instruction.h"

void tage_sc::ensure_init()
{
    if (initialized) return;

    tage.init_histories(hist);
    memset(ghist, 0, sizeof(ghist));
    print_storage_budget();
    initialized = true;
}

bool tage_sc::predict_branch(champsim::address ip,  [[maybe_unused]] champsim::address predicted_target, [[maybe_unused]] bool always_taken, uint8_t type)
{
    if (type != BRANCH_CONDITIONAL) //only train/predict on conditional branches. all branch types still update history in last_branch_result
        return false;
    ensure_init();

    uint64_t pc = ip.to<uint64_t>() >> 2;

    last_pred = prediction_entry{};
    last_pred_valid = true;
    prediction_entry& pe = last_pred;
    pe.pc = pc;

    bool prediction = tage.predict(pc, hist, pe);

#ifdef ENABLE_SC
    prediction = sc->predict(pc, pe.tinfo, hist, pe);
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
    uint64_t target = taken ? branch_target.to<uint64_t>() : ip.to<uint64_t>() + 4;  // use the fall-through PC

    if (last_pred_valid && branch_type == BRANCH_CONDITIONAL && last_pred.pc == pc) {
        const prediction_entry& pe = last_pred;


#ifdef ENABLE_SC
        sc->update(pe, taken);
#endif
        tage.update(pe, taken, pe.pred_sc, pe.pred_tsc);
    } 
    last_pred_valid = false;
    
    // update history for all branch types
    tage.update_history(ip.to<uint64_t>(), taken, target, branch_type, hist, ghist);
}