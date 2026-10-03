// address_library - reads the format 5 versionlib bin (see address_library.h).

#include "address_library.h"

#include <cstdio>

namespace {

const long kTableOffset = 0x60;

std::string ExeDir()
{
	char buf[MAX_PATH] = {};
	DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
	if (n == 0 || n >= MAX_PATH)
		return std::string();
	std::string path(buf, n);
	size_t slash = path.find_last_of("\\/");
	return slash == std::string::npos ? std::string() : path.substr(0, slash);
}

#pragma pack(push, 1)
struct Header
{
	std::int32_t format;
	std::int32_t version[4];
	char name[64];
	std::int32_t pointerSize;
	std::int32_t zero;
	std::uint32_t count;
};
#pragma pack(pop)
static_assert(sizeof(Header) == kTableOffset, "versionlib format 5 header is 0x60 bytes");

} // namespace

bool AddressLibrary::Open(int major, int minor, int build)
{
	m_count = 0;
	m_error.clear();

	char name[96];
	sprintf_s(name, "\\Data\\SKSE\\Plugins\\versionlib-%d-%d-%d-0.bin", major, minor, build);
	m_path = ExeDir() + name;

	FILE * f = nullptr;
	if (fopen_s(&f, m_path.c_str(), "rb") != 0 || !f)
	{
		m_error = "cannot open " + m_path;
		return false;
	}

	Header h = {};
	bool readOk = fread(&h, sizeof(h), 1, f) == 1;
	fseek(f, 0, SEEK_END);
	long size = ftell(f);
	fclose(f);

	if (!readOk)
	{
		m_error = "short header";
		return false;
	}
	if (h.format != 5)
	{
		m_error = "unsupported format " + std::to_string(h.format) + " (only 5 is read)";
		return false;
	}
	if (h.version[0] != major || h.version[1] != minor || h.version[2] != build || h.version[3] != 0)
	{
		m_error = "version mismatch: bin is " + std::to_string(h.version[0]) + "." + std::to_string(h.version[1]) + "." +
			std::to_string(h.version[2]) + "." + std::to_string(h.version[3]);
		return false;
	}
	if (h.pointerSize != 8)
	{
		m_error = "pointer size " + std::to_string(h.pointerSize);
		return false;
	}
	if (size != kTableOffset + static_cast<long>(h.count) * 4)
	{
		m_error = "size " + std::to_string(size) + " does not match count " + std::to_string(h.count);
		return false;
	}

	m_count = h.count;
	return true;
}

std::uint32_t AddressLibrary::Rva(std::uint32_t id) const
{
	if (id == 0 || id >= m_count)
		return 0;

	FILE * f = nullptr;
	if (fopen_s(&f, m_path.c_str(), "rb") != 0 || !f)
		return 0;

	std::uint32_t rva = 0;
	if (fseek(f, kTableOffset + static_cast<long>(id) * 4, SEEK_SET) != 0 || fread(&rva, 4, 1, f) != 1)
		rva = 0;
	fclose(f);
	return rva;
}
