// NetTune - ArkApi (ARK: Survival Evolved) server plugin.
//
// Speeds up delivering the world to a client (large bases first) by tuning the two
// server-side values that were measured to matter on v361.
//
// Findings (from the v361 binary and from live out-and-back base reloads):
//
// * UPlayer::CurrentNetSpeed is the budget for NEW actors, not a bytes/second cap.
//   In UNetDriver::ServerReplicateActors every newly relevant actor adds
//     DormancyRate += (1.0 / (f*195 + 30)) / DeltaSeconds / GDormancyLimitMultiplier,
//   where f = clamp((CurrentNetSpeed - 14000) / 46000, 0, 1), and DormancyRate decays
//   by 1.0 per tick. New actors per second is therefore about
//     (f*195 + 30) * GDormancyLimitMultiplier
//   i.e. ~30/s at 14000 and ~225/s at 60000. Above 60000 nothing changes: f saturates.
//   The value is what the client asks for (NMT_Netspeed, derived from its ClientNetQuality),
//   so a "Low" client gets 14000 and loads bases ~7.5x slower.
//
// * UNetDriver::NetServerMaxTickRate is the server frame rate: UGameEngine::GetMaxTickRate
//   reads it (clamped to [10, 120]); r.MaxFPS overrides it. It does not change the new-actor
//   rate (it cancels out of the formula above) - it trades CPU for granularity.
//
// * GDormancyLimitMultiplier (global, default 1.0) divides the increment above, so it scales
//   the new-actor rate linearly and is the only lever beyond 60000.
//
// Observed on a live server (peakDorm is the increment seen during a base reload):
//   tick20 netspeed14000 mult1 -> 0.676   (predicted 0.667)
//   tick20 netspeed60000 mult1 -> 0.076   (predicted 0.089)
//   tick20 netspeed60000 mult3 -> 0.025   (predicted 0.030)
//   tick40 netspeed60000 mult1 -> 0.180   (predicted 0.178)
//
// Deliberately not used (measured to have no effect): UNetDriver::MaxClientRate /
// MaxInternetClientRate (read only by UWorld::Listen, which forces 60000),
// AGameNetworkManager bandwidth fields, per-actor NetCullDistanceSquared / NetUpdateFrequency.
//
// Values are written only when they differ, so in a steady state the plugin writes nothing.
// They are re-checked periodically because the engine can still reset them (UWorld::Listen
// resets MaxClientRate on a map load, and a client can resend NMT_Netspeed).
//
// Config: Configs/config.json (hot-reloadable via RCON NetTune.Reload).

#include <API/ARK/Ark.h>
#include <Logger.h>
#include <Utils.h>
#include <Config.h>
#include <json.hpp>

#include <string>

#pragma comment(lib, "ArkApi.lib")

namespace
{
	constexpr char kPluginName[] = "NetTune";

	struct PluginConfig
	{
		bool debug = false;
		int net_server_max_tick_rate = 20;       // 0 = leave unchanged
		int current_net_speed = 60000;           // 0 = leave unchanged
		float dormancy_limit_multiplier = 0.0f;  // 0 = leave unchanged
		float reconcile_interval_seconds = 15.0f;
		float status_interval_seconds = 60.0f;   // logged at debug level
	};

	PluginConfig g_config;

	unsigned long long g_ticks = 0;
	float g_tick_window_seconds = 0.0f;
	unsigned long long g_tick_window_count = 0;
	float g_observed_fps = 0.0f;
	float g_since_reconcile = 0.0f;
	float g_since_status = 0.0f;

	// --- field access -------------------------------------------------------------------
	// ArkApi::GetAddress(base, name) returns 0 for a missing symbol, and returns base
	// itself for an offset of 0 - both are treated as "not found".
	template <typename T>
	T* SafeField(void* object, const char* name)
	{
		if (!object) {
			return nullptr;
		}

		T* field = GetNativePointerField<T*>(object, name);
		if (!field || reinterpret_cast<char*>(field) == reinterpret_cast<char*>(object)) {
			return nullptr;
		}

		return field;
	}

	// Global data symbols live in the ArkApi offsets map under "Global.<name>".
	template <typename T>
	T* SafeGlobal(const char* name)
	{
		return GetNativeDataPointerField<T*>(name);
	}

	// Writes only when the value differs; returns 1 if it wrote.
	template <typename T>
	int SetIfDiff(T* field, const T& value)
	{
		if (!field || *field == value) {
			return 0;
		}

		*field = value;
		return 1;
	}

	template <typename T>
	bool ReadField(void* object, const char* name, T& out)
	{
		T* field = SafeField<T>(object, name);
		if (!field) {
			return false;
		}

		out = *field;
		return true;
	}

	// --- apply --------------------------------------------------------------------------

	int ReconcileDriver(UNetDriver* driver)
	{
		int changed = 0;

		if (g_config.net_server_max_tick_rate > 0) {
			changed += SetIfDiff(SafeField<int>(driver, "UNetDriver.NetServerMaxTickRate"),
				g_config.net_server_max_tick_rate);
		}

		return changed;
	}

	int ReconcileGlobals()
	{
		int changed = 0;

		if (g_config.dormancy_limit_multiplier > 0.0f) {
			changed += SetIfDiff(SafeGlobal<float>("Global.GDormancyLimitMultiplier"),
				g_config.dormancy_limit_multiplier);
		}

		return changed;
	}

	int ReconcileConnections(UNetDriver* driver)
	{
		int changed = 0;

		for (UNetConnection* connection : driver->ClientConnectionsField()) {
			if (!connection) {
				continue;
			}

			if (g_config.current_net_speed > 0) {
				changed += SetIfDiff(SafeField<int>(connection, "UPlayer.CurrentNetSpeed"),
					g_config.current_net_speed);
			}
		}

		return changed;
	}

	void Reconcile()
	{
		UWorld* world = ArkApi::GetApiUtils().GetWorld();
		if (!world) {
			return;
		}

		UNetDriver* driver = world->NetDriverField();
		if (!driver) {
			return;
		}

		const int driver_changed = ReconcileDriver(driver);
		const int connections_changed = ReconcileConnections(driver);
		const int globals_changed = ReconcileGlobals();

		if (driver_changed + connections_changed + globals_changed > 0) {
			LOG_INFO("applied driver={} connections={} globals={}",
				driver_changed, connections_changed, globals_changed);
		}
	}

	// --- reporting ----------------------------------------------------------------------

	void LogStatus(const char* reason)
	{
		UWorld* world = ArkApi::GetApiUtils().GetWorld();
		UNetDriver* driver = world ? world->NetDriverField() : nullptr;
		if (!driver) {
			LOG_INFO("status({}): no NetDriver", reason);
			return;
		}

		int tick_rate = 0;
		ReadField(driver, "UNetDriver.NetServerMaxTickRate", tick_rate);

		float dormancy_multiplier = 0.0f;
		if (float* value = SafeGlobal<float>("Global.GDormancyLimitMultiplier")) {
			dormancy_multiplier = *value;
		}

		LOG_INFO("status({}): tickrate={} observedFps={} dormancyMult={}",
			reason, tick_rate, g_observed_fps, dormancy_multiplier);

		int index = 0;
		for (UNetConnection* connection : driver->ClientConnectionsField()) {
			if (!connection) {
				continue;
			}

			int net_speed = 0, queued_bytes = 0;
			unsigned int out_bytes = 0;
			float dormancy = 0.0f;
			ReadField(connection, "UPlayer.CurrentNetSpeed", net_speed);
			ReadField(connection, "UNetConnection.QueuedBytes", queued_bytes);
			ReadField(connection, "UNetConnection.OutBytes", out_bytes);
			ReadField(connection, "UNetConnection.DormancyRate", dormancy);

			LOG_INFO("status({}): conn[{}] ip='{}' netspeed={} queued={} outBps={} dorm={}",
				reason, index, connection->ClientGivenIPField().ToString(),
				net_speed, queued_bytes, out_bytes, dormancy);
			++index;
		}
	}

	void Tick(float delta_seconds)
	{
		++g_ticks;

		g_tick_window_seconds += delta_seconds;
		++g_tick_window_count;
		if (g_tick_window_seconds >= 1.0f) {
			g_observed_fps = static_cast<float>(g_tick_window_count) / g_tick_window_seconds;
			g_tick_window_seconds = 0.0f;
			g_tick_window_count = 0;
		}

		g_since_reconcile += delta_seconds;
		g_since_status += delta_seconds;

		if (g_since_reconcile >= g_config.reconcile_interval_seconds) {
			g_since_reconcile = 0.0f;
			Reconcile();
		}

		if (g_config.status_interval_seconds > 0.0f &&
			g_since_status >= g_config.status_interval_seconds) {
			g_since_status = 0.0f;
			LogStatus("periodic");
		}
	}

	// --- config -------------------------------------------------------------------------

	void ApplyConfig(const nlohmann::json& json)
	{
		g_config.debug = json.value("Debug", g_config.debug);
		g_config.net_server_max_tick_rate = json.value("NetServerMaxTickRate", g_config.net_server_max_tick_rate);
		g_config.current_net_speed = json.value("CurrentNetSpeed", g_config.current_net_speed);
		g_config.dormancy_limit_multiplier = json.value("DormancyLimitMultiplier", g_config.dormancy_limit_multiplier);
		g_config.reconcile_interval_seconds = json.value("ReconcileIntervalSeconds", g_config.reconcile_interval_seconds);
		g_config.status_interval_seconds = json.value("StatusIntervalSeconds", g_config.status_interval_seconds);
	}

	void LoadConfig()
	{
		g_config = PluginConfig{};

		try {
			ApplyConfig(Config::Read(kPluginName));
		}
		catch (const std::exception& error) {
			LOG_WARN("config not loaded ({}): using defaults", error.what());
		}

		LOG_SET_LEVEL(g_config.debug ? LOG_LEVEL_DEBUG : LOG_LEVEL_INFO);
	}

	void CmdReload(RCONClientConnection*, RCONPacket*, UWorld*)
	{
		LoadConfig();
		LOG_INFO("reloaded: tickrate={} currentNetSpeed={} dormancyMult={}",
			g_config.net_server_max_tick_rate, g_config.current_net_speed,
			g_config.dormancy_limit_multiplier);
		Reconcile();
	}

	void CmdStatus(RCONClientConnection*, RCONPacket*, UWorld*)
	{
		LogStatus("rcon");
	}
}

extern "C" __declspec(dllexport) void Plugin_Init()
{
	LOG_INIT(kPluginName);

	LoadConfig();

	ArkApi::GetCommands().AddOnTickCallback("nettune_tick", &Tick);
	ArkApi::GetCommands().AddRconCommand("NetTune.Reload", &CmdReload);
	ArkApi::GetCommands().AddRconCommand("NetTune.Status", &CmdStatus);

	Reconcile();

	LOG_INFO("loaded: tickrate={} currentNetSpeed={} dormancyMult={}",
		g_config.net_server_max_tick_rate, g_config.current_net_speed,
		g_config.dormancy_limit_multiplier);
}

extern "C" __declspec(dllexport) void Plugin_Unload()
{
	ArkApi::GetCommands().RemoveOnTickCallback("nettune_tick");
	ArkApi::GetCommands().RemoveRconCommand("NetTune.Reload");
	ArkApi::GetCommands().RemoveRconCommand("NetTune.Status");
}
