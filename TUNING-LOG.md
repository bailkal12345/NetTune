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

## Current state & next steps (2026-11-04)

* `240000 + 2` is clean by every metric; `peakDorm = cap` ⇒ demand still exceeds 450/s.
* Next step: **240000 + 3** (675/s, 3.0 s per 2000 structures) — expected walk stall ~0.6 s,
  approach ~2.0 s (borderline). Stop-line: mult 4 (approach ~2.6 s).
* Criteria per step: `queued` does not accumulate (≤ ~300 KB), `lag` ≤ ~1 s, no complaints.
* Wipe: netspeed is the shock absorber; if `observedFps` drops at 30 players, step the
  multiplier down, not netspeed.
* Candidate plugin feature (trigger validated by data): per-connection adaptive — when a
  connection's `QueuedBytes` grows past ~300–500 KB, temporarily lower its netspeed until drained.
* Open question for the EngineFixes developer: per-tick send budget / IsNetReady gate in the
  parallel path (the only unfixed overload source).
