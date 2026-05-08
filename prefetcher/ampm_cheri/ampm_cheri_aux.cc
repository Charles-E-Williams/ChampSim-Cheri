#include "ampm_cheri.h"

#include <algorithm>
#include <cmath>

#include "cache.h"


ampm_cheri::capability_size ampm_cheri::cap_size_category(uint64_t cap_length)
{
  if (cap_length <= (1ULL << 7))         return capability_size::SMALL;  // 0-128B
  if (cap_length <= (1ULL << 12))        return capability_size::MEDIUM;  // 128B-4KB
  if (cap_length <= (1ULL << 16))       return capability_size::LARGE;  // 4KB-64KB
  if (cap_length <= (1ULL << 20))     return capability_size::XLARGE;  // 64KB-1MB
  return capability_size::XXL;                                 // 1MB+
}

uint64_t ampm_cheri::make_zone_key(uint64_t cap_base, uint64_t cap_zone_id)
{
  uint64_t h = cap_base;
  h ^= (h >> 33);
  h *= 0xff51afd7ed558ccdULL;
  h ^= (h >> 33);
  h *= 0xc4ceb9fe1a85ec53ULL;
  h ^= (h >> 33);
  h ^= (cap_zone_id * 0x9e3779b97f4a7c15ULL);
  h ^= (h >> 16);
  if (h == 0) h = 1;
  return h & 0x7FFFFFFFFFFFFFFFULL;
}

auto ampm_cheri::zone_key_and_offset(champsim::address v_addr,  const champsim::capability& cap) const -> std::pair<region_key_type, std::size_t>
{

  if (!cheri::in_bounds(v_addr, cap.base, champsim::address{cap.length.to<uint64_t>()+cap.base.to<uint64_t>()}))
    return {region_key_type{0}, 0};

  uint64_t obj_cl = (v_addr.to<uint64_t>() - cap.base.to<uint64_t>()) >> LOG2_BLOCK_SIZE;
  uint64_t lpz    = lines_per_zone();

  // only capabilities that span multiple zones end up here
  uint64_t    cap_zone_id = obj_cl / lpz;
  std::size_t off         = static_cast<std::size_t>(obj_cl % lpz);
  return {region_key_type{make_zone_key(cap.base.to<uint64_t>(), cap_zone_id)}, off};
}


void ampm_cheri::add_to_map(champsim::address v_addr, champsim::address pa, const champsim::capability& cap, bool prefetch)
{
  auto [key, offset] = zone_key_and_offset(v_addr, cap);

  if (key.to<uint64_t>() == 0) //shouldn't end up here....
    return;

  auto region = regions.check_hit(region_type{key});
  if (region.has_value()) {

    // capability hashed to the same zone (should be rare)
    if (region->cap_base != cap.base.to<uint64_t>()) {
      zone_collision++;
      region->access_map.assign(lines_per_zone(), false);
      region->prefetch_map.assign(lines_per_zone(), false);
    }

    if (prefetch)
      region->prefetch_map.at(offset) = true;
    else
      region->access_map.at(offset) = true;

    region->cap_base = cap.base.to<uint64_t>();
    regions.fill(region.value());

  } else {
    region_type new_region{key};
    if (prefetch)
      new_region.prefetch_map.at(offset) = true;
    else
      new_region.access_map.at(offset) = true;
    new_region.cap_base = cap.base.to<uint64_t>();
    regions.fill(new_region);
  }
}

bool ampm_cheri::check_map(champsim::address v_addr, const champsim::capability& cap, bool prefetch)
{
  auto [key, offset] = zone_key_and_offset(v_addr, cap);
  auto region = regions.check_hit(region_type{key});

  if (!region.has_value()) //region miss
    return false;

  if (region->cap_base != cap.base.to<uint64_t>()) // capability base shouldn't change
    return false;

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
      uint64_t va_cand_val = va_candidate.to<uint64_t>();

      if (!cheri::prefetch_safe(va_candidate, cap)) {
        pf_bounded++;
        break;
      }

      if (((va_cand_val ^ va_val) >> LOG2_PAGE_SIZE) != 0)
        break;

      if (check_map(champsim::address{neg_step}, cap, false) &&
          check_map(champsim::address{neg_2step}, cap, false) &&
          !check_map(va_candidate, cap, false) &&
          !check_map(va_candidate, cap, true)) {

        if (va_block != champsim::block_number{pos_step}) {
          uint64_t page_mask = (1ULL << LOG2_PAGE_SIZE) - 1;

          // Reconstruct PA
          uint64_t pf_pa = (pa.to<uint64_t>() & ~page_mask) | (va_cand_val & page_mask);
          champsim::address pf_addr{pf_pa};

          if (cache->prefetch_line(pf_addr, two_level, metadata_in)) {
            add_to_map(va_candidate, pf_addr, cap, true);
            pf_count++;
          }
        }
      }
    }
  }
}