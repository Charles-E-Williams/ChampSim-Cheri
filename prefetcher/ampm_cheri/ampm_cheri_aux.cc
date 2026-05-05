#include "ampm_cheri.h"

#include <algorithm>
#include <cmath>

#include "cache.h"



int ampm_cheri::cap_size_class(uint64_t cap_length)
{
  if (cap_length <= 128)         return 0;  // 0-128B
  if (cap_length <= 4096)        return 1;  // 128B-4KB
  if (cap_length <= 65536)       return 2;  // 4KB-64KB
  if (cap_length <= 1048576)     return 3;  // 64KB-1MB
  return 4;                                 // 1MB+
}

uint64_t ampm_cheri::log2_size_class(uint64_t cap_length)
{
  if (cap_length <= 1) return 0;
  uint64_t sc = 63 - static_cast<uint64_t>(__builtin_clzll(cap_length));
  return (sc > 31) ? 31 : sc;
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

auto ampm_cheri::zone_key_and_offset(champsim::address v_addr,
                                     const champsim::capability& cap) const
    -> std::pair<region_key_type, std::size_t>
{
  uint64_t va   = v_addr.to<uint64_t>();
  uint64_t base = cap.base.to<uint64_t>();
  uint64_t top  = base + cap.length.to<uint64_t>();

  if (va < base || va >= top)
    return {region_key_type{0}, 0};

  uint64_t obj_cl = (va - base) >> LOG2_BLOCK_SIZE;
  uint64_t lpz    = lines_per_zone();

  // Only caps > SMALL_CAP_THRESHOLD reach here, so always multi-zone.
  uint64_t    cap_zone_id = obj_cl / lpz;
  std::size_t off         = static_cast<std::size_t>(obj_cl % lpz);
  return {region_key_type{make_zone_key(base, cap_zone_id)}, off};
}



void ampm_cheri::add_to_map(champsim::address v_addr, champsim::address pa,
                            const champsim::capability& cap, bool prefetch)
{
  auto [key, offset] = zone_key_and_offset(v_addr, cap);
  if (key.to<uint64_t>() == 0)
    return;

  auto existing = regions.check_hit(region_type{key});
  if (existing.has_value()) {

    // Collision: different object landed on the same hash slot
    if (existing->cap_base != cap.base.to<uint64_t>()) {
      stat_zone_collision++;
      existing->access_map.assign(lines_per_zone(), false);
      existing->prefetch_map.assign(lines_per_zone(), false);
    }

    if (prefetch)
      existing->prefetch_map.at(offset) = true;
    else
      existing->access_map.at(offset) = true;

    existing->cap_base = cap.base.to<uint64_t>();
    regions.fill(existing.value());

  } else {
    region_type r{key};
    if (prefetch)
      r.prefetch_map.at(offset) = true;
    else
      r.access_map.at(offset) = true;
    r.cap_base = cap.base.to<uint64_t>();
    regions.fill(r);
  }
}

bool ampm_cheri::check_map(champsim::address v_addr,
                           const champsim::capability& cap, bool prefetch)
{
  auto [key, offset] = zone_key_and_offset(v_addr, cap);
  auto region = regions.check_hit(region_type{key});
  if (!region.has_value()) return false;
  if (region->cap_base != cap.base.to<uint64_t>()) return false;
  return prefetch ? region->prefetch_map.at(offset)
                  : region->access_map.at(offset);
}


auto ampm_cheri::page_zone_key_and_offset(champsim::address pa) const
    -> std::pair<region_key_type, std::size_t>
{
  uint64_t pa_val = pa.to<uint64_t>();
  uint64_t zone   = pa_val >> ZONE_BITS;
  std::size_t off = (pa_val >> LOG2_BLOCK_SIZE) & (lines_per_zone() - 1);

  // Use PA zone directly as key (ensure non-zero, mask to 63 bits)
  uint64_t key = zone & 0x7FFFFFFFFFFFFFFFULL;
  if (key == 0) key = 1;
  return {region_key_type{key}, off};
}

void ampm_cheri::page_add_to_map(champsim::address pa, bool prefetch)
{
  auto [key, offset] = page_zone_key_and_offset(pa);
  auto existing = page_regions.check_hit(page_region_type{key});

  if (existing.has_value()) {
    if (prefetch)
      existing->prefetch_map.at(offset) = true;
    else
      existing->access_map.at(offset) = true;
    page_regions.fill(existing.value());
  } else {
    page_region_type r{key};
    if (prefetch)
      r.prefetch_map.at(offset) = true;
    else
      r.access_map.at(offset) = true;
    page_regions.fill(r);
  }
}

bool ampm_cheri::page_check_map(champsim::address pa, bool prefetch)
{
  auto [key, offset] = page_zone_key_and_offset(pa);
  auto region = page_regions.check_hit(page_region_type{key});
  if (!region.has_value()) return false;
  return prefetch ? region->prefetch_map.at(offset)
                  : region->access_map.at(offset);
}

void ampm_cheri::page_do_prefetch(CACHE* cache, champsim::address pa,
                                   uint32_t metadata_in, int degree,
                                   bool two_level, int size_cls)
{
  champsim::block_number pa_block{pa};
  uint64_t pa_val = pa.to<uint64_t>();
  int max_reach   = static_cast<int>(lines_per_zone());

  for (auto direction : {1, -1}) {
    for (int i = 1, pf_count = 0; i <= max_reach && pf_count < degree; i++) {
      const auto pos_step  = pa_block + (direction * i);
      const auto neg_step  = pa_block - (direction * i);
      const auto neg_2step = pa_block - (direction * 2 * i);

      champsim::address pa_candidate{pos_step};
      uint64_t pa_cand_val = pa_candidate.to<uint64_t>();

      // Stay within 2MB superpage
      if (((pa_cand_val ^ pa_val) >> LOG2_PAGE_SIZE) != 0)
        break;

      // AMPM stride confirmation: access[t-k] && access[t-2k]
      if (!page_check_map(champsim::address{neg_step}, false))
        continue;
      if (!page_check_map(champsim::address{neg_2step}, false))
        continue;

      // Target must not already be accessed or prefetched
      if (page_check_map(pa_candidate, false) ||
          page_check_map(pa_candidate, true))
        continue;

      if (cache->prefetch_line(pa_candidate, two_level, metadata_in)) {
        page_add_to_map(pa_candidate, true);
        pf_count++;
        stat_page_pf++;
        stat_pf_by_class[size_cls]++;
      }
    }
  }
}


uint64_t ampm_cheri::stride_hint_key(champsim::address ip,
                                     const champsim::capability& cap)
{
  uint64_t sc = log2_size_class(cap.length.to<uint64_t>());
  uint64_t ip_val = ip.to<uint64_t>();
  return ((ip_val * 0x517cc1b727220a95ULL) ^ (sc * 0x9e3779b97f4a7c15ULL))
         & 0x7FFFFFFFFFFFFFFFULL;
}

uint16_t ampm_cheri::base_tag(uint64_t cap_base)
{
  uint64_t h = cap_base * 0x517cc1b727220a95ULL;
  return static_cast<uint16_t>((h >> 32) ^ h);
}

int ampm_cheri::update_and_query_degree(champsim::address ip,
                                        champsim::address va,
                                        const champsim::capability& cap)
{
  uint64_t sht_key      = stride_hint_key(ip, cap);
  uint64_t cap_base_val = cap.base.to<uint64_t>();
  int32_t  current_cl   = static_cast<int32_t>(
      (va.to<uint64_t>() - cap_base_val) >> LOG2_BLOCK_SIZE);
  uint16_t btag         = base_tag(cap_base_val);

  stride_hint_entry probe{};
  probe.key = sht_key;
  auto found = sht.check_hit(probe);

  if (found.has_value()) {
    stat_sht_hit++;
    stride_hint_entry entry = *found;

    if (entry.cap_base_tag == btag) {
      int16_t stride = static_cast<int16_t>(current_cl - entry.last_cl_offset);
      if (stride != 0) {
        if (stride == entry.predicted_stride) {
          stat_sht_confirmed++;
          if (entry.confidence < SHT_CONF_MAX)
            entry.confidence++;
        } else {
          if (entry.confidence > 0)
            entry.confidence--;
          if (entry.confidence == 0)
            entry.predicted_stride = stride;
        }
      }
    } else {
      // Different object, same (IP, size_class): keep prediction, reset tracking
      entry.cap_base_tag = btag;
    }

    entry.last_cl_offset = current_cl;
    sht.fill(entry);

    if (entry.confidence >= SHT_CONF_BOOST && entry.predicted_stride != 0) {
      stat_degree_boosted++;
      int direction = (entry.predicted_stride > 0) ? 1 : -1;
      int64_t remaining = cheri::remaining_lines(
          champsim::block_number{va}, direction,
          cap.base, cheri::capability_top(cap));
      int abs_stride = std::abs(static_cast<int>(entry.predicted_stride));
      int max_useful = (abs_stride > 0)
          ? static_cast<int>(remaining / abs_stride) : 0;
      return std::clamp(max_useful, 1, MAX_DEGREE);
    }
    return BASE_DEGREE;

  } else {
    stat_sht_miss++;
    stride_hint_entry entry{};
    entry.key              = sht_key;
    entry.last_cl_offset   = current_cl;
    entry.cap_base_tag     = btag;
    entry.predicted_stride = 0;
    entry.confidence       = 0;
    sht.fill(entry);
    return BASE_DEGREE;
  }
}

void ampm_cheri::sht_reward(champsim::address ip,
                            const champsim::capability& cap)
{
  uint64_t sht_key = stride_hint_key(ip, cap);
  stride_hint_entry probe{};
  probe.key = sht_key;
  auto found = sht.check_hit(probe);
  if (found.has_value() && found->confidence < SHT_CONF_MAX) {
    found->confidence++;
    sht.fill(*found);
    stat_sht_rewarded++;
  }
}



void ampm_cheri::do_prefetch(CACHE* cache, champsim::address pa,
                             champsim::address va,
                             const champsim::capability& cap,
                             uint32_t metadata_in,
                             int degree, bool two_level)
{
  champsim::block_number va_block{va};
  uint64_t va_val       = va.to<uint64_t>();
  uint64_t cap_base_val = cap.base.to<uint64_t>();
  int max_reach = static_cast<int>(CROSS_ZONE_FACTOR * lines_per_zone());
  int cls = cap_size_class(cap.length.to<uint64_t>());

  uint64_t trigger_zone_id = ((va_val - cap_base_val) >> LOG2_BLOCK_SIZE)
                             / lines_per_zone();

  for (auto direction : {1, -1}) {
    for (int i = 1, pf_count = 0; i <= max_reach && pf_count < degree; i++) {
      const auto pos_step  = va_block + (direction * i);
      const auto neg_step  = va_block - (direction * i);
      const auto neg_2step = va_block - (direction * 2 * i);

      champsim::address va_candidate{pos_step};
      uint64_t va_cand_val = va_candidate.to<uint64_t>();

      // Cap bounds (VA)
      if (!cheri::prefetch_safe(va_candidate, cap)) {
        stat_pf_bounded++;
        break;
      }

      // 2MB superpage boundary (PA contiguity)
      if (((va_cand_val ^ va_val) >> LOG2_PAGE_SIZE) != 0)
        break;

      // Stride confirmation from bitmap
      if (!check_map(champsim::address{neg_step}, cap, false))
        continue;
      if (!check_map(champsim::address{neg_2step}, cap, false))
        continue;

      // Target must not already be covered
      if (check_map(va_candidate, cap, false) ||
          check_map(va_candidate, cap, true))
        continue;

      // Reconstruct PA within 2MB superpage
      uint64_t page_mask = (1ULL << LOG2_PAGE_SIZE) - 1;
      uint64_t pf_pa = (pa.to<uint64_t>() & ~page_mask)
                      | (va_cand_val & page_mask);
      champsim::address pf_addr{pf_pa};

      if (cache->prefetch_line(pf_addr, two_level, metadata_in)) {
        add_to_map(va_candidate, pf_addr, cap, true);
        pf_count++;
        stat_pf_by_class[cls]++;

        // Cross-zone stat
        uint64_t target_zone_id =
            ((va_cand_val - cap_base_val) >> LOG2_BLOCK_SIZE)
            / lines_per_zone();
        if (target_zone_id != trigger_zone_id)
          stat_cross_zone++;
      }
    }
  }
}