// allocator - skyrim's MemoryManager and ScrapHeap on mimalloc, owned by skybench (SKYBENCH_ALLOCATOR).
#pragma once

struct SKSEInterface;

// reads SKYBENCH_ALLOCATOR. unset or not "1": patches nothing, reserves
// nothing, starts no thread. "1": reserves our arena and takes over the
// allocator entries (see allocator.cpp for the knobs). call from
// SKSEPlugin_Load on 1.7.104: that runs after every SKSEPlugin_Preload,
// engine fixes' allocator patches included, and before WinMain starts the
// game's threads.
void InstallAllocator(const SKSEInterface * skse, unsigned long pluginHandle);
