// allocator_test - offline checks for the allocator's entry patching and its exclusive mimalloc arena (no game needed).
//
// build: cmake -DSKYBENCH_BUILD_TESTS=ON, then run skybench_alloc_test.exe.
// part 1 patches synthetic functions shaped like the 1.7.104 entries (a
// trampoline prologue, a null-check prologue entered at +9, an engine fixes
// style ff 25 jump, an e9 jump) and checks that the hook runs and the old
// code is still callable. part 2 hammers two heaps in one exclusive arena
// from several threads and checks every block lands inside the arena.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include "../src/allocator_patch.h"

#include <mimalloc.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <thread>
#include <vector>

namespace {

int g_failures = 0;

void Check(bool ok, const char * what)
{
	std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
	if (!ok)
		g_failures++;
}

// ---- part 1: entry patching ---------------------------------------------

using SizeFn = std::size_t (*)(void *, std::size_t);
using FreeFn = void (*)(std::size_t *, std::size_t);

void * g_prev[4] = {};

std::size_t HookSize(void * self, std::size_t v)
{
	return reinterpret_cast<SizeFn>(g_prev[0])(self, v) * 10;
}

void HookFree(std::size_t * out, std::size_t v)
{
	if (v == 0)
		return;
	reinterpret_cast<FreeFn>(g_prev[1])(out, v + 100);
}

std::size_t EfImpl(void *, std::size_t v)
{
	return v + 0xef00;
}

std::size_t HookEf(void * self, std::size_t v)
{
	return reinterpret_cast<SizeFn>(g_prev[2])(self, v) + 1;
}

std::size_t HookRel(void * self, std::size_t v)
{
	return reinterpret_cast<SizeFn>(g_prev[3])(self, v) + 2;
}

void Put(std::uint8_t * at, std::initializer_list<std::uint8_t> bytes)
{
	for (std::uint8_t b : bytes)
		*at++ = b;
}

void PartPatch()
{
	std::printf("-- entry patching\n");
	auto * page = static_cast<std::uint8_t *>(VirtualAlloc(nullptr, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
	std::memset(page, 0xcc, 0x1000);

	// 0x100: MemoryManager::Size shape. push rbx; push rbp; push rdi;
	// sub rsp, 20h | lea rax, [rdx+1]; add rsp, 20h; pop rdi; pop rbp; pop rbx; ret
	Put(page + 0x100, { 0x40, 0x53, 0x55, 0x57, 0x48, 0x83, 0xec, 0x20,
		0x48, 0x8d, 0x42, 0x01, 0x48, 0x83, 0xc4, 0x20, 0x5f, 0x5d, 0x5b, 0xc3 });

	// 0x200: Deallocate shape. test rdx, rdx; je +3 | mov [rcx], rdx; ret | ret
	Put(page + 0x200, { 0x48, 0x85, 0xd2, 0x0f, 0x84, 0x03, 0x00, 0x00, 0x00,
		0x48, 0x89, 0x11, 0xc3, 0xc3 });

	// 0x300: engine fixes shape. ff 25 00 00 00 00 <EfImpl>, int3 fill.
	Put(page + 0x300, { 0xff, 0x25, 0x00, 0x00, 0x00, 0x00 });
	std::uintptr_t ef = reinterpret_cast<std::uintptr_t>(&EfImpl);
	std::memcpy(page + 0x306, &ef, 8);

	// 0x400: e9 jump to 0x500: lea rax, [rdx+7]; ret
	Put(page + 0x400, { 0xe9, 0xfb, 0x00, 0x00, 0x00 });
	Put(page + 0x500, { 0x48, 0x8d, 0x42, 0x07, 0xc3 });

	auto plain = [&](std::uint32_t off) { return reinterpret_cast<SizeFn>(page + off); };
	Check(plain(0x100)(nullptr, 5) == 6, "synthetic size runs before the patch");
	Check(plain(0x300)(nullptr, 5) == 0xef05, "synthetic ef jump runs before the patch");
	Check(plain(0x400)(nullptr, 5) == 12, "synthetic e9 jump runs before the patch");

	alloc_patch::Site sites[4];
	sites[0].name = "size";
	sites[0].id = 1;
	std::memcpy(sites[0].vanilla, page + 0x100, 8);
	sites[0].vanillaLen = 8;
	sites[0].stolen = 8;
	sites[0].hook = reinterpret_cast<void *>(&HookSize);

	sites[1].name = "free";
	sites[1].id = 2;
	std::memcpy(sites[1].vanilla, page + 0x200, 9);
	sites[1].vanillaLen = 9;
	sites[1].skip = 9;
	sites[1].hook = reinterpret_cast<void *>(&HookFree);

	sites[2].name = "ef";
	sites[2].id = 3;
	std::memcpy(sites[2].vanilla, page + 0x100, 8);	// deliberately not matching
	sites[2].vanillaLen = 8;
	sites[2].stolen = 8;
	sites[2].hook = reinterpret_cast<void *>(&HookEf);

	sites[3].name = "rel";
	sites[3].id = 4;
	sites[3].hook = reinterpret_cast<void *>(&HookRel);

	const std::uintptr_t base = reinterpret_cast<std::uintptr_t>(page);
	const std::uint32_t rvas[4] = { 0x100, 0x200, 0x300, 0x400 };
	std::string why;
	for (int i = 0; i < 4; i++)
	{
		bool ok = alloc_patch::Prepare(sites[i], base, rvas[i], why);
		Check(ok, (std::string("prepare ") + sites[i].name + (ok ? "" : ": " + why)).c_str());
	}
	Check(sites[0].kind == alloc_patch::EntryKind::Vanilla, "size classified vanilla");
	Check(sites[1].kind == alloc_patch::EntryKind::Vanilla, "free classified vanilla");
	Check(sites[2].kind == alloc_patch::EntryKind::AbsJump && sites[2].jumpTarget == ef, "ef classified abs jump to EfImpl");
	Check(sites[3].kind == alloc_patch::EntryKind::RelJump && sites[3].jumpTarget == base + 0x500, "rel classified e9 jump");
	Check(sites[2].owner.find(".exe") != std::string::npos, (std::string("ef owner is this exe: ") + sites[2].owner).c_str());

	bool committed = alloc_patch::Commit(sites, 4, g_prev, why);
	Check(committed, (std::string("commit") + (committed ? "" : ": " + why)).c_str());
	if (!committed)
		return;

	for (auto & s : sites)
		Check(alloc_patch::StillOurs(s), (std::string("still ours: ") + s.name).c_str());

	Check(plain(0x100)(nullptr, 5) == 60, "size: hook ran, trampoline returned the old result");

	std::size_t out = 0;
	reinterpret_cast<FreeFn>(page + 0x200)(&out, 5);
	Check(out == 105, "free: hook ran, old code entered at +9");
	out = 7;
	reinterpret_cast<FreeFn>(page + 0x200)(&out, 0);
	Check(out == 7, "free: null handled by the hook");

	Check(plain(0x300)(nullptr, 5) == 0xef06, "ef: hook ran, previous replacement still called");
	Check(plain(0x400)(nullptr, 5) == 14, "rel: hook ran, previous target still called");

	// a second prepare now sees our own jump and would chain over it.
	alloc_patch::Site again = sites[0];
	Check(alloc_patch::Prepare(again, base, 0x100, why) && again.kind == alloc_patch::EntryKind::RelJump,
		"re-prepare sees our e9 as a jump, never as vanilla");

	// a site with unknown bytes is refused.
	Put(page + 0x600, { 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0xc3 });
	alloc_patch::Site unknown;
	unknown.name = "unknown";
	std::memcpy(unknown.vanilla, page + 0x100, 8);
	unknown.vanillaLen = 8;
	unknown.stolen = 8;
	unknown.hook = reinterpret_cast<void *>(&HookSize);
	Check(!alloc_patch::Prepare(unknown, base, 0x600, why), "unknown bytes refused");
}

// ---- part 2: exclusive arena -------------------------------------------

void PartArena()
{
	std::printf("-- exclusive arena\n");
	mi_option_set(mi_option_purge_delay, 1000);

	mi_arena_id_t arena = {};
	int err = mi_reserve_os_memory_ex(std::size_t(16) << 30, false, false, true, &arena);
	Check(err == 0, "reserve 16 GiB exclusive arena (the default)");
	if (err)
		return;

	std::size_t span = 0;
	auto base = reinterpret_cast<std::uintptr_t>(mi_arena_area(arena, &span));
	std::printf("     arena 0x%llx + %llu MiB\n", static_cast<unsigned long long>(base), static_cast<unsigned long long>(span >> 20));
	auto owns = [&](const void * p) { return reinterpret_cast<std::uintptr_t>(p) - base < span; };

	mi_heap_t * heap = mi_heap_new_in_arena(arena);
	mi_heap_t * scrap = mi_heap_new_in_arena(arena);
	Check(heap && scrap, "two heaps in the arena");

	// the default heap must not land in our arena.
	void * outside = mi_malloc(64);
	Check(!owns(outside), "default heap stays outside the exclusive arena");
	mi_free(outside);

	std::atomic<std::uint64_t> outsideCount{ 0 }, misaligned{ 0 }, nulls{ 0 }, total{ 0 };
	std::vector<void *> handoff[8];

	auto worker = [&](int t) {
		std::mt19937 rng(1234 + t);
		std::vector<void *> live;
		for (int i = 0; i < 200000; i++)
		{
			std::size_t size;
			unsigned r = rng() % 1000;
			if (r < 900) size = rng() % 256;
			else if (r < 990) size = 256 + rng() % 65536;
			else size = 65536 + rng() % (4u << 20);
			std::size_t align = (rng() % 4 == 0) ? (std::size_t(16) << (rng() % 9)) : 0;
			mi_heap_t * h = (rng() & 1) ? heap : scrap;
			void * p = align ? mi_heap_malloc_aligned(h, size, align) : mi_heap_malloc(h, size);
			total++;
			if (!p) { nulls++; continue; }
			if (!owns(p)) outsideCount++;
			if (align && (reinterpret_cast<std::uintptr_t>(p) & (align - 1))) misaligned++;
			if (size) std::memset(p, 0x5a, size < 64 ? size : 64);
			live.push_back(p);
			if (live.size() > 2000 || (rng() % 3 == 0 && !live.empty()))
			{
				std::size_t k = rng() % live.size();
				void * q = live[k];
				live[k] = live.back();
				live.pop_back();
				if (rng() % 2)
				{
					void * g = mi_heap_realloc(heap, q, rng() % 4096 + 1);
					if (g && !owns(g)) outsideCount++;
					if (g) mi_free(g);
				}
				else
				{
					mi_free(q);
				}
			}
		}
		handoff[t] = std::move(live);	// freed by another thread below
	};

	std::vector<std::thread> threads;
	for (int t = 0; t < 8; t++)
		threads.emplace_back(worker, t);
	for (auto & th : threads)
		th.join();

	std::thread freer([&]() {
		for (auto & v : handoff)
			for (void * p : v)
				mi_free(p);
	});
	freer.join();

	std::printf("     %llu allocations, %llu null, %llu outside, %llu misaligned\n",
		static_cast<unsigned long long>(total.load()), static_cast<unsigned long long>(nulls.load()),
		static_cast<unsigned long long>(outsideCount.load()), static_cast<unsigned long long>(misaligned.load()));
	Check(outsideCount == 0, "every block from the arena heaps is inside the arena");
	Check(misaligned == 0, "every aligned block is aligned");
	Check(nulls == 0, "no allocation failed");

	// a block bigger than one arena chunk: inside the arena, or null (the
	// hooks then overflow to the old allocator). never outside.
	void * big = mi_heap_malloc(heap, std::size_t(96) << 20);
	Check(big == nullptr || owns(big), big ? "96 MiB block inside the arena" : "96 MiB block refused (null), not taken from the os");
	if (big) mi_free(big);

	// scaleform (through engine fixes' allocator) asks the MemoryManager
	// for big alignments: inside the arena, or null. never outside.
	for (std::size_t align : { std::size_t(64) << 10, std::size_t(1) << 20, std::size_t(4) << 20 })
	{
		void * a = mi_heap_malloc_aligned(heap, std::size_t(256) << 10, align);
		char what[96];
		sprintf_s(what, "%llu KiB alignment: %s", static_cast<unsigned long long>(align >> 10),
			a ? (owns(a) ? "inside the arena" : "OUTSIDE the arena") : "null (overflows to the old allocator)");
		Check(a == nullptr || (owns(a) && (reinterpret_cast<std::uintptr_t>(a) & (align - 1)) == 0), what);
		if (a) mi_free(a);
	}

	void * huge = mi_heap_malloc(heap, std::size_t(20) << 30);
	Check(huge == nullptr, "20 GiB block (more than the arena) is null, not taken from the os");
	if (huge) mi_free(huge);
}

} // namespace

int main()
{
	PartPatch();
	PartArena();
	std::printf("%s (%d failures)\n", g_failures ? "FAILED" : "all passed", g_failures);
	return g_failures ? 1 : 0;
}
