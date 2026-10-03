# settings

the exact settings behind the two profiles, **strict** and **perf-max**. the numbers in the readme come from perf-max (campaigns c76 and c80). strict is the profile that keeps vanilla's exact image.

everything here comes from the campaign plans and the tuning report. anything we could not confirm from those files is marked **unconfirmed**.

## game and machine

- Skyrim Special Edition 1.7.104 (`SkyrimSE.exe` 1.7.104.0)
- SKSE 2.3.1
- ultra preset, 1920x1080, frame rate uncapped for the benchmarks (`bLockFramerate=0`, `iFPSClamp=0`)
- hardware-accelerated GPU scheduling (HAGS) on
- measured on an RTX 3060 Ti with driver 596.36 under Windows 11. we did not test other hardware.

## nvidia profile

in NVIDIA Profile Inspector, on the Skyrim profile, set the hidden setting "Vulkan/OpenGL Present Method - Flags" (`0x20324987`) to `0x00080104`.

- `0x00080004` is the value that lets the driver present DXVK through its own DXGI swapchain (independent flip).
- `0x00080104` adds bit 8 on top of that. it is the value used in every c76 and c80 row.
- do not set bit 18: it forces the slow composed present path.

## dxvk.conf

the fork is https://github.com/ble-ss/blessed-dxvk, branch `blessed`, built with llvm-mingw. the benchmark rows ran this `dxvk.conf` for both profiles:

```
d3d11.blessedCbRing = True
d3d11.blessedThreadedFrontEnd = True
d3d11.blessedCbMirror = True
d3d11.blessedVbRebar = True
dxvk.enableDescriptorHeap = False
dxvk.enableUnifiedImageLayouts = False
```

- `dxvk.enableDescriptorHeap = False` makes DXVK use descriptor buffers. the campaign c61 and later rows all ran with it.
- `dxvk.enableUnifiedImageLayouts = False` measured as a null in c72 (+0.10% perf-max, -0.05% strict, both across zero). it is part of the conf the c76 rows ran, so it is listed here.
- the early strict-screen rows (campaign c56) ran only the first four keys.

## environment variables

set these in the process environment of `SkyrimSE.exe`, for example in the launcher or a wrapper script.

### shared by both profiles

```
BLESSED_VOL_COLLAPSE=1
SKYBENCH_PRECIP_OCCLUSION=weather
BLESSED_SHADER_REPLACE=<folder of replacement shaders>
BLESSED_ZERO_COPY=1
BLESSED_FLUSH_AT_PASS_END=1
```

- `BLESSED_SHADER_REPLACE` points at a folder of compiled replacement shaders: the two exact blur replacements and the 8x8 volumetric generate. the sources live in blessed-dxvk, in `blessed-shaders/src/`. `blessed-shaders/build.sh` compiles them into `blessed-shaders/out/rewrite/` with fxc; it needs the game's own shaders first, extracted with `fxp_extract.py`. point the variable at that folder. **not yet tested from a clean checkout.** leave the variable unset to run without them, and expect a lower result: the blur replacements were worth about +1.1% and the 8x8 generate about +0.34% in whiterun.

### perf-max adds (temporal reuse)

```
BLESSED_VOL_HALFRATE=1
BLESSED_VOL_PERIOD=3
SKYBENCH_CASCADE_HALFRATE=1
SKYBENCH_CASCADE_HALFRATE_SUNDEG=0.05
BLESSED_REFLECT_HALFRATE=1
BLESSED_REFLECT_HALFRATE_SWITCH=0
```

- this is the c76 "full + p3" row (Whiterun +10.97%, traversal +4.37%) and the c80 perf-max row.
- c76 also measured a row with `BLESSED_VOL_PERIOD` left out ("full") and a row with `BLESSED_REFLECT_HALFRATE_MAXDEG=180` added instead of the period ("turn"). both are listed in the tuning report.
- `SKYBENCH_CASCADE_HALFRATE_*` variables are read by the SKSE plugin in this repository. `BLESSED_*` variables are read by the fork.
- `BLESSED_REFLECT_HALFRATE_SWITCH=0` means the water reflection reuse does not need the plugin's F8 switch.

### strict

strict is perf-max without any temporal reuse: leave out `BLESSED_VOL_HALFRATE`, `BLESSED_VOL_PERIOD`, `SKYBENCH_CASCADE_HALFRATE*` and `BLESSED_REFLECT_HALFRATE*`. the shared block above is the rest.

**unconfirmed:** the strict rows in the plans were last run in c72, before the cascade and reflection levers existed. so no plan file holds a strict row with the c76 plugin. strict is defined in the tuning report as perf-max without the temporal reuse, and the list above follows that definition.

## skse plugin

copy the built `skybench` plugin into `Data/SKSE/Plugins/`, with `skybench.ini` next to the dll. the shipped `skse/skybench/skybench.ini` is:

```
[HalfRate]
bEnabled=1
sToggleKey=F8
```

- `bEnabled=1` turns the half-rate far field on at game start; `0` turns it off.
- `sToggleKey` is the in-game toggle key: F1 to F24, A to Z, 0 to 9, or a virtual-key code such as `0x77`.
- the environment variable `SKYBENCH_HALFRATE=0` or `1` overrides `bEnabled`.
- **unconfirmed:** the benchmark plans do not state a `bEnabled` value for the strict rows. the plugin default is on, so for strict we would set `bEnabled=0` as well as leaving out the variables above. we did not run that exact combination.

## versions

- dxvk base: 3.1.1. the c76 build was the fork at `11cffcc1`, built with llvm-mingw ("noprobe" build). the llvm-mingw build measured 3.4% faster than the same source built with MSVC.
- **unconfirmed:** the branch `blessed` may have moved since `11cffcc1`. the numbers in the readme belong to that build.
