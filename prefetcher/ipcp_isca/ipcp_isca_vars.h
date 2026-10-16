#ifndef IPCP_ISCA_VARS_H
#define IPCP_ISCA_VARS_H

/***************************************************************************
IPCP at the L1D, sized as in Pakalapati & Panda, "Bouquet of Instruction
Pointers", ISCA'20 (Fig. 5, Table I).
***************************************************************************/

#define NUM_IP_TABLE_ENTRIES 64      // IP table entries, direct-mapped
#define NUM_IP_INDEX_BITS 6          // Bits to index into the IP table
#define NUM_IP_TAG_BITS 9            // Tag bits per IP table entry
#define NUM_IP_ALIGN_BITS 1          // RISC-V: 2-byte instruction alignment, PC bit 0 carries no information
#define NUM_SIG_BITS 7               // CPLX signature width
#define NUM_CSPT_ENTRIES 128         // = 2^NUM_SIG_BITS
#define NUM_RST_ENTRIES 8            // region stream table entries
#define NUM_LINES_IN_REGION 32       // 2 KB GS region
#define LOG2_LINES_IN_REGION 5
#define DENSE_THRESHOLD 24           // 75% of the region's lines
#define POSNEG_BITS 6                // pos/neg counter width
#define NUM_RR_ENTRIES 32            // recent request filter entries
#define RR_TAG_BITS 12               // tag bits per recent request filter entry
#define LOG2_LEARN_PAGE 12           // last-vpage / last-line-offset kept at 4 KB granularity (2 + 6 bits)
#define MAX_STRIDE 63                // 7-bit sign-magnitude stride

#define GS_DEGREE 6                  // default degrees (Sec. V)
#define CS_DEGREE 3
#define CPLX_DEGREE 3
#define THROTTLE_WINDOW 256          // per-class prefetch fills per accuracy check
#define ACC_HIGH 0.75                // throttling watermarks
#define ACC_LOW 0.40

#define NL_MISS_WINDOW 256           // tentative NL: misses per kilo-cycle over this many misses
#define NL_MPKC_THRESHOLD 15.0

#define NONE_TYPE 0                  // 2-bit class kept in the L1D line's prefetch metadata
#define GS_TYPE 1                    // global stream
#define CS_TYPE 2                    // constant stride
#define CPLX_TYPE 3                  // complex stride
#define NL_STAT 4                    // index of next-line prefetches in the issue statistics

#endif /* IPCP_ISCA_VARS_H */