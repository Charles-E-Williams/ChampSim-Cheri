#ifndef TAGE_DEFINES_H
#define TAGE_DEFINES_H

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

// core scaling knob: +1 roughly doubles total storage
#define LOGSCALE (BUDGET_LEVEL + 1)
// Seznec: SC GEHL tables use LOGSCALEB = LOGSCALE+1
#define LOGSCALEB (LOGSCALE + 1)

// per-level tuning: NHIST, TBITS, UWIDTH, MAXHIST
// NHIST capped at 28 for levels 5-6 so TAGE doesn't consume the full budget
#if BUDGET_LEVEL == 0
  #define NHIST     12
  #define TBITS      8
  #define UWIDTH     1
  #define MAXHIST  300
#elif BUDGET_LEVEL == 1
  #define NHIST     16
  #define TBITS     10
  #define UWIDTH     1
  #define MAXHIST  500
#elif BUDGET_LEVEL == 2
  #define NHIST     22
  #define TBITS     12
  #define UWIDTH     2
  #define MAXHIST  700
#elif BUDGET_LEVEL == 3
  #define NHIST     28
  #define TBITS     14
  #define UWIDTH     2
  #define MAXHIST 1000
#elif BUDGET_LEVEL == 4
  #define NHIST     28
  #define TBITS     14
  #define UWIDTH     2
  #define MAXHIST 1000
#elif BUDGET_LEVEL == 5
  #define NHIST     28
  #define TBITS     15
  #define UWIDTH     2
  #define MAXHIST 1500
#elif BUDGET_LEVEL == 6
  #define NHIST     28
  #define TBITS     15
  #define UWIDTH     2
  #define MAXHIST 2000
#endif

// tagged table geometry
#define LOGT     (7 + LOGSCALE)
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
#define LOGB        (11 + LOGSCALE)
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

// per-level local history control:
//   0 = no local history
//   1 = L only
//   2 = L + S
//   3 = L + S + T + Q  (full, matches Seznec at 192KB)
#if BUDGET_LEVEL <= 2
  #define LOCAL_LEVEL 0
#elif BUDGET_LEVEL <= 4
  #define LOCAL_LEVEL 3
#else
  #define LOCAL_LEVEL 2
#endif

// SC parameters
#ifdef ENABLE_SC
#define PERCWIDTH   6
#define LOGBIAS     (7 + LOGSCALE)
#define ENABLE_SC_OTHERTABLES
#define FORCEONHIGHCONF
#define WIDTHRES    15
#define WIDTHRESP   11
#define LOGSIZEUPS  5

// SC GEHL tables (global history): G, A, B, F, P
#define ENABLE_SC_GEHL
#define LOGGNB  (LOGSCALEB + 6)
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
#define LOGINB      LOGBIAS
#define LOGREGSIZE  6

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