// call_patch - checks an e8 call in the exe, then routes it through a thunk (skse branch-pool stub).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

struct SKSETrampolineInterface;

// one call site: the 5-byte e8 at siteRva, the bytes that must come just
// before it, and the rva the call must reach today.
struct CallSite
{
	const char * name;
	std::uint32_t siteRva;
	const std::uint8_t * preamble;
	std::size_t preambleLen;
	std::uint32_t targetRva;
};

// a checked site, ready to patch. fields is a comma-prefixed json fragment
// (site rva, the bytes found) for the events log.
struct CheckedCall
{
	bool ok = false;
	std::string reason;
	std::string fields;
	std::uintptr_t site = 0;
	std::uintptr_t target = 0;
};

// reads the site. ok only when the preamble matches, the byte is e8, and
// the call reaches targetRva (so a site another plugin already hooked is
// refused, never chained blind).
CheckedCall CheckCall(const CallSite & site);

// allocates a 14-byte absolute-jump stub to thunk from the skse branch
// pool and rewrites the call's rel32 to reach it. false (with reason) if
// any step fails; the site is left untouched then.
bool PatchCall(const CheckedCall & call, SKSETrampolineInterface * tramp, std::uint32_t pluginHandle,
	void * thunk, std::string & reason);

// "0x..." as a json string.
std::string JsonHex(std::uint64_t v);
