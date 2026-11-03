// NetTune - ArkApi (ARK: Survival Evolved) server plugin.
// The tuned values, the mechanism behind them and the measurements are in README.md.

#include <API/ARK/Ark.h>
#include <Logger.h>
#include <Utils.h>
#include <Config.h>
#include <json.hpp>

#include <algorithm>
#include <string>
#include <unordered_map>

#pragma comment(lib, "ArkApi.lib")

namespace
{
	constexpr char kPluginName[] = "NetTune";

	struct PluginConfig
	{
		bool debug = false;
		int server_max_tick_rate = 20;
		int current_net_speed = 60000;
		float dormancy_limit_multiplier = 0.0f;
		float reconcile_interval_seconds = 15.0f;
		float status_interval_seconds = 60.0f;
		float bandwidth_budget_mbps = 0.0f;
	};

	struct ConnectionSample
	{
		int net_speed = 0;
		int queued_bytes = 0;
		unsigned int out_bps = 0;
		unsigned int packets_lost_in = 0;
		unsigned int packets_lost_out = 0;
		float dormancy_rate = 0.0f;
		float avg_lag_seconds = 0.0f;
	};

	struct ConnectionPeaks
	{
		unsigned int out_bps = 0;
		float dormancy_rate = 0.0f;
		unsigned long long last_seen_tick = 0;
	};

	PluginConfig g_config;
	float g_observed_fps = 0.0f;
	float g_fps_window_seconds = 0.0f;
	unsigned long long g_fps_window_ticks = 0;
	float g_seconds_since_reconcile = 0.0f;
	float g_seconds_since_status = 0.0f;
	unsigned long long g_tick_count = 0;
	std::unordered_map<UNetConnection*, ConnectionPeaks> g_peaks;

	template <typename T>
	T* Field(void* object, const char* name)
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

	template <typename T>
	T ReadField(void* object, const char* name)
	{
		T* field = Field<T>(object, name);
		return field ? *field : T{};
	}

	template <typename T>
	int WriteIfDifferent(T* field, const T& value)
	{
		if (!field || *field == value) {
			return 0;
		}

		*field = value;
		return 1;
	}

	UNetDriver* NetDriver()
	{
		UWorld* world = ArkApi::GetApiUtils().GetWorld();
		return world ? world->NetDriverField() : nullptr;
	}

	ConnectionSample ReadConnection(UNetConnection* connection)
	{
		ConnectionSample sample;
		sample.net_speed = ReadField<int>(connection, "UPlayer.CurrentNetSpeed");
		sample.queued_bytes = ReadField<int>(connection, "UNetConnection.QueuedBytes");
		sample.out_bps = ReadField<unsigned int>(connection, "UNetConnection.OutBytes");
		sample.packets_lost_in = ReadField<unsigned int>(connection, "UNetConnection.InPacketsLost");
		sample.packets_lost_out = ReadField<unsigned int>(connection, "UNetConnection.OutPacketsLost");
		sample.dormancy_rate = ReadField<float>(connection, "UNetConnection.DormancyRate");
		sample.avg_lag_seconds = ReadField<float>(connection, "UNetConnection.AvgLag");
		return sample;
	}

	void Reconcile()
	{
		UNetDriver* driver = NetDriver();
		if (!driver) {
			return;
		}

		int driver_changed = 0;
		int connections_changed = 0;
		int globals_changed = 0;

		if (g_config.server_max_tick_rate > 0) {
			driver_changed += WriteIfDifferent(Field<int>(driver, "UNetDriver.NetServerMaxTickRate"),
				g_config.server_max_tick_rate);
		}

		if (g_config.current_net_speed > 0) {
			TArray<UNetConnection*>* connections =
				Field<TArray<UNetConnection*>>(driver, "UNetDriver.ClientConnections");
			if (connections) {
				for (int i = 0; i < connections->Num(); ++i) {
					UNetConnection* connection = (*connections)[i];
					if (connection) {
						connections_changed += WriteIfDifferent(
							Field<int>(connection, "UPlayer.CurrentNetSpeed"), g_config.current_net_speed);
					}
				}
			}
		}

		if (g_config.dormancy_limit_multiplier > 0.0f) {
			globals_changed += WriteIfDifferent(
				GetNativeDataPointerField<float*>("Global.GDormancyLimitMultiplier"),
				g_config.dormancy_limit_multiplier);
		}

		if (driver_changed + connections_changed + globals_changed > 0) {
			LOG_INFO("applied driver={} connections={} globals={}",
				driver_changed, connections_changed, globals_changed);
		}
	}

	void UpdatePeaks()
	{
		UNetDriver* driver = NetDriver();
		if (!driver) {
			return;
		}

		TArray<UNetConnection*>* connections =
			Field<TArray<UNetConnection*>>(driver, "UNetDriver.ClientConnections");
		if (!connections) {
			return;
		}

		++g_tick_count;

		for (int i = 0; i < connections->Num(); ++i) {
			UNetConnection* connection = (*connections)[i];
			if (!connection) {
				continue;
			}

			const ConnectionSample sample = ReadConnection(connection);
			ConnectionPeaks& peaks = g_peaks[connection];
			peaks.last_seen_tick = g_tick_count;
			peaks.out_bps = (std::max)(peaks.out_bps, sample.out_bps);
			peaks.dormancy_rate = (std::max)(peaks.dormancy_rate, sample.dormancy_rate);
		}
	}

	void PruneStalePeaks()
	{
		for (auto it = g_peaks.begin(); it != g_peaks.end();) {
			if (it->second.last_seen_tick == g_tick_count) {
				++it;
			}
			else {
				it = g_peaks.erase(it);
			}
		}
	}

	void LogStatus(const char* source, const bool periodic)
	{
		UNetDriver* driver = NetDriver();
		if (!driver) {
			LOG_INFO("status({}): no NetDriver", source);
			return;
		}

		TArray<UNetConnection*>* connections =
			Field<TArray<UNetConnection*>>(driver, "UNetDriver.ClientConnections");
		if (!connections) {
			LOG_INFO("status({}): no connection list", source);
			return;
		}

		const int tick_rate = ReadField<int>(driver, "UNetDriver.NetServerMaxTickRate");
		float dormancy_multiplier = 0.0f;
		if (float* value = GetNativeDataPointerField<float*>("Global.GDormancyLimitMultiplier")) {
			dormancy_multiplier = *value;
		}

		unsigned long long total_out_bps = 0;
		unsigned long long total_peak_out_bps = 0;
		int connection_index = 0;

		for (int i = 0; i < connections->Num(); ++i) {
			UNetConnection* connection = (*connections)[i];
			if (!connection) {
				continue;
			}

			const ConnectionSample sample = ReadConnection(connection);
			ConnectionPeaks& peaks = g_peaks[connection];

			if (!periodic || g_config.debug) {
				LOG_INFO("status({}): conn[{}] ip='{}' netspeed={} queued={} outBps={} peakOutBps={} dorm={} peakDorm={} lost={}/{} lag={}",
					source, connection_index, connection->ClientGivenIPField().ToString(),
					sample.net_speed, sample.queued_bytes, sample.out_bps, peaks.out_bps,
					sample.dormancy_rate, peaks.dormancy_rate,
					sample.packets_lost_in, sample.packets_lost_out, sample.avg_lag_seconds);
			}

			total_out_bps += sample.out_bps;
			total_peak_out_bps += peaks.out_bps;
			++connection_index;

			peaks = ConnectionPeaks{};
		}

		const double total_peak_mbps = static_cast<double>(total_peak_out_bps) * 8.0 / 1e6;
		float budget_utilization = -1.0f;
		if (g_config.bandwidth_budget_mbps > 0.0f) {
			budget_utilization = static_cast<float>(
				total_peak_mbps / static_cast<double>(g_config.bandwidth_budget_mbps) * 100.0);
		}

		LOG_INFO("status({}): tickrate={} observedFps={} dormancyMult={} connections={} totalOutBps={} totalPeakMbps={} budgetUtilPct={}",
			source, tick_rate, g_observed_fps, dormancy_multiplier,
			connection_index, total_out_bps, total_peak_mbps, budget_utilization);
	}

	void Tick(float delta_seconds)
	{
		g_fps_window_seconds += delta_seconds;
		++g_fps_window_ticks;
		if (g_fps_window_seconds >= 1.0f) {
			g_observed_fps = static_cast<float>(g_fps_window_ticks) / g_fps_window_seconds;
			g_fps_window_seconds = 0.0f;
			g_fps_window_ticks = 0;
		}

		g_seconds_since_reconcile += delta_seconds;
		g_seconds_since_status += delta_seconds;

		UpdatePeaks();

		if (g_seconds_since_reconcile >= g_config.reconcile_interval_seconds) {
			g_seconds_since_reconcile = 0.0f;
			Reconcile();
			PruneStalePeaks();
		}

		if (g_config.status_interval_seconds > 0.0f &&
			g_seconds_since_status >= g_config.status_interval_seconds) {
			g_seconds_since_status = 0.0f;
			LogStatus("periodic", true);
		}
	}

	void ApplyConfig(const nlohmann::json& json)
	{
		g_config.debug = json.value("Debug", g_config.debug);
		g_config.server_max_tick_rate = json.value("NetServerMaxTickRate", g_config.server_max_tick_rate);
		g_config.current_net_speed = json.value("CurrentNetSpeed", g_config.current_net_speed);
		g_config.dormancy_limit_multiplier = json.value("DormancyLimitMultiplier", g_config.dormancy_limit_multiplier);
		g_config.reconcile_interval_seconds = json.value("ReconcileIntervalSeconds", g_config.reconcile_interval_seconds);
		g_config.status_interval_seconds = json.value("StatusIntervalSeconds", g_config.status_interval_seconds);
		g_config.bandwidth_budget_mbps = json.value("BandwidthBudgetMbps", g_config.bandwidth_budget_mbps);
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
			g_config.server_max_tick_rate, g_config.current_net_speed,
			g_config.dormancy_limit_multiplier);
		Reconcile();
	}

	void CmdStatus(RCONClientConnection*, RCONPacket*, UWorld*)
	{
		LogStatus("rcon", false);
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
		g_config.server_max_tick_rate, g_config.current_net_speed,
		g_config.dormancy_limit_multiplier);
}

extern "C" __declspec(dllexport) void Plugin_Unload()
{
	ArkApi::GetCommands().RemoveOnTickCallback("nettune_tick");
	ArkApi::GetCommands().RemoveRconCommand("NetTune.Reload");
	ArkApi::GetCommands().RemoveRconCommand("NetTune.Status");
}
