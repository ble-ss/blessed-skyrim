// occlusion_probe - stage 0 of occlusion culling: force-culls a stable hashed fraction of main-view statics (SKYBENCH_OCCLUSION_PROBE).
//
// the question it answers: what is one culled draw worth in fps here? the
// lead runs fraction 0, 0.1, 0.3 and 0.5 and reads fps, cpu busy and draws
// per frame (stage 0 and the +3% gate).
//
// the hook: a body detour on BSCullingProcess::Process1 (no direct call to
// it exists in 1.7.104; the pointers sit only in vtables). the first
// instruction (mov [rsp+18h], rbx, 5 bytes) becomes a jmp; the trampoline
// runs that instruction and jumps back to +5.
//
// main view only: process->camera (+0x18) must be the WorldRoot camera
// (*(WorldRoot) + 0x128, what Main::WorldRootCamera returns). shadow,
// reflection, cubemap and first-person culls have their own cameras and
// pass through untouched.
//
// eligible, all of:
//   - Process1 would test it: cullMode (+0x30198) is 0, the world bound
//     radius is not 0 or flag 0x800 is set, flag 0x1000 is clear.
//   - it is a reference's own 3D root: NiAVObject::userData (+0xf8) is set.
//     object lod and terrain have no reference, so they never qualify.
//   - the reference is a REFR (0x3d), never an actor (ACHR 0x3e), and its
//     base form is STAT, SCOL, MSTT, TREE, FLOR, FURN or CONT.
//   - it does not reach the near plane: distance(bound centre, camera) is
//     at least radius + 2 * near (keeps near-plane straddlers).
// culled when hash(formID) < fraction. a formID is stable across frames
// and across runs, so the same objects stay culled and the 0.1 set sits
// inside the 0.3 set. a force-cull does what the engine's own all-fail path
// does (cullMode 2): skip the visit and, when the process recurses
// (+0x301d4) and updates the accumulate flag (+0x11d), clear bit 26 of the
// object's flags.
//
// 1.7.104 facts (capstone over the exe and the address library bin):
//   Process1            id 76553   rva 0xfeda70   prologue + head checked
//   all-fail path       rva 0xfedd26 (Process1 + 0x2b6), checked
//   WorldRoot           id 403513  rva 0x3230740
//   WorldRootCamera     id 36609   rva 0x65c480   mov rax,[WorldRoot]; mov rax,[rax+128h]
//   BSGraphics::State   id 411479  rva 0x33363a0  frameCount at +0x54
//   frame increment     id 77246 + 0xbb  rva 0x100a02b  inc [State+54h], right after Present

#include "occlusion_probe.h"

#include "address_library.h"
#include "call_patch.h"
#include "skybench_log.h"

#include "skse64/PluginAPI.h"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

const std::uint32_t kProcess1Id = 76553;
const std::uint32_t kWorldRootId = 403513;
const std::uint32_t kWorldRootCameraId = 36609;
const std::uint32_t kStateId = 411479;
const std::uint32_t kPresentFnId = 77246;
const std::uint32_t kFrameIncOffset = 0xbb;
const std::uint32_t kFrameCountOffset = 0x54;
const std::uint32_t kFailPathOffset = 0x2b6;

// Process1 from its entry up to the cullMode checks.
const std::uint8_t kHead[0x5c] = {
	0x48, 0x89, 0x5c, 0x24, 0x18,						// mov [rsp+18h], rbx   (relocated)
	0x48, 0x89, 0x74, 0x24, 0x20,						// mov [rsp+20h], rsi
	0x57,												// push rdi
	0x48, 0x81, 0xec, 0x20, 0x04, 0x00, 0x00,			// sub rsp, 420h
	0x83, 0xba, 0xf0, 0x00, 0x00, 0x00, 0x00,			// cmp dword [rdx+0f0h], 0     radius
	0x41, 0x8b, 0xf0, 0x48, 0x8b, 0xda, 0x48, 0x8b, 0xf9,
	0x75, 0x11,
	0x8b, 0x82, 0xf4, 0x00, 0x00, 0x00,					// mov eax, [rdx+0f4h]         flags
	0xc1, 0xe8, 0x0b, 0xa8, 0x01, 0x0f, 0x84, 0xa1, 0x02, 0x00, 0x00,
	0x8b, 0x91, 0x98, 0x01, 0x03, 0x00,					// mov edx, [rcx+30198h]       cullMode
	0x48, 0x89, 0xac, 0x24, 0x30, 0x04, 0x00, 0x00,
	0x0f, 0xb6, 0xa9, 0xd4, 0x01, 0x03, 0x00,			// movzx ebp, byte [rcx+301d4h]
	0x83, 0xfa, 0x02, 0x0f, 0x84, 0x63, 0x02, 0x00, 0x00,	// cullMode 2: all fail
	0x83, 0xfa, 0x01, 0x0f, 0x84, 0x23, 0x02, 0x00, 0x00,	// cullMode 1: all pass
};
const std::size_t kRelocated = 5;

// the all-fail path the force-cull copies.
const std::uint8_t kFailPath[0x18] = {
	0x40, 0x84, 0xed, 0x74, 0x13,						// test bpl, bpl; je out
	0x80, 0xb9, 0x1d, 0x01, 0x00, 0x00, 0x00, 0x74, 0x0a,	// cmp byte [rcx+11dh], 0; je out
	0x81, 0xa3, 0xf4, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xfb,	// and dword [rbx+0f4h], ~(1 << 26)
};

// BSCullingProcess / NiCullingProcess
const std::size_t kProcCamera = 0x18;
const std::size_t kProcAccumulate = 0x11d;
const std::size_t kProcCullMode = 0x30198;
const std::size_t kProcRecurse = 0x301d4;
// NiAVObject
const std::size_t kObjBound = 0xe4;			// NiBound: centre xyz, radius
const std::size_t kObjFlags = 0xf4;
const std::size_t kObjUserData = 0xf8;
const std::size_t kObjWorldTranslate = 0xa0;	// world (0x7c) + rotate (0x24)
const std::uint32_t kFlagBoundless = 0x800;
const std::uint32_t kFlag12 = 0x1000;
const std::uint32_t kFlagAccumulated = 0x4000000;
// NiCamera
const std::size_t kCameraNear = 0x160;		// viewFrustum (0x150) + near (0x10)
// SceneGraph
const std::size_t kSceneCamera = 0x128;
// TESForm / TESObjectREFR
const std::size_t kFormId = 0x14;
const std::size_t kFormType = 0x1a;
const std::size_t kRefBase = 0x40;
const std::uint8_t kTypeREFR = 0x3d;
const std::uint8_t kTypeACHR = 0x3e;

bool IsStaticBase(std::uint8_t t)
{
	switch (t)
	{
	case 0x22:	// STAT
	case 0x23:	// SCOL
	case 0x24:	// MSTT
	case 0x26:	// TREE
	case 0x27:	// FLOR
	case 0x28:	// FURN
	case 0x1c:	// CONT
		return true;
	default:
		return false;
	}
}

// per-thread counters: each cull job thread owns one cache line, so the
// hot path has no locked instructions. the tick sums them.
enum Counter
{
	kCalls,		// every Process1 call, any camera
	kTested,	// main-camera calls
	kRefs,		// main-camera calls on a reference's 3D root
	kActors,	// ... that is an actor
	kNear,		// ... a static reference kept for the near plane
	kEligible,	// ... a static reference that could be culled
	kCulled,	// ... that the hash culled
	kCounterCount
};
const char * const kCounterNames[kCounterCount] = {
	"calls", "tested", "refs", "actors", "near", "eligible", "culled"
};

struct alignas(64) Slot
{
	std::atomic<std::uint64_t> c[kCounterCount];
};
const std::uint32_t kMaxSlots = 64;
Slot g_slots[kMaxSlots] = {};
std::atomic<std::uint32_t> g_slotCount{ 0 };
thread_local Slot * t_slot = nullptr;
thread_local bool t_shared = false;
thread_local std::uint32_t t_lastFrame = 0;

inline void Bump(Counter c)
{
	Slot * s = t_slot;
	if (!s)
	{
		const std::uint32_t i = g_slotCount.fetch_add(1, std::memory_order_relaxed);
		t_shared = i >= kMaxSlots - 1;
		s = t_slot = &g_slots[t_shared ? kMaxSlots - 1 : i];
	}
	if (t_shared)
		s->c[c].fetch_add(1, std::memory_order_relaxed);
	else
		s->c[c].store(s->c[c].load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
}

using Process1Fn = void (*)(void * process, void * object, std::uintptr_t arg);

Process1Fn g_original = nullptr;
void ** g_worldRoot = nullptr;
const volatile std::uint32_t * g_frameCount = nullptr;
std::uint32_t g_threshold = 0;		// cull when hash < threshold (24-bit)
double g_fraction = 0.0;

// the tick: one thread per 5 s window claims it with a compare-exchange.
const std::uint64_t kTickMs = 5000;
std::atomic<std::uint64_t> g_nextTickMs{ 0 };
std::uint64_t g_lastTickMs = 0;
std::uint32_t g_lastTickFrame = 0;
std::uint64_t g_lastSums[kCounterCount] = {};

template <class T>
inline T Read(const void * p, std::size_t off)
{
	T v;
	std::memcpy(&v, static_cast<const std::uint8_t *>(p) + off, sizeof(T));
	return v;
}

// splitmix64 finalizer, top 24 bits.
inline std::uint32_t Hash24(std::uint32_t formId)
{
	std::uint64_t z = formId + 0x9e3779b97f4a7c15ull;
	z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
	z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
	z ^= z >> 31;
	return static_cast<std::uint32_t>(z >> 40);
}

void Tick(std::uint32_t frame)
{
	const std::uint64_t now = GetTickCount64();
	std::uint64_t due = g_nextTickMs.load(std::memory_order_relaxed);
	if (now < due || !g_nextTickMs.compare_exchange_strong(due, now + kTickMs))
		return;

	std::uint64_t sums[kCounterCount] = {};
	const std::uint32_t used = g_slotCount.load(std::memory_order_relaxed);
	const std::uint32_t n = used < kMaxSlots ? used : kMaxSlots;
	for (std::uint32_t i = 0; i < n; i++)
		for (int c = 0; c < kCounterCount; c++)
			sums[c] += g_slots[i].c[c].load(std::memory_order_relaxed);

	if (g_lastTickMs == 0)
	{
		// first frame seen: start the window, emit nothing yet.
		g_lastTickMs = now;
		g_lastTickFrame = frame;
		std::memcpy(g_lastSums, sums, sizeof(sums));
		return;
	}

	const std::uint32_t frames = frame - g_lastTickFrame;
	char buf[128];
	sprintf_s(buf, ", \"fraction\": %.3f, \"window_s\": %.3f, \"frames\": %u, \"threads\": %u",
		g_fraction, (now - g_lastTickMs) / 1000.0, frames, used);
	std::string fields = buf;
	for (int c = 0; c < kCounterCount; c++)
	{
		const std::uint64_t d = sums[c] - g_lastSums[c];
		sprintf_s(buf, ", \"%s\": %llu, \"%s_pf\": %.1f", kCounterNames[c],
			static_cast<unsigned long long>(d), kCounterNames[c], frames ? static_cast<double>(d) / frames : 0.0);
		fields += buf;
	}
	skybench_LogEvent("occlusion_probe_tick", fields);

	g_lastTickMs = now;
	g_lastTickFrame = frame;
	std::memcpy(g_lastSums, sums, sizeof(sums));
}

// true when the object is a static reference we may cull.
bool Eligible(const std::uint8_t * process, const std::uint8_t * obj, const std::uint8_t * camera)
{
	if (Read<std::uint32_t>(process, kProcCullMode) != 0)
		return false;
	const std::uint32_t flags = Read<std::uint32_t>(obj, kObjFlags);
	const float radius = Read<float>(obj, kObjBound + 12);
	if ((radius == 0.0f && !(flags & kFlagBoundless)) || (flags & kFlag12))
		return false;

	const auto * ref = Read<const std::uint8_t *>(obj, kObjUserData);
	if (!ref)
		return false;
	Bump(kRefs);
	const std::uint8_t refType = Read<std::uint8_t>(ref, kFormType);
	if (refType == kTypeACHR)
	{
		Bump(kActors);
		return false;
	}
	if (refType != kTypeREFR)
		return false;
	const auto * base = Read<const std::uint8_t *>(ref, kRefBase);
	if (!base || !IsStaticBase(Read<std::uint8_t>(base, kFormType)))
		return false;

	const float dx = Read<float>(obj, kObjBound + 0) - Read<float>(camera, kObjWorldTranslate + 0);
	const float dy = Read<float>(obj, kObjBound + 4) - Read<float>(camera, kObjWorldTranslate + 4);
	const float dz = Read<float>(obj, kObjBound + 8) - Read<float>(camera, kObjWorldTranslate + 8);
	const float reach = radius + 2.0f * Read<float>(camera, kCameraNear);
	if (dx * dx + dy * dy + dz * dz < reach * reach)
	{
		Bump(kNear);
		return false;
	}
	Bump(kEligible);
	return true;
}

void Thunk(void * process, void * object, std::uintptr_t arg)
{
	Bump(kCalls);

	auto * p = static_cast<std::uint8_t *>(process);
	auto * obj = static_cast<std::uint8_t *>(object);
	const auto * camera = Read<const std::uint8_t *>(p, kProcCamera);
	const auto * root = static_cast<const std::uint8_t *>(*g_worldRoot);
	if (!camera || !root || camera != Read<const std::uint8_t *>(root, kSceneCamera))
	{
		g_original(process, object, arg);
		return;
	}

	Bump(kTested);
	const std::uint32_t frame = *g_frameCount;
	if (frame != t_lastFrame)
	{
		t_lastFrame = frame;
		Tick(frame);
	}

	if (!Eligible(p, obj, camera))
	{
		g_original(process, object, arg);
		return;
	}

	const auto * ref = Read<const std::uint8_t *>(obj, kObjUserData);
	if (Hash24(Read<std::uint32_t>(ref, kFormId)) >= g_threshold)
	{
		g_original(process, object, arg);
		return;
	}

	// the engine's all-fail path.
	Bump(kCulled);
	if (p[kProcRecurse] && p[kProcAccumulate])
	{
		std::uint32_t flags = Read<std::uint32_t>(obj, kObjFlags);
		flags &= ~kFlagAccumulated;
		std::memcpy(obj + kObjFlags, &flags, sizeof(flags));
	}
}

std::string HexBytes(const std::uint8_t * p, std::size_t n)
{
	std::string out = "\"";
	char buf[4];
	for (std::size_t i = 0; i < n; i++)
	{
		sprintf_s(buf, i ? " %02x" : "%02x", p[i]);
		out += buf;
	}
	return out + "\"";
}

void Skip(const std::string & reason, const std::string & extra = std::string())
{
	skybench_Log("occlusion_probe: patched nothing - " + reason);
	skybench_LogEvent("occlusion_probe", ", \"status\": \"skipped\", \"reason\": \"" + reason + "\"" + extra);
}

// true when [p, p + n) is committed, executable memory inside the exe image.
bool IsExeCode(std::uintptr_t base, std::uintptr_t p, std::size_t n)
{
	MEMORY_BASIC_INFORMATION mbi = {};
	if (!VirtualQuery(reinterpret_cast<void *>(p), &mbi, sizeof(mbi)))
		return false;
	if (reinterpret_cast<std::uintptr_t>(mbi.AllocationBase) != base || mbi.State != MEM_COMMIT)
		return false;
	const DWORD exec = PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
	if (!(mbi.Protect & exec))
		return false;
	return p + n <= reinterpret_cast<std::uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
}

// where a rip-relative disp32 at site + dispAt (instruction length len) points.
std::uintptr_t RipTarget(std::uintptr_t site, std::size_t dispAt, std::size_t len)
{
	std::int32_t rel = 0;
	std::memcpy(&rel, reinterpret_cast<const void *>(site + dispAt), 4);
	return site + len + static_cast<std::intptr_t>(rel);
}

// writes jmp [rip + 0]; dq target (14 bytes) at p.
void WriteAbsJump(std::uint8_t * p, std::uintptr_t target)
{
	p[0] = 0xff;
	p[1] = 0x25;
	std::memset(p + 2, 0, 4);
	std::memcpy(p + 6, &target, 8);
}

} // namespace

void InstallOcclusionProbe(const SKSEInterface * skse, unsigned long pluginHandle)
{
	char * val = nullptr;
	std::size_t len = 0;
	if (_dupenv_s(&val, &len, "SKYBENCH_OCCLUSION_PROBE") != 0 || !val)
		return;
	const std::string raw(val);
	free(val);

	char * end = nullptr;
	const double fraction = std::strtod(raw.c_str(), &end);
	if (end == raw.c_str() || *end != '\0' || !(fraction >= 0.0 && fraction <= 1.0))
	{
		Skip("SKYBENCH_OCCLUSION_PROBE is not a fraction from 0 to 1");
		return;
	}
	g_fraction = fraction;
	g_threshold = static_cast<std::uint32_t>(std::lround(fraction * 16777216.0));

	AddressLibrary lib;
	if (!lib.Open(1, 7, 104))
	{
		Skip("address library: " + lib.error());
		return;
	}
	const std::uint32_t fnRva = lib.Rva(kProcess1Id);
	const std::uint32_t rootRva = lib.Rva(kWorldRootId);
	const std::uint32_t camFnRva = lib.Rva(kWorldRootCameraId);
	const std::uint32_t stateRva = lib.Rva(kStateId);
	const std::uint32_t presentRva = lib.Rva(kPresentFnId);
	if (!fnRva || !rootRva || !camFnRva || !stateRva || !presentRva)
	{
		Skip("id not in address library");
		return;
	}

	const std::uintptr_t base = reinterpret_cast<std::uintptr_t>(GetModuleHandleA(nullptr));
	const std::uintptr_t fn = base + fnRva;
	const std::string where = ", \"fn_rva\": " + JsonHex(fnRva);

	// Process1: the head and the all-fail path.
	if (!IsExeCode(base, fn, kFailPathOffset + sizeof(kFailPath)))
	{
		Skip("Process1 is not exe code", where);
		return;
	}
	const std::string bytes = where + ", \"bytes\": " + HexBytes(reinterpret_cast<const std::uint8_t *>(fn), 18);
	if (std::memcmp(reinterpret_cast<const void *>(fn), kHead, sizeof(kHead)) != 0)
	{
		Skip("unexpected bytes in Process1 (another hook got here first?)", bytes);
		return;
	}
	if (std::memcmp(reinterpret_cast<const void *>(fn + kFailPathOffset), kFailPath, sizeof(kFailPath)) != 0)
	{
		Skip("Process1 all-fail path not found", bytes);
		return;
	}

	// WorldRootCamera: mov rax, [rip + WorldRoot]; mov rax, [rax + 128h]; ret.
	const std::uintptr_t camFn = base + camFnRva;
	const std::uint8_t camTail[8] = { 0x48, 0x8b, 0x80, 0x28, 0x01, 0x00, 0x00, 0xc3 };
	if (!IsExeCode(base, camFn, 15) ||
		std::memcmp(reinterpret_cast<const void *>(camFn), "\x48\x8b\x05", 3) != 0 ||
		std::memcmp(reinterpret_cast<const void *>(camFn + 7), camTail, sizeof(camTail)) != 0 ||
		RipTarget(camFn, 3, 7) != base + rootRva)
	{
		Skip("WorldRootCamera does not read WorldRoot + 0x128", bytes);
		return;
	}

	// the per-frame counter: inc dword [rip + State + 54h] after Present.
	const std::uintptr_t inc = base + presentRva + kFrameIncOffset;
	if (!IsExeCode(base, inc, 6) ||
		std::memcmp(reinterpret_cast<const void *>(inc), "\xff\x05", 2) != 0 ||
		RipTarget(inc, 2, 6) != base + stateRva + kFrameCountOffset)
	{
		Skip("frame counter increment not found", bytes);
		return;
	}

	auto * tramp = static_cast<SKSETrampolineInterface *>(skse->QueryInterface(kInterface_Trampoline));
	if (!tramp || !tramp->AllocateFromBranchPool)
	{
		Skip("no skse trampoline interface", bytes);
		return;
	}
	// one block: [0, 14) jump to Thunk; [14, 33) the relocated first
	// instruction, then a jump back to Process1 + 5.
	auto * stub = static_cast<std::uint8_t *>(tramp->AllocateFromBranchPool(pluginHandle, 14 + kRelocated + 14));
	if (!stub)
	{
		Skip("branch pool allocation failed", bytes);
		return;
	}
	const std::intptr_t stubRel = reinterpret_cast<std::intptr_t>(stub) - static_cast<std::intptr_t>(fn + 5);
	if (stubRel < INT32_MIN || stubRel > INT32_MAX)
	{
		Skip("branch pool stub out of rel32 range", bytes);
		return;
	}
	WriteAbsJump(stub, reinterpret_cast<std::uintptr_t>(&Thunk));
	std::uint8_t * original = stub + 14;
	std::memcpy(original, kHead, kRelocated);
	WriteAbsJump(original + kRelocated, fn + kRelocated);
	FlushInstructionCache(GetCurrentProcess(), stub, 14 + kRelocated + 14);

	g_original = reinterpret_cast<Process1Fn>(original);
	g_worldRoot = reinterpret_cast<void **>(base + rootRva);
	g_frameCount = reinterpret_cast<const volatile std::uint32_t *>(base + stateRva + kFrameCountOffset);

	DWORD oldProtect = 0;
	if (!VirtualProtect(reinterpret_cast<void *>(fn), 5, PAGE_EXECUTE_READWRITE, &oldProtect))
	{
		Skip("VirtualProtect failed", bytes);
		return;
	}
	std::uint8_t jmp[5] = { 0xe9 };
	const std::int32_t rel = static_cast<std::int32_t>(stubRel);
	std::memcpy(jmp + 1, &rel, 4);
	std::memcpy(reinterpret_cast<void *>(fn), jmp, sizeof(jmp));
	VirtualProtect(reinterpret_cast<void *>(fn), 5, oldProtect, &oldProtect);
	FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void *>(fn), 5);

	char buf[96];
	sprintf_s(buf, ", \"fraction\": %.3f, \"threshold\": %u", fraction, g_threshold);
	skybench_Log("occlusion_probe: detoured BSCullingProcess::Process1, fraction " + raw);
	skybench_LogEvent("occlusion_probe", ", \"status\": \"patched\"" + std::string(buf) + bytes +
		", \"world_root_rva\": " + JsonHex(rootRva) + ", \"state_rva\": " + JsonHex(stateRva));
}
