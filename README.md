# NetTune

ArkApi (ARK: Survival Evolved) server plugin that speeds up delivering the world to a client
(large bases first) by tuning the ARK v361 replication levers that were **measured** to matter.

## How it works

Three values are reconciled (written only when they differ, so a steady state writes nothing):

| Value | Where | Effect |
| --- | --- | --- |
| `UPlayer.CurrentNetSpeed` | each `UNetConnection` | new-actor budget (saturates at `60000`) **and** the send-accounting drain rate, see below |
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
`60000`, so production does not grow beyond it - but `CurrentNetSpeed` has a second role with no
saturation (see the budget below). The client sends this value itself (`NMT_Netspeed`), so a "Low"
client gets `14000` and loads bases ~7.5x slower - which is what the plugin overrides.

`NetServerMaxTickRate` does **not** change the new-actor rate (it cancels out of the formula); it
trades CPU for granularity. `GDormancyLimitMultiplier` scales the rate linearly at any netspeed.

### The send-accounting budget (`IsNetReady`)

`FlushNet` adds every sent packet to the connection's `QueuedBytes`;
`UNetConnection::Tick` drains it at `Rate = CurrentNetSpeed` bytes per second, floored at
`-2*Rate*DeltaSeconds`. When `SendBuffer.Bytes + QueuedBytes >= 1`, `UChannel::IsNetReady` fails
(`ServerReplicateActors` calls it per actor; on failure the actor gets `ForceNetUpdate` and is
skipped for that tick). So after a send burst of B bytes the serial replication path is stalled
for about `B / CurrentNetSpeed` seconds - that stall is the structure flicker / stutter players
see near mega-bases. The parallel replication path (where the dormancy gate above lives) is not
IsNetReady-gated, so bursts themselves are not limited by `Rate`.

Two consequences:

* The complaint "lag near a big base" is driven by the burst size (set by the dormancy pace)
  **and** by the drain rate (set by `CurrentNetSpeed`). Lowering `CurrentNetSpeed` below 60000
  makes stalls *longer*, not shorter.
* Above `60000` the production is capped, but the drain keeps growing - `480000` cuts the
  post-burst stall window 8x compared to `60000` (and 2x compared to `240000`). This is why the
  recommended config uses it.

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
| `CurrentNetSpeed` | `60000` | per-connection value written by the plugin; drives both the new-actor budget (saturates at `60000`) and the `QueuedBytes` drain rate; `0` = leave unchanged |
| `DormancyLimitMultiplier` | `0.0` | `GDormancyLimitMultiplier`; `0.0` = leave unchanged (ARK default `1.0`) |
| `ReconcileIntervalSeconds` | `15.0` | how often the values are re-checked for drift |
| `StatusIntervalSeconds` | `60.0` | periodic status line; `0` = off |
| `BandwidthBudgetMbps` | `0.0` | per-server egress budget used for `budgetUtilPct`; `0.0` = hide it |
| `Debug` | `false` | per-connection detail in the periodic status line |

Re-applying is needed because the engine can reset the values (a client can resend `NMT_Netspeed`).
Every actual change is logged at debug level (`server_max_tick_rate changed`, `current_net_speed
changed`, `dormancy_limit_multiplier changed`), so with `Debug: false` a steady state is silent.

## Telemetry

The status line is emitted every `StatusIntervalSeconds` and on `NetTune.Status`. The summary line
is always logged; per-connection detail is only in the periodic line when `Debug` is `true`.

Per connection: `netspeed`, `queued`, `outBps`, `peakOutBps` (peak since the previous status line),
`dorm` / `peakDorm`, `cap` (the nominal ceiling `tickrate / ((f*195+30) * multiplier)`; a `peakDorm`
landing on it means the budget is binding), `lost=in/out`, `lag` (`AvgLag`, seconds; printed as `-`
while it still holds the uninitialized sentinel of a just-joined connection). Peaks are sampled
every tick, so a base-load spike is not missed.

`peakDorm > 0` means the new-actor throttle is the binding limit: after a tick `DormancyRate` is
either `0` (the actor list was exhausted before the budget was reached) or at most
`inc = 1 / ((f*195 + 30) * multiplier * DeltaSeconds)` (the budget was reached). It can never
approach `1.0`, so any non-zero peak is the ceiling. `DeltaSeconds` is the actual frame time of the
frame that set the peak, so at a target of 20 the ceiling is `20 / ((195+30) * multiplier)` and a
momentarily faster frame pushes the observed peak slightly above it (e.g. `0.0180` vs `0.0178` at
`multiplier = 5`). The new-actor rate itself does not depend on the frame rate: `inc * fps` is
always `(f*195 + 30) * multiplier` actors per second.

`totalPeakMbps` is the peak summed over all connections - this is what has to fit the link;
`budgetUtilPct` compares it against `BandwidthBudgetMbps` and is only printed when the budget is
configured.

The `queued` floor is `-2 * CurrentNetSpeed * DeltaSeconds`, so it doubles as a free fps gauge:
`fps ≈ 2 * CurrentNetSpeed / |queued|` (e.g. `-7440` at `60000` -> 16.1 fps). A floor deeper than
`-2 * CurrentNetSpeed / 20` marks a slow-frame moment.

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
  "CurrentNetSpeed": 480000,
  "DormancyLimitMultiplier": 3.0,
  "ReconcileIntervalSeconds": 5.0,
  "StatusIntervalSeconds": 300.0,
  "BandwidthBudgetMbps": 15.4
}
```

* `CurrentNetSpeed = 480000` -> the production part (`f`) saturates at `60000`, but the
  `QueuedBytes` drain runs at 480 KB/s, so a walking-near-base burst (76–180 KB/s observed)
  stalls the serial replication path for ≤ 0.38 s instead of seconds. Verified live: the
  `queued` floor tracked `-2*Rate*DeltaSeconds` at 40000/60000/120000/240000/480000, and an A/B
  (240000 vs 480000, same players) showed fps is unaffected - the drain rate has no observed
  CPU price.
* `DormancyLimitMultiplier = 3.0` -> ~675 new actors/s per client at `f = 1`; a 2000-structure
  base loads in ~3.0 s, a 40k mega-base streams for ~59 s (its demand never drops below the
  pace). **Mult 4 was player-rejected**: bigger bursts froze the client during loading even
  though the server-side stall windows stayed ≤ 0.45 s - the client-side processing of each
  burst is the binding constraint, and the `lag` tail per burst (0.4–0.84 s at 4 vs ≤ 0.42 at 3)
  was the leading indicator. The full tested ladder with per-step verdicts is in
  `TUNING-LOG.md`.
* `ReconcileIntervalSeconds = 5.0` -> a freshly joined client sits at the client-requested
  netspeed until the next reconcile; drop to `2.0` if join bursts are noticed.
* `NetServerMaxTickRate = 20` held `observedFps ~20` with 26+ players on the host; `15` is the
  CPU-conservative alternative. It does not affect base loading.
* `BandwidthBudgetMbps = 15.4` is the equal share of the shared 200 Mbit/s link (200 / 13) used
  for `budgetUtilPct`; it is a gauge, not a limiter. The summed per-map peak peaked at 35 Mbit/s
  of 200 - the link is not the limit.
* On a wipe, if `observedFps` drops at 20–30 players, step the multiplier down - netspeed is the
  shock absorber, keep it high. For the wave itself: `ReconcileIntervalSeconds 2.0`,
  `Debug true` and `StatusIntervalSeconds 30` until it settles; at 25 simultaneous loaders
  mult 3 draws ~94 Mbit/s of the shared 200 - start the wave at mult 2 if the link or host fps
  gets tight (the full wipe playbook is in `TUNING-LOG.md`).

## Build

Open `NetTune.vcxproj` (Visual Studio 2022, x64) and build `Release`. It expects the ArkApi headers
and `ArkApi.lib` at `..\..\ARK-Server-API` and the shared headers at `..\Includes` (override with
`/p:ArkApiRoot=...` and `/p:IncludesRoot=...`). Output: `build\Release\NetTune.dll`.

Deploy to `ShooterGame\Binaries\Win64\ArkApi\Plugins\NetTune\` together with `config.json` and
`PluginInfo.json` from `Configs\`.
