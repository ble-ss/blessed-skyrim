// water_halfrate - Main_RenderWaterEffects at half rate on the cpu (SKYBENCH_WATER_HALFRATE).
//
// 1.7.104 facts (capstone over the exe, cross-checked against the address
// library bin, scratchpad/water/{resolve,disasm}.py):
//   Main::Render               id 36559  rva 0x656e60
//   our call site               +0x172    rva 0x656fd2  49 8b cc | e8 59 09 00 00
//                               (mov rcx, r12; call). unconditional inside
//                               Main::Render's body.
//   Main_RenderWaterEffects    id 36560  rva 0x657930 (call target above)
//
// what 36560 does (full disassembly walked by hand, scratchpad/water/):
//   a leading gate: a global bool at rva 0x20b3730 - if false, 36560 does
//   nothing at all and returns immediately (no TESWaterSystem::Update, no
//   render). if true, it calls a second gate function (id 82074, a form
//   lookup plus a flag test plus a release call - not cheap) whose result
//   picks between "do the work now" and a rare path (~1/2048 calls, a
//   global counter's bit 11) that either also does the work or resets a
//   couple of counters and tail-calls elsewhere, touching neither
//   TESWaterSystem::Update nor the render queue.
//   only past all of that does it call TESWaterSystem::Update (id 32174)
//   and then drain a small lock-free job queue (head/tail indices live in
//   the water object, not in 36560 or a global frame counter) - each entry
//   is one queued reflection-cube face, invoked through its own vtable
//   (indirect calls, no fixed target to patch). the scouting doc's "2 faces
//   a frame" are jobs posted to this queue elsewhere; 36560 only drains
//   whatever is queued.
//
// trap 1 (face rotation): safe. the queue is not indexed by 36560's own
// counter or by the frame count, so skipping a call to 36560 just leaves
// jobs queued one extra frame - no face is ever skipped or stuck.
//
// trap 2 (the simulation is inside): real, but NOT safely splittable with
// what call_patch can do. a first cut here patched only the outer call and,
// on a skip frame, called TESWaterSystem::Update directly instead of
// nothing, replicating the four instructions vanilla uses to set up its
// arguments. in-game (campaign c40) that was 25% SLOWER (Whiterun 362->270
// fps), all in cpu time, gpu unchanged. root cause, found by re-reading the
// disassembly against the regression: that direct call skipped vanilla's
// own leading gate and the non-trivial second gate call above it, so it
// paid for a full TESWaterSystem::Update on frames where vanilla's gate
// would have skipped the whole function - and correctly deciding whether to
// call Update at all means running most of that same gate every skip frame
// too, which erases the saving. it also used VirtualQuery-backed pointer
// checks in the hot path (nothing else in this plugin's per-frame code
// does that) as a defensive read - cheap looking, but a kernel transition
// added twice a frame on every skip frame.
// given both problems trace back to reimplementing engine-internal gating
// we can only verify by benchmarking in-game, the safe fix is the simple
// one: skip the whole 36560 call on alternate frames, nothing else. that
// means TESWaterSystem::Update (ripples/displacement) also runs at half
// rate on the skipped frames - a visual-only tradeoff (matches how
// precip_gate/the fork's own halfrate parts already trade a frame of lag
// for cpu), not a correctness or performance risk.

#include "water_halfrate.h"

#include "address_library.h"
#include "call_patch.h"
#include "skybench_log.h"

#include "skse64/PluginAPI.h"

#include <cstdint>
#include <cstdlib>
#include <string>

namespace {

const std::uint32_t kMainRenderId = 36559;
const std::uint32_t kCallOffset = 0x172;
const std::uint32_t kWaterEffectsId = 36560;

// mov rcx, r12 (the call's preamble)
const std::uint8_t kPreamble[3] = { 0x49, 0x8b, 0xcc };

const std::uint32_t kTickEvery = 600;

using VoidFn = void (*)();

VoidFn g_original = nullptr;

bool g_frameToggle = false;
std::uint32_t g_calls = 0;
std::uint32_t g_rendered = 0;
std::uint32_t g_skipped = 0;

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
	skybench_Log("water_halfrate: patched nothing - " + reason);
	skybench_LogEvent("water_halfrate", ", \"status\": \"skipped\", \"reason\": \"" + Escape(reason) + "\"" + extra);
}

void Thunk()
{
	if (g_frameToggle)
	{
		g_rendered++;
		g_original();
	}
	else
	{
		g_skipped++;
	}
	g_frameToggle = !g_frameToggle;

	if (++g_calls >= kTickEvery)
	{
		char buf[128];
		sprintf_s(buf, ", \"rendered\": %u, \"skipped\": %u", g_rendered, g_skipped);
		skybench_LogEvent("water_halfrate_tick", buf);
		g_calls = 0;
		g_rendered = 0;
		g_skipped = 0;
	}
}

} // namespace

void InstallWaterHalfrate(const SKSEInterface * skse, unsigned long pluginHandle)
{
	char * val = nullptr;
	std::size_t len = 0;
	if (_dupenv_s(&val, &len, "SKYBENCH_WATER_HALFRATE") != 0 || !val)
		return;
	std::string mode(val);
	free(val);
	if (mode != "1" && mode != "on")
		return;

	AddressLibrary lib;
	if (!lib.Open(1, 7, 104))
	{
		Skip("address library: " + lib.error());
		return;
	}

	const std::uint32_t mainRender = lib.Rva(kMainRenderId);
	const std::uint32_t waterEffects = lib.Rva(kWaterEffectsId);
	if (!mainRender || !waterEffects)
	{
		Skip("id not in address library");
		return;
	}

	const CallSite site = { "water_halfrate", mainRender + kCallOffset, kPreamble, sizeof(kPreamble), waterEffects };
	const CheckedCall call = CheckCall(site);
	if (!call.ok)
	{
		Skip(call.reason, call.fields);
		return;
	}

	g_original = reinterpret_cast<VoidFn>(call.target);

	auto * tramp = static_cast<SKSETrampolineInterface *>(skse->QueryInterface(kInterface_Trampoline));
	std::string reason;
	if (!PatchCall(call, tramp, pluginHandle, reinterpret_cast<void *>(&Thunk), reason))
	{
		Skip(reason, call.fields);
		return;
	}

	skybench_Log("water_halfrate: hooked Main_RenderWaterEffects, skipping the call on alternate frames");
	skybench_LogEvent("water_halfrate",
		std::string(", \"status\": \"patched\"") + call.fields + ", \"target_rva\": " + JsonHex(waterEffects));
}
