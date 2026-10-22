# Berti-TDP: Berti with tag-directed prefetching (TDP)

`isca` branch, uncommitted. One module, `prefetcher/berti_tdp/`, laid out like `prefetcher/berti/`: `berti_tdp.h`,
`berti_tdp.cc`, `berti_tdp_params.h`. `diff prefetcher/berti/berti.cc prefetcher/berti_tdp/berti_tdp.cc` shows exactly
what TDP adds to Berti.

**How to run.** Config `champsim_cheri_berti_tdp_config.json` is the `champsim_riscv_berti` config with only the L1D
prefetcher and the executable name changed:

```
./config.sh champsim_cheri_berti_tdp_config.json && make
bin/champsim_cheri_berti_tdp -p --warmup-instructions 10000000 --simulation-instructions 20000000 <trace>
```

Do not pass `--skip-capmap`: TDP reads pointers from the capability map, and without it Berti-TDP behaves exactly
like Berti. The defines at the end of `berti_tdp_params.h` are the design; set `TDP_ENABLE` to 0 to get Berti bit for bit
(the control behind every "vs Berti" number here), or flip a switch to reproduce a row of §5.

## 1. Result

ROI IPC. CHERI traces, run with `-p` and the capability map loaded (no `--skip-capmap`). Slices: 10M warmup + 20M;
neutron, cactus: 5M + 10M. Noise is about ±0.4% per trace.

| | xalancbmk-142B | omnetpp-100B | povray-119B | neutron_s-59B | cactus_s-4162B |
|---|---|---|---|---|---|
| no prefetching | 0.3334 | 0.5091 | 1.644 | 1.314 | 1.205 |
| Berti | 0.3871 (+16.1%) | 0.6010 (+18.1%) | 1.597 (−2.9%) | 1.423 (+8.3%) | 1.203 (−0.2%) |
| **Berti-TDP** | **0.5126** | **0.6877** | 1.603 | 1.423 | 1.204 |
| **Berti-TDP vs Berti** | **+32.4%** | **+14.4%** | +0.4% | 0.0% | +0.1% |

- **All of the gain comes from the tag-directed prefetching.** With `TDP_ENABLE 0` the module reproduces Berti in every ROI counter. That was
  checked on the xalancbmk and omnetpp full windows and on short windows of xalancbmk, omnetpp and povray. Berti's
  code inside the module is unchanged, including its history-table defect (§7).
- **Where TDP has nothing to do, it does nothing.** In 10M instructions neutron's L1D fills hold 25 distinct
  pointers (its misses are in large arena and stack objects). TDP issues no prefetches there, and every counter
  equals Berti. Cactus: 592 TDP prefetches in 10M instructions.
- **povray gains little although its dereferences are well predicted.** Its misses are L2 hits: 273 L2 demand
  misses in 20M instructions, so there is little latency to hide.
- **The gain comes from fetching the same lines earlier, not more lines.** xalancbmk, 20M instructions:
  - cycles: 51.7M → 39.0M;
  - L2 demand misses: 538K → 219K;
  - L1D useful prefetches: 77K → 395K, useless 38K → 54K;
  - L2→DRAM requests (demand + prefetch): 896K → 929K (+4%).

  omnetpp: L2→DRAM requests 489K → 543K (+11%).

## 2. Why the capability helps a delta prefetcher here

The earlier attempts (`claude/berti_cap_findings.md`) used the *authorizing* capability, i.e. the bounds, size and
identity of the object being accessed. Berti's useful prefetches cross objects (it learns the allocator's layout),
so bounds removed useful prefetches. Object contexts turned out too fine for object code and too coarse for arrays.
Part of that write-up's conclusion was: "the unique capability information is about pointers, and Berti predicts
addresses".

Berti-TDP uses the other half of CHERI: **tagged memory**. The tag bits that travel with every line say exactly which
16-byte slots hold pointers, and each pointer carries its target (the cursor) and the target object's bounds.
Measured in a 48 KB L1D model with no prefetching (`scripts/ptr_lead.cc`, 20M instructions after 10M):

| | xalancbmk | omnetpp | povray |
|---|---|---|---|
| L1D misses that dereference a recently loaded pointer | 99.8% | 99.8% | 98.5% |
| ... where the pointer's own line was a miss | 38.7% | 22.9% | 16.5% |

Purecap code reaches almost every heap line through a pointer read from another line. So which line gets
dereferenced next is written in lines the L1D is receiving. A delta prefetcher can't see this. A tag-aware L1D can,
when the line arrives, which is earlier than the dependent load.

Where the cycles come from (xalancbmk, final design):

- **Accuracy:** on 0–128 B objects, 88% of L1D-filled prefetches are used: 179K on time, 190K late (the demand merges
  with the in-flight TDP prefetch).
- **Lead time:** 73% of matched fill-to-dereference gaps are 4–32 cycles and 15% are 64 cycles or more, against
  ~190-cycle misses.
- **Parallelism:** a line with several pointers launches all of their targets while the core is still stalled
  behind the first.

On omnetpp, 175K of the 433K matched sightings (40%) are dereferenced 64 to more than 30,000 cycles after their line
arrived. Those are fully timely, but only if the sighting's CHT entry survives until the dereference; §5 and §6 show
why that matters.

## 3. Mechanism

```
     line arrives in L1D (demand fill, or fill of a Berti prefetch) -- its tag bits mark the pointer slots
                                     |
                       sighting queue (8 entries), 2 processed per cycle
                                     |
          CHT: drop if (base hash, cursor line) already present, else record {base hash, cursor line, key}
                                     |
          PDT[key = IP whose miss or prefetch brought the line in] -> confident deltas d (L1D / L2 level)
                                     |
          prefetch line(cursor) + d, unless it is already in flight

     tagged demand load that misses L1D (or hits a TDP-prefetched line) at line L, authorized by capability C:
          CHT entry with base hash(C.base) and |L - cursor line| <= 7, nearest  ->  credit d = L - cursor line
```

- **Sightings:** the up-to-4 tagged slots of every line filled into L1D, by a demand miss or by a Berti prefetch.
  Code capabilities (EXECUTE) and null pointers are skipped.
- **Key:** the 16-bit IP hash Berti already computes, for the IP whose miss brought the line in. For a Berti prefetch
  it is the prefetch's trigger IP, carried with the request.
- **Capability History Table (CHT):** 128 entries, FIFO, each {base hash, cursor line, key, trained-delta mask}. A
  sighting already present is dropped. Berti re-requests the same lines, so 15–45% of sightings are such repeats on
  the object-heavy traces.
- **Pointer Delta Table (PDT):** 64 entries × 4 deltas, with Berti's epoch-confidence bookkeeping but not its timeliness
  filter (see Training). Per key, every 16 sightings each delta's dereference count sets its level for the next epoch:
  ≥ 8 means prefetch into L1D, ≥ 4 into L2, otherwise nothing. (Berti's epoch counts an IP's misses and uses > 10 and
  > 8 of 16, so its score is coverage; here the epoch counts sightings, so the score is closer to accuracy.) Delta = dereferenced line − cursor line, |d| ≤ 7, covering fields past the cursor's line and
  interior pointers.
- **Training:** this uses the CHERI link between a dereference and its pointer. The dereferencing load's authorizing
  capability *is* the pointer it loaded, so its base names the sighting. Training happens on tagged demand loads that
  miss L1D or hit a TDP-prefetched line, i.e. Berti's own "would have missed" events. A dereference counts whether or
  not its sighting came a full fetch latency before it. Berti counts only such timely deltas, and that filter was
  tried here (§5): it costs 2 points, because late TDP prefetches still hide part of each miss.
- **Issue:**
  - Each confident delta issues line(cursor) + d, unless it is already in flight (Berti's latency table). It is not
    clamped to the pointed-to object's bounds. Clamping was tested (§5) and costs 3 points on xalancbmk: confident
    deltas that leave the object reach its allocation neighbours, the same effect that made bounds hurt Berti.
  - The L1D level obeys Berti's MSHR limit (70%).
  - TDP prefetches carry the pointer's capability and Berti's IP tag.
  - A TDP-prefetched line's own pointers are not scanned again (depth 1).
- **Sighting queue:** 8 entries, 2 sightings processed per cycle (one CHT search and one PDT lookup each). Overflow
  drops the oldest; that happened 50 times on xalancbmk and never on omnetpp or povray.

## 4. Hardware cost

Field widths as simulated:

| structure | entries × bits | bytes |
|---|---|---|
| CHT: valid, base hash 16, cursor line 12 (low bits; the base hash disambiguates), key 16, trained mask 15 | 128 × 60 | 960 |
| PDT: valid, tag 16, epoch count 4, LRU 6, 4 × {valid, delta 4, conf 5, level 2} | 64 × 75 | 600 |
| Sighting queue: cursor line 42, base hash 16, key 16 | 8 × 74 | 74 |
| TDP bit per L1D block (for training on TDP-prefetch hits) | 768 × 1 | 96 |
| TDP bit per PQ/MSHR entry (the IP tag is Berti's existing latency-table field) | 224 × 1 | 28 |
| **Total** | | **≈1.76 KB** |

That is ~69% of Berti's own ≈2.55 KB, under 4% of the 48 KB L1D, and less than half of the earlier OOB-filter design
(3.78 KB). Not simulated: 12-bit hashes and an 8-bit trained mask would bring it to about 1.5 KB. The smallest tested
point is the 64-entry CHT (§5).

Logic:
- The L1D fill path already carries the line's 4 tag bits. Reading a tagged slot's cursor and bounds is the same
  decode a capability load does.
- Per processed sighting: one 128-entry CAM search (base hash and cursor line), one 64-entry PDT lookup, and up to 4
  adders.
- Per tagged demand miss: one CHT CAM search (base hash, ±7-line window).
- Nothing happens on L1D hits. Prefetch addresses are virtual: the L1D prefetcher is `virtual_prefetch`, as for Berti.

## 5. Ablations (full windows, IPC change vs Berti)

Each row is one change to `berti_tdp_params.h`.

| variant | setting | xalancbmk | omnetpp | povray |
|---|---|---|---|---|
| **design**: fill sightings, IP key, CHT 128, PDT 64×4, no target clamp, 2 sightings/cycle + 8-entry queue | (as shipped) | +32.4% | +14.4% | +0.4% |
| + clamp TDP targets to the pointed-to object's bounds | `TDP_BOUNDS 1` | +29.1% | +14.8% | +0.5% |
| + sightings from demand load hits returning a capability | `TDP_SIGHT_LOAD_HIT 1` | +32.3% | +10.2% | +0.6% |
| load-hit sightings only, no fill sightings | `TDP_SIGHT_LOAD_HIT 1`, `TDP_SIGHT_FILL 0` | +4.0% | +2.7% | +0.2% |
| no PDT: prefetch every sighted pointer's cursor line into L1D | `TDP_NOLEARN 1` | +14.8% | +4.1% | +0.7% |
| + Berti's timeliness filter: credit a dereference miss only if its sighting was ≥ 1 measured fetch latency earlier | `TDP_TIMELY 1` | +30.4% | +12.7% | +0.3% |
| key = hash(IP, pointed-to object length) | `TDP_KEY_LEN 1` | +32.2% | +13.0% | +0.3% |
| CHT 64 entries (−480 B) | `TDP_CHT_SIZE 64` | +30.7% | +13.5% | +0.4% |
| unlimited sightings per cycle (no queue) | `TDP_SIGHT_PER_CYCLE 0` | +32.4% | +14.7% | +0.4% |

| scheduler latency 1 (instead of 5) | Berti IPC | Berti-TDP IPC | Berti-TDP vs Berti |
|---|---|---|---|
| xalancbmk | 0.3902 | 0.5155 | +32.1% |
| omnetpp | 0.6085 | 0.6958 | +14.3% |

Each row changes one thing from the design:

- **Learning is most of the gain.** Prefetching every sighted pointer's line with no PDT gets half the gain on
  xalancbmk (+14.8%) and under a third on omnetpp (+4.1%). The PDT decides which pointers are worth it, which line of
  the pointed-to object, and at which level.
- **Berti's timeliness filter doesn't carry over.** Crediting only dereferences whose pointer was sighted at least one
  measured fetch latency before the demand (decided at the line's fill, as Berti does) rejects 67% of dereferences on
  xalancbmk and 45% on omnetpp. TDP prefetches become more accurate (omnetpp's useless TDP fills: 44K → 12K), but
  the late-but-useful ones disappear: on xalancbmk the small-object prefetches used late fall from 190K to 7K and L2
  demand misses rise from 219K to 397K. Berti can afford the filter because a late delta gets replaced by a longer,
  timely one from the same IP's history. A TDP prefetch has a single trigger, the line's arrival, so filtering only removes
  prefetches.
- **Lines arriving in L1D are the right moment.** Load-hit sightings alone give +4.0% / +2.7%. Added to fill
  sightings, they cost 4 points on omnetpp: they are frequent and mostly redundant, and they push long-lived fill
  sightings out of the CHT before those are dereferenced.
- **The pointed-to object's extent doesn't help.** Clamping targets to its bounds costs 3.3 points on xalancbmk.
  Putting its length in the key splits training (−1.4 points on omnetpp).
- **Cheaper points exist.** A 64-entry CHT (≈1.28 KB in total) keeps +30.7% / +13.5%. Limiting processing to 2
  sightings per cycle (with the 8-entry queue) costs at most 0.3 points vs unlimited.
- **Not a scheduler artifact.** With scheduler latency 1 instead of 5, both Berti and Berti-TDP get faster, and the
  gain is unchanged (+32.1% / +14.3%).

## 6. How the design was reached

Two decisions were made on full windows (IPC change vs Berti, xalancbmk / omnetpp / povray):

| candidate | change | reading |
|---|---|---|
| fill + load-hit sightings, bounds clamp | +29.5% / +9.9% / +0.6% | |
| fill sightings only, bounds clamp | +29.1% / +14.8% / +0.5% | load-hit sightings flood the CHT and evict long-lived fill sightings |
| **fill sightings only, no clamp (final)** | **+32.4% / +14.4% / +0.4%** | out-of-object deltas reach allocation neighbours |

Earlier steps, on short windows (3M warmup + 3M instructions):

IPC. Berti = 0.4541 xalancbmk, 0.5377 omnetpp, 1.628 povray. All rows have the bounds clamp; the earlier rows ran on
intermediate code (a 64-entry CHT, a key with the object length, no sighting queue, sightings from load hits too).

| step | xalancbmk | omnetpp | povray | reading |
|---|---|---|---|---|
| TDP from fills and load hits, key = IP + target length, CHT 64 | 0.5293 | 0.5676 | 1.635 | already +17% / +6% |
| key = IP only | 0.5299 | 0.5777 | – | the length splits training |
| CHT 32 / 64 / 128 | 0.5288 / 0.5293 / 0.5314 | 0.5492 / 0.5676 / 0.5859 | – | sightings must survive until the dereference |
| thresholds 6/3, 8/4, 10/5 of 16 | 0.5296 / 0.5293 / 0.5290 | 0.5665 / 0.5676 / 0.5619 | – | flat, so Berti-like 8/4 kept |
| depth 2 (also scan TDP-prefetched lines) | 0.5311 | 0.5663 | 1.633 | +0.3% at most; dropped |
| IP key, CHT 128, ≤ 2 sightings/cycle, excess dropped | 0.5239 | 0.5916 | 1.635 | dropping costs; queue instead |
| IP key, CHT 128, 2/cycle with an 8-entry queue | 0.5962 | 0.6083 | – | |
| ... load-hit sightings under a separate key | 0.5937 | 0.5920 | – | |
| ... load-hit sightings only when the queue is idle | 0.5966 | 0.6120 | – | |
| ... no load-hit sightings | 0.5948 | 0.6252 | – | load hits flood the CHT |
| scheduler latency 1 instead of 5 (Berti / Berti-TDP, first row's design) | 0.4592 / 0.5301 (+15.4%) | 0.5421 / 0.5749 (+6.1%) | – | gain is not a slow-scheduler artifact |

Ideas tried and dropped:

| idea | outcome |
|---|---|
| 8-entry retry buffer for TDP prefetches refused by a full PQ | The PQ is nearly always full of Berti's requests: 759 of 21K retried requests got in. No gain. |
| Berti leaves 16 PQ slots free for TDP requests (a read-only `CACHE` accessor) | Still refused ~20K times. No gain: omnetpp 0.5661 vs 0.5676; the TDP-off control 0.5355 vs Berti 0.5377. Reverted. |
| TDP requests at the PQ head, displacing the youngest Berti request (a `CACHE` change) | Displaced Berti L1D prefetches stay in Berti's latency table forever, so xalancbmk hits an assertion. No gain on omnetpp or povray. Reverted; `CACHE` is untouched. |
| Scan resident lines hit by Berti's redundant prefetch requests | Built; its full runs were stopped when load-hit sightings turned out harmful (same CHT-flooding concern). Not in the design. |

## 7. Notes and caveats

- **Berti's history-table defect is kept.** `HistoryTable::add` in the repo's `berti.cc` reads `historyt[set][-1]`
  at slot 0. Fixing it moves Berti by +0.1% / +0.1% / 0.0% / −0.6% (xalancbmk / omnetpp / povray / neutron). Berti-TDP
  keeps the defect so that TDP is its only difference from the `berti` baseline. The fix is three lines, if both
  are to be fixed together.
- **Capability map.** TDP reads the tags and capabilities of an arriving line from `champsim::cap_mem`, which
  holds the presimpoint snapshot plus completed capability stores: what the line contains. Runs must not use
  `--skip-capmap`, which empties the map; with it, Berti-TDP equals Berti. Berti and no-prefetching were run the same
  way (the map doesn't affect them).
- **Pointer prefetching without CHERI exists.** Content-directed prefetchers (Cooksey et al., ASPLOS 2002;
  Ebrahimi et al., HPCA 2009) guess pointers from data values. These traces don't carry data values, so that
  comparison can't be run here. The claim is that *in this design* all of the gain comes from capability information
  (tags, cursors, bounds, authorizing capabilities), not that pointer prefetching needs CHERI. What CHERI adds over a
  value-guessing CDP:
  - no false pointers, because the tag is exact;
  - an exact dereference-to-pointer link for training (the authorizing capability);
  - no data-value comparators in the fill path.

  The pointed-to object's bounds and length were both tried, as a target clamp and as part of the key. Both cost
  performance (§5).
- **Depth.** TDP goes one level: a TDP-prefetched line that arrives before its demand is not scanned again. When
  the demand merges with it in flight, the fill counts as a demand fill and is scanned. Deeper, SPP-style signature
  lookahead was considered and not built. `scripts/ptr_chain.cc` (no prefetching, 48 KB L1D model) shows the
  dependent-miss chains are short:

  | | xalancbmk | omnetpp | povray |
  |---|---|---|---|
  | chains of 1 / 2 / 3 / ≥ 4 dependent misses | 61 / 31 / 8 / 0.2% | 77 / 21 / 2 / 0.4% | 84 / 15 / 2 / 0.2% |
  | median pointer hops to a dereference miss | 4 | 3 | 2 |
  | dependent-miss links under 32 instructions apart | 97% | 89% | 90% |

  Chains are deep in pointers, but the intermediate hops hit in L1D, and there is little work per node for a chaser
  to get ahead of. Workloads with long chains of cold nodes (tree or list walks, graphs) should be checked with the
  same tool before designing for depth.
- **Five traces, one SimPoint each.** The two object-heavy slices carry the gain. The SPEC archives suggest where else
  to look: 429.mcf / 605.mcf_s (arcs pointing to nodes), 473.astar, 620.omnetpp_s, 623.xalancbmk_s. On several of
  these Berti loses to no prefetching because of its own redundant traffic, which TDP doesn't change.
- `TDP TAGGED_LOAD_MISSES` counts `cache_operate` presentations, MSHR retries included. Use the cache's own counters
  for performance attribution.

## 8. TDP on other L1D prefetchers

`inc/cheri_tdp.h` packages TDP as an engine that any `virtual_prefetch` L1D prefetcher can add with about 6 lines of
hooks (`on_operate`, `on_fill`, `on_cycle`, OR `tag_prefetch(ip)` into the host's prefetch metadata, and keep
`cheri_tdp::MD_MASK` in the metadata the fill hook returns). It is Berti-TDP's CHT + PDT, with two differences: a
32-entry recent-target filter replaces Berti's latency table as the "already in flight" check, and the PDT key is an
11-bit IP hash so that it fits in metadata bits 21-31 (ipcp_cap uses bits 0-18). Modules: `prefetcher/ip_stride_cheri_tdp`
and `prefetcher/ipcp_cap_tdp` (configs `champsim_cheri_ip_stride_tdp_config.json`, `champsim_cheri_ipcp_cap_tdp_config.json`).

IPC (10M+20M; neutron 5M+10M), change vs no prefetching:

| config | xalancbmk | omnetpp | povray | neutron |
|---|---|---|---|---|
| ip_stride_cheri | 0.3569 (+7.0%) | 0.5159 (+1.3%) | 1.639 (-0.3%) | 1.423 (+8.3%) |
| ip_stride_cheri + TDP | 0.4556 (+36.7%) | 0.5952 (+16.9%) | 1.646 (+0.1%) | 1.423 (+8.3%) |
| ipcp_cap | 0.4129 (+23.8%) | 0.5153 (+1.2%) | 1.632 (-0.7%) | 1.321 (+0.5%) |
| ipcp_cap + TDP | 0.5167 (+55.0%) | 0.5896 (+15.8%) | 1.640 (-0.2%) | 1.321 (+0.5%) |
| Berti | 0.3871 (+16.1%) | 0.6010 (+18.1%) | 1.597 (-2.9%) | 1.423 (+8.3%) |
| Berti-TDP | 0.5126 (+53.7%) | 0.6877 (+35.1%) | 1.603 (-2.5%) | 1.423 (+8.3%) |

TDP over its host: ip_stride +27.7% / +15.4%, ipcp_cap +25.1% / +14.4% (xalancbmk / omnetpp), against Berti's
+32.4% / +14.4%. About 86% of TDP fills are useful on xalancbmk with either host. (`no_pf` also disables the L1I
next-line prefetcher that the other configs have; neutron's +8.3% is that, not the L1D prefetcher.)

**ip_stride_cheri table size.** The module's 256 x 4 = 1024 entries are ChampSim's stock `ip_stride` geometry, about
24 KB as simulated (three 64-bit fields). Intel's L1 IP-stride table is 24 entries, fully associative (Chen et al.,
"Leaking Control Flow Information via the Hardware Prefetcher"). Measured:

| ip_stride_cheri table | xalancbmk | omnetpp | povray | neutron |
|---|---|---|---|---|
| 1024 (256x4) | 0.3569 | 0.5159 | 1.639 | 1.423 |
| 64 (16x4) | 0.3569 | 0.5088 | 1.642 | 1.421 |
| 24, fully associative | 0.3568 | 0.5086 | 1.642 | 1.420 |
| 24 FA + TDP | 0.4556 | 0.5871 | 1.648 | 1.420 |

Going to 24 entries costs 1.4% on omnetpp (whose ip_stride gain was only +1.3% to begin with) and nothing elsewhere;
TDP's gain is unchanged. A 24-entry table at realistic widths (~12-bit tag, 12-bit line offset, 7-bit stride, 2-bit
confidence) is about 100 B.

**Small-object filter** (`has_prefetchable_range`: skip objects with at most one line on either side of the cursor).
At 1024 entries it is neutral (IPC within 0.02% on all four traces; without it the table takes 7.26M more lookups on
xalancbmk and hit rate drops 85.6% -> 76.6%, but prefetches barely change). At 24 entries it is essential: without it,
xalancbmk's small DOM nodes evict the strided objects, useful prefetches fall from 21.8K to 2.5K, and IPC drops
0.3568 -> 0.3342 (-6.3%, all of ip_stride's gain). Keep it.

## 9. Files

- `prefetcher/berti_tdp/berti_tdp.h`, `berti_tdp.cc`: Berti's `berti.h` / `berti.cc` with the class renamed, plus the
  TDP: its structures at the end of the class, its functions at the end of the `.cc`, and `#if TDP_ENABLE` blocks in the
  hooks.
- `prefetcher/berti_tdp/berti_tdp_params.h`: Berti's `berti_params.h`, plus TDP's sizes, thresholds and switches.
- `champsim_cheri_berti_tdp_config.json`: builds `bin/champsim_cheri_berti_tdp`.
- `inc/cheri_tdp.h`: TDP as a reusable engine (§8); `prefetcher/ip_stride_cheri_tdp/`, `prefetcher/ipcp_cap_tdp/`: the
  user's `ip_stride_cheri` and `ipcp_cap` with it added; configs `champsim_cheri_ip_stride_tdp_config.json`,
  `champsim_cheri_ipcp_cap_tdp_config.json`.
- `scripts/ptr_lead.cc`: the offline pointer-lead analysis of §2, plus same-page shares (2 MB / 4 KB).
- `scripts/ptr_chain.cc`: the offline pointer-chain analysis of §7.
- `test/cpp/src/465-berti-tdp-behavior.cc`: run through a real `CACHE`. `berti_tdp` requests a pointer's target before
  its dereference; `berti`, or `berti_tdp` with untagged authority, never does. Full suite: 670 test cases pass.
