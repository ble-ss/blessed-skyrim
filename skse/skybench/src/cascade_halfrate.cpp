// cascade_halfrate - renders the sun's far shadow cascade every other frame and reuses its depth in between (SKYBENCH_CASCADE_HALFRATE).
//
// how the engine renders and reads the far cascade (1.7.104, read from the
// exe with capstone; scratchpad/halfcascade/):
//   - once a frame, Main::Render's shadow cull (id 107132) calls the sun's
//     UpdateCamera (BSShadowDirectionalLight vfunc 0x10, id 108496, the
//     "SetFrameCamera" of cascade_cull.cpp) with the view camera, then its
//     Accumulate, whose cull jobs register casters through id 106567 and
//     the mode 14 handler table (see cascade_cull.cpp).
//   - the shadow draw (id 107133) calls Render (vfunc 0xa, id 108489). it
//     calls RenderShadowmap (id 107604) per cascade: at +0x6f in a loop that
//     only runs when a global mode is 2 (render target 3), and at +0xc6 in
//     the normal loop (render target 2, the 4096 d16 array, slice = index).
//   - RenderShadowmap clears the slice when descriptor + 0xe8
//     (clearRenderTarget) is set, renders the accumulator with the cascade
//     camera, and only then writes descriptor + 0x00, lightTransform: a 4x4
//     from absolute world space to (u, v, depth), built from the cascade
//     camera. nothing else writes it. the frustum it is built from is first
//     rescaled by render target size / port size, so the rendered box sits
//     at u [0, S], v [1 - S, 1], z [0, 1] in its units, S = 8 here (the
//     readers undo the scale). KeepFresh measures S.
//   - both readers take that matrix and fold in the current camera origin
//     themselves: the shadow mask setup (BSUtilityShader, id 107764) for
//     ShadowMapProj[], the volumetric generate setup (id 107379) for its
//     ShadowMapProj[0..1]. so a kept lightTransform is read correctly from
//     any later camera position.
//   - SetFrameCamera fits cascade 1's box tightly (light-space min/max of
//     the split frustum's corners at endSplit[0] and endSplit[1], snapped
//     outward to the texel grid). a reused box covers last frame's slice,
//     not necessarily this frame's.
//
// what a reused frame does:
//   1. UpdateCamera (our vtable wrapper) runs as usual, then decides. the
//      decision needs: last frame rendered cascade 1 (never two reuses in a
//      row, gap < _MAXGAPMS), the camera moved and turned under the limits,
//      the light direction (cascade camera rotate column 0) and the split
//      distances are unchanged, and this frame's split corners (the
//      startSplit[1], endSplit[0] and endSplit[1] quads) project inside the
//      kept box with the kept matrix (a corner that was outside the fresh
//      box may be out by as much as it was then, never more). the engine then culls as usual; our mode 14 handler returns
//      early for cascade 1's accumulator, so it queues no casters and the
//      receiver bit is still set (the proven SKYBENCH_CULL_CASCADES=1 path).
//   2. Render: RenderShadowmap for cascade 1 runs with clearRenderTarget
//      cleared (no clear, and no draws since nothing was queued), then we
//      restore clearRenderTarget and put back the kept lightTransform.
// a rendered frame keeps a copy of the lightTransform it just wrote, and
// checks our own corner model against it: the fresh box must contain the
// corners by construction. if that check fails on any of the first 16
// renders, the feature turns itself off and says so (a wrong model can only
// cost reuses, never correctness).
//
// env (read once):
//   SKYBENCH_CASCADE_HALFRATE=1           on
//   SKYBENCH_CASCADE_HALFRATE_MAXMOVE=50  camera move per frame, units
//   SKYBENCH_CASCADE_HALFRATE_MAXDEG=1    camera turn per frame, degrees
//   SKYBENCH_CASCADE_HALFRATE_SUNDEG=0    light direction change allowed; 0 = any change renders
//   SKYBENCH_CASCADE_HALFRATE_MAXGAPMS=250 the kept depth must be this recent
//   SKYBENCH_CASCADE_HALFRATE_EDGE_TEXELS=0.5 how far past the kept box a corner may land
//
// log: one skybench.log line and one "cascade_halfrate" event every ~5 s:
// frames rendered / reused / forced (by motion, coverage, sun, splits,
// other).
//
// conflicts: SKYBENCH_CULL_CASCADES=1 owns the same handler slot; whoever
// patches first wins and the other stays off (logged). the fork's
// BLESSED_CASCADE_CACHE also rewrites cascade layers: do not combine.

#include "cascade_halfrate.h"

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

// ---- 1.7.104 ids and offsets ----

const std::uint32_t kLightVtableId = 255466;		// BSShadowDirectionalLight vtable
const std::uint32_t kUpdateCameraId = 108496;		// its vfunc 0x10
const std::size_t kUpdateCameraSlot = 0x10;

const std::uint32_t kRenderId = 108489;				// its vfunc 0xa
const std::uint32_t kRenderShadowmapId = 107604;
const std::uint32_t kRenderCallOffsets[2] = { 0x6f, 0xc6 };
const std::uint8_t kRenderPreamble[3] = { 0x48, 0x8b, 0xcb };	// mov rcx, rbx

const std::uint32_t kRegisterId = 106567;
const std::uint32_t kMode14HandlerId = 106576;
const std::uint32_t kHandlerTableId = 414684;
const std::uint32_t kHandlerSlot14Id = 414698;
const std::uint32_t kShadowMapRenderMode = 14;
const std::uint32_t kDispatchOffset = 0x89;
const std::uint8_t kDispatch[10] = { 0x48, 0x63, 0x86, 0x50, 0x01, 0x00, 0x00, 0x4c, 0x8d, 0x15 };

// BSShadowDirectionalLight
const std::size_t kCascadeCount = 0x140;
const std::size_t kCascadeArray = 0x148;
const std::size_t kCascadeStride = 0xf0;
const std::size_t kStartSplit = 0x598;		// float[3]
const std::size_t kEndSplit = 0x5a4;		// float[3]
// ShadowmapDescriptor
const std::size_t kDescTransform = 0x00;	// float[16]
const std::size_t kDescCamera = 0x40;
const std::size_t kDescAccumulator = 0x48;
const std::size_t kDescClear = 0xe8;		// bool clearRenderTarget
// NiCamera
const std::size_t kCamRotate = 0x7c;		// NiMatrix3, row major
const std::size_t kCamTranslate = 0xa0;
const std::size_t kCamFrustum = 0x150;		// left, right, top, bottom, near, far
const std::size_t kCamOrtho = 0x168;

const std::uint32_t kFarCascade = 1;
const std::uint32_t kModelProbation = 16;
// 3 quads: startSplit[1] (where the mask starts blending cascade 1 in),
// endSplit[0] and endSplit[1] (the two the engine fits the box to).
const int kCorners = 12;
const int kFitCorner = 4;	// corners from here on are the engine's fit set

using UpdateCameraFn = bool (*)(void * light, void * viewCamera);
using RenderShadowmapFn = void (*)(void * light, void * desc, std::uint32_t * counter, std::uint32_t flags);
using RegisterHandlerFn = bool (*)(void * accumulator, void * geometry, void * property, void * a4);

// ---- small vector helpers ----

struct V3 { float x = 0, y = 0, z = 0; };
V3 Sub(V3 a, V3 b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
float Dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
float Len(V3 a) { return std::sqrt(Dot(a, a)); }
bool Finite(V3 a) { return std::isfinite(a.x) && std::isfinite(a.y) && std::isfinite(a.z); }

template <class T>
T Read(const void * base, std::size_t off)
{
	T v;
	std::memcpy(&v, static_cast<const std::uint8_t *>(base) + off, sizeof(T));
	return v;
}

V3 ReadV3(const void * base, std::size_t off)
{
	return { Read<float>(base, off), Read<float>(base, off + 4), Read<float>(base, off + 8) };
}

// column c of a row-major NiMatrix3 at base + off.
V3 Column(const void * base, std::size_t off, int c)
{
	return { Read<float>(base, off + 4 * c), Read<float>(base, off + 4 * (3 + c)), Read<float>(base, off + 4 * (6 + c)) };
}

// ---- config ----

struct Config
{
	float maxMove = 50.0f;
	float maxDeg = 1.0f;
	float sunDeg = 0.0f;
	std::uint64_t maxGapMs = 250;
	float edgeTexels = 0.5f;
};
Config g_cfg;

float EnvFloat(const char * name, float fallback)
{
	char * v = nullptr;
	std::size_t n = 0;
	float out = fallback;
	if (_dupenv_s(&v, &n, name) == 0 && v)
	{
		char * end = nullptr;
		const float f = std::strtof(v, &end);
		if (end != v && std::isfinite(f) && f >= 0.0f)
			out = f;
	}
	free(v);
	return out;
}

// ---- state (main thread, except g_skipAccumulator) ----

UpdateCameraFn g_updateOriginal = nullptr;
RenderShadowmapFn g_renderOriginal = nullptr;

// the handler slot, patched lazily from UpdateCamera (the engine fills the
// table during renderer init, after plugins load).
std::uintptr_t g_slot = 0;
std::uintptr_t g_vanillaHandler = 0;
RegisterHandlerFn g_forward = nullptr;
enum class SlotState { kWaiting, kPatched, kGaveUp };
SlotState g_slotState = SlotState::kWaiting;

// read by the cull job threads: the accumulator that queues nothing this frame.
std::atomic<void *> g_skipAccumulator{ nullptr };
// and until when (GetTickCount64): a pointer left over from a frame whose
// Render never came can never swallow a later light's casters.
std::atomic<std::uint64_t> g_skipUntil{ 0 };

// the frame being decided / rendered.
struct Frame
{
	std::uint64_t serial = 0;
	void * light = nullptr;
	void * desc = nullptr;		// cascade 1's descriptor
	bool reuse = false;
	bool counted = false;		// Render counted this frame already
	bool camOk = false;
	V3 camPos, camDir;
	V3 corners[kCorners];		// startSplit[1], endSplit[0], endSplit[1] quads
};
Frame g_frame;

// the depth now in cascade 1's layer and what it was rendered with.
struct Kept
{
	bool valid = false;
	bool modelOk = false;		// our corners sat inside this fresh box
	std::uint64_t serial = 0;
	std::uint64_t tick = 0;
	void * light = nullptr;
	void * desc = nullptr;
	float transform[16] = {};
	V3 camPos, camDir, lightDir;
	float start1 = 0, end0 = 0, end1 = 0;
	float uvw[kCorners][3] = {};	// our corners through the kept matrix
	float boxS = 0;				// the rendered box in kept-matrix units: u [0, S], v [1 - S, 1], z [0, 1]
};
Kept g_kept;

// our corner model's standing: off for good after a failure in probation.
std::uint32_t g_modelPasses = 0;
std::uint32_t g_modelFails = 0;
bool g_modelDead = false;

// counters, reset every log window.
struct Counts
{
	std::uint64_t rendered = 0, reused = 0;
	std::uint64_t cadence = 0, motion = 0, coverage = 0, sun = 0, split = 0, other = 0;
};
Counts g_win, g_total;
std::uint64_t g_winStart = 0;
bool g_reportedDead = false;

// ---- the math ----

// p * M (row vectors, translation in row 3), divided by w.
bool Project(const float * m, V3 p, float out[3])
{
	const float x = p.x * m[0] + p.y * m[4] + p.z * m[8] + m[12];
	const float y = p.x * m[1] + p.y * m[5] + p.z * m[9] + m[13];
	const float z = p.x * m[2] + p.y * m[6] + p.z * m[10] + m[14];
	const float w = p.x * m[3] + p.y * m[7] + p.z * m[11] + m[15];
	if (!std::isfinite(w) || std::fabs(w) < 1.0e-6f)
		return false;
	out[0] = x / w;
	out[1] = y / w;
	out[2] = z / w;
	return std::isfinite(out[0]) && std::isfinite(out[1]) && std::isfinite(out[2]);
}

// the split frustum's corners at planar view depth d: camera origin plus
// d * (dir + x * right + y * up), x in {left, right}, y in {bottom, top}
// (NiCamera: rotate column 0 = view dir, 1 = up, 2 = right; the frustum is
// in units of view depth).
bool BuildCorners(const void * viewCamera, float start1, float end0, float end1, Frame & f)
{
	if (Read<std::uint8_t>(viewCamera, kCamOrtho) != 0)
		return false;
	const V3 pos = ReadV3(viewCamera, kCamTranslate);
	const V3 dir = Column(viewCamera, kCamRotate, 0);
	const V3 up = Column(viewCamera, kCamRotate, 1);
	const V3 right = Column(viewCamera, kCamRotate, 2);
	const float l = Read<float>(viewCamera, kCamFrustum + 0x0);
	const float r = Read<float>(viewCamera, kCamFrustum + 0x4);
	const float t = Read<float>(viewCamera, kCamFrustum + 0x8);
	const float b = Read<float>(viewCamera, kCamFrustum + 0xc);
	if (!Finite(pos) || !Finite(dir) || !std::isfinite(l + r + t + b) || !(r > l) || !(t > b))
		return false;
	const float dl = Len(dir);
	if (!(dl > 0.5f && dl < 2.0f) || !(start1 > 0.0f) || !(end0 > 0.0f) || !(end1 > end0) || !std::isfinite(end1))
		return false;

	const float depth[3] = { start1, end0, end1 };
	const float xs[2] = { l, r };
	const float ys[2] = { b, t };
	int k = 0;
	for (float d : depth)
		for (float x : xs)
			for (float y : ys)
			{
				f.corners[k].x = pos.x + d * (dir.x + x * right.x + y * up.x);
				f.corners[k].y = pos.y + d * (dir.y + x * right.y + y * up.y);
				f.corners[k].z = pos.z + d * (dir.z + x * right.z + y * up.z);
				k++;
			}
	f.camPos = pos;
	f.camDir = { dir.x / dl, dir.y / dl, dir.z / dl };
	return true;
}

float AngleDeg(V3 a, V3 b)
{
	const float la = Len(a), lb = Len(b);
	if (!(la > 0.0f && lb > 0.0f))
		return 180.0f;
	float c = Dot(a, b) / (la * lb);
	c = c > 1.0f ? 1.0f : (c < -1.0f ? -1.0f : c);
	return std::acos(c) * (180.0f / 3.14159265f);
}

bool SameSplit(float a, float b)
{
	return std::fabs(a - b) <= 1.0e-4f * std::fabs(b) + 1.0e-3f;
}

// ---- the handler slot ----

bool HalfrateHandler(void * accumulator, void * geometry, void * property, void * a4)
{
	if (accumulator == g_skipAccumulator.load(std::memory_order_relaxed) &&
		GetTickCount64() <= g_skipUntil.load(std::memory_order_relaxed))
		return true;	// queue nothing; the caller still marks the receiver bit
	return g_forward(accumulator, geometry, property, a4);
}

void TryPatchSlot()
{
	auto * slot = reinterpret_cast<std::uintptr_t *>(g_slot);
	const std::uintptr_t current = *reinterpret_cast<volatile std::uintptr_t *>(slot);
	const std::uintptr_t ours = reinterpret_cast<std::uintptr_t>(&HalfrateHandler);
	if (current == ours || current == 0)
		return;
	if (current != g_vanillaHandler)
	{
		g_slotState = SlotState::kGaveUp;
		skybench_Log("cascade_halfrate: mode 14 handler slot holds something else (SKYBENCH_CULL_CASCADES?), staying off");
		skybench_LogEvent("cascade_halfrate_slot", ", \"status\": \"skipped\", \"found\": " + JsonHex(current) +
			", \"expected\": " + JsonHex(g_vanillaHandler));
		return;
	}
	MEMORY_BASIC_INFORMATION mbi = {};
	const DWORD writable = PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
	if (!VirtualQuery(slot, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT || !(mbi.Protect & writable))
	{
		g_slotState = SlotState::kGaveUp;
		skybench_LogEvent("cascade_halfrate_slot", ", \"status\": \"skipped\", \"reason\": \"slot is not writable\"");
		return;
	}
	g_forward = reinterpret_cast<RegisterHandlerFn>(current);
	InterlockedExchangePointer(reinterpret_cast<void * volatile *>(slot), reinterpret_cast<void *>(ours));
	g_slotState = SlotState::kPatched;
	skybench_Log("cascade_halfrate: mode 14 register handler wrapped");
	skybench_LogEvent("cascade_halfrate_slot", ", \"status\": \"patched\"");
}

// ---- the log window ----

void Add(Counts & c, std::uint64_t Counts::* field)
{
	c.*field += 1;
}

void Count(std::uint64_t Counts::* field)
{
	Add(g_win, field);
	Add(g_total, field);
}

void MaybeReport(std::uint64_t now)
{
	if (g_winStart == 0)
		g_winStart = now;
	if (g_modelDead && !g_reportedDead)
	{
		g_reportedDead = true;
		skybench_Log("cascade_halfrate: corner model failed its check, reuse is off for this session");
	}
	if (now - g_winStart < 5000)
		return;

	const double secs = (now - g_winStart) / 1000.0;
	const Counts & w = g_win;
	char line[400];
	sprintf_s(line, "cascade_halfrate: %.1f s: far cascade rendered %llu, reused %llu; forced by motion %llu, coverage %llu, sun %llu, splits %llu, other %llu%s",
		secs, static_cast<unsigned long long>(w.rendered), static_cast<unsigned long long>(w.reused),
		static_cast<unsigned long long>(w.motion), static_cast<unsigned long long>(w.coverage),
		static_cast<unsigned long long>(w.sun), static_cast<unsigned long long>(w.split),
		static_cast<unsigned long long>(w.other), g_modelDead ? " (off: corner model failed)" : "");
	skybench_Log(line);

	char fields[400];
	sprintf_s(fields, ", \"secs\": %.2f, \"rendered\": %llu, \"reused\": %llu, \"forced_motion\": %llu, \"forced_coverage\": %llu, \"forced_sun\": %llu, \"forced_split\": %llu, \"forced_other\": %llu, \"model_passes\": %u, \"model_fails\": %u, \"slot_patched\": %s",
		secs, static_cast<unsigned long long>(w.rendered), static_cast<unsigned long long>(w.reused),
		static_cast<unsigned long long>(w.motion), static_cast<unsigned long long>(w.coverage),
		static_cast<unsigned long long>(w.sun), static_cast<unsigned long long>(w.split),
		static_cast<unsigned long long>(w.other), g_modelPasses, g_modelFails,
		g_slotState == SlotState::kPatched ? "true" : "false");
	skybench_LogEvent("cascade_halfrate", fields);

	g_win = Counts();
	g_winStart = now;
}

// ---- the decision ----

enum class Why { kReuse, kCadence, kMotion, kCoverage, kSun, kSplit, kOther };

Why Decide(void * light, void * desc, const float split[3], V3 lightDir, bool cornersOk, std::uint64_t now)
{
	const Kept & k = g_kept;
	// cadence first: a frame after a reuse (or with no kept depth) renders.
	if (!k.valid || k.serial + 1 != g_frame.serial)
		return Why::kCadence;
	if (g_slotState != SlotState::kPatched || g_modelDead || !k.modelOk || !cornersOk ||
		k.light != light || k.desc != desc || now - k.tick > g_cfg.maxGapMs)
		return Why::kOther;

	if (Len(Sub(g_frame.camPos, k.camPos)) > g_cfg.maxMove || AngleDeg(g_frame.camDir, k.camDir) > g_cfg.maxDeg)
		return Why::kMotion;

	if (g_cfg.sunDeg > 0.0f ? AngleDeg(lightDir, k.lightDir) > g_cfg.sunDeg
							: (lightDir.x != k.lightDir.x || lightDir.y != k.lightDir.y || lightDir.z != k.lightDir.z))
		return Why::kSun;

	if (!SameSplit(split[0], k.start1) || !SameSplit(split[1], k.end0) || !SameSplit(split[2], k.end1))
		return Why::kSplit;

	// coverage: every corner of this frame's slice must land where the kept
	// box has depth, no further out than the same corner did on the frame
	// that rendered it (so we are never worse than a fresh render).
	// the box in kept-matrix units (see KeepFresh): u [0, S], v [1 - S, 1],
	// z [0, 1]. tolerance: _EDGE_TEXELS of the 4096 layer.
	const float boxLo[3] = { 0.0f, 1.0f - k.boxS, 0.0f };
	const float boxHi[3] = { k.boxS, 1.0f, 1.0f };
	const float tol[3] = { g_cfg.edgeTexels * k.boxS / 4096.0f, g_cfg.edgeTexels * k.boxS / 4096.0f, g_cfg.edgeTexels / 4096.0f };
	for (int i = 0; i < kCorners; i++)
	{
		float p[3];
		if (!Project(k.transform, g_frame.corners[i], p))
			return Why::kCoverage;
		for (int a = 0; a < 3; a++)
		{
			const float lo = (k.uvw[i][a] < boxLo[a] ? k.uvw[i][a] : boxLo[a]) - tol[a];
			const float hi = (k.uvw[i][a] > boxHi[a] ? k.uvw[i][a] : boxHi[a]) + tol[a];
			if (p[a] < lo || p[a] > hi)
				return Why::kCoverage;
		}
	}
	return Why::kReuse;
}

bool UpdateCameraHook(void * light, void * viewCamera)
{
	g_skipAccumulator.store(nullptr, std::memory_order_relaxed);
	const bool result = g_updateOriginal(light, viewCamera);

	if (g_slotState == SlotState::kWaiting)
		TryPatchSlot();

	const std::uint64_t now = GetTickCount64();
	g_frame = Frame();
	static std::uint64_t s_serial = 0;
	g_frame.serial = ++s_serial;
	g_frame.light = light;

	const std::uint32_t count = Read<std::uint32_t>(light, kCascadeCount);
	auto * cascades = Read<std::uint8_t *>(light, kCascadeArray);
	if (count > kFarCascade && cascades && viewCamera)
	{
		void * desc = cascades + kFarCascade * kCascadeStride;
		void * cam = Read<void *>(desc, kDescCamera);
		void * acc = Read<void *>(desc, kDescAccumulator);
		g_frame.desc = desc;

		const float split[3] = { Read<float>(light, kStartSplit + 4 * kFarCascade), Read<float>(light, kEndSplit),
			Read<float>(light, kEndSplit + 4 * kFarCascade) };
		const V3 lightDir = cam ? Column(cam, kCamRotate, 0) : V3();
		g_frame.camOk = BuildCorners(viewCamera, split[0], split[1], split[2], g_frame);

		const Why why = (cam && acc) ? Decide(light, desc, split, lightDir, g_frame.camOk, now) : Why::kOther;
		switch (why)
		{
		case Why::kReuse: break;
		case Why::kCadence: break;
		case Why::kMotion: Count(&Counts::motion); break;
		case Why::kCoverage: Count(&Counts::coverage); break;
		case Why::kSun: Count(&Counts::sun); break;
		case Why::kSplit: Count(&Counts::split); break;
		default: Count(&Counts::other); break;
		}
		if (why == Why::kReuse)
		{
			g_frame.reuse = true;
			g_skipUntil.store(now + 100, std::memory_order_relaxed);
			g_skipAccumulator.store(acc, std::memory_order_relaxed);
		}
	}

	MaybeReport(now);
	return result;
}

// ---- Render's calls to RenderShadowmap ----

void KeepFresh(void * light, void * desc)
{
	Kept & k = g_kept;
	std::memcpy(k.transform, static_cast<std::uint8_t *>(desc) + kDescTransform, sizeof(k.transform));
	k.serial = g_frame.serial;
	k.tick = GetTickCount64();
	k.light = light;
	k.desc = desc;
	k.camPos = g_frame.camPos;
	k.camDir = g_frame.camDir;
	void * cam = Read<void *>(desc, kDescCamera);
	k.lightDir = cam ? Column(cam, kCamRotate, 0) : V3();
	k.start1 = Read<float>(light, kStartSplit + 4 * kFarCascade);
	k.end0 = Read<float>(light, kEndSplit);
	k.end1 = Read<float>(light, kEndSplit + 4 * kFarCascade);
	k.valid = cam != nullptr;

	// where the rendered box sits in this matrix's units. RenderShadowmap
	// builds lightTransform from the cascade frustum rescaled by render
	// target size / port size (id 107604 + 0x1cf), keeping its left and
	// bottom edges: after the uv bias the box is u [0, S], v [1 - S, 1],
	// z [0, 1], S = port / target (8 on this machine, first in-game run:
	// the fit corners touched u 0 and 8, v -7 and 1). the readers undo the
	// scale themselves. S is measured here, not assumed: the box is square
	// (SetFrameCamera takes the larger extent), so the fit corners reach S
	// on one axis, to within the texel snap (conservative by at most that).
	bool projected = g_frame.camOk;
	for (int i = 0; projected && i < kCorners; i++)
		projected = Project(k.transform, g_frame.corners[i], k.uvw[i]);
	float minU = INFINITY, maxU = -INFINITY, minV = INFINITY, maxV = -INFINITY, minZ = INFINITY, maxZ = -INFINITY;
	for (int i = kFitCorner; projected && i < kCorners; i++)
	{
		minU = std::fmin(minU, k.uvw[i][0]);
		maxU = std::fmax(maxU, k.uvw[i][0]);
		minV = std::fmin(minV, k.uvw[i][1]);
		maxV = std::fmax(maxV, k.uvw[i][1]);
		minZ = std::fmin(minZ, k.uvw[i][2]);
		maxZ = std::fmax(maxZ, k.uvw[i][2]);
	}
	const float S = std::fmax(maxU, 1.0f - minV);
	k.boxS = S;
	// the convention check: the fit corners must touch the two pinned
	// edges (u = 0, v = 1) and stay inside the box on every axis.
	const float slack = 0.02f * S;
	bool inside = projected && std::isfinite(S) && S > 0.0f &&
		std::fabs(minU) <= slack && std::fabs(maxV - 1.0f) <= slack &&
		maxU <= S + slack && minV >= 1.0f - S - slack &&
		minZ >= -0.02f && maxZ <= 1.02f;
	k.modelOk = inside;

	const std::uint32_t checks = g_modelPasses + g_modelFails;
	if (inside)
		g_modelPasses++;
	else
		g_modelFails++;
	if (!inside && checks < kModelProbation)
		g_modelDead = true;

	// the first check, and every failed one up to 8, go to the events log.
	if (checks == 0 || (!inside && g_modelFails <= 8))
	{
		std::string f;
		char buf[192];
		sprintf_s(buf, ", \"inside\": %s, \"cam_ok\": %s, \"box_s\": %.4f, \"end0\": %.1f, \"end1\": %.1f, \"uvw\": [",
			inside ? "true" : "false", g_frame.camOk ? "true" : "false", k.boxS, k.end0, k.end1);
		f += buf;
		for (int i = 0; i < kCorners; i++)
		{
			sprintf_s(buf, "%s[%.4f, %.4f, %.4f]", i ? ", " : "", k.uvw[i][0], k.uvw[i][1], k.uvw[i][2]);
			f += buf;
		}
		f += "]";
		skybench_LogEvent("cascade_halfrate_model", f);
	}
}

void RenderShadowmapThunk(void * light, void * desc, std::uint32_t * counter, std::uint32_t flags)
{
	if (light != g_frame.light || desc != g_frame.desc || !desc)
	{
		g_renderOriginal(light, desc, counter, flags);
		return;
	}

	if (g_frame.reuse && g_kept.valid && g_kept.desc == desc)
	{
		auto * clear = static_cast<std::uint8_t *>(desc) + kDescClear;
		const std::uint8_t wasClear = *clear;
		*clear = 0;
		g_renderOriginal(light, desc, counter, flags);
		*clear = wasClear;
		// the cull is long done: nothing may skip past this point.
		g_skipAccumulator.store(nullptr, std::memory_order_relaxed);
		std::memcpy(static_cast<std::uint8_t *>(desc) + kDescTransform, g_kept.transform, sizeof(g_kept.transform));
		if (!g_frame.counted)
			Count(&Counts::reused);
	}
	else
	{
		g_renderOriginal(light, desc, counter, flags);
		KeepFresh(light, desc);
		if (!g_frame.counted)
			Count(&Counts::rendered);
	}
	g_frame.counted = true;
}

// ---- install ----

void Skip(const std::string & reason, const std::string & extra = std::string())
{
	skybench_Log("cascade_halfrate: patched nothing - " + reason);
	skybench_LogEvent("cascade_halfrate", ", \"status\": \"skipped\", \"reason\": \"" + reason + "\"" + extra);
}

bool CodeMatches(std::uintptr_t p, const std::uint8_t * bytes, std::size_t n)
{
	MEMORY_BASIC_INFORMATION mbi = {};
	if (!VirtualQuery(reinterpret_cast<void *>(p), &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT)
		return false;
	if (p + n > reinterpret_cast<std::uintptr_t>(mbi.BaseAddress) + mbi.RegionSize)
		return false;
	return std::memcmp(reinterpret_cast<const void *>(p), bytes, n) == 0;
}

} // namespace

void InstallCascadeHalfrate(const SKSEInterface * skse, unsigned long pluginHandle)
{
	char * val = nullptr;
	std::size_t len = 0;
	const bool on = (_dupenv_s(&val, &len, "SKYBENCH_CASCADE_HALFRATE") == 0 && val && std::string(val) == "1");
	free(val);
	if (!on)
		return;

	g_cfg.maxMove = EnvFloat("SKYBENCH_CASCADE_HALFRATE_MAXMOVE", g_cfg.maxMove);
	g_cfg.maxDeg = EnvFloat("SKYBENCH_CASCADE_HALFRATE_MAXDEG", g_cfg.maxDeg);
	g_cfg.sunDeg = EnvFloat("SKYBENCH_CASCADE_HALFRATE_SUNDEG", g_cfg.sunDeg);
	g_cfg.maxGapMs = static_cast<std::uint64_t>(EnvFloat("SKYBENCH_CASCADE_HALFRATE_MAXGAPMS", static_cast<float>(g_cfg.maxGapMs)));
	g_cfg.edgeTexels = EnvFloat("SKYBENCH_CASCADE_HALFRATE_EDGE_TEXELS", g_cfg.edgeTexels);

	AddressLibrary lib;
	if (!lib.Open(1, 7, 104))
	{
		Skip("address library: " + lib.error());
		return;
	}
	const std::uint32_t vtableRva = lib.Rva(kLightVtableId);
	const std::uint32_t updateRva = lib.Rva(kUpdateCameraId);
	const std::uint32_t renderRva = lib.Rva(kRenderId);
	const std::uint32_t renderShadowmapRva = lib.Rva(kRenderShadowmapId);
	const std::uint32_t registerRva = lib.Rva(kRegisterId);
	const std::uint32_t handlerRva = lib.Rva(kMode14HandlerId);
	const std::uint32_t tableRva = lib.Rva(kHandlerTableId);
	const std::uint32_t slotRva = lib.Rva(kHandlerSlot14Id);
	if (!vtableRva || !updateRva || !renderRva || !renderShadowmapRva || !registerRva || !handlerRva || !tableRva || !slotRva)
	{
		Skip("id not in address library");
		return;
	}
	if (slotRva != tableRva + 8 * kShadowMapRenderMode)
	{
		Skip("handler slot id is not table + 14 * 8");
		return;
	}

	const std::uintptr_t base = reinterpret_cast<std::uintptr_t>(GetModuleHandleA(nullptr));

	// the register function dispatches through the table on accumulator + 0x150.
	const std::uintptr_t dispatch = base + registerRva + kDispatchOffset;
	if (!CodeMatches(dispatch, kDispatch, sizeof(kDispatch)))
	{
		Skip("register dispatch not found");
		return;
	}
	std::int32_t rel = 0;
	std::memcpy(&rel, reinterpret_cast<const void *>(dispatch + sizeof(kDispatch)), 4);
	if (dispatch + sizeof(kDispatch) + 4 + static_cast<std::intptr_t>(rel) != base + tableRva)
	{
		Skip("register dispatch does not use the handler table");
		return;
	}

	// the vtable slot must hold vanilla UpdateCamera.
	auto * vslot = reinterpret_cast<std::uintptr_t *>(base + vtableRva + 8 * kUpdateCameraSlot);
	if (*vslot != base + updateRva)
	{
		Skip("UpdateCamera vtable slot is not vanilla", ", \"found\": " + JsonHex(*vslot - base));
		return;
	}

	// both RenderShadowmap calls in Render: check both before patching either.
	CheckedCall calls[2];
	std::string sites;
	for (int i = 0; i < 2; i++)
	{
		const CallSite site = { "render_shadowmap", renderRva + kRenderCallOffsets[i], kRenderPreamble, sizeof(kRenderPreamble), renderShadowmapRva };
		calls[i] = CheckCall(site);
		sites += ", \"site" + std::to_string(i) + "\": {" + calls[i].fields.substr(2) + "}";
		if (!calls[i].ok)
		{
			Skip("render site " + std::to_string(i) + ": " + calls[i].reason, sites);
			return;
		}
	}

	auto * tramp = static_cast<SKSETrampolineInterface *>(skse->QueryInterface(kInterface_Trampoline));
	g_renderOriginal = reinterpret_cast<RenderShadowmapFn>(base + renderShadowmapRva);
	g_updateOriginal = reinterpret_cast<UpdateCameraFn>(base + updateRva);
	g_slot = base + slotRva;
	g_vanillaHandler = base + handlerRva;

	std::string reason;
	if (!PatchCall(calls[1], tramp, pluginHandle, reinterpret_cast<void *>(&RenderShadowmapThunk), reason))
	{
		Skip("render site 1: " + reason, sites);
		return;
	}
	if (!PatchCall(calls[0], tramp, pluginHandle, reinterpret_cast<void *>(&RenderShadowmapThunk), reason))
	{
		// site 0 only runs in a mode we have not seen; without it the thunk
		// on site 1 still keeps the matrix right, but the reuse would be
		// wrong for target 3. never reuse then: leave UpdateCamera alone.
		Skip("render site 0: " + reason + " (site 1 hooked as passthrough)", sites);
		return;
	}

	DWORD oldProtect = 0;
	if (!VirtualProtect(vslot, 8, PAGE_READWRITE, &oldProtect))
	{
		Skip("VirtualProtect on the vtable failed (render sites hooked as passthrough)", sites);
		return;
	}
	InterlockedExchangePointer(reinterpret_cast<void * volatile *>(vslot), reinterpret_cast<void *>(&UpdateCameraHook));
	VirtualProtect(vslot, 8, oldProtect, &oldProtect);

	char cfg[200];
	sprintf_s(cfg, ", \"maxmove\": %.2f, \"maxdeg\": %.3f, \"sundeg\": %.3f, \"maxgapms\": %llu, \"edge_texels\": %.2f",
		g_cfg.maxMove, g_cfg.maxDeg, g_cfg.sunDeg, static_cast<unsigned long long>(g_cfg.maxGapMs), g_cfg.edgeTexels);
	skybench_Log("cascade_halfrate: hooked UpdateCamera (vtable) and both RenderShadowmap calls in Render");
	skybench_LogEvent("cascade_halfrate", ", \"status\": \"patched\"" + sites + cfg);
}
