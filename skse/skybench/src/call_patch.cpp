// call_patch - checks an e8 call in the exe, then routes it through a thunk (see call_patch.h).

#include "call_patch.h"

#include "skse64/PluginAPI.h"

#include <cstdio>
#include <cstring>

namespace {

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

} // namespace

std::string JsonHex(std::uint64_t v)
{
	char buf[32];
	sprintf_s(buf, "\"0x%llx\"", static_cast<unsigned long long>(v));
	return buf;
}

CheckedCall CheckCall(const CallSite & s)
{
	CheckedCall out;
	const std::uintptr_t base = reinterpret_cast<std::uintptr_t>(GetModuleHandleA(nullptr));
	const std::uintptr_t site = base + s.siteRva;
	out.fields = ", \"site_rva\": " + JsonHex(s.siteRva);

	if (s.preambleLen > 8 || !IsExeCode(base, site - s.preambleLen, s.preambleLen + 5))
	{
		out.reason = "call site is not exe code";
		return out;
	}

	std::uint8_t found[13] = {};
	const std::size_t n = s.preambleLen + 5;
	std::memcpy(found, reinterpret_cast<const void *>(site - s.preambleLen), n);
	out.fields += ", \"bytes\": " + HexBytes(found, n);
	const std::uint8_t * call = found + s.preambleLen;

	if (std::memcmp(found, s.preamble, s.preambleLen) != 0 || call[0] != 0xe8)
	{
		out.reason = "unexpected bytes at call site";
		return out;
	}

	std::int32_t rel = 0;
	std::memcpy(&rel, call + 1, 4);
	const std::uintptr_t target = site + 5 + static_cast<std::intptr_t>(rel);
	if (target != base + s.targetRva)
	{
		out.reason = "call target is not the expected function (another hook got here first?)";
		out.fields += ", \"target_rva\": " + JsonHex(target - base) + ", \"expected_rva\": " + JsonHex(s.targetRva);
		return out;
	}

	out.ok = true;
	out.site = site;
	out.target = target;
	return out;
}

bool PatchCall(const CheckedCall & call, SKSETrampolineInterface * tramp, std::uint32_t pluginHandle,
	void * thunk, std::string & reason)
{
	if (!call.ok)
	{
		reason = "site not checked";
		return false;
	}
	if (!tramp || !tramp->AllocateFromBranchPool)
	{
		reason = "no skse trampoline interface";
		return false;
	}

	// 14-byte absolute jump stub near the exe: jmp [rip + 0]; dq thunk.
	auto * stub = static_cast<std::uint8_t *>(tramp->AllocateFromBranchPool(pluginHandle, 14));
	if (!stub)
	{
		reason = "branch pool allocation failed";
		return false;
	}
	const std::intptr_t stubRel = reinterpret_cast<std::intptr_t>(stub) - static_cast<std::intptr_t>(call.site + 5);
	if (stubRel < INT32_MIN || stubRel > INT32_MAX)
	{
		reason = "branch pool stub out of rel32 range";
		return false;
	}
	const std::uintptr_t thunkAddr = reinterpret_cast<std::uintptr_t>(thunk);
	stub[0] = 0xff;
	stub[1] = 0x25;
	std::memset(stub + 2, 0, 4);
	std::memcpy(stub + 6, &thunkAddr, 8);

	DWORD oldProtect = 0;
	if (!VirtualProtect(reinterpret_cast<void *>(call.site), 5, PAGE_EXECUTE_READWRITE, &oldProtect))
	{
		reason = "VirtualProtect failed";
		return false;
	}
	const std::int32_t newRel = static_cast<std::int32_t>(stubRel);
	std::memcpy(reinterpret_cast<void *>(call.site + 1), &newRel, 4);
	VirtualProtect(reinterpret_cast<void *>(call.site), 5, oldProtect, &oldProtect);
	FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void *>(call.site), 5);
	return true;
}
