// address_library - resolves address library ids to rvas from the installed versionlib bin (format 5).
#pragma once

#include <cstdint>
#include <string>

// format 5 (the one shipped for 1.6.1130+ / 1.7.x) is a flat table:
//   0x00 int32 format (5)
//   0x04 int32 version[4]            1, 7, 104, 0
//   0x14 char  name[64]              "SkyrimSE.exe"
//   0x54 int32 pointer size          8
//   0x58 int32 (zero)
//   0x5C uint32 count
//   0x60 uint32 rva[count]           indexed by id, 0 = unknown
// the file is exactly 0x60 + 4 * count bytes. only the header and the ids
// asked for are read; nothing is kept in memory.
class AddressLibrary
{
public:
	// opens Data\SKSE\Plugins\versionlib-<a>-<b>-<c>-0.bin next to the
	// running exe and checks its header. on failure, error() says why.
	bool Open(int major, int minor, int build);

	// 0 when the id is out of range, unknown, or the file read failed.
	std::uint32_t Rva(std::uint32_t id) const;

	const std::string & path() const { return m_path; }
	const std::string & error() const { return m_error; }

private:
	std::string m_path;
	std::string m_error;
	std::uint32_t m_count = 0;
};
