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
| Trace format | `inc/trace_instruction.h` | `cheri_instr`: 128 bytes, layout pinned by `static_assert`s. This is a binary contract with the CHERI-QEMU tracer and `cheri-trace-filter`. |
| Instruction | `inc/instruction.h` | `auth_cap`, `transferred_cap`, `cap_op`, `is_presimpoint`, and a `cheri_instr` constructor |
| Trace reader | `inc/tracereader.h`, `src/tracereader.cc` | PRESIMPOINT entries write `cap_mem` and are never emitted as instructions. They are skipped once `cap_mem` is finalized, i.e. after a trace wrap. The first non-presimpoint entry finalizes `cap_mem` and prints `[TRACE] ... presimpoint phase complete`. |
| CLI | `src/main.cc` | `-p/--cheri-purecap`. `initialize_capability_memory(NUM_CPUS)` is always called. |
| Capability memory | `inc/capability_memory.h`, `src/capability_memory.cc` | Per-CPU shadow of 16-byte slots. A presimpoint map is compacted by the idempotent `finalize()` into a sorted vector plus an interned descriptor table. Later stores and invalidations are tracked in separate structures. |
| Core | `src/ooo_cpu.cc`, `inc/ooo_cpu.h` | `LSQ_ENTRY` carries the auth and transferred caps. Loads and stores put `auth_cap` on the packet and warn when it is untagged. `do_complete_store` is the only runtime writer of `cap_mem`: a tagged transferred cap is stored, anything else invalidates the slot. |
| Packets | `inc/channel.h` | `request.cap`, `response.cap`, a 6-arg `response` constructor. The upstream 5-arg constructor is kept. |
| Caches | `inc/cache.h`, `src/cache.cc`, `inc/block.h` | `cap` on lookup and fill entries, `BLOCK::auth_cap`. The hit response carries the cap loaded from `cap_mem`. The victim's `auth_cap` becomes `evicted_cap`. Cap-carrying `prefetch_line` overloads. `CACHE::v_addr` and `vaddr_evicted` side-channel members. |
| DRAM | `src/dram_controller.cc` | `cap` passes through responses. |
| Stats | `inc/cache_stats.h`, `src/cache_stats.cc`, `src/plain_printer.cc` | `cap_auth_*` and `cap_data_*` hit/miss by size class; `capabilities_per_cl_{hit,miss}`. Plain text only. Downstream plot scripts parse this format, so do not change it. |
| Utilities | `inc/cheri_prefetch_utils.h` | Permission bits, `CAPS_PER_CL`, `TLBClone`, bounds helpers |
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

## Design timeline (commit dates)
- **2025-04 to 2025-09:** QEMU tracer and converters, RISC-V branch handling, first capability-aware trace format (`b2914368`, 2025-09-02).
- **2025-10-27:** `cap_mem` memory map (`a8f6633a`). Capabilities reach prefetchers through cache-side state, with no hook changes.
- **2026-02-18 / 02-26:** First CHERI prefetcher (`ip_stride_cheri`), then CHERI cache stats.
- **2026-03-17:** Auth/transferred caps plus a `cap_op` bitmask. PRESIMPOINT entries load `cap_mem` without entering the pipeline. Rule: only capability *stores* update `cap_mem`, at store completion.
- **2026-03-28:** DPC4 infrastructure (`is_instr`). Untagged-authority fallback paths removed from the CHERI prefetchers (`3a6144aa`).
- **2026-03-29 to 2026-04-03:** `TLBClone` added (`99932c81`), removed (`2dc0d48d`), then kept in utils.
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
  - `static_assert`s in `inc/trace_instruction.h` pin the `cheri_instr` layout.
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

- **Prefetches inherit the triggering access's capability** (per-cache JSON knob `inherit_trigger_cap`, default `false`). This is the first part of task 2.
  - **Mechanism:**
    - `CACHE::impl_prefetcher_cache_operate` records the triggering access's `cap` on entry and clears it on exit.
    - The no-cap `CACHE::prefetch_line(addr, fill_this_level, metadata)` attaches that recorded cap to the prefetch packet when it is called inside the hook.
    - The cap overloads are unaffected. Calls outside `cache_operate` (cycle and fill hooks) still get an untagged cap.
    - **The inherited cap is re-pointed at the prefetched line.** `offset = prefetch VA - base`, so `capability_cursor()` and `lines_from_cap_base()` at the next level describe the prefetched line, not the trigger. Base, length, permissions and tag are unchanged.
      - The prefetch VA is `pf_addr` on `virtual_prefetch` caches.
      - On physical caches, it is the trigger's VA page spliced with `pf_addr`'s page offset, when the prefetch is on the trigger's physical page.
      - Otherwise, the trigger's offset is kept and counted in the new stat `pf_cap_offset_unadjusted`, which is printed as `PREFETCH CAP OFFSET UNADJUSTED` for L1D/L2C/LLC and included in JSON. This covers a physical prefetch on a different page, and a prefetch below `base`. A negative offset would wrap and trip the overflow assert in `cheri::capability_cursor()`.
  - **Why:** every CHERI prefetcher issued through the no-cap overload, so every prefetch packet was untagged. At L2C/LLC, `prefetch_activate` is `LOAD,PREFETCH`, so every PREFETCH access there reached the prefetcher untagged and hit the untagged-cap early return. CHERI prefetchers at L2C/LLC therefore never trained on L1D (or L2C) prefetches. The fill's `cap` and `BLOCK::auth_cap` of prefetched lines were untagged for the same reason.
  - **Enabled** at L1D, L2C and LLC in all `champsim_cheri_*` configs. It stays off (the default) in `champsim_config.json`, `champsim_no_pf_config.json` and all `champsim_riscv_*` configs, and at L1I everywhere, so baseline runs are unchanged. Stock and third-party prefetchers were not edited.
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
  - **Issuing capability:** the cap on the prefetch packet (explicit, or inherited and offset-adjusted via `inherit_trigger_cap`); UNTAGGED if none.
    - Its size class and base are stamped in `prefetch_line` on the `tag_lookup_type`, copied to `fill_type`, and stored on `BLOCK` (`pf_cap_class`, `pf_cap_base`, separate from `auth_cap`) when the fill sets the prefetch bit.
    - For late-useful prefetches they are read from the in-flight entry before `fill_type::merge`.
  - **Counters,** keyed by (size class, CPU of the prefetch):
    - `pf_issued_by_cap_size` (sums to `pf_issued`); `pf_issued_skip_fill_by_cap_size` (`fill_this_level == false`, never credited anywhere).
    - `pf_redundant_by_cap_size` (own prefetch hit a resident line).
    - `pf_fill_own_by_cap_size` (fills that set the prefetch bit; ≤ `pf_fill`, which also counts upper-level prefetches).
    - `pf_useful_timely_demand_by_cap_size` and `pf_useful_timely_upper_pf_by_cap_size` (both at the `try_hit` site), and `pf_useful_late_by_cap_size` (at the `handle_miss` site). `timely_demand + timely_upper_pf + late` sums to `pf_useful`. `pf_useless_by_cap_size` sums to `pf_useless`.
    - `pf_useful_same_object_by_cap_size` (demand cap tagged with the issuing base) and `pf_useful_demand_untagged_by_cap_size`. Both are counted only for demand uses (timely-demand and late).
    - `pf_out_of_bounds_at_issue_by_cap_size` (tagged cap, prefetch VA known and outside `[base, base+length)`).
  - **Wiring:** all counters are in `end_phase`'s `roi_stats` copy and in `operator-`.
  - **Output:**
    - Plain text: a per-CPU "Prefetch Usefulness by Capability Size" table for L1D/L2C/LLC, printed after the existing CHERI sections and only when a count is nonzero. It shows raw counts plus derived columns over prefetch fills = fill_own + late (the Berti, MICRO'22 definition; upstream does not count a late prefetch as a fill because the merged demand takes over its MSHR entry, but it still brought the line in):
      - accuracy = (timely_demand + late) / prefetch fills (demand-only useful);
      - timely % = timely_demand / prefetch fills;
      - late % = late / prefetch fills (timely % + late % = accuracy);
      - unused at end = fill_own − timely_demand − timely_upper_pf − useless (informational; counts as not useful).
      - Upper-level-prefetch hits have their own raw column (`TimelyUpPf`) and are not in accuracy.
    - JSON: `"prefetch by capability size"` → counter → class → per-CPU raw counts.
    - Coverage by size is computed offline against the authority-capability LOAD miss table.
  - **Counting semantics are upstream's**, verified in the port:
    - A timely useful is any non-own access that hits a prefetched block, including a PREFETCH from the upper level (`useful_prefetch = hit && way->prefetch && !handle_pkt.prefetch_from_this`); the hit also clears the prefetch bit. The late site, by contrast, excludes PREFETCH requests.
    - **Choice:** `pf_useful` is left as upstream counts it. The by-size counters split the timely site so the two cases stay visible, and the printed accuracy counts only demand uses. A prefetch whose line was first touched by an upper-level prefetch counts as neither useful nor useless in accuracy, even if a demand later hits the line, because the bit is already cleared.
    - `pf_fill` excludes fills that bypass the cache.
    - `invalidate_entry` counts nothing.
  - **Same-object rule:** "same object" also requires a tagged issuing capability.
  - **Resident blocks:** resident-at-end can be off by the rare invalidation.
  - Test: `437-cheri-prefetch-usefulness-by-cap-size.cc` (11 cases).

## Known issues (intentionally not changed)
- **Stray `extern` in `src/ooo_cpu.cc`.** It declares `extern std::vector<champsim::capability_memory> cap_mem;` at global scope. It is unused; the real object is `champsim::cap_mem`.
- **Order-dependent side channels.** `CACHE::v_addr` and `vaddr_evicted` are written in `try_hit`/`handle_fill` and read by prefetchers.
- **Hardcoded constants.** The caps-per-cache-line loops in `cache.cc` use `4` and `16` instead of `cheri::CAPS_PER_CL` / the alignment constant; the pattern is repeated three times.
- **`cache_stats` `operator-`** does not subtract `miss_merge`/`fill`. This matches upstream behaviour.
- **`CACHE::auth_capability`** is declared but not read by the core.
- **Unported side branches.** `cheri_ampm` (`01c5aa6`, `af1cbd3`) rewrites `ampm_cheri` with feedback throttling. `hook_extensions` (the same two commits plus `eb12d5c`) adds a `vaddr` hook parameter. Both branch from `1671e64`, conflict with `e0e3600`, and are not ported.

## Keeping in sync with upstream
```
git fetch upstream
git merge upstream/master          # never rebase published history, never force-push
git submodule update --init vcpkg && ./vcpkg/vcpkg install   # if the vcpkg baseline moved
```
Remotes: `origin` = `Charles-E-Williams/ChampSim-Cheri`, `upstream` = `ChampSim/ChampSim`.

Upcoming: upstream PR #564 ("module_inheritance", on `develop` since 2026-06-04) turns modules into an inheritance-based system with one flattened hook set. When it reaches `master`, the extended DPC4/CHERI hooks (layer A) and every module in layer C will need to be reworked.
