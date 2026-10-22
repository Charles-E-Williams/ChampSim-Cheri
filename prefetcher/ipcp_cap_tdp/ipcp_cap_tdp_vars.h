#ifndef IPCP_CAP_TDP_VARS_H
#define IPCP_CAP_TDP_VARS_H

/***************************************************************************
Capability-aware IPCP at the L1D. The paper parameters are those of
ipcp_isca (ISCA'20, Fig. 5, Table I); the capability layer adds the last block.
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

// Capability layer
#define OBJ_TAG_BITS 5               // C1/C2: hash of the object base kept per IP
#define LARGE_OBJECT_BYTES 2048      // C3: objects of at least one GS region
#define OOB_CLASSES 4                // C4: small-object size classes <=64 B, <=128 B, <=512 B, <2 KB
#define OOB_CTR_MAX 15               // C4: 4-bit counters per (size class, IP-table index)
#define OOB_CTR_INIT 8
#define OOB_L1_THRESHOLD 8           // C4: >= issue to the L1D (and C2 off for the IP)
#define OOB_L2_THRESHOLD 4           // C4: >= issue to the L2C only, below drop
#define OOB_CREDIT 7                 // C4: added on a first demand use; 1 is subtracted on an eviction without use
#define OOB_PROBE_MASK 0x7           // C4: lines with these bits zero always issue to the L1D

#endif /* IPCP_CAP_TDP_VARS_H */