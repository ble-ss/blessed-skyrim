# blessed skyrim: tuning report

the running ledger of numbers. every row names its run id, and the raw run folders were kept off the published tree. newest findings at the top of each section.

## the rig

| part | value |
|---|---|
| gpu | nvidia geforce rtx 3060 ti, 8 gb, driver 596.36 |
| cpu | amd ryzen 5 5600, 6 cores |
| ram | 32 gb |
| os | windows 11 iot enterprise ltsc 2024 (26100) |
| game | skyrim special edition, steam, `SkyrimSE.exe` 1.7.104.0 (2026-09-04), on `D:\SteamLibrary` |
| mod manager | mod organizer 2.5.2, `C:\Modding\MO2`, instance data `E:\mo2 together`, profile `Default` |

## state found on 2026-09-22

- the game is on d:, not c:. mo2's `gamePath` agrees.
- `SkyrimSE.exe` is 1.7.104, but the skse in the game folder is 2.2.6 (`skse64_1_6_1170.dll`), built for 1.6.1170. skse will not load on this exe as installed. fix: skse 2.3.1 targets 1.7.104 (confirmed on skse.silverlock.org, 2026-09-22), so no downgrade is needed for skse itself.
- 2026-09-22: skse 2.3.1 installed from nexus (downloaded from nexus). old loader, `skse64_1_6_1170.dll` and 63 `Data\Scripts` files backed up in `backup/game-dir-2026-09-22/`. its changelog says plugins must be rebuilt for 1.7.104 "due to the address library change".
- reshade 6.6.1 sits in the game folder as `dxgi.dll`. every native run so far would have had it loaded. it must be moved aside for clean baselines.
- the mo2 profile holds zero installed mods: base game, the three dlc, and the creation club content. that is a clean vanilla baseline.
- `iShadowMapResolutionPrimary=2048` in the profile `skyrim.ini`; no custom display keys yet.

## baselines

### campaign c1: native vs dxvk vs our fork, whiterun (2026-09-22, night)

setup: the `skybench` save (whiterun, noon, standing on the bridge), ultra preset, 1920x1080 borderless, vanilla + ae content, skse 2.3.1 + our `skybench.dll`, driver 596.36. 45 s capture after 15 s settle, 3 repeats per row, interleaved (a b c d e, e d c b a, ...). run ids `c1-<row>-r1..r3`, evidence in the runs' capture folders. plan `whiterun-save`. medians of per-run means:

| row | gpu busy | gpu busy p99 | frame p99 | vram | power | present path |
|---|---|---|---|---|---|---|
| native dx11 | **5.02 ms** | 5.55 ms | 16.51 ms | 2742 mb | 79.2 w | hardware: independent flip |
| dxvk 3.1.1 stock | 5.92 ms (+18%) | 6.56 ms | 16.60 ms | 3012 mb | 76.7 w | composed: copy with gpu gdi |
| dxvk, `cachedDynamicResources` off | 5.65 ms (+13%) | 6.15 ms | 16.57 ms | 3173 mb | 79.0 w | composed: copy with gpu gdi |
| our fork, features off | 5.88 ms (+17%) | 6.42 ms | 16.58 ms | 3237 mb | 79.3 w | composed: copy with gpu gdi |
| our fork + probe | 6.07 ms | 6.61 ms | 16.60 ms | 3236 mb | 79.4 w | composed: copy with gpu gdi |

**every row runs at 60 fps: skyrim caps itself** (cpu busy 16.4 ms = a spinning frame limiter; not vsync, `SyncInterval` 0). so fps and frame times are equal by construction, and gpu busy is the comparison. one outlier: `c1-dxvk-r2` p99 20.5 ms (a hitch), left in.

what it says:

- at the cap, dxvk costs about 0.9 ms more gpu per frame than native (+18%). **caveat: dxvk still presents through a composited copy** (the nvidia "prefer layered on dxgi" setting sits on a different driver profile than the one `SkyrimSE.exe` uses; fix pending), so part of that gap is the present path, not dxvk.
- the skyrim cache workaround that dxvk 3.1.1 turns on by default (`d3d11.cachedDynamicResources = a`) costs **+0.27 ms gpu** here: the gpu reads constant buffers from host-cached memory. it exists to save cpu; at a 60 fps cap on this rig the cpu has room, so it is a net loss we could flip per scene.
- our fork with its features off matches stock (5.88 vs 5.92 ms): the ray tracing plumbing costs nothing when idle.
- dxvk uses 270-495 mb more vram than native.

### campaign c2: what ray-traced sun shadows cost (2026-09-23, early morning)

same setup as c1 (whiterun save, ultra, 1080p, 60 fps cap, 45 s capture, 3 interleaved repeats, probe on in every row). all rows our fork. rt env: scene capture of the depth prepass, the sun from the shadow projection, one ray per pixel (plan `rt-shadows`). run ids `c2-<row>-r1..r3`.

| row | gpu busy | frame p99 | power | vram | dxvk worker busy | skyrim inside dxvk | raster draws skipped |
|---|---|---|---|---|---|---|---|
| fork, raster shadow maps | 6.07 ms | 16.63 ms | 79.5 w | 3231 mb | 3.85 ms | 0.241 ms | 0 |
| fork + rt sun shadows (cascades still drawn) | 6.68 ms | 16.72 ms | 99.2 w | 3270 mb | 4.27 ms | 0.269 ms | 0 |
| **fork + rt sun shadows, raster cascades skipped** | **6.04 ms** | 16.63 ms | **76.2 w** | 3274 mb | **3.46 ms** | 0.253 ms | **2,559 / frame** |

the rt pass itself: 0.75 ms (cascades drawn) / 0.84 ms (cascades skipped), from our own gpu timestamps (`probe/rtshadow.jsonl`, identical to 0.003 ms across repeats).

what it says:

- **ray-traced sun shadows cost nothing here.** skipping the 2,559 cascade draws per frame saves as much gpu as the ray pass costs (6.04 vs 6.07 ms), draws 3 w less, and takes 10% off dxvk's worker thread (3.85 → 3.46 ms).
- with the cascades still drawn, rt adds 0.61 ms gpu and, oddly, 20 w (99.2 w); worth a closer look (clock and utilisation telemetry are in the runs).
- the game thread still issues all 5,282 indexed draws: the engine walks the cascades, dxvk drops them. an skse hook could stop the walk itself.
- the ray-traced mask matches vanilla's within 3.4/255 mean absolute difference in this view (`docs/media/2026-09-23-first-light-whiterun-stable.png`).

### campaign c3: the whole blessed stack vs vanilla (2026-09-23, early morning)

rows: native dx11, our fork (features off), and **blessed**: rt sun shadows with the raster cascades skipped, soft shadows (sun disk + temporal + bilateral), npcs and creatures in the tlas (compute skinning), and gi v0 (ray-traced probe grid written into skyrim's own ambient). same setup as c1/c2, 3 interleaved repeats (plan `blessed-stack`, run ids `c3-<row>-r1..r3`). `c3-fork-r3` failed (the game exited at the main menu during warmup; not reproduced), so the fork row has 2 runs.

| row | fps | gpu busy | gpu p99 | frame p99 | power | vram |
|---|---|---|---|---|---|---|
| native dx11 | 60.7 | 5.01 ms | 5.43 ms | 16.51 ms | 79.1 w | 2763 mb |
| our fork, features off | 61.0 | 6.06 ms | 6.55 ms | 16.60 ms | 79.0 w | 3240 mb |
| **blessed stack** | **57.7** | 6.61 ms | 7.12 ms | **19.31 ms** | 76.7 w | 3286 mb |

**the blessed row dropped below 60 fps, and not from the gpu** (6.61 ms busy). our probe found it: `DrawIndexed` cost 4.1 ms per frame on the game's own thread (0.03 ms without our hooks). the gi hook reads its probe grid for every lighting draw (1,259 per frame, 8 probes each), and the readback ring sat in write-combined (uncached) memory, where cpu reads crawl. fix: host-cached memory for the ring (fork, after c3). one confirming run (`blessed-cached-1`, 30 s): **60.9 fps, frame p99 16.76 ms, gpu busy 6.55 ms**, game thread inside dxvk 1.74 ms (4.5 ms before). the remaining 1.5 ms of hook cost (gi lookups, skinning bone snapshots) is next; c3 gets re-run when gi v1 lands.

gpu cost of the whole stack at the cap: **6.55-6.61 ms vs 6.06 ms for the plain fork (+0.5 ms), and 5.01 ms native.** ray-traced shadows, soft, with npcs, plus ray-traced gi: half a millisecond over dxvk without them.

### campaign c4: uncapped (2026-09-23, 03:00)

the 60 fps cap hid every cpu difference: capped, every row reads 16.41 ms of cpu busy, because the frame limiter's sleep counts as busy. `--uncap` sets `bLockFramerate=0` and `iFPSClamp=0` in both inis and havok `fMaxTime` to 1/200 s. same save, ultra, 1080p, 45 s, 3 interleaved repeats (plan `uncapped`, run ids `c4-<row>-r1..r3`). the fork rows ran with the probe on.

| row | fps | 1% low | frame p99 | cpu busy p50 | gpu busy | power | game thread in dxvk |
|---|---|---|---|---|---|---|---|
| native dx11 | **222.7** | 179.3 | 5.58 ms | 4.22 ms | 4.47 ms | 198.8 w | - |
| dxvk 3.1.1 (stock) | 163.5 | 123.8 | 8.13 ms | 6.00 ms | 5.29 ms | 183.1 w | - |
| our fork, features off | **85.4** | 69.0 | 14.55 ms | 11.47 ms | 5.69 ms | 122.1 w | 0.20 ms |
| fork + rt shadows, cascades skipped | 78.7 | 60.0 | 18.38 ms | 12.27 ms | 5.67 ms | 107.8 w | 0.43 ms |
| blessed stack (gi v0) | 65.8 | 53.2 | 20.02 ms | 14.57 ms | 6.20 ms | 104.3 w | 1.65 ms |

what it says:

- **every row is cpu-bound.** gpu busy stays at 4.5-6.2 ms while frames take 4.5-15 ms.
- **native beats stock dxvk by 36% uncapped** (222.7 vs 163.5 fps): 1.8 ms more cpu per frame. part of it is the present path (dxvk still composes a copy; see the nvidia setting in `PROJECT.md`).
- **our fork is half as fast as stock dxvk with every feature off** (85.4 vs 163.5 fps): 5.5 ms more cpu per frame, yet our probe says the game thread spends only 0.20 ms inside dxvk's calls. the cost is somewhere the probe does not look (the probe itself, or the msvc build). campaign c5 splits it.
- rt shadows with the cascades skipped cost 0.8 ms of cpu here. gi v0's per-draw lookups cost another 2.3 ms (1.46 ms of it inside `DrawIndexed`); gi v1 replaces those lookups with a per-present copy.
- run-to-run spread is wide on the rt rows (`c4-fork-rt-skip`: 84.1, 64.6, 87.4 fps). the uncapped scene is less repeatable than the capped one.
- the shader cache was thrown out on every run: stock and fork write the same cache file with different version strings (`v3.1.1` vs `v3.1.1-65-g`). the harness now keeps one cache per build.

### correction: the probe's per-call times before 2026-09-23 03:30 were wrong

the first probe converted each call's duration to whole microseconds, so a ~50 ns call read as 0. every "game thread inside dxvk" number from c1 to c4 is far too low (c1 said 0.29 ms; the real figure is about 2.1 ms). the conclusion drawn from it ("dxvk's front end is cheap, the cost is the engine's own walk") was wrong too. it also cost 4.6 ms per frame to run, invisible under the 60 fps cap. call counts, the cs worker thread's busy time and presentmon numbers were never affected. fixed in fork `ea8537a4` (raw tsc ticks, calibrated per window, the probe's own cost subtracted, `BLESSED_PROBE_EVERY=N` to time one frame in N). the c1 table that stood here is withdrawn; the honest one is under c6.

### campaign c5: what the old probe cost (2026-09-23, 03:10)

uncapped, 30 s, 3 interleaved repeats (plan `fork-cpu`, `c5-<row>-r1..r3`).

| row | fps | cpu busy p50 | gpu busy |
|---|---|---|---|
| dxvk 3.1.1 (stock) | 159.8 | 6.10 ms | 5.32 ms |
| our fork, features off, no probe | **150.1** | 6.54 ms | 5.40 ms |
| our fork, features off, old probe | 88.6 | 11.16 ms | 5.67 ms |

the fork itself is 0.44 ms of cpu behind stock (msvc build, or the ray tracing extensions it enables). the old probe was the other 4.6 ms.

### campaign c6: the fixed probe, and gi v1 (2026-09-23, 03:40)

uncapped, 30 s, 3 interleaved repeats (plan `probe-gi`, `c6-<row>-r1..r3`). blessed = rt sun shadows with cascades skipped, soft shadows, npcs in the tlas, gi.

| row | fps | 1% low | frame p99 | cpu busy p50 | gpu busy | power |
|---|---|---|---|---|---|---|
| fork, features off | 150.8 | 119.1 | 8.40 ms | 6.54 ms | 5.38 ms | 172.7 w |
| fork, probe every frame | 136.1 | 108.9 | 9.18 ms | 7.24 ms | 5.49 ms | 161.8 w |
| fork, probe 1 frame in 8 | 143.2 | 111.4 | 9.13 ms | 6.79 ms | 5.44 ms | 166.9 w |
| **blessed, gi v1** | **101.1** | 85.9 | 11.65 ms | 9.79 ms | 6.07 ms | 135.5 w |
| blessed, gi v0 | 102.0 | 88.8 | 11.27 ms | 9.71 ms | 6.02 ms | 135.0 w |
| blessed, gi v1, probe 1 in 8 | 99.1 | 84.7 | 11.81 ms | 9.97 ms | 6.09 ms | 132.3 w |

the render thread, honestly (probe 1 frame in 8, median of in-world windows, per frame):

| call | fork, features off | blessed |
|---|---|---|
| skyrim's thread inside dxvk | **2.14 ms** | **4.98 ms** |
| `Map` (12,990 calls, nearly all `WRITE_DISCARD` on cbuffers) | 1.34 ms (103 ns each) | 1.42 ms |
| `DrawIndexed` (5,284 calls) | 0.12 ms (22 ns each) | 2.77 ms (525 ns each) |
| vertex/index buffer binds (11,400 calls) | 0.38 ms | 0.39 ms |
| `SetShaderResources` (3,870 calls) | 0.12 ms | 0.12 ms |
| dxvk cs worker thread busy | 3.35 ms | 3.65 ms |

what it says:

- **the render thread is worth fixing inside dxvk.** skyrim's thread spends a third of its 6.5 ms frame inside dxvk's calls, and `Map(DISCARD)` on constant buffers is two thirds of that. native dx11 runs the whole frame in 4.2 ms.
- **our own hooks are the biggest cost in the blessed stack.** the split (`hooks-split-1`): scene capture 1.72 ms (632 ns on each of 2,720 non-cascade draws), gi's ambient patch 0.91 ms (335 ns each), dxvk's own draw 0.03 ms. both run on every draw but want only one pass's. both are being worked on.
- the probe at 1 frame in 8 costs 0.25 ms per frame; it is the default for campaigns now. its nested scopes (map inside `Map`, the draw sub-buckets) inflate their parent: with `BLESSED_PROBE_NEST=0` `Map` reads **56 ns**, not 100 (`nest0-1` vs `nest1-1`). so the stock discard path is already near what we hoped to cut it to; the map-discard fix (plain inline flags, probe arguments computed only when sampling) is merged.
- the hook split after round one of the hook work (`hooks-split-2`): the pass check was never the cost. what costs is the work after it: scene capture's resolve 1,609 ns on each of 1,063 captured draws (1.71 ms), gi's patch 744 ns on each of 1,288 (0.96 ms). stack samples (our cpu sampler) put most of the capture time in the skinned path's per-draw copies of 3,840 bytes of bones. a second round is under way.
- **round two (`cd306da6`): the blessed stack 101.1 → 109.6 fps** uncapped (`hooks-noprobe-3`, one run; cpu busy 9.03 ms, gpu 5.95 ms). skinned bones go into a per-frame arena handed to the cs thread at present, skinned draws from passes after the mask are no longer copied, gi samples are memoised. same scene: 954 tlas instances, 1,193 gi patches per frame, 763 meshes coloured. split (`hooks-split-3`): scene capture 1.21 ms (static 0.32, skinned 0.87, of which 0.75 ms is 74 copied draws at 10 µs each, unexplained, round three on it), gi 0.83 ms (sampling 0.38).
- **round three (`69d8af88`): 110.7 fps** (`hooks-noprobe-4`). the split names the skinned cost: the arena is fine (no regrowth), but **reading the 3,840 bytes of bones off the mapped cbuffer takes 7.6 µs per draw** (about 500 mb/s: those bytes are not in the cpu cache when we read them, likely streaming stores by the game). round four moves bone reads to the gpu (the skin shader reads the draw's own cbuffer allocation by device address).
- **round four (`893c5252`): 118.7 fps** (`hooks-noprobe-5`). skyrim's thread never touches the bones now; the skinned path costs 0.02 ms per frame (was 0.88). same 954 instances, npcs render identically.
- round five (static captures batched to present, a per-object gi sample cache) was **neutral** (118.6 fps) and reverted: the static path still cost 301 ns per draw with no `EmitCs` at all, and a gi cache lookup (571 ns) cost more than the sample it saved (152 ns). what is left per draw is cold-cache latency (the game's own work between draws evicts whatever we touch), not instructions. kept on branch `hook-cpu-2` for reference.
- campaign c7 (uncapped, 45 s, 3 repeats, round four): noisy, three runs fell 20-30% below their siblings in every backend including stock dxvk, so the machine, not our code. medians: native 221.8, stock dxvk 159.5, fork 147.7, **blessed 118.6**. c8 re-runs it with 5 repeats.
- **campaign c8** (round four, uncapped, 45 s, 5 interleaved repeats, `c8-<row>-r1..r5`), medians: native 212.3 fps, stock dxvk 160.2 (two runs dipped to 89 and 140), fork 148.6, **blessed 117.4** (115.9-120.2, tight).
- **gi's patch moved to dxvk's cs thread (`512f04bf`): 131.0 fps** (`gics-noprobe-1`). skyrim's thread no longer samples the probe grid: a gi draw now rides dxvk's own batched draw command with a flag, and the cs thread writes the ambient into the draw's bound cbuffer allocation right before recording it (the gpu reads it after submission). gi on the game thread 0.86 → 0.15 ms per frame; the cs thread 3.5 → 4.24 ms busy (of an 8 ms frame). same image (mean difference 0.5/255), same 1,200 patches, same 763 meshes.
- **campaign c9** (gi on the cs thread, 5 repeats), medians: native 221.1, stock dxvk 166.1, fork 150.9, **blessed 133.8**. round two dipped in every backend (blessed 70.4, dxvk 84.3, native r1 184.2): machine noise, a background job on this box around 06:00.
- **static scene capture moved to the cs thread too (`4657d20c`): 143.5 fps** (`scenecs-noprobe-1`). the same flag-in-the-draw-command trick: skyrim's thread keeps only the cached pass check and the input-layout checks; the cs thread reads the bound vertex/index buffers, the transform and the camera from dxvk's own bound state. scene capture on the game thread 1.72 → 0.09 ms per frame; the cs thread 4.24 → 4.41 ms busy. same 954 tlas instances, same image (mean difference 0.2/255).

### campaign c10: the headline (2026-09-23, 06:10)

uncapped, whiterun save, ultra, 1080p, 45 s, 5 interleaved repeats (plan `uncapped-2`, `c10-<row>-r1..r5`), fork `4657d20c`, no probe. every row tight (no dips this time).

| row | fps | 1% low | frame p99 | cpu busy p50 | gpu busy | power |
|---|---|---|---|---|---|---|
| native dx11 | **221.5** | 182.8 | 5.47 ms | 4.33 ms | 4.50 ms | 199.2 w |
| dxvk 3.1.1 (stock) | 164.1 | 131.0 | 7.64 ms | 6.00 ms | 5.29 ms | 188.4 w |
| our fork, features off | 150.1 | 120.8 | 8.28 ms | 6.57 ms | 5.40 ms | 176.1 w |
| **blessed: rt sun shadows (cascades skipped), soft shadows, npcs in the tlas, rt gi v1** | **139.1** | 113.0 | 8.86 ms | 7.13 ms | 5.68 ms | 166.4 w |

what it says:

- **the whole ray-traced stack costs 7% of the frame rate and 0.28 ms of gpu** over our fork without it. this morning it cost 33% (101.1 vs 150.8 in c6).
- the fork is 0.57 ms of cpu behind stock dxvk (150.1 vs 164.1 fps): the msvc build or the ray tracing extensions it enables. a mingw build of the fork would split the two.
- native dx11 is still 1.7 ms of cpu per frame ahead of stock dxvk. dxvk's own calls on skyrim's thread (`Map(DISCARD)` 0.72 ms, binds ~0.5 ms) and the composited present path (the nvidia layered-present setting is still on the wrong profile) are the known parts.
- next on the render thread: the engine's cascade walk (+17% measured, needs the address library), then dxvk's own map/bind path.

### campaign c24: the critical levers, one at a time and together (2026-09-23, 21:30)

ultra 1080p, whiterun, uncapped, 30 s, 3 interleaved repeats (plan `critical-levers`, `c24-<row>-r1..r3`), llvm-mingw no-probe fork, layered present on. `perf` = cb ring + threaded front end. every run passed shadowcheck. two runs were redone (a warmup exit and a presentmon session clash).

| row | fps | 1% low | gpu busy | cpu busy |
|---|---|---|---|---|
| native dx11 | 215.8 | 167.2 | 4.54 ms | 4.48 ms |
| perf | 193.0 | 159.4 | 5.16 ms | 5.06 ms |
| perf + precipitation gate (`SKYBENCH_PRECIP_OCCLUSION=weather`) | 193.5 | 161.4 | 5.14 ms | 5.03 ms |
| perf + vol collapse (`BLESSED_VOL_COLLAPSE=1`) | 201.0 | 163.3 | 4.94 ms | 4.84 ms |
| perf + vol half-rate (`BLESSED_VOL_HALFRATE=1`) | 203.6 | 158.5 | 5.11 ms | 4.80 ms |
| **perf-max (all three)** | **207.9** | **165.5** | 4.91 ms | 4.67 ms |

- **the gap to native went from 10.6% to 3.7%** (22.8 fps to 7.9 fps; about 0.18 ms a frame).
- vol collapse is bit-identical (8 of 8 in-game checks, `lever-verify-1`): +4.1%. vol half-rate holds the volume one frame at a still camera: +5.5%. together +7.7%: they overlap, since half-rate skips the chain collapse speeds up.
- the precipitation gate is worth nothing measurable here (+0.3%).
- not in this campaign: the cascade walk hook (blessed only: it removes vanilla's sun shadows under `perf`), the rebar ring (dead: -30%), the cascade cache (fixed after this campaign, not yet measured).

### the front end's silent exits, found and fixed (campaigns c38-c44, 2026-09-24)

since the 18:52 build on 2026-09-23, about 1 perf run in 10 ended with the game gone and no windows error report. the chase, briefly:
- the fork's crash logger (last-chance + first-chance reports, fork `34328024`) named it: `0xc0000005` read of `0x28` on `dxvk-fe`, in `FePacketRec::Replay`'s cb-rename case, a null block.
- round 1 (count an op only after its fields are stored) and the context lock (c39, c43: 2/16 with the lock, 3/16 without) changed nothing; the op checker hid it (c38/c39: 0 in 24).
- round 2 added a crash-time dump of the front end's ring. all five c43 dumps: **an open packet had grown across the end of the 4 mb record ring.** after an allocation ended exactly on a lap boundary, the write limit jumped a whole lap ahead, the packet kept growing, its tail ops wrapped to the ring's start, and replay walked linearly past the end into zeros (op 0 = cb rename). the first wrap lands in the startup loading screen, ~7 s in, where most exits were.
- fix (fork `03d8cff2`): the limit is the lap of the last byte written; a packet never crosses the ring end. an allocator model: 56 crossings in 4 million steps before, 0 after.

| campaign | build | loading-screen runs | exits |
|---|---|---|---|
| c43 | before the fix | 32 | 5 (all `0xc0000005` on `dxvk-fe`, all a packet across the ring end) |
| **c44** | **the fix** | **32** | **0** |

### campaign c37: the cascade cache and the bridge, steam overlay off (2026-09-24, 20:30)

plan `critical-3`, ultra 1080p, 3 repeats (the clean redo after the overlay went off; crashes redone). every non-bridge run passed shadowcheck.

| row | fps | 1% low | gpu busy |
|---|---|---|---|
| native dx11 | 220.2 | 174.6 | 4.49 ms |
| perf-max | 209.3 | 176.0 | 4.89 ms |
| perf-max + cascade cache (full restore) | 208.5 | 169.4 | 4.92 ms |
| + cache, draw restore | 207.7 | 167.1 | 4.94 ms |
| + cache, partial restore | 208.0 | 155.3 | 4.93 ms |
| perf + the d3d12 present bridge | 189.8 | 166.7 | 5.54 ms |
| perf + bridge, steam overlay layer env off too | 189.6 | 168.8 | 5.55 ms |

- **perf-max: -5.0% on fps against native, +0.8% on 1% lows.**
- **the cascade cache still caches nothing** (`cached_draws` 0; 174/706 meshes stay "moving" after the treeparams fix), so the partial restore never ran and these rows only price the hooks. still open.
- **the bridge survives in game with the steam overlay off** (the overlay was its killer), but it costs 9% against perf. parked: bit 8 of the nvidia flag gives lower latency for less.

### campaign c80: the stock ladder, and a repeat of the headline (2026-09-28, 02:04)

plan `c80-ladder`, native, stock dxvk 3.1.1 (`--dxvk-build stock`, no fork keys) and perf-max (c76's full + p3), no mods, 6 repeats with r1 dropped; 36 of 36 ok, no device loss, all on hardware independent flip. round eight asked for it: the writeup's stock figure was cross-campaign.

| scene | row | median fps | 1% low | latency p50 | vs native, paired (95% ci) |
|---|---|---|---|---|---|
| whiterun | native | 219.0 | 176.4 | 9.6 ms | |
| whiterun | stock | 161.8 | 128.5 | 6.2 ms | -26.13% (-27.25 to -25.02), 0/5 |
| whiterun | **perf-max** | **246.0** | 176.1 | 4.6 ms | **+12.09% (+11.12 to +13.07), 5/5** |
| traversal | native | 240.5 | 216.2 | 12.4 ms | |
| traversal | stock | 205.7 | 193.5 | 5.6 ms | -14.57% (-14.90 to -14.24), 0/5 |
| traversal | **perf-max** | **251.4** | 217.1 | 4.7 ms | **+4.04% (+2.81 to +5.26), 5/5** |

- perf-max against stock, paired: whiterun +51.76% (+50.24 to +53.29), **1.52x**; traversal +21.78% (+20.76 to +22.79), **1.22x**.
- the headline reproduces independently of c76 (+12.09% / +4.04% against +10.97% / +4.37%). the 1% lows are level here (176.1 against 176.4, 217.1 against 216.2); c76's 1% low lead does not repeat, so it is reported as "ahead in c76, level in c80".
- a first try at a shared-clock latency test (`--input-taps 250`, scroll lock every 250 ms, merged from `input-taps`): presentmon never saw the taps (`c80s-native-save-taps`: 114 sent, 0 of 6,231 frames with `AllInputToPhotonLatency`). a camera test is in the backlog.

### campaign c79: the mod fair fight, community shaders and ngio (2026-09-28, 00:05)

plan `c79-mods`, native against perf-max (c76's full + p3), 6 repeats with r1 dropped; 47 of 48 ok (`c79-native-ngio-save-r1` kept its journal: ngio rewrites `GrassControl.ini`, fixed in `0fe9714`; r1 is the warm-up). **dtcsef** = display tweaks (tearing on, which uncaps native under cs) + community shaders 1.9 at its defaults + engine fixes 7.0.21, on both sides; **ngio** = no grass in objects ng 1.6.14 in live mode (no cache, ray casting on, vanilla density), on both sides, nothing else.

| scene | row | native fps / 1% low / latency | perf-max fps / 1% low / latency | vs native, paired (95% ci) |
|---|---|---|---|---|
| whiterun | dtcsef | 103.1 / 85.8 / 22.4 ms | 120.3 / 114.4 / 10.0 ms | **+16.64% (+16.41 to +16.86), 5/5** |
| traversal | dtcsef | 151.8 / 117.8 / 19.6 ms | 161.0 / 151.3 / 7.1 ms | **+6.05% (+5.52 to +6.57), 5/5** |
| whiterun | ngio | 218.3 / 170.9 / 9.6 ms | 243.2 / 169.6 / 4.7 ms | +12.91% (+9.42 to +16.39), 5/5 |
| traversal | ngio | 242.5 / 212.3 / 12.3 ms | 253.1 / 217.5 / 4.7 ms | +4.50% (+3.57 to +5.43), 5/5 |

(fps, 1% low and latency are medians over r2-r6; latency is presentmon `DisplayLatency` on each side's own clock, see round eight.)

- native runs uncapped on hardware independent flip in every row (frame p50 9.66 ms whiterun, 6.56 ms traversal under cs). the r2 `scene.png` pairs match: same scene, fog, god rays, braziers, npcs (both auditors looked).
- under community shaders the fork is not c76's fork: the volumetric collapse never learns its chain, flush-at-pass-end runs at its cap (122 render-target changes and 27-30 deferred flushes a frame against 12.9 and 0 in c76), zero-copy falls back on 1-2 frames. the far cascade still reuses every other frame; reflection reuse is configured but not shown in the logs. native loses nothing from this, so it doesn't favor us; the gain comes from a smaller lever set.
- native's cs 1% lows carry hitches (max frame 18.2 ms whiterun, 19.8 ms traversal, against 8.9 / 8.4 ms on the fork) in every repeat. cause not identified (first suspect: driver-side shader compiles on native; the run dirs don't archive `CommunityShaders.log`). the 1% low gap is real in the data and unexplained.
- ngio live costs neither side anything measurable against c76/c77; the 1% lows are level in whiterun (3/5).
- round eight (two independent audits) recomputed c76 and c77 from the run dirs: both reproduce to the second decimal.

### campaign c78: community shaders caps native at the refresh rate (2026-09-27, 22:28, stopped)

plan `c78-mods`, community shaders 1.9 with engine fixes 7.0.21 staged on both sides (`--engine-fixes`; the cs log now gets past its engine-fixes check). a one-time compile run first (`c78w-cs-compile`, `--warmup 3600`) filled the persisted cache: 3,577 shader files in the persisted shader cache, folder `075c1cdd28079a65` (file count). stopped after four runs, all void as a comparison:

| run | fps | present mode | frame interval p50 |
|---|---|---|---|
| `c78-native-csef-save-r1` | 75.0 | Hardware Composed: Independent Flip | 13.34 ms (gpu busy 9.44) |
| `c78-pm-csef-save-r1` | 119.2 | Hardware: Independent Flip | 8.39 ms |
| `c78-native-csef-traverse-r1` | 75.0 | Hardware Composed: Independent Flip | 13.34 ms (gpu busy 6.46) |
| `c78-pm-csef-traverse-r1` | 161.1 | Hardware: Independent Flip | |

- native was locked to the 75 hz refresh: community shaders rebuilds the swapchain (its log: "upgrading swap chain format from 28 to R10G10B10A2_UNORM") and native lands on the composed flip path without tearing. the fork presents its own way and is not capped. void; not reported.
- fix: display tweaks' `EnableTearing` uncaps native (c79s: native 102.0 fps on hardware independent flip, frame p50 9.72 ms with gpu busy 9.75 ms). the rerun is c79: display tweaks + community shaders + engine fixes on both sides.
- harness: engine fixes writes into `EngineFixes_SNCT.ini` at runtime and ngio rewrites `GrassControl.ini` on startup; restore now removes those staged copies by name (`54b17b0`, `0fe9714`).

### campaign c77: the mod fair fight, display tweaks (2026-09-27, 20:05)

plan `c77-mods`, the question: could a player just install perf mods on dx11 and beat us, and can we use the same mods? the same mod on both sides, native against perf-max (c76's full + p3), 6 repeats with r1 dropped; 48 of 48 ok, no device loss, no contamination. **dt** = display tweaks 0.5.25 (the mod files staged for the run, `dt-bench`: its stock ini with vsync off, no frame cap, tearing on); **cs** = community shaders 1.9 (the aio build of 2026-09-27).

| scene | mod | native fps / 1% low / latency | perf-max fps / 1% low / latency | vs native, paired (95% ci) |
|---|---|---|---|---|
| whiterun | dt | 220.3 / 174.3 / 10.0 ms | 246.6 / 179.2 / 4.7 ms | +12.07% (+11.04 to +13.10), 5/5 |
| traversal | dt | 241.0 / 215.3 / 12.4 ms | 250.2 / 216.6 / 4.7 ms | +4.12% (+3.55 to +4.69), 5/5 |

- **display tweaks runs on our stack and the lead holds**: +12.1% and +4.1% with it on both sides, against c76's +11.0% and +4.4% without it. on native it's worth about 1% (220.3 against c76's 218.1 in whiterun, 241.0 against 239.8 in the traversal), and it doesn't touch the display latency.
- **the cs rows are void.** community shaders loaded and then stood down at its first check: the last line of `CommunityShaders.log` is `Required DLL Data/SKSE/Plugins/EngineFixes.dll was missing`. those rows measured the plain game with an idle dll (219.9 and 240.5 fps, level with dt's native), so they're kept on disk and not reported. the rerun stages engine fixes 7.0.21 (`--engine-fixes`) on both sides (c78).

### campaign c76: the hardened perf-max stack (2026-09-27, 19:20)

plan `c76-final`, blessed `11cffcc1` (clean noprobe build: zero-copy, flush-at-pass-end, the hardened half-rate reflections `BLESSED_REFLECT_HALFRATE=1`, `BLESSED_VOL_PERIOD`), the plugin with the half-rate cascade at `SUNDEG=0.05`, 6 repeats with r1 dropped; 48 of 48 ok, no driver reset. **full** = perf-max with every lever above (reflections: the learned water cube only, motion- and age-gated, `_SWITCH=0` so the plugin's f8 switch isn't needed); **p3** = + `BLESSED_VOL_PERIOD=3` (the volumetric generate every third eligible frame, reviewed); **turn** = + `BLESSED_REFLECT_HALFRATE_MAXDEG=180` (a reflection cube doesn't change when the camera only turns).

| scene | row | median fps | 1% low | latency p50 | vs native, paired (95% ci) |
|---|---|---|---|---|---|
| whiterun | native | 218.1 | 164.2 | 9.8 ms | |
| whiterun | full | 239.7 | 168.0 | 4.8 ms | +9.63% (+7.85 to +11.41), 5/5 |
| whiterun | **full + p3** | **242.2** | **170.2** | **4.6 ms** | **+10.97% (+10.05 to +11.89), 5/5** |
| whiterun | full + turn | 241.1 | 169.3 | 4.8 ms | +10.51% (+9.85 to +11.17), 5/5 |
| traversal | native | 239.8 | 204.2 | 12.4 ms | |
| traversal | full | 245.0 | 214.3 | 4.8 ms | +2.55% (+1.90 to +3.20), 5/5 |
| traversal | **full + p3** | **250.7** | **216.2** | **4.7 ms** | **+4.37% (+3.70 to +5.03), 5/5** |
| traversal | full + turn | 247.1 | 214.9 | 4.7 ms | +2.96% (+1.96 to +3.96), 5/5 |

- **perf-max leads native in both scenes on every row**, fps, 1% lows and latency.
- period 3 against full, same build: traversal +1.77% (+0.71 to +2.84, 5/5), whiterun +1.24% (4/5). it's an image call: the volumetric light is up to two frames old on steady frames.
- the relaxed turn gate: +0.4% / +0.8%, inside noise, but free by construction.

### campaign c75: the cascade's sun tolerance (2026-09-27, 17:20)

plan `c75-sundeg`, perf-max with zero-copy, flush-at-pass-end, the half-rate cascade and half-rate reflections (c74's "both"), 6 repeats with r1 dropped; 36 of 36 ok. the cascade's sun gate at 0 (any change of the sun direction forces a render) against `SKYBENCH_CASCADE_HALFRATE_SUNDEG=0.05`: in single traversal runs the reuse went from 48 of 7,202 frames to 3,672 of 7,345 (every other frame), because the sun direction jitters by a hair every frame while flying.

| scene | row | median fps | 1% low | vs native, paired (95% ci) |
|---|---|---|---|---|
| whiterun | native | 218.0 | 160.3 | |
| whiterun | both, sun 0 | 239.8 | 170.5 | +9.85% (+9.29 to +10.40) |
| whiterun | **both, sun 0.05** | **242.4** | **173.0** | **+11.26% (+10.62 to +11.89), 5/5** |
| traversal | native | 239.5 | 199.0 | |
| traversal | both, sun 0 | 242.8 | 214.2 | +0.89% (-0.10 to +1.89) |
| traversal | **both, sun 0.05** | **247.4** | **218.6** | **+2.99% (+2.49 to +3.49), 5/5** |

- **perf-max leads native by 11% in whiterun and 3% in the traversal, 1% lows ahead in both.** a 0.05 degree tolerance bounds the far shadows' error to a twentieth of a degree for one frame.

### campaign c74: perf-max's temporal levers, half-rate far cascade and reflections (2026-09-27, 14:50)

plan `c74-temporal`, deployed blessed `87146656`-era noprobe (`5169652a`), zero-copy and flush-at-pass-end on, the `half-cascade` plugin (`e03c4f5`) for every row including native, 6 repeats with r1 dropped; 60 of 60 ok, no driver reset. **casc** = `SKYBENCH_CASCADE_HALFRATE=1` (the far sun cascade reused on alternate frames under motion, sun, split and coverage gates; the kept light transform restored for both atlas readers; still-camera screenshots on vs off differ no more than two runs of the same setting, shadowcheck ok). **refl** = c73's half-rate water reflections.

| scene | row | median fps | vs native, paired (95% ci) | vs base, same build |
|---|---|---|---|---|
| whiterun | native | 220.7 | | |
| whiterun | perf-max | 228.5 | +2.65% | |
| whiterun | + casc | 236.6 | +7.42% (+6.51 to +8.33) | +4.68%, 5/5 |
| whiterun | + refl | 231.8 | +4.47% | +1.81% |
| whiterun | **+ both** | **242.0** | **+9.65% (+9.38 to +9.91), 5/5** | +6.85%, 5/5 |
| traversal | native | 240.7 | | |
| traversal | perf-max | 237.4 | -1.22% | |
| traversal | + casc | 236.5 | -1.66% | -0.45% |
| traversal | **+ refl** | **242.0** | **+0.66% (+0.36 to +0.96), 5/5** | +1.91%, 5/5 |
| traversal | + both | 241.4 | +0.45% (-0.56 to +1.46), 4/5 | +1.69%, 5/5 |

- **perf-max beats native in both scenes.** whiterun +9.65% with both levers; the traversal +0.5 to +0.7%.
- **the cascade is a cpu win**: whiterun is game-thread-bound, and a reused frame skips ~2,280 caster registrations and submits. in the traversal it barely engages: the sun-direction gate forces most renders while flying (`forced_sun`, e.g. 861 of 1,066 in one window); a small sun tolerance (`SKYBENCH_CASCADE_HALFRATE_SUNDEG`) is the next knob.
- 1% lows: ours 175-178 vs native 172.8 in whiterun, 213 vs 209.9 in the traversal. latency 4.8-5.0 ms vs native's ~10-12.

### campaign c73: water reflections at half rate, perf-max passes native in the traversal (2026-09-27, 12:15)

plan `c73-reflect`, traversal, deployed blessed `87146656`, zero-copy and flush-at-pass-end on, 6 repeats with r1 dropped; 18 of 18 ok. **refl** = `BLESSED_HALFRATE=1 SKYBENCH_HALFRATE=1 BLESSED_HALFRATE_PARTS=reflect` (built 2026-09-23, never priced alone): the water reflection cube faces update every other frame (period 2; the log confirms `far=0 reflect=1 period=2`, on via the plugin's switch).

| row | median fps | vs native (239.3), paired (95% ci) | wins |
|---|---|---|---|
| perf-max | 236.5 | -1.35% (-1.77 to -0.93) | 0/5 |
| **perf-max + reflections at half rate** | **242.5** | **+1.23% (+0.40 to +2.07)** | **5/5** |

- **+2.62% against the same build** (+2.05 to +3.18, 5/5); latency unchanged (4.8 ms).
- **the image cost is a judgement call**: water reflections lag one frame on reused frames. the independent audit's round-seven caveats before shipping: the target recognition (a square rgba16f target with depth, within a size range) is broad, and the reflection cadence has no motion gate; per-face refresh age and fast turns and cell changes need a look.

### the layout census and c72: unified image layouts (2026-09-27, 10:00)

**census** (`layout-census` `f3246def`, `BLESSED_LAYOUT_CENSUS=1`, strict with zero-copy and flush-at-pass-end, one run per scene): per frame, attachment transitions at render pass begin 86.5 (traversal) / 93.1 (whiterun), restorations at render pass end 85.5 / 86, transfer transitions 8, queue-family pairs ~0, pipeline barrier calls 195.5 / 214.9 (102 / 109 layout-only), images restored then transitioned straight back with no read in between 13 / 13. nearly every render pass pays a transition in and out: the price of `dxvk.enableUnifiedImageLayouts = False`.

**c72** (plan `c72-unified`, traversal, deployed blessed `87146656`, zero-copy and flush-at-pass-end on, 6 repeats with r1 dropped; 30 of 30 ok): unified layouts on vs off, paired: perf-max +0.10% (-0.72 to +0.91), strict -0.05% (-0.86 to +0.76). **null**: the ~170 transitions a frame don't show. against native (240.5): perf-max -1.12% (on) / -1.21% (off), strict -5.6%.

### campaign c71: the water cube's shared depth and a fourth swapchain image (2026-09-27, 08:20)

plan `c71-cube-img4`, traversal, the `cube-port` dll (blessed `87146656` + cubedepth + `BLESSED_SWAPCHAIN_IMAGES`) via `--dxvk-build fork-bisect`, zero-copy and flush-at-pass-end on in every fork row, 6 repeats with r1 dropped; 42 of 42 ok.

| row | median fps | vs native (240.6), paired | vs base, same dll |
|---|---|---|---|
| perf-max base | 237.4 | -1.50% (-2.00 to -1.00) | |
| perf-max + `BLESSED_CUBE_DEPTH=1` | 236.9 | -1.64% | -0.14% (-0.85 to +0.57) |
| perf-max + `BLESSED_SWAPCHAIN_IMAGES=4` | 237.0 | -1.61% | -0.10% (-0.52 to +0.32) |
| perf-max + both | 237.0 | -1.49% | +0.01% |
| strict base / both | 227.8 / 226.6 | -5.54% / -5.79% | -0.27% |

- **both null; closed.** the cube fix is exact (vvl clean, identical faces) and doesn't show; a fourth image doesn't remove the acquire idle's cost from the frame.
- the traversal with zero-copy and flush-at-pass-end: **perf-max -1.5%, strict -5.5%**.

### campaign c70: flushes at the render pass end (2026-09-27, 06:20)

plan `c70-fpe`, the `flush-pass-end` dll (blessed `db013efa` + `dc12327b`, reviewed) via `--dxvk-build fork-bisect`, zero-copy on in every fork row, 6 repeats with r1 dropped; 60 of 60 ok, no device loss, no driver reset. **fpe** = `BLESSED_FLUSH_AT_PASS_END=1`: weak implicit flushes wait for the next render target change instead of splitting an open render pass (in game: mid-pass flushes 3-4.6 → ~0 a frame, ~16 deferrals a frame, no gpu idle from them, the chunk cap rarely hit).

| scene | row | median fps | vs native, paired (95% ci) | fpe vs off, same dll |
|---|---|---|---|---|
| whiterun | native | 222.1 | | |
| whiterun | perf-max off / **fpe** | 225.9 / **230.3** | +2.00% / **+3.51% (+3.08 to +3.94)** | **+1.48% (+0.48 to +2.49), 5/5** |
| whiterun | strict off / **fpe** | 220.5 / **221.9** | -0.54% / **+0.02%** | +0.57% (-0.18 to +1.32), 4/5 |
| traversal | native | 240.8 | | |
| traversal | perf-max off / fpe | 236.4 / 235.8 | -2.36% / -1.79% | +0.61% (-1.74 to +2.96) |
| traversal | strict off / fpe | 226.6 / 227.0 | -6.05% / -5.65% | +0.43% (-0.26 to +1.11), 4/5 |

- **a real win in whiterun**: perf-max +3.5% over native, strict dead level. merged (blessed `87146656`, off by default; the production conf sets it).
- the traversal moves a little and stays short: perf-max -1.8%, strict -5.7%. per the round-six audit, what's left there is mostly the layered present's hand-off tail.

### c68 and c69: pass timing on the current build (2026-09-27, 04:20)

blessed `db013efa` (zero-copy on for the fork rows), `--gpu-passes 1` against native `--native-timing`, 2 repeats each. the round-six audit reads the traversal pair in full: strict `gpu_ms_all` +0.23 ms over native, the per-pass **floor** deltas sum to +0.002 / +0.000 ms, and the gap is the present hand-off's tail landing in the prepass, far cascade and volumetric generate.

- **whiterun (c69), strict with zero-copy: our gpu frame is 0.376 ms lighter than native's** (4.48 vs 4.86 ms, modal frames; the far cascade +0.058 is the only large item). strict still only ties native in fps there, so whiterun is set by the game thread for both backends (cpu busy ~4.4 ms each), not the gpu.
- **`BLESSED_NO_TRANSFER_QUEUE=1` (strict traversal):** `idle_by.sdma` 0.035 → 0 but an equal unattributed idle appears (`idle_by.none` 0.046) and `gpu_ms_all` gets worse (4.607 vs 4.549). the "sdma wait" is at least partly the graphics queue starving for submitted work.
- **`BLESSED_COPY_FB=1` at pass resolution (strict traversal):** `gpu_ms_all` 4.553 vs 4.549; confirms c66's null.

### campaign c67: zero-copy present (2026-09-27, 02:50)

plan `c67-zerocopy`, test build `r5` via `--dxvk-build fork-bisect`, 6 repeats with r1 dropped; 60 of 60 ok, no device loss, no driver reset. **zc** = `BLESSED_ZERO_COPY=1` (reviewed, 15 fixes): the frame's back-buffer render targets are redirected into the acquired swapchain image and the present blit is skipped; the hud overlays arrive intact (`r5-zerocopy-traverse/scene_end.png`), still a hardware independent flip, no fallback lines in game.

| scene | row | median fps | vs native, paired (95% ci) | zc vs off, same dll | latency p50 |
|---|---|---|---|---|---|
| whiterun | native | 220.9 | | | |
| whiterun | perf-max off / zc | 224.5 / 224.0 | +1.32% / +1.16% | -0.15% (-1.40 to +1.10) | 5.2 ms |
| whiterun | strict off / zc | 218.6 / 219.5 | -1.20% / -0.74% | +0.46% (-0.22 to +1.14), 4/5 | 5.1 ms |
| traversal | native | 240.7 | | | |
| traversal | perf-max off / zc | 235.1 / **236.6** | -2.37% / **-1.67%** | **+0.73% (-0.01 to +1.46), 5/5** | 4.9 ms |
| traversal | strict off / zc | 225.1 / 225.9 | -6.32% / -5.96% | **+0.39% (+0.03 to +0.75), 5/5** | 5.1 ms |

- **a small real gain, largest on the traversal** (~0.03 ms, the blit's measured cost). latency unchanged. merged into blessed (off by default until a longer soak).
- the traversal is still short: perf-max -1.7%, strict -6%.

### campaign c66: the framebuffer copy (2026-09-27, 00:30)

plan `c66-fbcopy`, test build `r5` (blessed `b7d33f98`, getter shadow off, + `fb-copy` + `zero-copy`, both off unless switched) via `--dxvk-build fork-bisect`, 6 repeats with r1 dropped; 60 of 60 ok, **no device loss in 20 whiterun dxvk runs** (the shadow fix holds). **fb** = `BLESSED_COPY_FB=1`: full same-format `R16G16B16A16_FLOAT` copies (both hdr copies) go through dxvk's fragment-shader copy with uint views; bit-exact on every float16 pattern (test), ~4,600 copies per 5 s in game, 0 fallbacks.

| scene | row | median fps | vs native, paired (95% ci) | fb vs off, same dll |
|---|---|---|---|---|
| whiterun | native | 220.7 | | |
| whiterun | perf-max off / fb | 227.9 / 226.8 | +2.34% / +2.51% | +0.17% (-1.28 to +1.63) |
| whiterun | strict off / fb | 219.8 / 217.7 | -0.75% / -1.32% | -0.58% (-1.34 to +0.19) |
| traversal | native | 240.4 | | |
| traversal | perf-max off / fb | 235.2 / 234.5 | -2.16% / -2.38% | -0.23% (-1.04 to +0.59) |
| traversal | strict off / fb | 226.3 / 225.3 | -5.87% / -6.16% | -0.31% (-1.05 to +0.44) |

- **no gain**: the synthetic's -0.019 ms per copy (and -0.022 on its reader) doesn't show in the frame. closed; the fb-copy branch stays unmerged.
- standings on the stable build: whiterun perf-max +2.3% (4/5), strict -0.75% (ci touching zero); traversal perf-max -2.2%, strict -5.9%.

### the whiterun device losses were the getter shadow (2026-09-27, 22:00)

c65 (the 10-pair confirmation on `93468542`) lost the device in 4 of its first 9 whiterun dxvk runs, with five nvidia driver resets; it was stopped after 27 of 66 runs. tabulating every whiterun dxvk run since c58 by configuration:

| whiterun, dxvk | runs | device lost |
|---|---|---|
| before the getter shadow existed (c59, c61) | 57 | 0 |
| c62, same dll, shadow off (`BLESSED_FE_OM_SHADOW=0`) | 12 | 0 |
| c62, same dll, shadow on | 12 | 4 |
| shadow on, c62-c65 (incl. the k8 bisect dll) | 47 | 13 |
| every traversal run, any setting | 131 | 0 |

- **the om getter shadow (`fee8fb66`) makes whiterun lose the device about one run in four.** the record-time shadow update ran in the clean drain rows too, so the fault is in the getters answering from it (a view handed back after its last reference went to the worker's deferred release is the first suspect). `BLESSED_FE_SHADOW_VERIFY` hid it: verify drains on every getter.
- **correction:** the c62 note above blamed a stale incremental build for its four losses. that was wrong; the five clean whiterun runs of the rebuilt dll were luck at a ~25% loss rate.
- **fixed by switching it off** (blessed `b7d33f98`: the shadow runs only with `BLESSED_FE_OM_SHADOW=1`); it bought no fps (c62). the tweet's numbers (c61) predate it.

### campaign c64: async volumetrics priced, and closed (2026-09-27, 19:00)

plan `c64-async`, candidate `k8` (blessed `93468542` + the volasync branch's fixes through `c58a5b14`) via `--dxvk-build fork-bisect`, 4 repeats with r1 dropped. **async2** = `BLESSED_VOL_ASYNC=2` (generate on a second graphics-family queue, the chain on the compute-only queue).

| scene | row | median fps | paired vs sync on the same dll |
|---|---|---|---|
| traversal | perf-max sync / async2 | 233.9 / 232.3 | **-0.96% (-1.72 to -0.20)**, 3/3 slower |
| traversal | strict sync / async2 | 225.9 / 221.0 | **-1.87% (-2.97 to -0.76)**, 3/3 slower |
| whiterun | perf-max sync / async2 | 228.0 / 224.2 | -1.67%, n=2 |

- **async is slower here, in both modes of the frame.** the synthetic's gain (-0.09 to -0.14 ms) doesn't survive the real frame: the second queue competes with the passes it was meant to hide behind.
- **and the k8 dll isn't stable**: five whiterun runs failed, four with `VK_ERROR_DEVICE_LOST`, including two **sync** rows (async off), and eight nvidia driver resets (event 153) between 18:02 and 18:58. the deployed `93468542` ran every c63 block and today's other runs without a loss.
- **closed**: async volumetrics stays out of blessed; the volasync branch's later fixes (`efb8bba8`, `04334a7b`, `c58a5b14`, `775761c8`) stay unmerged. the real finds that came out of it are already in blessed (`DxvkCommandList::next()`, verify's init order).

### campaign c63: the "treat dxvk as native" flag values (2026-09-27)

plan `c63-flags`, fork `93468542` (clean noprobe), perf-max with volgen and dbuf-uoff, 2 repeats per block. we set nvidia's `Vulkan/OpenGL Present Method - Flags` (`0x20324987`) by hand in profile inspector between blocks; bit 8 before and after as the control. every run on a hardware independent flip.

| flag value | perf-max whiterun | perf-max traversal | display latency p50 | native whiterun / traversal |
|---|---|---|---|---|
| `0x00080104` (bit 8, before) | 227.7 / 227.2 | 234.4 / 235.1 | 4.9-5.0 ms | 214.8 / 217.8 · 239.7 / 241.2 |
| `0x000803A5` | 221.8 / 228.8 | 235.1 / 234.1 | 5.0-5.2 ms | 214.3 / 217.3 · 241.4 / 238.8 |
| `0x000802A5` | 225.7 / 224.8 | 231.3 / 231.4 | **10.3-10.9 ms** | 220.9 / 220.1 · 240.3 / 239.1 |
| `0x00080104` (bit 8, after) | 226.8 / 228.4 | 236.1 / 233.8 | 4.9-5.2 ms | 219.0 / 218.3 · 240.6 / 240.8 |

- **neither value helps.** `0x803A5` (which keeps bit 8) matches bit 8; `0x802A5` (without bit 8) loses ~1.5% and doubles display latency, as bit 8's absence predicts. bit 8 stays the standing value. the present hand-off lever is closed on the driver side.
- one nvidia driver reset (event 153) at 17:03 fell inside `c63-f803a5-native-save-r2`, a native run with `0x803A5` set, probably during its load; the run itself completed normally (217.3). a mild strike against that value, not an async effect.
- the first block (`c63-bit8a`) is void: presentmon recorded nothing machine-wide after the afternoon's driver resets; a restart fixed it.

### campaign c62: the front end's getter shadow, and where the traversal waits now (2026-09-26, 13:00)

plan `c62-fe-shadow`, fork `fd581f9e` (noprobe), c61's configurations with volgen, 6 repeats with r1 dropped. **shadow** = `OMGetRenderTargets*` answered from an app-thread copy of the bindings (`fee8fb66`, reviewed; `BLESSED_FE_SHADOW_VERIFY` found 0 mismatches in 5.57 million slot comparisons in game); **drain** = `BLESSED_FE_OM_SHADOW=0`, the old getters.

| scene | row | median fps | vs native, paired (95% ci) | wins |
|---|---|---|---|---|
| whiterun | native dx11 | 220.1 | | |
| whiterun | **perf-max, shadow** | **226.2** | **+2.48% (+1.23 to +3.73), n=3** | **3/3** |
| whiterun | perf-max, drain | 226.1 | +2.60% (+1.88 to +3.32) | 5/5 |
| whiterun | strict, shadow | 219.6 | -0.50% (-1.50 to +0.50), n=4 | 1/4 |
| traversal | native dx11 | 241.3 | | |
| traversal | perf-max, shadow | 236.4 | -2.17% (-2.78 to -1.56) | 0/5 |
| traversal | strict, shadow | 226.5 | -6.38% | 0/5 |

- **correction (independent audit, round four):** four whiterun shadow runs (`pm r5`, `pm r6`, `strict r1`, `strict r5`) lost the device mid-capture and still passed the old gate; `pm r6` kept 2.3 s of frames and reported 228.7 fps. they are removed above (the first table had +3.03%). the gate now fails a run on any device loss, under 85% capture coverage, or a frame interval over 1 s (`0243212`). the losses fit the stale incremental build: a clean build of the same code ran whiterun five times clean, with 0 shadow mismatches in 5.5 million comparisons (`shadowcheck-*`). c58, c59 and c61 have no hidden failures. confidence intervals use t for n-1 degrees of freedom from here on (the tables above used 2.57 for five pairs; 2.78 is right).
- **shadow against drain, paired: no measurable change** (perf-max +0.42% / +0.09%, strict +0.31% / -0.27%, every ci across zero). the drain was where the main thread waited, not what it waited for.
- **a second cpu sample with the shadow on** (`scratchpad/cpuprof-shadow-trav.txt`): the main thread now waits inside `Present` (`D3D11SwapChain::BlessedRecordPresent`, 17 of 60 samples), the normal gpu-bound backpressure. the traversal is gpu-limited; perf-max's gap there is ~0.086 ms of frame time.
- kept: the shadow is exact and removes a wait the next bottleneck would otherwise hide behind.

**warning, same day:** blessed `c7876c05` / `dcf3c674` (the volasync merge) loses the device right after the save loads, with async off (`bisect-base`); `fd581f9e` runs clean (`bisect-good-fd581f9e`). **correction (13:30): the code was fine, the build was stale.** the same commit rebuilt from scratch runs clean (`bisect-k0`); the incremental `build.mingw-noprobe` had built across a merge that changed class layouts. deployed now: a clean build of `93468542`. `BLESSED_VOL_ASYNC=1` itself still loses the device in game on the clean build, in every configuration tried (`clean-async1*`); still open.

### campaign c61: the volumetric generate reshaped, exact (2026-09-26, 10:20)

plan `c61-volgen`, ultra 1080p, bit 8, hags on, fork `8af12a4a` (noprobe), descriptor buffers + unified layouts off on every dxvk row, 6 interleaved repeats with r1 dropped (5 measured). **volgen** = the replacement shader folder `blur2-volgen-8x8`: the two blur replacements plus vanilla's volumetric generate `cs.ab674eb1` with only its thread-group declaration changed (32x32x1 to 8x8x1, dxbc checksum recomputed) and its dispatch rewritten 10x6x90 to 40x24x90 by a `.dispatch` sidecar. the shader reads only its global thread id (no group ids, no groupshared, no barriers), so every invocation computes exactly what it did; the harness confirms both output volumes byte-identical, with a negative control that does differ. in the harness the dispatch drops from 0.47-0.56 to 0.335 ms.

| scene | row | median fps | vs native, paired (95% ci) | wins |
|---|---|---|---|---|
| whiterun | native dx11 | 218.9 | | |
| whiterun | perf-max | 223.5 | +2.01% (+1.77 to +2.25) | 5/5 |
| whiterun | **perf-max + volgen** | **223.8** | **+2.35% (+1.97 to +2.72)** | **5/5** |
| whiterun | strict | 216.2 | -1.37% (-1.98 to -0.75) | 0/5 |
| whiterun | **strict + volgen** | **217.6** | **-0.47% (-0.97 to +0.04)** | 1/5 |
| traversal | native dx11 | 241.0 | | |
| traversal | perf-max | 233.9 | -2.83% (-3.62 to -2.05) | 0/5 |
| traversal | perf-max + volgen | 234.8 | -2.32% (-2.99 to -1.64) | 0/5 |
| traversal | strict | 223.3 | -7.14% | 0/5 |
| traversal | strict + volgen | 225.1 | -6.44% | 0/5 |

- **volgen against its own base, paired:** strict whiterun +0.92% (+0.32 to +1.51, 5/5), strict traversal +0.75% (-0.00 to +1.50), perf-max +0.34% and +0.53% (half-rate runs the generate every other frame, so half the gain).
- **strict is now within half a percent of native in whiterun**, and its ci touches zero.
- the traversal is unchanged in kind: its limit is the game thread (the front end's getter drains), not gpu work.

### c60 and the far-cascade cull count: where the traversal's gap sits (2026-09-26, 07:40)

**c60** (plan `passes-c60`): per-pass gpu time, native (`--native-timing`) against perf-max dbuf-uoff (`--gpu-passes 1`), 2 repeats each, game frames only.

- **traversal: +0.234 ms of gpu over native.** depth prepass `5c1cdfaf` (~900 draws) +0.077 (15.8%); `556a3b73` (gbuffer-ish, ~167 draws) 0.019 → 0.075 ms (+295%); alpha-tested casters `489864ca` in the atlas +0.056 (34%); a transfer pass +0.034; the water cube `79c17b07` +0.029; `6baabfc5` +0.023; `83560015` +0.023; dxvk's present blit +0.040.
- **whiterun: -0.057 ms, we're faster overall**; the far cascade `08e79d9c` +0.144 (bit 8 parks the present round trip there) and `556a3b73` +0.065 are the only large items.

**the far-cascade cull, stage 0** (`wt/far-cull` `906a142`, fix of the first build; runs `farcull-diag2-save`, `farcull-count2-traverse`): every frame's far-cascade region derives (`state_ok` = frames, no rejects). casters whose shadow can only land where the mask reads cascade 0 (view depth < `StartSplitDistances.y` = 933 units): **13% of far registrations in whiterun (~389 a frame), 26-29% in the traversal.** stage 1 (the skip) is built, off, and unproven: proving it changes no pixel is round three's open question.

### campaign c59: descriptor buffers warm, whiterun and traversal (2026-09-26, 06:50)

plan `c59`, ultra 1080p, bit 8, hags on, fork `3e9e81ce` (noprobe), 11 interleaved repeats with r1 dropped as the warm-up; 88 of 88 ok. **dbuf-uoff** = `dxvk.enableDescriptorHeap = False` (descriptor buffers) plus `dxvk.enableUnifiedImageLayouts = False`. every dxvk row has the blur replacements.

| scene | row | median fps | vs native, paired (95% ci) | wins | 1% low | latency p50 |
|---|---|---|---|---|---|---|
| whiterun | native dx11 | 221.0 | | | 174.7 | 10.0 ms |
| whiterun | perf-max | 222.5 | +0.86% (+0.32 to +1.40) | 9/10 | 171.9 | 5.2 ms |
| whiterun | **perf-max, dbuf-uoff** | **224.5** | **+1.45% (+0.55 to +2.35)** | **9/10** | 167.4 | 5.2 ms |
| whiterun | strict, dbuf-uoff | 217.7 | -1.27% (-2.03 to -0.51) | 1/10 | 171.2 | 5.1 ms |
| traversal | native dx11 | 241.0 | | | 213.9 | 12.4 ms |
| traversal | perf-max | 231.9 | -3.87% (-4.09 to -3.66) | 0/10 | 207.7 | 4.9 ms |
| traversal | perf-max, dbuf-uoff | 233.5 | -3.19% (-3.42 to -2.96) | 0/10 | 209.1 | 4.9 ms |
| traversal | strict (heap) | 221.3 | -8.20% (-8.39 to -8.01) | 0/10 | 205.9 | 5.2 ms |

- **c58's win repeats**: perf-max beats native in whiterun again, and warm descriptor buffers with unified layouts off widen it to +1.45%. strict improves from -2.0% (c58, heap) to -1.3%.
- **the traversal is the wall**: every configuration loses, 3.2% at best. half-rate volumetrics is worth ~4.6% there (perf-max vs strict), far more than in whiterun.
- with warm caches the descriptor-buffer rows are as steady as the heap's (whiterun sd ~2 fps once one early outlier is set aside).

### campaign c58: perf-max passes native in whiterun, paired (2026-09-26, 03:20)

plan `primed`, whiterun, ultra 1080p, bit 8, hags on, the fork at `3e9e81ce` (noprobe). 11 interleaved repeats; **r1 of every row is its warm-up and is dropped**, so each row is 10 measured runs. paired = each run against native's run in the same repeat.

| row | median fps | vs native, paired mean (95% ci) | wins | 1% low | latency p50 |
|---|---|---|---|---|---|
| native dx11 | 219.2 | | | 170.4 | 9.5 ms |
| **perf-max + blur replacements** | **221.5** | **+1.23% (+0.38 to +2.07)** | **8/10** | 165.2 | 5.2 ms |
| strict + blur, descriptor heap | 215.0 | -1.96% (-2.84 to -1.08) | 1/10 | 166.8 | 5.2 ms |
| strict + blur, `dxvk.enableUnifiedImageLayouts = False` | 215.5 | -1.49% (-2.06 to -0.92) | 0/10 | 165.4 | 5.2 ms |
| strict + blur, descriptor buffer (`dxvk.enableDescriptorHeap = False`) | 216.4 | -2.02% (-3.71 to -0.34) | 2/10 | 165.8 | 5.1 ms |
| strict + blur, `BLESSED_CB_MIRROR_EARLY=1` | device lost, 11 of 11 | | | | |

- **perf-max with the blur replacements beats native at ultra 1080p in whiterun**, with the confidence interval above zero. its image is vanilla's except the volumetric light, which is vanilla's at half rate (temporal reuse); strict is still 2% behind.
- **descriptor buffers keep warming across runs**: 205.5, 209.4, 216.9, 215.9, 211.4, 202.6, then 219.2, 216.0, 218.1, 219.2, 219.0. the last five sit at native, and above the heap's 212-217. an independent audit was right that one warm-up isn't enough; c59 reruns it warm.
- **unified image layouts off** is +0.49% over the heap (ci -0.33 to +1.31, 7/10): not proven; carried into c59.
- **the early mirror split loses the device** in every run (`VK_ERROR_DEVICE_LOST` in `c58-strict-mirror-early-*/SkyrimSE_d3d11_crash.log`). parked; it was worth 0.03-0.08 ms at best.
- 1% lows: native's are higher here (170 vs 165-167), the reverse of c53. the hitch that sets them is worth a look before any posting.

### campaign c57-hagson: hardware gpu scheduling on, the a/b's second half (2026-09-25, 23:50)

the same plan and frozen dlls as c57-hagsoff; `HwSchMode = 2` (recorded in every manifest from here on). 30 of 30 ok; `c57-hagson-native-save-r4` was contaminated (another build running on the machine) and redone on `--resume`.

| row | whiterun, hags off | whiterun, hags on | traversal, hags off | traversal, hags on | latency p50 whiterun, off → on |
|---|---|---|---|---|---|
| native dx11 | 215.4 | 220.8 (+2.5%) | 234.4 | 240.4 (+2.6%) | 12.1 → 9.6 ms |
| strict + blur | 207.5 | 214.7 (+3.5%) | 213.9 | 222.0 (+3.8%) | 5.9 → 5.3 ms |
| perf-max (no blur replacements) | 211.1 | 221.0 (+4.7%) | 217.1 | 228.0 (+5.0%) | 5.8 → 5.2 ms |

- **hags on helps everyone, and us more than native.** it stays on.
- against native with hags on: strict -2.8% in whiterun, -7.7% on the traversal; perf-max +0.1% in whiterun (a tie), -5.2% on the traversal.
- the whiterun rows' fourth and fifth repeats ran slow in every configuration (native 201.7 and 213.7, strict 208.4 and 206.3, perf-max 215.2 and 211.0) while the traversal rows didn't; another job's gpu test between runs is the likely cause, since only compilers are on the watch list. medians rest on the three settled repeats. other jobs' test apps get added to the watch list before the next campaign.

### campaign c57-hagsoff: hardware gpu scheduling off, first half of the a/b (2026-09-25, 20:30)

plan `hags-ab`, ultra 1080p, bit 8, 5 interleaved repeats; 30 of 30 ok. `HwSchMode = 1` (hags off, after a reboot). the fork dlls are frozen (noprobe build from `6c07a6f6`) so the hags-on half runs the same binaries. one run dropped: `c57-hagsoff-perfmax-save-r1` saw another job's `cl.exe` (skybench's own watch caught it; campaign.json did not), so that row is 4 runs.

| row | whiterun fps (median) | 1% low | display latency p50 | traversal fps (median) | 1% low | display latency p50 |
|---|---|---|---|---|---|---|
| native dx11 | 215.4 | 169.5 | 12.1 ms | 234.4 | 214.0 | 12.8 ms |
| strict + blur | 207.5 (-3.7%) | 185.8 | 5.9 ms | 213.9 (-8.7%) | 199.5 | 6.7 ms |
| perf-max | 211.1 (-2.0%) | 185.4 | 5.8 ms | 217.1 (-7.4%) | 195.3 | 6.2 ms |

- **against the hags-on days, hags off looks worse for us than for native**: native whiterun 215.4 vs 218.2 (c56, -1.3%), strict 207.5 vs 213.4 (-2.8%); traversal native 234.4 vs 241.4 (c55, -2.9%), perf-max 217.1 vs 229.0 (-5.2%). those are different days, so the hags-on half (`c57-hagson`, same plan, same dlls) is the real comparison.
- native's two early whiterun runs were slow (203.7, 209.1); the median sits on the three that settled.
- the latency rule from the audit still holds: native and ours are read on different presentmon clocks, so compare each column with itself across the halves, not across rows.

### campaign c56: the strict vanilla image, the blur replacements, descriptor models (2026-09-25, 20:00)

plan `strict-screen`, whiterun, ultra 1080p, bit 8, 3 repeats; 15 of 15 ok, shadows ok. **strict** = perf-max without `BLESSED_VOL_HALFRATE` (no temporal reuse; an independent audit): cb ring, threaded front end, cb vram mirror, vb rebar, vol collapse (bit-identical), precipitation gate.

| row | fps (median) | runs |
|---|---|---|
| native dx11 | 218.2 | |
| strict | 211.0 | 210.0-214.5 |
| **strict + blur replacements** (`cf1e1a21`, `dbb99d2e`; 128/128 dispatches bit-identical in game, `blurverify-*`) | **213.4** | 213.2-213.7 |
| + `dxvk.enableDescriptorHeap = False` | 203.8 | 179.4-219.9 |
| + heap and descriptor buffer off (legacy sets) | 174.0 | 160.7-177.5 |

- **with vanilla's image, we're 2.2% behind native** (213.4 vs 218.2); half-rate volumetrics had covered the rest.
- **the blur replacements are a real, exact gain** (+1.1%, tight runs): the audit's first item.
- **dxvk's default descriptor heap is the right path**; the others lose (the audit's second item, closed).

### campaign c55: the dynamic vertex/index buffer in vram (2026-09-25, 18:00)

plan `vb-rebar`, ultra 1080p, 3 repeats, bit 8; 18 of 18 ok, shadows ok. `d3d11.blessedVbRebar = True` (fork): dynamic buffers bound only as vertex/index data keep dxvk's own device-local host-visible memory instead of the skyrim profile's cached override. found through `566c03f3` (a full-screen glare composite at ~2x native) reading a 4 mb `VERTEX_BUFFER|INDEX_BUFFER` streaming buffer written ~12.6 times a frame with `NO_OVERWRITE`: bulk sequential writes, which write-combined memory handles (unlike the cb ring's ~13k scattered maps).

| scene | native | perf-max + mirror | + vb rebar |
|---|---|---|---|
| whiterun | 221.4 | 218.6 | **220.3** (+0.8%; -0.5% vs native) |
| traversal | 241.4 | 228.2 | **229.0** (+0.4%; -5.1% vs native) |

- small and clean (the rows don't overlap; cpu busy unchanged). kept for perf-max.
- native's whiterun median moves between campaigns (218.9 in c53, 221.4 here): compare within a campaign.
- the water cube's extra cost (traversal): dxvk tracks writes per image, not per array layer, so each of the six faces takes the in-order barrier path. the sun cascades are a layered image too.

### campaign c53: level with native dx11 at ultra in whiterun (2026-09-25, 16:30)

plan `mirror-confirm`: native vs perf-max + the cb vram mirror (perf-max = llvm-mingw no-probe fork, cb ring, threaded front end, vol collapse, vol half-rate, precipitation gate; the nvidia flag with bit 8), ultra 1080p, 5 interleaved repeats, three scenes; 30 of 30 ok, shadows ok. display latency = presentmon's median `DisplayLatency`.

| scene | native fps | ours fps | 1% low native / ours | gpu busy native / ours | display latency native / ours |
|---|---|---|---|---|---|
| whiterun | 218.9 | **219.0** | 171.5 / **178.2** | 4.51 / 4.43 ms | 9.6 / **5.1 ms** |
| grass (riverwood) | 194.7 | 192.1 | 155.0 / 148.3 | 5.04 / 5.37 ms | 9.3 / **6.1 ms** |
| traversal (`--motion fly`) | 241.3 | 228.5 | 218.5 / 206.0 | 4.15 / 4.39 ms | 12.4 / **5.1 ms** |

- **whiterun: level with native (+0.05%), 1% lows +3.9%, display latency 47% lower.**
- **corrections (an independent audit, 2026-09-25):** (1) the win is a median tie, not a reliable win: the mean of the five runs favours native (219.6 vs 217.9), and the paired differences run -4.9 to +0.5 fps. (2) perf-max includes `BLESSED_VOL_HALFRATE=1`, which reuses last frame's volumetrics at a still camera: not vanilla's image, strictly; the strict configuration drops it. (3) the display-latency figures are presentmon's `DisplayLatency` from each backend's frame start, not input-to-photon.
- the forest is 1.3% behind and the traversal 5.3% behind; both on the gpu side (grass +0.33 ms, traversal +0.24 ms of gpu busy). the traversal points at streaming (uploads, new pipelines): the next target.
- the cpu side is heavier on ours (traversal 4.25 vs 2.48 ms of cpu busy) but hidden behind the gpu at ultra.

### campaign c52: the constant-buffer vram mirror, 0.5% from native (2026-09-25, 15:00)

plan `cb-mirror`: ultra 1080p, whiterun, bit 8, 3 repeats. `d3d11.blessedCbMirror = True` (fork `7b1dc318`): the game keeps writing constant buffers into cached system memory (the only fast place for its cpu: the rebar ring and `cachedDynamicResources = vir` both cost ~30%), and each submission's used ring range is copied to a device-local mirror on the transfer queue; the gpu reads the mirror. shadows ok.

| row | fps | 1% low | gpu busy | cpu busy |
|---|---|---|---|---|
| native dx11 | 220.0 | 173.9 | 4.49 ms | 4.41 ms |
| perf-max | 212.8 | 187.3 | 4.46 ms | 4.68 ms |
| **perf-max + cb vram mirror** | **218.8** | **181.8** | **4.43 ms** | **4.56 ms** |

- **+2.8% over perf-max; 1.2 fps (0.5%) behind native on average, 4.5% ahead on 1% lows.**
- it engages completely: per 600 frames 7.66 m renames mirrored, 0 cpu-read, 0 lookup misses on the front end's compact path; ~5.97 mb copied a frame in ~6 transfer submissions.

### campaign c51: vrs and the half-rate far field, switched on properly (2026-09-25, 14:00)

plan `unmeasured-levers-2`: perf-max, ultra 1080p, bit 8, 3 repeats (c47 had them wrongly configured). shadows ok.

| row | fps | 1% low | gpu busy |
|---|---|---|---|
| perf-max | 213.3 | 191.0 | 4.45 ms |
| + half-rate far field (`BLESSED_HALFRATE=1`, `SKYBENCH_HALFRATE=1`; log: "on") | 212.7 | 187.6 | 4.50 ms |
| + vrs, constant 2x2 (the ceiling) | 210.5 | 185.7 | 4.51 ms |
| + vrs, adaptive | 208.5 | 187.7 | 4.60 ms |

- **correction (an independent audit, 2026-09-25): the "constant 2x2" row ran with vrs off.** the plan passed `BLESSED_VRS=const2x2`; the parser takes `2x2` (`blessed_vrs.cpp`), anything else is off, and its logs show no activation. only adaptive vrs was measured (-2.3%). the ceiling is untested.
- **the half-rate far field gives nothing** in this scene.
- both stay off; dead ends for the critical goal.

### hidden nvidia vulkan settings (2026-09-25, 04:20)

the synthetic app on skyrim's profile (bit 8 standing), 15 s per row, each value imported on top of bit 8 and set back to 0 after. 14 values of five settings from profile inspector's `Reference.xml`: "Vulkan Present and Swapchain Behavior" `0x208845AC` (`0x10000` "WDDM executes present flip in the VkQueue's HW context", `0x20` present on async compute, `0x10`, `0x4` zero-copy flip via asyncblit, `0x2`, `0x1`, `0x10020`), "Deferred Barrier Execution" `0x0049C741` (`0x8`, `0x20`, `0x40`), "vkQueueSubmit Control Flags" `0x203BB0F7` (`0x1`), "Tiled Cache Enable Vulkan" `0x20523DD1` (`0x2`, `0x100`), "Barrier Deferral with Transfer Operations" `0x20BC1A30` (`0xFFF`).

- **none changed anything**: every row frame 4.695-4.699 ms, display latency 6.14-6.19 ms, hardware independent flip, identical to the two baselines. either the layered present ignores them or the driver doesn't read them from this profile. closed.
- the scan's final cleanup split setting names on spaces and handed profile inspector a bad path; it hung elevated on a dialog from 04:21 until it was closed by hand at ~13:10, which stalled the overnight queue (c51, c52). the per-row resets had already run, so the profile stayed correct (bit 8, the touched settings at 0).

### campaign c50: the mid-frame split, on bit 8 (2026-09-25, 04:30)

plan `midframe-split`: perf-max with `BLESSED_MIDFRAME_SPLIT=N` (an extra non-blocking submit before the nth pass; fork `beef9e67`), bit 8 profile, ultra 1080p, 3 repeats; 33 of 33 ok, shadows ok.

| n | 0 (off) | 1 | 2 | 3 | 5 | 8 | 9 | 10 | 11 | 12 | 13 |
|---|---|---|---|---|---|---|---|---|---|---|---|
| fps | 212.4 | 213.0 | 213.2 | 212.8 | 212.4 | 212.5 | 212.7 | 211.7 | 212.2 | 212.5 | 211.6 |

- **no split point moves the frame on bit 8** (all within ±0.4%). in the synthetic app the split reproduced bit 8's effect without the flag; with bit 8 standing it has nothing left to take. its remaining use is a software fallback for machines without the flag (a flag-off a/b is owed before claiming that).

### campaign c49: dxvk's own knobs (2026-09-25, 03:00)

plan `dxvk-conf-sweep`: perf-max plus one dxvk.conf line per row, ultra 1080p, bit 8, 3 repeats; every run ok and passed shadowcheck.

| knob | fps | 1% low | cpu busy |
|---|---|---|---|
| base (perf-max) | 212.7 | 186.2 | 4.68 ms |
| `d3d11.relaxedBarriers = True` | 212.8 | 195.4 | 4.68 ms |
| `d3d11.relaxedGraphicsBarriers = True` | 212.6 | 195.5 | 4.68 ms |
| `dxvk.enableGraphicsPipelineLibrary = False` | 213.1 | 196.3 | 4.68 ms |
| `dxvk.zeroMappedMemory = False` | 212.9 | 194.5 | 4.67 ms |
| `dxgi.maxFrameLatency = 1` | 197.5 | 173.3 | 4.92 ms |
| `d3d11.cachedDynamicResources = vir` | 146.1 | 118.5 | 6.59 ms |
| `... = ""` | 146.0 | 119.4 | 6.60 ms |
| `... = vir`, no cb ring | 145.3 | 116.4 | 6.63 ms |

- **dxvk's defaults for skyrim are already the best of these.** the barrier and pipeline-library knobs move nothing; one frame of latency costs 7%.
- **`cachedDynamicResources` without the `c` flag costs 31%** (cpu busy +1.9 ms): the game's constant-buffer writes go to write-combined memory, the same wall the rebar ring hit. the hotspots harness gain on `556a3b73` doesn't survive the real frame. this is why dxvk ships `a` for skyrim.

### where bit 8's cost went, in game (campaign c48, 2026-09-25, 02:00)

`c48-passes-native-r1/r2` (native, timing proxy) vs `c48-passes-pm-r1/r2` (perf-max, pass timer), the standing bit 8 profile, ultra 1080p, whiterun; game frames only (windows with > 60 passes; a plain `passdiff` picked main-menu windows). all 67 passes match.

- **the whole frame is 0.15-0.18 ms over native.** 72-74% of it is one pass: the 4096² sun cascade depth map (~2,280 draws), +0.27-0.28 ms over native, against +0.06 ms before bit 8. the next biggest is a gbuffer-like pass at +0.08 ms (unchanged since before bit 8).
- the fork's gaps split: frame 4.71 ms, passes 4.61, present 0.03, idle 0.07, **frame-start idle 0.01 ms** (0.37 before bit 8).
- **so bit 8 moves the old present round trip into whichever pass is running when the hand-off lands**, and that is the longest pass. the mid-frame split sweep (c50) brackets it at N=9-13.

### campaign c46: bit 8 standing, perf-max nearly level; community shaders doesn't load (2026-09-25, 01:00)

plan `cs-compare`, ultra 1080p, 3 repeats, the first campaign on the standing bit 8 profile (`0x00080104`).

| row | whiterun fps | 1% low | gpu busy | grass fps | 1% low |
|---|---|---|---|---|---|
| native dx11 | 217.5 | 162.3 | 4.52 ms | 190.0 | 150.6 |
| **perf-max** | **211.4** | **168.6** | **4.47 ms** | **188.2** | **157.1** |
| blessed (full ray-traced stack) | 143.9 | 138.8 | 7.40 ms | 172.2 | 164.4 |

- **perf-max is 2.8% behind native in whiterun and 0.9% in the grass scene, and ahead on 1% lows (+3.9%, +4.3%).** its gpu busy is now under native's; the rest of the gap sits in presentation.
- **community shaders (the aio dev build d2025.11.25 from a wabbajack list) never reached the main menu on 1.7.104** in any of 15 runs: skse's log stops at `loading plugin "CommunityShaders"` and cs writes no log of its own (an unsupported-version stop, as predicted from its commonlib pin). a cs release that supports 1.7.104 (1.9.0+, with engine fixes 7.0.21) is needed.

### campaign c45: the cascade cache works, and loses (2026-09-24, 23:30)

plan `cache-4`, ultra 1080p, 3 repeats, after the fix that keys only the constant registers a shader reads (fork `c71a8bb0`: unread c0 held stale ring bytes, so every mesh looked moved). every run passed shadowcheck.

| row | fps | 1% low | gpu busy | cpu busy |
|---|---|---|---|---|
| native dx11 | 217.6 | 160.6 | 4.50 ms | 4.43 ms |
| perf-max | 207.9 | 163.0 | 4.91 ms | 4.68 ms |
| + cascade cache, full restore | 201.3 | 157.9 | 5.09 ms | 4.85 ms |
| + cascade cache, partial restore | 199.9 | 140.4 | 5.13 ms | 4.85 ms |

- **it caches now and stays exact**: 148 near / 1,019 far draws cached, ~134 + ~930 skipped a frame, every verify (`kind: cache` and `kind: restore`) `differ: 0`.
- **and it costs 3-4%**: the skipped draws are depth-only shadow draws, cheap on the gpu; about half the cascade draws still redraw every frame (wind-animated trees, actors); the partial restore covers 33% of the far layer (not the 1-3% estimated: movers are spread across it) and never engages on the near layer; the keys cost ~0.17 ms of cpu.
- **parked** (off, with its verify tools): on this gpu and scene the ceiling (a fraction of vanilla's ~0.78 ms cascade bill) is below its own overhead.

### nvidia's hidden present-method flag, bit by bit (2026-09-24, 20:40)

the synthetic app (`wt/present-idle/pi-test`, on skyrim's nvidia profile, 1080p uncapped, 6 heavy passes, 15 s per row), fork msvc build. `0x20324987` ("vulkan/opengl present method - flags") is `0x00080004` on this machine (bits 2 and 19, the layered-present promotion); each row sets one more bit, imported with profile inspector, the original restored after (`base-after` matches `base1`/`base2`). script and logs: the session scratchpad `bitscan/`.

| flag | fps | frame | frame-start idle | passes | present mode | display latency |
|---|---|---|---|---|---|---|
| base `0x00080004` | 206.8 | 4.77 ms | 0.365 ms | 4.36 ms | hardware: independent flip | 14.1 ms |
| **+ bit 8 (`0x00080104`)** | **209.6** | **4.70 ms** | **0.013 ms** | 4.64 ms | **hardware: independent flip** | **6.1 ms** |
| + bit 18 | 215.1 | 4.62 ms | 0.058 ms | 4.50 ms | composed: copy with gpu gdi | 29.2 ms |
| + bit 12 | 74.2 | vsync-locked | | | | |
| + bit 3 | 199.9 | 4.93 ms | 0.494 ms | | | |
| the other 25 bits, and bit 2 cleared | 204.7-206.2 | ~4.79 ms | ~0.35 ms | | unchanged | |

- **bit 8 keeps the independent flip, halves display latency (14.1 → 6.1 ms) and removes the frame-start idle**; the round trip's cost partly moves into the passes (+0.28 ms), so the frame gains only 1.4%. an in-game a/b is queued (`nvflag-*` runs).
- **in game, bit 8 holds (the `nvflag-*` a/b, perf-max, whiterun, ultra 1080p, alternating, 3 each):** 212.2-213.0 fps against 208.7-209.4 (+1.8%), display latency 5.5 ms against 11.5 ms, hardware independent flip every run, shadows ok. native dx11 in the same scene (c37): 10.0 ms. **our fork with bit 8 reaches the screen 45% sooner than native dx11.**
- bit 18 is layering off by another name (composed, 29 ms): a trap.
- **application-controlled exclusive fullscreen (`BLESSED_FSE=app`, fork `ca85effc`) doesn't give a direct flip either**: off the layered profile, with the exclusive mode acquired, nvidia's vulkan still presents "composed: copy with gpu gdi" at 29.1 ms display latency (frame-start idle 0.056 ms, frame 4.60 ms). every no-round-trip path found so far costs ~15 ms of latency; bit 8 is the only one that keeps the independent flip.

### campaigns c40 and c42: water effects at half rate on the cpu (2026-09-24, 19:45)

`SKYBENCH_WATER_HALFRATE=1` (`Main_RenderWaterEffects`, id 36560, skipped on alternate frames), perf-max, 720p low, 3 repeats.

| row | c40 (first version) | c42 (whole call skipped) |
|---|---|---|
| whiterun, off | 362.1 | 362.4 |
| whiterun, on | 270.5 (-25%) | 368.6 (+1.7%) |
| riverwood, off | 343.9 | 339.9 |
| riverwood, on | 256.1 (-26%) | 342.4 (+0.7%) |

- the first version kept the water simulation on skip frames by calling `TESWaterSystem::Update` itself, which bypassed the engine's own gates and paid a full update every skip frame (+1.1 ms of cpu).
- **the fixed version is worth at most +1.7%, inside the run spread, and runs ripples and displacement at half rate.** the profile's 9% of the main thread did not turn into frame rate. kept in the plugin, off; not a lever.

### campaign c41: the first real traversal (2026-09-24, 19:30)

plan `fly`: the `skybench-traverse` save with `--motion fly` (no-clip, an 8° climb, autorun: the player crosses the whiterun plains above the ground; verified by the end-of-capture shots, `fly-test-2`), 3 repeats, all 15 runs ok, none exited.

| row | fps | 1% low | gpu busy | cpu busy |
|---|---|---|---|---|
| native dx11, ultra 1080p | 239.9 | 199.1 | 4.18 ms | 2.64 ms |
| perf | 202.6 | 176.1 | 4.95 ms | 4.82 ms |
| perf-max | 218.1 | 178.6 | 4.57 ms | 4.46 ms |
| perf-max, 720p low, no pinning | 555.6 | 393.0 | 1.74 ms | 1.67 ms |
| perf-max, 720p low, main thread alone on a core | 552.3 | 382.4 | 1.72 ms | 1.67 ms |

- **traversing, perf-max trails native by 9.1% (1% lows 10.3%).** the gap is on the gpu side (0.39 ms of gpu busy): most of it is the ~0.36 ms layered-present round trip, which weighs more at these ~4 ms frames.
- **pinning does nothing while moving either**; with c32 that closes it.

### the main thread, timed by engine function (campaign c36, 2026-09-24, 17:00)

plan `frame-timer` with `SKYBENCH_FRAME_TIMER=1` and `SKYBENCH_CASCADE_CENSUS=1` (plugin rdtsc brackets; output per run since `fb3f3d2`), 720p low, 2 repeats, medians of the per-window p50s after load. the traverse rows are void (the `fly` motion was broken until `3bd3689`/the climb fix).

| row | frame | Main::Render | outside render | draw submission (SetupAndDrawPass) | cull on main |
|---|---|---|---|---|---|
| native, whiterun | 2.91 ms | 2.01 | 0.90 | 1.17 | 0.41 |
| perf-max, whiterun | 2.82 ms | 1.86 | 0.95 | 1.00 | 0.43 |
| native, riverwood | 3.16 ms | 2.20 | 0.96 | 1.43 | 0.34 |
| perf-max, riverwood | 3.05 ms | 2.03 | 1.01 | 1.23 | 0.36 |

- **our main thread is already 0.1 ms under native's**: draw submission is cheaper (1.00 vs 1.17 ms) because the threaded front end takes the d3d11 work off it.
- **the sun cascades (census, perf-max whiterun): the cull stage (107132) costs 0.53 ms a frame, the draw stage (107133) 0.34 ms, the gap between them 0.007 ms.** together 31% of the main thread. running the cascade cull on a worker while the main thread does other render work (a reorder of `Main::Render`'s sequence) could save up to ~0.5 ms of a cpu-bound frame.

### the main thread, profiled by engine function (2026-09-24, 13:30)

`profile-main-2`: perf (ring + threaded front end), 720p low, whiterun, 200 cdb stack samples of every thread during a 300 s capture (the run's fps is void: each sample freezes the game). skyrim frames mapped to address library ids through the exe's `.pdata` (our cpu sampler and a pe-offsets helper); names from community shaders' `FrameAnnotations.cpp`. the main thread was busy in 159 of 200 samples.

| where the main thread is | share of its busy samples |
|---|---|
| `Main::Render` / RenderPlayerView (36559), all of it | 75% |
| - `Main_RenderWorld` (107142) | 25% |
| - 107133 + 107132 (unnamed; likely the shadow map passes) | 26% |
| - `Main_RenderWaterEffects` (36560) | 9% |
| - `Main_RenderDepth`, the prepass (107139) | 8% |
| of which inside `BSBatchRenderer::SetupAndDrawPass` (107644), the draw submission | 44% |
| of which in the cull entries (106921, 76553/76554) | 10% |
| outside `Main::Render` (game update, scripts, physics dispatch) | 25% |

- **three quarters of the main thread is rendering, and 44% is draw submission** (walking the accumulated passes and issuing draws, now into our front end). that is where parallel draw recording aims; culling is only 10% on the main thread (the rest is on the job workers already).
- **water effects cost 9% of the main thread every frame**: a plugin half-rate for `Main_RenderWaterEffects` (36560) is a cheap cpu lever (the fork's half-rate only saves the gpu side).
- game update is a quarter: the full-pipelining ceiling here is about a 25% shorter main-thread frame.

### 107133 and 107132 resolved: the sun's shadow cull and draw calls (2026-09-24, recording stage 1)

capstone over the exe (`.pdata` + the address library bin) plus a static read of the `BSShadowDirectionalLight`/`BSShadowFrustumLight`/`BSShadowParabolicLight` vtables (their addresses from `Offsets_VTABLE.h`, read directly out of the exe image, no game process needed) resolves both of stage 0's hedged ids.

`Main::Render` (id 36559, rva `0x656e60`) calls both directly, once per frame, back to back, `0x146` bytes apart, with no other work of its own in between:

| offset | id | size | does |
|---|---|---|---|
| `Main::Render`+`0x1c4` | **107132** | `0x457` | resets light-local state, then dispatches the light's `Accumulate` through its vtable (vfunc 9): `BSShadowDirectionalLight::Accumulate` id **108488**, `BSShadowFrustumLight::Accumulate` id **108615** (matches `point_shadow_cull.cpp`'s own independent finding), `BSShadowParabolicLight::Accumulate` id **108626** - which reaches `BSShadowLight::AccumulateShadowmap` (id 107602, shared) and `RunCullJob` (id 106921) |
| `Main::Render`+`0x30a` | **107133** | `0x25c` | a short reset, then one indirect call at +`0x1ae` (`call qword ptr [r8 + 0x50]`, vfunc 0xA) dispatching the light's `RenderShadowmaps`: `BSShadowDirectionalLight::RenderShadowmaps` id **108489**, `BSShadowFrustumLight::RenderShadowmaps` id **108616**, `BSShadowParabolicLight::RenderShadowmaps` id **108627** - which reaches `BSBatchRenderer::SetupAndDrawPass` (107644) |

so **107132 is the cull/accumulate stage, 107133 is the draw stage**, for whichever light(s) are active - not two different light types as the stage-0 hedge read. `profile-main-2`'s own raw stacks confirm the split is real: of 19 samples inside 107132, 0 touch d3d11 or `SetupAndDrawPass`; of 22 samples inside 107133, 19 do. in the whiterun-bridge scene (no active point/spot shadow-casting lights) both calls exclusively drive the sun's cascades, which is why stage 0 saw them paired at ~26% of the main thread.

**this changes the stage-1 design, not just the names.** the original plan's shape ("decide work runs on a worker while the main thread does other work, joins before the pass's normal call site") assumes room between dispatch and use. there is almost none here: `0x146` bytes of `Main::Render`'s own code sit between the two calls, doing setup only (perf-marker begin/end calls, `id 77503/77505/77506`), not independent rendering work. dispatching 107132 to a worker at its call site and joining at 107133's call site would buy close to nothing - the main thread would block on the join almost immediately, since the sun's shadow draw is the very next thing `Main::Render` does and nothing else is queued to overlap against. the actual overlap window this pass needs is earlier: alongside havok's tick and the precipitation occlusion map, both of which the doc's own section 1 table places earlier in `Main::Render` - which means reordering `Main::Render`'s own call sequence, not just redirecting one call, a materially bigger and riskier change than a checked call-site patch. `SKYBENCH_CASCADE_CENSUS` (this commit) measures 107132 and 107133 separately, plus the gap between them, so the next step has real numbers instead of the aggregate 26% before deciding whether that reordering is worth it.

evidence: the cpu sampler's output in the capture folder of `profile-main-2` (`grep 'id_107132 '` / `grep 'id_107133 '`), the scratch disassembly tooling and full worked traces in the recording-stage1 report (not committed - see the session's `SubagentHandback`).

**`SetupAndDrawPass`'s decide-side children, named (2026-09-24):** stage 0 also asked what 107297/107298/107300/107637/70776/107762 (samples deep inside `BSBatchRenderer::SetupAndDrawPass`, id 107644) actually do. read each one's own disassembled body (not just its call chain, and correctly bounded — the naive fixed-window read runs several of these together, since msvc packs them back to back with no gap):

| id | what it does |
|---|---|
| **107297** | decodes a render-pass flag mask (bit tests against two cached global toggles), then calls a shared table lookup (id 108328) — **technique/permutation selection** |
| **107298** | a 19-way jump table on a `BSShaderProperty` type byte, each case writing floats into an object-local array — **cbuffer content assembly**, per property type |
| **107300** | reads a geometry/pass flag bit, grows two per-object scratch arrays, and (per the raw stacks) feeds directly into `D3DXVec3TransformCoord`/`D3DXVec3TransformNormal` — **world-transform math** |
| **107637** | a 13-way jump table on a per-technique byte, grows a scratch cbuffer-sized buffer, and its own call chain leads directly into the game's own d3d11 front-end thunks (`id 77263`/`77386`/`77271` → `d3d11!Ordinal21`) — **shader/cbuffer setup immediately followed by the actual `SetShader`/`SetConstantBuffers` calls**: this is the real decide/submit boundary, not pure decide |
| **70776** | iterates a small per-item active-flag array and, for each active entry, makes a virtual call (vtable +`0x128`) into a per-item setup routine — **per-light/per-technique setup dispatch** |
| **107762** | grows two more scratch buffers (the same lazy-cache pattern as 107297/107300), and (per the raw stacks) computes `D3DXMatrixTranspose` right before use — **cbuffer content assembly**, matrix packing |

three of the six (107297, 107298, 107300, 107762) are pure per-object math/memory with no d3d11 call in their own bodies — real "decide" work, in principle movable to a worker. 107637 straddles decide and submit (it's also where `RendererShadowState` gets touched, indirectly, through the front end), and 70776's virtual dispatch wasn't traced further this session. these are compiler-generated, lazily-cached functions over undocumented private struct layouts (the exact per-technique cbuffer byte packing was not fully recovered) — safely reimplementing or body-detouring them with byte-identical output is real, further RE work, not something this session's capstone pass alone can guarantee; a wrong byte in the reimplementation is exactly the "state-cache corruption" bug class the design doc warns about, and it was not run in the game.

### campaign c32: thread pinning (2026-09-24, 02:30)

plan `pinning`: perf-max, 720p low (cpu-bound), 3 repeats, the five `--pin` presets. every captured run held hardware independent flip. medians (fps):

| preset | whiterun | "traverse" (see below) |
|---|---|---|
| none | 357.7 | 561.3 |
| main-alone (main thread alone on core 0) | 350.6 | 581.5 |
| split (main, dxvk-cs, dxvk-fe each on its own core) | 343.1 | 572.7 |
| split-smt (the same on single logical cpus, job workers on the siblings) | 334.6 | 536.3 |
| ideal-only (preferred cores, no masks) | 356.9 | 568.1 |

- **pinning is not a lever in whiterun**: the scheduler alone (none) is fastest; hard masks cost 2-6%; giving smt siblings to the job workers costs most.
- the "traverse" column is the rock-face close-up (see c29's correction); main-alone's +3.6% there is inside the run spread and unproven. redo with `--motion fly`.

### campaigns c29 and c30: the new scenes, and engine fixes (2026-09-24, 01:30)

**c29** (plan `new-scenes`): ultra 1080p, 3 repeats, the two new saves. `traverse` is the whiterun plains with `--motion autorun`; the harness's screenshot is taken before the capture, so that the player really ran during it is not yet verified (an end-of-capture shot is owed). one contaminated run dropped.

| scene | native | perf | perf-max | perf-max vs native |
|---|---|---|---|---|
| grass (riverwood) | 186.6 (1% 128.1) | 172.5 (132.2) | 183.3 (137.8) | -1.8%, 1% lows +7.6% |
| traverse (plains) | 252.1 (1% 213.9) | 215.7 (182.3) | 233.0 (190.6) | -7.6%, 1% lows -10.9% |

- **correction (c32's end-of-capture shots): the traverse runs are not a traversal.** autorun carried the player a few steps into a rock face, so the row measured a near-still close-up. `--motion fly` (no-clip, then autorun) replaces it; the traverse numbers here are void until redone.
- in the forest the gap nearly closes (-1.8%) and our 1% lows win. on the open plains it's wider: native's gpu busy is 3.91 ms against our 4.36 ms, and at ~4 ms frames the fixed ~0.36 ms present round trip weighs more. the faster the frame, the more the present cost shows.

**c30** (plan `engine-fixes`): engine fixes 7.0.21 against our allocator, perf-max, 720p low (cpu-bound), whiterun, 3 repeats, staged per run by `--engine-fixes`. it loaded on 1.7.104 without trouble.

| row | fps | 1% low |
|---|---|---|
| perf-max | 352.1 | 261.4 |
| + engine fixes (its defaults, its allocator) | 343.3 | 259.7 |
| + engine fixes with its allocator off + ours | 344.0 | 249.8 |
| + our allocator | 357.0 | 263.6 |

- **engine fixes brings no speed here** (-2.5%, near the run spread); its value is its fixes. **our allocator is +1.4%**, within the spread. neither is a critical-goal lever at this scene.

### campaign c28: perf-max again, and the cascade cache's first price (2026-09-23, 23:40)

plan `critical-2`, ultra 1080p, 3 repeats, fork `30e976d5`. every run passed shadowcheck. one perf run died in warmup with `0xC0000005` (the first exit caught with a code; being traced).

| row | fps | 1% low | gpu busy |
|---|---|---|---|
| native dx11 | 216.3 | 155.9 | 4.51 ms |
| perf | 194.5 | 161.5 | 5.11 ms |
| perf-max | 207.7 | 164.7 | 4.92 ms |
| perf-max + cascade cache (0.5 texel) | 194.5 | 156.7 | 5.26 ms |
| perf-max + cascade cache (exact) | 197.2 | 158.9 | 4.98 ms |

- **perf-max repeats c24: 4.0% behind native on fps, 5.6% ahead on 1% lows.**
- **the cascade cache costs 5-6% as it stands**: it caches nothing yet (every mesh stays flagged as moving) and still pays the depth copy on reuse frames. still open.

### campaign c25: the hidden engine switches (2026-09-23, 22:00)

plan `engine-switches`, perf at 720p low and ultra 1080p, 3 repeats, the switches through the new `--ini` (display keys in both ini files). medians (fps):

| switch | 720p low | ultra 1080p |
|---|---|---|
| base | 345.4 | 193.8 |
| `bJoblistActiveWait=0` | 340.4 | 193.6 |
| `bCullingJobEnablePlaneOptimization=1` | 342.5 | 193.7 |
| `bEnableFrontToBackPrepass=1` | 337.2 | 193.9 |
| `bLodZPrepass=0` | 348.8 | 194.5 |
| all four | 337.5 | 193.9 |

- **none of them moves the frame rate.** every row is within the run spread (about ±2%). whether `bJoblistActiveWait=0` even took effect is unproven: the census run to check it exited mid-capture; it gets redone.
- 14 runs were redone: 12 lost to a presentmon etw session clash (fixed: our own session name, `7fa50e3`), 2 to the game exiting (open).

### the thread census: where skyrim's cpu goes (2026-09-23, 19:40)

`census-perf-low720-1` (perf, 309 fps) and `census-native-low720-1` (native, 318 fps): 720p low, whiterun, 30 s, per-thread cpu time from `GetThreadTimes` at capture start and end (`--census`, our thread census script). cpu ms per second of wall time; 1000 = one core.

| thread | perf | native |
|---|---|---|
| six game workers (`SkyrimSE.exe+0xce4290`, five more idle) | 2,066 | 2,091 |
| main (owns the window) | 764 | 845 |
| dxvk cs | 523 | - |
| our threaded front end | 495 | - |
| dxvk submit / descriptor / queue | 176 / 129 / 74 | - |
| nvidia d3d11 driver (43 threads) | - | 506 |
| audio | 30 | 23 |
| six workers at `SkyrimSE.exe+0xec59b0` (havok's, likely) | 0 | 1 |
| total | 4,281 | 3,479 |

- **the six game workers burn two whole cores**, each 330-350 ms/s, about 80% of it user time, in both backends. at a still camera there isn't two cores of job work in this frame: this is `bJoblistActiveWait=1`, the job threads spinning while they wait. they sit on smt siblings of the cores the main thread and dxvk's threads need. `bJoblistActiveWait=0` is the first row of the engine switches campaign.
- **the main thread is lighter on our fork** (764 vs 845 ms/s): the front end moves dxvk's work off it. dxvk's own threads add 1.4 cores against the nvidia driver's 0.5.
- **havok costs nothing here.** its likely workers are idle in whiterun at a still camera; physics needs a traversal or combat scene to show up. the `fMaxTime` fix stays a correctness fix.

### campaign c23: the perf profile vs native dx11 vs stock dxvk (2026-09-23, 18:50)

the ask: all our tuning minus shaders against vanilla dx11. whiterun save, uncapped, 30 s, 5 interleaved repeats (plan `perf-vs-native`, `c23-<row>-r1..r5`), layered present on, hardware independent flip everywhere. `perf` is the llvm-mingw no-probe fork with the constant-buffer ring and the threaded front end, the two features proven before the campaign started. none of the evening's levers are in it yet (cached cascades, vol half-rate, vol collapse, rebar ring, precipitation gate, cascade walk hook): those are c24. medians; every ultra run passed shadowcheck. four runs were voided and rerun: three with a compiler running, one during a busy-spinning `wait-bench.cmd` (native read 170 vs 196-215, fixed in `59c6b76`).

| row | ultra 1080p fps | 1% low | gpu busy | cpu busy | 720p low fps | 1% low | cpu busy |
|---|---|---|---|---|---|---|---|
| native dx11 | 205.0 | 131.3 | 4.58 ms | 4.62 ms | 320.6 | 225.9 | 2.99 ms |
| stock dxvk 3.1.1 | 150.6 | 105.8 | 5.49 ms | 6.30 ms | 243.6 | 172.1 | 3.88 ms |
| **perf** | **192.1** | **139.4** | 5.16 ms | 5.06 ms | **317.1** | **237.4** | 3.00 ms |

- **ultra 1080p: perf is 6.3% behind native on average fps and 6.2% ahead on 1% lows.** the frame is gpu-bound (perf's gpu busy 5.16 ms vs native's 4.58 ms): the 0.58 ms gap is the critical goal's target.
- **720p low: perf ties native** (-1.1% fps, inside the run spread: perf 270-338, native 305-331) **and beats it on 1% lows by 5.1%.** c21's +2.8% did not repeat as a clear win.
- **against stock dxvk, perf is +27.6% at ultra and +30.2% at 720p low**, with 20-38% better 1% lows.

### everything together, first look (2026-09-23, 18:30)

`everything-1`: rt sun shadows (cascade walk skipped), npcs in the tlas, gi v1.1 bounds, rtao, traced volumetrics (gain 24 + soft shoulder: ours 0.139 vs vanilla's 0.160 mean), the `blessed` look (bless grade, half of the custom bloom, its grain and exposure, less haze), cb ring, threaded front end; mingw fork; shadows ok; no compiler during the run.

**137.3 fps, 1% low 91.8, gpu 7.27 ms, cpu 7.15 ms** (native dx11 at the same settings: 220). image vs vanilla: `docs/media/2026-09-23-vanilla-vs-everything.png`. read: softer, more grounded, air with depth; but darker and a little muddy against vanilla, as gi occlusion, ao and the look's softer highlights stack. next: bless's shadow/interior floor for gi (0.16 under the bounce, shade keeping ~45%) before tuning the look further.

### the last gpu gap to native is at present time (2026-09-23, 18:00)

the fork's gaps measurement (`gpu-gaps`, `gpu-gaps-2`, merged `cd749061`) splits each frame's gpu timeline into skyrim's passes, dxvk's own work and idle, with a self-check against the pass timer. whiterun ultra 1080p, ring + threaded front end, game windows only (`gaps2-base-2`, 54/54 windows pass the self-check):

| part | ms per frame |
|---|---|
| frame (present to present, gpu) | 5.26 (= presentmon gpu busy 5.27) |
| skyrim's passes | 4.81 |
| dxvk's own gpu work (init, uploads, blit) | ~0.03 |
| **idle at the start of each frame** | **0.36** (all "queued": the frame's work was already submitted; the gpu hadn't started it) |

so dxvk's rendering is at native speed and dxvk adds no gpu work of its own worth measuring; the ~0.4 ms left is gpu-side work after each present, outside dxvk's command buffers (most likely the driver's layered-present copy/flip). two dxvk-side cuts (`BLESSED_FLUSH=pass`, `BLESSED_NO_TRANSFER_QUEUE=1`) changed nothing, as this predicts; both stay off. next measure: the same split with nvidia's present method native vs layered, and a fullscreen vs borderless a/b. note: a first run of this build (`gaps2-base-1`, 145.9 fps, cpu-bound) was a one-off; single runs don't get the campaign's contamination watch.

### ray-traced ao and volumetrics, first in-game runs (2026-09-23, 17:20)

full blessed stack + ring + cascade fix, whiterun, one run each, shadows checked ok in every run.

| run | fps | gpu busy | feature gpu |
|---|---|---|---|
| stack (`look2-none-1`) | 173.0 | 5.75 ms | |
| + rtao (`ao-rt-1`) | 160.4 | 6.23 ms | 0.69 ms (half-res, 2 rays), vanilla sao skipped (3 draws) |
| + traced volumetrics (`vol-rt-2`) | 129.9 | 6.76 ms | 0.70 ms trace |

- **rtao proof**: forcing our ao texture to black blacks out all world geometry, fog and sky untouched (`ao-black-1`): we own the texture skyrim's sao composite reads. the traced ao adds soft contact darkening under eaves and porches. `docs/media/2026-09-23-ao-proof-and-rtao.png`.
- **volumetrics**: the first cut (`vol-rt-1`) showed no shafts: the pass-138 skip dropped the draw our trace runs from (same bug class as the mask skip); that skip is opt-in now. `vol-rt-2` traces: strong shafts through the gate, sky blown out on the right (`gain=40` is an uncalibrated guess; calibrate to vanilla's strength next). the frame lost 43 fps for +1.0 ms of gpu busy, so something also stalls the frame: to find. `docs/media/2026-09-23-volumetrics-first.png`.

### campaign c22: our allocator (main heap), cpu-bound (2026-09-23, 16:50)

720p low, ring + threaded front end, 3 repeats (plan `alloc-ab`). allocator v2.1 (plugin `c79507c`), main heap only (the scrap heap half drops the sun shadows and stays off).

| row | median fps | median 1% low | cpu busy |
|---|---|---|---|
| ring + threaded | 348.9 | 258.4 | 2.73 ms |
| + our allocator (main heap) | 349.6 | 271.7 | 2.72 ms |

neutral in steady state (the 1% low difference is inside the noise: one baseline run dipped to 241). same image. the 238 mb requests now go to the old allocator (`reason: huge`, served, non-null). where an allocator should pay is loading and cell transitions, which this standing benchmark doesn't exercise: a traversal benchmark is the next measure for it.

### gpu parity, pass by pass, and the blessed look (2026-09-23, 16:30)

**the gpu pass diff** (a per-pass diff tool, run on the capture folders of `passes-native-2` and `passes-fork-1` with `--fold-xfer`): the native timing proxy (now a full context wrapper, `tools/d3d11-timing`) and the fork's pass timer (`BLESSED_GPU_PASSES`), same whiterun frame, ultra 1080p. all 67 passes align. **the sum of skyrim's passes: native 4.72 ms, dxvk 4.77 ms (+0.05 ms).** the largest per-pass deltas are small (a gbuffer-ish pass +0.08 ms, the cascades +0.04, the sun mask +0.02). yet presentmon's gpu busy shows ~0.5-0.7 ms more for dxvk (c18: 5.4 vs 4.5 ms). **so dxvk renders skyrim's passes at native gpu speed; the gap is between the passes**: dxvk's own uploads/staging copies, barriers and layout transitions, and submission bubbles. that is the gpu track's real target now.

**the blessed look** (`BLESSED_LOOK=bless`, fork, on the full blessed stack + ring + cascade fix; the final pass on this frame is ps `98e1bd7b`, not the dump's `831de5eb`: set `BLESSED_LOOK_PS_FINAL=98e1bd7b`), one run each, shadows checked with our shadow check script:

| run | fps | gpu busy | shadows |
|---|---|---|---|
| no look (`look2-none-1`) | 173.0 | 5.75 ms | ok |
| bless profile (`look2-bless-1`) | 163.3 | 6.12 ms | ok |
| custom profile, all r_edit values (`look2-custom-1`) | 162.2 | 6.17 ms | ok |

the look pass itself: 0.33 ms gpu (`look.jsonl`). the skip-replaced "skip the sun mask draw" dropped our traced shadows in game (twice, even with the post-draw hooks run); it is opt-in now (`BLESSED_SHADOW_SKIP_MASK_DRAW=1`).

### a trap in our own screenshots: "view b" is missing shadows (2026-09-23, 15:40)

some runs' screenshots showed the left wall sunlit instead of shaded, and read like a different camera angle. they are not: the fire baskets and npcs sit on the same pixels. the sun shadows are missing. every run with that frame had broken shadows: `shdist-1` (`fShadowDistance=1`), `cascade-hook-on-1/-2` (the first cascade hook), and `alloc-v2-1` (allocator v2 refused a 238 mb allocation and returned null; its +18% was a false win from less gpu work). checked by an edge-feature classifier over every recent run: every campaign row (c15-c21), `cascade-fix-1`, `stacked-1` and c20's blessed rows have their shadows; those numbers stand. from now on every run's screenshot gets classified before its number is used.

### campaign c21: faster than native dx11 on the cpu (2026-09-23, 15:20)

the ultra 1080p scene is gpu-bound now (the ring alone takes the fork's cpu below its gpu), so the cpu work only shows where the gpu isn't the wall: **720p, low preset**, uncapped, whiterun, 30 s, 3 interleaved repeats (plan `cpu-bound`, `c21-<row>-r1..r3`), llvm-mingw no-probe fork, layered present on. every run: hardware independent flip.

| row | fps | 1% low | frame p99 | cpu busy | gpu busy |
|---|---|---|---|---|---|
| native dx11 | 345.0 | 266.5 | 3.77 ms | 2.79 ms | 1.58 ms |
| stock dxvk 3.1.1 | 269.5 | 208.3 | 4.83 ms | 3.53 ms | 2.18 ms |
| our fork + constant-buffer ring | 315.8 | 228.3 | 4.41 ms | 2.97 ms | 2.11 ms |
| **our fork + ring + threaded d3d11 front end** | **354.7** | **277.3** | **3.61 ms** | **2.69 ms** | 2.06 ms |

- **our fork beats native dx11 in skyrim's cpu-bound case: +2.8% fps, +4.1% 1% lows, 0.1 ms less cpu per frame.** stock dxvk is 22% behind native here.
- the threaded front end (fork `67df0716`, `d3d11.blessedThreadedFrontEnd`) is worth +12% over the ring alone here; at ultra 1080p (gpu-bound) it shows as +16% 1% lows (`tfe-thr-1` vs `tfe-base-1`, one run each: 145.1 vs 124.7).
- **correction:** a single-run pass just before (`cpu-native-1` etc.) read native 227.8 and the fork 344.2 ("+51%"). that native run sat at 1,703 mhz / 83 w (the campaign's: 1,890 mhz / 132 w): the gpu hadn't ramped after idle, and at this light load the frame follows the clocks. single runs after idle are void; interleaved campaigns only.

### the render thread, round two: the ring, the cascade walk, the allocator (2026-09-23, 14:00)

all on the llvm-mingw fork, layered present on, uncapped whiterun.

**campaign c19** (plan `perf-parts`, probe 1 frame in 8 in every row, 3 repeats; flagged runs dropped):

| row | fps | 1% low | cpu busy | gpu busy | game thread in dxvk | cs busy |
|---|---|---|---|---|---|---|
| fork, features off | 141.6 | 97.0 | 6.68 ms | 5.57 ms | 2.40 ms | 3.14 ms |
| **+ constant-buffer ring** (`d3d11.blessedCbRing=True`) | **188.8** | 125.7 | **4.98 ms** | 5.19 ms | 2.08 ms | 2.80 ms |

- **the ring: +33%, 1.7 ms of cpu per frame**, far above its 0.5-0.8 ms estimate: the saving is mostly outside dxvk's call time (2.40 → 2.08 ms), i.e. in the engine's own thread around the maps (allocation-cache and cs traffic pressure gone). same image (mean difference 0.3/255). fork `821f069a`.
- **our allocator crashes the game right after load**; bisected: the main heap alone runs (`bisect-alloc-mm-1`, 145.7 fps, one run), the scrap-heap half crashes. still open.

**the cascade walk, fixed** (plugin `f59449e`, one run each, full blessed stack):

| run | fps | cpu busy | gpu busy | draws per frame | sun shadows |
|---|---|---|---|---|---|
| no hook (`cascade-hook-off-1`) | 138.4 | 6.97 ms | 5.98 ms | 5,283 | yes |
| cull every caster (`cascade-hook-on-1`) | 173.8 | 5.60 ms | 5.74 ms | 2,576 | **lost** |
| **skip only the sun's caster queueing** (`cascade-fix-1`) | **167.0** | 5.72 ms | 5.84 ms | 2,577 | **yes** |

the engine marks shadow receivers while it registers cascade casters (`BSShaderAccumulator` register, id 106567, ors the sun's bit into each geometry's light mask), so culling every caster also dropped every receiver to unshadowed permutations. the fix keeps the walk and skips only queueing the sun's casters. **+20.7% with shadows.**

**stacked (`stacked-1`, one run): the full blessed stack + ring + cascade fix, no-probe mingw build: 173.1 fps, 1% low 134.2, cpu 5.63 ms, gpu 5.75 ms.** the blessed stack is now gpu-bound; the gpu track decides the rest of the gap to native (220).

### campaigns c17 and c18: nvidia's hidden dxvk flag, and the mingw build (2026-09-23, 12:30)

nvidia's driver detects dxvk and refuses to present it through a dxgi swapchain, whatever "prefer layered on dxgi swapchain" says, unless the hidden profile setting "Vulkan/OpenGL Present Method - Flags" (`0x20324987`) allows it: `0x00080004`, "allow promoting dxvk to dxgi/directflip" (found through nvidia profile inspector's setting list; set on skyrim's profile, see `PROJECT.md`). c17 ran with the flag off, c18 with it on, same session, uncapped, whiterun, 30 s, 3 interleaved repeats each (plan `layered-ab`). the harness now waits for a quiet machine and flags any run a compiler touched: `c17-native-r1` was flagged and dropped (an earlier attempt, c15/c16, ran while nine compile jobs ran and is void: native swung 212 → 100 fps).

| row | flag | fps | 1% low | frame p99 | cpu busy | display latency p50 | present path |
|---|---|---|---|---|---|---|---|
| native dx11 | off | 219.6 | 176.6 | 5.66 ms | 4.44 ms | 9.64 ms | hardware: independent flip |
| native dx11 | on | 220.5 | 175.0 | 5.71 ms | 4.41 ms | 9.88 ms | hardware: independent flip |
| dxvk 3.1.1 | off | 163.2 | 119.6 | 8.36 ms | 5.96 ms | **24.26 ms** | composed: copy with gpu gdi |
| dxvk 3.1.1 | on | 162.0 | 127.2 | 7.86 ms | 5.92 ms | **7.91 ms** | hardware: independent flip |
| our fork (msvc) | off | 148.9 | 115.9 | 8.63 ms | 6.59 ms | 25.41 ms | composed: copy with gpu gdi |
| our fork (msvc) | on | 150.0 | 113.4 | 8.81 ms | 6.40 ms | 8.36 ms | hardware: independent flip |
| our fork (llvm-mingw) | off | 154.0 | 119.8 | 8.35 ms | 6.38 ms | 24.99 ms | composed: copy with gpu gdi |
| our fork (llvm-mingw) | on | 154.0 | 120.4 | 8.30 ms | 6.24 ms | 8.21 ms | hardware: independent flip |
| blessed (rt stack) | off | 140.9 | 110.6 | 9.04 ms | 7.02 ms | 25.78 ms | composed: copy with gpu gdi |
| blessed (rt stack) | on | 140.3 | 109.6 | 9.12 ms | 6.91 ms | 9.39 ms | hardware: independent flip |

what it says:

- **the flag cuts dxvk's display latency by two thirds: ~25 ms → ~8 ms**, level with native dx11 (9.6-9.9 ms). frame rate doesn't move (every row is cpu-bound), cpu busy drops 0.1-0.2 ms (no composition copy).
- **the llvm-mingw (clang 23) build is +3.4% over msvc** (154.0 vs 148.9-150.0 fps, cpu busy 6.24-6.38 vs 6.40-6.59 ms). it becomes the default build.
- stock dxvk (gcc) still leads our mingw fork by ~0.3 ms of cpu: our fork's code with features off, the probe included (the `fork-mingw-noprobe` variant is the next row to measure).

### campaigns c11 and c12: where the fork's 0.6 ms behind stock goes (2026-09-23, 07:00)

uncapped, 30 s, 5 repeats each (plan `fork-gap`, plan `compiler`), no probe, every blessed feature off.

| row | median fps | cpu busy |
|---|---|---|
| dxvk 3.1.1 as released (gcc 16.2) | 165.2 | 5.96 ms |
| dxvk 3.1.1 unmodified, built with our msvc 19.44 (`--dxvk-build stock-msvc`) | 158.3 | 6.23 ms |
| our fork, features off | 149.1-150.9 | 6.54-6.62 ms |
| our fork, ray tracing extensions not even enabled (`BLESSED_RT=0`) | 150.5 | 6.54 ms |

- **the ray tracing extensions cost nothing** (150.5 vs 150.9).
- **the compiler costs 0.27 ms** per frame (gcc vs msvc on identical source).
- **our disabled features cost 0.39 ms** (~6.5 ns on each of ~60,000 calls): out-of-line gate checks and cold globals. it is being worked on, plus a build option that compiles the probe out.
- **c13 (fork `fca554ad`, gates made inline): no change.** stock-msvc 159.4, fork 150.2, blessed 139.4 (medians, 5 repeats). the four out-of-line gates were real but were not the cost; the 0.36 ms is elsewhere. (the first cut of that fix crashed at load with features on: config globals logged during dll static init, before dxvk's logger existed; caught in `zc-blessed-1`, fixed before merge.) c14 tests the probe compiled out.
- **c14: the probe compiled out (`-Dblessed_probe=false`, `--dxvk-build fork-noprobe`) is worth 0.09 ms**: stock-msvc 157.8, fork 151.1, fork-noprobe 153.4 fps (medians, 5 repeats). the fork's remaining cost with everything off is ~0.18 ms (~3%), near this box's run-to-run noise; not worth bisecting eighty commits for now. headline builds can use the no-probe variant.
- gi v1 costs what v0 did (0.155 ms trace, 164 albedo textures, 763 meshes coloured, 19% of probes rejected as inside geometry).

### first live runs (2026-09-22 night): main menu only, not for posting

an anniversary edition "Thanks for buying... DOWNLOAD" modal blocks the main menu, so the scene's `coc` never ran. these rows prove the pipeline, not performance.

| run id | backend | avg fps | p99 frame ms | cpu busy ms | gpu busy ms | present runtime | present mode | vram mb |
|---|---|---|---|---|---|---|---|---|
| smoke-native-2 | native dx11 | 60.9 | 16.48 | 16.41 | 0.91 | DXGI | Hardware: Independent Flip | 1026 |
| smoke-native-3 | native dx11, window unfocused | 25.9 | 324.94 | 39.56 | 0.88 | DXGI | Hardware: Independent Flip | 896 |
| fork-boot-1 | our dxvk fork (msvc) | 59.3 | 23.31 | 16.91 | 0.97 | Other (vulkan) | Composed: Copy with GPU GDI | 1503 |

what they taught us:

- the main menu caps near 60 fps on both backends. gpu busy under 1 ms.
- an unfocused window drops to 26 fps with 325 ms spikes. the harness now sets `bAlwaysActive=1` and brings the window to the front before capture.
- **present path is not equal yet.** native gets hardware independent flip; dxvk (nvidia vulkan in a borderless window) gets "composed: copy with gpu gdi", a slower, higher-latency path. a fair dxvk-vs-native comparison needs nvidia's "vulkan/opengl present method" set to "prefer layered on dxgi swapchain" (nvidia control panel, per program), and every result row records its present mode.
- dxvk uses about 500 mb more vram at the menu (1503 vs 1026 mb).

## dxvk vs native dx11

what we expect before measuring (research, 2026-09-22):

- dxvk issue [#5885](https://github.com/doitsujin/dxvk/issues/5885), opened by doodlum (the community shaders author, who is moving to dxvk "to support more advanced rendering systems"): about 30% slower than native dx11 on windows with nvidia. doitsujin's read: dxvk is slower than nvidia's own dx11 driver when cpu-bound. skyrim does about 20k constant-buffer maps per frame.
- doodlum's own numbers on an rtx 4080: dxvk default 147.4 fps, `d3d11.cachedDynamicBuffers=c` 160.4, `=a` 180.3 (+22%).
- dxvk 3.1.1 (2026-09-15) ships the workaround as a built-in app profile: `d3d11.cachedDynamicResources = a` for `SkyrimSE.exe` (`src/util/config/config.cpp:507`, verified). note the real key is `cachedDynamicResources`; the issue comment's `cachedDynamicBuffers` is not it. so 3.1.1 defaults already include the +22% trick. what we don't know: does it beat native on a ryzen 5 5600?
- dxvk and reshade-as-`dxgi.dll` fight over the same file. reshade has to go through its vulkan layer instead.

no runs yet. first plan: plan `dxvk-vs-native` (native, dxvk 3.1.1 defaults, dxvk with the cache turned off to price the workaround; whiterun, ultra, 1080p, uncapped, 3 repeats interleaved).

## ray tracing

**decision 2026-09-22: remix dropped. we build our own hybrid tracer in a dxvk fork; plan and unfair advantage in `PROJECT.md`.** the upstream remix branch below is kept as reference numbers only.

found 2026-09-22: community shaders already has an unmerged, public (gpl) remix path tracer for skyrim se on vulkan. it's branch `codex/remix-vulkan` by doodlum, last commit 2026-09-21 ("feat(remix): native scene host and test harness"). it has a scene graph bridge, materials, foliage, skin, trees, landscape, water and sky, plus a d3d11 remix build of dxvk. the d3d11 remix runtime is `Frissj/Titanfall-2-Remix` patched by `tools/remix/PrepareRuntime.ps1`. blocker: the branch pins CommonLibSSE-NG v6.7.1 (latest = 1.7.99), and our exe is 1.7.104, so the pin needs a bump to a commit with 1.7.104 (upstream `0fee48ad` or later).

their numbers (handover doc, 2026-09-21; every figure is from one rtx 4080, no vram figures, nothing on 8 gb cards. we are the first 3060 ti data point):

| config | rendered fps | presented fps (dlss fg) |
|---|---|---|
| default | 26.6 | 53.2 |
| out-of-band reflex markers off | 53.5 | 107.0 |
| + dlss ultra performance, tlas refit | 74.7 | 149.4 |

open upstream: frame pacing (37% of frame pairs bunched with markers off), reflex, 60 fps at 1080p without frame gen. earlier, full-scene runs were 15-16 fps at 1680x1050 (2026-09-11). `-TlasRefit` saved 2.2 ms gpu (scenePrep 6.2 to 4.0 ms).

no runs of ours yet.

## shader stacks

no runs yet.
