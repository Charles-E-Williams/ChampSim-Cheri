#include "ampm_span.h"

#include <algorithm>

#include "cache.h"

uint64_t ampm_span::make_zone_key(uint64_t cap_base, uint64_t cap_zone_id)
{
  uint64_t h = cap_base;
  h ^= (h >> 33);
  h *= 0xff51afd7ed558ccdULL;
  h ^= (h >> 33);
  h *= 0xc4ceb9fe1a85ec53ULL;
  h ^= (h >> 33);
  h ^= (cap_zone_id * 0x9e3779b97f4a7c15ULL);
  h ^= (h >> 16);
  if (h == 0)
    h = 1;
  return h & 0x7FFFFFFFFFFFFFFFULL;
}

auto ampm_span::zone_key_and_offset(champsim::address va, const champsim::capability& cap) const -> std::pair<region_key_type, std::size_t>
{
  if (!cheri::in_bounds(va, cap.base, champsim::address{cap.length.to<uint64_t>() + cap.base.to<uint64_t>()}))
    return {region_key_type{0}, 0};

  uint64_t obj_cl = (va.to<uint64_t>() - cap.base.to<uint64_t>()) >> LOG2_BLOCK_SIZE;
  uint64_t lpz = lines_per_zone();

  // only capabilities that span multiple zones end up here
  uint64_t cap_zone_id = obj_cl / lpz;
  std::size_t off = static_cast<std::size_t>(obj_cl % lpz);
  return {region_key_type{make_zone_key(cap.base.to<uint64_t>(), cap_zone_id)}, off};
}

void ampm_span::add_to_map(champsim::address va, const champsim::capability& cap, bool prefetch)
{
  auto [key, offset] = zone_key_and_offset(va, cap);

  if (key.to<uint64_t>() == 0) // shouldn't end up here....
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

bool ampm_span::check_map(champsim::address va, const champsim::capability& cap, bool prefetch)
{
  auto [key, offset] = zone_key_and_offset(va, cap);
  auto region = regions.check_hit(region_type{key});

  if (!region.has_value()) // region miss
    return false;

  if (region->cap_base != cap.base.to<uint64_t>()) // capability base shouldn't change
    return false;

  return prefetch ? region->prefetch_map.at(offset) : region->access_map.at(offset);
}

// Object zones: AMPM-CHERI's do_prefetch
void ampm_span::do_prefetch(CACHE* cache, champsim::address pa, champsim::address va, const champsim::capability& cap, uint32_t metadata_in, int degree,
                            bool two_level)
{
  champsim::block_number va_block{va};
  uint64_t va_val = va.to<uint64_t>();

  for (auto direction : {1, -1}) {
    for (int i = 1, pf_count = 0; i <= static_cast<int>(lines_per_zone()) && pf_count < degree; i++) {
      const auto pos_step = va_block + (direction * i);
      const auto neg_step = va_block - (direction * i);
      const auto neg_2step = va_block - (direction * 2 * i);

      champsim::address va_candidate{pos_step};
      uint64_t va_cand_val = va_candidate.to<uint64_t>();

      if (!cheri::prefetch_safe(va_candidate, cap)) {
        pf_bounded++;
        break;
      }

      if (((va_cand_val ^ va_val) >> LOG2_PAGE_SIZE) != 0)
        break;

      if (check_map(champsim::address{neg_step}, cap, false) && check_map(champsim::address{neg_2step}, cap, false) && !check_map(va_candidate, cap, false)
          && !check_map(va_candidate, cap, true)) {
        if (va_block != champsim::block_number{pos_step}) {
          uint64_t page_mask = (1ULL << LOG2_PAGE_SIZE) - 1;

          // Reconstruct PA
          uint64_t pf_pa = (pa.to<uint64_t>() & ~page_mask) | (va_cand_val & page_mask);
          champsim::address pf_addr{pf_pa};

          if (cache->prefetch_line(pf_addr, two_level, metadata_in)) {
            add_to_map(va_candidate, cap, true);
            pf_count++;
          }
        }
      }
    }
  }
}

// Page zones: baseline AMPM's do_prefetch on page_engine's maps. A candidate may leave the 4KB zone (at most one zone
// ahead, never off the OS page) when it is inside the trigger's object or along its allocation run. Other out-of-object
// candidates go through the gate.
void ampm_span::page_do_prefetch(CACHE* cache, champsim::address addr, std::optional<champsim::address> va_line, const champsim::capability& cap, int run_dir,
                                 uint32_t metadata_in, bool two_level)
{
  champsim::block_number block_addr{addr};
  const unsigned key = gate_key(cap);

  for (auto direction : {1, -1}) {
    for (int i = 1, prefetches_issued = 0; prefetches_issued < PREFETCH_DEGREE; i++) {
      const auto pos_step_addr = block_addr + (direction * i);
      const auto neg_step_addr = block_addr - (direction * i);
      const auto neg_2step_addr = block_addr - (direction * 2 * i);

      const bool beyond_zone = ampm::AMPM_Module::page{pos_step_addr}.to<uint64_t>() != ampm::AMPM_Module::page{addr}.to<uint64_t>();
      if constexpr (!SPAN_ENABLED && !CROSS_ALL) {
        if (beyond_zone) // goes off the 4KB zone
          break;
      } else if (i > static_cast<int>(lines_per_zone()) || ((champsim::address{pos_step_addr}.to<uint64_t>() ^ addr.to<uint64_t>()) >> LOG2_PAGE_SIZE) != 0) {
        break; // at most one zone of lookahead, never off the OS page
      }

      if (!page_engine.check_pagemap(champsim::address{neg_step_addr}, false) || !page_engine.check_pagemap(champsim::address{neg_2step_addr}, false)
          || page_engine.check_pagemap(champsim::address{pos_step_addr}, false) || page_engine.check_pagemap(champsim::address{pos_step_addr}, true)
          || block_addr == champsim::block_number{pos_step_addr})
        continue;

      bool fill_this_level = two_level;
      bool gated = false;
      auto act = gate_action::ISSUE_HERE;
      if constexpr (SPAN_ENABLED || GATE_ENABLED || CROSS_ALL) {
        const bool in_obj = va_line.has_value() && cheri::overlaps_bounds(champsim::block_number{*va_line} + (direction * i), cap.base, cheri::capability_top(cap));
        const bool along_run = SPAN_ENABLED && run_dir == direction;
        if (!in_obj && !along_run) {
          if constexpr (GATE_ENABLED) {
            gated = true;
            act = gate_decide(key);
            if (act == gate_action::DROP) {
              gate_record(champsim::block_number{pos_step_addr}.to<uint64_t>(), key, act);
              continue;
            }
            fill_this_level = fill_this_level && act == gate_action::ISSUE_HERE;
          } else if (beyond_zone && !CROSS_ALL) {
            span_blocked++;
            continue; // without the gate, only object and run continuations leave the zone
          }
        }
        if (beyond_zone)
          (in_obj ? span_in_object : along_run ? span_along_run : gated ? span_gated : span_unchecked)++;
      }

      champsim::address pf_addr{pos_step_addr};
      if (cache->prefetch_line(pf_addr, fill_this_level, metadata_in)) {
        page_engine.add_to_pagemap(pf_addr, true);
        prefetches_issued++;
        if (gated)
          gate_record(champsim::block_number{pf_addr}.to<uint64_t>(), key, act);
      }
    }
  }
}

// An object continues a run when its base is within RUN_SLACK bytes above a recent object's top (direction +1), or its
// top within RUN_SLACK bytes below a recent object's base (-1), and both have the same permissions. A 2-bit counter
// with hysteresis needs RUN_MIN steps in one direction; an out-of-order pair weakens the run instead of reversing it.
int ampm_span::run_update(const champsim::capability& cap)
{
  const uint64_t b = cap.base.to<uint64_t>();
  const uint64_t t = b + cap.length.to<uint64_t>();
  run_entry* victim = &runs[0];
  for (auto& r : runs) {
    if (r.valid && r.perms == cap.permissions) {
      if (b == r.base) {
        r.lru = ++run_clock;
        return r.count >= RUN_MIN ? r.dir : 0;
      }
      const int dir = (b >= r.top && b - r.top < RUN_SLACK) ? 1 : (r.base >= t && r.base - t < RUN_SLACK) ? -1 : 0;
      if (dir != 0) {
        if (dir == r.dir) {
          r.count = std::min(r.count + 1, 3);
        } else if (r.count > 0) {
          r.count--;
        } else {
          r.dir = dir;
          r.count = 1;
        }
        r.base = b;
        r.top = t;
        r.lru = ++run_clock;
        return r.count >= RUN_MIN ? r.dir : 0;
      }
    }
    if (!r.valid || (victim->valid && r.lru < victim->lru))
      victim = &r;
  }
  *victim = run_entry{b, t, cap.permissions, 0, 0, ++run_clock, true};
  return 0;
}

unsigned ampm_span::gate_key(const champsim::capability& cap)
{
  unsigned kind = 0; // RW: heap, stack, writable globals
  if (cheri::has_execute_permissions(cap.permissions))
    kind = 2; // X: PCC-derived
  else if (!cheri::has_store_permissions(cap.permissions))
    kind = 1; // RO: rodata, relro

  const uint64_t len = cap.length.to<uint64_t>();
  unsigned cls = 3;
  if (len <= (uint64_t{1} << 7))
    cls = 0;
  else if (len <= (uint64_t{1} << 12))
    cls = 1;
  else if (len <= (uint64_t{1} << 16))
    cls = 2;

  return kind * NUM_CLASSES + cls;
}

ampm_span::gate_action ampm_span::gate_decide(unsigned key) const
{
  const int c = gate_ctr[key];
  if (c >= GATE_HIGH)
    return gate_action::ISSUE_HERE;
  if (c >= GATE_LOW)
    return gate_action::ISSUE_LOWER;
  return gate_action::DROP;
}

// Track an out-of-object candidate and the action taken. A DROP is tracked as a shadow entry; re-recording a tracked
// line only upgrades shadow -> issued.
void ampm_span::gate_record(uint64_t line, unsigned key, gate_action act)
{
  auto& set = gate_tracker[line % GATE_SETS];
  const auto tag = static_cast<uint16_t>((line / GATE_SETS) & ((uint64_t{1} << GATE_TAG_BITS) - 1));
  const bool shadow = (act == gate_action::DROP);

  for (auto& e : set) {
    if (e.valid && e.tag == tag) {
      e.shadow = e.shadow && shadow;
      return;
    }
  }

  if (shadow)
    gate_stats[key].dropped++;
  else
    gate_stats[key].issued++;

  auto* victim = &set[0];
  for (auto& e : set) {
    if (!e.valid) {
      victim = &e;
      break;
    }
    if (e.lru < victim->lru)
      victim = &e;
  }
  if (victim->valid)
    gate_update(victim->key, -GATE_DEC);
  *victim = gate_entry{tag, static_cast<uint8_t>(key), shadow, true, ++gate_clock};
}

// A demand access to a tracked line credits and retires it
void ampm_span::gate_on_demand(uint64_t line)
{
  auto& set = gate_tracker[line % GATE_SETS];
  const auto tag = static_cast<uint16_t>((line / GATE_SETS) & ((uint64_t{1} << GATE_TAG_BITS) - 1));
  for (auto& e : set) {
    if (e.valid && e.tag == tag) {
      gate_update(e.key, GATE_INC);
      if (e.shadow)
        gate_stats[e.key].used_shadow++;
      else
        gate_stats[e.key].used_issued++;
      e.valid = false;
      return;
    }
  }
}

void ampm_span::gate_update(unsigned key, int d) { gate_ctr[key] = std::clamp(gate_ctr[key] + d, 0, GATE_CTR_MAX); }
