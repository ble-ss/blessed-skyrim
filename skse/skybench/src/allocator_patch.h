// allocator_patch - points a game function's entry at ours and keeps a way to call whatever it ran before.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace alloc_patch {

// what a function entry looks like before we take it.
enum class EntryKind
{
	Unknown,
	Vanilla,	// the 1.7.104 bytes, untouched
	AbsJump,	// ff 25 00 00 00 00 <abs64>   (commonlib replace_func, engine fixes)
	RelJump,	// e9 <rel32>
	MovJump,	// 48 b8 <abs64> ff e0         (mov rax, imm; jmp rax)
};

const char * KindName(EntryKind k);

struct Site
{
	// set by the caller
	const char * name = nullptr;
	std::uint32_t id = 0;				// address library id
	std::uint8_t vanilla[16] = {};		// expected entry bytes on 1.7.104
	std::size_t vanillaLen = 0;			// how many of them must match
	std::size_t stolen = 0;				// vanilla only: position-independent bytes the trampoline re-runs
	std::size_t skip = 0;				// vanilla only, when stolen is 0: previous = entry + skip
	void * hook = nullptr;

	// set by Prepare
	std::uintptr_t entry = 0;
	EntryKind kind = EntryKind::Unknown;
	std::uintptr_t jumpTarget = 0;		// the target, for the jump kinds
	std::string owner;					// "vanilla", or the module that owns the jump target

	// set by Commit
	std::uintptr_t stub = 0;			// near stub the entry now jumps to
};

// reads the entry at image + rva and classifies it. false (with why) when
// the site can't be taken safely: unknown bytes, bad alignment, not code.
bool Prepare(Site & s, std::uintptr_t imageBase, std::uint32_t rva, std::string & why);

// patches every site in array order. prevOut[i] receives what the game
// would have called for site i before the patch: a trampoline (vanilla),
// entry + skip (vanilla null-check prologue), or the old jump target.
// prevOut is filled before any entry is written. on failure nothing is
// written.
bool Commit(Site * sites, std::size_t count, void ** prevOut, std::string & why);

// true when the entry still jumps to our stub (nobody patched over us).
bool StillOurs(const Site & s);

} // namespace alloc_patch
