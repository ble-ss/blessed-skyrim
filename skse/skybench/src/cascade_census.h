// cascade_census - times the sun's shadow-map cull and draw calls separately (SKYBENCH_CASCADE_CENSUS).
#pragma once

struct SKSEInterface;

// reads SKYBENCH_CASCADE_CENSUS. unset: patches nothing.
//
// stage 0 (docs/tuning-report.md, "the main thread, profiled by engine
// function") found id 107133 + id 107132 together at ~26% of the main
// thread's busy samples, hedged as "unnamed; likely the shadow map
// passes." this hook resolves and splits them: both are called exactly
// once per frame, directly from Main::Render (id 36559), back to back -
// id 107132 first (the light's Accumulate/cull dispatch, ending in
// RunCullJob, id 106921 - 0 of 19 stage-0 samples touch d3d11 or
// SetupAndDrawPass), then id 107133 (the light's RenderShadowmaps/draw
// dispatch, which does reach BSBatchRenderer::SetupAndDrawPass - 19 of 22
// stage-0 samples do). in the whiterun-bridge scene (no active point or
// spot shadow-casting lights) both exclusively drive the sun's cascades.
//
// this measures each call's own wall time (rdtsc, calibrated the same way
// the d3d11 probe is) so a future prepare-ahead change has the missing
// decide-vs-submit split for this pass, and confirms whether the two
// calls are genuinely back to back (no independent main-thread work
// between them to overlap a worker against) - see the recording-stage1
// report for why that finding changes the stage-1 design for this pass.
//
// call from SKSEPlugin_Load, on 1.7.104 only. cost when unset: zero,
// nothing is patched.
void InstallCascadeCensus(const SKSEInterface * skse, unsigned long pluginHandle);
