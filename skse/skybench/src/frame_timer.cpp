// frame_timer - rdtsc-brackets Main::Render and its main-thread engine calls (SKYBENCH_FRAME_TIMER).
//
// SKYBENCH_FRAME_TIMER=1 turns this on (default unset, does nothing) - the
// same pattern as SKYBENCH_HAVOK/SKYBENCH_PRECIP_OCCLUSION, not a build
// flag: the harness deploys one skybench.dll and picks the probe per run
// with env vars, so this is always compiled in and only ever installs its
// hooks when the var is "1".
//
// 1.7.104 facts (found with capstone over the exe and the address library
// bin, read-only):
//
//   Main::Render itself has exactly one caller in .text (a scan of every
//   e8 in the image): a small function, address-library id 36558, rva
//   0x656d10, that copies the frozen WorldRootCamera translation into two
//   other structures, calls two unrelated helper functions (rva 0x100a3f0,
//   0x1537420 - neither looks like game-logic/ai/havok/papyrus work; the
//   first loads a string pointer into rcx right before its call, which
//   looks like a marker/breadcrumb, not a tick), then calls Main::Render,
//   then restores a saved state byte. its call to Main::Render:
//     site   rva 0x656e2d   48 8b cd | e8 2e 00 00 00   (mov rcx, rbp; call)
//     Main::Render  id 36559  rva 0x656e60
//   id 36558 itself has NO direct call or jmp anywhere in .text (also
//   checked) - it is reached indirectly (a function pointer or task
//   object, consistent with main-thread.md's note that Main defers work
//   through its own packed task queue). that means the brief's "walk back
//   from Main::Render's call site" bottoms out at id 36558 without a
//   further static caller to inspect, and id 36558's own body does not
//   look like Main::Update (no ai/havok/papyrus work, only camera-derived
//   state and two unrelated calls). Main::Update's rva is NOT resolved by
//   this pass; see the build report for what a further pass would need.
//
//   the call site above has exactly one caller (checked the same way), so
//   it is patched with call_patch.h like precip_gate.cpp/havok_step.cpp -
//   "few call sites, patch the call site". Main::Render as a whole is
//   timed by bracketing that one call; the time from one call's entry to
//   the next call's entry is the frame total (Main::Update plus render).
//
//   the other five functions the brief asks for all have MANY call sites
//   (checked the same way: a full e8/vtable scan) or none at all in .text,
//   so each gets a minimal entry (body) detour instead - "many call sites,
//   minimal entry detour" - the same technique occlusion_probe.cpp already
//   uses for Process1, reused here for the timer's own independent hook:
//
//     BSBatchRenderer::SetupAndDrawPass   id 107644  rva 0x1560340  13 e8 callers
//     BSRenderPass::ClearRenderPass       id 107498  rva 0x1555920  22 e8 callers
//     BSCullingProcess::Process(CullingContext&)
//                                         id 106921  rva 0x152b740  21 e8 callers
//     BSCullingProcess::Process1          id 76553   rva 0xfeda70   0 callers (vtable only)
//     BSCullingProcess::Process2          id 76554   rva 0xfedd70   0 callers (vtable only)
//
//   an entry detour composes safely with point_shadow_cull.cpp's call-site
//   patches on two of Process(CullingContext&)'s 21 callers: those
//   patches redirect a caller to a thunk that still calls through to the
//   real function, whose own first bytes we detour here, so every call -
//   patched caller or not - still passes through our timer.
//
//   two of the five detoured prologues (ClearRenderPass, Process2) open
//   with a conditional branch inside the minimum 5 bytes we must relocate:
//     ClearRenderPass  0x1555920  48 85 c9 74 62            test rcx,rcx; je +0x62 (short)
//     Process2         0xfedd70   48 85 d2 0f 84 c5 01 00 00 test rdx,rdx; je +0x1c5 (near)
//   both are "je" (condition 4). relocating a branch verbatim would jump
//   to the wrong place once the bytes move, so the relocated copy always
//   re-encodes it as a short "je" to a label a few bytes further into the
//   SAME trampoline, which then either falls through to fn+relocatedLen
//   (not taken) or absolute-jumps to the branch's real original target,
//   fn+jccTargetOffset (taken) - see WriteJccSafeHead. the other three
//   (SetupAndDrawPass, Process(CullingContext&), Process1) have no branch
//   or rip-relative operand in their relocated prefix, so they use the
//   plain copy-and-jump-back technique occlusion_probe.cpp already uses.
//
//   argument registers for each detoured function (so the c++ thunk's own
//   parameter list reproduces the real calling convention exactly, since
//   jmp-into-the-thunk preserves whatever registers the real caller set
//   up): SetupAndDrawPass(this, BSRenderPass*, u32, u32, u32 [5th on
//   stack]) per CommonLibSSE-NG's RE/B/BSBatchRenderer.h; ClearRenderPass
//   (this) per RE/B/BSRenderPass.h; Process(CullingContext&)(CullJob*, u32
//   walkMode, u32 accumMode) - confirmed against point_shadow_cull.cpp's
//   own RunCullJobFn, which already calls this same id with this same
//   signature; Process1(void* process, void* object, uintptr_t arg) -
//   reused verbatim from occlusion_probe.cpp's already-working hook;
//   Process2(void*, void*, void*, void*) - inferred from its prologue
//   (rcx/rdx/r8/r9 each moved whole into a callee-saved register), not
//   independently confirmed against a header - the timer only needs the
//   slot count and width, never the values, so a wrong field *name* here
//   cannot corrupt anything it forwards untouched.
//
//   "main vs job worker" thread classification does not use
//   JobListManager::ServingThread (id 403107, main-thread.md's suggestion
//   for the *research* stage-0 sample): Main::Render only ever runs on the
//   game's main thread by construction, so the timer just remembers
//   GetCurrentThreadId() the first time its own Main::Render hook fires
//   and compares every sub-hook's calling thread against that - simpler,
//   and needs no engine layout at all.
//
// per-frame bucketing: SetupAndDrawPass/ClearRenderPass/Process(CullingContext&)/
// Process1/Process2 all accumulate into atomic counters that the
// Main::Render hook resets-and-reads (atomic exchange) once per call, right
// after the real Main::Render returns - so "this frame's" sub-totals are
// whatever those hooks added during that one call. a cull job dispatched to
// a job worker that is still finishing just past Main::Render's return would
// land in the next frame's bucket instead - a small, documented slop, not a
// correctness bug, and irrelevant to the aggregate stats this tool reports.
// frame_total needs two consecutive Main::Render entries, so it (and the
// "rest" bucket, frame_total minus render) are always reported one frame
// behind the render/draw/cull buckets measured during the call in between;
// PendingFrame carries that one frame's numbers forward so every bucket in
// one flushed sample lines up to the same frame.
//
// kFlushEvery frames of samples are kept per bucket (a plain array, sorted
// only at flush time) and reduced to mean/p50/p95 every 120 frames, written
// as one json line via WriteLine - a private file, distinct from
// skybench.log/SKYBENCH_EVENTS, since only this module's own file is asked
// for. everything here only ever runs from Main::Render's own hook or a
// hook it calls into, i.e. WriteLine's file access is always single
// threaded - no mutex needed, unlike skybench.cpp's shared logger.

#include "frame_timer.h"

#include "address_library.h"
#include "call_patch.h"
#include "skybench_log.h"

#include "skse64/PluginAPI.h"

#include <windows.h>
#include <intrin.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>

namespace {

// ---- ids / rvas (see the header comment) --------------------------------

const std::uint32_t kMainRenderCallerId = 36558;	// the wrapper that calls Main::Render
const std::uint32_t kMainRenderCallOffset = 0x11d;	// +rva of the call within it
const std::uint8_t kMainRenderCallPreamble[3] = { 0x48, 0x8b, 0xcd };	// mov rcx, rbp
const std::uint32_t kMainRenderId = 36559;

const std::uint32_t kSetupAndDrawPassId = 107644;
const std::uint8_t kSetupAndDrawPassHead[6] = { 0x48, 0x8b, 0xc4, 0x57, 0x41, 0x54 };	// mov rax,rsp; push rdi; push r12

const std::uint32_t kClearRenderPassId = 107498;
const std::uint8_t kClearRenderPassHead[5] = { 0x48, 0x85, 0xc9, 0x74, 0x62 };	// test rcx,rcx; je short +0x62
const std::size_t kClearRenderPassHeadLen = 3;
const std::size_t kClearRenderPassJccTarget = 0x67;	// fn + 5 + 0x62

const std::uint32_t kProcessCullId = 106921;
const std::uint8_t kProcessCullHead[12] = {
	0x48, 0x8b, 0xc4, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57
};	// mov rax,rsp; push rdi/r12/r13/r14/r15

const std::uint32_t kProcess1Id = 76553;
const std::uint8_t kProcess1Head[5] = { 0x48, 0x89, 0x5c, 0x24, 0x18 };	// mov [rsp+18h], rbx (occlusion_probe.cpp)

const std::uint32_t kProcess2Id = 76554;
const std::uint8_t kProcess2Head[9] = { 0x48, 0x85, 0xd2, 0x0f, 0x84, 0xc5, 0x01, 0x00, 0x00 };	// test rdx,rdx; je near +0x1c5
const std::size_t kProcess2HeadLen = 3;
const std::size_t kProcess2JccTarget = 0x1ce;	// fn + 9 + 0x1c5

const std::uint32_t kFlushEvery = 120;

// ---- small helpers (each detour file keeps its own copies - see occlusion_probe.cpp) ----

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

void WriteAbsJump(std::uint8_t * p, std::uintptr_t target)
{
	p[0] = 0xff;
	p[1] = 0x25;
	std::memset(p + 2, 0, 4);
	std::memcpy(p + 6, &target, 8);
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
	skybench_Log("frame_timer: " + reason);
	skybench_LogEvent("frame_timer", ", \"status\": \"skipped\", \"reason\": \"" + reason + "\"" + extra);
}

// result of one entry-detour install: original() is where to call through
// to run the rest of the real function (relocated head + jump back).
struct DetourResult
{
	bool ok = false;
	std::string reason;
	void * original = nullptr;
};

// plain copy-and-jump-back: headLen bytes of the real prologue (already
// verified against expected, no branch or rip-relative operand in them)
// relocated verbatim, then an absolute jump back to fn + headLen.
DetourResult InstallSimpleDetour(std::uintptr_t base, std::uint32_t id, const AddressLibrary & lib,
	const std::uint8_t * expected, std::size_t headLen, void * thunk,
	SKSETrampolineInterface * tramp, std::uint32_t pluginHandle, const char * label)
{
	DetourResult r;
	const std::uint32_t fnRva = lib.Rva(id);
	if (!fnRva)
	{
		r.reason = std::string(label) + ": id not in address library";
		return r;
	}
	const std::uintptr_t fn = base + fnRva;
	const std::string where = ", \"fn_rva\": " + JsonHex(fnRva);
	if (!IsExeCode(base, fn, headLen))
	{
		r.reason = std::string(label) + ": not exe code" + where;
		return r;
	}
	if (std::memcmp(reinterpret_cast<const void *>(fn), expected, headLen) != 0)
	{
		r.reason = std::string(label) + ": unexpected bytes (another hook got here first?)" + where +
			", \"bytes\": " + HexBytes(reinterpret_cast<const std::uint8_t *>(fn), headLen);
		return r;
	}
	if (!tramp || !tramp->AllocateFromBranchPool)
	{
		r.reason = std::string(label) + ": no skse trampoline interface";
		return r;
	}

	auto * stub = static_cast<std::uint8_t *>(tramp->AllocateFromBranchPool(pluginHandle, 14 + headLen + 14));
	if (!stub)
	{
		r.reason = std::string(label) + ": branch pool allocation failed" + where;
		return r;
	}
	WriteAbsJump(stub, reinterpret_cast<std::uintptr_t>(thunk));
	std::uint8_t * originalCopy = stub + 14;
	std::memcpy(originalCopy, expected, headLen);
	WriteAbsJump(originalCopy + headLen, fn + headLen);
	FlushInstructionCache(GetCurrentProcess(), stub, 14 + headLen + 14);

	const std::intptr_t rel = reinterpret_cast<std::intptr_t>(stub) - static_cast<std::intptr_t>(fn + 5);
	if (rel < INT32_MIN || rel > INT32_MAX)
	{
		r.reason = std::string(label) + ": branch pool stub out of rel32 range" + where;
		return r;
	}

	DWORD oldProtect = 0;
	if (!VirtualProtect(reinterpret_cast<void *>(fn), 5, PAGE_EXECUTE_READWRITE, &oldProtect))
	{
		r.reason = std::string(label) + ": VirtualProtect failed" + where;
		return r;
	}
	std::uint8_t jmp[5] = { 0xe9 };
	const std::int32_t rel32 = static_cast<std::int32_t>(rel);
	std::memcpy(jmp + 1, &rel32, 4);
	std::memcpy(reinterpret_cast<void *>(fn), jmp, sizeof(jmp));
	VirtualProtect(reinterpret_cast<void *>(fn), 5, oldProtect, &oldProtect);
	FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void *>(fn), 5);

	r.ok = true;
	r.original = originalCopy;
	skybench_LogEvent("frame_timer_hook",
		", \"fn\": \"" + std::string(label) + "\", \"status\": \"patched\", \"technique\": \"entry_detour\"" + where);
	return r;
}

// same idea, but the relocated prefix's last instruction is a "je" (short
// or near in the original - always re-encoded short here, see the header
// comment) that must be retargeted since it moved: headLen bytes copied
// verbatim, then a short je to a label a few bytes later in the same
// stub, which either falls through to fn + expectedLen (not taken) or
// jumps to fn + jccTarget (taken, the branch's real original destination -
// untouched original code, since we never write past fn's first 5 bytes).
DetourResult InstallJccDetour(std::uintptr_t base, std::uint32_t id, const AddressLibrary & lib,
	const std::uint8_t * expected, std::size_t expectedLen, std::size_t headLen, std::size_t jccTarget,
	void * thunk, SKSETrampolineInterface * tramp, std::uint32_t pluginHandle, const char * label)
{
	DetourResult r;
	const std::uint32_t fnRva = lib.Rva(id);
	if (!fnRva)
	{
		r.reason = std::string(label) + ": id not in address library";
		return r;
	}
	const std::uintptr_t fn = base + fnRva;
	const std::string where = ", \"fn_rva\": " + JsonHex(fnRva);
	if (!IsExeCode(base, fn, expectedLen))
	{
		r.reason = std::string(label) + ": not exe code" + where;
		return r;
	}
	if (std::memcmp(reinterpret_cast<const void *>(fn), expected, expectedLen) != 0)
	{
		r.reason = std::string(label) + ": unexpected bytes (another hook got here first?)" + where +
			", \"bytes\": " + HexBytes(reinterpret_cast<const std::uint8_t *>(fn), expectedLen);
		return r;
	}
	if (!tramp || !tramp->AllocateFromBranchPool)
	{
		r.reason = std::string(label) + ": no skse trampoline interface";
		return r;
	}

	const std::size_t offHead = 14;
	const std::size_t offJcc = offHead + headLen;
	const std::size_t offNotTaken = offJcc + 2;
	const std::size_t offTaken = offNotTaken + 14;
	const std::size_t total = offTaken + 14;

	auto * stub = static_cast<std::uint8_t *>(tramp->AllocateFromBranchPool(pluginHandle, total));
	if (!stub)
	{
		r.reason = std::string(label) + ": branch pool allocation failed" + where;
		return r;
	}
	WriteAbsJump(stub, reinterpret_cast<std::uintptr_t>(thunk));
	std::memcpy(stub + offHead, expected, headLen);

	const std::intptr_t takenRel = reinterpret_cast<std::intptr_t>(stub + offTaken) -
		(reinterpret_cast<std::intptr_t>(stub + offJcc) + 2);
	if (takenRel < INT8_MIN || takenRel > INT8_MAX)
	{
		r.reason = std::string(label) + ": jcc relocation out of rel8 range (layout bug)" + where;
		return r;
	}
	stub[offJcc] = 0x74;	// je rel8 (same condition as both known original forms - see header comment)
	stub[offJcc + 1] = static_cast<std::uint8_t>(static_cast<std::int8_t>(takenRel));
	WriteAbsJump(stub + offNotTaken, fn + expectedLen);
	WriteAbsJump(stub + offTaken, fn + jccTarget);
	FlushInstructionCache(GetCurrentProcess(), stub, static_cast<SIZE_T>(total));

	const std::intptr_t rel = reinterpret_cast<std::intptr_t>(stub) - static_cast<std::intptr_t>(fn + 5);
	if (rel < INT32_MIN || rel > INT32_MAX)
	{
		r.reason = std::string(label) + ": branch pool stub out of rel32 range" + where;
		return r;
	}

	DWORD oldProtect = 0;
	if (!VirtualProtect(reinterpret_cast<void *>(fn), 5, PAGE_EXECUTE_READWRITE, &oldProtect))
	{
		r.reason = std::string(label) + ": VirtualProtect failed" + where;
		return r;
	}
	std::uint8_t jmp[5] = { 0xe9 };
	const std::int32_t rel32 = static_cast<std::int32_t>(rel);
	std::memcpy(jmp + 1, &rel32, 4);
	std::memcpy(reinterpret_cast<void *>(fn), jmp, sizeof(jmp));
	VirtualProtect(reinterpret_cast<void *>(fn), 5, oldProtect, &oldProtect);
	FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void *>(fn), 5);

	r.ok = true;
	r.original = stub + offHead;
	skybench_LogEvent("frame_timer_hook",
		", \"fn\": \"" + std::string(label) + "\", \"status\": \"patched\", \"technique\": \"entry_detour_jcc\"" + where);
	return r;
}

// ---- timing -------------------------------------------------------------

double g_cyclesPerMs = 0.0;
double g_pairCostNs = 0.0;

inline double CyclesToMs(std::uint64_t cycles)
{
	return g_cyclesPerMs > 0.0 ? static_cast<double>(cycles) / g_cyclesPerMs : 0.0;
}

void Calibrate()
{
	LARGE_INTEGER freq = {}, t0 = {}, t1 = {};
	QueryPerformanceFrequency(&freq);
	QueryPerformanceCounter(&t0);
	const unsigned __int64 c0 = __rdtsc();
	Sleep(5);
	QueryPerformanceCounter(&t1);
	const unsigned __int64 c1 = __rdtsc();
	const double seconds = freq.QuadPart > 0 ? static_cast<double>(t1.QuadPart - t0.QuadPart) / static_cast<double>(freq.QuadPart) : 0.0;
	g_cyclesPerMs = seconds > 0.0 ? static_cast<double>(c1 - c0) / (seconds * 1000.0) : 3000000.0;	// ~3 GHz fallback

	// the cost of one rdtsc-bracket pair itself (two rdtsc reads, back to
	// back): the minimum of many samples, converted with the calibration
	// above. this is what every sub-hook and the render hook pay per call.
	unsigned __int64 best = ~0ull;
	for (int i = 0; i < 20000; i++)
	{
		const unsigned __int64 a = __rdtsc();
		const unsigned __int64 b = __rdtsc();
		const unsigned __int64 d = b - a;
		if (d < best)
			best = d;
	}
	g_pairCostNs = CyclesToMs(best) * 1e6;
}

// ---- per-bucket rolling window (120 frames, mean/p50/p95 at flush) ------

enum Bucket
{
	kFrameTotal, kRender, kRest,
	kDrawSumMs, kDrawCount,
	kCullMainMs, kCullJobMs, kCullMainCount, kCullJobCount,
	kP1MainMs, kP1JobMs, kP1MainCount, kP1JobCount,
	kP2MainMs, kP2JobMs, kP2MainCount, kP2JobCount,
	kClearCount,
	kBucketCount
};
const char * const kBucketNames[kBucketCount] = {
	"frame_total_ms", "render_ms", "rest_ms",
	"draw_pass_sum_ms", "draw_pass_count",
	"cull_entry_main_ms", "cull_entry_job_ms", "cull_entry_main_count", "cull_entry_job_count",
	"process1_main_ms", "process1_job_ms", "process1_main_count", "process1_job_count",
	"process2_main_ms", "process2_job_ms", "process2_main_count", "process2_job_count",
	"clear_render_pass_count"
};

struct Series
{
	double buf[kFlushEvery];
	std::uint32_t n = 0;

	void Push(double v)
	{
		if (n < kFlushEvery)
			buf[n++] = v;
	}
	void Stats(double & mean, double & p50, double & p95) const
	{
		if (n == 0)
		{
			mean = p50 = p95 = 0.0;
			return;
		}
		double sorted[kFlushEvery];
		std::memcpy(sorted, buf, sizeof(double) * n);
		std::sort(sorted, sorted + n);
		double sum = 0.0;
		for (std::uint32_t i = 0; i < n; i++)
			sum += sorted[i];
		mean = sum / n;
		p50 = sorted[(n * 50) / 100 < n ? (n * 50) / 100 : n - 1];
		p95 = sorted[(n * 95) / 100 < n ? (n * 95) / 100 : n - 1];
	}
	void Reset() { n = 0; }
};

Series g_series[kBucketCount];
std::uint32_t g_windowFrames = 0;
std::uint64_t g_pairsWindowSum = 0;
std::string g_outPath;

std::string ResolveOutputPath()
{
	std::string base;
	char * dir = nullptr;
	std::size_t len = 0;
	if (_dupenv_s(&dir, &len, "SKYBENCH_PROBE_DIR") == 0 && dir)
	{
		base = dir;
		free(dir);
	}
	else
	{
		char * userProfile = nullptr;
		std::size_t ulen = 0;
		if (_dupenv_s(&userProfile, &ulen, "USERPROFILE") == 0 && userProfile)
		{
			base = std::string(userProfile) + "\\Documents\\My Games\\Skyrim Special Edition\\SKSE";
			free(userProfile);
		}
	}
	if (base.empty())
		return std::string();
	CreateDirectoryA(base.c_str(), nullptr);
	return base + "\\frame_timer.jsonl";
}

void WriteLine(const std::string & line)
{
	if (g_outPath.empty())
		return;
	std::ofstream f(g_outPath, std::ios::app);
	if (!f.is_open())
		return;
	f << line << "\n";
}

void FlushWindow()
{
	char buf[256];
	std::string line = "{\"frames\": ";
	sprintf_s(buf, "%u", g_windowFrames);
	line += buf;

	for (int i = 0; i < kBucketCount; i++)
	{
		double mean = 0.0, p50 = 0.0, p95 = 0.0;
		g_series[i].Stats(mean, p50, p95);
		sprintf_s(buf, ", \"%s_mean\": %.4f, \"%s_p50\": %.4f, \"%s_p95\": %.4f",
			kBucketNames[i], mean, kBucketNames[i], p50, kBucketNames[i], p95);
		line += buf;
		g_series[i].Reset();
	}

	const double meanPairs = g_windowFrames ? static_cast<double>(g_pairsWindowSum) / g_windowFrames : 0.0;
	const double overheadMs = meanPairs * g_pairCostNs / 1e6;
	sprintf_s(buf, ", \"timer_pair_cost_ns\": %.2f, \"timer_pairs_per_frame_mean\": %.2f, \"timer_overhead_ms_per_frame_est\": %.5f}",
		g_pairCostNs, meanPairs, overheadMs);
	line += buf;

	WriteLine(line);
	g_windowFrames = 0;
	g_pairsWindowSum = 0;
}

struct PendingFrame
{
	double renderMs = 0.0, drawSumMs = 0.0;
	std::uint64_t drawCount = 0;
	double cullMainMs = 0.0, cullJobMs = 0.0;
	std::uint64_t cullMainCount = 0, cullJobCount = 0;
	double p1MainMs = 0.0, p1JobMs = 0.0;
	std::uint64_t p1MainCount = 0, p1JobCount = 0;
	double p2MainMs = 0.0, p2JobMs = 0.0;
	std::uint64_t p2MainCount = 0, p2JobCount = 0;
	std::uint64_t clearCount = 0;
};
PendingFrame g_pending;
bool g_havePending = false;

void PushFrame(double frameTotalMs, const PendingFrame & p)
{
	const double restMs = frameTotalMs - p.renderMs;
	g_series[kFrameTotal].Push(frameTotalMs);
	g_series[kRender].Push(p.renderMs);
	g_series[kRest].Push(restMs);
	g_series[kDrawSumMs].Push(p.drawSumMs);
	g_series[kDrawCount].Push(static_cast<double>(p.drawCount));
	g_series[kCullMainMs].Push(p.cullMainMs);
	g_series[kCullJobMs].Push(p.cullJobMs);
	g_series[kCullMainCount].Push(static_cast<double>(p.cullMainCount));
	g_series[kCullJobCount].Push(static_cast<double>(p.cullJobCount));
	g_series[kP1MainMs].Push(p.p1MainMs);
	g_series[kP1JobMs].Push(p.p1JobMs);
	g_series[kP1MainCount].Push(static_cast<double>(p.p1MainCount));
	g_series[kP1JobCount].Push(static_cast<double>(p.p1JobCount));
	g_series[kP2MainMs].Push(p.p2MainMs);
	g_series[kP2JobMs].Push(p.p2JobMs);
	g_series[kP2MainCount].Push(static_cast<double>(p.p2MainCount));
	g_series[kP2JobCount].Push(static_cast<double>(p.p2JobCount));
	g_series[kClearCount].Push(static_cast<double>(p.clearCount));

	g_pairsWindowSum += 1 + p.drawCount + p.cullMainCount + p.cullJobCount +
		p.p1MainCount + p.p1JobCount + p.p2MainCount + p.p2JobCount;
	g_windowFrames++;
	if (g_windowFrames >= kFlushEvery)
		FlushWindow();
}

// ---- thread classification -----------------------------------------------

std::atomic<std::uint32_t> g_mainThreadId{ 0 };

inline bool OnMainThread()
{
	return GetCurrentThreadId() == g_mainThreadId.load(std::memory_order_relaxed);
}

// ---- per-frame accumulators (written by the sub-hooks, harvested once per
// Main::Render call by RenderThunk via exchange) ---------------------------

std::atomic<std::uint64_t> g_drawCycles{ 0 }, g_drawCount{ 0 };
std::atomic<std::uint64_t> g_clearCount{ 0 };
std::atomic<std::uint64_t> g_cullMainCycles{ 0 }, g_cullJobCycles{ 0 }, g_cullMainCount{ 0 }, g_cullJobCount{ 0 };
std::atomic<std::uint64_t> g_p1MainCycles{ 0 }, g_p1JobCycles{ 0 }, g_p1MainCount{ 0 }, g_p1JobCount{ 0 };
std::atomic<std::uint64_t> g_p2MainCycles{ 0 }, g_p2JobCycles{ 0 }, g_p2MainCount{ 0 }, g_p2JobCount{ 0 };

// ---- sub-hooks ------------------------------------------------------------

using SetupAndDrawPassFn = void (*)(void *, void *, std::uint32_t, std::uint32_t, std::uint32_t);
SetupAndDrawPassFn g_origDraw = nullptr;
void DrawThunk(void * thiz, void * pass, std::uint32_t technique, std::uint32_t alphaTest, std::uint32_t renderFlags)
{
	const unsigned __int64 t0 = __rdtsc();
	g_origDraw(thiz, pass, technique, alphaTest, renderFlags);
	const unsigned __int64 t1 = __rdtsc();
	g_drawCycles.fetch_add(t1 - t0, std::memory_order_relaxed);
	g_drawCount.fetch_add(1, std::memory_order_relaxed);
}

using ClearRenderPassFn = void (*)(void *);
ClearRenderPassFn g_origClear = nullptr;
void ClearThunk(void * thiz)
{
	g_origClear(thiz);
	g_clearCount.fetch_add(1, std::memory_order_relaxed);
}

using ProcessCullFn = void (*)(void *, std::uint32_t, std::uint32_t);
ProcessCullFn g_origCull = nullptr;
void CullThunk(void * job, std::uint32_t walkMode, std::uint32_t accumMode)
{
	const unsigned __int64 t0 = __rdtsc();
	g_origCull(job, walkMode, accumMode);
	const unsigned __int64 t1 = __rdtsc();
	const std::uint64_t d = t1 - t0;
	if (OnMainThread())
	{
		g_cullMainCycles.fetch_add(d, std::memory_order_relaxed);
		g_cullMainCount.fetch_add(1, std::memory_order_relaxed);
	}
	else
	{
		g_cullJobCycles.fetch_add(d, std::memory_order_relaxed);
		g_cullJobCount.fetch_add(1, std::memory_order_relaxed);
	}
}

using Process1Fn = void (*)(void *, void *, std::uintptr_t);
Process1Fn g_origP1 = nullptr;
void P1Thunk(void * process, void * object, std::uintptr_t arg)
{
	const unsigned __int64 t0 = __rdtsc();
	g_origP1(process, object, arg);
	const unsigned __int64 t1 = __rdtsc();
	const std::uint64_t d = t1 - t0;
	if (OnMainThread())
	{
		g_p1MainCycles.fetch_add(d, std::memory_order_relaxed);
		g_p1MainCount.fetch_add(1, std::memory_order_relaxed);
	}
	else
	{
		g_p1JobCycles.fetch_add(d, std::memory_order_relaxed);
		g_p1JobCount.fetch_add(1, std::memory_order_relaxed);
	}
}

using Process2Fn = void (*)(void *, void *, void *, void *);
Process2Fn g_origP2 = nullptr;
void P2Thunk(void * process, void * object, void * a3, void * a4)
{
	const unsigned __int64 t0 = __rdtsc();
	g_origP2(process, object, a3, a4);
	const unsigned __int64 t1 = __rdtsc();
	const std::uint64_t d = t1 - t0;
	if (OnMainThread())
	{
		g_p2MainCycles.fetch_add(d, std::memory_order_relaxed);
		g_p2MainCount.fetch_add(1, std::memory_order_relaxed);
	}
	else
	{
		g_p2JobCycles.fetch_add(d, std::memory_order_relaxed);
		g_p2JobCount.fetch_add(1, std::memory_order_relaxed);
	}
}

// ---- Main::Render (the frame boundary) ------------------------------------

using MainRenderFn = void (*)(void *, std::uint32_t, std::uint32_t);
MainRenderFn g_origRender = nullptr;
std::uint64_t g_lastEntry = 0;
bool g_haveEntry = false;

void RenderThunk(void * main, std::uint32_t edxArg, std::uint32_t r8Arg)
{
	g_mainThreadId.store(GetCurrentThreadId(), std::memory_order_relaxed);

	const unsigned __int64 entry = __rdtsc();
	if (g_haveEntry && g_havePending)
		PushFrame(CyclesToMs(entry - g_lastEntry), g_pending);
	g_lastEntry = entry;
	g_haveEntry = true;

	const unsigned __int64 t0 = __rdtsc();
	g_origRender(main, edxArg, r8Arg);
	const unsigned __int64 t1 = __rdtsc();

	g_pending.renderMs = CyclesToMs(t1 - t0);
	g_pending.drawSumMs = CyclesToMs(g_drawCycles.exchange(0, std::memory_order_relaxed));
	g_pending.drawCount = g_drawCount.exchange(0, std::memory_order_relaxed);
	g_pending.clearCount = g_clearCount.exchange(0, std::memory_order_relaxed);
	g_pending.cullMainMs = CyclesToMs(g_cullMainCycles.exchange(0, std::memory_order_relaxed));
	g_pending.cullJobMs = CyclesToMs(g_cullJobCycles.exchange(0, std::memory_order_relaxed));
	g_pending.cullMainCount = g_cullMainCount.exchange(0, std::memory_order_relaxed);
	g_pending.cullJobCount = g_cullJobCount.exchange(0, std::memory_order_relaxed);
	g_pending.p1MainMs = CyclesToMs(g_p1MainCycles.exchange(0, std::memory_order_relaxed));
	g_pending.p1JobMs = CyclesToMs(g_p1JobCycles.exchange(0, std::memory_order_relaxed));
	g_pending.p1MainCount = g_p1MainCount.exchange(0, std::memory_order_relaxed);
	g_pending.p1JobCount = g_p1JobCount.exchange(0, std::memory_order_relaxed);
	g_pending.p2MainMs = CyclesToMs(g_p2MainCycles.exchange(0, std::memory_order_relaxed));
	g_pending.p2JobMs = CyclesToMs(g_p2JobCycles.exchange(0, std::memory_order_relaxed));
	g_pending.p2MainCount = g_p2MainCount.exchange(0, std::memory_order_relaxed);
	g_pending.p2JobCount = g_p2JobCount.exchange(0, std::memory_order_relaxed);
	g_havePending = true;
}

} // namespace

void InstallFrameTimer(const SKSEInterface * skse, unsigned long pluginHandle)
{
	char * val = nullptr;
	std::size_t len = 0;
	if (_dupenv_s(&val, &len, "SKYBENCH_FRAME_TIMER") != 0 || !val)
		return;
	const std::string mode(val);
	free(val);
	if (mode != "1")
		return;

	Calibrate();
	g_outPath = ResolveOutputPath();
	if (g_outPath.empty())
		skybench_Log("frame_timer: no output dir resolved (USERPROFILE unset?) - jsonl will not be written");

	AddressLibrary lib;
	if (!lib.Open(1, 7, 104))
	{
		Skip("address library: " + lib.error());
		return;
	}

	const std::uintptr_t base = reinterpret_cast<std::uintptr_t>(GetModuleHandleA(nullptr));
	auto * tramp = static_cast<SKSETrampolineInterface *>(skse->QueryInterface(kInterface_Trampoline));

	// Main::Render: required. one caller in .text, so a call-site patch
	// (call_patch.h), same technique as precip_gate.cpp/havok_step.cpp.
	const std::uint32_t callerRva = lib.Rva(kMainRenderCallerId);
	const std::uint32_t renderRva = lib.Rva(kMainRenderId);
	if (!callerRva || !renderRva)
	{
		Skip("Main::Render: id not in address library");
		return;
	}
	const CallSite renderSite = { "main_render", callerRva + kMainRenderCallOffset,
		kMainRenderCallPreamble, sizeof(kMainRenderCallPreamble), renderRva };
	const CheckedCall renderCall = CheckCall(renderSite);
	if (!renderCall.ok)
	{
		Skip("Main::Render: " + renderCall.reason, renderCall.fields);
		return;
	}
	g_origRender = reinterpret_cast<MainRenderFn>(renderCall.target);
	std::string reason;
	if (!PatchCall(renderCall, tramp, pluginHandle, reinterpret_cast<void *>(&RenderThunk), reason))
	{
		Skip("Main::Render: " + reason, renderCall.fields);
		return;
	}
	skybench_LogEvent("frame_timer_hook", ", \"fn\": \"main_render\", \"status\": \"patched\", \"technique\": \"call_site\"" +
		renderCall.fields);

	// the five sub-hooks: each optional, independent, logged and skipped
	// on its own if its bytes don't match (another hook, or a future
	// runtime, got there first) - the render timing above still works.
	const DetourResult draw = InstallSimpleDetour(base, kSetupAndDrawPassId, lib, kSetupAndDrawPassHead,
		sizeof(kSetupAndDrawPassHead), reinterpret_cast<void *>(&DrawThunk), tramp, pluginHandle, "setup_and_draw_pass");
	if (draw.ok)
		g_origDraw = reinterpret_cast<SetupAndDrawPassFn>(draw.original);
	else
		Skip(draw.reason);

	const DetourResult clear = InstallJccDetour(base, kClearRenderPassId, lib, kClearRenderPassHead,
		sizeof(kClearRenderPassHead), kClearRenderPassHeadLen, kClearRenderPassJccTarget,
		reinterpret_cast<void *>(&ClearThunk), tramp, pluginHandle, "clear_render_pass");
	if (clear.ok)
		g_origClear = reinterpret_cast<ClearRenderPassFn>(clear.original);
	else
		Skip(clear.reason);

	const DetourResult cull = InstallSimpleDetour(base, kProcessCullId, lib, kProcessCullHead,
		sizeof(kProcessCullHead), reinterpret_cast<void *>(&CullThunk), tramp, pluginHandle, "process_culling_context");
	if (cull.ok)
		g_origCull = reinterpret_cast<ProcessCullFn>(cull.original);
	else
		Skip(cull.reason);

	const DetourResult p1 = InstallSimpleDetour(base, kProcess1Id, lib, kProcess1Head,
		sizeof(kProcess1Head), reinterpret_cast<void *>(&P1Thunk), tramp, pluginHandle, "process1");
	if (p1.ok)
		g_origP1 = reinterpret_cast<Process1Fn>(p1.original);
	else
		Skip(p1.reason);

	const DetourResult p2 = InstallJccDetour(base, kProcess2Id, lib, kProcess2Head,
		sizeof(kProcess2Head), kProcess2HeadLen, kProcess2JccTarget,
		reinterpret_cast<void *>(&P2Thunk), tramp, pluginHandle, "process2");
	if (p2.ok)
		g_origP2 = reinterpret_cast<Process2Fn>(p2.original);
	else
		Skip(p2.reason);

	char buf[128];
	sprintf_s(buf, ", \"cycles_per_ms\": %.1f, \"pair_cost_ns\": %.2f, \"flush_every\": %u",
		g_cyclesPerMs, g_pairCostNs, kFlushEvery);
	skybench_Log("frame_timer: installed, writing to " + (g_outPath.empty() ? std::string("<nowhere>") : g_outPath));
	skybench_LogEvent("frame_timer", ", \"status\": \"installed\", \"out_path\": \"" + g_outPath + "\"" +
		std::string(", \"sub_hooks\": {\"draw\": ") + (draw.ok ? "true" : "false") +
		", \"clear\": " + (clear.ok ? "true" : "false") +
		", \"cull\": " + (cull.ok ? "true" : "false") +
		", \"process1\": " + (p1.ok ? "true" : "false") +
		", \"process2\": " + (p2.ok ? "true" : "false") + "}" + buf);
}
