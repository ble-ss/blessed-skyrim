// havok_step - sets fMaxTime:HAVOK from the measured frame time each frame, clamped to a min/max fps (SKYBENCH_HAVOK).
#pragma once

struct SKSEInterface;

// reads SKYBENCH_HAVOK. unset (or anything but "perframe"/"1"): patches
// nothing. "perframe" (or "1"): each call to Main::Render, fMaxTime:HAVOK is
// set to clamp(time since the previous call, 1/SKYBENCH_HAVOK_MAXFPS,
// 1/SKYBENCH_HAVOK_MINFPS) - the SkyrimDisplayTweaks havok fix, ported to
// our own hook. SKYBENCH_HAVOK_MINFPS defaults to 60, SKYBENCH_HAVOK_MAXFPS
// to 300. logs havok_fix (installed/skipped) and havok_fix_tick every 600
// frames. call from SKSEPlugin_Load, on 1.7.104 only.
void InstallHavokStep(const SKSEInterface * skse, unsigned long pluginHandle);
