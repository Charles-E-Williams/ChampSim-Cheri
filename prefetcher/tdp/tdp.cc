#include "tdp.h"

#include <iostream>

void tdp::prefetcher_initialize() { std::cout << "TDP (standalone, recursive)" << std::endl; }

void tdp::prefetcher_cycle_operate() { engine.on_cycle(*this); }

uint32_t tdp::prefetcher_cache_operate(champsim::address addr, champsim::address, uint32_t, champsim::capability cap, uint8_t cache_hit,
                                       bool useful_prefetch, access_type type, uint32_t metadata_in, uint32_t metadata_hit)
{
  engine.on_operate(champsim::block_number{addr}.to<uint64_t>(), cap, cache_hit, useful_prefetch, type, metadata_hit);
  return metadata_in;
}

uint32_t tdp::prefetcher_cache_fill(champsim::address addr, champsim::address ip, uint32_t cpu, champsim::capability, bool useless, long, long way,
                                    bool prefetch, champsim::address evicted_addr, champsim::capability, uint32_t metadata_in, uint32_t metadata_evict,
                                    uint32_t)
{
  if (evicted_addr == champsim::address{} && way == intern_->NUM_WAY)
    return metadata_in; // a dropped fill (as berti_tdp treats it)
  engine.on_fill(*this, champsim::block_number{addr}.to<uint64_t>(), ip.to<uint64_t>(), cpu, prefetch, metadata_in, useless, metadata_evict);
  return metadata_in;
}

void tdp::prefetcher_final_stats()
{
  std::cout << "TDP (standalone, region of interest)" << std::endl;
  engine.print_stats(std::cout);
}
