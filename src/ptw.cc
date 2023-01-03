#include "ptw.h"

#include <numeric>

#include "champsim.h"
#include "champsim_constants.h"
#include "instruction.h"
#include "util.h"
#include "vmem.h"

PageTableWalker::PageTableWalker(std::string v1, uint32_t cpu, double freq_scale, std::vector<std::pair<std::size_t, std::size_t>> pscl_dims, uint32_t v10,
                                 uint32_t v11, uint32_t v12, uint32_t v13, uint64_t latency, std::vector<champsim::channel*>&& ul, champsim::channel* ll, VirtualMemory& _vmem)
    : champsim::operable(freq_scale), upper_levels(std::move(ul)), lower_level(ll), NAME(v1), RQ_SIZE(v10), MSHR_SIZE(v11), MAX_READ(v12), MAX_FILL(v13), HIT_LATENCY(latency),
      vmem(_vmem), CR3_addr(_vmem.get_pte_pa(cpu, 0, std::size(pscl_dims) + 1).first)
{
  auto level = std::size(pscl_dims) + 1;
  for (auto x : pscl_dims) {
    auto shamt = _vmem.shamt(level--);
    pscl.emplace_back(x.first, x.second, pscl_indexer{shamt}, pscl_indexer{shamt});
  }
}

bool PageTableWalker::handle_read(const request_type& handle_pkt)
{
  pscl_entry walk_init = {handle_pkt.v_address, CR3_addr, std::size(pscl)};
  std::vector<std::optional<pscl_entry>> pscl_hits;
  std::transform(std::begin(pscl), std::end(pscl), std::back_inserter(pscl_hits), [walk_init](auto& x) { return x.check_hit(walk_init); });
  walk_init =
      std::accumulate(std::begin(pscl_hits), std::end(pscl_hits), std::optional<pscl_entry>(walk_init), [](auto x, auto& y) { return y.value_or(*x); }).value();

  auto walk_offset = vmem.get_offset(handle_pkt.address, walk_init.level) * PTE_BYTES;

  if constexpr (champsim::debug_print) {
    std::cout << "[" << NAME << "] " << __func__ << " instr_id: " << handle_pkt.instr_id;
    std::cout << " address: " << std::hex << walk_init.vaddr;
    std::cout << " v_address: " << handle_pkt.v_address << std::dec;
    std::cout << " pt_page offset: " << walk_offset / PTE_BYTES;
    std::cout << " translation_level: " << walk_init.level << std::endl;
  }

  request_type packet = handle_pkt;
  packet.v_address = handle_pkt.address;
  packet.init_translation_level = walk_init.level;
  packet.cycle_enqueued = current_cycle;

  return step_translation(champsim::splice_bits(walk_init.ptw_addr, walk_offset, LOG2_PAGE_SIZE), packet.init_translation_level, packet);
}

bool PageTableWalker::handle_fill(const mshr_type& fill_mshr)
{
  if constexpr (champsim::debug_print) {
    std::cout << "[" << NAME << "] " << __func__ << " instr_id: " << fill_mshr.instr_id;
    std::cout << " address: " << std::hex << fill_mshr.address;
    std::cout << " v_address: " << fill_mshr.v_address;
    std::cout << " data: " << fill_mshr.data << std::dec;
    std::cout << " pt_page offset: " << ((fill_mshr.data & champsim::bitmask(LOG2_PAGE_SIZE)) >> champsim::lg2(PTE_BYTES));
    std::cout << " translation_level: " << +fill_mshr.translation_level;
    std::cout << " event: " << fill_mshr.event_cycle << " current: " << current_cycle << std::endl;
  }

  if (fill_mshr.translation_level == 0) {
    response_type ret_pkt{fill_mshr};
    ret_pkt.address = fill_mshr.v_address;

    for (auto ret : fill_mshr.to_return)
      ret->push_back(ret_pkt);

    total_miss_latency += current_cycle - fill_mshr.cycle_enqueued;
    return true;
  } else {
    const auto pscl_idx = std::size(pscl) - fill_mshr.translation_level;
    pscl.at(pscl_idx).fill({fill_mshr.v_address, fill_mshr.data, fill_mshr.translation_level - 1});

    return step_translation(fill_mshr.data, fill_mshr.translation_level - 1, fill_mshr);
  }
}

bool PageTableWalker::step_translation(uint64_t addr, std::size_t transl_level, const request_type& source)
{
  auto fwd_pkt = source;
  fwd_pkt.address = addr;
  fwd_pkt.is_translated = true;
  fwd_pkt.type = TRANSLATION;
  fwd_pkt.to_return = {&lower_level->returned};
  fwd_pkt.translation_level = transl_level;

  auto matches_and_inflight = [addr](const auto& x) {
    return (x.address >> LOG2_BLOCK_SIZE) == (addr >> LOG2_BLOCK_SIZE) && x.event_cycle == std::numeric_limits<uint64_t>::max();
  };
  auto mshr_entry = std::find_if(std::begin(MSHR), std::end(MSHR), matches_and_inflight);

  bool success = true;
  if (mshr_entry == std::end(MSHR))
    success = lower_level->add_rq(fwd_pkt);

  if (success) {
    fwd_pkt.to_return = source.to_return; // Set the return for MSHR packet same as read packet.
    fwd_pkt.type = source.type;
    fwd_pkt.event_cycle = std::numeric_limits<uint64_t>::max();
    MSHR.push_back(fwd_pkt);
  }

  return success;
}

void PageTableWalker::operate()
{
  for (const auto& x : lower_level->returned)
    finish_packet(x);
  lower_level->returned.clear();

  auto fill_this_cycle = MAX_FILL;
  while (fill_this_cycle > 0 && !std::empty(MSHR) && MSHR.front().event_cycle <= current_cycle) {
    auto success = handle_fill(MSHR.front());
    if (!success)
      break;

    MSHR.pop_front();
    fill_this_cycle--;
  }

  auto tag_bw = MAX_READ;
  for (auto ul : upper_levels) {
    auto [rq_begin, rq_end] = champsim::get_span_p(std::cbegin(ul->RQ), std::cend(ul->RQ), tag_bw, [this](const auto& pkt) { return this->handle_read(pkt); });
    tag_bw -= std::distance(rq_begin, rq_end);
    ul->RQ.erase(rq_begin, rq_end);
  }
}

void PageTableWalker::finish_packet(const response_type& packet)
{
  for (auto& mshr_entry : MSHR) {
    if (eq_addr<mshr_type>{packet.address, LOG2_BLOCK_SIZE}(mshr_entry)) {
      uint64_t penalty;
      if (mshr_entry.translation_level == 0)
        std::tie(mshr_entry.data, penalty) = vmem.va_to_pa(mshr_entry.cpu, mshr_entry.v_address);
      else
        std::tie(mshr_entry.data, penalty) = vmem.get_pte_pa(mshr_entry.cpu, mshr_entry.v_address, mshr_entry.translation_level);
      mshr_entry.event_cycle = current_cycle + (warmup ? 0 : penalty);

      if constexpr (champsim::debug_print) {
        std::cout << "[" << NAME << "_MSHR] " << __func__ << " instr_id: " << mshr_entry.instr_id;
        std::cout << " address: " << std::hex << mshr_entry.address;
        std::cout << " v_address: " << mshr_entry.v_address;
        std::cout << " data: " << mshr_entry.data << std::dec;
        std::cout << " translation_level: " << +mshr_entry.translation_level;
        std::cout << " occupancy: " << std::size(MSHR);
        std::cout << " event: " << mshr_entry.event_cycle << " current: " << current_cycle << std::endl;
      }
    }
  }

  std::sort(std::begin(MSHR), std::end(MSHR), [](const auto& x, const auto& y) { return x.event_cycle < y.event_cycle; });
}

void PageTableWalker::print_deadlock()
{
  if (!std::empty(MSHR)) {
    std::cout << NAME << " MSHR Entry" << std::endl;
    std::size_t j = 0;
    for (mshr_type entry : MSHR) {
      std::cout << "[" << NAME << " MSHR] entry: " << j++ << " instr_id: " << entry.instr_id;
      std::cout << " address: " << std::hex << entry.address << " v_address: " << entry.v_address << std::dec << " type: " << +entry.type;
      std::cout << " translation_level: " << +entry.translation_level << " event_cycle: " << entry.event_cycle << std::endl;
    }
  } else {
    std::cout << NAME << " MSHR empty" << std::endl;
  }
}
