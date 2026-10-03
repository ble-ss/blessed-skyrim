// cascade_cull - stops the sun shadow cascades collecting casters while keeping the sun's shadow receivers (SKYBENCH_CULL_CASCADES).
//
// how the engine uses a cascade (1.7.104, read from the exe):
//   - SetFrameCamera builds each cascade's culling planes (the call we
//     thunk), copies them into the cascade's culler, and sets the cascade's
//     BSShaderAccumulator (shadowMapData + 0x48) to render mode 14.
//   - the cull jobs hand every geometry that passes the planes to the
//     accumulator's register function (id 106567). it calls a per-render-mode
//     handler from a table (id 414684, indexed by accumulator + 0x150), then
//     ORs accumulator + 0x164 (1 << the sun's shadow light index) into the
//     geometry's light data + 0x1c. that bit makes the geometry a receiver.
//   - GetRenderPasses (BSLightingShaderProperty vfunc 0x2a) gives a lit draw
//     its ShadowDir and DefShadow permutation bits only when that bit is set.
// so a cascade that culls everything also strips the sun's receiver bit from
// every lit draw, and the lit pass stops reading the shadow mask.
//
// modes:
//   "1"       keep the planes; swap the mode-14 handler slot (id 414698) for
//             one that registers nothing for the sun's cascade accumulators
//             and forwards every other accumulator (spot and point shadow
//             maps use mode 14 too). receivers are still marked.
//   "planes"  the first version: one plane every bound is behind. cheapest,
//             but it loses the receivers, so the sun shadows vanish.
//   other     the call thunk only (passthrough).
//
// 1.7.104 facts:
//   SetFrameCamera        id 108496  rva 0x157f080
//   the call              +0x1c12    rva 0x1580c92  e8 e9 0d 00 00
//   the builder           id 108499  rva 0x1581a80
//   mode 14 write         +0x2586    rva 0x1581606  c7 81 50 01 00 00 0e 00 00 00
//   register              id 106567  rva 0x151e2f0  dispatch at +0x89
//   mode 14 handler       id 106576  rva 0x151ec10
//   handler table         id 414684  rva 0x33d4a20  (slot 14: id 414698)

#include "cascade_cull.h"

#include "address_library.h"
#include "skybench_log.h"

#include "skse64/PluginAPI.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace {

const std::uint32_t kSetFrameCameraId = 108496;
const std::uint32_t kBuilderId = 108499;
const std::uint32_t kCallOffset = 0x1c12;

const std::uint32_t kRegisterId = 106567;
const std::uint32_t kMode14HandlerId = 106576;
const std::uint32_t kHandlerTableId = 414684;
const std::uint32_t kHandlerSlot14Id = 414698;
const std::uint32_t kShadowMapRenderMode = 14;

// the three bytes before the call: mov rcx, rsi (dirLight into arg 0).
const std::uint8_t kPreamble[3] = { 0x48, 0x8b, 0xce };

// SetFrameCamera + 0x257a: the cascade accumulator gets render mode 14.
//   mov rax, [rsi + 0x148]         cascades array
//   mov rcx, [rdi + rax + 0x48]    shadowMapData[i].accumulator
//   mov dword [rcx + 0x150], 0xe   renderMode = 14
const std::uint32_t kModeWriteOffset = 0x257a;
const std::uint8_t kModeWrite[22] = {
	0x48, 0x8b, 0x86, 0x48, 0x01, 0x00, 0x00,
	0x48, 0x8b, 0x4c, 0x07, 0x48,
	0xc7, 0x81, 0x50, 0x01, 0x00, 0x00, 0x0e, 0x00, 0x00, 0x00
};

// register + 0x89: movsxd rax, [rsi + 0x150]; lea r10, [rip + table].
const std::uint32_t kDispatchOffset = 0x89;
const std::uint8_t kDispatch[10] = { 0x48, 0x63, 0x86, 0x50, 0x01, 0x00, 0x00, 0x4c, 0x8d, 0x15 };

// BSShadowDirectionalLight: cascade count at + 0x140, cascade array at
// + 0x148 (stride 0xf0, RenderShadowmaps loops it), accumulator at + 0x48.
const size_t kCascadeCount = 0x140;
const size_t kCascadeArray = 0x148;
const size_t kCascadeStride = 0xf0;
const size_t kCascadeAccumulator = 0x48;
const std::uint32_t kMaxCascades = 8;

// NiPlane: distance(x) = dot(normal, x) - constant (the engine's
// NiPlane(normal, point) ctor at rva 0xef74a0 stores constant = dot(n, p)).
struct NiPlane
{
	float normal[3];
	float constant;
};

// NiFrustumPlanes as SetFrameCamera builds and copies it: six planes, then
// activePlanes (bit i = plane i, the builder zeroes it and ORs 1 << i per
// plane it writes), then basePlaneStates (0x3f, set by the caller) and two
// more dwords the caller copies along.
struct NiFrustumPlanes
{
	NiPlane planes[6];
	std::uint32_t activePlanes;
	std::uint32_t basePlaneStates;
	std::uint32_t unk68;
	std::uint32_t unk6C;
};
static_assert(sizeof(NiFrustumPlanes) == 0x70, "SetFrameCamera copies 0x70 bytes of planes");

using BuildPlanesFn = void (*)(void * dirLight, NiFrustumPlanes * outPlanes, void * frustumSplit,
	std::uint32_t * splitCornerIndices, std::uint32_t numSplitCornerIndices,
	void * lightDir, void * cameraPos, std::uint32_t cornerOffsetIndex);

// the per-render-mode register handler: (accumulator, geometry, property, ?).
// the vanilla mode 14 handler queues the property's shadow passes and
// always returns true.
using RegisterHandlerFn = bool (*)(void * accumulator, void * geometry, void * property, void * a4);

enum class Mode
{
	kPassthrough,
	kSkipCasters,
	kPlanes,
};

const char * ModeName(Mode m)
{
	switch (m)
	{
	case Mode::kSkipCasters: return "skip_casters";
	case Mode::kPlanes: return "planes";
	default: return "passthrough";
	}
}

BuildPlanesFn g_original = nullptr;
Mode g_mode = Mode::kPassthrough;
std::atomic<std::uint64_t> g_calls{ 0 };

// kSkipCasters state. the slot is patched lazily from the thunk: the engine
// fills the handler table during renderer init, after plugins load.
std::uintptr_t g_slot = 0;					// address of the table's mode 14 slot
std::uintptr_t g_vanillaHandler = 0;		// what the slot must hold before we patch
RegisterHandlerFn g_forward = nullptr;		// the handler we forward to
enum class SlotState { kWaiting, kPatched, kGaveUp };
std::atomic<SlotState> g_slotState{ SlotState::kWaiting };

// the sun's cascade accumulators, refreshed on every SetFrameCamera call.
std::atomic<void *> g_sunAccumulators[kMaxCascades] = {};
std::atomic<std::uint32_t> g_sunAccumulatorCount{ 0 };

const float kFar = 1.0e30f;

std::string Hex(std::uint64_t v)
{
	char buf[32];
	sprintf_s(buf, "\"0x%llx\"", static_cast<unsigned long long>(v));
	return buf;
}

std::string HexBytes(const std::uint8_t * p, size_t n)
{
	std::string out = "\"";
	char buf[4];
	for (size_t i = 0; i < n; i++)
	{
		sprintf_s(buf, i ? " %02x" : "%02x", p[i]);
		out += buf;
	}
	return out + "\"";
}

void CullEverything(NiFrustumPlanes * p)
{
	// plane 0: dot(n, x) - 1e30 is about -1e30 for any bound in the world,
	// far below -radius, so every bound is fully outside and culled at the
	// root. plane 1 is its mirror, which culls the same way if the test
	// ever runs with the opposite sign; under the engine's sign it reads as
	// fully inside and is simply dropped for the subtree.
	p->planes[0] = { { 0.0f, 0.0f, 1.0f }, kFar };
	p->planes[1] = { { 0.0f, 0.0f, 1.0f }, -kFar };
	p->activePlanes |= 0x3;
}

// runs on the cull job threads, for every geometry any mode 14 accumulator
// takes in. true without queuing anything is what the vanilla handler
// returns too; the caller then marks the receiver bit as usual.
bool SkipSunCasters(void * accumulator, void * geometry, void * property, void * a4)
{
	const std::uint32_t n = g_sunAccumulatorCount.load(std::memory_order_relaxed);
	for (std::uint32_t i = 0; i < n; i++)
	{
		if (g_sunAccumulators[i].load(std::memory_order_relaxed) == accumulator)
			return true;
	}
	return g_forward(accumulator, geometry, property, a4);
}

void CacheSunAccumulators(void * dirLight)
{
	auto * light = static_cast<std::uint8_t *>(dirLight);
	std::uint32_t count = *reinterpret_cast<std::uint32_t *>(light + kCascadeCount);
	auto * cascades = *reinterpret_cast<std::uint8_t **>(light + kCascadeArray);
	if (!cascades)
		count = 0;
	if (count > kMaxCascades)
		count = kMaxCascades;

	std::uint32_t kept = 0;
	for (std::uint32_t i = 0; i < count; i++)
	{
		void * accumulator = *reinterpret_cast<void **>(cascades + i * kCascadeStride + kCascadeAccumulator);
		if (accumulator)
			g_sunAccumulators[kept++].store(accumulator, std::memory_order_relaxed);
	}
	g_sunAccumulatorCount.store(kept, std::memory_order_relaxed);
}

void TryPatchSlot()
{
	auto * slot = reinterpret_cast<std::uintptr_t *>(g_slot);
	const std::uintptr_t current = *reinterpret_cast<volatile std::uintptr_t *>(slot);
	const std::uintptr_t ours = reinterpret_cast<std::uintptr_t>(&SkipSunCasters);

	if (current == ours)
		return;
	if (current == 0)
		return;	// table not filled yet; try again next call

	if (current != g_vanillaHandler)
	{
		g_slotState.store(SlotState::kGaveUp);
		skybench_Log("cascade_cull: handler slot holds something else, casters stay");
		skybench_LogEvent("cascade_cull_slot", ", \"status\": \"skipped\", \"reason\": \"slot is not the vanilla mode 14 handler\", \"found\": " +
			Hex(current) + ", \"expected\": " + Hex(g_vanillaHandler));
		return;
	}

	MEMORY_BASIC_INFORMATION mbi = {};
	const DWORD writable = PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
	if (!VirtualQuery(slot, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT || !(mbi.Protect & writable))
	{
		g_slotState.store(SlotState::kGaveUp);
		skybench_LogEvent("cascade_cull_slot", ", \"status\": \"skipped\", \"reason\": \"slot is not writable\"");
		return;
	}

	g_forward = reinterpret_cast<RegisterHandlerFn>(current);
	InterlockedExchangePointer(reinterpret_cast<void * volatile *>(slot), reinterpret_cast<void *>(ours));
	g_slotState.store(SlotState::kPatched);

	skybench_Log("cascade_cull: mode 14 register handler swapped, sun cascades queue no casters");
	skybench_LogEvent("cascade_cull_slot", ", \"status\": \"patched\", \"sun_accumulators\": " +
		std::to_string(g_sunAccumulatorCount.load()));
}

void Thunk(void * dirLight, NiFrustumPlanes * outPlanes, void * frustumSplit,
	std::uint32_t * splitCornerIndices, std::uint32_t numSplitCornerIndices,
	void * lightDir, void * cameraPos, std::uint32_t cornerOffsetIndex)
{
	g_original(dirLight, outPlanes, frustumSplit, splitCornerIndices, numSplitCornerIndices,
		lightDir, cameraPos, cornerOffsetIndex);

	std::uint32_t builtMask = outPlanes->activePlanes;

	if (g_mode == Mode::kPlanes)
	{
		CullEverything(outPlanes);
	}
	else if (g_mode == Mode::kSkipCasters)
	{
		CacheSunAccumulators(dirLight);
		// also re-arms the slot if the engine ever refills its table.
		if (g_slotState.load(std::memory_order_relaxed) != SlotState::kGaveUp)
			TryPatchSlot();
	}

	std::uint64_t n = g_calls.fetch_add(1, std::memory_order_relaxed) + 1;
	if (n == 1 || n % 10000 == 0)
	{
		char fields[192];
		sprintf_s(fields, ", \"calls\": %llu, \"built_mask\": %u, \"mode\": \"%s\", \"slot_patched\": %s, \"sun_accumulators\": %u",
			static_cast<unsigned long long>(n), builtMask, ModeName(g_mode),
			g_slotState.load() == SlotState::kPatched ? "true" : "false",
			g_sunAccumulatorCount.load());
		skybench_LogEvent("cascade_cull_tick", fields);
	}
}

void Skip(const std::string & reason, const std::string & extra = std::string())
{
	std::string escaped;
	for (char c : reason)
	{
		if (c == '"' || c == '\\')
			escaped += '\\';
		escaped += c;
	}
	skybench_Log("cascade_cull: patched nothing - " + reason);
	skybench_LogEvent("cascade_cull", ", \"status\": \"skipped\", \"reason\": \"" + escaped + "\"" + extra);
}

// true when [p, p + n) is committed, executable memory inside the exe image.
bool IsExeCode(std::uintptr_t base, std::uintptr_t p, size_t n)
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

// checks everything kSkipCasters leans on. empty string = all good.
std::string CheckSkipCasters(const AddressLibrary & lib, std::uintptr_t base, std::uint32_t fnRva)
{
	const std::uint32_t registerRva = lib.Rva(kRegisterId);
	const std::uint32_t handlerRva = lib.Rva(kMode14HandlerId);
	const std::uint32_t tableRva = lib.Rva(kHandlerTableId);
	const std::uint32_t slotRva = lib.Rva(kHandlerSlot14Id);
	if (!registerRva || !handlerRva || !tableRva || !slotRva)
		return "register ids not in address library";
	if (slotRva != tableRva + 8 * kShadowMapRenderMode)
		return "handler slot id is not table + 14 * 8";

	// SetFrameCamera sets the cascade accumulators to mode 14.
	const std::uintptr_t modeWrite = base + fnRva + kModeWriteOffset;
	if (!IsExeCode(base, modeWrite, sizeof(kModeWrite)) ||
		std::memcmp(reinterpret_cast<const void *>(modeWrite), kModeWrite, sizeof(kModeWrite)) != 0)
		return "SetFrameCamera mode 14 write not found";

	// register dispatches through the table on accumulator + 0x150.
	const std::uintptr_t dispatch = base + registerRva + kDispatchOffset;
	if (!IsExeCode(base, dispatch, sizeof(kDispatch) + 4) ||
		std::memcmp(reinterpret_cast<const void *>(dispatch), kDispatch, sizeof(kDispatch)) != 0)
		return "register dispatch not found";
	std::int32_t rel = 0;
	std::memcpy(&rel, reinterpret_cast<const void *>(dispatch + sizeof(kDispatch)), 4);
	if (dispatch + sizeof(kDispatch) + 4 + static_cast<std::intptr_t>(rel) != base + tableRva)
		return "register dispatch does not use the handler table";

	g_slot = base + slotRva;
	g_vanillaHandler = base + handlerRva;
	return std::string();
}

} // namespace

void InstallCascadeCull(const SKSEInterface * skse, unsigned long pluginHandle)
{
	char * val = nullptr;
	size_t len = 0;
	if (_dupenv_s(&val, &len, "SKYBENCH_CULL_CASCADES") != 0 || !val)
		return;
	std::string mode(val);
	free(val);
	if (mode == "1")
		g_mode = Mode::kSkipCasters;
	else if (mode == "planes")
		g_mode = Mode::kPlanes;
	else
		g_mode = Mode::kPassthrough;

	AddressLibrary lib;
	if (!lib.Open(1, 7, 104))
	{
		Skip("address library: " + lib.error());
		return;
	}

	std::uint32_t fnRva = lib.Rva(kSetFrameCameraId);
	std::uint32_t builderRva = lib.Rva(kBuilderId);
	if (!fnRva || !builderRva)
	{
		Skip("id not in address library");
		return;
	}

	const std::uintptr_t base = reinterpret_cast<std::uintptr_t>(GetModuleHandleA(nullptr));
	const std::uintptr_t site = base + fnRva + kCallOffset;
	const std::uintptr_t expected = base + builderRva;
	const std::string where = ", \"site_rva\": " + Hex(fnRva + kCallOffset);

	if (g_mode == Mode::kSkipCasters)
	{
		std::string problem = CheckSkipCasters(lib, base, fnRva);
		if (!problem.empty())
		{
			Skip("skip_casters: " + problem, where);
			return;
		}
	}

	if (!IsExeCode(base, site - sizeof(kPreamble), sizeof(kPreamble) + 5))
	{
		Skip("call site is not exe code", where);
		return;
	}

	std::uint8_t found[8] = {};
	std::memcpy(found, reinterpret_cast<const void *>(site - sizeof(kPreamble)), sizeof(found));
	const std::string bytes = where + ", \"bytes\": " + HexBytes(found, sizeof(found));
	const std::uint8_t * call = found + sizeof(kPreamble);

	if (std::memcmp(found, kPreamble, sizeof(kPreamble)) != 0 || call[0] != 0xe8)
	{
		Skip("unexpected bytes at call site", bytes);
		return;
	}

	std::int32_t rel = 0;
	std::memcpy(&rel, call + 1, 4);
	const std::uintptr_t target = site + 5 + static_cast<std::intptr_t>(rel);
	if (target != expected)
	{
		Skip("call target is not the culling-plane builder (another hook got here first?)",
			bytes + ", \"target_rva\": " + Hex(target - base) + ", \"expected_rva\": " + Hex(builderRva));
		return;
	}

	auto * tramp = static_cast<SKSETrampolineInterface *>(skse->QueryInterface(kInterface_Trampoline));
	if (!tramp || !tramp->AllocateFromBranchPool)
	{
		Skip("no skse trampoline interface", bytes);
		return;
	}

	// 14-byte absolute jump stub near the exe: jmp [rip + 0]; dq Thunk.
	auto * stub = static_cast<std::uint8_t *>(tramp->AllocateFromBranchPool(pluginHandle, 14));
	if (!stub)
	{
		Skip("branch pool allocation failed", bytes);
		return;
	}
	const std::intptr_t stubRel = reinterpret_cast<std::intptr_t>(stub) - static_cast<std::intptr_t>(site + 5);
	if (stubRel < INT32_MIN || stubRel > INT32_MAX)
	{
		Skip("branch pool stub out of rel32 range", bytes);
		return;
	}
	const std::uintptr_t thunkAddr = reinterpret_cast<std::uintptr_t>(&Thunk);
	stub[0] = 0xff;
	stub[1] = 0x25;
	std::memset(stub + 2, 0, 4);
	std::memcpy(stub + 6, &thunkAddr, 8);

	g_original = reinterpret_cast<BuildPlanesFn>(target);

	DWORD oldProtect = 0;
	if (!VirtualProtect(reinterpret_cast<void *>(site), 5, PAGE_EXECUTE_READWRITE, &oldProtect))
	{
		Skip("VirtualProtect failed", bytes);
		return;
	}
	const std::int32_t newRel = static_cast<std::int32_t>(stubRel);
	std::memcpy(reinterpret_cast<void *>(site + 1), &newRel, 4);
	VirtualProtect(reinterpret_cast<void *>(site), 5, oldProtect, &oldProtect);
	FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void *>(site), 5);

	std::string extra;
	if (g_mode == Mode::kSkipCasters)
		extra = ", \"slot_rva\": " + Hex(g_slot - base) + ", \"handler_rva\": " + Hex(g_vanillaHandler - base);

	skybench_Log(std::string("cascade_cull: hooked BuildCascadeCameraCullingPlanes call, mode ") + ModeName(g_mode));
	skybench_LogEvent("cascade_cull",
		", \"status\": \"patched\", \"mode\": \"" + std::string(ModeName(g_mode)) + "\"" +
		bytes + ", \"target_rva\": " + Hex(builderRva) + extra);
}
