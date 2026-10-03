// cascade_census - times the sun's shadow-map cull and draw calls separately (see cascade_census.h).
//
// 1.7.104 facts (found with capstone over the exe and the address library bin):
//   Main::Render (id 36559, rva 0x656e60) calls both, directly, once per
//   frame, back to back:
//     the cull call   rva 0x657024  e8 (id 107132, rva 0x1537fd0)
//     the draw call   rva 0x65716a  e8 (id 107133, rva 0x1538430)
//   107132 (0x457 bytes): resets light-local state, then dispatches the
//   light's Accumulate through its vtable (vfunc 9 - confirmed by reading
//   the exe's own vtables: BSShadowDirectionalLight::Accumulate id 108488,
//   BSShadowFrustumLight::Accumulate id 108615, BSShadowParabolicLight::
//   Accumulate id 108626), which reaches BSShadowLight::AccumulateShadowmap
//   (id 107602, shared) and RunCullJob (id 106921). pure cpu: of 19 stage-0
//   samples inside 107132, 0 touch d3d11 or SetupAndDrawPass.
//   107133 (0x25c bytes): a short reset, then one indirect call at
//   +0x1ae (call qword ptr [r8 + 0x50], vfunc 0xA) dispatching the light's
//   RenderShadowmaps (BSShadowDirectionalLight::RenderShadowmaps id 108489,
//   BSShadowFrustumLight::RenderShadowmaps id 108616, BSShadowParabolicLight
//   ::RenderShadowmaps id 108627 - same vtable read), which reaches
//   BSBatchRenderer::SetupAndDrawPass (id 107644). 19 of 22 stage-0 samples
//   inside 107133 do touch d3d11/SetupAndDrawPass.
//   in the whiterun-bridge scene (no active point/spot shadow-casting
//   lights) both calls exclusively drive the sun's cascades.

#include "cascade_census.h"

#include "address_library.h"
#include "call_patch.h"
#include "skybench_log.h"

#include "skse64/PluginAPI.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <intrin.h>
#include <string>

namespace {

const std::uint32_t kCullId = 107132;
const std::uint32_t kDrawId = 107133;
const std::uint32_t kMainRenderId = 36559;
const std::uint32_t kCullCallOffset = 0x1c4;   // Main::Render + 0x1c4
const std::uint32_t kDrawCallOffset = 0x30a;   // Main::Render + 0x30a

// mov rcx, rsi (the cull call's one argument).
const std::uint8_t kCullPreamble[3] = { 0x48, 0x8b, 0xce };
// the last 3 bytes of the preceding call's rel32 (a perf-marker end call,
// id 77503); an opaque but exact byte match for this build, same
// convention as every other call site in this plugin.
const std::uint8_t kDrawPreamble[3] = { 0x54, 0x9c, 0x00 };

// generic passthrough: whatever Main::Render put in rcx/rdx/r8/r9 flows
// through unread and unchanged, so this is correct regardless of the real
// arity (only rcx is live for the cull call; the draw call's own arity
// was not pinned down this session - see the report). the real return
// value (rax) flows back the same way.
using GenericFn = std::uintptr_t (*)(std::uintptr_t, std::uintptr_t, std::uintptr_t, std::uintptr_t);

GenericFn g_cullOriginal = nullptr;
GenericFn g_drawOriginal = nullptr;

std::atomic<std::uint64_t> g_cullCalls{ 0 };
std::atomic<std::uint64_t> g_cullCycles{ 0 };
std::atomic<std::uint64_t> g_drawCalls{ 0 };
std::atomic<std::uint64_t> g_drawCycles{ 0 };
std::atomic<std::uint64_t> g_gapCycles{ 0 };   // cull's return to draw's call, on the main thread
std::atomic<std::uint64_t> g_lastCullEnd{ 0 };

double g_tscPerSecond = 0.0;

void CalibrateTsc()
{
	LARGE_INTEGER f{};
	QueryPerformanceFrequency(&f);
	LARGE_INTEGER qpc0{};
	QueryPerformanceCounter(&qpc0);
	const std::uint64_t tsc0 = __rdtsc();
	// a short busy-wait: ~5ms is enough to calibrate tsc/s within <1%.
	LARGE_INTEGER qpc1{};
	do
	{
		QueryPerformanceCounter(&qpc1);
	} while ((qpc1.QuadPart - qpc0.QuadPart) * 1000 < f.QuadPart * 5);
	const std::uint64_t tsc1 = __rdtsc();
	const double seconds = static_cast<double>(qpc1.QuadPart - qpc0.QuadPart) / static_cast<double>(f.QuadPart);
	g_tscPerSecond = static_cast<double>(tsc1 - tsc0) / seconds;
}

double Ms(std::uint64_t cycles)
{
	if (g_tscPerSecond <= 0.0)
		return 0.0;
	return 1000.0 * static_cast<double>(cycles) / g_tscPerSecond;
}

void Tick()
{
	const std::uint64_t n = g_drawCalls.load(std::memory_order_relaxed);
	if (n != 1 && n % 3600 != 0)   // ~once at start, then about once a minute at 60 fps
		return;
	const std::uint64_t cullCalls = g_cullCalls.load(std::memory_order_relaxed);
	const std::uint64_t cullCycles = g_cullCycles.load(std::memory_order_relaxed);
	const std::uint64_t drawCycles = g_drawCycles.load(std::memory_order_relaxed);
	const std::uint64_t gapCycles = g_gapCycles.load(std::memory_order_relaxed);
	char fields[320];
	sprintf_s(fields,
		", \"cull_calls\": %llu, \"draw_calls\": %llu, "
		"\"cull_ms_per_call\": %.5f, \"draw_ms_per_call\": %.5f, \"gap_ms_per_call\": %.5f, "
		"\"cull_ms_total\": %.3f, \"draw_ms_total\": %.3f",
		static_cast<unsigned long long>(cullCalls), static_cast<unsigned long long>(n),
		cullCalls ? Ms(cullCycles) / static_cast<double>(cullCalls) : 0.0,
		n ? Ms(drawCycles) / static_cast<double>(n) : 0.0,
		n ? Ms(gapCycles) / static_cast<double>(n) : 0.0,
		Ms(cullCycles), Ms(drawCycles));
	skybench_LogEvent("cascade_census_tick", fields);
}

std::uintptr_t CullThunk(std::uintptr_t a, std::uintptr_t b, std::uintptr_t c, std::uintptr_t d)
{
	const std::uint64_t t0 = __rdtsc();
	const std::uintptr_t ret = g_cullOriginal(a, b, c, d);
	const std::uint64_t t1 = __rdtsc();
	g_cullCalls.fetch_add(1, std::memory_order_relaxed);
	g_cullCycles.fetch_add(t1 - t0, std::memory_order_relaxed);
	g_lastCullEnd.store(t1, std::memory_order_relaxed);
	return ret;
}

std::uintptr_t DrawThunk(std::uintptr_t a, std::uintptr_t b, std::uintptr_t c, std::uintptr_t d)
{
	const std::uint64_t t0 = __rdtsc();
	const std::uint64_t lastCullEnd = g_lastCullEnd.load(std::memory_order_relaxed);
	if (lastCullEnd && t0 > lastCullEnd)
		g_gapCycles.fetch_add(t0 - lastCullEnd, std::memory_order_relaxed);
	const std::uintptr_t ret = g_drawOriginal(a, b, c, d);
	const std::uint64_t t1 = __rdtsc();
	g_drawCalls.fetch_add(1, std::memory_order_relaxed);
	g_drawCycles.fetch_add(t1 - t0, std::memory_order_relaxed);
	Tick();
	return ret;
}

void Skip(const std::string & reason, const std::string & extra = std::string())
{
	skybench_Log("cascade_census: patched nothing - " + reason);
	skybench_LogEvent("cascade_census", ", \"status\": \"skipped\", \"reason\": \"" + reason + "\"" + extra);
}

} // namespace

void InstallCascadeCensus(const SKSEInterface * skse, unsigned long pluginHandle)
{
	char * val = nullptr;
	std::size_t len = 0;
	const bool set = (_dupenv_s(&val, &len, "SKYBENCH_CASCADE_CENSUS") == 0 && val);
	free(val);
	if (!set)
		return;

	AddressLibrary lib;
	if (!lib.Open(1, 7, 104))
	{
		Skip("address library: " + lib.error());
		return;
	}

	const std::uint32_t mainRenderRva = lib.Rva(kMainRenderId);
	const std::uint32_t cullFnRva = lib.Rva(kCullId);
	const std::uint32_t drawFnRva = lib.Rva(kDrawId);
	if (!mainRenderRva || !cullFnRva || !drawFnRva)
	{
		Skip("id not in address library");
		return;
	}

	const CallSite cullSite = { "cull", mainRenderRva + kCullCallOffset, kCullPreamble, sizeof(kCullPreamble), cullFnRva };
	const CallSite drawSite = { "draw", mainRenderRva + kDrawCallOffset, kDrawPreamble, sizeof(kDrawPreamble), drawFnRva };

	// check both before touching either: both stages or neither.
	const CheckedCall cull = CheckCall(cullSite);
	const CheckedCall draw = CheckCall(drawSite);
	const std::string sites = ", \"cull\": {" + cull.fields.substr(2) + "}, \"draw\": {" + draw.fields.substr(2) + "}";
	if (!cull.ok)
	{
		Skip("cull site: " + cull.reason, sites);
		return;
	}
	if (!draw.ok)
	{
		Skip("draw site: " + draw.reason, sites);
		return;
	}

	auto * tramp = static_cast<SKSETrampolineInterface *>(skse->QueryInterface(kInterface_Trampoline));
	g_cullOriginal = reinterpret_cast<GenericFn>(cull.target);
	g_drawOriginal = reinterpret_cast<GenericFn>(draw.target);

	std::string reason;
	if (!PatchCall(cull, tramp, pluginHandle, reinterpret_cast<void *>(&CullThunk), reason))
	{
		Skip("cull site: " + reason, sites);
		return;
	}
	if (!PatchCall(draw, tramp, pluginHandle, reinterpret_cast<void *>(&DrawThunk), reason))
	{
		skybench_Log("cascade_census: cull hooked, draw failed - " + reason);
		skybench_LogEvent("cascade_census", ", \"status\": \"partial\", \"reason\": \"" + reason + "\"" + sites);
		return;
	}

	CalibrateTsc();

	skybench_Log("cascade_census: hooked the sun's shadow cull and draw calls");
	skybench_LogEvent("cascade_census", ", \"status\": \"patched\"" + sites +
		", \"tsc_per_second\": " + std::to_string(g_tscPerSecond));
}
