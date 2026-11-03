// NetTune - ARK: Survival Evolved v361 server plugin (ArkApi / version.dll).
//
// Ускоряет доставку мира клиенту (в первую очередь баз/структур) и даёт
// инструменты измерения/адаптации.
//
// ---------------------------------------------------------------------------
// ЧТО РЕАЛЬНО ВЛИЯЕТ (проверено по бинарю v361 + живой сервер)
// ---------------------------------------------------------------------------
// 1) UNetDriver::NetServerMaxTickRate  ->  FPS сервера.
//    UGameEngine::GetMaxTickRate читает NetDriver+0x48 и клампит в [10,120];
//    r.MaxFPS перебивает. Т.е. это частота тиков движка, а значит и частота
//    ServerReplicateActors + FlushNet. Самый прямой рычаг.
//    В коде поле НИГДЕ не перезаписывается (только конфиг при инициализации).
//
// 2) UPlayer::CurrentNetSpeed  ->  БЮДЖЕТ НОВЫХ АКТОРОВ (dormancy-троттлинг).
//    В ServerReplicateActors на каждый новый актор копится
//    DormancyRate += (1.0/(f*195+30))/DeltaSeconds/GDormancyLimitMultiplier,
//    где f = clamp((CurrentNetSpeed-14000)/46000, 0, 1).
//    f насыщается на 1.0 при CurrentNetSpeed >= 60000 => выше 60000 смысла нет.
//    Диапазон [14000, 60000]; это же значение присылает клиент (NMT_Netspeed)
//    из своей настройки ClientNetQuality, и сервер клампит его в тот же диапазон.
//
// 3) GDormancyLimitMultiplier (глобал, дефолт 1.0) - делитель инкремента.
// 4) GNoDormancyLimit (глобал, дефолт false) - снимает троттлинг.
//
// Чего НЕ делает:
//   * CurrentNetSpeed НЕ лимит байт/сек: у UNetConnection::IsNetReady нет ни
//     одного прямого вызова (Ghidra caller_count=0), и на живом сервере при
//     загрузке базы пик ~296 КБ/с при QueuedBytes <= 0.
//   * MaxClientRate перезаписывается только в UWorld::Listen (=60000).
//
// ---------------------------------------------------------------------------
// ПОЧЕМУ НЕТ ЦИКЛА ПЕРЕЗАПИСИ
// ---------------------------------------------------------------------------
// Все целевые поля переписываются только по конкретным событиям:
//   * NetServerMaxTickRate - никогда;
//   * MaxClientRate/MaxInternetClientRate - UWorld::Listen (загрузка карты);
//   * CurrentNetSpeed - сообщение клиента NMT_Netspeed (только при смене его
//     настройки), APlayerController::SetPlayer (при possession),
//     ClientCapBandwidth (только понижает).
// Поэтому плагин делает "reconcile": раз в reconcileIntervalSeconds читает
// значения и пишет ТОЛЬКО если они разошлись с целью. В стабильном состоянии
// записей нет вообще - это не цикл перезаписи, а починка дрейфа.

#include <windows.h>

#include <API/ARK/Ark.h>
#include <Logger/Logger.h>
#include <json.hpp>
#include <Utils.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#pragma comment(lib, "ArkApi.lib")

namespace
{
	constexpr const char* kPluginName = "NetTune";
	constexpr const char* kConfigFile = "NetTune.json";
	constexpr const char* kTickId = "nettune_tick";

	// ---------------------------------------------------------------- config

	struct Config
	{
		bool enabled = true;
		std::string mode = "adaptive";          // "static" | "adaptive"
		double reconcile_interval_seconds = 15.0;
		double status_interval_seconds = 30.0;

		// UNetDriver
		int net_server_max_tick_rate = 0;
		int max_client_rate = 0;
		int max_internet_client_rate = 0;

		// UPlayer (static mode)
		int current_net_speed = 0;
		int configured_internet_speed = 0;
		int configured_lan_speed = 0;

		// adaptive mode
		int adaptive_min = 14000;
		int adaptive_max = 60000;
		int adaptive_step_up = 10000;
		int adaptive_step_down = 15000;
		unsigned int adaptive_lost_packets = 2;   // max lost packets/sec in window
		float adaptive_lag_threshold = 0.f;       // AvgLag (s); 0 = ignore
		double adaptive_evaluate_seconds = 10.0;

		// dormancy throttle globals
		float dormancy_limit_multiplier = 0.f;    // 0 = leave
		bool disable_dormancy_throttling = false;

		// AActor (structures) - optional, off by default
		float structure_replication_interval_multiplier = 0.f;
		float structure_net_update_frequency = 0.f;
		double structure_apply_interval_seconds = 60.0;
	};

	Config g_cfg;
	std::string g_config_path;

	struct ConnState
	{
		int applied = 0;
		unsigned int window_lost_max = 0;
		float window_lag_max = 0.f;
		double last_sample_ms = 0.0;
		double last_eval_ms = 0.0;
		unsigned int peak_out_bps = 0;
		float peak_dorm = 0.f;
	};

	std::unordered_map<UNetConnection*, ConnState> g_conn;

	// наблюдаемый FPS (наш тик-колбэк) - проверка NetServerMaxTickRate
	unsigned long long g_tick_count = 0;
	double g_fps_window_start_ms = 0.0;
	double g_observed_fps = 0.0;
	unsigned long long g_fps_window_ticks = 0;

	// ---------------------------------------------------------------- helpers

	std::string PluginDir()
	{
		char buf[MAX_PATH] = {};
		HMODULE mod = nullptr;
		if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
			GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			reinterpret_cast<LPCSTR>(&PluginDir), &mod) && mod)
		{
			GetModuleFileNameA(mod, buf, MAX_PATH);
		}
		std::string path(buf);
		const auto pos = path.find_last_of("\\/");
		return pos == std::string::npos ? std::string(".") : path.substr(0, pos);
	}

	void LogI(const std::string& msg) { if (Log::GetLog()) Log::GetLog()->info("[NetTune] {}", msg); }
	void LogW(const std::string& msg) { if (Log::GetLog()) Log::GetLog()->warn("[NetTune] {}", msg); }

	template <typename T>
	T* SafeFieldPtr(void* obj, const char* name)
	{
		if (!obj)
			return nullptr;
		T* p = GetNativePointerField<T*>(obj, name);
		if (!p || reinterpret_cast<char*>(p) == reinterpret_cast<char*>(obj))
			return nullptr;
		return p;
	}

	template <typename T>
	bool ReadField(void* obj, const char* name, T& out)
	{
		T* p = SafeFieldPtr<T>(obj, name);
		if (!p)
			return false;
		out = *p;
		return true;
	}

	// Глобал по имени: ArkApi кладёт data-символы под ключом "Global.<name>".
	template <typename T>
	T* SafeGlobal(const char* name)
	{
		return GetNativeDataPointerField<T*>(name);
	}

	// ---------------------------------------------------------------- config io

	void WriteDefaultConfig()
	{
		std::ofstream out(g_config_path);
		if (!out.is_open())
			return;

		out << R"({
  "enabled": true,
  "mode": "adaptive",
  "reconcileIntervalSeconds": 15.0,
  "statusIntervalSeconds": 30.0,

  "NetServerMaxTickRate": 20,
  "MaxClientRate": 60000,
  "MaxInternetClientRate": 60000,

  "CurrentNetSpeed": 60000,
  "ConfiguredInternetSpeed": 60000,
  "ConfiguredLanSpeed": 60000,

  "AdaptiveMin": 14000,
  "AdaptiveMax": 60000,
  "AdaptiveStepUp": 10000,
  "AdaptiveStepDown": 15000,
  "AdaptiveLostPackets": 2,
  "AdaptiveLagThreshold": 0.0,
  "AdaptiveEvaluateSeconds": 10.0,

  "DormancyLimitMultiplier": 0.0,
  "DisableDormancyThrottling": false,

  "StructureReplicationIntervalMultiplier": 0.0,
  "StructureNetUpdateFrequency": 0.0,
  "StructureApplyIntervalSeconds": 60.0
}
)";
	}

	void ApplyConfig(const nlohmann::json& j)
	{
		g_cfg.enabled = j.value("enabled", g_cfg.enabled);
		g_cfg.mode = j.value("mode", g_cfg.mode);
		if (g_cfg.mode != "static" && g_cfg.mode != "adaptive")
		{
			LogW("unknown mode '" + g_cfg.mode + "' - using adaptive");
			g_cfg.mode = "adaptive";
		}
		g_cfg.reconcile_interval_seconds = j.value("reconcileIntervalSeconds", g_cfg.reconcile_interval_seconds);
		g_cfg.status_interval_seconds = j.value("statusIntervalSeconds", g_cfg.status_interval_seconds);

		g_cfg.net_server_max_tick_rate = j.value("NetServerMaxTickRate", g_cfg.net_server_max_tick_rate);
		g_cfg.max_client_rate = j.value("MaxClientRate", g_cfg.max_client_rate);
		g_cfg.max_internet_client_rate = j.value("MaxInternetClientRate", g_cfg.max_internet_client_rate);

		g_cfg.current_net_speed = j.value("CurrentNetSpeed", g_cfg.current_net_speed);
		g_cfg.configured_internet_speed = j.value("ConfiguredInternetSpeed", g_cfg.configured_internet_speed);
		g_cfg.configured_lan_speed = j.value("ConfiguredLanSpeed", g_cfg.configured_lan_speed);

		g_cfg.adaptive_min = j.value("AdaptiveMin", g_cfg.adaptive_min);
		g_cfg.adaptive_max = j.value("AdaptiveMax", g_cfg.adaptive_max);
		g_cfg.adaptive_step_up = j.value("AdaptiveStepUp", g_cfg.adaptive_step_up);
		g_cfg.adaptive_step_down = j.value("AdaptiveStepDown", g_cfg.adaptive_step_down);
		g_cfg.adaptive_lost_packets = j.value("AdaptiveLostPackets", g_cfg.adaptive_lost_packets);
		g_cfg.adaptive_lag_threshold = j.value("AdaptiveLagThreshold", g_cfg.adaptive_lag_threshold);
		g_cfg.adaptive_evaluate_seconds = j.value("AdaptiveEvaluateSeconds", g_cfg.adaptive_evaluate_seconds);

		g_cfg.dormancy_limit_multiplier = j.value("DormancyLimitMultiplier", g_cfg.dormancy_limit_multiplier);
		g_cfg.disable_dormancy_throttling = j.value("DisableDormancyThrottling", g_cfg.disable_dormancy_throttling);

		g_cfg.structure_replication_interval_multiplier = j.value(
			"StructureReplicationIntervalMultiplier", g_cfg.structure_replication_interval_multiplier);
		g_cfg.structure_net_update_frequency = j.value(
			"StructureNetUpdateFrequency", g_cfg.structure_net_update_frequency);
		g_cfg.structure_apply_interval_seconds = j.value(
			"StructureApplyIntervalSeconds", g_cfg.structure_apply_interval_seconds);
	}

	void LoadConfig()
	{
		g_config_path = PluginDir() + "\\" + kConfigFile;

		std::ifstream in(g_config_path);
		if (!in.is_open())
		{
			WriteDefaultConfig();
			LogW("config missing - wrote defaults to " + g_config_path);
			return;
		}

		try
		{
			nlohmann::json j;
			in >> j;
			ApplyConfig(j);
			LogI("config loaded from " + g_config_path);
		}
		catch (const std::exception& e)
		{
			LogW(std::string("config parse failed (") + e.what() + ") - keeping current values");
		}
	}

	// ---------------------------------------------------------------- apply

	// Пишет значение только если оно отличается. Возвращает 1, если была запись.
	template <typename T>
	int SetIfDiff(T* p, const T& want)
	{
		if (!p || *p == want)
			return 0;
		*p = want;
		return 1;
	}

	int ReconcileDriver(UNetDriver* driver)
	{
		int changed = 0;
		if (g_cfg.net_server_max_tick_rate > 0)
			changed += SetIfDiff(SafeFieldPtr<int>(driver, "UNetDriver.NetServerMaxTickRate"),
				g_cfg.net_server_max_tick_rate);
		if (g_cfg.max_client_rate > 0)
			changed += SetIfDiff(SafeFieldPtr<int>(driver, "UNetDriver.MaxClientRate"), g_cfg.max_client_rate);
		if (g_cfg.max_internet_client_rate > 0)
			changed += SetIfDiff(SafeFieldPtr<int>(driver, "UNetDriver.MaxInternetClientRate"),
				g_cfg.max_internet_client_rate);
		return changed;
	}

	int ReconcileGlobals()
	{
		int changed = 0;
		if (g_cfg.dormancy_limit_multiplier > 0.f)
			changed += SetIfDiff(SafeGlobal<float>("Global.GDormancyLimitMultiplier"),
				g_cfg.dormancy_limit_multiplier);
		if (g_cfg.disable_dormancy_throttling)
			changed += SetIfDiff(SafeGlobal<bool>("Global.GNoDormancyLimit"), true);
		return changed;
	}

	int ReconcileConnectionStatic(UNetConnection* c)
	{
		int changed = 0;
		if (g_cfg.current_net_speed > 0)
			changed += SetIfDiff(SafeFieldPtr<int>(c, "UPlayer.CurrentNetSpeed"), g_cfg.current_net_speed);
		if (g_cfg.configured_internet_speed > 0)
			changed += SetIfDiff(SafeFieldPtr<int>(c, "UPlayer.ConfiguredInternetSpeed"),
				g_cfg.configured_internet_speed);
		if (g_cfg.configured_lan_speed > 0)
			changed += SetIfDiff(SafeFieldPtr<int>(c, "UPlayer.ConfiguredLanSpeed"), g_cfg.configured_lan_speed);
		return changed;
	}

	// Адаптив: цель в [min,max], старт с максимума, шаг вниз при потерях/лаге.
	// InPacketsLost/OutPacketsLost/AvgLag обновляются и СБРАСЫВАЮТСЯ раз в
	// StatPeriod (1 c), поэтому сэмплируем раз в секунду и берём максимум за окно.
	int ReconcileConnectionAdaptive(UNetConnection* c, ConnState& st, double now_ms)
	{
		int* speed = SafeFieldPtr<int>(c, "UPlayer.CurrentNetSpeed");
		if (!speed)
			return 0;

		if (st.applied == 0)
			st.applied = g_cfg.adaptive_max;   // сразу максимум: цель - быстро отдать мир
		st.applied = (std::max)(g_cfg.adaptive_min, (std::min)(g_cfg.adaptive_max, st.applied));

		unsigned int in_lost = 0, out_lost = 0;
		float avg_lag = 0.f;
		ReadField(c, "UNetConnection.InPacketsLost", in_lost);
		ReadField(c, "UNetConnection.OutPacketsLost", out_lost);
		ReadField(c, "UNetConnection.AvgLag", avg_lag);

		if (st.last_sample_ms == 0.0 || now_ms - st.last_sample_ms >= 1000.0)
		{
			st.last_sample_ms = now_ms;
			const unsigned int lost = in_lost + out_lost;
			if (lost > st.window_lost_max)
				st.window_lost_max = lost;
			if (avg_lag > st.window_lag_max)
				st.window_lag_max = avg_lag;
		}

		if (st.last_eval_ms == 0.0 || now_ms - st.last_eval_ms >= g_cfg.adaptive_evaluate_seconds * 1000.0)
		{
			const bool loss_bad = st.window_lost_max > g_cfg.adaptive_lost_packets;
			const bool lag_bad = g_cfg.adaptive_lag_threshold > 0.f &&
				st.window_lag_max > g_cfg.adaptive_lag_threshold;

			const int target = (loss_bad || lag_bad)
				? (std::max)(g_cfg.adaptive_min, st.applied - g_cfg.adaptive_step_down)
				: (std::min)(g_cfg.adaptive_max, st.applied + g_cfg.adaptive_step_up);

			if (target != st.applied)
			{
				LogI("adaptive " + c->ClientGivenIPField().ToString() + ": netspeed " +
					std::to_string(st.applied) + " -> " + std::to_string(target) +
					" (lostMax=" + std::to_string(st.window_lost_max) + "/s, lagMax=" +
					std::to_string(st.window_lag_max) + "s)");
				st.applied = target;
			}

			st.window_lost_max = 0;
			st.window_lag_max = 0.f;
			st.last_eval_ms = now_ms;
		}

		return SetIfDiff(speed, st.applied);
	}

	int ReconcileStructures()
	{
		int changed = 0;
		for (auto* structure : GetAllStructures())
		{
			if (!IsValid(structure))
				continue;

			if (g_cfg.structure_replication_interval_multiplier > 0.f)
				changed += SetIfDiff(SafeFieldPtr<float>(structure, "AActor.ReplicationIntervalMultiplier"),
					g_cfg.structure_replication_interval_multiplier);

			if (g_cfg.structure_net_update_frequency > 0.f)
				changed += SetIfDiff(SafeFieldPtr<float>(structure, "AActor.NetUpdateFrequency"),
					g_cfg.structure_net_update_frequency);
		}
		return changed;
	}

	// ---------------------------------------------------------------- reporting

	std::string GlobalInfo()
	{
		std::string s = "dormancyMult=";
		if (float* p = SafeGlobal<float>("Global.GDormancyLimitMultiplier"))
			s += std::to_string(*p);
		else
			s += "n/a";
		s += " noDormancyLimit=";
		if (bool* p = SafeGlobal<bool>("Global.GNoDormancyLimit"))
			s += (*p ? "1" : "0");
		else
			s += "n/a";
		return s;
	}

	void LogStatus(const char* reason)
	{
		UWorld* world = ArkApi::GetApiUtils().GetWorld();
		if (!world)
		{
			LogI(std::string("status(") + reason + "): no world");
			return;
		}

		auto* driver = world->NetDriverField();
		if (!driver)
		{
			LogI(std::string("status(") + reason + "): no NetDriver");
			return;
		}

		int tick_rate = 0, max_client = 0, max_internet = 0;
		unsigned int drv_out = 0, drv_pkts = 0, drv_bunches = 0;
		ReadField(driver, "UNetDriver.NetServerMaxTickRate", tick_rate);
		ReadField(driver, "UNetDriver.MaxClientRate", max_client);
		ReadField(driver, "UNetDriver.MaxInternetClientRate", max_internet);
		ReadField(driver, "UNetDriver.OutBytes", drv_out);
		ReadField(driver, "UNetDriver.OutPackets", drv_pkts);
		ReadField(driver, "UNetDriver.OutBunches", drv_bunches);

		LogI(std::string("status(") + reason + "): enabled=" + (g_cfg.enabled ? "1" : "0") +
			" mode=" + g_cfg.mode +
			" tickrate=" + std::to_string(tick_rate) +
			" observedFps=" + std::to_string(g_observed_fps) +
			" maxclientrate=" + std::to_string(max_client) +
			" maxinternet=" + std::to_string(max_internet) +
			" drvOutBytes=" + std::to_string(drv_out) +
			" drvOutPkts=" + std::to_string(drv_pkts) +
			" drvOutBunches=" + std::to_string(drv_bunches) +
			" " + GlobalInfo());

		int index = 0;
		for (auto* c : driver->ClientConnectionsField())
		{
			if (!c)
				continue;

			int netspeed = 0, internet = 0, lan = 0, queued = 0, maxpacket = 0, ack_deficit = 0;
			unsigned int in_lost = 0, out_lost = 0, out_bytes = 0, in_bytes = 0;
			float dorm = 0.f, avg_lag = 0.f;
			ReadField(c, "UPlayer.CurrentNetSpeed", netspeed);
			ReadField(c, "UPlayer.ConfiguredInternetSpeed", internet);
			ReadField(c, "UPlayer.ConfiguredLanSpeed", lan);
			ReadField(c, "UNetConnection.QueuedBytes", queued);
			ReadField(c, "UNetConnection.MaxPacket", maxpacket);
			ReadField(c, "UNetConnection.AckDeficit", ack_deficit);
			ReadField(c, "UNetConnection.InPacketsLost", in_lost);
			ReadField(c, "UNetConnection.OutPacketsLost", out_lost);
			ReadField(c, "UNetConnection.OutBytes", out_bytes);
			ReadField(c, "UNetConnection.InBytes", in_bytes);
			ReadField(c, "UNetConnection.DormancyRate", dorm);
			ReadField(c, "UNetConnection.AvgLag", avg_lag);

			const ConnState& st = g_conn[c];
			LogI("status(" + std::string(reason) + "): conn[" + std::to_string(index) + "] ip='" +
				c->ClientGivenIPField().ToString() + "' netspeed=" + std::to_string(netspeed) +
				" cfg_internet=" + std::to_string(internet) + " cfg_lan=" + std::to_string(lan) +
				" queued=" + std::to_string(queued) + " maxpacket=" + std::to_string(maxpacket) +
				" dorm=" + std::to_string(dorm) + " peakDorm=" + std::to_string(st.peak_dorm) +
				" outBps=" + std::to_string(out_bytes) + " peakOutBps=" + std::to_string(st.peak_out_bps) +
				" avglag=" + std::to_string(avg_lag) +
				" ackdeficit=" + std::to_string(ack_deficit) +
				" inLost=" + std::to_string(in_lost) + " outLost=" + std::to_string(out_lost) +
				" inBytes=" + std::to_string(in_bytes));
			++index;
		}
		if (index == 0)
			LogI(std::string("status(") + reason + "): no client connections");
	}

	// ---------------------------------------------------------------- tick

	void Tick(float delta_seconds)
	{
		++g_tick_count;

		const double now_ms = static_cast<double>(GetTickCount64());

		// наблюдаемый FPS
		if (g_fps_window_start_ms == 0.0)
			g_fps_window_start_ms = now_ms;
		++g_fps_window_ticks;
		if (now_ms - g_fps_window_start_ms >= 1000.0)
		{
			g_observed_fps = static_cast<double>(g_fps_window_ticks) * 1000.0 /
				(now_ms - g_fps_window_start_ms);
			g_fps_window_ticks = 0;
			g_fps_window_start_ms = now_ms;
		}

		static double since_reconcile = 0.0;
		static double since_status = 0.0;
		static double since_structures = 0.0;

		since_reconcile += delta_seconds;
		since_status += delta_seconds;
		since_structures += delta_seconds;

		UWorld* world = ArkApi::GetApiUtils().GetWorld();

		// Пиковая телеметрия - каждый тик, чтобы поймать всплеск загрузки базы.
		if (world)
		{
			if (auto* driver = world->NetDriverField())
			{
				std::unordered_set<UNetConnection*> live;
				for (auto* c : driver->ClientConnectionsField())
				{
					if (!c)
						continue;
					live.insert(c);
					ConnState& st = g_conn[c];
					unsigned int out_bps = 0;
					float dorm = 0.f;
					ReadField(c, "UNetConnection.OutBytes", out_bps);
					ReadField(c, "UNetConnection.DormancyRate", dorm);
					if (out_bps > st.peak_out_bps)
						st.peak_out_bps = out_bps;
					if (dorm > st.peak_dorm)
						st.peak_dorm = dorm;
				}
				// чистим состояние отключившихся соединений
				for (auto it = g_conn.begin(); it != g_conn.end();)
					it = live.count(it->first) ? std::next(it) : g_conn.erase(it);
			}
		}

		if (g_cfg.enabled && since_reconcile >= g_cfg.reconcile_interval_seconds)
		{
			since_reconcile = 0.0;

			if (world)
			{
				int driver_changed = 0, conn_changed = 0, globals_changed = 0;

				if (auto* driver = world->NetDriverField())
				{
					driver_changed = ReconcileDriver(driver);

					const bool adaptive = (g_cfg.mode == "adaptive");
					for (auto* c : driver->ClientConnectionsField())
					{
						if (!c)
							continue;
						ConnState& st = g_conn[c];
						conn_changed += adaptive ? ReconcileConnectionAdaptive(c, st, now_ms)
						                         : ReconcileConnectionStatic(c);
					}
				}
				globals_changed = ReconcileGlobals();

				if (driver_changed + conn_changed + globals_changed > 0)
				{
					LogI("reconcile: driver=" + std::to_string(driver_changed) +
						" connections=" + std::to_string(conn_changed) +
						" globals=" + std::to_string(globals_changed));
				}
			}
		}

		if (g_cfg.enabled &&
			(g_cfg.structure_replication_interval_multiplier > 0.f ||
				g_cfg.structure_net_update_frequency > 0.f) &&
			since_structures >= g_cfg.structure_apply_interval_seconds)
		{
			since_structures = 0.0;
			const int changed = ReconcileStructures();
			if (changed > 0)
				LogI("reconcile structures=" + std::to_string(changed));
		}

		if (since_status >= g_cfg.status_interval_seconds)
		{
			since_status = 0.0;
			LogStatus(g_cfg.enabled ? "periodic" : "periodic (disabled)");
		}
	}

	// ---------------------------------------------------------------- commands

	void CmdStatus(RCONClientConnection*, RCONPacket*, UWorld*) { LogStatus("rcon"); }

	void CmdReload(RCONClientConnection*, RCONPacket*, UWorld*)
	{
		LoadConfig();
		LogI("reloaded: enabled=" + std::string(g_cfg.enabled ? "1" : "0") + " mode=" + g_cfg.mode +
			" tickrate=" + std::to_string(g_cfg.net_server_max_tick_rate) +
			" CurrentNetSpeed=" + std::to_string(g_cfg.current_net_speed) +
			" Adaptive=[" + std::to_string(g_cfg.adaptive_min) + "," +
			std::to_string(g_cfg.adaptive_max) + "]" +
			" dormancyMult=" + std::to_string(g_cfg.dormancy_limit_multiplier));
	}

	void CmdOn(RCONClientConnection*, RCONPacket*, UWorld*) { g_cfg.enabled = true; LogI("enabled"); LogStatus("on"); }

	void CmdOff(RCONClientConnection*, RCONPacket*, UWorld*) { g_cfg.enabled = false; LogI("disabled"); }

	void CmdResetPeaks(RCONClientConnection*, RCONPacket*, UWorld*)
	{
		for (auto& kv : g_conn)
		{
			kv.second.peak_out_bps = 0;
			kv.second.peak_dorm = 0.f;
		}
		LogI("peaks reset");
	}
} // namespace

extern "C" __declspec(dllexport) void Plugin_Init()
{
	Log::Get().Init(kPluginName);
	LogI("initializing");

	LoadConfig();

	auto& cmds = ArkApi::GetCommands();
	cmds.AddOnTickCallback(kTickId, &Tick);
	cmds.AddRconCommand("NetTune.status", &CmdStatus);
	cmds.AddRconCommand("NetTune.reload", &CmdReload);
	cmds.AddRconCommand("NetTune.on", &CmdOn);
	cmds.AddRconCommand("NetTune.off", &CmdOff);
	cmds.AddRconCommand("NetTune.resetpeaks", &CmdResetPeaks);

	LogI("loaded: enabled=" + std::string(g_cfg.enabled ? "1" : "0") + " mode=" + g_cfg.mode);
}

extern "C" __declspec(dllexport) void Plugin_Unload()
{
	auto& cmds = ArkApi::GetCommands();
	cmds.RemoveOnTickCallback(kTickId);
	cmds.RemoveRconCommand("NetTune.status");
	cmds.RemoveRconCommand("NetTune.reload");
	cmds.RemoveRconCommand("NetTune.on");
	cmds.RemoveRconCommand("NetTune.off");
	cmds.RemoveRconCommand("NetTune.resetpeaks");
	LogI("unloaded");
}
