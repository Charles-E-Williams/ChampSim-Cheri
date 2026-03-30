#include "ampm_cheri.h"

#include "cache.h"


uint64_t ampm_cheri::make_zone_key(uint64_t cap_base, uint64_t cap_zone_id)
{
  uint64_t k = cap_base ^ (cap_zone_id * 0x9e3779b97f4a7c15ULL);
  k ^= (k >> 16);
  k ^= (k >> 8);
  if (k == 0) k = 1;
  return k & 0x7FFFFFFFFFFFFFFFULL;  // must fit in signed long for lru_table
}

auto ampm_cheri::zone_key_and_offset(champsim::address v_addr, const champsim::capability& cap) const -> std::pair<region_key_type, std::size_t>
{
    uint64_t va   = v_addr.to<uint64_t>();
    uint64_t base = cap.base.to<uint64_t>();
    uint64_t top = base + cap.length.to<uint64_t>();

    // out of bounds
    if (va < base || va >= top) 
        return {region_key_type{0}, 0};
    
    uint64_t obj_cl      = (va - base) >> LOG2_BLOCK_SIZE;
    uint64_t lpz         = lines_per_zone();
    uint64_t cap_zone_id = obj_cl / lpz;
    std::size_t off      = static_cast<std::size_t>(obj_cl % lpz);

  return std::make_pair(region_key_type{make_zone_key(base, cap_zone_id)}, off);
}

void ampm_cheri::add_to_map(champsim::address v_addr, champsim::address pa,
                            const champsim::capability& cap, bool prefetch)
{
  auto [key, offset] = zone_key_and_offset(v_addr, cap);

  // dont record
  if (key.to<uint64_t>() == 0) {
    fmt::print("Not recording to map\n");
    return;
  }

  auto existing = regions.check_hit(region_type{key});

  if (existing.has_value()) {
    if (prefetch) 
        existing->prefetch_map.at(offset) = true;
    else          
        existing->access_map.at(offset) = true;

    existing->cap_base = cap.base.to<uint64_t>();
    existing->cap_top  = cheri::capability_top(cap).to<uint64_t>(); 

    regions.fill(existing.value());

  } else {
    auto nr = region_type{key};

    if (prefetch) 
        nr.prefetch_map.at(offset) = true;
    else         
        nr.access_map.at(offset) = true;

    nr.cap_base = cap.base.to<uint64_t>();
    nr.cap_top  = cap.base.to<uint64_t>() + cap.length.to<uint64_t>();
    regions.fill(nr);
  }

  reverse_map.fill({pa, key, offset});
}

bool ampm_cheri::check_map(champsim::address v_addr, const champsim::capability& cap, bool prefetch)
{
  auto [key, offset] = zone_key_and_offset(v_addr, cap);
  auto region = regions.check_hit(region_type{key});

  if (!region.has_value()) return false;
  return prefetch ? region->prefetch_map.at(offset) : region->access_map.at(offset);
}

void ampm_cheri::do_prefetch(CACHE* cache, champsim::address pa, champsim::address va,
                            const champsim::capability& cap, uint32_t metadata_in,
                            int degree, bool two_level)
{
  champsim::block_number va_block{va};
  uint64_t va_val = va.to<uint64_t>();

  for (auto direction : {1, -1}) {
    for (int i = 1, pf_count = 0; i <= static_cast<int>(lines_per_zone()) && pf_count < degree; i++) {
      const auto pos_step  = va_block + (direction * i);
      const auto neg_step  = va_block - (direction * i);
      const auto neg_2step = va_block - (direction * 2 * i);

      champsim::address va_candidate{pos_step};

      if (!cheri::prefetch_safe(va_candidate, cap)) {
        stat_pf_bounded_by_cap++;
        break;
      }

      if (check_map(champsim::address{neg_step}, cap, false) &&
          check_map(champsim::address{neg_2step}, cap, false) &&
          !check_map(va_candidate, cap, false) &&
          !check_map(va_candidate, cap, true)) {

        if (va_block != champsim::block_number{pos_step}) {
          uint64_t va_cand_val = va_candidate.to<uint64_t>();

          auto pa_opt = tlb.translate(va_cand_val);
          if (pa_opt.has_value()) {
            bool same_page = ((va_cand_val ^ va_val) >> LOG2_PAGE_SIZE) == 0;
            if (!same_page)
              stat_cross_page_detected++;

            champsim::address pf_addr{pa_opt.value()};
            if (cache->prefetch_line(pf_addr, two_level, metadata_in)) {
              add_to_map(va_candidate, pf_addr, cap, true);
              pf_count++;
            }
          } else {
            stat_cross_page_detected++;
            stat_cross_page_cant_issue++;
          }
        }
      }
    }
  }
}