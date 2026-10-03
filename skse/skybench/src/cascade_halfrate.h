// cascade_halfrate - renders the sun's far shadow cascade every other frame and reuses its depth in between (SKYBENCH_CASCADE_HALFRATE).
#pragma once

struct SKSEInterface;

// reads SKYBENCH_CASCADE_HALFRATE. anything but "1": patches nothing, costs
// nothing. perf-max only; strict never sets it.
//
// on a reused frame cascade 1 queues no casters (the mode 14 register
// handler returns early for its accumulator, receivers are still marked),
// its layer is not cleared, and its lightTransform is put back to the one
// the kept depth was rendered with, so the shadow mask and the volumetric
// generate read the old depth with its own matrix. see the .cpp header for
// the engine facts, the gates and the log line.
//
// call from SKSEPlugin_Load, on 1.7.104 only.
void InstallCascadeHalfrate(const SKSEInterface * skse, unsigned long pluginHandle);
