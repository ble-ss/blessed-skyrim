// halfrate_switch - the in-game switch for the fork's half-rate far field (a hotkey and an ini line).

#include "halfrate_switch.h"
#include "skybench_log.h"

#include <windows.h>

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

namespace {

// keep in step with the fork's src/d3d11/blessed_halfrate.cpp
struct HalfRateShared
{
	unsigned int	magic;
	unsigned int	version;
	volatile LONG	enabled;
	volatile LONG	toggles;
};

const unsigned int kMagic = 0x46524842;	// "BHRF"
const unsigned int kVersion = 1;
const wchar_t * kSharedName = L"Local\\BlessedHalfRate";

HANDLE g_mapping = nullptr;
HalfRateShared * g_shared = nullptr;
int g_toggleKey = VK_F8;
std::string g_toggleKeyName = "F8";

std::string PluginIniPath()
{
	HMODULE self = nullptr;
	if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			reinterpret_cast<LPCSTR>(&InstallHalfRateSwitch), &self))
		return std::string();

	char path[MAX_PATH] = {};
	DWORD len = GetModuleFileNameA(self, path, MAX_PATH);
	if (len == 0 || len >= MAX_PATH)
		return std::string();

	std::string p(path, len);
	size_t slash = p.find_last_of("\\/");
	return (slash == std::string::npos ? std::string() : p.substr(0, slash + 1)) + "skybench.ini";
}

// "F8", "f8", "K", "7", "0x77", "119" -> a virtual-key code, or 0
int ParseKey(const std::string & raw)
{
	std::string s;
	for (char c : raw)
		if (!isspace(static_cast<unsigned char>(c)))
			s += static_cast<char>(toupper(static_cast<unsigned char>(c)));

	if (s.empty())
		return 0;

	if (s.size() > 2 && s[0] == '0' && s[1] == 'X')
		return static_cast<int>(strtol(s.c_str() + 2, nullptr, 16)) & 0xff;

	if (s[0] == 'F' && s.size() >= 2 && s.size() <= 3 && isdigit(static_cast<unsigned char>(s[1])))
	{
		int n = atoi(s.c_str() + 1);
		if (n >= 1 && n <= 24)
			return VK_F1 + (n - 1);
		return 0;
	}

	if (s.size() == 1 && (isalpha(static_cast<unsigned char>(s[0])) || isdigit(static_cast<unsigned char>(s[0]))))
		return s[0];	// 'A'-'Z' and '0'-'9' are their own virtual-key codes

	bool allDigits = true;
	for (char c : s)
		allDigits = allDigits && isdigit(static_cast<unsigned char>(c));
	if (allDigits)
		return atoi(s.c_str()) & 0xff;

	return 0;
}

bool GameHasFocus()
{
	HWND fg = GetForegroundWindow();
	if (!fg)
		return false;
	DWORD pid = 0;
	GetWindowThreadProcessId(fg, &pid);
	return pid == GetCurrentProcessId();
}

void LogState(const char * why)
{
	bool on = g_shared && g_shared->enabled != 0;
	long toggles = g_shared ? g_shared->toggles : 0;
	skybench_LogEvent("halfrate", std::string(", \"enabled\": ") + (on ? "true" : "false") +
		", \"why\": \"" + why + "\", \"key\": \"" + g_toggleKeyName + "\", \"toggles\": " + std::to_string(toggles));
}

void WatchKey()
{
	bool wasDown = false;

	for (;;)
	{
		Sleep(30);

		bool down = (GetAsyncKeyState(g_toggleKey) & 0x8000) != 0;

		if (down && !wasDown && GameHasFocus())
		{
			LONG now = g_shared->enabled ? 0 : 1;
			InterlockedExchange(&g_shared->enabled, now);
			InterlockedIncrement(&g_shared->toggles);
			LogState("key");
		}

		wasDown = down;
	}
}

} // namespace

void InstallHalfRateSwitch()
{
	std::string ini = PluginIniPath();

	int enabled = 1;
	char keyBuf[64] = "F8";

	if (!ini.empty())
	{
		enabled = GetPrivateProfileIntA("HalfRate", "bEnabled", 1, ini.c_str()) != 0 ? 1 : 0;
		GetPrivateProfileStringA("HalfRate", "sToggleKey", "F8", keyBuf, sizeof(keyBuf), ini.c_str());
	}

	char * envVal = nullptr;
	size_t envLen = 0;
	if (_dupenv_s(&envVal, &envLen, "SKYBENCH_HALFRATE") == 0 && envVal)
	{
		enabled = std::string(envVal) == "0" ? 0 : 1;
		free(envVal);
	}

	int key = ParseKey(keyBuf);
	if (key)
	{
		g_toggleKey = key;
		g_toggleKeyName = keyBuf;
	}
	else
	{
		skybench_Log(std::string("halfrate: could not read sToggleKey '") + keyBuf + "', using F8");
	}

	g_mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
		static_cast<DWORD>(sizeof(HalfRateShared)), kSharedName);
	if (!g_mapping)
	{
		skybench_Log("halfrate: CreateFileMapping failed (" + std::to_string(GetLastError()) + "); the fork keeps its own default");
		return;
	}

	g_shared = static_cast<HalfRateShared *>(MapViewOfFile(g_mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(HalfRateShared)));
	if (!g_shared)
	{
		skybench_Log("halfrate: MapViewOfFile failed (" + std::to_string(GetLastError()) + "); the fork keeps its own default");
		CloseHandle(g_mapping);
		g_mapping = nullptr;
		return;
	}

	// enabled first, magic last: the fork only trusts the block once the
	// magic is there
	g_shared->version = kVersion;
	InterlockedExchange(&g_shared->enabled, enabled);
	InterlockedExchange(&g_shared->toggles, 0);
	MemoryBarrier();
	g_shared->magic = kMagic;

	skybench_Log("halfrate: ini " + (ini.empty() ? std::string("(none)") : ini) +
		", key " + g_toggleKeyName);
	LogState("load");

	std::thread(WatchKey).detach();
}
