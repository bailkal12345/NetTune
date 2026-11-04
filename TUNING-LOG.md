# NetTune tuning log

Live experiment notes from the Genesis production server (10 s status logging, 2026-11-03/04).
Complaint scenario: players near a huge base (structures flicker / pop-in / "прогруз" stutters).

## Verified engine model (v361)

* Dormancy throttle runs in **both** replication paths (the parallel per-connection task and
  `ServerReplicateActors`), for actors **without an existing channel**:
  `if (GNoDormancyLimit || DormancyRate <= 1.0) { DormancyRate += (1/(f*195+30))/DeltaSeconds/GDormancyLimitMultiplier; unstasis; open channel }`
  with `f = clamp((CurrentNetSpeed - 14000) / 46000, 0, 1)`. New-actor rate is
  `(f*195+30) * multiplier` per second; `DormancyRate` decays by 1.0 per connection tick.
  Evidence: `game_aquatica.c` lines 7502236 (parallel task) and 7506293 (serial path);
  `GNoDormancyLimit` is checked **before** the budget, so setting it bypasses the throttle.
* `FlushNet` sends the whole `SendBuffer` (max `MaxPacket` = 512 bytes; `SendBuffer.Max =
  MaxPacket << 3` bits, `InitSendBuffer`) and then does `QueuedBytes += size; OutBytes += size`.
* `UNetConnection::Tick`: `QueuedBytes -= Rate * DeltaSeconds`, floored at `-2 * Rate * DeltaSeconds`.
  **`Rate == CurrentNetSpeed`** — the observed floor followed every live write:
  `-4000` @40000/fps20, `-6000` @60000/fps20, `-7440` @60000/fps16, `-12000` @120000, `-24000` @240000.
* `UNetConnection::IsNetReady`: `return SendBuffer.Bytes + QueuedBytes < 1`.
* `UChannel::IsNetReady` (`NumOutRec >= 2048`, `4096` for high-traffic channels) **is called from
  `ServerReplicateActors`** (xref `0x142601d91`) — corrects the earlier "IsNetReady is dead" claim.
  On failure with no force flag the code calls `AActor::ForceNetUpdate()` (AActor vftable+0x7c0)
  and skips the actor for that tick. Channels are not closed here.
* Consequence: after a send burst of B bytes the **serial replication path is stalled for about
  `B / CurrentNetSpeed` seconds** (accounting drain). The parallel path is dormancy-gated only and
  is **not** IsNetReady-gated, so bursts are not limited by `Rate`.
* "**Above 60000 does nothing**" is wrong: `f` saturates (production stops growing) but
  `Rate = CurrentNetSpeed` is the drain, so post-burst stalls shrink linearly with it.

## Empirical ladder (single client near the mega-base)

| Config | Actors/s | Observed bursts | Serial-path stall | Feedback |
| --- | --- | --- | --- | --- |
| 60000 + 5 | 1125 | 745–848 KB/s; accounting 2.7 MB | 7–14+ s (45 s to drain) | flicker + lag up to 8.86 s |
| 40000 + 5 | 701 | 423–433 KB/s; accounting 2 MB | ~10–50 s (drain only 40 KB/s) | flicker persisted despite healthy lag |
| 60000 + 2 | 450 | 92–107 KB/s | ~1.7 s per burst | "лагает" |
| 40000 + 1 | 140 | small | — | clean but slow (14 s per 2000 structures) |
| 60000 + 1 | 225 | 30–56 KB/s walking; 453–468 KB/s joins | < 1 s | "не лагает, бывает прогруз" |
| 120000 + 1 | 225 | 62–267 KB/s tolerated | ≤ 2.2 s once | no queues at all |
| 240000 + 2 | 450 | 80–157 KB/s walking (predicted 0.4 s) | 0.3–0.65 s | clean metrics, `peakDorm` pinned at cap |
| 240000 + 3 | 675 | 86–162 KB/s walking (predicted ~148); solo-walker session 18:01–18:05: bursts 87–180 KB/s | 0.36–0.75 s | clean over 8+ min: lag 0.07–0.37, no queues ever, zero loss; fps dips to 17.8–18.6 = host load |
| 240000 + 4 | 900 | 108–213 KB/s bursts (two clients at the base simultaneously); sustained outBps 30–107 KB/s | ≤ 0.9 s per burst (model) | metrics borderline: lag up to 0.69 (vs ≤ 0.37 at mult 3), `peakDorm` pinned at cap 0.02222, fps 15.0–16.3 sustained with only 2 conns; verdict pending player feedback; fps attribution (host vs production cost) unresolved |

Measured coefficients:

* Walking-near-base burst ≈ **0.18–0.35 KB per actor/s** (~0.22 typical).
* Approach/join burst ≈ **0.6–0.75 KB per actor/s**.
* Complaint threshold ≈ **1–1.5 s** of continuous serial-path stall per burst.

## Log reading notes

* `queued` floor = `-2 * Rate * DeltaSeconds` → instant fps estimate: `fps = 2 * Rate / |queued|`
  (e.g. `-7440` @60000 → 16.1 fps; `-20254` @240000 → 11.9 fps).
* `lag=-` means the uninitialized `AvgLag` sentinel of a just-joined connection (filtered).
* `cap = tickrate / ((f*195+30) * multiplier)`; `peakDorm ≈ cap` ⇒ the throttle is binding
  (demand exceeds the pace); `peakDorm` may exceed the nominal cap by ~1–3% on faster frames.
* `AvgLag` can freeze at its last value while a connection stalls (8.86 repeated in 3 samples).
* Constant baseline `lag` 0.2–0.3 regardless of state = VPN clients (104.28.x / 31.13.x are
  Cloudflare ranges); not a server problem.
* A mission start (`MissionDisabler: started mission ...`) coincided with an fps dip
  20 → 15.8–18.5 for ~2 min (17:40) while traffic stayed small — server-side event load,
  not replication.
* Fresh joins live at the client-requested netspeed until the next reconcile
  (`ReconcileIntervalSeconds`); at 60000 default that is a 174 KB/s burst + lag 1.08 spike.

## NPCNetworkStasisRangeScale (verified in v361 binary)

`AActor::GetNetStasisAndRangeMultiplier` (dump 7115854, VA 0x142476df0; dino override 0x1406ebe60):

* Scaling applies **only to wild NPCs**: skipped when `PlayerCountEnd == 0`, actor is not an NPC,
  `TargetingTeam > 50000` (player/tribe-owned), `bUseNetworkSpatialization == 0`,
  `OverrideStasisComponentRadius > 0`; Genesis mission dinos (`bIsTemporaryMissionDino`) excluded too.
* `mult = (PercentEnd * t + (1 - t)) * base`, `t = clamp((players - Start) / (End - Start), 0, 1)`,
  cached per frame; player count from world state. Returned 1.0 is reported as 0.0 and callers
  treat `<= 0` as 1.0.
* Consumers (vftable slot 311 / offset 0x9b8, 27 call sites): `AActor::IsNetRelevant` spatial branch
  (dump ~7121420) culls NPCs beyond `NetCullDistance * mult`; the stasis manager wake check
  (dump ~7500828/7505399, `PendingUnStasisList`) wakes stasised dinos only within
  `mult^2 * 24000^2 * scale` (radius × mult, area × mult²).
* Read from `GameUserSettings.ini` `[ServerSettings]` (dump 7842022); unset values keep the
  globals at 0 and `End == 0` disables scaling entirely.
* Prod values 5 / 40 / 0.5: factor 1.0 at ≤5 players (inactive at current 3–4), 0.79 @20,
  0.64 @30, 0.50 @≥40. Complementary to NetTune: stall = burst/Rate — NetTune raises the drain,
  this shrinks the burst (fewer wild dinos waking/culling per player) and stasis CPU load.
  Visual trade-off: wild dinos appear closer as pop grows (half distance at 40 players).

## Current state & next steps (2026-11-04)

* `240000 + 3` confirmed over a dedicated 8-minute solo walk around the base (18:01–18:05):
  bursts 87–180 KB/s, serial-path stalls ≤ 0.75 s, `queued` never went positive, `lag`
  0.07–0.37, zero loss. `peakDorm = cap` throughout ⇒ demand still exceeds 675/s; the stop at
  mult 3 is set by the approach-burst risk of mult 4 (~2.6 s), not by demand.
* Recommended operating point: **`CurrentNetSpeed 240000, DormancyLimitMultiplier 3`**
  (675/s, 3.0 s per 2000 structures). Optional A/B: brief mult 4 trial with players as judges;
  revert on the first flicker complaint.
* Criteria per step: `queued` does not accumulate (≤ ~300 KB), `lag` ≤ ~1 s, no complaints.
* Wipe: netspeed is the shock absorber; if `observedFps` drops at 30 players, step the
  multiplier down, not netspeed.
* Candidate plugin feature (trigger validated by data): per-connection adaptive — when a
  connection's `QueuedBytes` grows past ~300–500 KB, temporarily lower its netspeed until drained.
* Open question for the EngineFixes developer: per-tick send budget / IsNetReady gate in the
  parallel path (the only unfixed overload source).
