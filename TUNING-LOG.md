# NetTune tuning log

Live experiment notes from the Genesis production server (10 s status logging, 2026-11-03/04).
Complaint scenario: players near a huge base (**40 000+ structures**) - structures flicker /
pop-in / "прогруз" stutters. A full relevant-set load of this base at 675/s takes ~59 s of
continuous streaming, which is why `peakDorm` never left the ceiling in any test on this map.

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
| 480000 + 3 | 675 | 86–179 KB/s (same as 240000+3); sustained outBps up to 125 KB/s | ≤ 0.37 s (burst/Rate) | per-connection metrics clean: lag 0.09–0.42, no queues, zero loss; floor tracked -2*480000/fps (61–65 K) ✓; **but fps 15.7–17.7 persistent with 3 conns** — same evening phase as the 240000+4 window; needs the netspeed A/B to attribute (host load vs serial-path CPU) |
| 480000 + 4 | 900 | solo walk 18:33–18:42 (~10 min): bursts 109–216 KB/s, sustained outBps 21–106 KB/s | ≤ 0.45 s (216/480) | **player-rejected**: freezes during base loading ("клиент не успевает"); the lag tail 0.4–0.84 s per burst was the leading indicator - client-side processing is the binding constraint on burst size |
| 480000 + 3 (final) | 675 | post-revert window 18:45–18:47: bursts 76–180 KB/s | ≤ 0.38 s (180/480) | clean: lag 0.08–0.30 typical; one 2.09 spike and one 0.75 immediately after burst sequences (client processing debt draining); `peakDorm` at `inc` (0.0221–0.0257 ≈ `fps/((f*195+30)*3)`) - demand still ≥ 675/s |

Measured coefficients:

* Walking-near-base burst ≈ **0.18–0.35 KB per actor/s** (~0.22 typical).
* Approach/join burst ≈ **0.6–0.75 KB per actor/s**.
* Complaint threshold ≈ **1–1.5 s** of continuous serial-path stall per burst.

## Log reading notes

* `queued` floor = `-2 * Rate * DeltaSeconds` → instant fps estimate: `fps = 2 * Rate / |queued|`
  (e.g. `-7440` @60000 → 16.1 fps; `-20254` @240000 → 11.9 fps).
* `lag=-` means the uninitialized `AvgLag` sentinel of a just-joined connection (filtered).
* `cap = tickrate / ((f*195+30) * multiplier)`; `peakDorm ≈ cap` ⇒ the throttle is binding
  (demand exceeds the pace). `peakDorm` tracks `inc` at the **actual** frame delta: at fps 15.7
  and mult 4 the ceiling is `1/(900*0.0637) = 0.0174`, not the nominal `0.0222` - do not read a
  sub-nominal peak as "not binding"; compare against `inc` at the current fps
  (`inc ≈ fps / ((f*195+30) * multiplier)`).
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

## Full production day (2026-11-05 06:57 -> 2026-11-06 06:56, 24 h, 13 maps, final config)

Config on prod: the tuned values (480000 + 3) with debug telemetry (`Debug true`,
`StatusIntervalSeconds 10`); ~8500 statuses per map, 24k-25k per-connection samples on busy maps.
Correction: the earlier "17:15 restart" note was wrong - the servers ran the full 24 h
continuously; that timestamp was just the moment the first log snapshot was copied.

* Host fps with players: **avg 16.2-17.4 per map, max 20.0-20.1 everywhere**; single 10-s dips
  to 2.5-8.7 (host hiccups, likely saves/GC). Host per-minute egress peak **9.85 Mbit/s of 200
  (4.9%)** (03:16), average 1.9 - the link is idle. Correction: the earlier note of "17.4
  Mbit/s at 17:33" was an artifact of summing over the mixed snapshot folder (duplicate pid
  generations); the recheck over the complete server-side logs gives 9.85 max for this day.
* Config held all day: `mult=3` on every map, `netspeed=480000` dominant; `14000`/`60000` appear
  only inside the 5 s fresh-join window before reconcile.
* Per-connection: `queued` max 427 KB (drains < 1 s at 480 KB/s), several maps never went
  positive; bursts up to 1090 KB/s single-second (rare approach events, ~2.3 s stall window);
  `peakDorm` pinned at cap on loading clients.
* Real lag spikes >= 2 s: **26 windows in 24 h across 13 maps**, clustered on repeat client IPs
  (95.24.179.63, 176.210.129.119, 85.174.180.172, 89.22.177.26, 109.201.59.151,
  5.149.159.181) - client connection quality, not server-side (no queues, no loss correlation).
* Outgoing-loss windows (>= 5 packets): 179 in 24 h, max 292/101/97/95/78 packets, on the same
  heavy-loader IPs (37.99.37.39, 95.32.167.62, 109.201.59.151, 91.149.103.188,
  85.174.180.172, 193.160.224.122) - burst-induced client-side loss, not systemic.

## Days after the final config (2026-11-06 and 2026-11-07, full-history export recheck)

Source: a full export of the server-side log directory (2026-11-03 10:50 -> 2026-11-07 20:24,
~294k NetTune lines, 109 files). Debug telemetry was returned off after the 24-h observation
day: from the 06:56 restart on Nov 6 the statuses are summary-only at `StatusIntervalSeconds
300` - so no per-connection detail (lag/loss/queued) exists for these days by design.

* `mult=3` on every server on every day; no stale configs anywhere (the one suspicious span -
  server8 pid 17548 carrying `mult=1/3/5` across Nov 4-7 - is mult 3 on Nov 7; the old values
  were its Nov 4-5 ladder-era tail).
* No NetTune errors or warnings in the whole history - only periodic statuses, config loads,
  reloads and per-field `changed ->` lines.
* Nov 6 (full day): fps with players avg 15.7-17.7 per map, max 20.1; host egress peak-minute
  sum **15.4 Mbit/s (7.7%)** at 22:09, avg 1.8.
* Nov 7 (partial, to 20:24): the best day yet - fps with players avg 19.7-20.0 per map
  (single dips to 5.7); noticeably busier traffic (avg peak-sum 2.8, peak **14.3 Mbit/s** at
  16:03, one map `server4` averaging 6.05 Mbit/s with 11 peak connections). Config held.
* Whole-history anomalies stay confined to the debug-instrumented day (Nov 5) and the ladder
  test days (Nov 3-4): 17 unique lag-spike windows (>=2 s) and 185 out-loss windows (>=5 pkt),
  all on the same repeat-client IPs (plus 136.169.205.208 and 188.243.61.115 from the Nov 4
  test day). Nothing new on Nov 6-7 is measurable, and nothing points at the server.

## Current state & next steps (2026-11-04)

* **FINAL: `CurrentNetSpeed 480000, DormancyLimitMultiplier 3`** (675/s, ~3.0 s per 2000
  structures, ~59 s of streaming for the 40k mega-base). Player-tested: mult 4 froze the client
  during loading bursts; mult 3 is smooth. Metrics at 3: walk bursts 76–180 KB/s, serial stalls
  ≤ 0.38 s, lag mostly ≤ 0.3 with rare post-burst spikes (≤ 2.09, decaying).
* The lag tail per burst (0.4–0.84 s at mult 4 vs ≤ 0.42 at mult 3) turned out to be the leading
  indicator of client-side overload - watch it before players complain.
* Wipe-day overlay: `ReconcileIntervalSeconds 2.0`, `Debug true`, `StatusIntervalSeconds 30`,
  start the wave at mult 2 if 25+ players load simultaneously (link math: 25 approach bursts at
  mult 3 ≈ 94 Mbit/s of the shared 200; at mult 5 ≈ 158 - over the safe share). Return to
  `Debug false` / 300 s after the wave; keep netspeed high - it is the shock absorber.
* Wipe: netspeed is the shock absorber; if `observedFps` drops at 30 players, step the
  multiplier down, not netspeed.
* Candidate plugin feature (trigger validated by data): per-connection adaptive — when a
  connection's `QueuedBytes` grows past ~300–500 KB, temporarily lower its netspeed until drained.
* Open question for the EngineFixes developer: per-tick send budget / IsNetReady gate in the
  parallel path (the only unfixed overload source).
