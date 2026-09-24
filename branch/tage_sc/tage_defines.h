#ifndef TAGE_DEFINES_H
#define TAGE_DEFINES_H

// ---------------------------------------------------------------------------
//  BUDGET_LEVEL selects the hardware budget target (set via compiler flag):
//    0 → 24 KB    1 → 48 KB    2 → 96 KB    3 → 192 KB (Seznec reference)
//    4 → 384 KB   5 → 768 KB   6 → 1536 KB
// ---------------------------------------------------------------------------
#ifndef BUDGET_LEVEL
#define BUDGET_LEVEL 3
#endif
#if BUDGET_LEVEL < 0 || BUDGET_LEVEL > 6
#error "BUDGET_LEVEL must be 0-6"
#endif

// SC toggle: define TAGE_ONLY via compiler flag to disable SC
#ifndef TAGE_ONLY
#define ENABLE_SC
#endif

// ---------------------------------------------------------------------------
//  Per-level parameter table
//
//  Each level's LOGT (tagged-table way size), LOGB (bimodal), LOGBIAS (SC
//  bias/IMLI table size), and LOGSCALEB (SC GEHL table scale) are set
//  independently to hit the target budget.  Level 3 exactly reproduces
//  Seznec's CBP 2025 defaults (LOGSCALE=4 in his code).
//
//  Level | Target | LOGT | LOGB | LOGBIAS | LOGSCB | NHIST | TBITS | UW | MAXHIST | LOCAL
//  ------|--------|------|------|---------|--------|-------|-------|----|---------|------
//    0   |  24 KB |  10  |  12  |    7    |    2   |  12   |   8   |  1 |   200   |  0
//    1   |  48 KB |  11  |  13  |    8    |    2   |  12   |  10   |  1 |   500   |  0
//    2   |  96 KB |  11  |  14  |    8    |    3   |  20   |  12   |  2 |   700   |  0
//    3   | 192 KB |  11  |  15  |   11    |    5   |  28   |  14   |  2 |  1000   |  3
//    4   | 384 KB |  12  |  16  |   12    |    6   |  28   |  14   |  2 |  1000   |  3
//    5   | 768 KB |  13  |  17  |   13    |    7   |  27   |  15   |  2 |  1500   |  3
//    6   |1536 KB |  14  |  18  |   14    |    8   |  27   |  15   |  2 |  2000   |  3
// ---------------------------------------------------------------------------

#if BUDGET_LEVEL == 0
  #define LOGT       10
  #define LOGB       12
  #define NHIST      12
  #define TBITS       8
  #define UWIDTH      1
  #define MAXHIST   200
  #define LOCAL_LEVEL 0
  #ifdef ENABLE_SC
    #define LOGBIAS     7
    #define LOGSCALEB   2
  #endif

#elif BUDGET_LEVEL == 1
  #define LOGT       11
  #define LOGB       13
  #define NHIST      12
  #define TBITS      10
  #define UWIDTH      1
  #define MAXHIST   500
  #define LOCAL_LEVEL 0
  #ifdef ENABLE_SC
    #define LOGBIAS     8
    #define LOGSCALEB   2
  #endif

#elif BUDGET_LEVEL == 2
  #define LOGT       11
  #define LOGB       14
  #define NHIST      20
  #define TBITS      12
  #define UWIDTH      2
  #define MAXHIST   700
  #define LOCAL_LEVEL 0
  #ifdef ENABLE_SC
    #define LOGBIAS     8
    #define LOGSCALEB   3
  #endif

#elif BUDGET_LEVEL == 3
  #define LOGT       11
  #define LOGB       15
  #define NHIST      28
  #define TBITS      14
  #define UWIDTH      2
  #define MAXHIST  1000
  #define LOCAL_LEVEL 3
  #ifdef ENABLE_SC
    #define LOGBIAS    11
    #define LOGSCALEB   5
  #endif

#elif BUDGET_LEVEL == 4
  #define LOGT       12
  #define LOGB       16
  #define NHIST      28
  #define TBITS      14
  #define UWIDTH      2
  #define MAXHIST  1000
  #define LOCAL_LEVEL 3
  #ifdef ENABLE_SC
    #define LOGBIAS    12
    #define LOGSCALEB   6
  #endif

#elif BUDGET_LEVEL == 5
  #define LOGT       13
  #define LOGB       17
  #define NHIST      27
  #define TBITS      15
  #define UWIDTH      2
  #define MAXHIST  1500
  #define LOCAL_LEVEL 3
  #ifdef ENABLE_SC
    #define LOGBIAS    13
    #define LOGSCALEB   7
  #endif

#elif BUDGET_LEVEL == 6
  #define LOGT       14
  #define LOGB       18
  #define NHIST      27
  #define TBITS      15
  #define UWIDTH      2
  #define MAXHIST  2000
  #define LOCAL_LEVEL 3
  #ifdef ENABLE_SC
    #define LOGBIAS    14
    #define LOGSCALEB   8
  #endif
#endif

// tagged table geometry (derived from LOGT)
#define LOGASSOC 1
#define ASSOC    (1 << LOGASSOC)
#define LOGG     (LOGT - LOGASSOC)
#define NBANK    NHIST

// partial skewed associativity (Seznec 1993)
#if (LOGASSOC == 1)
#define PSK   1
#define REPSK 1
#else
#define PSK   0
#define REPSK 0
#endif

// fixed TAGE parameters
#define CWIDTH      3
#define BIMWIDTH    3
#define HYSTSHIFT   1
#define PHISTWIDTH  27
#define MINHIST     3
#define BITS_PER_BR 5

#define HISTBUFFERLENGTH 8192
#define NHIST_MAX 40

// allocation/replacement
#define BORNTICK    4096
#define ALTWIDTH    5
#define LOGCOUNT    6
#define FILTERALLOCATION

// ---------------------------------------------------------------------------
//  SC parameters (only when ENABLE_SC is defined)
// ---------------------------------------------------------------------------
#ifdef ENABLE_SC
#define PERCWIDTH   6

// LOGINB = LOGBIAS (IMLI bias tables share size with PC bias tables)
#define LOGINB      LOGBIAS
#define LOGREGSIZE  6

#define ENABLE_SC_OTHERTABLES
#define FORCEONHIGHCONF
#define WIDTHRES    15
#define WIDTHRESP   11
#define LOGSIZEUPS  5

// SC GEHL tables (global history): G, A, B, F, P
#define ENABLE_SC_GEHL
#define LOGGNB  (LOGSCALEB + 6)
#define LOGANB  (LOGSCALEB + 6)
#define GNB     5
#define ANB     4
#define LOGBNB  (LOGSCALEB + 6)
#define BNB     1
#define LOGFNB  (LOGSCALEB + 6)
#define FNB     1
#define LOGPNB  (LOGSCALEB + 5)
#define PNB     1

// SC IMLI: both large-region (64B) and small-region (4B)
#define ENABLE_SC_IMLI

// multiplicative weights for local and IMLI components
#define ENABLE_SC_EXTRAW

// local history SC tables (controlled by LOCAL_LEVEL)
#if LOCAL_LEVEL >= 1
#define ENABLE_SC_LOCAL
#define LOGLNB  (LOGSCALEB + 6)
#define LNB     4
#define LOGLOCAL    7
#define NLOCAL      (1 << LOGLOCAL)
#endif

#if LOCAL_LEVEL >= 2
#define ENABLE_SC_LOCALS
#define LOGSNB  (LOGSCALEB + 6)
#define SNB     4
#define LOGSECLOCAL 5
#define NSECLOCAL   (1 << LOGSECLOCAL)
#endif

#if LOCAL_LEVEL >= 3
#define ENABLE_SC_LOCALT
#define LOGTNB  (LOGSCALEB + 6)
#define TNB     3
#define LOGTLOCAL   5
#define NTLOCAL     (1 << LOGTLOCAL)
#define LOGQNB  (LOGSCALEB + 6)
#define QNB     3
#define LOGQLOCAL   4
#define NQLOCAL     (1 << LOGQLOCAL)
#endif

#endif // ENABLE_SC

#define MAX_PRED_ENTRIES 256

#endif