// allocator_patch - points a game function's entry at ours and keeps a way to call whatever it ran before.
//
// every entry gets a 5-byte e9 jump to a stub near the exe, written as one
// aligned 8-byte exchange so no thread can ever run half a jump. the stub
// is an absolute jump to our hook. what the game ran before stays callable:
//   vanilla entry     a trampoline: the first `stolen` bytes (checked to be
//                     position independent on 1.7.104), then a jump back
//                     to entry + stolen; or entry + skip, past a null-check
//                     prologue our hook does itself.
//   a jump already    its target (engine fixes, or any other hooker). that
//                     target is a whole function with the same contract,
//                     so calling it is exactly what the game would have done.

#include "allocator_patch.h"

#include <Windows.h>

#include <cstring>

namespace alloc_patch {

namespace {

const std::size_t kEntryJumpLen = 5;
const std::size_t kAbsJumpLen = 14;
const std::size_t kSlotSize = 64;

bool IsExecutable(std::uintptr_t p, std::size_t n)
{
	MEMORY_BASIC_INFORMATION mbi = {};
	if (!VirtualQuery(reinterpret_cast<void *>(p), &mbi, sizeof(mbi)))
		return false;
	if (mbi.State != MEM_COMMIT)
		return false;
	const DWORD exec = PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
	if (!(mbi.Protect & exec))
		return false;
	return p + n <= reinterpret_cast<std::uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
}

std::string OwnerOf(std::uintptr_t p)
{
	HMODULE mod = nullptr;
	if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			reinterpret_cast<LPCSTR>(p), &mod) || !mod)
		return "anonymous memory";

	char path[MAX_PATH] = {};
	if (!GetModuleFileNameA(mod, path, MAX_PATH))
		return "unknown module";

	const char * name = path;
	for (const char * c = path; *c; c++)
	{
		if (*c == '\\' || *c == '/')
			name = c + 1;
	}
	return name;
}

void WriteAbsJump(std::uint8_t * at, std::uintptr_t target)
{
	at[0] = 0xff;
	at[1] = 0x25;
	std::memset(at + 2, 0, 4);
	std::memcpy(at + 6, &target, 8);
}

// a free, committed read-write block within rel32 reach of `near`.
std::uint8_t * AllocNear(std::uintptr_t nearAddr, std::size_t size)
{
	SYSTEM_INFO si = {};
	GetSystemInfo(&si);
	const std::uintptr_t gran = si.dwAllocationGranularity;
	const std::uintptr_t reach = 0x70000000;	// well inside +-2 GiB from anything in the image

	const std::uintptr_t start = nearAddr & ~(gran - 1);
	for (std::uintptr_t step = gran; step < reach; step += gran)
	{
		const std::uintptr_t candidates[2] = { start - step, start + step };
		for (std::uintptr_t a : candidates)
		{
			if (a < gran)
				continue;
			MEMORY_BASIC_INFORMATION mbi = {};
			if (!VirtualQuery(reinterpret_cast<void *>(a), &mbi, sizeof(mbi)) || mbi.State != MEM_FREE)
				continue;
			void * p = VirtualAlloc(reinterpret_cast<void *>(a), size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
			if (p)
				return static_cast<std::uint8_t *>(p);
		}
	}
	return nullptr;
}

bool RelReach(std::uintptr_t from, std::uintptr_t to)
{
	const std::int64_t rel = static_cast<std::int64_t>(to) - static_cast<std::int64_t>(from);
	return rel >= INT32_MIN && rel <= INT32_MAX;
}

bool WriteEntryJump(std::uintptr_t entry, std::uintptr_t stub)
{
	const std::int32_t rel = static_cast<std::int32_t>(static_cast<std::int64_t>(stub) - static_cast<std::int64_t>(entry + kEntryJumpLen));

	DWORD oldProtect = 0;
	if (!VirtualProtect(reinterpret_cast<void *>(entry), 8, PAGE_EXECUTE_READWRITE, &oldProtect))
		return false;

	volatile LONG64 * q = reinterpret_cast<volatile LONG64 *>(entry);
	LONG64 value = *q;
	std::uint8_t * b = reinterpret_cast<std::uint8_t *>(&value);
	b[0] = 0xe9;
	std::memcpy(b + 1, &rel, 4);
	InterlockedExchange64(q, value);

	DWORD ignored = 0;
	VirtualProtect(reinterpret_cast<void *>(entry), 8, oldProtect, &ignored);
	FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void *>(entry), 8);
	return true;
}

} // namespace

const char * KindName(EntryKind k)
{
	switch (k)
	{
	case EntryKind::Vanilla: return "vanilla";
	case EntryKind::AbsJump: return "abs_jump";
	case EntryKind::RelJump: return "rel_jump";
	case EntryKind::MovJump: return "mov_jump";
	default: return "unknown";
	}
}

bool Prepare(Site & s, std::uintptr_t imageBase, std::uint32_t rva, std::string & why)
{
	s.entry = 0;
	s.kind = EntryKind::Unknown;
	s.jumpTarget = 0;
	s.owner.clear();

	if (!rva)
	{
		why = std::string(s.name) + ": address library has no rva for id " + std::to_string(s.id);
		return false;
	}

	const std::uintptr_t entry = imageBase + rva;
	if (entry & 7)
	{
		why = std::string(s.name) + ": entry is not 8-byte aligned";
		return false;
	}
	if (!IsExecutable(entry, 16))
	{
		why = std::string(s.name) + ": entry is not committed executable memory";
		return false;
	}

	const std::uint8_t * b = reinterpret_cast<const std::uint8_t *>(entry);
	s.entry = entry;

	if (s.vanillaLen && std::memcmp(b, s.vanilla, s.vanillaLen) == 0)
	{
		const bool trampolineOk = s.stolen >= kEntryJumpLen && s.stolen <= s.vanillaLen;
		const bool skipOk = s.stolen == 0 && s.skip >= kEntryJumpLen && s.skip <= s.vanillaLen;
		if (!trampolineOk && !skipOk)
		{
			why = std::string(s.name) + ": site table has no safe way to call the vanilla code";
			return false;
		}
		s.kind = EntryKind::Vanilla;
		s.owner = "vanilla";
		return true;
	}

	if (b[0] == 0xff && b[1] == 0x25 && b[2] == 0 && b[3] == 0 && b[4] == 0 && b[5] == 0)
	{
		s.kind = EntryKind::AbsJump;
		std::memcpy(&s.jumpTarget, b + 6, 8);
	}
	else if (b[0] == 0xe9)
	{
		std::int32_t rel = 0;
		std::memcpy(&rel, b + 1, 4);
		s.kind = EntryKind::RelJump;
		s.jumpTarget = entry + kEntryJumpLen + static_cast<std::int64_t>(rel);
	}
	else if (b[0] == 0x48 && b[1] == 0xb8 && b[10] == 0xff && b[11] == 0xe0)
	{
		s.kind = EntryKind::MovJump;
		std::memcpy(&s.jumpTarget, b + 2, 8);
	}
	else
	{
		char buf[64];
		sprintf_s(buf, "%02x %02x %02x %02x %02x %02x", b[0], b[1], b[2], b[3], b[4], b[5]);
		why = std::string(s.name) + ": entry is neither vanilla nor a known jump (" + buf + ")";
		return false;
	}

	if (!IsExecutable(s.jumpTarget, 1))
	{
		why = std::string(s.name) + ": existing jump leads to non-executable memory";
		s.kind = EntryKind::Unknown;
		return false;
	}

	s.owner = OwnerOf(s.jumpTarget);
	return true;
}

bool Commit(Site * sites, std::size_t count, void ** prevOut, std::string & why)
{
	if (!count)
		return true;

	for (std::size_t i = 0; i < count; i++)
	{
		if (sites[i].kind == EntryKind::Unknown || !sites[i].entry || !sites[i].hook)
		{
			why = std::string(sites[i].name ? sites[i].name : "?") + ": not prepared";
			return false;
		}
	}

	std::uint8_t * block = AllocNear(sites[0].entry, count * kSlotSize);
	if (!block)
	{
		why = "no free memory within jump reach of the exe";
		return false;
	}

	for (std::size_t i = 0; i < count; i++)
	{
		Site & s = sites[i];
		std::uint8_t * slot = block + i * kSlotSize;
		const std::uintptr_t stub = reinterpret_cast<std::uintptr_t>(slot);
		if (!RelReach(s.entry + kEntryJumpLen, stub))
		{
			VirtualFree(block, 0, MEM_RELEASE);
			why = std::string(s.name) + ": stub out of rel32 reach";
			return false;
		}

		// slot + 0: jump to our hook.
		WriteAbsJump(slot, reinterpret_cast<std::uintptr_t>(s.hook));

		// slot + 16: the trampoline, when the vanilla entry needs one.
		void * prev = nullptr;
		switch (s.kind)
		{
		case EntryKind::Vanilla:
			if (s.stolen)
			{
				std::uint8_t * tramp = slot + 16;
				std::memcpy(tramp, reinterpret_cast<const void *>(s.entry), s.stolen);
				WriteAbsJump(tramp + s.stolen, s.entry + s.stolen);
				prev = tramp;
			}
			else
			{
				prev = reinterpret_cast<void *>(s.entry + s.skip);
			}
			break;
		default:
			prev = reinterpret_cast<void *>(s.jumpTarget);
			break;
		}
		prevOut[i] = prev;
		s.stub = stub;
	}

	DWORD oldProtect = 0;
	if (!VirtualProtect(block, count * kSlotSize, PAGE_EXECUTE_READ, &oldProtect))
	{
		VirtualFree(block, 0, MEM_RELEASE);
		why = "could not make the stub block executable";
		return false;
	}
	FlushInstructionCache(GetCurrentProcess(), block, count * kSlotSize);

	for (std::size_t i = 0; i < count; i++)
	{
		if (!WriteEntryJump(sites[i].entry, sites[i].stub))
		{
			// sites before i are live and stay live: their stubs are valid,
			// and prevOut already routes everything they don't own.
			why = std::string(sites[i].name) + ": VirtualProtect on the entry failed";
			return false;
		}
	}
	return true;
}

bool StillOurs(const Site & s)
{
	if (!s.entry || !s.stub)
		return false;
	const std::uint8_t * b = reinterpret_cast<const std::uint8_t *>(s.entry);
	if (b[0] != 0xe9)
		return false;
	std::int32_t rel = 0;
	std::memcpy(&rel, b + 1, 4);
	return s.entry + kEntryJumpLen + static_cast<std::int64_t>(rel) == s.stub;
}

} // namespace alloc_patch
