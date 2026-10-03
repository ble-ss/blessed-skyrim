// precip_gate - renders the precipitation occlusion map only while rain or snow can fall (SKYBENCH_PRECIP_OCCLUSION).
//
// 1.7.104 facts (capstone over the exe and the address library bin):
//   Main::Render  id 36559  rva 0x656e60 (community shaders' Skylighting
//   hooks the same call: RelocationID(35560, 36559) + 0x3bf on 1.7.99+)
//   the call  +0x3bf  rva 0x65721f  b1 01 e8 51 28 ee 00 | e8 0c 34 ee 00
//   target    id 107144  rva 0x153a630, a jmp to id 26175 rva 0x40ea30,
//             Precipitation::RenderOcclusion. It takes no arguments.
//
// what RenderOcclusion does (flag = bUsePrecipitationOcclusion, copied to a
// byte in the Precipitation constructor, id 26177 at rva 0x40eda0):
//   sky = Sky::GetSingleton() (id 13878); precip = sky + 0xa0
//   ok  = type(sky->currentWeather) < 2 || type(sky->lastWeather) <= 1
//         where type(w) = w->precipitationData (+0x8c8) ->data[9]
//         (BSTArray at +0x20, kParticleType), 0 rain, 1 snow
//   if flag and ok and (precip->currentPrecip or lastPrecip) has an emitter:
//       set up and render the 512x512 mask (the two d16 passes, ps 489864ca)
// the mask keeps rendering every frame for as long as lastWeather had rain,
// long after the transition to a clear weather is over. The gate adds that
// one condition: the last weather only counts while the transition to the
// current one is still running (Sky::currentWeatherPct, +0x1b8, below 1).

#include "precip_gate.h"

#include "address_library.h"
#include "call_patch.h"
#include "skybench_log.h"

#include "skse64/PluginAPI.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

const std::uint32_t kMainRenderId = 36559;
const std::uint32_t kCallOffset = 0x3bf;
const std::uint32_t kRenderOcclusionThunkId = 107144;
const std::uint32_t kSkyGetSingletonId = 13878;

// mov cl, 1; call rel32 (the call just before ours).
const std::uint8_t kPreamble[7] = { 0xb1, 0x01, 0xe8, 0x51, 0x28, 0xee, 0x00 };

// offsets read by RenderOcclusion itself (see the header comment)
const std::size_t kSkyCurrentWeather = 0x48;
const std::size_t kSkyLastWeather = 0x50;
const std::size_t kSkyCurrentWeatherPct = 0x1b8;  // commonlibsse-ng RE/S/Sky.h
const std::size_t kWeatherPrecipData = 0x8c8;
const std::size_t kPrecipDataArray = 0x20;
const std::size_t kParticleTypeOffset = 0x24;      // data[9], kParticleType
const std::size_t kFormId = 0x14;

// frames the map keeps rendering after the gate closes (a fade tail)
const std::uint32_t kGraceFrames = 120;
const std::uint32_t kTickEvery = 600;

enum class Mode { kPassthrough, kWeather, kOff };

using VoidFn = void (*)();
using GetSkyFn = std::uint8_t * (*)();

Mode g_mode = Mode::kPassthrough;
VoidFn g_original = nullptr;
GetSkyFn g_getSky = nullptr;

std::uint32_t g_grace = 0;
std::uint32_t g_calls = 0;
std::uint32_t g_rendered = 0;
std::uint32_t g_skipped = 0;
int g_lastWant = -1;

const char * ModeName(Mode m)
{
	switch (m)
	{
	case Mode::kWeather: return "weather";
	case Mode::kOff: return "off";
	default: return "passthrough";
	}
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
	skybench_Log("precip_gate: patched nothing - " + reason);
	skybench_LogEvent("precip_gate", ", \"status\": \"skipped\", \"reason\": \"" + Escape(reason) + "\"" + extra);
}

template <class T>
T Read(const std::uint8_t * p, std::size_t off)
{
	T v;
	std::memcpy(&v, p + off, sizeof(T));
	return v;
}

// the weather's particle type as RenderOcclusion reads it, -1 without data
int PrecipType(const std::uint8_t * weather)
{
	if (!weather)
		return -1;
	auto * data = Read<const std::uint8_t *>(weather, kWeatherPrecipData);
	if (!data)
		return -1;
	auto * values = Read<const std::uint8_t *>(data, kPrecipDataArray);
	if (!values)
		return -1;
	return static_cast<int>(Read<std::uint32_t>(values, kParticleTypeOffset));
}

std::uint32_t FormId(const std::uint8_t * form)
{
	return form ? Read<std::uint32_t>(form, kFormId) : 0;
}

void Thunk()
{
	if (g_mode == Mode::kPassthrough)
	{
		g_original();
		return;
	}

	bool want = false;
	int curType = -1, lastType = -1;
	float pct = -1.0f;
	std::uint32_t curId = 0, lastId = 0;

	if (g_mode == Mode::kWeather)
	{
		const std::uint8_t * sky = g_getSky();
		if (!sky)
		{
			want = true;
		}
		else
		{
			auto * cur = Read<const std::uint8_t *>(sky, kSkyCurrentWeather);
			auto * last = Read<const std::uint8_t *>(sky, kSkyLastWeather);
			pct = Read<float>(sky, kSkyCurrentWeatherPct);
			curType = PrecipType(cur);
			lastType = PrecipType(last);
			curId = FormId(cur);
			lastId = FormId(last);

			const bool curWet = curType == 0 || curType == 1;
			const bool lastWet = lastType == 0 || lastType == 1;
			// a pct we cannot trust (not a fraction) keeps vanilla's rule
			const bool pctSane = pct >= 0.0f && pct <= 1.0f;
			want = curWet || (lastWet && (!pctSane || pct < 1.0f));
		}
	}

	if (want)
		g_grace = kGraceFrames;
	else if (g_grace)
	{
		g_grace--;
		want = true;
	}

	if (want)
	{
		g_rendered++;
		g_original();
	}
	else
	{
		g_skipped++;
	}

	char buf[256];
	const bool changed = static_cast<int>(want) != g_lastWant;
	if (changed || ++g_calls >= kTickEvery)
	{
		sprintf_s(buf, ", \"render\": %s, \"rendered\": %u, \"skipped\": %u, \"cur_weather\": \"%08X\", \"cur_type\": %d, "
			"\"last_weather\": \"%08X\", \"last_type\": %d, \"pct\": %.3f",
			want ? "true" : "false", g_rendered, g_skipped, curId, curType, lastId, lastType, pct);
		skybench_LogEvent(changed ? "precip_gate_change" : "precip_gate_tick", buf);
		g_lastWant = static_cast<int>(want);
		g_calls = 0;
		g_rendered = 0;
		g_skipped = 0;
	}
}

} // namespace

void InstallPrecipGate(const SKSEInterface * skse, unsigned long pluginHandle)
{
	char * val = nullptr;
	std::size_t len = 0;
	if (_dupenv_s(&val, &len, "SKYBENCH_PRECIP_OCCLUSION") != 0 || !val)
		return;
	std::string mode(val);
	free(val);
	if (mode == "weather" || mode == "1")
		g_mode = Mode::kWeather;
	else if (mode == "off")
		g_mode = Mode::kOff;
	else
		g_mode = Mode::kPassthrough;

	AddressLibrary lib;
	if (!lib.Open(1, 7, 104))
	{
		Skip("address library: " + lib.error());
		return;
	}

	const std::uint32_t mainRender = lib.Rva(kMainRenderId);
	const std::uint32_t occlusion = lib.Rva(kRenderOcclusionThunkId);
	const std::uint32_t getSky = lib.Rva(kSkyGetSingletonId);
	if (!mainRender || !occlusion || !getSky)
	{
		Skip("id not in address library");
		return;
	}

	const CallSite site = { "precip", mainRender + kCallOffset, kPreamble, sizeof(kPreamble), occlusion };
	const CheckedCall call = CheckCall(site);
	if (!call.ok)
	{
		Skip(call.reason, call.fields);
		return;
	}

	const std::uintptr_t base = reinterpret_cast<std::uintptr_t>(GetModuleHandleA(nullptr));
	g_original = reinterpret_cast<VoidFn>(call.target);
	g_getSky = reinterpret_cast<GetSkyFn>(base + getSky);

	auto * tramp = static_cast<SKSETrampolineInterface *>(skse->QueryInterface(kInterface_Trampoline));
	std::string reason;
	if (!PatchCall(call, tramp, pluginHandle, reinterpret_cast<void *>(&Thunk), reason))
	{
		Skip(reason, call.fields);
		return;
	}

	skybench_Log(std::string("precip_gate: hooked Precipitation::RenderOcclusion, mode ") + ModeName(g_mode));
	skybench_LogEvent("precip_gate",
		", \"status\": \"patched\", \"mode\": \"" + std::string(ModeName(g_mode)) + "\"" + call.fields +
		", \"target_rva\": " + JsonHex(occlusion) + ", \"sky_rva\": " + JsonHex(getSky));
}
