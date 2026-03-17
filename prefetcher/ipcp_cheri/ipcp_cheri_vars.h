#ifndef IPCP_CHERI_VARS_H
#define IPCP_CHERI_VARS_H

// Original IPCP parameters
#define NUM_IP_TABLE_L1_ENTRIES 1024
#define NUM_GHB_ENTRIES 16
#define NUM_IP_INDEX_BITS 10
#define NUM_IP_TAG_BITS 6
#define S_TYPE 1   // stream
#define CS_TYPE 2  // constant stride
#define CPLX_TYPE 3 // complex stride
#define NL_TYPE 4  // next line


// #define SIG_DEBUG_PRINT
#ifdef SIG_DEBUG_PRINT
#define SIG_DP(x) x
#else
#define SIG_DP(x)
#endif

#endif