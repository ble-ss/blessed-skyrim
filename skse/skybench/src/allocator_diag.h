// allocator_diag - a first-chance crash reporter for the allocator (allocator_crash events), so a silent game death leaves a trace.
#pragma once

#include <cstdint>

// adds a vectored exception handler. it reports access violations, illegal
// and privileged instructions and stack overflows whose rip is in the exe or
// in skybench.dll, and heap corruption from anywhere: one allocator_crash
// event per distinct rip (at most 32 per process), flushed at once. it never handles the
// exception; the game's own handling carries on. the arena range lets it
// say whether the faulting data address is one of our blocks.
void InstallAllocatorCrashHandler(std::uintptr_t arenaBase, std::uintptr_t arenaSpan,
	std::uintptr_t scrapBase, std::uintptr_t scrapSpan);
