#include "ampm_cheri.h"

#include <algorithm>
#include <cmath>

#include "cache.h"


// ===========================================================================
// capability confidence table
// ===========================================================================
 
uint64_t ampm_cheri::capability_confidence_table::make_key(
    champsim::address ip, const champsim::capability& cap)
{
  // mix ip and cap_base; both are unique per (caller, object)
  return ampm_cheri::get_ip_hash((ip.to<uint64_t>() ^ cap.base.to<uint64_t>()) & INT64_MAX);
}
 
uint16_t ampm_cheri::capability_confidence_table::base_tag(uint64_t cap_base)
{
  // 16-bit fingerprint for cap-switch detection
  return static_cast<uint16_t>(ampm_cheri::get_ip_hash(cap_base) & 0xFFFFU);
}
 
uint8_t ampm_cheri::capability_confidence_table::conf_increment(uint8_t conf, uint8_t amount)
{
  return (static_cast<unsigned>(conf) + amount > MAX_CONFIDENCE)
             ? MAX_CONFIDENCE
             : static_cast<uint8_t>(conf + amount);
}
 
uint8_t ampm_cheri::capability_confidence_table::conf_decrement(uint8_t conf, uint8_t amount)
{
  return (conf < amount) ? 0 : static_cast<uint8_t>(conf - amount);
}
 
// linear scan for the first threshold that conf does not exceed
static inline std::size_t conf_bin(uint8_t conf, const std::array<uint8_t,16>& th)
{
  for (std::size_t i = 0; i < th.size(); i++)
    if (conf < th[i]) return i;
  return th.size() - 1;
}
 
int ampm_cheri::capability_confidence_table::conf_to_depth(uint8_t conf)
{
  return CONF_DEPTHS[conf_bin(conf, CONF_THRESH)];
}
 
int ampm_cheri::capability_confidence_table::conf_to_zone_la(uint8_t conf)
{
  return CONF_ZONE_LA[conf_bin(conf, CONF_THRESH)];
}
 
uint8_t ampm_cheri::capability_confidence_table::squash_chance(uint8_t conf)
{
  return SQUASH_CHANCE[conf_bin(conf, CONF_THRESH)];
}
 
uint8_t ampm_cheri::capability_confidence_table::update_and_query(
    champsim::address ip, champsim::address va, champsim::capability& cap)
{
  entry probe;
  probe.key = make_key(ip, cap);
  const uint64_t cap_base = cap.base.to<uint64_t>();
  const uint16_t tag      = base_tag(cap_base);
  const uint64_t cl_off   = (va.to<uint64_t>() - cap_base) >> LOG2_BLOCK_SIZE;
 
  auto e = table.check_hit(probe);
  if (e.has_value()) {
    // cap-switch: same key hash, different cap_base. reset.
    if (e->cap_base_tag != tag) {
      e->cap_base_tag        = tag;
      e->confidence          = STARTING_CONFIDENCE;
      e->issue_counter       = 0;
      e->useful_counter      = 0;
      e->direction           = STARTING_DIR_FORWARD_CTR;
      e->has_last_cl_offset  = false;
    }
 
    // direction tracking from cap-relative cl offset
    if (e->has_last_cl_offset) {
      if (cl_off > e->last_cl_offset)
        e->direction = std::min<uint8_t>(DIR_COUNTER_MAX, e->direction + 1);
      else if (cl_off < e->last_cl_offset && e->direction > 0)
        e->direction--;
    }
    e->last_cl_offset      = static_cast<uint32_t>(cl_off);
    e->has_last_cl_offset  = true;
 
    hit++;
    table.fill(e.value());
    return e->confidence;
  }
 
  // miss: install fresh entry
  miss++;
  probe.cap_base_tag       = tag;
  probe.last_cl_offset     = static_cast<uint32_t>(cl_off);
  probe.has_last_cl_offset = true;
  table.fill(probe);
  return STARTING_CONFIDENCE;
}
 
int ampm_cheri::capability_confidence_table::get_direction(
    champsim::address ip, const champsim::capability& cap)
{
  entry probe;
  probe.key = make_key(ip, cap);
  auto e = table.check_hit(probe);
  if (!e.has_value()) return 1;  // default forward
  return (e->direction >= DIR_FORWARD_THRESH) ? 1 : -1;
}
 
void ampm_cheri::capability_confidence_table::update_on_useful_pf(
    champsim::address ip, champsim::capability& cap)
{
  entry probe;
  probe.key = make_key(ip, cap);
  auto e = table.check_hit(probe);
  if (!e.has_value()) return;
 
  // bin at current conf for histogram
  useful_at_conf[conf_bin(e->confidence, CONF_THRESH)]++;
 
  e->useful_counter = static_cast<uint8_t>(e->useful_counter + USEFUL_COUNTER_INCR);
  if (e->useful_counter >= USEFUL_COUNTER_MAX) {
    e->confidence     = conf_increment(e->confidence, USEFUL_CONF_INCR);
    e->useful_counter = 0;
    promoted++;
  }
  table.fill(e.value());
}
 
void ampm_cheri::capability_confidence_table::update_on_fill(
    champsim::address ip, champsim::capability& cap)
{
  entry probe;
  probe.key = make_key(ip, cap);
  auto e = table.check_hit(probe);
  if (!e.has_value()) return;
 
  prefetches_at_conf[conf_bin(e->confidence, CONF_THRESH)]++;
 
  e->issue_counter = static_cast<uint8_t>(e->issue_counter + ISSUE_COUNTER_INCR);
  if (e->issue_counter >= ISSUE_COUNTER_MAX) {
    e->confidence    = conf_decrement(e->confidence, ISSUE_CONF_DECR);
    e->issue_counter = 0;
    demoted++;
  }
  table.fill(e.value());
}
 
// ===========================================================================
// zone walker table
// ===========================================================================
 
void ampm_cheri::zone_walker_table::log_ip(region_key_type zone_key, uint16_t ip_hash)
{
  entry probe{zone_key};
  auto e = table.check_hit(probe);
  if (e.has_value()) {
    e->ip_hashes.fill(ip_hash);
    table.fill(e.value());
  } else {
    probe.ip_hashes.fill(ip_hash);
    table.fill(probe);
  }
}
 
// returns the zone's own learned confidence. peer-IP CCT lookup is not
// directly possible here because CCT keys are (ip x cap) and we lack cap
// context; the zone's own confidence aggregates exactly this signal since
// every IP touching the zone updates it through update_on_useful_pf /
// update_on_issued_pf. peer count is tracked for stats only.
uint8_t ampm_cheri::zone_walker_table::zone_boost_confidence(
    region_key_type zone_key, uint16_t current_ip_hash,
    const capability_confidence_table& /*cct*/)
{
  entry probe{zone_key};
  auto e = table.check_hit(probe);
  if (!e.has_value()) return 0;
 
  // count peers (other IPs that have touched this zone)
  auto contents = e->ip_hashes.get_contents();
  int peers = 0;
  for (auto& h : contents)
    if (h.last_used != 0 && h.data != current_ip_hash) peers++;
 
  if (peers > 0) {
    zone_boost_eligible++;
    zone_boost_applied++;
  }
  return e->confidence;
}
 
void ampm_cheri::zone_walker_table::update_on_useful_pf(region_key_type zone_key)
{
  entry probe{zone_key};
  auto e = table.check_hit(probe);
  if (!e.has_value()) return;
 
  e->useful_counter++;
  if (e->useful_counter >= USEFUL_COUNTER_MAX) {
    e->confidence = (static_cast<unsigned>(e->confidence) + USEFUL_CONF_INCR > 255)
                        ? 255
                        : static_cast<uint8_t>(e->confidence + USEFUL_CONF_INCR);
    e->useful_counter = 0;
  }
  table.fill(e.value());
}
 
void ampm_cheri::zone_walker_table::update_on_issued_pf(region_key_type zone_key)
{
  entry probe{zone_key};
  auto e = table.check_hit(probe);
  if (!e.has_value()) return;
 
  e->issue_counter++;
  if (e->issue_counter >= ISSUE_COUNTER_MAX) {
    e->confidence = (e->confidence < ISSUE_CONF_DECR)
                        ? 0
                        : static_cast<uint8_t>(e->confidence - ISSUE_CONF_DECR);
    e->issue_counter = 0;
  }
  table.fill(e.value());
}



ampm_cheri::capability_size ampm_cheri::cap_size(uint64_t cap_length)
{
  if (cap_length <= (1ULL << 7))         return capability_size::SMALL;  // 0-128B
  if (cap_length <= (1ULL << 12))        return capability_size::MEDIUM;  // 128B-4KB
  if (cap_length <= (1ULL << 16))       return capability_size::LARGE;  // 4KB-64KB
  if (cap_length <= (1ULL << 20))     return capability_size::XLARGE;  // 64KB-1MB
  return capability_size::XXL;                                 // 1MB+
}

uint64_t ampm_cheri::log2_size(uint64_t cap_length)
{
  if (cap_length <= 1) return 0;
  auto lg2 = champsim::lg2(cap_length);
  return (lg2 > MAX_LOG2_SIZE) ? MAX_LOG2_SIZE : lg2;
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

auto ampm_cheri::zone_key_and_offset(champsim::address v_addr, const champsim::capability& cap) const  -> std::pair<region_key_type, std::size_t>
{
  uint64_t cap_base = cap.base.to<uint64_t>();
  uint64_t va_int   = v_addr.to<uint64_t>();
  uint64_t top      = cap_base + cap.length.to<uint64_t>();
 
  if (va_int < cap_base || va_int >= top)
    return {region_key_type{champsim::address{0}}, 0};

  uint64_t cap_off  = va_int - cap_base;                  // bytes into cap
  uint64_t zone_id  = cap_off >> CHERI_AMPM_ZONE_BITS;    // 4 KB zone index
  uint64_t key64    = make_zone_key(cap_base, zone_id);
 
  // cl offset within the 64-line zone
  uint64_t zone_cl  = (cap_off >> LOG2_BLOCK_SIZE) & (lines_per_zone() - 1);
 
  region_key_type key{champsim::address{key64}};
  return {key, static_cast<std::size_t>(zone_cl)};
}



void ampm_cheri::add_to_map(champsim::address v_addr, const champsim::capability& cap, bool prefetch)
{
  auto [zone_key, off] = zone_key_and_offset(v_addr, cap);
  if (zone_key.to<uint64_t>() == 0) return;    
  uint64_t cap_base = cap.base.to<uint64_t>();
 
  region_type key{zone_key};
  auto entry = regions.check_hit(key);
 
  if (entry.has_value()) {
    // collision: same hash, different cap_base. reset map
    if (entry->cap_base != cap_base) {
      zone_collision++;
      region_type new_entry{zone_key};
      new_entry.cap_base = cap_base;
      if (prefetch) new_entry.prefetch_map[off] = true;
      else          new_entry.access_map[off]   = true;
      regions.fill(new_entry);
      return;
    }
    if (prefetch) entry->prefetch_map[off] = true;
    else          entry->access_map[off]   = true;
    regions.fill(entry.value());
    return;
  }
 
  region_type new_entry{zone_key};
  new_entry.cap_base = cap_base;
  if (prefetch) new_entry.prefetch_map[off] = true;
  else          new_entry.access_map[off]   = true;
  regions.fill(new_entry);
}

bool ampm_cheri::check_map(champsim::address v_addr, const champsim::capability& cap, bool prefetch)
{
  auto [key, offset] = zone_key_and_offset(v_addr, cap);
  if (key.to<uint64_t>() == 0) return false;
  auto region = regions.check_hit(region_type{key});
  if (!region.has_value()){ region_miss++; return false;}
  if (region->cap_base != cap.base.to<uint64_t>()) return false;
  return prefetch ? region->prefetch_map.at(offset) : region->access_map.at(offset);
}


void ampm_cheri::do_prefetch(CACHE* cache, champsim::address pa,
                             champsim::address va,
                             champsim::address ip,
                             champsim::capability& cap,
                             uint32_t metadata_in, 
                             uint32_t cpu, 
                             int degree, int zone_la, int direction, bool two_level)
{

  if (direction != 1 && direction != -1) direction = 1;
 
  const uint64_t cap_base = cap.base.to<uint64_t>();
  const uint64_t va_int   = va.to<uint64_t>();
  const uint64_t pa_int   = pa.to<uint64_t>();
  const uint64_t page_mask = (1ULL << LOG2_PAGE_SIZE) - 1;
  const int64_t  curr_cl  = static_cast<int64_t>((va_int - cap_base) >> LOG2_BLOCK_SIZE);
 
  const auto cls = cap_size(cap.length.to<uint64_t>());
  const std::size_t lines_in_zone = lines_per_zone();
  const int64_t max_stride = static_cast<int64_t>((zone_la + 1) * lines_in_zone);
  const int64_t curr_zone  = curr_cl / static_cast<int64_t>(lines_in_zone);
 
  int issued = 0;
  for (int64_t k = 1; k <= max_stride && issued < degree; k++) {
    const int64_t d_k    = static_cast<int64_t>(direction) * k;
    const int64_t target = curr_cl + d_k;
    const int64_t past1  = curr_cl - d_k;
    const int64_t past2  = curr_cl - 2 * d_k;
 
    if (past1 < 0 || past2 < 0) continue;
 
    // candidate VA, cap-relative
    auto target_va = champsim::address{cap_base + (static_cast<uint64_t>(target) << LOG2_BLOCK_SIZE)};
    auto past1_va  = champsim::address{cap_base + (static_cast<uint64_t>(past1)  << LOG2_BLOCK_SIZE)};
    auto past2_va  = champsim::address{cap_base + (static_cast<uint64_t>(past2)  << LOG2_BLOCK_SIZE)};
 
    // cap bounds enforced by CHERI util (in_bounds + load perm)
    if (!cheri::prefetch_safe(target_va, cap)) {
      pf_bounded++;
      break;
    }
 
    // stop at page boundary
    if (((target_va.to<uint64_t>() ^ va_int) >> LOG2_PAGE_SIZE) != 0)
      break;
 
    // stride confirmation from access bitmap only (not our prefetch footprint)
    if (!check_map(past1_va, cap, false)) continue;
    if (!check_map(past2_va, cap, false)) continue;
 
    // already covered?
    if (check_map(target_va, cap, false) || check_map(target_va, cap, true))
      continue;
 
    // reconstruct PA from VA inside the superpage
    uint64_t pf_pa = (pa_int & ~page_mask) | (target_va.to<uint64_t>() & page_mask);
    champsim::address pf_addr{pf_pa};
 
    // CHERI-aware prefetch issue
    if (!cache->prefetch_line(pf_addr, two_level,cpu,ip, metadata_in, cap))
      break;  // mshr / pq full
 
    add_to_map(target_va, cap, true);
    pf_by_size[cls]++;
 
    int64_t target_zone = target / static_cast<int64_t>(lines_in_zone);
    if (target_zone != curr_zone) cross_zone++;
 
    issued++;
  }
 
  if (issued > 0) cct.degree_incr++;
}