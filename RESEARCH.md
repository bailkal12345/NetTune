# NetTune - research notes

Everything in `README.md` is backed by the v361 binary and/or a live measurement. This file
collects what was **not** confirmed, so it deliberately stays out of the plugin.

## Not tested at all (code reading only)

| Thing | What the code says | Why it is not in the plugin |
| --- | --- | --- |
| `AGameNetworkManager` bandwidth fields (`TotalNetBandwidth`, `MaxDynamicBandwidth`, `MinDynamicBandwidth`, `AdjustedNetSpeed`) | present, `AdjustedNetSpeed = MaxDynamicBandwidth` on init | never seen in the send path; never measured |
| `GNoDormancyLimit` (`-nodormancythrottling`) | removes the dormancy throttle entirely | only read in code; never tested (risky, "nuclear") |
| `[ServerSettings] NPCNetworkStasisRangeScalePlayerCountStart` / `...End` / `NPCNetworkStasisRangeScalePercentEnd` | shrink the network range of NPCs as the player count grows | the live server has them at `0` (scaling off); never tested |
| `AWorldSettings.bDisableSpatialReplication` | makes `IsNetRelevantFor` always return `true` | never tested (would replicate everything) |
| Client view distance (`sg.ViewDistanceQuality`, `r.ViewDistanceScale`, `r.WorldLingerTiles`) | client-side render distance and tile lingering | client setting; a server plugin cannot change it |
| `UNetConnection.bDisableFlushNetUntilActuallyReady` | ARK addition that suppresses `FlushNet` | noticed in the dump; not investigated |
| The "10 servers / 200 Mbit/s / 20 players" recommendation | arithmetic (200/10 Mbit, 20x60 KB/s) | analytical, not load-tested at that scale |

## Tested, but inconclusive

* **`MaxClientRate` / `MaxInternetClientRate`** - raising them (to 200000) showed no observable
  change, but that run also raised `CurrentNetSpeed`, so the two were never isolated. Code reading
  says only `UWorld::Listen` reads `MaxClientRate` (and forces `60000`).
* **`AActor.NetCullDistanceSquared` / `NetCullDistanceSquaredDormant`** - a load test raised and then
  dropped them to `1` for ~830 structures; the set of actor channels did not change at all
  (`openChannels` 145 -> 145, `DormancyRate` stayed `0`). So they appear **not** to govern structure
  relevancy. Hypothesis: relevancy goes through `NetworkSpatializationParent` /
  `APrimalStructure::IsNetRelevantFor` - the exact mechanism was not established.
* **`AActor.NetUpdateFrequency` / `ReplicationIntervalMultiplier`** - set on ~23k structures during a
  test; no effect measured. By analysis they gate only *repeat* updates, not the initial send.
* **`UNetConnection::IsNetReady` is dead** - evidence is (a) no direct callers in Ghidra and
  (b) `QueuedBytes <= 0` at a 296 KB/s peak. Indirect calls were not excluded, so this is strong
  evidence rather than proof.
* **Adaptive mode** (removed) - the write path was verified, but the decision path (step down on
  loss/lag) never triggered: the link stayed clean and the dormancy throttle never bound.

## Derived, not directly measured

* **New actors per second** is computed from the dormancy increment
  (`(f*195 + 30) * GDormancyLimitMultiplier`), not counted. `peakDorm` is sampled in a
  `UWorld::Tick` hook that runs **before** the net tick, so it is the post-decay leftover of the
  previous frame. It is either `0` (the actor list was exhausted) or in `(0, inc]` with
  `inc = tickrate / ((f*195+30) * multiplier)` (the budget was reached), so `peakDorm > 0` means the
  cap was reached. It can never approach `1.0` (the ceiling is `inc`), which is why an earlier
  version of this file wrongly read a near-zero `peakDorm` as "the throttle does not bind".
* **`observedFps` below `NetServerMaxTickRate`.** On an idle single server the engine honours the
  value exactly (`20 -> 19.99`, `15 -> 14.95`). On a loaded host it does not (`15 -> ~13`,
  `20 -> ~15`), i.e. the shortfall is CPU/load, not an engine clamp. It does not affect the
  new-actor rate (that cap is per second, not per tick).
* **Packet-rate ceiling** - `UNetConnection.MaxPacket` was observed at `512` bytes and `FlushNet`
  sends the accumulated `SendBuffer` as one packet, but the real packets/second x payload ceiling was
  never isolated (it did not bind at the settings tested).
* **The failed load-test trick** - hiding structures (cull distance -> 1) and restoring them did not
  close/reopen their channels, which is why the experiments were done with real out-and-back trips.

## Numbers seen but not explained

* `AvgLag` was ~0.25-0.39 s on a clean link and spiked to ~4 s during one load with **zero** packet
  loss - not investigated.
* `peakDorm` per configuration: `0.076` (tick 20, netspeed 60000, mult 1), `0.676`
  (tick 20, netspeed 14000), `0.025` (tick 20, netspeed 60000, mult 3), `0.180`
  (tick 40, netspeed 60000).
