#include "ampm_cap.h"

#include <algorithm>
#include <cmath>

#include "cache.h"


ampm_cap::capability_size ampm_cap::cap_size_category(uint64_t cap_length)
{
  if (cap_length <= (1ULL << 7))         return capability_size::SMALL;  // 0-128B
  if (cap_length <= (1ULL << 12))        return capability_size::MEDIUM;  // 128B-4KB
  if (cap_length <= (1ULL << 16))       return capability_size::LARGE;  // 4KB-64KB
  if (cap_length <= (1ULL << 20))     return capability_size::XLARGE;  // 64KB-1MB
  return capability_size::XXL;                                 // 1MB+
}

uint64_t ampm_cap::make_zone_key(uint64_t cap_base, uint64_t cap_zone_id)
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

auto ampm_cap::zone_key_and_offset(champsim::address va,  const champsim::capability& cap) const -> std::pair<region_key_type, std::size_t>
{

  if (!cheri::in_bounds(va, cap.base, champsim::address{cap.length.to<uint64_t>()+cap.base.to<uint64_t>()}))
    return {region_key_type{0}, 0};

  uint64_t obj_cl = (va.to<uint64_t>() - cap.base.to<uint64_t>()) >> LOG2_BLOCK_SIZE;
  uint64_t lpz    = lines_per_zone();

  // only capabilities that span multiple zones end up here
  uint64_t    cap_zone_id = obj_cl / lpz;
  std::size_t off         = static_cast<std::size_t>(obj_cl % lpz);
  return {region_key_type{make_zone_key(cap.base.to<uint64_t>(), cap_zone_id)}, off};
}


void ampm_cap::add_to_map(champsim::address va, const champsim::capability& cap, bool prefetch)
{
  auto [key, offset] = zone_key_and_offset(va, cap);

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

bool ampm_cap::check_map(champsim::address va, const champsim::capability& cap, bool prefetch)
{
  auto [key, offset] = zone_key_and_offset(va, cap);
  auto region = regions.check_hit(region_type{key});

  if (!region.has_value()) //region miss
    return false;

  if (region->cap_base != cap.base.to<uint64_t>()) // capability base shouldn't change
    return false;

  return prefetch ? region->prefetch_map.at(offset) : region->access_map.at(offset);
}


void ampm_cap::do_prefetch(CACHE* cache, champsim::address pa, champsim::address va,
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

          // Candidates beyond PREFETCH_DEGREE (boost) fill the next level down
          const bool boosted = (pf_count >= PREFETCH_DEGREE);
          if (cache->prefetch_line(pf_addr, boosted ? false : two_level, metadata_in)) {
            add_to_map(va_candidate, cap, true);
            pf_count++;
            if (boosted)
              pf_boost++;
          }
        }
      }
    }
  }
}

void ampm_cap::page_do_prefetch(CACHE* cache, champsim::address addr, std::optional<champsim::address> va_line, const champsim::capability& cap,
                                cheri_policy::OobConfidence::key oob_key, uint32_t metadata_in, int degree, bool two_level)
{
  champsim::block_number block_addr{addr};

  // attempt to prefetch in the positive, then negative direction
  for (auto direction : {1, -1}) {
    for (int i = 1, prefetches_issued = 0; prefetches_issued < degree; i++) {
      const auto pos_step_addr = block_addr + (direction * i);
      const auto neg_step_addr = block_addr - (direction * i);
      const auto neg_2step_addr = block_addr - (direction * 2 * i);

      // goes off physical page
      if (ampm::AMPM_Module::page{pos_step_addr}.to<uint64_t>() != ampm::AMPM_Module::page{addr}.to<uint64_t>())
        break;

      if (page_engine.check_pagemap(champsim::address{neg_step_addr}, false) && page_engine.check_pagemap(champsim::address{neg_2step_addr}, false)
          && !page_engine.check_pagemap(champsim::address{pos_step_addr}, false) && !page_engine.check_pagemap(champsim::address{pos_step_addr}, true)) {
        // found something that we should prefetch
        if (block_addr != champsim::block_number{pos_step_addr}) {
          champsim::address pf_addr{pos_step_addr};
          bool fill_this_level = two_level;

          // Out-of-object candidate: the learned OOB confidence decides (issue here, one level down, or drop)
          bool oob_cand = false;
          auto act = cheri_policy::oob_action::ISSUE_HERE;
          if (va_line.has_value()) {
            const champsim::block_number pf_va_block = champsim::block_number{*va_line} + (direction * i);
            oob_cand = !cheri::overlaps_bounds(pf_va_block, cap.base, cheri::capability_top(cap));
          }
          if (oob_cand) {
            act = oob.decide(oob_key);
            if (act == cheri_policy::oob_action::DROP) {
              oob.record(champsim::block_number{pf_addr}.to<uint64_t>(), oob_key, act);
              continue;
            }
            if (act == cheri_policy::oob_action::ISSUE_LOWER)
              fill_this_level = false;
          }

          if (cache->prefetch_line(pf_addr, fill_this_level, metadata_in)) {
            page_engine.add_to_pagemap(champsim::address{pos_step_addr}, true);
            prefetches_issued++;
            if (oob_cand)
              oob.record(champsim::block_number{pf_addr}.to<uint64_t>(), oob_key, act);
          }
        }
      }
    }
  }
}
