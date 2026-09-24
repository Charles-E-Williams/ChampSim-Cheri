#include "no.h"

uint32_t no::prefetcher_cache_operate(champsim::address addr, champsim::address ip, uint32_t cpu, champsim::capability cap, uint8_t cache_hit,
                                      bool useful_prefetch, access_type type, uint32_t metadata_in, uint32_t metadata_hit)
{
  // assert(addr == ip); // Invariant for instruction prefetchers
  return metadata_in;
}

uint32_t no::prefetcher_cache_fill(champsim::address addr, champsim::address ip, uint32_t cpu, champsim::capability cap, bool useless, long set, long way,
                                   bool prefetch, champsim::address evicted_addr, champsim::capability evicted_cap, uint32_t metadata_in,
                                   uint32_t metadata_evict, uint32_t cpu_evict)
{
  return metadata_in;
}
