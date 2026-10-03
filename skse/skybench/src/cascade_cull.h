// cascade_cull - the sun shadow cascades queue no casters but keep marking receivers (SKYBENCH_CULL_CASCADES).
#pragma once

struct SKSEInterface;

// reads SKYBENCH_CULL_CASCADES. unset: patches nothing. "1": the sun's
// cascades queue no casters but still mark the sun's shadow receivers.
// "planes": the cascades cull everything (receivers lost, shadows vanish).
// any other value: hooks the call and passes through.
// call from SKSEPlugin_Load, on 1.7.104 only, before community shaders'
// post-post-load fixes, which then chain over this hook.
void InstallCascadeCull(const SKSEInterface * skse, unsigned long pluginHandle);
