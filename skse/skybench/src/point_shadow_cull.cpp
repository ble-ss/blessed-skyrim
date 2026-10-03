// point_shadow_cull - call thunks on the cull job of spot and omni shadow maps; the job's cull mode becomes all-fail.
//
// every shadow light fills its maps the same way: it builds a cull job
// (CullJobDescriptor, commonlibsse-ng RE/B/BSCullingProcess.h) and hands it
// to one function, RunCullJob (id 106921), which walks the scene with a
// culling process and appends casters to the map's accumulator. the job
// carries a cull mode; BSCullingProcess::Process1 and the parabolic
// culler's override both check it first, and kAllFail (2) appends nothing
// and stops at the root. so the thunk sets that mode and calls through.
// the maps are still cleared and rendered (with no draws), the mask pass
// still runs, and the lit shader keeps its shadowed permutations.
//
// the sun's plane trick (cascade_cull.cpp) is not used here: the omni
// culler (BSParabolicCullingProcess, vtable id 255528) overrides the bound
// tests with a sphere and a hemisphere plane and zeroes activePlanes, so it
// never reads custom planes.
//
// 1.7.104 facts (found with capstone over the exe and the address library bin):
//   spot:  BSShadowFrustumLight::Accumulate (vfunc 9, id 108615) calls
//          BSShadowLight::AccumulateShadowmap  id 107602  rva 0x155cea0
//          the call  +0x1d0  rva 0x155d070  8b d3 48 8d 4d bf | e8 cb e6 fc ff
//          107602 is shared with the sun (BSShadowDirectionalLight::
//          Accumulate); the thunk passes the sun through (job +0x5d set).
//   omni:  BSShadowParabolicLight::Accumulate  id 108626  rva 0x15888d0
//          the call  +0x2fa  rva 0x1588bca  41 8b d6 48 8d 4d 97 | e8 71 2b fa ff
//          one job per light; dual paraboloid (2 maps) fills both
//          accumulators from the same job.
//   RunCullJob  id 106921  rva 0x152b740

#include "point_shadow_cull.h"

#include "address_library.h"
#include "call_patch.h"
#include "skybench_log.h"

#include "skse64/PluginAPI.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>

namespace {

const std::uint32_t kSpotAccumId = 107602;
const std::uint32_t kSpotCallOffset = 0x1d0;
const std::uint32_t kOmniAccumId = 108626;
const std::uint32_t kOmniCallOffset = 0x2fa;
const std::uint32_t kRunCullJobId = 106921;

// mov edx, ebx; lea rcx, [rbp - 0x41] (the job).
const std::uint8_t kSpotPreamble[6] = { 0x8b, 0xd3, 0x48, 0x8d, 0x4d, 0xbf };
// mov edx, r14d; lea rcx, [rbp - 0x69] (the job).
const std::uint8_t kOmniPreamble[7] = { 0x41, 0x8b, 0xd6, 0x48, 0x8d, 0x4d, 0x97 };

// BSCullingProcess::BSCPCullingType
const std::uint32_t kAllFail = 2;

// the fields of CullJobDescriptor (0x68 bytes) the thunk touches.
struct CullJob
{
	std::uint8_t pad00[0x50];
	std::uint32_t cullMode;          // 50, 3 or 4 from both accumulates
	float lightRadius;               // 54
	std::uint32_t unk58;             // 58
	bool isParabolic;                // 5C
	bool isDirectionalLight;         // 5D
	bool ignorePreprocess;           // 5E
	bool doCustomCullPlanes;         // 5F
	std::uint8_t pad60[8];
};
static_assert(offsetof(CullJob, cullMode) == 0x50, "CullJobDescriptor::cullMode");
static_assert(offsetof(CullJob, isParabolic) == 0x5c, "CullJobDescriptor::isParabolic");
static_assert(offsetof(CullJob, isDirectionalLight) == 0x5d, "CullJobDescriptor::isDirectionalLight");
static_assert(sizeof(CullJob) == 0x68, "CullJobDescriptor is 0x68 bytes");

// walkMode: 0 scene root, 1 the light's culling-object list, 2 a list
// entry's own culling process. accumMode: 0 finishes into the accumulator.
using RunCullJobFn = void (*)(CullJob * job, std::uint32_t walkMode, std::uint32_t accumMode);

RunCullJobFn g_original = nullptr;
bool g_cull = false;
std::atomic<std::uint64_t> g_spot{ 0 };
std::atomic<std::uint64_t> g_omni{ 0 };
std::atomic<std::uint64_t> g_sun{ 0 };

void Tick()
{
	const std::uint64_t spot = g_spot.load(std::memory_order_relaxed);
	const std::uint64_t omni = g_omni.load(std::memory_order_relaxed);
	const std::uint64_t n = spot + omni;
	if (n != 1 && n % 5000 != 0)
		return;
	char fields[160];
	sprintf_s(fields, ", \"spot\": %llu, \"omni\": %llu, \"sun_passed\": %llu, \"culled\": %s",
		static_cast<unsigned long long>(spot), static_cast<unsigned long long>(omni),
		static_cast<unsigned long long>(g_sun.load(std::memory_order_relaxed)), g_cull ? "true" : "false");
	skybench_LogEvent("point_shadow_cull_tick", fields);
}

void SpotThunk(CullJob * job, std::uint32_t walkMode, std::uint32_t accumMode)
{
	if (job->isDirectionalLight)
	{
		g_sun.fetch_add(1, std::memory_order_relaxed);
	}
	else
	{
		if (g_cull)
			job->cullMode = kAllFail;
		g_spot.fetch_add(1, std::memory_order_relaxed);
		Tick();
	}
	g_original(job, walkMode, accumMode);
}

void OmniThunk(CullJob * job, std::uint32_t walkMode, std::uint32_t accumMode)
{
	if (g_cull)
		job->cullMode = kAllFail;
	g_omni.fetch_add(1, std::memory_order_relaxed);
	Tick();
	g_original(job, walkMode, accumMode);
}

std::string Escape(const std::string & s)
{
	std::string out;
	for (char c : s)
	{
		if (c == '"' || c == '\\')
			out += '\\';
		out += c;
	}
	return out;
}

void Skip(const std::string & reason, const std::string & extra = std::string())
{
	skybench_Log("point_shadow_cull: patched nothing - " + reason);
	skybench_LogEvent("point_shadow_cull", ", \"status\": \"skipped\", \"reason\": \"" + Escape(reason) + "\"" + extra);
}

std::string Env(const char * name, bool & set)
{
	char * val = nullptr;
	std::size_t len = 0;
	set = (_dupenv_s(&val, &len, name) == 0 && val);
	std::string out = set ? std::string(val) : std::string();
	free(val);
	return out;
}

} // namespace

void InstallPointShadowCull(const SKSEInterface * skse, unsigned long pluginHandle)
{
	bool set = false;
	const std::string mode = Env("SKYBENCH_CULL_POINT_SHADOWS", set);
	if (!set)
		return;

	bool forkSet = false;
	const bool forkTraces = (Env("BLESSED_POINT_SHADOWS", forkSet) == "1");
	std::string why;
	if (mode == "force")
		g_cull = true;
	else if (mode == "1")
	{
		g_cull = forkTraces;
		if (!forkTraces)
			why = "BLESSED_POINT_SHADOWS is not 1, so nothing would write the point shadows; passing through";
	}

	AddressLibrary lib;
	if (!lib.Open(1, 7, 104))
	{
		Skip("address library: " + lib.error());
		return;
	}

	const std::uint32_t spotFn = lib.Rva(kSpotAccumId);
	const std::uint32_t omniFn = lib.Rva(kOmniAccumId);
	const std::uint32_t runJob = lib.Rva(kRunCullJobId);
	if (!spotFn || !omniFn || !runJob)
	{
		Skip("id not in address library");
		return;
	}

	const CallSite spotSite = { "spot", spotFn + kSpotCallOffset, kSpotPreamble, sizeof(kSpotPreamble), runJob };
	const CallSite omniSite = { "omni", omniFn + kOmniCallOffset, kOmniPreamble, sizeof(kOmniPreamble), runJob };

	// check both before touching either: both light types or neither.
	const CheckedCall spot = CheckCall(spotSite);
	const CheckedCall omni = CheckCall(omniSite);
	const std::string sites = ", \"spot\": {" + spot.fields.substr(2) + "}, \"omni\": {" + omni.fields.substr(2) + "}";
	if (!spot.ok)
	{
		Skip("spot site: " + spot.reason, sites);
		return;
	}
	if (!omni.ok)
	{
		Skip("omni site: " + omni.reason, sites);
		return;
	}

	auto * tramp = static_cast<SKSETrampolineInterface *>(skse->QueryInterface(kInterface_Trampoline));
	g_original = reinterpret_cast<RunCullJobFn>(spot.target);

	std::string reason;
	if (!PatchCall(spot, tramp, pluginHandle, reinterpret_cast<void *>(&SpotThunk), reason))
	{
		Skip("spot site: " + reason, sites);
		return;
	}
	if (!PatchCall(omni, tramp, pluginHandle, reinterpret_cast<void *>(&OmniThunk), reason))
	{
		// the spot hook stays in; say so, it is harmless either way.
		skybench_Log("point_shadow_cull: spot hooked, omni failed - " + reason);
		skybench_LogEvent("point_shadow_cull", ", \"status\": \"partial\", \"reason\": \"" + Escape(reason) + "\"" +
			sites + ", \"mode\": \"" + std::string(g_cull ? "cull" : "passthrough") + "\"");
		return;
	}

	const std::string modeName = g_cull ? "cull" : "passthrough";
	skybench_Log("point_shadow_cull: hooked the spot and omni cull jobs, mode " + modeName +
		(why.empty() ? std::string() : " (" + why + ")"));
	skybench_LogEvent("point_shadow_cull",
		", \"status\": \"patched\", \"mode\": \"" + modeName + "\", \"requested\": \"" + Escape(mode) +
		"\", \"blessed_point_shadows\": " + (forkTraces ? "true" : "false") + sites +
		", \"target_rva\": " + JsonHex(runJob) +
		(why.empty() ? std::string() : ", \"note\": \"" + Escape(why) + "\""));
}
