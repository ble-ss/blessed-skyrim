// water_halfrate - Main_RenderWaterEffects at half rate on the cpu (SKYBENCH_WATER_HALFRATE).
#pragma once

struct SKSEInterface;

// reads SKYBENCH_WATER_HALFRATE. unset (or anything but "1"/"on"): patches
// nothing. "1" (or "on"): the call to Main_RenderWaterEffects (36560) from
// Main::Render (36559) is skipped on alternate frames. 36560 drains a small
// job queue (the reflection cube's 2 faces/frame) before its own internal
// counters would refill it, so skipping the call every other frame only
// halves the refresh rate - no face gets stuck. it also calls
// TESWaterSystem::Update first, so this halves the water
// simulation/ripples too, on purpose: an earlier build tried to keep
// simulation at full rate by calling Update directly on the skipped
// frames, and that was 25% slower in-game (see water_halfrate.cpp for the
// root cause) because deciding whether Update should even run means
// running most of 36560's own non-trivial gate logic anyway. the simple
// full skip is the safe, verified-cheap option.
// logs water_halfrate (patched/skipped, the site) and water_halfrate_tick
// every 600 calls (rendered, skipped). call from SKSEPlugin_Load, on
// 1.7.104 only.
void InstallWaterHalfrate(const SKSEInterface * skse, unsigned long pluginHandle);
