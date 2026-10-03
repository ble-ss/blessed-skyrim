// occlusion_probe - stage 0 of occlusion culling: force-culls a stable hashed fraction of main-view statics (SKYBENCH_OCCLUSION_PROBE).
#pragma once

struct SKSEInterface;

// reads SKYBENCH_OCCLUSION_PROBE=<fraction> (0 to 1). unset: patches
// nothing. set: detours BSCullingProcess::Process1 and, for the main
// camera's culls only, culls that fraction of eligible static references
// (0 = count only). counters go to the events jsonl every 5 s as
// "occlusion_probe_tick". call from SKSEPlugin_Load, on 1.7.104 only.
void InstallOcclusionProbe(const SKSEInterface * skse, unsigned long pluginHandle);
