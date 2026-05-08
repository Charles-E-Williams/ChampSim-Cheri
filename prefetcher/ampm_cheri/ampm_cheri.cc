#include "ampm_cheri.h"


#include "cache.h"

void ampm_cheri::prefetcher_initialize()
{
  
  std::cout << "AMPM-CHERI Prefetcher\n"
            << "  CHERI-AMPM ZONE BITS           = " << CHERI_AMPM_ZONE_BITS << "\n"
            << "  CACHELINES PER ZONE      = " << lines_per_zone() << "\n"
            << "  MINIMUM CACHE LINES = " <<  MIN_CAP_CACHE_LINES << " CL\n"
            << "  REGION_SETS         = " << REGION_SETS << "\n"
            << "  REGION_WAYS         = " << REGION_WAYS << "\n"
            << "  CAPABILITY CONFIDENCE TABLE       = " << cct.SETS << "x" << cct.WAYS << "\n"
            << "  ZONE WALKER TABLE      = " << zwt.SETS << "x" << zwt.WAYS << "\n";
}


uint32_t ampm_cheri::prefetcher_cache_operate(champsim::address addr,
                                              champsim::address vaddr,
                                              champsim::address ip,
                                              uint32_t cpu,
                                              champsim::capability cap,
                                              bool cache_hit,
                                              bool useful_prefetch,
                                              access_type type,
                                              uint32_t metadata_in,
                                              uint32_t metadata_hit)
{

  if (!cheri::is_tag_valid(cap))
    return metadata_in;


  uint64_t cap_len   = cap.length.to<uint64_t>();
  uint64_t cap_lines = cap_len >> LOG2_BLOCK_SIZE;
  auto cls = cap_size(cap_len);
  access_by_size[cls]++;

  if (!cache_hit) {
    if (type == access_type::PREFETCH) {
      prefetch_sample_table.fill(sample_table_entry{addr, intern_->current_cycle(), static_cast<uint8_t>(cls)});
    } else if (type == access_type::LOAD) {
      if (!prefetch_sample_table.check_hit(sample_table_entry{addr, 0, 0}).has_value()) {
        demand_sample_table.fill(sample_table_entry{addr, intern_->current_cycle(), static_cast<uint8_t>(cls)});
      }
    }
  }
  if (cap_lines < MIN_CAP_CACHE_LINES){
    return metadata_in;
  }

  champsim::address va = cheri::capability_cursor(cap);


  auto [zone_key, zone_offset] = zone_key_and_offset(vaddr, cap);  


  if (useful_prefetch) {
    useful_by_size[cls]++;
    cct.update_on_useful_pf(ip, cap);
    zwt.update_on_useful_pf(zone_key);
    global_useful_epoch++;
  }

  add_to_map(vaddr, cap, false);

  uint16_t ip_hash = get_ip_hash(ip.to<uint64_t>());
  zwt.log_ip(zone_key, ip_hash);

  uint8_t cct_conf = cct.update_and_query(ip, vaddr, cap);
  uint8_t boost_conf = zwt.zone_boost_confidence(zone_key, ip_hash, cct);
  uint8_t raw_confidence = std::max(cct_conf, boost_conf);
  uint8_t eff_conf = raw_confidence > global_conf_modifier ? raw_confidence - global_conf_modifier : 0;

  uint8_t squash = 0;// cct.squash_chance(eff_conf);
  if ((intern_->current_cycle() % GLOBAL_SQUASH_MOD_MAX) < squash){
    cct.squashed++;
    return metadata_in;
  }
  
  int space_in_mshr = std::max(0,(int)intern_->get_mshr_size() - (int)intern_->get_mshr_occupancy() - (int)intern_->get_pq_occupancy().back());
  int degree    = std::min(cct.conf_to_depth(eff_conf), space_in_mshr);
  bool two_level = intern_->get_mshr_occupancy_ratio() < 0.5;
  if (degree > 0)
    do_prefetch(intern_, addr, vaddr, ip, cap, metadata_in, cpu,
      degree, cct.conf_to_zone_la(eff_conf),cct.get_direction(ip, cap), two_level);

  watchdog_counter = 0;
  return metadata_in;
}



uint32_t ampm_cheri::prefetcher_cache_fill(champsim::address addr,
                                           champsim::address vaddr,
                                           champsim::address ip,
                                           uint32_t cpu,
                                           champsim::capability cap,
                                           bool useless,
                                           long set, long way,
                                           bool prefetch,
                                           champsim::address evicted_addr,
                                           champsim::capability evicted_cap,
                                           uint32_t metadata_in,
                                           uint32_t metadata_evict,
                                           uint32_t cpu_evict)
{
  // if(!intern_->warmup && prefetch) {
  //   std::cout << "PREFETCHER_CACHE_FILL(): IP =" <<ip.to<uint64_t>() << "\n";
  //   std::cout << "PREFETCHER_CACHE_FILL(): CPU =" << cpu << "\n";
  //   std::cout << "PREFETCHER_CACHE_FILL() EVICTED CAP:" << evicted_cap.length << "\n";
  //   std::cout << "PREFETCHER_CACHE_FILL() CAP:" << cap.length << "\n";
  //   assert(ip.to<uint64_t>());
  // }


  // clear evicted line out of its zone bitmap. evicted_addr is PA;
  // zone keying is cap-relative VA, so derive the VA from the cap cursor.
  if (evicted_cap.tag) {
    champsim::address evicted_va = cheri::capability_cursor(evicted_cap);
    auto [evz_key, evz_off] = zone_key_and_offset(evicted_va, evicted_cap);
    if (evz_key.to<uint64_t>() != 0) {
      region_type probe{evz_key};
      auto re = regions.check_hit(probe);
      if (re.has_value() && re->cap_base == evicted_cap.base.to<uint64_t>()) {
        re->access_map[evz_off]   = false;
        re->prefetch_map[evz_off] = false;
        regions.fill(re.value());
      }
    }
  }
 
  // a prefetch fill: penalize CCT and ZWT, bump epoch
  if (prefetch && cheri::is_tag_valid(cap)) {
    cct.update_on_fill(ip, cap);
    auto [fz_key, fz_off] = zone_key_and_offset(vaddr, cap);
    (void)fz_off;
    if (fz_key.to<uint64_t>() != 0)
      zwt.update_on_issued_pf(fz_key);
    global_epoch_counter++;
  }
 
  // useless eviction: a prefetched line that was never demanded
  if (useless)
    global_useless_epoch++;
 
  // latency sample resolution
  if (prefetch) {
    auto e = prefetch_sample_table.check_hit(sample_table_entry{addr, 0, 0});
    if (e.has_value()) {
      if (evicted_addr != champsim::address{}) {
        prefetch_latency_cycles_epoch += intern_->current_cycle() - e->cycle_missed;
        prefetch_sampled_epoch++;
      }
      prefetch_sample_table.invalidate(sample_table_entry{addr, 0, 0});
    }
  } else {
    auto e = demand_sample_table.check_hit(sample_table_entry{addr, 0, 0});
    if (e.has_value()) {
      if (evicted_addr != champsim::address{}) {
        demand_latency_cycles_epoch += intern_->current_cycle() - e->cycle_missed;
        demand_sampled_epoch++;
      }
      demand_sample_table.invalidate(sample_table_entry{addr, 0, 0});
    }
  }
 
  return metadata_in;
}

void ampm_cheri::prefetcher_cycle_operate()
{
  // watchdog: relax global modifier if prefetcher is idle
  watchdog_counter++;
  if (watchdog_counter > WATCHDOG_INTERVAL) {
    global_conf_modifier = (global_conf_modifier > GLOBAL_CONF_DECR)
                               ? static_cast<uint8_t>(global_conf_modifier - GLOBAL_CONF_DECR)
                               : 0;
    watchdog_counter = 0;
  }
 
  // accuracy epoch (every GLOBAL_EPOCH_LENGTH prefetch fills)
  if (global_epoch_counter >= GLOBAL_EPOCH_LENGTH) {
    uint64_t total = global_useful_epoch + global_useless_epoch;
    if (total > 0) {
      global_usefulness = static_cast<double>(global_useful_epoch) / static_cast<double>(total);
      if (global_usefulness < GLOBAL_TARGET_ACCURACY) {
        unsigned next = static_cast<unsigned>(global_conf_modifier) + GLOBAL_CONF_INCR;
        global_conf_modifier = (next > 255u) ? 255u : static_cast<uint8_t>(next);
      } else {
        global_conf_modifier = (global_conf_modifier > GLOBAL_CONF_DECR)
                                   ? static_cast<uint8_t>(global_conf_modifier - GLOBAL_CONF_DECR)
                                   : 0;
      }
    }
    global_epoch_counter = 0;
    global_useful_epoch  = 0;
    global_useless_epoch = 0;
  }
 
  // latency epoch: tighten if prefetches are tracking demand latency closely
  latency_epoch_cycle_counter++;
  if (latency_epoch_cycle_counter >= LATENCY_EPOCH_CYCLES) {
    if (demand_sampled_epoch >= LATENCY_MIN_SAMPLES &&
        prefetch_sampled_epoch >= LATENCY_MIN_SAMPLES) {
      double demand_lat   = static_cast<double>(demand_latency_cycles_epoch)
                                / static_cast<double>(demand_sampled_epoch);
      double prefetch_lat = static_cast<double>(prefetch_latency_cycles_epoch)
                                / static_cast<double>(prefetch_sampled_epoch);
      if (prefetch_lat > LATENCY_RATIO_THRESH * demand_lat) {
        unsigned next = static_cast<unsigned>(global_conf_modifier) + GLOBAL_CONF_INCR;
        global_conf_modifier = (next > 255u) ? 255u : static_cast<uint8_t>(next);
      }
    }
    prefetch_latency_cycles_epoch = 0;
    prefetch_sampled_epoch        = 0;
    demand_latency_cycles_epoch   = 0;
    demand_sampled_epoch          = 0;
    latency_epoch_cycle_counter   = 0;
  }
}


void ampm_cheri::prefetcher_final_stats()
{
  std::cout << "\n=== AMPM-CHERI Final Stats ===\n"
            << "  Bounded by cap:        " << pf_bounded << "\n"
            << "  Zone hash collisions:  " << zone_collision << "\n"
            << "  Cross-zone prefetches: " << cross_zone << "\n"
             << " REGION MISSES: " << region_miss << "\n"
            << "  Cursor/addr mismatch:  " << cursor_mismatch << "\n";


  cct.print_stats();
  zwt.print_stats();
  std::cout << "\n  Prefetch metrics with respect to capability size\n";
  static const char* size_name[NUM_SIZES] = {"SMALL","MEDIUM","LARGE","XLARGE","XXL"};

  for (int c = 0; c < NUM_SIZES; c++) {
    std::cout << "  [" << size_name[c] << "]"
              << "  accesses=" << access_by_size[c]
              << "  pf_issued=" << pf_by_size[c]
              << "  useful=" << useful_by_size[c] << "\n";
  }
}
