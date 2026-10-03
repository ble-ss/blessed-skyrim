// havok_step - sets fMaxTime:HAVOK from the measured frame time each frame, clamped to a min/max fps (SKYBENCH_HAVOK).
//
// 1.7.104 facts (capstone over the exe, cross-checked against the address
// library bin):
//   Main::Render         id 36559  rva 0x656e60 (precip_gate.cpp already
//                         hooks a different call inside this same function)
//   our frame-tick call   +0xd3     rva 0x656f33  49 8b cc | e8 58 0d 00 00
//                         (mov rcx, r12; call). unconditional: nothing
//                         between Main::Render's entry and this call can
//                         skip it, so it runs exactly once per call to
//                         Main::Render (checked by disassembling the whole
//                         span). the callee (id 36561, rva 0x657c90) takes
//                         the original rcx argument and returns a pointer
//                         used later in the function; we pass both through
//                         unchanged.
//   INISettingCollection::Singleton      ae id 411155  rva 0x3327860
//   INIPrefSettingCollection::Singleton  ae id 410219  rva 0x3275710
//     (ids from CommonLibSSE-NG's Offset.h, RELOCATION_ID's ae slot. both
//     resolve, in versionlib-1-7-104-0.bin, to the exact rva skse64's own
//     GameSettings.cpp hardcodes for the pre-AE runtime - g_iniSettingCollection
//     and g_iniPrefSettingCollection have not moved across these builds,
//     which is an independent, exact confirmation of the id.)
//
// SettingCollectionList's layout (skse64's GameSettings.h, cross-checked
// against CommonLibSSE-NG's static_assert(sizeof(INISettingCollection) ==
// 0x128)): a vtable, then an inline first Entry { Setting * setting; Entry *
// next; } at +0x118. Setting is { vtable; Data data (a union, +0x8); char *
// name (+0x10) }. we walk this list ourselves (bounded, every pointer
// checked with VirtualQuery before it's read) instead of calling the
// engine's Get() - one linked-list walk at install, nothing to hook.
//
// SKYBENCH_HAVOK=perframe turns this on (default off, does nothing).
// SKYBENCH_HAVOK_MINFPS (default 60) and SKYBENCH_HAVOK_MAXFPS (default 300)
// set the clamp: fMaxTime:HAVOK = clamp(last frame time, 1/MAXFPS, 1/MINFPS),
// matching SkyrimDisplayTweaks' havok fix. logs havok_fix (installed or
// skipped, with why) and havok_fix_tick every 600 frames.

#include "havok_step.h"

#include "address_library.h"
#include "call_patch.h"
#include "skybench_log.h"

#include "skse64/PluginAPI.h"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

const std::uint32_t kMainRenderId = 36559;
const std::uint32_t kCallOffset = 0xd3;
const std::uint32_t kCallTargetId = 36561;
const std::uint8_t kPreamble[3] = { 0x49, 0x8b, 0xcc };	// mov rcx, r12

const std::uint32_t kIniSettingCollectionId = 411155;
const std::uint32_t kIniPrefSettingCollectionId = 410219;

const std::size_t kItemsOffset = 0x118;
const std::size_t kSettingDataOffset = 0x08;
const std::size_t kSettingNameOffset = 0x10;
const std::uint32_t kMaxEntries = 8192;
const std::uint32_t kTickEvery = 600;

using VoidPtrFn = void * (*)(void *);

VoidPtrFn g_original = nullptr;
std::atomic<float *> g_fMaxTime{ nullptr };
float g_minFps = 60.0f;
float g_maxFps = 300.0f;

std::chrono::steady_clock::time_point g_lastTick;
bool g_haveLastTick = false;
std::uint32_t g_frames = 0;

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
	skybench_Log("havok_step: patched nothing - " + reason);
	skybench_LogEvent("havok_fix", ", \"status\": \"skipped\", \"reason\": \"" + Escape(reason) + "\"" + extra);
}

// true when [p, p + n) is committed, readable memory (any protection short
// of no-access/guard). used for every pointer we follow through the ini
// setting collections, since they are plain heap data we never verified by
// disassembly.
bool IsReadable(const void * p, std::size_t n)
{
	if (!p)
		return false;
	MEMORY_BASIC_INFORMATION mbi = {};
	if (!VirtualQuery(p, &mbi, sizeof(mbi)))
		return false;
	if (mbi.State != MEM_COMMIT)
		return false;
	if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))
		return false;
	return reinterpret_cast<std::uintptr_t>(p) + n <=
		reinterpret_cast<std::uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
}

// a bounded, checked read of a setting name. empty when it isn't a short,
// readable, null-terminated string (never trusts an unbounded strlen).
std::string SafeName(const char * name)
{
	std::string out;
	for (std::size_t i = 0; i < 64; i++)
	{
		if (!IsReadable(name + i, 1))
			return std::string();
		char c = name[i];
		if (c == '\0')
			return out;
		out += c;
	}
	return std::string();
}

// walks one SettingCollectionList looking for wantName. null if not found,
// or if the list looks corrupt before it's found.
float * FindSetting(std::uintptr_t collection, const char * wantName, float & outDefault)
{
	std::uintptr_t entry = collection + kItemsOffset;

	for (std::uint32_t i = 0; i < kMaxEntries && entry; i++)
	{
		if (!IsReadable(reinterpret_cast<void *>(entry), 16))
			return nullptr;

		std::uintptr_t setting = 0, next = 0;
		std::memcpy(&setting, reinterpret_cast<void *>(entry), 8);
		std::memcpy(&next, reinterpret_cast<void *>(entry + 8), 8);

		if (setting && IsReadable(reinterpret_cast<void *>(setting + kSettingNameOffset), 8))
		{
			char * namePtr = nullptr;
			std::memcpy(&namePtr, reinterpret_cast<void *>(setting + kSettingNameOffset), 8);

			if (SafeName(namePtr) == wantName && IsReadable(reinterpret_cast<void *>(setting + kSettingDataOffset), 4))
			{
				float * data = reinterpret_cast<float *>(setting + kSettingDataOffset);
				outDefault = *data;
				return data;
			}
		}

		entry = next;
	}
	return nullptr;
}

// main ini collection first, then the pref one - the same order skse64's
// own GetINISetting uses.
float * ResolveHavokSetting(const AddressLibrary & lib, std::uintptr_t base, std::string & how)
{
	const std::uint32_t rvas[2] = { lib.Rva(kIniSettingCollectionId), lib.Rva(kIniPrefSettingCollectionId) };
	const char * const names[2] = { "ini", "inipref" };

	for (int pass = 0; pass < 2; pass++)
	{
		if (!rvas[pass])
			continue;

		void * singletonAddr = reinterpret_cast<void *>(base + rvas[pass]);
		if (!IsReadable(singletonAddr, 8))
			continue;

		std::uintptr_t collection = 0;
		std::memcpy(&collection, singletonAddr, 8);
		if (!collection || !IsReadable(reinterpret_cast<void *>(collection + kItemsOffset), 16))
			continue;

		float def = 0.0f;
		float * found = FindSetting(collection, "fMaxTime:HAVOK", def);
		if (found && def > 0.0001f && def < 1.0f)
		{
			how = names[pass];
			return found;
		}
	}
	return nullptr;
}

void * Thunk(void * a1)
{
	void * result = g_original(a1);

	float * setting = g_fMaxTime.load(std::memory_order_relaxed);
	if (setting)
	{
		auto now = std::chrono::steady_clock::now();
		if (g_haveLastTick)
		{
			double dt = std::chrono::duration<double>(now - g_lastTick).count();
			const double lo = 1.0 / g_maxFps;
			const double hi = 1.0 / g_minFps;
			if (dt < lo) dt = lo;
			if (dt > hi) dt = hi;
			*setting = static_cast<float>(dt);

			if (++g_frames >= kTickEvery)
			{
				char buf[160];
				sprintf_s(buf, ", \"fmaxtime\": %.6f, \"frame_time\": %.6f", *setting, dt);
				skybench_LogEvent("havok_fix_tick", buf);
				g_frames = 0;
			}
		}
		g_lastTick = now;
		g_haveLastTick = true;
	}

	return result;
}

} // namespace

void InstallHavokStep(const SKSEInterface * skse, unsigned long pluginHandle)
{
	char * val = nullptr;
	std::size_t len = 0;
	if (_dupenv_s(&val, &len, "SKYBENCH_HAVOK") != 0 || !val)
		return;
	std::string mode(val);
	free(val);
	if (mode != "perframe" && mode != "1")
		return;

	auto readFpsEnv = [](const char * name, float def) -> float
	{
		char * v = nullptr;
		std::size_t n = 0;
		float out = def;
		if (_dupenv_s(&v, &n, name) == 0 && v)
		{
			double parsed = atof(v);
			if (parsed > 0.0)
				out = static_cast<float>(parsed);
			free(v);
		}
		return out;
	};
	g_minFps = readFpsEnv("SKYBENCH_HAVOK_MINFPS", 60.0f);
	g_maxFps = readFpsEnv("SKYBENCH_HAVOK_MAXFPS", 300.0f);
	if (g_maxFps <= g_minFps)
	{
		Skip("SKYBENCH_HAVOK_MAXFPS must be greater than SKYBENCH_HAVOK_MINFPS");
		return;
	}

	AddressLibrary lib;
	if (!lib.Open(1, 7, 104))
	{
		Skip("address library: " + lib.error());
		return;
	}

	const std::uintptr_t base = reinterpret_cast<std::uintptr_t>(GetModuleHandleA(nullptr));

	std::string how;
	float * setting = ResolveHavokSetting(lib, base, how);
	if (!setting)
	{
		Skip("fMaxTime:HAVOK not found (or not sane) in the ini setting collections");
		return;
	}

	const std::uint32_t renderRva = lib.Rva(kMainRenderId);
	const std::uint32_t targetRva = lib.Rva(kCallTargetId);
	if (!renderRva || !targetRva)
	{
		Skip("id not in address library");
		return;
	}

	const CallSite site = { "havok_step", renderRva + kCallOffset, kPreamble, sizeof(kPreamble), targetRva };
	const CheckedCall call = CheckCall(site);
	if (!call.ok)
	{
		Skip(call.reason, call.fields);
		return;
	}

	g_original = reinterpret_cast<VoidPtrFn>(call.target);

	auto * tramp = static_cast<SKSETrampolineInterface *>(skse->QueryInterface(kInterface_Trampoline));
	std::string reason;
	if (!PatchCall(call, tramp, pluginHandle, reinterpret_cast<void *>(&Thunk), reason))
	{
		Skip(reason, call.fields);
		return;
	}

	g_fMaxTime.store(setting, std::memory_order_relaxed);

	char extra[192];
	sprintf_s(extra, ", \"collection\": \"%s\", \"initial_value\": %.6f, \"min_fps\": %.1f, \"max_fps\": %.1f",
		how.c_str(), *setting, g_minFps, g_maxFps);

	skybench_Log("havok_step: hooked Main::Render, fMaxTime:HAVOK found in " + how);
	skybench_LogEvent("havok_fix", std::string(", \"status\": \"installed\"") + extra + call.fields);
}
