#include "cheri_ptr_chase.h"

#include "cache.h"

void cheri_ptr_chase::prefetcher_initialize()
{
  stat_ptr_found      = 0;
  stat_ptr_not_found  = 0;
  stat_chase_started  = 0;
  stat_pf_issued      = 0;
  stat_bloom_filtered = 0;
  stat_map_miss       = 0;
  stat_null_or_loop   = 0;
  stat_conf_too_low   = 0;
  stat_page_crossings = 0; 
}

uint32_t cheri_ptr_chase::prefetcher_cache_operate(champsim::address addr, champsim::address ip, uint32_t cpu, champsim::capability cap, uint8_t cache_hit,
                                                   bool useful_prefetch, access_type type, uint32_t metadata_in, uint32_t metadata_hit)
{


  auto stored = champsim::cap_mem[intern_->cpu].load_capability(addr);
  bool found_pointer = stored.has_value() && stored->tag;

  if (found_pointer) {
    stat_ptr_found++;
    uint64_t target_cursor = cheri::capability_cursor(*stored).to<uint64_t>();

    pct_train(ip, true);

    if (target_cursor != 0) {

      uint64_t current_vpn = addr.to<uint64_t>() >> LOG2_BLOCK_SIZE;
      uint64_t target_vpn = target_cursor >> LOG2_BLOCK_SIZE;
      if (current_vpn != target_vpn) stat_page_crossings++;

      champsim::address target_addr{target_cursor};
      if (!filter.test(target_addr)) {
        filter.add(target_addr);
        prefetch_line(target_addr, true, 0);
        stat_pf_issued++;
      }
      pct_chase(ip, target_cursor);
    }
  } else {
    stat_ptr_not_found++;
    pct_train(ip, false);
  }

  // fold the IP into 32 bits so we can remember who caused the miss when it fills
  return static_cast<uint32_t>(ip.to<uint64_t>() ^ (ip.to<uint64_t>() >> 32));
}

uint32_t cheri_ptr_chase::prefetcher_cache_fill(champsim::address addr, champsim::address ip, uint32_t cpu, champsim::capability cap, bool useless, long set,
                                                long way, bool prefetch, champsim::address evicted_addr, champsim::capability evicted_cap,
                                                uint32_t metadata_in, uint32_t metadata_evict, uint32_t cpu_evict)
{
  uint64_t cl_base = champsim::block_number{addr}.to<uint64_t>() << LOG2_BLOCK_SIZE;

  // scan the newly arrived cacheline
  for (unsigned slot = 0; slot < CAP_SLOTS_PER_CL; slot++) {
    uint64_t slot_va = cl_base + (static_cast<uint64_t>(slot) << cheri::CAP_ALIGNMENT_BITS);

    auto stored = champsim::cap_mem[intern_->cpu].load_capability(champsim::address{slot_va});

    if (stored.has_value() && stored->tag) {
      uint64_t target = cheri::capability_cursor(*stored).to<uint64_t>();
      
        uint64_t current_vpn = slot_va >> LOG2_BLOCK_SIZE;
        uint64_t target_vpn = target >> LOG2_BLOCK_SIZE;
        if (current_vpn != target_vpn) stat_page_crossings++;

      if (target != 0 && target != cl_base) {
        uint64_t cl = cl_base >> LOG2_BLOCK_SIZE;
        uint64_t idx = filter.hash(cl) % PTR_MAP_SIZE;
        ptr_map[idx] = {cl, target};

        champsim::address pf_addr{target};
        if (!filter.test(pf_addr)) {
          filter.add(pf_addr);
          prefetch_line(pf_addr, true, metadata_in); // Pass metadata forward
          stat_pf_issued++;
        }
        
        break; 
      }
    }
  }
  return metadata_in;
}

void cheri_ptr_chase::prefetcher_final_stats()
{
  fmt::print("\n");
  fmt::print("=== CHERI Pointer Chasing Prefetcher Stats ===\n");
  fmt::print("  Pointer found in cap_mem:   {}\n", stat_ptr_found);
  fmt::print("  No pointer in cap_mem:      {}\n", stat_ptr_not_found);
  fmt::print("  Pointer hit rate:           {:.1f}%\n", stat_ptr_found + stat_ptr_not_found > 0
                 ? 100.0 * static_cast<double>(stat_ptr_found) / (stat_ptr_found + stat_ptr_not_found)
                 : 0.0);
  fmt::print("  ---\n");
  fmt::print("  Chases initiated:             {}\n", stat_chase_started);
  fmt::print("  Prefetches issued:          {}\n", stat_pf_issued);
  fmt::print("  Skipped (confidence too low):     {}\n", stat_conf_too_low);
  fmt::print("  Skipped (bloom filter):     {}\n", stat_bloom_filtered);
  fmt::print("  Stopped (map miss):     {}\n", stat_map_miss);
  fmt::print("  Stopped (null/self-loop):   {}\n", stat_null_or_loop);
  fmt::print("  2MB Page Crossings:         {}\n", stat_page_crossings);
  if (stat_pf_issued > 0) {
      fmt::print("  Page Crossing Rate:         {:.2f}%\n", 
                 ((double)stat_page_crossings / stat_ptr_found)* 100.0);
  }
  fmt::print("============================================\n");
}