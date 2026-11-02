// NetTune - ARK: Survival Evolved v361 server plugin (ArkApi / version.dll).
//
// Назначение: ускорить доставку мира клиенту (в первую очередь баз/структур)
// и дать инструменты для измерения/адаптации.
//
// ---------------------------------------------------------------------------
// ЧТО РЕАЛЬНО ВЛИЯЕТ (проверено по бинарю v361 + живой сервер)
// ---------------------------------------------------------------------------
// 1) UNetDriver::NetServerMaxTickRate  ->  FPS сервера.
//    UGameEngine::GetMaxTickRate берёт его из NetDriver+0x48 и клампит в
//    [10,120]; r.MaxFPS перебивает. Т.е. это реальная частота тиков движка,
//    а значит и частота ServerReplicateActors + FlushNet. Самый прямой рычаг.
//
// 2) UPlayer::CurrentNetSpeed  ->  БЮДЖЕТ НОВЫХ АКТОРОВ (dormancy-троттлинг).
//    В UNetDriver::ServerReplicateActors на каждый новый актор копится
//    DormancyRate += (1.0/(f*195+30))/DeltaSeconds/GDormancyLimitMultiplier,
//    где f = clamp((CurrentNetSpeed-14000)/46000, 0, 1).
//    DormancyRate убывает на 1.0 за тик, поэтому чем меньше инкремент -
//    тем больше новых акторов успевает уйти за тик.
//    ВАЖНО: f насыщается на 1.0 при CurrentNetSpeed >= 60000, поэтому
//    значения выше 60000 НИЧЕГО не добавляют. Диапазон [14000, 60000].
//    Именно это поле присылает клиент (NMT_Netspeed) из своего
//    ClientNetQuality (14000/20000/40000/60000); сервер клампит его в тот же
//    диапазон (UWorld::NotifyControlMessage, case 4).
//
// 3) GDormancyLimitMultiplier (глобал, дефолт 1.0) - делитель инкремента.
//    Больше => больше новых акторов за тик. Раньше задавался только
//    командной строкой -DormancyNetMultiplier=<x>; плагин может менять на лету.
// 4) GNoDormancyLimit (глобал, дефолт false) - полностью снимает троттлинг
//    (командная строка -nodormancythrottling).
//
// Чего НЕ делает (важно, чтобы не обманываться):
//   * CurrentNetSpeed НЕ является жёстким лимитом байт/сек: UNetConnection::
//     IsNetReady (проверка QueuedBytes) в этом билде нигде не вызывается,
//     поэтому бюджет не гейтит отправку напрямую.
//   * MaxClientRate читается в UWorld::Listen (жёстко ставит себе 60000) и,
//     похоже, только при инициализации соединения - на уже идущий поток
//     влияет слабо.
//
// ---------------------------------------------------------------------------
// РЕЖИМЫ
// ---------------------------------------------------------------------------
//   "static"   - раз в ApplyIntervalSeconds выставлять фиксированные значения.
//   "adaptive" - на соединение: цель в [AdaptiveMin, AdaptiveMax], шаг вверх
//                при чистом канале и вниз при потерях/лаге. Значения выше 60000
//                бессмысленны (см. п.2), поэтому AdaptiveMax по умолчанию 60000.
//
// Всё применяется по таймеру, потому что:
//   * клиент перезаписывает CurrentNetSpeed при смене своей настройки;
//   * UWorld::Listen перезаписывает MaxClientRate=60000 при загрузке карты;
//   * новые структуры/соединения появляются со временем.

#include <windows.h>

#include <API/ARK/Ark.h>
#include <Logger/Logger.h>
#include <json.hpp>
#include <Utils.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

#pragma comment(lib, "ArkApi.lib")

namespace
{
	constexpr const char* kPluginName = "NetTune";
	constexpr const char* kConfigFile = "NetTune.json";
	constexpr const char* kTickId = "nettune_tick";

	struct Config
	{
		bool enabled = true;
		std::string mode = "adaptive";
		double apply_interval_seconds = 10.0;
		double status_interval_seconds = 60.0;

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
		unsigned int adaptive_lost_packets = 2;   // max lost packets/sec in window before stepping down
		float adaptive_lag_threshold = 0.f;        // AvgLag seconds; 0 = ignore lag
		double adaptive_evaluate_seconds = 10.0;

		// dormancy throttle globals
		float dormancy_limit_multiplier = 0.f;      // 0 = leave
		bool disable_dormancy_throttling = false;   // false = leave

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
		unsigned int window_lost_max = 0;   // max per-second lost packets in the window
		float window_lag_max = 0.f;         // max AvgLag in the window
		double last_sample_ms = 0.0;
		double last_eval_ms = 0.0;
		unsigned int peak_out_bps = 0;      // session max OutBytes (= per-second counter)
		float peak_dorm = 0.f;              // session max DormancyRate
	};

	std::unordered_map<UNetConnection*, ConnState> g_conn;

	std::atomic<int> g_driver_applied{ 0 };
	std::atomic<int> g_conn_applied{ 0 };
	std::atomic<int> g_global_applied{ 0 };
	std::atomic<int> g_actor_applied{ 0 };

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

	// Глобал по имени (PDB data-символ). nullptr, если не найден.
	template <typename T>
	T* SafeGlobal(const char* name)
	{
		return GetNativeDataPointerField<T*>(name);
	}

	// ------------------------------------------------------------------
	// Конфиг
	// ------------------------------------------------------------------

	void WriteDefaultConfig()
	{
		std::ofstream out(g_config_path);
		if (!out.is_open())
			return;

		out << R"({
  "enabled": true,
  "mode": "adaptive",
  "applyIntervalSeconds": 10.0,
  "statusIntervalSeconds": 60.0,

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
		g_cfg.apply_interval_seconds = j.value("applyIntervalSeconds", g_cfg.apply_interval_seconds);
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

	// ------------------------------------------------------------------
	// Применение
	// ------------------------------------------------------------------

	int ApplyDriver(UNetDriver* driver)
	{
		int changed = 0;
		if (g_cfg.net_server_max_tick_rate > 0)
			if (int* p = SafeFieldPtr<int>(driver, "UNetDriver.NetServerMaxTickRate"))
				if (*p != g_cfg.net_server_max_tick_rate) { *p = g_cfg.net_server_max_tick_rate; ++changed; }
		if (g_cfg.max_client_rate > 0)
			if (int* p = SafeFieldPtr<int>(driver, "UNetDriver.MaxClientRate"))
				if (*p != g_cfg.max_client_rate) { *p = g_cfg.max_client_rate; ++changed; }
		if (g_cfg.max_internet_client_rate > 0)
			if (int* p = SafeFieldPtr<int>(driver, "UNetDriver.MaxInternetClientRate"))
				if (*p != g_cfg.max_internet_client_rate) { *p = g_cfg.max_internet_client_rate; ++changed; }
		return changed;
	}

	int ApplyGlobals()
	{
		int changed = 0;
		if (g_cfg.dormancy_limit_multiplier > 0.f)
		{
			if (float* p = SafeGlobal<float>("Global.GDormancyLimitMultiplier"))
				if (*p != g_cfg.dormancy_limit_multiplier) { *p = g_cfg.dormancy_limit_multiplier; ++changed; }
		}
		if (g_cfg.disable_dormancy_throttling)
		{
			if (bool* p = SafeGlobal<bool>("Global.GNoDormancyLimit"))
				if (!*p) { *p = true; ++changed; }
		}
		return changed;
	}

	int ApplyConnectionStatic(UNetConnection* c)
	{
		int changed = 0;
		if (g_cfg.current_net_speed > 0)
			if (int* p = SafeFieldPtr<int>(c, "UPlayer.CurrentNetSpeed"))
				if (*p != g_cfg.current_net_speed) { *p = g_cfg.current_net_speed; ++changed; }
		if (g_cfg.configured_internet_speed > 0)
			if (int* p = SafeFieldPtr<int>(c, "UPlayer.ConfiguredInternetSpeed"))
				if (*p != g_cfg.configured_internet_speed) { *p = g_cfg.configured_internet_speed; ++changed; }
		if (g_cfg.configured_lan_speed > 0)
			if (int* p = SafeFieldPtr<int>(c, "UPlayer.ConfiguredLanSpeed"))
				if (*p != g_cfg.configured_lan_speed) { *p = g_cfg.configured_lan_speed; ++changed; }
		return changed;
	}

	// Адаптив на соединение: цель в [min,max], шаг вниз при потерях/лаге.
	//
	// UNetConnection::InPacketsLost/OutPacketsLost и AvgLag обновляются в
	// UNetConnection::Tick и СБРАСЫВАЮТСЯ раз в StatPeriod (1 c), поэтому
	// сэмплируем их раз в секунду и берём максимум за окно.
	int ApplyConnectionAdaptive(UNetConnection* c, ConnState& st, double now_ms)
	{
		int* speed = SafeFieldPtr<int>(c, "UPlayer.CurrentNetSpeed");
		if (!speed)
			return 0;

		// Стартуем сразу с максимума: задача - быстро отдать мир, а потери
		// уронят значение на следующей же оценке.
		if (st.applied == 0)
			st.applied = (std::max)(g_cfg.adaptive_min, (std::min)(g_cfg.adaptive_max, *speed));
		if (st.applied < g_cfg.adaptive_max && st.last_eval_ms == 0.0)
			st.applied = g_cfg.adaptive_max;
		st.applied = (std::max)(g_cfg.adaptive_min, (std::min)(g_cfg.adaptive_max, st.applied));

		unsigned int in_lost = 0, out_lost = 0;
		float avg_lag = 0.f;
		ReadField(c, "UNetConnection.InPacketsLost", in_lost);
		ReadField(c, "UNetConnection.OutPacketsLost", out_lost);
		ReadField(c, "UNetConnection.AvgLag", avg_lag);

		// Раз в секунду фиксируем максимум за окно.
		if (st.last_sample_ms == 0.0 || now_ms - st.last_sample_ms >= 1000.0)
		{
			st.last_sample_ms = now_ms;
			const unsigned int lost = in_lost + out_lost;
			if (lost > st.window_lost_max)
				st.window_lost_max = lost;
			if (avg_lag > st.window_lag_max)
				st.window_lag_max = avg_lag;
		}

		int changed = 0;
		if (st.last_eval_ms == 0.0 || now_ms - st.last_eval_ms >= g_cfg.adaptive_evaluate_seconds * 1000.0)
		{
			const bool loss_bad = st.window_lost_max > g_cfg.adaptive_lost_packets;
			const bool lag_bad = g_cfg.adaptive_lag_threshold > 0.f && st.window_lag_max > g_cfg.adaptive_lag_threshold;

			int target = st.applied;
			if (loss_bad || lag_bad)
				target = (std::max)(g_cfg.adaptive_min, st.applied - g_cfg.adaptive_step_down);
			else
				target = (std::min)(g_cfg.adaptive_max, st.applied + g_cfg.adaptive_step_up);

			if (target != st.applied)
			{
				LogI("adaptive " + c->ClientGivenIPField().ToString() + ": netspeed " +
					std::to_string(st.applied) + " -> " + std::to_string(target) +
					" (lostMax=" + std::to_string(st.window_lost_max) + "/s lagMax=" +
					std::to_string(st.window_lag_max) + "s)");
				st.applied = target;
			}

			st.window_lost_max = 0;
			st.window_lag_max = 0.f;
			st.last_eval_ms = now_ms;
		}

		if (*speed != st.applied)
		{
			*speed = st.applied;
			++changed;
		}
		return changed;
	}

	int ApplyStructures()
	{
		int changed = 0;
		for (auto* structure : GetAllStructures())
		{
			if (!IsValid(structure))
				continue;

			if (g_cfg.structure_replication_interval_multiplier > 0.f)
				if (float* p = SafeFieldPtr<float>(structure, "AActor.ReplicationIntervalMultiplier"))
					if (*p != g_cfg.structure_replication_interval_multiplier)
					{ *p = g_cfg.structure_replication_interval_multiplier; ++changed; }

			if (g_cfg.structure_net_update_frequency > 0.f)
				if (float* p = SafeFieldPtr<float>(structure, "AActor.NetUpdateFrequency"))
					if (*p != g_cfg.structure_net_update_frequency)
					{ *p = g_cfg.structure_net_update_frequency; ++changed; }
		}
		return changed;
	}

	// ------------------------------------------------------------------
	// Отчёт / измерение
	// ------------------------------------------------------------------

	std::string GlobalInfo()
	{
		std::string s = "globals dormancyMult=";
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

		LogI(std::string("status(") + reason + "): tickrate=" + std::to_string(tick_rate) +
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

			ConnState& st = g_conn[c];
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

	// ------------------------------------------------------------------
	// Тик
	// ------------------------------------------------------------------

	void Tick(float delta_seconds)
	{
		(void)delta_seconds;

		static double since_apply = 0.0;
		static double since_status = 0.0;
		static double since_structures = 0.0;

		since_apply += delta_seconds;
		since_status += delta_seconds;
		since_structures += delta_seconds;

		if (g_cfg.enabled && since_apply >= g_cfg.apply_interval_seconds)
		{
			since_apply = 0.0;

			UWorld* world = ArkApi::GetApiUtils().GetWorld();
			if (world)
			{
				int driver_changed = 0, conn_changed = 0, globals_changed = 0;

				if (auto* driver = world->NetDriverField())
				{
					driver_changed = ApplyDriver(driver);

					const double now_ms = static_cast<double>(GetTickCount64());
					const bool adaptive = (g_cfg.mode == "adaptive");
					for (auto* c : driver->ClientConnectionsField())
					{
						if (!c)
							continue;
						ConnState& st = g_conn[c];
						conn_changed += adaptive ? ApplyConnectionAdaptive(c, st, now_ms)
						                         : ApplyConnectionStatic(c);
					}
				}
				globals_changed = ApplyGlobals();

				g_driver_applied.fetch_add(driver_changed);
				g_conn_applied.fetch_add(conn_changed);
				g_global_applied.fetch_add(globals_changed);

				if (driver_changed + conn_changed + globals_changed > 0)
				{
					LogI("applied driver=" + std::to_string(driver_changed) +
						" connections=" + std::to_string(conn_changed) +
						" globals=" + std::to_string(globals_changed));
				}
			}
		}

		// Пиковый захват: OutBytes на соединении - счётчик за StatPeriod (1 c),
		// т.е. это уже байт/сек. DormancyRate показывает, работает ли троттлинг
		// новых акторов. Сэмплируем каждый тик, чтобы не пропустить всплеск
		// при загрузке базы.
		if (g_cfg.enabled)
		{
			if (UWorld* world = ArkApi::GetApiUtils().GetWorld())
			{
				if (auto* driver = world->NetDriverField())
				{
					for (auto* c : driver->ClientConnectionsField())
					{
						if (!c)
							continue;
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
				}
			}
		}

		if (g_cfg.enabled &&
			(g_cfg.structure_replication_interval_multiplier > 0.f ||
				g_cfg.structure_net_update_frequency > 0.f) &&
			since_structures >= g_cfg.structure_apply_interval_seconds)
		{
			since_structures = 0.0;
			const int changed = ApplyStructures();
			g_actor_applied.fetch_add(changed);
			if (changed > 0)
				LogI("applied structures=" + std::to_string(changed));
		}

		if (since_status >= g_cfg.status_interval_seconds)
		{
			since_status = 0.0;
			LogStatus(g_cfg.enabled ? "periodic" : "periodic (disabled)");
		}
	}

	void CmdStatus(RCONClientConnection*, RCONPacket*, UWorld*) { LogStatus("rcon"); }

	void CmdReload(RCONClientConnection*, RCONPacket*, UWorld*)
	{
		LoadConfig();
		LogI("reloaded: enabled=" + std::string(g_cfg.enabled ? "1" : "0") + " mode=" + g_cfg.mode +
			" tickrate=" + std::to_string(g_cfg.net_server_max_tick_rate) +
			" CurrentNetSpeed=" + std::to_string(g_cfg.current_net_speed) +
			" Adaptive=[" + std::to_string(g_cfg.adaptive_min) + "," + std::to_string(g_cfg.adaptive_max) + "]" +
			" dormancyMult=" + std::to_string(g_cfg.dormancy_limit_multiplier));
	}

	void CmdOn(RCONClientConnection*, RCONPacket*, UWorld*) { g_cfg.enabled = true; LogI("enabled"); LogStatus("on"); }
	void CmdOff(RCONClientConnection*, RCONPacket*, UWorld*) { g_cfg.enabled = false; LogI("disabled"); }
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
	LogI("unloaded");
}
