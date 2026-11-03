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

#include "NativeField.h"

#pragma comment(lib, "ArkApi.lib")

#define PLUGIN_NAME "NetTune"

namespace
{
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

	namespace fields
	{
		NativeField<TArray<UNetConnection*>> client_connections("UNetDriver.ClientConnections");
		NativeField<int> server_max_tick_rate("UNetDriver.NetServerMaxTickRate");
		NativeField<int> current_net_speed("UPlayer.CurrentNetSpeed");
		NativeField<int> queued_bytes("UNetConnection.QueuedBytes");
		NativeField<unsigned int> out_bytes("UNetConnection.OutBytes");
		NativeField<unsigned int> in_packets_lost("UNetConnection.InPacketsLost");
		NativeField<unsigned int> out_packets_lost("UNetConnection.OutPacketsLost");
		NativeField<float> dormancy_rate("UNetConnection.DormancyRate");
		NativeField<float> avg_lag("UNetConnection.AvgLag");
		NativeGlobal<float> dormancy_limit_multiplier("Global.GDormancyLimitMultiplier");
	}

	PluginConfig g_config;
	unsigned long long g_tick_count = 0;
	float g_observed_fps = 0.0f;
	float g_fps_window_seconds = 0.0f;
	unsigned long long g_fps_window_ticks = 0;
	float g_seconds_since_reconcile = 0.0f;
	float g_seconds_since_status = 0.0f;
	std::unordered_map<UNetConnection*, ConnectionPeaks> g_peaks;

	UNetDriver* GetNetDriver()
	{
		UWorld* world = ArkApi::GetApiUtils().GetWorld();
		return world ? world->NetDriverField() : nullptr;
	}

	ConnectionSample ReadConnection(UNetConnection* connection)
	{
		ConnectionSample sample;
		sample.net_speed = *fields::current_net_speed.Get(connection);
		sample.queued_bytes = *fields::queued_bytes.Get(connection);
		sample.out_bps = *fields::out_bytes.Get(connection);
		sample.packets_lost_in = *fields::in_packets_lost.Get(connection);
		sample.packets_lost_out = *fields::out_packets_lost.Get(connection);
		sample.dormancy_rate = *fields::dormancy_rate.Get(connection);
		sample.avg_lag_seconds = *fields::avg_lag.Get(connection);
		return sample;
	}

	void Reconcile()
	{
		UNetDriver* driver = GetNetDriver();
		if (!driver) {
			return;
		}

		if (g_config.server_max_tick_rate > 0) {
			if (fields::server_max_tick_rate.Set(driver, g_config.server_max_tick_rate)) {
				LOG_DEBUG("server_max_tick_rate changed -> {}", g_config.server_max_tick_rate);
			}
		}

		if (g_config.current_net_speed > 0) {
			if (TArray<UNetConnection*>* connections = fields::client_connections.Get(driver)) {
				int connections_changed = 0;
				for (int i = 0; i < connections->Num(); ++i) {
					UNetConnection* connection = (*connections)[i];
					if (connection) {
						connections_changed +=
							fields::current_net_speed.Set(connection, g_config.current_net_speed);
					}
				}
				if (connections_changed > 0) {
					LOG_DEBUG("current_net_speed changed -> {} ({} connections)",
						g_config.current_net_speed, connections_changed);
				}
			}
		}

		if (g_config.dormancy_limit_multiplier > 0.0f) {
			if (fields::dormancy_limit_multiplier.Set(g_config.dormancy_limit_multiplier)) {
				LOG_DEBUG("dormancy_limit_multiplier changed -> {}", g_config.dormancy_limit_multiplier);
			}
		}
	}

	void UpdatePeaks()
	{
		UNetDriver* driver = GetNetDriver();
		if (!driver) {
			return;
		}

		TArray<UNetConnection*>* connections = fields::client_connections.Get(driver);
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
		UNetDriver* driver = GetNetDriver();
		if (!driver) {
			LOG_INFO("status({}): no NetDriver", source);
			return;
		}

		TArray<UNetConnection*>* connections = fields::client_connections.Get(driver);
		if (!connections) {
			LOG_INFO("status({}): no connection list", source);
			return;
		}

		const int tick_rate = *fields::server_max_tick_rate.Get(driver);
		float dormancy_multiplier = 0.0f;
		if (float* value = fields::dormancy_limit_multiplier.Get()) {
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
				std::string cap = "-";
				if (tick_rate > 0 && dormancy_multiplier > 0.0f && sample.net_speed > 0) {
					const float f = std::clamp((sample.net_speed - 14000.0f) / 46000.0f, 0.0f, 1.0f);
					cap = fmt::format("{:.4g}", tick_rate / ((f * 195.0f + 30.0f) * dormancy_multiplier));
				}

				LOG_INFO("status({}): conn[{}] ip='{}' netspeed={} queued={} outBps={} peakOutBps={} dorm={:.4g} peakDorm={:.4g} cap={} lost={}/{} lag={}",
					source, connection_index, connection->ClientGivenIPField().ToString(),
					sample.net_speed, sample.queued_bytes, sample.out_bps, peaks.out_bps,
					sample.dormancy_rate, peaks.dormancy_rate, cap,
					sample.packets_lost_in, sample.packets_lost_out,
					sample.avg_lag_seconds > 60.0f ? "-" : fmt::format("{:.3g}", sample.avg_lag_seconds));
			}

			total_out_bps += sample.out_bps;
			total_peak_out_bps += peaks.out_bps;
			++connection_index;

			peaks = ConnectionPeaks{};
		}

		const double total_peak_mbps = static_cast<double>(total_peak_out_bps) * 8.0 / 1e6;

		if (g_config.bandwidth_budget_mbps > 0.0f) {
			const float budget_utilization = static_cast<float>(
				total_peak_mbps / static_cast<double>(g_config.bandwidth_budget_mbps) * 100.0);
			LOG_INFO("status({}): tickrate={} observedFps={:.1f} dormancyMult={} connections={} totalOutBps={} totalPeakMbps={:.2f} budgetUtilPct={:.1f}",
				source, tick_rate, g_observed_fps, dormancy_multiplier,
				connection_index, total_out_bps, total_peak_mbps, budget_utilization);
		}
		else {
			LOG_INFO("status({}): tickrate={} observedFps={:.1f} dormancyMult={} connections={} totalOutBps={} totalPeakMbps={:.2f}",
				source, tick_rate, g_observed_fps, dormancy_multiplier,
				connection_index, total_out_bps, total_peak_mbps);
		}
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
			ApplyConfig(Config::Read(PLUGIN_NAME));
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
	LOG_INIT(PLUGIN_NAME);

	LoadConfig();

	ArkApi::GetCommands().AddOnTickCallback("NetTuneTick", &Tick);
	ArkApi::GetCommands().AddRconCommand("NetTune.Reload", &CmdReload);
	ArkApi::GetCommands().AddRconCommand("NetTune.Status", &CmdStatus);

	Reconcile();

	LOG_DEBUG("loaded: tickrate={} currentNetSpeed={} dormancyMult={}",
		g_config.server_max_tick_rate, g_config.current_net_speed,
		g_config.dormancy_limit_multiplier);
}

extern "C" __declspec(dllexport) void Plugin_Unload()
{
	ArkApi::GetCommands().RemoveOnTickCallback("NetTuneTick");
	ArkApi::GetCommands().RemoveRconCommand("NetTune.Reload");
	ArkApi::GetCommands().RemoveRconCommand("NetTune.Status");
}
