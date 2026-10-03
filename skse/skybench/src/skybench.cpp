// skybench - a minimal skse plugin for the blessed skyrim harness.
//
// tells the harness what the game is doing (menu open/close, cell loaded,
// save loaded) and can close a blocking menu (the AE "thanks for buying"
// modal that eats input at the main menu). built directly against skse
// 2.3.1's own source for 1.7.104 - no commonlibsse-ng. the one engine hook
// (cascade_cull.cpp, SKYBENCH_CULL_CASCADES) finds its address through the
// address library bin (address_library.cpp).

#include "skse64_common/Relocation.h"
#include "skse64_common/skse_version.h"
#include "skse64/PluginAPI.h"
#include "skse64/GameTypes.h"
#include "skse64/GameEvents.h"
#include "skse64/GameForms.h"
#include "skse64/GameMenus.h"
#include "skse64/gamethreads.h"

#include "allocator.h"
#include "cascade_census.h"
#include "cascade_halfrate.h"	// blessed: SKYBENCH_CASCADE_HALFRATE
#include "cascade_cull.h"
#include "halfrate_switch.h"
#include "havok_step.h"
#include "water_halfrate.h"
#include "occlusion_probe.h"
#include "point_shadow_cull.h"
#include "frame_timer.h"
#include "precip_gate.h"
#include "skybench_log.h"

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

namespace {

// ---- logging -----------------------------------------------------------

std::mutex g_logMutex;
std::string g_logPath;
std::string g_eventsPath;
bool g_eventsEnabled = false;
std::chrono::steady_clock::time_point g_loadTime;

std::string GetSkseDocsDir()
{
	char * userProfile = nullptr;
	size_t len = 0;
	std::string result;
	if (_dupenv_s(&userProfile, &len, "USERPROFILE") == 0 && userProfile)
	{
		result = userProfile;
		result += "\\Documents\\My Games\\Skyrim Special Edition\\SKSE";
		free(userProfile);
	}
	return result;
}

void EnsureDir(const std::string & path)
{
	CreateDirectoryA(path.c_str(), nullptr);
}

double SecondsSinceLoad()
{
	return std::chrono::duration<double>(std::chrono::steady_clock::now() - g_loadTime).count();
}

void AppendLine(const std::string & path, const std::string & line)
{
	std::lock_guard<std::mutex> lock(g_logMutex);
	std::ofstream f(path, std::ios::app);
	if (!f.is_open())
		return;
	f << line << "\n";
	f.flush();
}

void Log(const std::string & msg)
{
	if (g_logPath.empty())
		return;
	char buf[64];
	SYSTEMTIME st;
	GetLocalTime(&st);
	sprintf_s(buf, "[%02u:%02u:%02u.%03u] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
	AppendLine(g_logPath, buf + msg);
}

// minimal json string escaping - only quotes and backslashes are expected
// in practice (menu names, editor ids), but this is cheap insurance.
std::string JsonEscape(const std::string & s)
{
	std::string out;
	out.reserve(s.size() + 2);
	for (char c : s)
	{
		if (c == '"' || c == '\\')
			out += '\\';
		out += c;
	}
	return out;
}

// builds one json-lines event: {"t": <seconds>, "ev": "<name>", ...fields}
// fields is a pre-built, already-comma-prefixed fragment (or empty).
void LogEvent(const std::string & ev, const std::string & fields = std::string())
{
	// always goes to the main log too, so a plain read of skybench.log
	// tells the harness what happened even without SKYBENCH_EVENTS set.
	Log("event " + ev + fields);

	if (!g_eventsEnabled || g_eventsPath.empty())
		return;

	char tbuf[32];
	sprintf_s(tbuf, "%.3f", SecondsSinceLoad());

	std::string line = "{\"t\": ";
	line += tbuf;
	line += ", \"ev\": \"";
	line += JsonEscape(ev);
	line += "\"";
	line += fields;
	line += "}";

	AppendLine(g_eventsPath, line);
}

// ---- skse interfaces -----------------------------------------------------

PluginHandle g_pluginHandle = kPluginHandle_Invalid;
SKSETaskInterface * g_task = nullptr;
bool g_runtimeOk = false;

// ---- SKYBENCH_CLOSE_MENUS config -----------------------------------------

std::vector<std::string> g_closeMenus;
bool g_closeAll = false;

const char * const kMainMenuExempt[] = {
	"Main Menu", "Cursor Menu", "Console", "Loading Menu", "Fader Menu", "HUD Menu"
};

bool IsExempt(const std::string & name)
{
	for (auto & exempt : kMainMenuExempt)
	{
		if (name == exempt)
			return true;
	}
	return false;
}

void LoadCloseMenuConfig()
{
	char * val = nullptr;
	size_t len = 0;
	if (_dupenv_s(&val, &len, "SKYBENCH_CLOSE_MENUS") != 0 || !val)
		return;

	std::string spec(val);
	free(val);

	if (spec == "*")
	{
		g_closeAll = true;
		return;
	}

	size_t start = 0;
	while (start <= spec.size())
	{
		size_t comma = spec.find(',', start);
		std::string tok = spec.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
		if (!tok.empty())
			g_closeMenus.push_back(tok);
		if (comma == std::string::npos)
			break;
		start = comma + 1;
	}
}

bool ShouldCloseMenu(const std::string & name)
{
	if (g_closeAll)
		return !IsExempt(name);

	for (auto & m : g_closeMenus)
	{
		if (m == name)
			return true;
	}
	return false;
}

// ---- deferred menu close (must run from the skse task interface, not from
// inside the event sink - see UIManager::AddMessage in GameMenus.h) --------

class CloseMenuTask : public TaskDelegate
{
public:
	explicit CloseMenuTask(std::string name) : m_name(std::move(name)) {}

	void Run() override
	{
		UIManager * ui = UIManager::GetSingleton();
		if (!ui)
		{
			Log("menu_close: UIManager singleton is null, dropped for " + m_name);
			return;
		}

		// the engine's ui message ids: 3 = hide (skse's kMessage_Close),
		// 4 = force hide. SKYBENCH_CLOSE_MSG overrides, default 4, because
		// the ae "download" message box ignores a plain hide (plugin-menu-2).
		UInt32 msg = 4;
		char * v = nullptr;
		size_t n = 0;
		if (_dupenv_s(&v, &n, "SKYBENCH_CLOSE_MSG") == 0 && v)
		{
			msg = (UInt32)strtoul(v, nullptr, 10);
			free(v);
		}

		StringCache::Ref ref(m_name.c_str());
		CALL_MEMBER_FN(ui, AddMessage)(&ref, msg, nullptr);

		std::string fields = ", \"name\": \"" + JsonEscape(m_name) + "\", \"msg\": " + std::to_string(msg);
		LogEvent("menu_close_sent", fields);
	}

	void Dispose() override
	{
		delete this;
	}

private:
	std::string m_name;
};

void QueueMenuClose(const std::string & name)
{
	if (!g_task)
	{
		Log("menu_close: no task interface, dropped for " + name);
		return;
	}
	g_task->AddTask(new CloseMenuTask(name));
}

// ---- event sinks -----------------------------------------------------

class MenuOpenCloseSink : public BSTEventSink<MenuOpenCloseEvent>
{
public:
	EventResult ReceiveEvent(MenuOpenCloseEvent * evn, EventDispatcher<MenuOpenCloseEvent> *) override
	{
		if (!evn)
			return kEvent_Continue;

		std::string name = evn->menuName.c_str() ? evn->menuName.c_str() : "";
		std::string fields = ", \"name\": \"" + JsonEscape(name) + "\", \"open\": " + (evn->opening ? "true" : "false");
		LogEvent("menu", fields);

		if (evn->opening && ShouldCloseMenu(name))
			QueueMenuClose(name);

		return kEvent_Continue;
	}
};

class CellLoadedSink : public BSTEventSink<TESCellFullyLoadedEvent>
{
public:
	EventResult ReceiveEvent(TESCellFullyLoadedEvent * evn, EventDispatcher<TESCellFullyLoadedEvent> *) override
	{
		char buf[32] = "null";
		if (evn && evn->cell)
			sprintf_s(buf, "%u", evn->cell->formID);

		// editor ids aren't cheaply available from this header set without
		// pulling in a lot more of the form lookup machinery - see report.
		std::string fields = std::string(", \"form_id\": ") + (evn && evn->cell ? buf : "null");
		LogEvent("cell_loaded", fields);
		return kEvent_Continue;
	}
};

class LoadGameSink : public BSTEventSink<TESLoadGameEvent>
{
public:
	EventResult ReceiveEvent(TESLoadGameEvent *, EventDispatcher<TESLoadGameEvent> *) override
	{
		LogEvent("load_game_event");
		return kEvent_Continue;
	}
};

MenuOpenCloseSink g_menuSink;
CellLoadedSink g_cellSink;
LoadGameSink g_loadGameSink;

void RegisterSinks()
{
	if (!g_runtimeOk)
		return;

	MenuManager * menuMgr = MenuManager::GetSingleton();
	if (menuMgr)
		menuMgr->MenuOpenCloseEventDispatcher()->AddEventSink(&g_menuSink);
	else
		Log("RegisterSinks: MenuManager singleton is null");

	EventDispatcherList * dispatchers = GetEventDispatcherList();
	if (dispatchers)
	{
		dispatchers->unk210.AddEventSink(&g_cellSink);				// TESCellFullyLoadedEvent
		dispatchers->loadGameEventDispatcher.AddEventSink(&g_loadGameSink);
	}
	else
	{
		Log("RegisterSinks: GetEventDispatcherList() is null");
	}

	Log("sinks registered (menu, cell_loaded, load_game_event)");
}

// ---- skse messaging ----------------------------------------------------

void MessageHandler(SKSEMessagingInterface::Message * msg)
{
	if (!msg)
		return;

	switch (msg->type)
	{
	case SKSEMessagingInterface::kMessage_PostLoad:
		LogEvent("post_load");
		break;
	case SKSEMessagingInterface::kMessage_InputLoaded:
		LogEvent("input_loaded");
		break;
	case SKSEMessagingInterface::kMessage_DataLoaded:
		LogEvent("data_loaded");
		RegisterSinks();
		break;
	case SKSEMessagingInterface::kMessage_PostLoadGame:
		{
			bool success = msg->data != nullptr;
			LogEvent("post_load_game", success ? ", \"success\": true" : ", \"success\": false");
		}
		break;
	case SKSEMessagingInterface::kMessage_NewGame:
		LogEvent("new_game");
		break;
	default:
		break;
	}
}

} // namespace

// ---- log bridge for the other source files (see skybench_log.h) --------

void skybench_Log(const std::string & msg)
{
	Log(msg);
}

void skybench_LogEvent(const std::string & ev, const std::string & fields)
{
	LogEvent(ev, fields);
}

// ---- forced debug-log stand-in (see skse_compat.h) --------------------

void skybench_DebugMessage(const char * fmt, ...)
{
	char buf[512];
	va_list args;
	va_start(args, fmt);
	vsnprintf(buf, sizeof(buf), fmt, args);
	va_end(args);
	buf[sizeof(buf) - 1] = '\0';
	Log(std::string("[skse] ") + buf);
}

// ---- plugin entry points ------------------------------------------------

extern "C" {

__declspec(dllexport) SKSEPluginVersionData SKSEPlugin_Version =
{
	SKSEPluginVersionData::kVersion,

	1,
	"skybench",

	"blessed skyrim",
	"",

	0,	// versionIndependenceEx
	0,	// versionIndependence - we use hardcoded 1.7.104 addresses, not signatures
	{ RUNTIME_VERSION_1_7_104, 0 },

	0,	// seVersionRequired
};

__declspec(dllexport) bool SKSEPlugin_Load(const SKSEInterface * skse)
{
	g_loadTime = std::chrono::steady_clock::now();

	std::string docsDir = GetSkseDocsDir();
	if (!docsDir.empty())
	{
		EnsureDir(docsDir);
		g_logPath = docsDir + "\\skybench.log";
	}

	char * eventsVal = nullptr;
	size_t eventsLen = 0;
	if (_dupenv_s(&eventsVal, &eventsLen, "SKYBENCH_EVENTS") == 0 && eventsVal)
	{
		g_eventsEnabled = true;
		// "1" keeps events in skybench.log; anything else is a file path
		// (the harness points it into the run folder).
		g_eventsPath = (std::string(eventsVal) == "1") ? g_logPath : std::string(eventsVal);
		free(eventsVal);
	}

	LoadCloseMenuConfig();

	Log("skybench loading. skse=" + std::to_string(skse->skseVersion) +
		" runtime=" + std::to_string(skse->runtimeVersion) +
		" editor=" + std::to_string(skse->isEditor));

	g_runtimeOk = (skse->runtimeVersion == RUNTIME_VERSION_1_7_104);
	if (!g_runtimeOk)
	{
		Log("runtime version is not 1.7.104 - skybench will log messaging events only, no hooks/sinks/menu-close");
	}

	g_pluginHandle = skse->GetPluginHandle();

	auto * messaging = static_cast<SKSEMessagingInterface *>(skse->QueryInterface(kInterface_Messaging));
	if (messaging)
		messaging->RegisterListener(g_pluginHandle, "SKSE", MessageHandler);
	else
		Log("no messaging interface");

	g_task = static_cast<SKSETaskInterface *>(skse->QueryInterface(kInterface_Task));
	if (!g_task)
		Log("no task interface - menu-close requests will be dropped");

	// SKYBENCH_CULL_CASCADES: patch at load, before community shaders'
	// post-post-load fixes hook the same call and chain over ours.
	if (g_runtimeOk)
		InstallAllocator(skse, g_pluginHandle);	// blessed: SKYBENCH_ALLOCATOR
	if (g_runtimeOk)
		InstallCascadeCull(skse, g_pluginHandle);
	if (g_runtimeOk)
		InstallCascadeCensus(skse, g_pluginHandle);	// blessed: SKYBENCH_CASCADE_CENSUS
	if (g_runtimeOk)
		InstallCascadeHalfrate(skse, g_pluginHandle);	// blessed: SKYBENCH_CASCADE_HALFRATE
	if (g_runtimeOk)
		InstallPointShadowCull(skse, g_pluginHandle);
	if (g_runtimeOk)
		InstallOcclusionProbe(skse, g_pluginHandle);	// blessed: SKYBENCH_OCCLUSION_PROBE
	if (g_runtimeOk)
		InstallPrecipGate(skse, g_pluginHandle);	// blessed: SKYBENCH_PRECIP_OCCLUSION
	if (g_runtimeOk)
		InstallHavokStep(skse, g_pluginHandle);	// blessed: SKYBENCH_HAVOK
	if (g_runtimeOk)
		InstallWaterHalfrate(skse, g_pluginHandle);	// blessed: SKYBENCH_WATER_HALFRATE
	if (g_runtimeOk)
		InstallFrameTimer(skse, g_pluginHandle);	// blessed: SKYBENCH_FRAME_TIMER

	// the half-rate far field switch (F8, [HalfRate] in skybench.ini). hooks
	// nothing in the game, so it runs on any runtime.
	InstallHalfRateSwitch();

	if (g_closeAll)
		Log("SKYBENCH_CLOSE_MENUS=* - will close any menu at boot except the main-menu exemptions");
	else if (!g_closeMenus.empty())
	{
		std::string list;
		for (auto & m : g_closeMenus)
		{
			if (!list.empty()) list += ", ";
			list += m;
		}
		Log("SKYBENCH_CLOSE_MENUS=" + list);
	}

	Log("skybench loaded");
	return true;
}

} // extern "C"
