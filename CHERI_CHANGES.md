# ChampSim-CHERI: changes relative to upstream ChampSim

This branch is upstream [ChampSim/ChampSim](https://github.com/ChampSim/ChampSim) `master` plus the CHERI fork's changes.

| | |
|---|---|
| Upstream base of the original fork | `19f8c80` (2025-02-15, "Merge PR #595 livelock-period-extension"). First fork commit `b1df8f4` (2025-04-02). |
| Original fork head | `fe067a5a`, tagged `cheri-cal-2026`. 118 fork commits, plus the ranks=2 config commit made before the port. |
| Port target | upstream `master` `410cee62` (2026-09-04, PR #722). |
| Port method | `git diff 19f8c80 cheri-cal-2026` split by path into layers, re-applied onto upstream `master` (one commit per layer). The full per-commit history remains reachable through the `cheri-cal-2026` tag and the old `master`. |

## Layers

### A. DPC4 hook infrastructure (not CHERI-specific)
Commits: `ae7b5d6` (2026-03-04, DPC4 prefetchers), `c64598a1` (2026-03-28, prefetcher API), `838f78e2` (2026-03-28, `is_instr` gating), `d62f8668` (2026-05-04, more hook params).

- **Extended prefetcher hooks, dispatched first:**
  - `prefetcher_cache_operate(addr, ip, cpu, cap, cache_hit, useful_prefetch, type, metadata_in, metadata_hit)`
  - `prefetcher_cache_fill(addr, ip, cpu, cap, useless, set, way, prefetch, evicted_addr, evicted_cap, metadata_in, metadata_evict, cpu_evict)`
  - An intermediate fill form, `(addr, set, way, prefetch, evicted_addr, metadata_in, evicted_cap)`, is also accepted.
  - All upstream legacy signatures still dispatch through the `if constexpr` chain in `inc/cache.h`.
- **Silent-mismatch pitfall.** A prefetcher whose signature matches no form is silently ignored: it compiles, but issues zero prefetches. This bit the project in Jun 2025 and again on 2026-04-01 (`spp_ppf_cheri`).
- **`is_instr` gating.** `is_instr` sits on `channel::request`, `CACHE::tag_lookup_type` and `CACHE::fill_type`, and `CACHE::module_is_instr()` reads it. Fetch packets set it in `O3_CPU::do_fetch_instruction`. Prefetchers are activated only on data accesses (`should_activate_prefetcher(...) && !module_is_instr(...)`) and trained only on data fills. As a result, L2C/LLC prefetchers never see instruction-fetch misses, which differs from stock upstream even for non-CHERI prefetchers. **Removed after the port; see "Behaviour changes after the port".**
- **`BLOCK::cpu`** is recorded at fill and used for `cpu_evict`.

### B. CHERI core simulator
Key commits: `a8f6633a` (2025-10-27, cap memory map), `6cd3d3d3` (2026-01-30), `82b09f63` (2026-02-26, cap stats), `f7a40a39` / `3c12146f` (2026-03-17, PRESIMPOINT and auth/transferred trace format), `53d7a0be` (2026-04-08, compact capability memory), `0297503e` (2026-04-13, `evicted_cap`).

| Area | Files | Change |
|---|---|---|
| Capability type | `inc/cheri.h` | `champsim::capability {offset, base, length, permissions, tag}`, `cap_op_type {NONE, AUTH, TRANSFERRED, BOTH, PRESIMPOINT}` |
| Trace format | `inc/trace_instruction.h` | `cheri_instr`: 128 bytes. This is a binary contract with the CHERI-QEMU tracer and `cheri-trace-filter`. |
| Instruction | `inc/instruction.h` | `auth_cap`, `transferred_cap`, `cap_op`, `is_presimpoint`, and a `cheri_instr` constructor |
| Trace reader | `inc/tracereader.h`, `src/tracereader.cc` | PRESIMPOINT entries write `cap_mem` and are never emitted as instructions. They are skipped once `cap_mem` is finalized, i.e. after a trace wrap. The first non-presimpoint entry finalizes `cap_mem` and prints `[TRACE] ... presimpoint phase complete`. |
| CLI | `src/main.cc` | `-p/--cheri-purecap`. `initialize_capability_memory(NUM_CPUS)` is always called. |
| Capability memory | `inc/capability_memory.h`, `src/capability_memory.cc` | Per-CPU shadow of 16-byte slots. A presimpoint map is compacted by the idempotent `finalize()` into a sorted vector plus an interned descriptor table. Later stores and invalidations are tracked in separate structures. |
| Core | `src/ooo_cpu.cc`, `inc/ooo_cpu.h` | `LSQ_ENTRY` carries the auth and transferred caps. Loads and stores put `auth_cap` on the packet and warn when it is untagged. `do_complete_store` is the only runtime writer of `cap_mem`: a tagged transferred cap is stored, anything else invalidates the slot. |
| Packets | `inc/channel.h` | `request.cap`, `response.cap`, a 6-arg `response` constructor. The upstream 5-arg constructor is kept. |
| Caches | `inc/cache.h`, `src/cache.cc`, `inc/block.h` | `cap` on lookup and fill entries, `BLOCK::auth_cap`. The hit response carries the cap loaded from `cap_mem`. The victim's `auth_cap` becomes `evicted_cap`. Cap-carrying `prefetch_line` overloads. (The fork's `CACHE::v_addr` / `vaddr_evicted` side-channel members were removed after the port.) |
| DRAM | `src/dram_controller.cc` | `cap` passes through responses. |
| Stats | `inc/cache_stats.h`, `src/cache_stats.cc`, `src/plain_printer.cc` | `cap_auth_*` and `cap_data_*` hit/miss by size class; `capabilities_per_cl_{hit,miss}`. Plain text only (original CHERI distributions). Downstream plot scripts parse this format, so do not change it. |
| Utilities | `inc/cheri_prefetch_utils.h` | Permission bits, `CAPS_PER_CL`, bounds helpers (`TLBClone` was removed after the port) |
| Misc | `inc/msl/lru_table.h`, `inc/ptw.h`, `src/ptw.cc`, `src/vmem.cc`, `inc/register_allocator.h`, `inc/champsim.h`, `src/modules.cc`, `Makefile` | Small supporting edits. `REG_RETURN` was added for RISC-V branches. |

### C. Modules
- **CHERI-aware prefetchers:** `ip_stride_cheri`, `ip_stride_cheri_dynamic`, `next_line_cheri`, `ampm_cheri`, `sms_cheri`, `spp_cheri`, `spp_ppf_cheri`, `ipcp_cheri`, `berti_cheri`, `cheri_ptr_chase`.
- **Third-party baselines,** carried as-is: `ampm`, `sms`, `ipcp`, `berti`, `spp_ppf`, `asd`, `hasd`, and branch predictor `tage_sc`.
- **Stock modules edited to the extended hooks:** `no`, `ip_stride`, `next_line`, `spp_dev`, `va_ampm_lite`.
- Berti's "dropped packet" branch (`way == NUM_WAY` with an empty `evicted_addr`) arrived with the Berti import `5c3b28a9` (2026-04-09), not with the `is_instr` change.

### D. Configs and scripts
- The `champsim_{cheri,riscv}_*_config.json` and `champsim_no_pf_config.json` files, `scripts/build_all.sh`, and `scripts/update_configs.py`.
- Upstream's `champsim_config.json`, which the old fork had deleted, is restored.
- The QEMU trace converters that used to live under `tracer/` were removed in `fb661361`, because QEMU now emits ChampSim traces directly.
- `champsim_cheri_config.json` was repaired in `208c54e6`. It had been broken since the `kratos` prefetcher was deleted in April 2026 (`ed9b7113`): its L2C prefetcher still named `kratos`, so `config.sh` rejected it. The L2C prefetcher is now `no`.

## Design timeline (commit dates)
- **2025-04 to 2025-09:** QEMU tracer and converters, RISC-V branch handling, first capability-aware trace format (`b2914368`, 2025-09-02).
- **2025-10-27:** `cap_mem` memory map (`a8f6633a`). Capabilities reach prefetchers through cache-side state, with no hook changes.
- **2026-02-18 / 02-26:** First CHERI prefetcher (`ip_stride_cheri`), then CHERI cache stats.
- **2026-03-17:** Auth/transferred caps plus a `cap_op` bitmask. PRESIMPOINT entries load `cap_mem` without entering the pipeline. Rule: only capability *stores* update `cap_mem`, at store completion.
- **2026-03-28:** DPC4 infrastructure (`is_instr`). Untagged-authority fallback paths removed from the CHERI prefetchers (`3a6144aa`).
- **2026-03-29 to 2026-04-03:** `TLBClone` added (`99932c81`), removed (`2dc0d48d`), then kept in utils. It had no users and was deleted in 2026-09.
- **2026-04-08:** Compact capability memory. `finalize()` made idempotent to fix the trace-wrap crash.
- **2026-04-13:** `evicted_cap` threaded through the fill hook (for AMPM-CHERI zone cleanup).
- **2026-05-04:** More hook parameters (`d62f8668`); the explicit `cap` argument replaced `intern_->get_authorizing_capability()`.
- **2026-05-08 to 2026-06-05:** AMPM-CHERI design with a page-based fallback, followed by tuning. This is the state used for the IEEE CAL results.

## Port notes (2026-09)
- **Upstream's 2025-05 cache/channel refactor:**
  - `cap` and `is_instr` moved from `mshr_type` to `fill_type`.
  - The fork's `cap` edit to channel collision/forwarding code was dropped along with that code.
  - Every response in `CACHE` (fill, hit) and in `DRAM_CHANNEL` (warmup, dbus, write-forward) carries `cap`.
  - CHERI stats sit in the same places in `try_hit`, `handle_miss` and `handle_write`.
- The heartbeat moved to upstream event listeners. The fork's heartbeat declarations in `ooo_cpu.cc` were dropped.
- `src/channel.cc`, `inc/msl/fwcounter.h` and `src/register_allocator.cc` are identical to upstream: the fork's fixes already existed there, or the code was removed.
- **Tests added with the port** (`test/cpp/src/`):
  - `002-cheri-cap-mem-setup.cc`: a Catch2 listener that resets `champsim::cap_mem` before each test case. `CACHE` indexes `cap_mem` by CPU, so without it every cache test reads an empty vector.
  - `086-cheri-tracereader.cc`: PRESIMPOINT entries never become instructions, `cap_mem` is loaded and finalized, and a wrapped trace does not reload it.
  - `090-capability-memory.cc`: finalize idempotence, and store/invalidate/load after finalize.
  - `433-cheri-capability-hooks.cc`: the demand `cap` reaches `cache_operate` at two chained levels, and the victim's `auth_cap` reaches `cache_fill` as `evicted_cap`.
  - `453-va-ampm-lite-behavior.cc` was updated to call the extended `impl_prefetcher_*` signatures.
  - `static_assert`s pinning the `cheri_instr` layout were added to `inc/trace_instruction.h` with the port and later removed.
- Upstream `master` links `libCLI11`. Run `./vcpkg/bootstrap-vcpkg.sh && ./vcpkg/vcpkg install` after `git submodule update --init`.

## Behaviour changes after the port (2026-09)
- **Untagged capability → early exit** (`acea8ec6`). Every CHERI prefetcher's `cache_operate` returns `metadata_in` immediately when the authorizing capability is untagged. Before this, `berti_cheri` had a page-bounded fallback, and `cheri_ptr_chase` never checked the tag. Fill hooks are unchanged.
- **`BLOCK::auth_cap` is only updated by tagged demand hits.**
  - Before: `try_hit` overwrote `way->auth_cap` with the incoming packet's cap on every hit, with the comment "update auth cap if the block is modified".
  - Why that was wrong:
    - Prefetch requests carry either no cap (legacy `prefetch_line`) or the prefetcher's cap, not a demand's authority.
    - Untagged accesses carry no authority at all.
    - So a prefetch hit or an untagged hit (e.g. an L1D prefetch arriving at L2C) erased the capability of the last real access to the line. That capability later reaches prefetchers as `evicted_cap` (AMPM-CHERI zone cleanup) and rides on the writeback packet.
  - Now: `try_hit` updates it only when `handle_pkt.cap.tag && handle_pkt.type != access_type::PREFETCH`. Fills still set `auth_cap` from the fill entry.
  - Test: `434-cheri-auth-cap-on-hit.cc`.

- **Prefetches inherit the triggering access's capability** (always on). This started as the first part of brief task 2; task 2 itself was later dropped (see below).
  - **Mechanism:**
    - `try_hit` records the triggering access's `cap` (with its physical and virtual addresses) just before calling the prefetcher's `cache_operate`, and clears it on return.
    - The no-cap `CACHE::prefetch_line(addr, fill_this_level, metadata)` attaches that recorded cap to the prefetch packet when it is called inside the hook.
    - Calls outside `cache_operate` (cycle and fill hooks) through the no-cap overload still get an untagged cap.
    - **Every tagged cap on a prefetch is re-pointed at the prefetched line**, explicit or inherited (`CACHE::repoint_prefetch_cap`, in all three `prefetch_line` overloads).
      - Why: a prefetch carries its trigger's cap, so the cursor (`base + offset`) points at the trigger's address. At L2C/LLC, CHERI prefetchers train on PREFETCH accesses and derive the position within the object from the cursor (`lines_from_cap_base()`, `capability_cursor()`), and the filled block keeps the cap as `auth_cap` (later `evicted_cap`). An earlier version re-pointed only inherited caps. That was wrong: `ip_stride_cheri`'s lookahead cap and `sms_cheri`'s buffered caps also carry the trigger's cursor.
      - Only the offset changes; base, length, permissions and tag never do.
      - The prefetch VA is `pf_addr` on `virtual_prefetch` caches. On physical caches it is the trigger's VA page spliced with `pf_addr`'s page offset, when the prefetch is on the trigger's physical page; this works only inside `cache_operate`.
      - With `line` = the cache line containing the prefetch VA and the object `[base, base + length)`:
        - the cursor is already inside `line`: unchanged (covers `cheri_ptr_chase`, whose pointer cap already points at its target);
        - else `line` overlaps the object: `offset = max(prefetch VA, base) − base`, so the cursor never points outside the object (the first partial line of an unaligned object points at `base`);
        - else (the line lies entirely outside the object), or the prefetch VA is unknown: the offset is left unchanged.
      - This replaces the earlier "below `base` is left unchanged" rule. The counter that tracked unchanged offsets (`pf_cap_offset_unadjusted`, plain-text `PREFETCH CAP OFFSET UNADJUSTED`, JSON `prefetch cap offset unadjusted`) was later removed.
      - A prefetch issued with an explicit cap from `prefetcher_cycle_operate` on a physical cache (e.g. `sms_cheri` at L2C) has no known VA, so the cache leaves its cap as the prefetcher set it.
    - **`cheri::prefetch_safe()` uses the same line-overlap test** through the shared helper `cheri::overlaps_bounds()`: `base < top && line_start < top && line_start + BLOCK_SIZE > base`, plus load permission.
      - Before, it accepted a prefetch only if its address was inside `[base, top)`, so the first line of an object with an unaligned base was rejected.
      - `overlaps_bounds()` had no callers. Its old form also had an off-by-one (`block_end > base` with an inclusive `block_end`), fixed by the shared formula.
      - Callers (their behaviour at object edges changes): `next_line_cheri.cc:17`, `ip_stride_cheri.cc:73`, `ip_stride_cheri_dynamic.cc:91`, `spp_cheri.cc:98`, `ampm_cheri_aux.cc:115`, `ipcp_cheri.cc:96, :208, :229, :257, :277`, `berti_cheri.cc:1052`.
    - `cheri::hash_capability()` hashes only `base` and `length`, so re-pointing does not change it.
    - Test: `438-cheri-prefetch-cap-repoint.cc`.
  - **Why:** every CHERI prefetcher issued through the no-cap overload, so every prefetch packet was untagged. At L2C/LLC, `prefetch_activate` is `LOAD,PREFETCH`, so every PREFETCH access there reached the prefetcher untagged and hit the untagged-cap early return. CHERI prefetchers at L2C/LLC therefore never trained on L1D (or L2C) prefetches. The fill's `cap` and `BLOCK::auth_cap` of prefetched lines were untagged for the same reason.
  - **Always on, in every cache and every config.**
    - It was first a per-cache JSON knob, `inherit_trigger_cap`, enabled only at L1D/L2C/LLC in the `champsim_cheri_*` configs. The knob was then removed: its builder methods, the `CACHE` member, the `config/` handling and the field in every JSON config.
    - **Effect on stock and third-party prefetchers:** none on their behaviour. They use the legacy hook signatures and never read the capability. What changes is statistics: their prefetch packets now carry the trigger's capability, so the by-size prefetch counters, and the fill's `cap` / `BLOCK::auth_cap` of their prefetched lines, reflect it. With untagged triggers (non-CHERI traces) nothing changes.
    - Tests: 435's knob-off case was removed. 437-11 now checks that prefetches triggered by untagged accesses land in UNTAGGED.
  - **Cycle-hook issuers** now keep the triggering cap with each buffered candidate and use the cap overload:
    - `ip_stride_cheri` / `ip_stride_cheri_dynamic`: `active_lookahead.cap`.
    - `sms_cheri`: `pref_buffer` now stores `(address, capability)` pairs, filled from the trigger in `cache_operate`.
  - **`cheri_ptr_chase`** targets a different object than the trigger, so it never inherits. All three call sites (the target in `cache_operate`, `pct_chase`, and `prefetcher_cache_fill`) pass the chased pointer's own capability through the cap overload. `ptr_map` entries now store that capability alongside the target address.
  - Test: `435-cheri-inherit-trigger-cap.cc`.

- **Untagged-authority warnings are rate-limited.**
  - `execute_load` and `do_complete_store` now print their `[OOO_CPU] WARNING: ... missing tagged authority capability` message once per CPU, the first time each occurs; the text is unchanged.
  - Every occurrence is counted in `cpu_stats::untagged_auth_loads` and `untagged_auth_stores`. These are printed per core as `cpuN UNTAGGED AUTHORITY CAPABILITY LOADS: x STORES: y`, and included in JSON.
  - Like the other core stats, the counters reset at each phase, so the final (ROI) numbers exclude warmup.
  - Test `198-core-plain-printer.cc` expects the new line.

- **`is_instr` gating removed (brief §10 task 1).**
  - What was removed: the `is_instr` field on `channel::request`, `CACHE::tag_lookup_type` and `CACHE::fill_type`; `CACHE::module_is_instr()`; the flag set in `O3_CPU::do_fetch_instruction`.
  - Prefetcher activation in `try_hit` is upstream's `should_activate_prefetcher(handle_pkt)` again, so each cache's JSON `prefetch_activate` list alone decides. `handle_fill` calls `prefetcher_cache_fill` for every fill, as upstream does.
  - Consequences:
    - L2C/LLC prefetchers now see instruction-fetch misses (LOAD) and instruction-line fills, and L1I prefetchers are trained on their fills.
    - Instruction fetches carry no authorizing capability. CHERI prefetchers return early from `cache_operate` on them (untagged-cap policy). Their fill hooks either ignore the untagged `cap`/`evicted_cap` or check the tag (`ampm_cheri`).
    - Stock and third-party prefetchers at L2C/LLC now train on instruction misses, so baseline (`champsim_riscv_*`) results change too.
    - The CHERI cache stats were never gated, so they are unchanged.
  - Test: `436-l1i-miss-activates-l2c-prefetcher.cc`.
  - Upstream's `_is_instruction_cache` / `_is_instruction_prefetcher` in `config/` and `test/python/` are unrelated upstream config flags and remain.

- **Prefetch usefulness by capability size (brief §10 task 3, revised 2026-09-24).** Stats only; no timing or control-flow change.
  - **Issuing capability:** the cap on the prefetch packet (explicit, or inherited from the trigger and offset-adjusted); UNTAGGED if none.
    - Its size class and base are stamped in `prefetch_line` on the `tag_lookup_type`, copied to `fill_type`, and stored on `BLOCK` (`pf_cap_class`, `pf_cap_base`, separate from `auth_cap`) when the fill sets the prefetch bit.
    - For late-useful prefetches they are read from the in-flight entry before `fill_type::merge`.
  - **Counters,** keyed by (size class, CPU of the prefetch):
    - `pf_issued_by_cap_size` (sums to `pf_issued`); `pf_issued_skip_fill_by_cap_size` (`fill_this_level == false`, never credited anywhere).
    - `pf_redundant_by_cap_size` (own prefetch hit a resident line).
    - `pf_fill_own_by_cap_size` (fills that set the prefetch bit; ≤ `pf_fill`, which also counts upper-level prefetches).
    - `pf_useful_timely_demand_by_cap_size` and `pf_useful_timely_upper_pf_by_cap_size` (both at the `try_hit` site), and `pf_useful_late_by_cap_size` (at the `handle_miss` site). `timely_demand + timely_upper_pf + late` sums to `pf_useful`. `pf_useless_by_cap_size` sums to `pf_useless`.
    - `pf_useful_same_object_by_cap_size` (demand cap tagged with the issuing base) and `pf_useful_demand_untagged_by_cap_size`. Both are counted only for demand uses (timely-demand and late).
  - **Wiring:** all counters are in `end_phase`'s `roi_stats` copy and in `operator-`.
  - **Output:**
    - Plain text: two per-CPU tables for L1D/L2C/LLC, printed after the existing CHERI sections. Rows whose cells are all zero are skipped, and a table with no rows is not printed. Long column names use two header lines. Counter names and JSON keys are unchanged.
      - **"Prefetch Outcomes by Object Size (ROI)":** Object Size | Issued (`issued`) | Already Cached (`redundant`) | Filled (`fill_own`) | Used On Time (`timely_demand`) | Used Late (`late`) | Evicted Unused (`useless`) | Accuracy | On-Time % | Late %.
        - The derived columns use prefetch fills = Filled + Used Late as the denominator (the Berti, MICRO'22 definition). Upstream does not count a late prefetch as a fill, because the merged demand takes over its MSHR entry, but the prefetch still brought the line in.
        - Accuracy = (Used On Time + Used Late) / prefetch fills (demand-only useful).
        - On-Time % = Used On Time / prefetch fills; Late % = Used Late / prefetch fills; On-Time % + Late % = Accuracy.
        - Filled stays because it is part of the accuracy denominator. An earlier derived "Still Cached" column (Filled − Used On Time − `timely_upper_pf` − Evicted Unused) was dropped; see warmup carryover below.
      - **"Prefetch Consumers by Object Size (ROI)":** Object Size | Same Object (`same_object`) | Other Object (Used On Time + Used Late − Same Object − Untagged Demand) | Untagged Demand (`demand_untagged`) | Upper-Level Prefetch Hit (`timely_upper_pf`, not counted in accuracy) | Sent To Lower Level (`skip_fill`, issued with `fill_this_level == false` and never credited).
    - JSON: `"prefetch by capability size"` → counter → class → per-CPU raw counts.
    - Coverage by size is computed offline against the authority-capability LOAD miss table.
  - **Counting semantics are upstream's**, verified in the port:
    - A timely useful is any non-own access that hits a prefetched block, including a PREFETCH from the upper level (`useful_prefetch = hit && way->prefetch && !handle_pkt.prefetch_from_this`); the hit also clears the prefetch bit. The late site, by contrast, excludes PREFETCH requests.
    - **Choice:** `pf_useful` is left as upstream counts it. The by-size counters split the timely site so the two cases stay visible, and the printed accuracy counts only demand uses. A prefetch whose line was first touched by an upper-level prefetch counts as neither useful nor useless in accuracy, even if a demand later hits the line, because the bit is already cleared.
    - `pf_fill` excludes fills that bypass the cache.
    - `invalidate_entry` counts nothing.
  - **Same-object rule:** "same object" also requires a tagged issuing capability.
  - **Warmup carryover:** like upstream's prefetch counters, the by-size counters include prefetches issued during warmup whose fill, use or eviction happens in the ROI.
    - `begin_phase` resets the counters but not the prefetch bits or in-flight entries.
    - Each prefetch has at most one outcome, so per cache the carryover is at most its line count plus its MSHR and prefetch-queue entries.
  - **Invalidations:** `invalidate_entry` clears a block without counting it, so a rare invalidated prefetch has no outcome.
  - Test: `437-cheri-prefetch-usefulness-by-cap-size.cc` (11 cases: 437-1 to 437-8 and 437-10 to 437-12; 437-9 was removed with the out-of-bounds counter).

- **The core presents the authorizing capability with its cursor at the effective address.**
  - **What the trace records:** the authorizing-capability register as it was before the instruction's immediate is added. CHERI bounds-checks the effective address (cursor + immediate).
  - **Evidence:** a temporary debug counter compared `base + offset` with each tagged demand access's VA at L1D.
    - Totals: exact 1,189,694; same line 343,685; different line 627,212 (29%).
    - The same IP always showed the same constant `cursor − va`. Examples: stack loads and stores through the 1 GiB stack capability at −12, −32 and −60, and a register-save prologue at −64 … −176.
  - **Change:** `O3_CPU::set_auth_cap_cursor()`, called in `execute_load` and `do_complete_store` (per LSQ entry, so each memory operand gets its own VA), sets `offset = v_address − base` on the packet's tagged authorizing cap.
    - Base, length, permissions and tag are unchanged.
    - `cap_mem` is untouched: transferred capabilities are data and keep their own cursors.
    - If the VA is outside `[base, base + length)`, the offset is left alone and counted in the new per-CPU core stat `auth_cap_va_out_of_bounds`. A wrap below base would trip `capability_cursor()`'s assert. The first occurrence prints `[OOO_CPU] WARNING: Memory access outside the bounds of its tagged authority capability...` once; the total is printed per core as `cpuN AUTHORITY CAPABILITY DOES NOT COVER ACCESS: n` and in JSON. A valid CHERI trace should produce none.
  - **Effect:** this changes every CHERI prefetcher that derives position from the cursor: `lines_from_cap_base()`, `capability_cursor()`, `sms_cheri`'s region offset and `ampm_cheri`'s zone offset. The authorizing caps stored on blocks (`auth_cap`, later `evicted_cap`) and passed down the hierarchy change too.
  - Test `198-core-plain-printer.cc` expects the new line.

- **`CACHE::v_addr` / `vaddr_evicted` side channel removed.**
  - What it was: two members written in `try_hit`/`handle_fill` just before the prefetcher hooks and read by three CHERI prefetchers.
    - At a physical cache the cache's own prefetches carry `v_address = 0`, so for evicted own-prefetched lines it gave AMPM-CHERI a VA of 0, and their zone cleanup was silently skipped.
  - Removed: both members, every write, and the now-unused `CACHE::module_vaddress()`.
  - Readers now derive what they need from the capability, which the core presents with its cursor at the effective address. Choice per reader:
    - **`spp_cheri` operate, option (A).** The demand's virtual line comes from `cheri::line_va_from_cursor(cap, addr)`: accepted only if the cap is tagged and the cursor's line position within the page equals the physical address's. On failure, return early (untagged-cap policy), counted as `Cursor/line check failed` in its final stats. The VA only feeds the candidate VAs for `prefetch_safe` and lookahead, so results match the old side channel whenever the check passes.
    - **`sms_cheri` decompose, option (B).** Everything comes from the cursor and the physical address. Target physical line = demand's physical line + (target object line − demand object line)·64, kept only if it stays on the demand's physical page (the next-region path too). The bounds checks are unchanged, expressed object-relatively.
      - Each buffered target now stores the demand's capability re-pointed at the target line (`cheri::repoint_cap_to_line`, the same line-overlap rule as `CACHE::repoint_prefetch_cap`). Its prefetches, issued later from `cycle_operate`, therefore carry correct cursors.
      - Results differ slightly from before for objects with an unaligned `base`. The old target address was `cap_base + line·64`, which isn't line-aligned; the new one is the demand's line plus a whole number of lines.
    - **`ampm_cheri` operate and fill, option (A)**, so zone keys match.
      - Operate: the VA comes from `cheri::line_va_from_cursor(cap, addr)`. When the check fails, a large-cap access uses AMPM's page-based fallback engine, counted as `Cursor/line check failed (to page path)`.
      - Fill: the VA of the evicted line is the line of `evicted_cap`'s cursor, checked against `evicted_addr`. On failure the cleanup is skipped and counted (`Eviction cleanup skipped`). The `evicted_addr == {}` early return is kept.
      - This fixes cleanup for L2C own-prefetched lines, whose re-pointed caps point into their line. More zone bits are cleared than before, which changes AMPM-CHERI's later prefetch decisions.
  - **Invalid victim ways:** `handle_fill` passes a default (untagged) `evicted_cap` whenever the victim way is invalid (`evicted_valid = way != set_end && way->valid`), including after `invalidate_entry`, so a stale `auth_cap` is never passed.
  - **Shared helpers** in `inc/cheri_prefetch_utils.h`: `line_va_from_cursor()` (option A) and `repoint_cap_to_line()`. `CACHE::repoint_prefetch_cap` now uses the latter too.
  - **Limitations:**
    - At a physical cache, a cross-page prefetch has no usable VA: its cap can't be re-pointed, so its cursor stays in another page and the (A) check rejects it.
    - The (A) check compares only the line position within the page, so a cursor off by a whole number of pages would pass.
    - (B) trusts the cursor without a check.
  - `grep -rn "v_addr\b"` over `inc src prefetcher` still matches upstream identifiers that have nothing to do with the side channel: the `response` constructor parameters in `inc/channel.h`, the deadlock-print format strings in `src/cache.cc`, stock `va_ampm_lite`, and comments in the `berti` baselines. `grep -rn "intern_->v_addr\|vaddr_evicted\|CACHE::v_addr\|module_vaddress" inc src prefetcher` returns nothing.
  - Test: `439-cheri-cursor-derived-va.cc`.

- **Brief §10 task 2 dropped** (the central out-of-bounds prefetch drop in `CACHE::prefetch_line`, and the bounds-only ablation for stock prefetchers).
  - CHERI prefetchers already bound their own prefetches (`cheri::prefetch_safe()` and prefetcher-specific bounds logic), so a cache-side filter would never fire for them.
  - A stock prefetcher with a cache-side bounds filter is not a meaningful baseline.
  - Trigger-capability inheritance and prefetch-cap re-pointing, which started as part of task 2, stay; they feed the CHERI prefetchers and the task 3 stats.
  - **`pf_out_of_bounds_at_issue_by_cap_size` removed** (counter, ROI copy, `operator-`, the plain-text `OOBIssue` column, the JSON field, test 437-9, and its entry in the counter list that 437-10/437-11 use). It measured what task 2 would drop, and it would always read zero for CHERI prefetchers.

## Known issues (intentionally not changed)
- **`cache_stats` `operator-`** does not subtract `miss_merge`/`fill`. This matches upstream behaviour.
- **Unported side branches.** `cheri_ampm` (`01c5aa6`, `af1cbd3`) rewrites `ampm_cheri` with feedback throttling. `hook_extensions` (the same two commits plus `eb12d5c`) adds a `vaddr` hook parameter. Both branch from `1671e64`, conflict with `e0e3600`, and are not ported.

### Resolved (2026-09)
- The unused global `extern std::vector<champsim::capability_memory> cap_mem;` in `src/ooo_cpu.cc` is removed.
- The unused `CACHE::auth_capability` member is removed; nothing read it, including prefetchers.
- The three caps-per-cache-line loops in `src/cache.cc` now share `tagged_caps_in_line()`, using `cheri::CAPS_PER_CL` and `cheri::CAP_ALIGNMENT_BYTES` instead of `4` and `16`.
- The untagged-authority warning flood is rate-limited (see "Behaviour changes after the port").

## Keeping in sync with upstream
```
git fetch upstream
git merge upstream/master          # never rebase published history, never force-push
git submodule update --init vcpkg && ./vcpkg/vcpkg install   # if the vcpkg baseline moved
```
Remotes: `origin` = `Charles-E-Williams/ChampSim-Cheri`, `upstream` = `ChampSim/ChampSim`.

Upcoming: upstream PR #564 ("module_inheritance", on `develop` since 2026-06-04) turns modules into an inheritance-based system with one flattened hook set. When it reaches `master`, the extended DPC4/CHERI hooks (layer A) and every module in layer C will need to be reworked.
