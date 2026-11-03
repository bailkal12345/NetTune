# NetTune

ArkApi (ARK: Survival Evolved) server plugin that speeds up delivering the world to a client
(large bases first) by tuning the ARK v361 replication levers that were **measured** to matter.

## How it works

Three values are reconciled (written only when they differ, so a steady state writes nothing):

| Value | Where | Effect |
| --- | --- | --- |
| `UPlayer.CurrentNetSpeed` | each `UNetConnection` | budget for **new** actors, see below |
| `UNetDriver.NetServerMaxTickRate` | net driver | server frame rate (`UGameEngine::GetMaxTickRate`, clamped to `[10, 120]`) |
| `GDormancyLimitMultiplier` | global | divides the new-actor increment (linear lever) |

`CurrentNetSpeed` is **not** a bytes/second cap. In `UNetDriver::ServerReplicateActors` every newly
relevant actor adds to the connection's dormancy budget:

```
DormancyRate += (1.0 / (f*195 + 30)) / DeltaSeconds / GDormancyLimitMultiplier
f = clamp((CurrentNetSpeed - 14000) / 46000, 0, 1)      // saturates at 60000
```

`DormancyRate` decays by `1.0` per tick, so new actors per second is about
`(f*195 + 30) * GDormancyLimitMultiplier`: ~30/s at `14000`, ~225/s at `60000`. `f` saturates at
`60000`, so **values above 60000 do nothing**. The client sends this value itself
(`NMT_Netspeed`, derived from its `ClientNetQuality`), so a "Low" client gets `14000` and loads
bases roughly 7.5x slower — which is exactly what the plugin overrides.

`NetServerMaxTickRate` does **not** change the new-actor rate: `tickrate` cancels out of the formula
above (a higher tick rate means proportionally fewer actors per tick). It trades CPU for granularity.

`GDormancyLimitMultiplier` is the only lever beyond `60000`; it scales the rate linearly.

### Measurements (live server, out-and-back base reloads)

`peakDorm` is the increment observed during a reload:

| tick rate | CurrentNetSpeed | multiplier | predicted | observed |
| --- | --- | --- | --- | --- |
| 20 | 14000 | 1.0 | 0.667 | 0.676 |
| 20 | 60000 | 1.0 | 0.089 | 0.076 |
| 20 | 60000 | 3.0 | 0.030 | 0.025 |
| 40 | 60000 | 1.0 | 0.178 | 0.180 |

Peak egress also scaled with the budget: **44 KB/s** at `14000` vs **296 KB/s** at `60000`.

### Deliberately not used

Measured to have no effect, so they are not configurable: `UNetDriver.MaxClientRate` /
`MaxInternetClientRate` (read only by `UWorld::Listen`, which forces `60000`),
`AGameNetworkManager` bandwidth fields, and per-actor `NetCullDistanceSquared` / `NetUpdateFrequency`
(they do not affect the initial load; changing a structure's cull distance does not even change the
set of replicated actors).

See `RESEARCH.md` for the levers that were **not** confirmed (or only read in code) and therefore
stay out of the plugin.

## Configuration

`Configs/config.json` (hot-reloadable, no restart):

| Key | Default | Meaning |
| --- | --- | --- |
| `NetServerMaxTickRate` | `20` | server frame rate; `0` = leave unchanged |
| `CurrentNetSpeed` | `60000` | per-client new-actor budget (`14000..60000`); `0` = leave unchanged |
| `DormancyLimitMultiplier` | `0.0` | `GDormancyLimitMultiplier`; `0.0` = leave unchanged (ARK default `1.0`) |
| `ReconcileIntervalSeconds` | `15.0` | how often the values are re-checked for drift |
| `StatusIntervalSeconds` | `60.0` | periodic status line (debug level); `0` = off |
| `Debug` | `false` | raise the plugin log level to debug |

Re-applying is needed because the engine can still reset the values: `UWorld::Listen` resets
`MaxClientRate` on a map load, and a client can resend `NMT_Netspeed`. Nothing is written while the
values already match.

## RCON

| Command | Effect |
| --- | --- |
| `NetTune.Reload` | re-read `config.json` and reconcile immediately |
| `NetTune.Status` | log the current values (tick rate, observed FPS, per-connection net speed) |

## Build

Open `NetTune.vcxproj` (Visual Studio 2022, x64) and build `Release`.
It links against `ArkApi.lib` from `..\..\ARK-Server-API` and includes the shared headers from
`..\Includes`. The output DLL goes to `build\Release\NetTune.dll`.

Deploy to `ShooterGame\Binaries\Win64\ArkApi\Plugins\NetTune\` together with the `config.json` and
`PluginInfo.json` from `Configs\`. If `config.json` is missing the plugin logs a warning and runs on
the defaults above (it does not create the file).
