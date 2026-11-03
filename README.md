# NetTune

ArkApi (ARK: Survival Evolved) server plugin that speeds up delivering the world to a client
(large bases first) by tuning the ARK v361 replication levers that were **measured** to matter.

## How it works

Three values are reconciled (written only when they differ, so a steady state writes nothing):

| Value | Where | Effect |
| --- | --- | --- |
| `UPlayer.CurrentNetSpeed` | each `UNetConnection` | budget for **new** actors, see below |
| `UNetDriver.NetServerMaxTickRate` | net driver | server frame rate (clamped to `[10, 120]` by the engine) |
| `GDormancyLimitMultiplier` | global | divides the new-actor increment (linear lever) |

`CurrentNetSpeed` is **not** a bytes/second cap. In `UNetDriver::ServerReplicateActors` every newly
relevant actor adds to the connection's dormancy budget:

```
DormancyRate += (1.0 / (f*195 + 30)) / DeltaSeconds / GDormancyLimitMultiplier
f = clamp((CurrentNetSpeed - 14000) / 46000, 0, 1)
```

`DormancyRate` decays by `1.0` per tick, so new actors per second is about
`(f*195 + 30) * GDormancyLimitMultiplier`: ~30/s at `14000`, ~225/s at `60000`. `f` saturates at
`60000`, so **values above 60000 do nothing**. The client sends this value itself (`NMT_Netspeed`),
so a "Low" client gets `14000` and loads bases ~7.5x slower - which is what the plugin overrides.

`NetServerMaxTickRate` does **not** change the new-actor rate (it cancels out of the formula); it
trades CPU for granularity. `GDormancyLimitMultiplier` is the only lever beyond `60000`.

Measured live (out-and-back base reloads, predicted vs observed `peakDorm`):

| tick rate | CurrentNetSpeed | multiplier | predicted | observed |
| --- | --- | --- | --- | --- |
| 20 | 14000 | 1.0 | 0.667 | 0.676 |
| 20 | 60000 | 1.0 | 0.089 | 0.076 |
| 20 | 60000 | 3.0 | 0.030 | 0.025 |
| 40 | 60000 | 1.0 | 0.178 | 0.180 |

Levers that were measured to do nothing (`MaxClientRate`/`MaxInternetClientRate`,
`AGameNetworkManager` fields, per-actor cull distance / update frequency) and everything only read
in code are listed in `RESEARCH.md`.

## Configuration

`Configs/config.json` (hot-reloadable via `NetTune.Reload`; if missing, the plugin logs a warning
and runs on the defaults - it does not create the file):

| Key | Default | Meaning |
| --- | --- | --- |
| `NetServerMaxTickRate` | `20` | server frame rate; `0` = leave unchanged |
| `CurrentNetSpeed` | `60000` | per-client new-actor budget (`14000..60000`); `0` = leave unchanged |
| `DormancyLimitMultiplier` | `0.0` | `GDormancyLimitMultiplier`; `0.0` = leave unchanged (ARK default `1.0`) |
| `ReconcileIntervalSeconds` | `15.0` | how often the values are re-checked for drift |
| `StatusIntervalSeconds` | `60.0` | periodic status line; `0` = off |
| `BandwidthBudgetMbps` | `0.0` | per-server egress budget used for `budgetUtilPct`; `0.0` = hide it |
| `Debug` | `false` | per-connection detail in the periodic status line |

Re-applying is needed because the engine can reset the values (a client can resend `NMT_Netspeed`).

## Telemetry

The status line is emitted every `StatusIntervalSeconds` and on `NetTune.Status`. The summary line
is always logged; per-connection detail is only in the periodic line when `Debug` is `true`.

Per connection: `netspeed`, `queued`, `outBps`, `peakOutBps` (peak since the previous status line),
`dorm` / `peakDorm`, `lost=in/out`, `lag` (`AvgLag`, seconds; `9999` is an uninitialized sentinel of
a just-joined connection). Peaks are sampled every tick, so a base-load spike is not missed.

`peakDorm > 0` means the new-actor throttle is the binding limit: after a tick `DormancyRate` is
either `0` (the actor list was exhausted before the budget was reached) or at most
`inc = tickrate / ((f*195 + 30) * multiplier)` (the budget was reached). It can never approach
`1.0`, so any non-zero peak is the ceiling.

`totalPeakMbps` is the peak summed over all connections - this is what has to fit the link;
`budgetUtilPct` compares it against `BandwidthBudgetMbps`.

## RCON

| Command | Effect |
| --- | --- |
| `NetTune.Reload` | re-read `config.json` and reconcile immediately |
| `NetTune.Status` | log the current values and the telemetry above |

## Recommended config (13 maps, 200 Mbit/s, up to 100 players)

```json
{
  "Debug": false,
  "NetServerMaxTickRate": 20,
  "CurrentNetSpeed": 60000,
  "DormancyLimitMultiplier": 6.0,
  "ReconcileIntervalSeconds": 5.0,
  "StatusIntervalSeconds": 300.0,
  "BandwidthBudgetMbps": 15.4
}
```

* `DormancyLimitMultiplier = 6.0` -> ~1350 new actors/s per client; a 2000-structure base loads in
  ~1.5 s. The ceiling was reached at multipliers 1, 2, 3 and 4; at 6 only the most populated map
  still hit it, so 12 of 13 maps are no longer throttled. Stop raising when `peakDorm` stays `0`
  or well below the ceiling - the remaining limit is then the client (rendering / streaming).
* `ReconcileIntervalSeconds = 5.0` -> a freshly joined client sits at the server default `14000`
  until the next reconcile; this shortens the slow start of every session (worst at a wipe).
* `NetServerMaxTickRate = 20` held `observedFps ~20` with 26 players on the host; `15` is the
  CPU-conservative alternative. It does not affect base loading.
* Bandwidth: the summed per-map peak was 5.3 / 8.0 / 4.4 Mbit/s of 200 at multipliers 1 / 3 / 6 -
  the link is not the limit. `CurrentNetSpeed = 40000` is the fallback if it ever is.

For testing use `Debug: true` and `StatusIntervalSeconds: 15`; the tuned values are the same.

## Build

Open `NetTune.vcxproj` (Visual Studio 2022, x64) and build `Release`. It expects the ArkApi headers
and `ArkApi.lib` at `..\..\ARK-Server-API` and the shared headers at `..\Includes` (override with
`/p:ArkApiRoot=...` and `/p:IncludesRoot=...`). Output: `build\Release\NetTune.dll`.

Deploy to `ShooterGame\Binaries\Win64\ArkApi\Plugins\NetTune\` together with `config.json` and
`PluginInfo.json` from `Configs\`.
