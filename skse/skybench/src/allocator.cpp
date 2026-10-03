// allocator - skyrim's MemoryManager and ScrapHeap on mimalloc, owned by skybench (SKYBENCH_ALLOCATOR).
//
// what it takes over on 1.7.104 (ids from the address library, the same
// ones engine fixes replaces):
//   MemoryManager::Size        68100  rva 0xcddb50
//   MemoryManager::Deallocate  68117  rva 0xcde6a0
//   MemoryManager::Reallocate  68116  rva 0xcde4a0
//   MemoryManager::Allocate    68115  rva 0xcde250
//   ScrapHeap::Deallocate      68146  rva 0xcdfc60
//   ScrapHeap::DeallocateAlignImpl (vtable slot 5)  68140  rva 0xcdf360
//   ScrapHeap::Allocate        68144  rva 0xcdf660
// AutoScrapBuffer's ctor and dtor call the ScrapHeap entries, and vtable
// slot 4 (AllocateAlignImpl, 68139) jumps to Allocate, so they follow
// without a patch of their own.
//
// ownership is by address: every block we hand out lives in one of two
// exclusive mimalloc arenas we reserve up front (MemoryManager, scrap), so
// "is this ours, and whose" is one subtract and one compare per arena.
// anything else (blocks made before we patched, engine fixes' blocks, the
// rare overflow) goes back to whatever the entry ran before. scrap blocks
// carry a 16-byte header (owner heap, live mark) so the scrap hooks keep
// vanilla's rules exactly: see "scrap block header" below.
// entries are patched frees first, allocations last, so at every moment
// each pointer's free goes to the allocator that made it.
//
// knobs (all read once, at load):
//   SKYBENCH_ALLOCATOR=1              on. anything else: off, nothing touched.
//   SKYBENCH_ALLOC_PARTS=mm           which groups to take (default mm only; mm,scrap
//                                     adds the scrap heap, which loses the sun
//                                     shadows today). scrap needs mm: vanilla frees
//                                     scrap blocks through MemoryManager::Deallocate.
//   SKYBENCH_ALLOC_ARENA_GB=16        MemoryManager arena, 1-16 GiB (reserved, committed on use).
//   SKYBENCH_ALLOC_SCRAP_ARENA_GB=4   scrap arena, 1-16 GiB.
//   SKYBENCH_ALLOC_LARGE_PAGES=0      1: large os pages (needs SeLockMemoryPrivilege;
//                                     commits the arenas up front, so size them).
//   SKYBENCH_ALLOC_PURGE_MS=1000      delay before freed pages are purged; -1 never, 0 at once.
//   SKYBENCH_ALLOC_PURGE_DECOMMIT=1   0: purge resets pages instead of decommitting them
//                                     (a stale read then sees old or zero bytes, not a fault).
//   SKYBENCH_ALLOC_CRASHLOG=1         0: no vectored crash reporter (allocator_diag.cpp).
//   SKYBENCH_ALLOC_STATS_S=10         seconds between allocator_stats events; 0 off.
//   scrap bisect (only with scrap on):
//   SKYBENCH_ALLOC_SCRAP_MIN / _MAX   bytes: requests outside [min, max] stay on vanilla.
//   SKYBENCH_ALLOC_SCRAP_SKIP=rva,..  caller return addresses (hex, "exe+" allowed)
//                                     whose requests stay on vanilla.
//   SKYBENCH_ALLOC_SCRAP_ZERO=1       zero every scrap block.
//   SKYBENCH_ALLOC_SCRAP_CENSUS=1     sample callers; allocator_stats gets scrap_callers.

#include "allocator.h"

#include "address_library.h"
#include "allocator_diag.h"
#include "allocator_patch.h"
#include "skybench_log.h"

#include <mimalloc.h>
#include <mimalloc-stats.h>

#include <intrin.h>

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <string>

namespace {

// ---- config ------------------------------------------------------------

std::string Env(const char * name)
{
	char * v = nullptr;
	size_t n = 0;
	std::string out;
	if (_dupenv_s(&v, &n, name) == 0 && v)
	{
		out = v;
		free(v);
	}
	return out;
}

long EnvLong(const char * name, long fallback)
{
	std::string v = Env(name);
	if (v.empty())
		return fallback;
	char * end = nullptr;
	long r = strtol(v.c_str(), &end, 10);
	return (end && *end == '\0') ? r : fallback;
}

struct Config
{
	bool mm = true;
	bool scrap = false;		// opt-in: the scrap group loses the sun shadows (alloc-v2-1)
	long arenaGb = 16;
	long scrapArenaGb = 4;
	bool largePages = false;
	long purgeMs = 1000;
	bool purgeDecommits = true;
	bool crashLog = true;
	unsigned long long scrapMin = 0;			// scrap requests outside [min, max] go to vanilla
	unsigned long long scrapMax = ~0ull;
	bool scrapZero = false;
	bool scrapCensus = false;
	std::string scrapSkip;					// caller rvas (hex, comma separated) that stay on vanilla
	long statsSeconds = 10;
};

Config ReadConfig()
{
	Config c;
	std::string parts = Env("SKYBENCH_ALLOC_PARTS");
	if (!parts.empty())
	{
		c.mm = parts.find("mm") != std::string::npos;
		c.scrap = parts.find("scrap") != std::string::npos;
	}
	c.arenaGb = EnvLong("SKYBENCH_ALLOC_ARENA_GB", 16);
	if (c.arenaGb < 1) c.arenaGb = 1;
	if (c.arenaGb > 16) c.arenaGb = 16;	// one mimalloc arena tops out at 16 GiB
	c.scrapArenaGb = EnvLong("SKYBENCH_ALLOC_SCRAP_ARENA_GB", 4);
	if (c.scrapArenaGb < 1) c.scrapArenaGb = 1;
	if (c.scrapArenaGb > 16) c.scrapArenaGb = 16;
	c.largePages = EnvLong("SKYBENCH_ALLOC_LARGE_PAGES", 0) == 1;
	c.purgeMs = EnvLong("SKYBENCH_ALLOC_PURGE_MS", 1000);
	c.purgeDecommits = EnvLong("SKYBENCH_ALLOC_PURGE_DECOMMIT", 1) != 0;
	c.crashLog = EnvLong("SKYBENCH_ALLOC_CRASHLOG", 1) != 0;
	{
		std::string v = Env("SKYBENCH_ALLOC_SCRAP_MIN");
		if (!v.empty()) c.scrapMin = strtoull(v.c_str(), nullptr, 0);
		v = Env("SKYBENCH_ALLOC_SCRAP_MAX");
		if (!v.empty()) c.scrapMax = strtoull(v.c_str(), nullptr, 0);
	}
	c.scrapZero = EnvLong("SKYBENCH_ALLOC_SCRAP_ZERO", 0) == 1;
	c.scrapCensus = EnvLong("SKYBENCH_ALLOC_SCRAP_CENSUS", 0) == 1;
	c.scrapSkip = Env("SKYBENCH_ALLOC_SCRAP_SKIP");
	c.statsSeconds = EnvLong("SKYBENCH_ALLOC_STATS_S", 10);
	return c;
}

// ---- state -------------------------------------------------------------

// two exclusive arenas: MemoryManager blocks in one, scrap blocks in the
// other, so every hook can tell the two apart by address alone.
std::uintptr_t g_base = 0;			// MemoryManager arena: [g_base, g_base + g_span)
std::uintptr_t g_span = 0;
std::uintptr_t g_scrapBase = 0;		// scrap arena
std::uintptr_t g_scrapSpan = 0;
mi_heap_t * g_heap = nullptr;
mi_heap_t * g_scrapHeap = nullptr;

inline bool Owns(const void * p)
{
	return reinterpret_cast<std::uintptr_t>(p) - g_base < g_span;
}

inline bool OwnsScrap(const void * p)
{
	return reinterpret_cast<std::uintptr_t>(p) - g_scrapBase < g_scrapSpan;
}

// what the entries ran before (vanilla trampolines, or engine fixes).
using MMAllocFn = void * (*)(void * self, std::size_t size, std::uint32_t alignment, bool aligned);
using MMReallocFn = void * (*)(void * self, void * old, std::size_t size, std::uint32_t alignment, bool aligned);
using MMFreeFn = void (*)(void * self, void * p, bool aligned);
using MMSizeFn = std::size_t (*)(void * self, void * p, bool flag);
using SHAllocFn = void * (*)(void * self, std::size_t size, std::size_t alignment);
using SHFreeFn = void (*)(void * self, void * p);
using SHFreeRefFn = void (*)(void * self, void ** pp);

// filled by alloc_patch::Commit before it writes any entry, in site order
// (see BuildSites): mm = size, free, realloc, alloc; scrap = free,
// virtual free, alloc.
void * g_mmPrev[4] = {};
void * g_scrapPrev[3] = {};

inline MMSizeFn PrevSize() { return reinterpret_cast<MMSizeFn>(g_mmPrev[0]); }
inline MMFreeFn PrevFree() { return reinterpret_cast<MMFreeFn>(g_mmPrev[1]); }
inline MMReallocFn PrevRealloc() { return reinterpret_cast<MMReallocFn>(g_mmPrev[2]); }
inline MMAllocFn PrevAlloc() { return reinterpret_cast<MMAllocFn>(g_mmPrev[3]); }
inline SHFreeFn PrevScrapFree() { return reinterpret_cast<SHFreeFn>(g_scrapPrev[0]); }
inline SHFreeRefFn PrevScrapFreeRef() { return reinterpret_cast<SHFreeRefFn>(g_scrapPrev[1]); }
inline SHAllocFn PrevScrapAlloc() { return reinterpret_cast<SHAllocFn>(g_scrapPrev[2]); }

// counters for the rare paths only, sharded so threads don't share a line.
enum Counter
{
	kForeignFree, kForeignRealloc, kForeignSize, kForeignScrapFree, kOverflow,
	kScrapWrongHeap,		// ScrapHeap::Deallocate on another heap's block: vanilla ignores it, so do we
	kScrapDoubleFree,		// a scrap block freed twice (or a garbage pointer into the scrap arena)
	kScrapFreeOfMM,			// ScrapHeap::Deallocate on a MemoryManager block: vanilla ignores it
	kScrapViaMM,			// a scrap block freed or resized through the MemoryManager (vanilla allows it)
	kScrapTooBig,			// >= 64 MiB scrap request: vanilla returns null
	kScrapRoutedVanilla,	// sent to vanilla by SKYBENCH_ALLOC_SCRAP_MIN / _MAX / _SKIP
	kCounterCount
};

struct alignas(64) Shard
{
	std::atomic<std::uint64_t> v[kCounterCount];
};
Shard g_shards[16];

inline void Count(Counter c)
{
	g_shards[(GetCurrentThreadId() >> 2) & 15].v[c].fetch_add(1, std::memory_order_relaxed);
}

std::uint64_t Total(Counter c)
{
	std::uint64_t sum = 0;
	for (auto & s : g_shards)
		sum += s.v[c].load(std::memory_order_relaxed);
	return sum;
}

std::atomic<int> g_overflowsLogged{ 0 };
std::uintptr_t g_exeBase = 0;

// blocks this big never try mimalloc: one v3 arena object tops out at
// 224 MiB (7 bitmap chunks of 32 MiB; measured, 234,880,012 bytes fit and
// 234,882,456 don't), and the game asks for 238 MiB twice at boot. the old
// allocator serves them; they route back to it by address.
const std::size_t kHugeBlock = std::size_t(192) << 20;

// every overflow is counted; the first 16 are logged in full: what was
// asked, why mimalloc said no, who asked (the caller's return address),
// and what the old allocator gave back instead (never null in vanilla:
// its Allocate retries through its out-of-memory handler and the crt).
void NoteOverflow(const char * where, std::size_t size, std::size_t alignment, bool aligned,
	const char * reason, int err, void * caller, const void * result)
{
	Count(kOverflow);
	if (g_overflowsLogged.fetch_add(1, std::memory_order_relaxed) < 16)
	{
		const std::uintptr_t c = reinterpret_cast<std::uintptr_t>(caller);
		char fields[384];
		sprintf_s(fields, ", \"where\": \"%s\", \"size\": %llu, \"alignment\": %llu, \"aligned\": %s, \"reason\": \"%s\", \"errno\": %d"
			", \"caller\": \"exe+0x%llx\", \"served\": \"%s\", \"result\": \"0x%llx\"",
			where, static_cast<unsigned long long>(size), static_cast<unsigned long long>(alignment),
			aligned ? "true" : "false", reason, err,
			static_cast<unsigned long long>(c >= g_exeBase ? c - g_exeBase : 0),
			result ? "old allocator" : "NULL", static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(result)));
		skybench_LogEvent("allocator_overflow", fields);
	}
}

// ---- scrap bisect knobs and caller census --------------------------------

// with the scrap group on, the sun shadows vanish (alloc-v2-1) while the
// mm group alone keeps them (bisect-alloc-mm-1). these knobs route part of
// the scrap traffic back to vanilla to find which requests matter. a
// request routed back gets a vanilla block, which our frees send back to
// vanilla by address, so any mix is safe.
std::size_t g_scrapMin = 0;
std::size_t g_scrapMax = ~std::size_t(0);
bool g_scrapZero = false;
bool g_scrapFilter = false;		// any of min/max/skip set: one predictable branch when not
std::uintptr_t g_scrapSkip[16] = {};
int g_scrapSkipCount = 0;

bool g_census = false;
struct CensusSlot
{
	std::atomic<std::uintptr_t> rva{ 0 };
	std::atomic<std::uint64_t> count{ 0 };
	std::atomic<std::uint64_t> bytes{ 0 };
};
const int kCensusSlots = 256;
CensusSlot g_censusSlots[kCensusSlots];
thread_local std::uint32_t t_censusTick = 0;

// every 64th scrap request per thread: count its caller (by return address).
void CensusNote(std::uintptr_t rva, std::size_t size)
{
	if ((++t_censusTick & 63) != 0)
		return;
	std::size_t h = (rva * 0x9E3779B97F4A7C15ull) >> 56;
	for (int i = 0; i < kCensusSlots; i++)
	{
		CensusSlot & slot = g_censusSlots[(h + i) & (kCensusSlots - 1)];
		std::uintptr_t cur = slot.rva.load(std::memory_order_relaxed);
		if (cur == 0)
		{
			std::uintptr_t expected = 0;
			if (slot.rva.compare_exchange_strong(expected, rva) || expected == rva)
			{
				slot.count.fetch_add(1, std::memory_order_relaxed);
				slot.bytes.fetch_add(size, std::memory_order_relaxed);
				return;
			}
			continue;
		}
		if (cur == rva)
		{
			slot.count.fetch_add(1, std::memory_order_relaxed);
			slot.bytes.fetch_add(size, std::memory_order_relaxed);
			return;
		}
	}
}

// the top callers, for allocator_stats.
std::string CensusTop(int n)
{
	struct Row { std::uintptr_t rva; std::uint64_t count; std::uint64_t bytes; };
	Row rows[kCensusSlots];
	int m = 0;
	for (auto & slot : g_censusSlots)
	{
		std::uintptr_t r = slot.rva.load(std::memory_order_relaxed);
		if (r)
			rows[m++] = { r, slot.count.load(std::memory_order_relaxed), slot.bytes.load(std::memory_order_relaxed) };
	}
	std::sort(rows, rows + m, [](const Row & a, const Row & b) { return a.count > b.count; });
	std::string out = "[";
	for (int i = 0; i < m && i < n; i++)
	{
		char buf[96];
		sprintf_s(buf, "%s\"exe+0x%llx x%llu avg %llu\"", i ? ", " : "", static_cast<unsigned long long>(rows[i].rva),
			static_cast<unsigned long long>(rows[i].count * 64),
			static_cast<unsigned long long>(rows[i].count ? rows[i].bytes / rows[i].count : 0));
		out += buf;
	}
	return out + "]";
}

// ---- scrap block header ------------------------------------------------

// every scrap block carries 16 bytes in front of it: the ScrapHeap object
// that made it, a live/dead mark, and the distance back to the start of
// mimalloc's block. vanilla frees a scrap block only through the heap that
// made it (ScrapHeap::Deallocate on any other heap's block is a silent
// no-op, a range check), while MemoryManager::Deallocate finds the owning
// heap and frees it from any thread. the header lets us keep both rules.
const std::uint32_t kScrapLive = 0x5c4a7b1e;
const std::uint32_t kScrapDead = 0xdeadb10c;

struct ScrapHeader
{
	void * owner;
	std::uint32_t magic;
	std::uint32_t offset;
};
static_assert(sizeof(ScrapHeader) == 16, "the header fills the 16 bytes in front of the block");

inline ScrapHeader * HeaderOf(void * p)
{
	return reinterpret_cast<ScrapHeader *>(static_cast<char *>(p) - sizeof(ScrapHeader));
}

// frees a live scrap block. a second free (or a stray pointer into the
// scrap arena) finds no live mark, is counted and ignored; the swap is
// atomic, so two racing frees can't both reach mi_free.
void ScrapRelease(void * p)
{
	ScrapHeader * h = HeaderOf(p);
	if (InterlockedCompareExchange(reinterpret_cast<volatile LONG *>(&h->magic),
			static_cast<LONG>(kScrapDead), static_cast<LONG>(kScrapLive)) != static_cast<LONG>(kScrapLive))
	{
		Count(kScrapDoubleFree);
		return;
	}
	mi_free(static_cast<char *>(p) - h->offset);
}

std::size_t ScrapUsable(void * p)
{
	ScrapHeader * h = HeaderOf(p);
	return mi_usable_size(static_cast<char *>(p) - h->offset) - h->offset;
}

// ---- MemoryManager -----------------------------------------------------

inline void * MiAlloc(mi_heap_t * heap, std::size_t size, std::uint32_t alignment, bool aligned)
{
	return (aligned && alignment > 1)
		? mi_heap_malloc_aligned(heap, size, alignment)
		: mi_heap_malloc(heap, size);
}

// the old allocator serves it: huge, arena full, or an alignment mimalloc
// refuses. its free comes back to it because it isn't in our range.
void * MMOverflow(void * self, std::size_t size, std::uint32_t alignment, bool aligned,
	const char * where, const char * reason, int err, void * caller)
{
	void * q = PrevAlloc()(self, size, alignment, aligned);
	NoteOverflow(where, size, alignment, aligned, reason, err, caller, q);
	return q;
}

void * MMAllocate(void * self, std::size_t size, std::uint32_t alignment, bool aligned)
{
	if (size >= kHugeBlock)
		return MMOverflow(self, size, alignment, aligned, "mm_alloc", "huge", 0, _ReturnAddress());

	void * p = MiAlloc(g_heap, size, alignment, aligned);
	if (Owns(p))
		return p;

	const int err = errno;
	if (p)
		mi_free(p);
	return MMOverflow(self, size, alignment, aligned, "mm_alloc", p ? "outside the arena" : "mimalloc null", err, _ReturnAddress());
}

void * MMReallocate(void * self, void * old, std::size_t size, std::uint32_t alignment, bool aligned)
{
	if (OwnsScrap(old))
	{
		// vanilla resizes a scrap block through the MemoryManager by moving
		// it into a MemoryManager block: size it, allocate, copy, free.
		Count(kScrapViaMM);
		if (size == 0)
			return nullptr;
		void * q = size >= kHugeBlock
			? MMOverflow(self, size, alignment, aligned, "mm_realloc", "huge", 0, _ReturnAddress())
			: MMAllocate(self, size, alignment, aligned);
		if (!q)
			return nullptr;
		const std::size_t oldSize = ScrapUsable(old);
		std::memcpy(q, old, oldSize < size ? oldSize : size);
		ScrapRelease(old);
		return q;
	}

	if (old && !Owns(old))
	{
		// made before we patched (or by the overflow path): its own
		// allocator resizes it. vanilla's reallocate gets the new block
		// from the Allocate entry, which is ours, so it migrates over.
		Count(kForeignRealloc);
		return PrevRealloc()(self, old, size, alignment, aligned);
	}

	// vanilla: a zero-size reallocate returns null and leaves the block alone.
	if (size == 0)
		return nullptr;

	if (!old)
		return MMAllocate(self, size, alignment, aligned);

	void * p = nullptr;
	int err = 0;
	const char * reason = "huge";
	if (size < kHugeBlock)
	{
		p = (aligned && alignment > 1)
			? mi_heap_realloc_aligned(g_heap, old, size, alignment)
			: mi_heap_realloc(g_heap, old, size);
		if (Owns(p))
			return p;
		err = errno;
		reason = p ? "outside the arena" : "mimalloc null";
	}

	// could not grow inside the arena: old is untouched when p is null.
	if (p)
		mi_free(p);
	void * q = MMOverflow(self, size, alignment, aligned, "mm_realloc", reason, err, _ReturnAddress());
	if (!q)
		return nullptr;
	std::size_t oldSize = mi_usable_size(old);
	std::memcpy(q, old, oldSize < size ? oldSize : size);
	mi_free(old);
	return q;
}

void MMDeallocate(void * self, void * p, bool aligned)
{
	if (Owns(p))
	{
		mi_free(p);
		return;
	}
	if (OwnsScrap(p))
	{
		// vanilla finds the owning scrap heap and frees it there, from any thread.
		Count(kScrapViaMM);
		ScrapRelease(p);
		return;
	}
	if (!p)
		return;
	Count(kForeignFree);
	PrevFree()(self, p, aligned);
}

std::size_t MMSize(void * self, void * p, bool flag)
{
	if (Owns(p))
		return mi_usable_size(p);
	if (OwnsScrap(p))
		return ScrapUsable(p);
	Count(kForeignSize);
	return PrevSize()(self, p, flag);
}

// ---- ScrapHeap ---------------------------------------------------------

void * ScrapAllocate(void * self, std::size_t size, std::size_t alignment)
{
	// vanilla refuses 64 MiB and up (the limit at rva 0x63bce0) and the
	// caller falls back to the MemoryManager.
	if (size >= 0x4000000)
	{
		Count(kScrapTooBig);
		return nullptr;
	}

	// the vanilla scrap heap never hands out less than 0x10 bytes or less
	// than 8-byte alignment (engine fixes keeps the same floor).
	if (size < 0x10)
		size = 0x10;
	if (alignment < 8)
		alignment = 8;

	if (g_census || g_scrapFilter)
	{
		const std::uintptr_t rva = reinterpret_cast<std::uintptr_t>(_ReturnAddress()) - g_exeBase;
		if (g_census)
			CensusNote(rva, size);
		if (g_scrapFilter)
		{
			bool vanilla = size < g_scrapMin || size > g_scrapMax;
			for (int i = 0; i < g_scrapSkipCount && !vanilla; i++)
				vanilla = g_scrapSkip[i] == rva;
			if (vanilla)
			{
				Count(kScrapRoutedVanilla);
				return PrevScrapAlloc()(self, size, alignment);
			}
		}
	}

	// the header sits in the `a` bytes in front of the user block, so the
	// user block keeps the alignment asked for.
	const std::size_t a = alignment < 16 ? 16 : alignment;
	char * base = static_cast<char *>(g_scrapZero
		? mi_heap_zalloc_aligned(g_scrapHeap, size + a, a)
		: mi_heap_malloc_aligned(g_scrapHeap, size + a, a));
	if (OwnsScrap(base))
	{
		char * p = base + a;
		ScrapHeader * h = HeaderOf(p);
		h->owner = self;
		h->offset = static_cast<std::uint32_t>(a);
		h->magic = kScrapLive;
		return p;
	}

	const int err = errno;
	if (base)
		mi_free(base);
	void * q = PrevScrapAlloc()(self, size, alignment);
	NoteOverflow("scrap_alloc", size, alignment, true, base ? "outside the arena" : "mimalloc null", err, _ReturnAddress(), q);
	return q;
}

// what vanilla ScrapHeap::Deallocate(self, p) does with a pointer we can
// place; false when p belongs to the old allocator.
bool ScrapFreeOurs(void * self, void * p)
{
	if (OwnsScrap(p))
	{
		if (HeaderOf(p)->owner != self)
		{
			// vanilla: outside this heap's range, ignored.
			Count(kScrapWrongHeap);
			return true;
		}
		ScrapRelease(p);
		return true;
	}
	if (Owns(p))
	{
		// vanilla: a MemoryManager block is outside the scrap range, ignored.
		Count(kScrapFreeOfMM);
		return true;
	}
	return false;
}

void ScrapDeallocate(void * self, void * p)
{
	if (!p || ScrapFreeOurs(self, p))
		return;
	Count(kForeignScrapFree);
	PrevScrapFree()(self, p);
}

// the virtual DeallocateAlignImpl(void *&) (vtable slot 5, id 68140): its
// own copy of the free code, used by MemoryManager::Deallocate's dispatch
// and by virtual callers. vanilla clears the caller's pointer.
void ScrapDeallocateRef(void * self, void ** pp)
{
	void * p = *pp;
	if (!p || ScrapFreeOurs(self, p))
	{
		*pp = nullptr;
		return;
	}
	Count(kForeignScrapFree);
	PrevScrapFreeRef()(self, pp);
}

// ---- sites -------------------------------------------------------------

alloc_patch::Site MakeSite(const char * name, std::uint32_t id, std::initializer_list<std::uint8_t> bytes,
	std::size_t stolen, std::size_t skip, void * hook)
{
	alloc_patch::Site s;
	s.name = name;
	s.id = id;
	std::size_t i = 0;
	for (std::uint8_t b : bytes)
		s.vanilla[i++] = b;
	s.vanillaLen = i;
	s.stolen = stolen;
	s.skip = skip;
	s.hook = hook;
	return s;
}

// frees first, allocation last (see the file comment). the byte strings
// are the 1.7.104 entries; `stolen` covers only whole, position-independent
// instructions; the two frees open with `test rdx, rdx; je`, which our
// hooks do themselves, so the old code is entered past it at +9.
alloc_patch::Site g_mmSites[4];
alloc_patch::Site g_scrapSites[3];
bool g_mmLive = false;
bool g_scrapLive = false;

void BuildSites()
{
	g_mmSites[0] = MakeSite("MemoryManager::Size", 68100,
		{ 0x40, 0x53, 0x55, 0x57, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b, 0xf9, 0x41, 0x0f, 0xb6, 0xe8 },
		8, 0, reinterpret_cast<void *>(&MMSize));		// push rbx; push rbp; push rdi; sub rsp, 20h
	g_mmSites[1] = MakeSite("MemoryManager::Deallocate", 68117,
		{ 0x48, 0x85, 0xd2, 0x0f, 0x84, 0x0a, 0x01, 0x00, 0x00, 0x48, 0x89, 0x5c, 0x24, 0x08 },
		0, 9, reinterpret_cast<void *>(&MMDeallocate));
	g_mmSites[2] = MakeSite("MemoryManager::Reallocate", 68116,
		{ 0x40, 0x53, 0x55, 0x57, 0x41, 0x54, 0x48, 0x83, 0xec, 0x28, 0x33, 0xed },
		6, 0, reinterpret_cast<void *>(&MMReallocate));	// push rbx; push rbp; push rdi; push r12
	g_mmSites[3] = MakeSite("MemoryManager::Allocate", 68115,
		{ 0x48, 0x89, 0x5c, 0x24, 0x20, 0x55, 0x57, 0x41, 0x54 },
		5, 0, reinterpret_cast<void *>(&MMAllocate));		// mov [rsp+20h], rbx

	g_scrapSites[0] = MakeSite("ScrapHeap::Deallocate", 68146,
		{ 0x48, 0x85, 0xd2, 0x0f, 0x84, 0x34, 0x01, 0x00, 0x00, 0x57, 0x48, 0x83, 0xec, 0x20 },
		0, 9, reinterpret_cast<void *>(&ScrapDeallocate));
	g_scrapSites[1] = MakeSite("ScrapHeap::DeallocateAlignImpl", 68140,
		{ 0x40, 0x56, 0x57, 0x41, 0x56, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b, 0x32 },
		5, 0, reinterpret_cast<void *>(&ScrapDeallocateRef));	// push rsi; push rdi; push r14
	g_scrapSites[2] = MakeSite("ScrapHeap::Allocate", 68144,
		{ 0x4c, 0x89, 0x44, 0x24, 0x18, 0x53, 0x57, 0x41, 0x54 },
		5, 0, reinterpret_cast<void *>(&ScrapAllocate));	// mov [rsp+18h], r8
}

// ---- logging helpers ---------------------------------------------------

std::string JsonStr(const std::string & s)
{
	std::string out = "\"";
	for (char c : s)
	{
		if (c == '"' || c == '\\')
			out += '\\';
		out += c;
	}
	return out + "\"";
}

std::string Hex(std::uint64_t v)
{
	char buf[32];
	sprintf_s(buf, "\"0x%llx\"", static_cast<unsigned long long>(v));
	return buf;
}

// "vanilla", or the owner of the previous target when all sites agree,
// else "mixed".
std::string GroupPrev(const alloc_patch::Site * sites, std::size_t n)
{
	std::string owner = sites[0].owner;
	for (std::size_t i = 1; i < n; i++)
	{
		if (sites[i].owner != owner)
			return "mixed";
	}
	return owner;
}

std::string SiteList(const alloc_patch::Site * sites, std::size_t n, std::uintptr_t image)
{
	std::string out = "[";
	for (std::size_t i = 0; i < n; i++)
	{
		const auto & s = sites[i];
		if (i)
			out += ", ";
		out += "{\"name\": " + JsonStr(s.name) + ", \"id\": " + std::to_string(s.id) +
			", \"rva\": " + Hex(s.entry ? s.entry - image : 0) +
			", \"was\": \"" + alloc_patch::KindName(s.kind) + "\"" +
			", \"owner\": " + JsonStr(s.owner) + "}";
	}
	return out + "]";
}

void MiOutput(const char * msg, void *)
{
	std::string line = msg ? msg : "";
	while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
		line.pop_back();
	if (!line.empty())
		skybench_Log("allocator: mimalloc: " + line);
}

// ---- stats thread ------------------------------------------------------

DWORD WINAPI StatsThread(void * param)
{
	const DWORD intervalMs = static_cast<DWORD>(reinterpret_cast<std::uintptr_t>(param)) * 1000;
	bool overwrittenLogged = false;

	for (;;)
	{
		Sleep(intervalMs);

		mi_stats_t st;
		mi_stats_init(&st);
		mi_stats_get(&st);

		size_t elapsed = 0, user = 0, sys = 0, rss = 0, peakRss = 0, commit = 0, peakCommit = 0, faults = 0;
		mi_process_info(&elapsed, &user, &sys, &rss, &peakRss, &commit, &peakCommit, &faults);

		char fields[1024];
		sprintf_s(fields,
			", \"arena_committed_mb\": %.1f, \"arena_committed_peak_mb\": %.1f"
			", \"purged_mb\": %.1f, \"commit_calls\": %lld, \"purge_calls\": %lld"
			", \"threads\": %lld, \"os_pages_outside_arena\": %lld"
			", \"foreign_free\": %llu, \"foreign_realloc\": %llu, \"foreign_size\": %llu"
			", \"foreign_scrap_free\": %llu, \"overflow\": %llu"
			", \"scrap_wrong_heap\": %llu, \"scrap_double_free\": %llu, \"scrap_free_of_mm\": %llu"
			", \"scrap_via_mm\": %llu, \"scrap_too_big\": %llu, \"scrap_routed_vanilla\": %llu"
			", \"process_commit_mb\": %.1f, \"process_peak_commit_mb\": %.1f, \"page_faults\": %llu",
			st.committed.current / 1048576.0, st.committed.peak / 1048576.0,
			st.purged.total / 1048576.0, static_cast<long long>(st.commit_calls.total),
			static_cast<long long>(st.purge_calls.total),
			static_cast<long long>(st.threads.current), static_cast<long long>(st.pages_os_allocated.current),
			static_cast<unsigned long long>(Total(kForeignFree)),
			static_cast<unsigned long long>(Total(kForeignRealloc)),
			static_cast<unsigned long long>(Total(kForeignSize)),
			static_cast<unsigned long long>(Total(kForeignScrapFree)),
			static_cast<unsigned long long>(Total(kOverflow)),
			static_cast<unsigned long long>(Total(kScrapWrongHeap)),
			static_cast<unsigned long long>(Total(kScrapDoubleFree)),
			static_cast<unsigned long long>(Total(kScrapFreeOfMM)),
			static_cast<unsigned long long>(Total(kScrapViaMM)),
			static_cast<unsigned long long>(Total(kScrapTooBig)),
			static_cast<unsigned long long>(Total(kScrapRoutedVanilla)),
			commit / 1048576.0, peakCommit / 1048576.0, static_cast<unsigned long long>(faults));
		if (g_census && g_scrapLive)
			skybench_LogEvent("allocator_stats", std::string(fields) + ", \"scrap_callers\": " + CensusTop(16));
		else
			skybench_LogEvent("allocator_stats", fields);

		if (!overwrittenLogged)
		{
			std::string lost;
			for (auto & s : g_mmSites)
			{
				if (g_mmLive && s.stub && !alloc_patch::StillOurs(s))
					lost += std::string(lost.empty() ? "" : ", ") + s.name;
			}
			for (auto & s : g_scrapSites)
			{
				if (g_scrapLive && s.stub && !alloc_patch::StillOurs(s))
					lost += std::string(lost.empty() ? "" : ", ") + s.name;
			}
			if (!lost.empty())
			{
				overwrittenLogged = true;
				skybench_Log("allocator: WARNING another plugin patched over our entries after load: " + lost +
					". blocks we made may now be freed by the wrong allocator.");
				skybench_LogEvent("allocator_overwritten", ", \"sites\": " + JsonStr(lost));
			}
		}
	}
}

// ---- install -----------------------------------------------------------

void Skip(const std::string & reason)
{
	skybench_Log("allocator: patched nothing - " + reason);
	skybench_LogEvent("allocator", ", \"status\": \"skipped\", \"reason\": " + JsonStr(reason));
}

bool TakeGroup(alloc_patch::Site * sites, std::size_t n, void ** prev, std::uintptr_t image,
	const AddressLibrary & lib, const char * group, std::string & outcome)
{
	std::string why;
	for (std::size_t i = 0; i < n; i++)
	{
		if (!alloc_patch::Prepare(sites[i], image, lib.Rva(sites[i].id), why))
		{
			outcome = why;
			skybench_Log(std::string("allocator: ") + group + " left alone - " + why);
			return false;
		}
	}
	if (!alloc_patch::Commit(sites, n, prev, why))
	{
		outcome = why;
		skybench_Log(std::string("allocator: ") + group + " commit failed - " + why);
		return false;
	}
	outcome = "installed";
	return true;
}

} // namespace

void InstallAllocator(const SKSEInterface *, unsigned long)
{
	if (Env("SKYBENCH_ALLOCATOR") != "1")
		return;

	const Config cfg = ReadConfig();
	BuildSites();

	AddressLibrary lib;
	if (!lib.Open(1, 7, 104))
	{
		Skip("address library: " + lib.error());
		return;
	}

	const std::uintptr_t image = reinterpret_cast<std::uintptr_t>(GetModuleHandleA(nullptr));
	g_exeBase = image;
	g_scrapMin = static_cast<std::size_t>(cfg.scrapMin);
	g_scrapMax = static_cast<std::size_t>(cfg.scrapMax);
	g_scrapZero = cfg.scrapZero;
	g_census = cfg.scrapCensus;
	{
		std::size_t pos = 0;
		while (pos < cfg.scrapSkip.size() && g_scrapSkipCount < 16)
		{
			std::size_t comma = cfg.scrapSkip.find(',', pos);
			std::string tok = cfg.scrapSkip.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
			if (tok.rfind("exe+", 0) == 0)
				tok = tok.substr(4);
			if (!tok.empty())
				g_scrapSkip[g_scrapSkipCount++] = static_cast<std::uintptr_t>(strtoull(tok.c_str(), nullptr, 16));
			if (comma == std::string::npos)
				break;
			pos = comma + 1;
		}
	}
	g_scrapFilter = g_scrapMin > 0 || g_scrapMax != ~std::size_t(0) || g_scrapSkipCount > 0;
	const bool efLoaded = GetModuleHandleA("EngineFixes.dll") != nullptr;

	// mimalloc first: the arena must exist before any entry points at us.
	mi_register_output(&MiOutput, nullptr);
	mi_option_set(mi_option_purge_delay, cfg.purgeMs);
	mi_option_set(mi_option_allow_large_os_pages, cfg.largePages ? 1 : 0);
	mi_option_set(mi_option_purge_decommits, cfg.purgeDecommits ? 1 : 0);
	mi_option_set(mi_option_show_errors, 1);	// mimalloc's own complaints go to skybench.log

	// scrap blocks go back through the MemoryManager in vanilla (its
	// Deallocate finds the owning scrap heap), so the scrap group is only
	// safe when the MemoryManager group is ours too: otherwise vanilla (or
	// engine fixes) would hand our scrap blocks to the crt or tbb.
	const bool wantScrap = cfg.scrap && cfg.mm;
	if (!cfg.mm)
	{
		Skip(cfg.scrap
			? "SKYBENCH_ALLOC_PARTS=scrap without mm: vanilla MemoryManager::Deallocate would hand our scrap blocks to the crt"
			: "SKYBENCH_ALLOC_PARTS names no group");
		return;
	}

	auto reserve = [&](long gb, std::uintptr_t & base, std::uintptr_t & span, mi_heap_t *& heap) {
		mi_arena_id_t arena = {};
		int err = mi_reserve_os_memory_ex(static_cast<std::size_t>(gb) << 30, cfg.largePages, cfg.largePages, true, &arena);
		if (err != 0)
			return "could not reserve a " + std::to_string(gb) + " GiB arena (error " + std::to_string(err) + ")";
		std::size_t areaSize = 0;
		void * area = mi_arena_area(arena, &areaSize);
		heap = mi_heap_new_in_arena(arena);
		if (!area || !areaSize || !heap)
			return std::string("mimalloc arena or heap setup failed");
		base = reinterpret_cast<std::uintptr_t>(area);
		span = areaSize;
		return std::string();
	};

	std::string why = reserve(cfg.arenaGb, g_base, g_span, g_heap);
	if (why.empty() && wantScrap)
		why = reserve(cfg.scrapArenaGb, g_scrapBase, g_scrapSpan, g_scrapHeap);
	if (!why.empty())
	{
		Skip(why);
		return;
	}

	// before any entry points at us, so the first fault after it is caught.
	if (cfg.crashLog)
		InstallAllocatorCrashHandler(g_base, g_span, g_scrapBase, g_scrapSpan);

	std::string mmOutcome = "off";
	std::string scrapOutcome = "off";

	if (cfg.mm)
	{
		g_mmLive = TakeGroup(g_mmSites, 4, g_mmPrev, image, lib, "MemoryManager", mmOutcome);
	}
	if (wantScrap && g_mmLive)
	{
		g_scrapLive = TakeGroup(g_scrapSites, 3, g_scrapPrev, image, lib, "ScrapHeap", scrapOutcome);
	}
	else if (cfg.scrap)
	{
		scrapOutcome = "needs the MemoryManager group (SKYBENCH_ALLOC_PARTS must include mm, and mm must install)";
		skybench_Log("allocator: ScrapHeap left alone - " + scrapOutcome);
	}

	// a partial commit (a later VirtualProtect failing) leaves the earlier
	// entries live, which is consistent (frees go first); report what is
	// actually live, entry by entry.
	auto liveList = [](const alloc_patch::Site * sites, std::size_t n) {
		std::string out;
		for (std::size_t i = 0; i < n; i++)
		{
			if (alloc_patch::StillOurs(sites[i]))
				out += std::string(out.empty() ? "" : ", ") + sites[i].name;
		}
		return out;
	};

	const std::string mmPrev = cfg.mm ? GroupPrev(g_mmSites, 4) : "";
	const std::string scrapPrev = cfg.scrap ? GroupPrev(g_scrapSites, 3) : "";
	const bool efChained = mmPrev == "EngineFixes.dll" || scrapPrev == "EngineFixes.dll";

	std::string fields =
		", \"status\": " + JsonStr((g_mmLive || g_scrapLive) ? "installed" : "skipped") +
		", \"allocator\": \"mimalloc " + std::to_string(MI_MALLOC_VERSION) + "\"" +
		", \"mm\": " + JsonStr(mmOutcome) + ", \"mm_prev\": " + JsonStr(mmPrev) +
		", \"scrap\": " + JsonStr(scrapOutcome) + ", \"scrap_prev\": " + JsonStr(scrapPrev) +
		", \"live\": " + JsonStr(liveList(g_mmSites, 4) + (g_scrapLive ? std::string(g_mmLive ? ", " : "") + liveList(g_scrapSites, 3) : "")) +
		", \"engine_fixes_loaded\": " + (efLoaded ? "true" : "false") +
		", \"engine_fixes_chained\": " + (efChained ? "true" : "false") +
		", \"arena_base\": " + Hex(g_base) + ", \"arena_mb\": " + std::to_string(g_span >> 20) +
		", \"large_pages\": " + (cfg.largePages ? "true" : "false") +
		", \"purge_ms\": " + std::to_string(cfg.purgeMs) +
		", \"scrap_arena_base\": " + Hex(g_scrapBase) + ", \"scrap_arena_mb\": " + std::to_string(g_scrapSpan >> 20) +
		", \"purge_decommits\": " + (cfg.purgeDecommits ? "true" : "false") +
		", \"crash_log\": " + (cfg.crashLog ? "true" : "false") +
		", \"scrap_min\": " + std::to_string(cfg.scrapMin) + ", \"scrap_max\": " + std::to_string(cfg.scrapMax) +
		", \"scrap_skip\": " + JsonStr(cfg.scrapSkip) +
		", \"scrap_zero\": " + (cfg.scrapZero ? "true" : "false") +
		", \"scrap_census\": " + (cfg.scrapCensus ? "true" : "false") +
		", \"sites\": " + SiteList(g_mmSites, cfg.mm ? 4 : 0, image) +
		", \"scrap_sites\": " + SiteList(g_scrapSites, g_scrapLive ? 3 : 0, image);
	skybench_LogEvent("allocator", fields);

	if (efChained)
	{
		skybench_Log("allocator: engine fixes had already replaced these entries (its MemoryManager/ScrapHeap "
			"patches). skybench now serves every new block; engine fixes' allocator only frees the blocks it "
			"made before skybench loaded. this is safe with its options on or off; its other fixes are untouched.");
	}
	else if (efLoaded)
	{
		skybench_Log("allocator: engine fixes is loaded but its allocator patches are off; skybench took the vanilla entries.");
	}

	if ((g_mmLive || g_scrapLive) && cfg.statsSeconds > 0)
	{
		HANDLE t = CreateThread(nullptr, 64 * 1024, &StatsThread,
			reinterpret_cast<void *>(static_cast<std::uintptr_t>(cfg.statsSeconds)), 0, nullptr);
		if (t)
			CloseHandle(t);
	}
}
