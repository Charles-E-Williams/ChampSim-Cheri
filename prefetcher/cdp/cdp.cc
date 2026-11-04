#include "cdp.h"

#include <iostream>

void cdp::CDP_Module::do_prefetch(CACHE* intern, champsim::address addr, uint32_t cpu, uint32_t metadata_in, bool fill_this_level)
{
  const unsigned depth = md_depth(metadata_in); // a line CDP did not fetch (demand or another prefetcher)
  if (depth >= MAX_DEPTH) {
    depth_stop++;
    return;
  }

  champsim::block_number line{addr};
  const uint64_t line_va = champsim::address{line}.to<uint64_t>();
  lines_scanned++;
  for (unsigned slot = 0; slot < cheri::CAPS_PER_CL; slot++) {
    auto pointer = champsim::cap_mem[cpu].load_capability(champsim::address{line_va + slot * cheri::CAP_ALIGNMENT_BYTES});
    if (!pointer.has_value() || !pointer->tag || !cheri::has_load_permissions(pointer->permissions) || cheri::has_execute_permissions(pointer->permissions))
      continue; // not a data pointer
    const uint64_t cursor = pointer->base.to<uint64_t>() + pointer->offset.to<uint64_t>();
    if (cursor == 0)
      continue;
    pointers_found++;

    champsim::block_number target{champsim::address{cursor}};
    if (target == line)
      continue; // points into this line
    const uint32_t md = MD_CDP | ((depth + 1) << MD_DEPTH_SHIFT);
    if (intern->prefetch_line(champsim::address{target}, fill_this_level, md, *pointer))
      issued[depth + 1]++;
    else
      pq_full++;
  }
}

void cdp::CDP_Module::print_stats() const
{
  std::cout << "CDP MAX_DEPTH " << MAX_DEPTH << std::endl;
  std::cout << "CDP lines_scanned " << lines_scanned << " pointers_found " << pointers_found << " depth_stop " << depth_stop << " pq_full " << pq_full
            << std::endl;
  std::cout << "CDP issued_by_depth";
  for (unsigned d = 1; d <= MAX_DEPTH; d++)
    std::cout << " " << d << ":" << issued[d];
  std::cout << std::endl;
}

uint32_t cdp::prefetcher_cache_operate(champsim::address addr, champsim::address ip, uint32_t cpu, champsim::capability cap, uint8_t cache_hit,
                                       bool useful_prefetch, access_type type, uint32_t metadata_in, uint32_t metadata_hit)
{
  return metadata_in;
}

uint32_t cdp::prefetcher_cache_fill(champsim::address addr, champsim::address ip, uint32_t cpu, champsim::capability cap, bool useless, long set,
                                    long way, bool prefetch, champsim::address evicted_addr, champsim::capability evicted_cap, uint32_t metadata_in,
                                    uint32_t metadata_evict, uint32_t cpu_evict)
{
  if (way == intern_->NUM_WAY)
    return metadata_in; // the fill was dropped
  engine.do_prefetch(intern_, addr, cpu, metadata_in, CDP_FILL_L1);
  return metadata_in;
}

void cdp::prefetcher_final_stats()
{
  std::cout << "CDP FILL_L1 " << CDP_FILL_L1 << std::endl;
  engine.print_stats();
}