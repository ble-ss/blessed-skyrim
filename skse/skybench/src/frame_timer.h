// frame_timer - rdtsc-brackets Main::Render and its main-thread engine calls (SKYBENCH_FRAME_TIMER).
#pragma once

struct SKSEInterface;

// stage 0 of the multi-core work. always
// compiled into skybench.dll; reads SKYBENCH_FRAME_TIMER. unset (or
// anything but "1"): patches nothing, same pattern as SKYBENCH_HAVOK /
// SKYBENCH_PRECIP_OCCLUSION - one deployed dll, the harness picks the
// probe per run with env vars alone. "1": installs. call from
// SKSEPlugin_Load, on 1.7.104 only.
//
// writes one json line every 120 frames to
// <SKYBENCH_PROBE_DIR env, or the plugin log dir>\frame_timer.jsonl: for
// each measured bucket, "<name>_mean"/"<name>_p50"/"<name>_p95" over that
// window, plus the timer's own measured overhead. see frame_timer.cpp's
// header comment for exactly what each bucket is and how it was found.
void InstallFrameTimer(const SKSEInterface * skse, unsigned long pluginHandle);
