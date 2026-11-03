# NetTune - research notes

Everything in `README.md` is backed by the v361 binary and/or a live measurement. This file lists
what was **not** confirmed and therefore stays out of the plugin.

## Read in code only, never tested

* `AGameNetworkManager` bandwidth fields (`TotalNetBandwidth`, `MaxDynamicBandwidth`,
  `MinDynamicBandwidth`, `AdjustedNetSpeed`) - never seen in the send path.
* `GNoDormancyLimit` (`-nodormancythrottling`) - removes the throttle entirely; risky, untested.
* `NPCNetworkStasisRangeScale*` server settings - the live server has them at `0` (off).
* `AWorldSettings.bDisableSpatialReplication` - would replicate everything.
* Client view distance settings (`sg.ViewDistanceQuality`, `r.ViewDistanceScale`,
  `r.WorldLingerTiles`) - client-side, a server plugin cannot change them.
* `UNetConnection.bDisableFlushNetUntilActuallyReady` - noticed in the dump, not investigated.

## Tested, but inconclusive

* **`MaxClientRate` / `MaxInternetClientRate`** - no observable change, but `CurrentNetSpeed` was
  raised in the same run, so they were never isolated. Code reading: only `UWorld::Listen` reads
  `MaxClientRate` (and forces `60000`).
* **`AActor.NetCullDistanceSquared`** - raised then dropped to `1` for ~830 structures; the actor
  channel set did not change at all. Relevancy likely goes through
  `NetworkSpatializationParent` / `APrimalStructure::IsNetRelevantFor`; not established.
* **`AActor.NetUpdateFrequency` / `ReplicationIntervalMultiplier`** - set on ~23k structures, no
  effect; by analysis they gate only repeat updates, not the initial send.
* **`UNetConnection::IsNetReady` is dead** - no direct callers in Ghidra and `QueuedBytes <= 0`
  at a 296 KB/s peak; indirect calls were not excluded.
* **Adaptive mode** (removed) - the write path worked, but the decision path (step down on
  loss/lag) never triggered: the link stayed clean and the throttle never bound.
* **Hiding structures via cull distance** to force a reload - did not close/reopen their channels,
  which is why the experiments used real out-and-back trips.

## Derived, not directly measured

* **New actors per second** is computed from the dormancy increment, not counted. `peakDorm` is
  sampled in a `UWorld::Tick` hook that runs before the net tick, so it is the post-decay leftover
  of the previous frame: `0` (actor list exhausted) or in `(0, inc]` (budget reached). It can never
  approach `1.0` - an earlier reading of near-zero `peakDorm` as "the throttle does not bind" was
  wrong.
* **`observedFps` below `NetServerMaxTickRate`** on a loaded host is CPU/load, not an engine clamp
  (an idle server honours the value exactly). It does not affect the new-actor rate.
* **Packet-rate ceiling** (`MaxPacket` observed at `512`, `FlushNet` sends one packet per call) -
  never isolated; it did not bind at the settings tested.

## Unexplained observations

* `AvgLag` ~0.25-0.39 s on a clean link, one spike to ~4 s with zero packet loss.
* Peak `peakDorm` per configuration: `0.676` (tick 20, netspeed 14000, mult 1), `0.076`
  (tick 20, netspeed 60000, mult 1), `0.025` (mult 3), `0.180` (tick 40).
