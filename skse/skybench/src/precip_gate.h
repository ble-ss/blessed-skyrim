// precip_gate - the precipitation occlusion map only while rain or snow can fall (SKYBENCH_PRECIP_OCCLUSION).
#pragma once

struct SKSEInterface;

// reads SKYBENCH_PRECIP_OCCLUSION. unset: patches nothing.
// "weather" (or "1"): Precipitation::RenderOcclusion runs only while the
// current weather has rain or snow, or the transition away from a wet one
// is still running (plus 120 frames). "off": it never runs (the proof run:
// the two 512x512 d16 passes, ps 489864ca, must vanish from a dump).
// any other value: hooks the call and passes through.
// call from SKSEPlugin_Load, on 1.7.104 only.
void InstallPrecipGate(const SKSEInterface * skse, unsigned long pluginHandle);
