// point_shadow_cull - makes spot and omni shadow maps cull every caster (SKYBENCH_CULL_POINT_SHADOWS).
#pragma once

struct SKSEInterface;

// reads SKYBENCH_CULL_POINT_SHADOWS. unset: patches nothing. "1": culls
// every caster, but only while BLESSED_POINT_SHADOWS=1 (the fork traces the
// point shadows); without it the hook passes through. "force": culls even
// without the fork (point shadows turn fully lit; for measuring the walk).
// any other value: hooks and passes through. call from SKSEPlugin_Load, on
// 1.7.104 only.
void InstallPointShadowCull(const SKSEInterface * skse, unsigned long pluginHandle);
