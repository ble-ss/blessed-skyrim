// allocator_diag - a first-chance crash reporter for the allocator (allocator_crash events), so a silent game death leaves a trace.
//
// the game dies quietly after load with the scrap heap taken (c19, bisect
// 2026-09-23) and leaves no dump. this handler writes where: code, rip as
// module+rva, the data address of an access violation and whether it is
// inside our arena (and committed), the argument registers, and up to 12
// frames unwound through the exe's own .pdata, plus a raw scan of the
// stack for return addresses when unwinding stops early. it formats into
// stack buffers and returns EXCEPTION_CONTINUE_SEARCH every time.

#include "allocator_diag.h"

#include "skybench_log.h"

#include <Windows.h>

#include <atomic>
#include <cstdio>
#include <string>

namespace {

const DWORD kHeapCorruption = 0xC0000374;

std::uintptr_t g_arenaBase = 0;
std::uintptr_t g_arenaSpan = 0;
std::uintptr_t g_scrapBase = 0;
std::uintptr_t g_scrapSpan = 0;
std::uintptr_t g_exeBase = 0;
std::uintptr_t g_exeEnd = 0;
std::uintptr_t g_ourBase = 0;
std::uintptr_t g_ourEnd = 0;
const int kMaxReports = 32;
std::atomic<int> g_reports{ 0 };
std::atomic<std::uintptr_t> g_seenRips[kMaxReports];
std::atomic<DWORD> g_busyThread{ 0 };

void ModuleRange(HMODULE mod, std::uintptr_t & base, std::uintptr_t & end)
{
	base = reinterpret_cast<std::uintptr_t>(mod);
	auto * dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(mod);
	auto * nt = reinterpret_cast<const IMAGE_NT_HEADERS64 *>(base + dos->e_lfanew);
	end = base + nt->OptionalHeader.SizeOfImage;
}

// "exe+0x1234", "skybench+0x1234", "arena+0x1234", "name.dll+0x1234" or "0x...".
void Describe(std::uintptr_t a, char * out, size_t n)
{
	if (a >= g_exeBase && a < g_exeEnd)
	{
		sprintf_s(out, n, "exe+0x%llx", static_cast<unsigned long long>(a - g_exeBase));
		return;
	}
	if (a >= g_ourBase && a < g_ourEnd)
	{
		sprintf_s(out, n, "skybench+0x%llx", static_cast<unsigned long long>(a - g_ourBase));
		return;
	}
	if (a - g_arenaBase < g_arenaSpan)
	{
		sprintf_s(out, n, "mm_arena+0x%llx", static_cast<unsigned long long>(a - g_arenaBase));
		return;
	}
	if (a - g_scrapBase < g_scrapSpan)
	{
		sprintf_s(out, n, "scrap_arena+0x%llx", static_cast<unsigned long long>(a - g_scrapBase));
		return;
	}
	HMODULE mod = nullptr;
	if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			reinterpret_cast<LPCSTR>(a), &mod) && mod)
	{
		char path[MAX_PATH] = {};
		GetModuleFileNameA(mod, path, MAX_PATH);
		const char * name = path;
		for (const char * c = path; *c; c++)
		{
			if (*c == '\\' || *c == '/')
				name = c + 1;
		}
		sprintf_s(out, n, "%s+0x%llx", name, static_cast<unsigned long long>(a - reinterpret_cast<std::uintptr_t>(mod)));
		return;
	}
	sprintf_s(out, n, "0x%llx", static_cast<unsigned long long>(a));
}

bool InCode(std::uintptr_t a)
{
	return (a >= g_exeBase && a < g_exeEnd) || (a >= g_ourBase && a < g_ourEnd);
}

const char * StateName(std::uintptr_t a)
{
	MEMORY_BASIC_INFORMATION mbi = {};
	if (!VirtualQuery(reinterpret_cast<void *>(a), &mbi, sizeof(mbi)))
		return "unknown";
	if (mbi.State == MEM_COMMIT)
		return (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) ? "committed-noaccess" : "committed";
	if (mbi.State == MEM_RESERVE)
		return "reserved";
	return "free";
}

void Append(char * buf, size_t n, size_t & used, const char * s)
{
	int w = sprintf_s(buf + used, n - used, "%s", s);
	if (w > 0)
		used += static_cast<size_t>(w);
}

LONG CALLBACK Handler(EXCEPTION_POINTERS * ep)
{
	const EXCEPTION_RECORD * er = ep->ExceptionRecord;
	const DWORD code = er->ExceptionCode;
	const bool fault = code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_ILLEGAL_INSTRUCTION ||
		code == EXCEPTION_PRIV_INSTRUCTION || code == EXCEPTION_STACK_OVERFLOW;
	if (!fault && code != kHeapCorruption)
		return EXCEPTION_CONTINUE_SEARCH;

	const CONTEXT * ctx = ep->ContextRecord;
	if (fault && !InCode(ctx->Rip))
		return EXCEPTION_CONTINUE_SEARCH;

	// one report at a time, never re-entered from our own formatting.
	const DWORD self = GetCurrentThreadId();
	if (g_busyThread.load() == self)
		return EXCEPTION_CONTINUE_SEARCH;

	// the game may probe memory under its own __try and survive; report
	// each distinct faulting rip once, so those can't use up the budget
	// before the fatal one arrives.
	{
		const std::uintptr_t rip = ctx->Rip;
		for (auto & seen : g_seenRips)
		{
			if (seen.load(std::memory_order_relaxed) == rip)
				return EXCEPTION_CONTINUE_SEARCH;
		}
		const int slot = g_reports.fetch_add(1);
		if (slot >= kMaxReports)
			return EXCEPTION_CONTINUE_SEARCH;
		g_seenRips[slot].store(rip, std::memory_order_relaxed);
	}
	DWORD expected = 0;
	while (!g_busyThread.compare_exchange_weak(expected, self))
	{
		expected = 0;
		Sleep(1);
	}

	char buf[3072];
	size_t used = 0;
	char a[96];
	char tmp[256];

	Describe(ctx->Rip, a, sizeof(a));
	sprintf_s(tmp, ", \"code\": \"0x%08lx\", \"thread\": %lu, \"rip\": \"%s\", \"first_chance\": true", code, self, a);
	Append(buf, sizeof(buf), used, tmp);

	if (code == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2)
	{
		const std::uintptr_t data = er->ExceptionInformation[1];
		const ULONG_PTR op = er->ExceptionInformation[0];
		Describe(data, a, sizeof(a));
		sprintf_s(tmp, ", \"access\": \"%s\", \"data\": \"%s\", \"data_raw\": \"0x%llx\", \"data_in_arena\": \"%s\", \"data_state\": \"%s\"",
			op == 0 ? "read" : (op == 1 ? "write" : "execute"), a, static_cast<unsigned long long>(data),
			(data - g_arenaBase < g_arenaSpan) ? "mm" : ((data - g_scrapBase < g_scrapSpan) ? "scrap" : "no"), StateName(data));
		Append(buf, sizeof(buf), used, tmp);
	}

	sprintf_s(tmp, ", \"rax\": \"0x%llx\", \"rcx\": \"0x%llx\", \"rdx\": \"0x%llx\", \"r8\": \"0x%llx\", \"r9\": \"0x%llx\", \"rsp\": \"0x%llx\"",
		static_cast<unsigned long long>(ctx->Rax), static_cast<unsigned long long>(ctx->Rcx),
		static_cast<unsigned long long>(ctx->Rdx), static_cast<unsigned long long>(ctx->R8),
		static_cast<unsigned long long>(ctx->R9), static_cast<unsigned long long>(ctx->Rsp));
	Append(buf, sizeof(buf), used, tmp);

	// unwind through .pdata.
	Append(buf, sizeof(buf), used, ", \"frames\": [");
	CONTEXT c = *ctx;
	int frames = 0;
	for (; frames < 12 && c.Rip; frames++)
	{
		Describe(c.Rip, a, sizeof(a));
		sprintf_s(tmp, "%s\"%s\"", frames ? ", " : "", a);
		Append(buf, sizeof(buf), used, tmp);

		DWORD64 imageBase = 0;
		PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(c.Rip, &imageBase, nullptr);
		if (!fn)
		{
			// leaf or stub without unwind data: the return address is at rsp.
			if (c.Rsp & 7)
				break;
			MEMORY_BASIC_INFORMATION mbi = {};
			if (!VirtualQuery(reinterpret_cast<void *>(c.Rsp), &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT)
				break;
			c.Rip = *reinterpret_cast<DWORD64 *>(c.Rsp);
			c.Rsp += 8;
			continue;
		}
		void * handlerData = nullptr;
		DWORD64 establisher = 0;
		RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, c.Rip, fn, &c, &handlerData, &establisher, nullptr);
	}
	Append(buf, sizeof(buf), used, "]");

	// raw scan: code addresses in the first 0x400 bytes of the stack.
	Append(buf, sizeof(buf), used, ", \"stack_scan\": [");
	int found = 0;
	MEMORY_BASIC_INFORMATION mbi = {};
	if (VirtualQuery(reinterpret_cast<void *>(ctx->Rsp), &mbi, sizeof(mbi)) && mbi.State == MEM_COMMIT)
	{
		const std::uintptr_t top = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
		for (std::uintptr_t p = ctx->Rsp & ~std::uintptr_t(7); p + 8 <= top && p < ctx->Rsp + 0x400 && found < 10; p += 8)
		{
			const std::uintptr_t v = *reinterpret_cast<const std::uintptr_t *>(p);
			if (!InCode(v))
				continue;
			Describe(v, a, sizeof(a));
			sprintf_s(tmp, "%s\"+0x%llx %s\"", found ? ", " : "", static_cast<unsigned long long>(p - ctx->Rsp), a);
			Append(buf, sizeof(buf), used, tmp);
			found++;
		}
	}
	Append(buf, sizeof(buf), used, "]");

	skybench_LogEvent("allocator_crash", buf);
	g_busyThread.store(0);
	return EXCEPTION_CONTINUE_SEARCH;
}

} // namespace

void InstallAllocatorCrashHandler(std::uintptr_t arenaBase, std::uintptr_t arenaSpan,
	std::uintptr_t scrapBase, std::uintptr_t scrapSpan)
{
	g_arenaBase = arenaBase;
	g_arenaSpan = arenaSpan;
	g_scrapBase = scrapBase;
	g_scrapSpan = scrapSpan;
	ModuleRange(GetModuleHandleA(nullptr), g_exeBase, g_exeEnd);

	HMODULE ours = nullptr;
	GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
		reinterpret_cast<LPCSTR>(&InstallAllocatorCrashHandler), &ours);
	if (ours)
		ModuleRange(ours, g_ourBase, g_ourEnd);

	if (AddVectoredExceptionHandler(1, &Handler))
		skybench_Log("allocator: crash reporter armed (allocator_crash events)");
}
