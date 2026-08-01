# Netcode metrics review

Review of the 18-commit metrics stack on `prometheus-cleanup` (`1b7e429994`..`ed78261545`) against its stated purpose: **identify problems with our network code, and identify problems with a player's or the server's connection.** Everything below was read off the branch as of 2026-07-26; line numbers refer to that state.

## 0. Namespaces and the global `gameid` label

Every exported series carries **`gameid`**, the game's 16-byte identifier as 32 lowercase hex chars — the same id the engine broadcasts to clients and stamps into demo files, so a scrape joins directly against a demo or an autohost record without any external mapping.

It is applied as a prometheus *constant label* on every family, set in one place: the `counterFamily` / `gaugeFamily` helpers in `InitMetrics()` (and explicitly on the histogram, which does not go through them). Adding a metric picks the label up automatically.

Constant labels are fixed when a family is registered, which forced one ordering change: `GenerateAndSendGameID()` was split into `ComputeGameID()` — pure derivation, now called just before `InitMetrics()` — and the broadcast half, left exactly where it was. Packet ordering on the wire is unchanged. `gameID` is now zero-initialised, since server-side demo playback never computes one and the label is read unconditionally; such an instance reports `gameid="000…0"`.

**Cardinality note.** This makes every series churn per game, which is precisely what the storage plan warned about for the 30-day aggregation tier ("stable keys — host/region/mode, *not* instance id"). Two things make it acceptable: the scrape target is already per-game, so its `instance` label churns identically and `gameid` adds no new dimension; and it belongs in the raw/short-retention tier, with `gameid` dropped by the `vmagent` aggregation rules before anything lands in the 30-day store. If a long-lived instance ever hosts several games in sequence (`CGameServer::Reload`), the label correctly changes with the game while `instance` would not — which is the case that makes it worth having at all.

## 1. Namespaces

Two prefixes, split by **what the metric describes**, not by which class computes it:

- **`recoil_network_`** — the transport and the links: bytes, packets, chunks, queues, response time, jitter, loss, throttling, socket errors, connection lifecycle. 59 families.
- **`recoil_server_`** — game, simulation and host state: `frame`, `speed_factor`, `wanted_speed_factor`, `paused`, `participants`, `max_lag` / `median_lag`, `max_cpu` / `median_cpu`, `player_lag_milliseconds`, `player_cpu_usage`, the three desync families, `game_start_timestamp_seconds`, `info`, `dropped_frame_time_milliseconds_total`. 17 families.

Three boundary calls worth knowing, because a reasonable person could place them either way:

- **Lag and cpu stay `recoil_server_`.** `player_lag_milliseconds` is a *game-time* measure (sim frames behind, converted to ms) that is contaminated by client CPU by design — the whole reason `player_cpu_usage` sits next to it is to tell the two apart. Neither is a property of the link. They are still the first things you reach for when a player says "lag", so if the dashboards want them under `recoil_network_` that is a defensible move, just not the honest default.
- **Connection lifecycle moved to `recoil_network_`.** Disconnect and reject rates are headline network-health numbers and belong globbed with the rest of the transport. See the funnel below.
- **`dropped_frame_time_milliseconds_total` stays `recoil_server_`.** It measures the host failing to schedule the sim, which is deliberately *not* a network signal — separating it from network causes is the entire point of it existing (§6c).

The split also renamed `netcode_loop_iterations_total` → `recoil_network_loop_iterations_total`, since the prefix made the infix stutter.

### The connection lifecycle funnel

These four are meant to be read together — a funnel from inbound attempt to teardown — so they share one stem and glob as `recoil_network_connections_*`:

| metric | meaning |
| --- | --- |
| `recoil_network_connections_attempted_total` | inbound attempts, including invalid ones — the denominator |
| `recoil_network_connections_rejected_total{reason}` | refused: `handshake`, `version`, `name`, `password`, `state`, `other` |
| `recoil_network_connections_established_total{reconnect}` | accepted and bound to a slot; `reconnect=1` for resumed sessions |
| `recoil_network_connections_closed_total{reason}` | ended: `quit`, `timeout`, `kick`, `replaced` |

**Invariant, and the cost of this choice.** Singular `recoil_network_connection_*` means "per-player, carries a `playerid` label"; plural `recoil_network_connections_*` means instance-level lifecycle counts. That is a *one-character* distinction, which is the known downside of this scheme — it is easy to misread on a dashboard and easy to typo in a query. It was chosen over a visually distinct stem (`link_*`) because the plural is the honest English and keeps the funnel adjacent to the live gauge. Two consequences follow:

- Nothing instance-level may ever be named into the singular prefix. The registration site carries a comment saying so.
- All 23 singular-prefix families are registered through `counterFamily`/`gaugeFamily`, never the unlabelled `counter`/`gauge` helpers — that is the mechanical check that the invariant still holds.

## 2. What is exported today

70 metric families:

- **47 instance-level** — always exported when `MetricsPort` is set.
- **23 per-player** — behind `MetricsPerPlayer` (default off): 20 `recoil_network_connection_*` plus `recoil_server_player_lag_milliseconds`, `player_cpu_usage`, `player_desyncs_total`.

Rough series counts for one game instance:

| mode | series | dominant contributor |
| --- | --- | --- |
| aggregate only | ~115 | `message_bytes_total` at ~50 (2 directions × ~25 live NETMSG types) |
| + per-player, 16 players | ~500 | the 23 per-player families |

Two things worth noting up front:

- In aggregate-only mode, **`recoil_network_message_bytes_total` alone is ~45% of the cardinality** — more than every link-quality metric combined. That is a fine trade if the composition question ("what is flooding the link") is one you actually ask, but it should be a conscious one.
- The stack has outgrown its storage sizing. The original napkin was ~8 series/player + ~20/instance ≈ **150 for an 8v8**; it is now ~500 for a 16-player game with per-player on, and ~115 with it off. At ~300 concurrent games that is fine either way; at the 1.0 target (~6.5k concurrent games) the vmagent stream-aggregation plan needs re-checking against the new number, not the old one.

## 3. The structural question: is aggregate-only a real deployment target?

This is the single biggest complexity driver in the stack and worth deciding before any trimming.

13 of the 23 per-player families are `sum()`-able duplicates of an instance-level family (bytes, packets, resent/redundant/duplicate/lost chunks, socket errors, throttle counters). To serve both, every counter carries a `.total` **and** a `.player` pointer (`ConnectionMetrics::DeltaCounter`), and the six signals that *aren't* summable needed a bespoke aggregate each: `max_lag`, `median_lag`, `max_cpu`, `median_cpu`, `max_response_time`, `max_response_time_spike`, `max_incoming_bandwidth_usage`. Those seven scalar gauges exist **only** because per-player might be off, and four of the seven have correctness problems (§5, §6).

Two coherent postures:

- **Aggregate-only is real** (fleet-wide dashboards on hundreds of instances, per-player enabled ad-hoc for a specific incident). Then keep the flag, but fix the max/median layer — it is the part carrying the bugs.
- **Aggregate-only is a hedge.** Then default `MetricsPerPlayer` on for dedicated servers, delete the ~10 redundant aggregate *sums* (recoverable in PromQL with `sum without(playerid)`), delete the seven max/median gauges, and delete the `.total` half of `DeltaCounter`. That removes roughly 17 families and a third of the bookkeeping in `UpdateMetrics`. 500 series/instance × 500 concurrent games is 250k series — real, but well inside a single Prometheus.

The second is the larger simplification, and the goal statement ("problems with **a player's** connection") points at the per-player half being the load-bearing one. But it is a fleet-cost decision, not a code decision.

## 4. Metrics that can only ever report zero

Six families are structurally dead, and three more describe a limiter that does not limit. That is 11 of 70 (~16%) of the surface.

### 4a. The incoming-bandwidth limiter is not wired up

`ServerReadNet` copies `aiLinkData.bandwidthUsage` into a **local** at `GameServer.cpp:2645`, mutates it (2654, 2696), and writes back only `numPacketsSent` (2704–2705). `GameParticipant::ClientLinkData::bandwidthUsage` is therefore permanently 0. Consequences:

| metric | why it is zero |
| --- | --- |
| `recoil_network_max_incoming_bandwidth_usage` | reads `pair.second.bandwidthUsage` — the stored value, always 0 |
| `recoil_network_connection_incoming_bandwidth_usage` | same |
| `recoil_network_throttle_dropped_packets_total` | `forcedDropPacket` needs `bwLimitWasReached` (always false, it is computed from the stored 0) **or** `LinkIncomingPeakBandwidth <= 0`. At the default 32768 it is never true. Zero at default config; only reachable if an operator explicitly sets peak bandwidth to 0. |
| `recoil_network_connection_throttle_dropped_packets_total` | same |
| `recoil_network_incoming_throttled_milliseconds_total` | `bwLimitIsReached` now only accumulates within a single pass, so it needs >32 KB from one link inside one ~5 ms loop iteration (≈6.5 MB/s from one client). Effectively never. |
| `recoil_network_connection_incoming_throttled_milliseconds_total` | same |

Plus `recoil_network_incoming_peak_bandwidth_limit`, `_sustained_bandwidth_limit`, `_max_waiting_packets_limit` — three constant gauges publishing the configuration of that limiter.

The code comment at `GameServer.cpp:1297` frames this as intentional: *"It is currently always zero because ServerReadNet never writes it back, which is the condition this series exists to surface."* That rationale does not hold up. A permanently-flat series surfaces nothing to anyone who does not already know the bug — and if you do know it, the metric adds nothing. There is no threshold, no alert, and no baseline that distinguishes "limiter idle" from "limiter broken".

The right move is one of:

1. **Fix it** — write `bandwidthUsage` back next to `numPacketsSent` at 2704. One line. Then all six metrics become live and the incoming throttle starts working again for the first time since 2015. This is a behaviour change to the throttle and should land as its own commit, separate from the metrics stack, because it can start dropping packets on servers that have been running without an incoming limiter for a decade.
2. **Delete all nine** and leave a comment in `ServerReadNet` pointing at the write-back bug.

Shipping a metrics stack whose 9 zero-valued families are the only documentation of a netcode bug is the worst of the three options.

### 4b. `receive_queue_depth` measures a queue that is drained by construction — FIXED

`ConnectionStats::receiveQueueDepth` is `UDPConnection::msgQueue.size()` (`UDPConnection.h:148`). `ServerReadNet` drains that queue unconditionally and completely at `GameServer.cpp:2605` (`while ((packet = playerLink->GetData()) != nullptr)`), and `UpdateMetrics` runs from `Update()` in the same loop iteration, immediately after. The queue is always empty at the sampling instant.

So `recoil_network_receive_queue_depth` and `recoil_network_connection_receive_queue_depth` were also always 0. Both are now deleted and replaced by the two queues that do back up — see §6d.

## 5. Genuine redundancy

**`recoil_network_connections` ≡ `sum(recoil_server_participants)` — DELETED.** Both were incremented under the same `p.clientLink != nullptr` guard, and every participant is classified as exactly one of player/spectator. PromQL recovers it exactly.

**`recoil_network_max_response_time_spike_milliseconds` ⊇ `recoil_network_max_response_time_milliseconds` by construction — REMOVED.** Per connection, `responseTimeMaxMs = max(bucket0, bucket1, responseMs)` and `responseTimeMs = responseMs` (`UDPConnection.h` `GetStats`), so spike ≥ smoothed for every link, and therefore for the max over links. Combined with `recoil_network_response_time_milliseconds` (a full 10-bucket histogram covering the tail fleet-wide) and now jitter (§6a), the instance level had four views of one signal. The aggregate spike gauge is deleted; `max_response_time` stays (it answers "worst *link*", which a sample histogram cannot), as do the histogram and jitter. The **per-player** `connection_response_time_spike_milliseconds` is kept — it is the only single-burst view for a specific player, and `responseTimeMaxMs[2]` in `UDPConnection` still feeds it.

**Desync is covered three times**: `desync_events_total` (episodes), `desynced_players_total` (Σ players over all episodes), `player_desyncs_total{playerid}`. The middle one has an awkward unit — an episode with 5 bad clients increments it by 5, so neither a rate nor a ratio against `desync_events_total` means much without knowing the participant count. Either drop it or redefine it as a gauge of players *currently* diverged. (Separately: desync is a sim-determinism signal, not a netcode one. It is cheap and operationally valuable, so keep it — just note it is outside the stated goal.)

**Split-name vs labelled socket errors — FIXED.** Three instance families (`socket_send_errors_total`, `socket_receive_errors_total`, `listener_receive_errors_total`) collapsed into one `recoil_network_socket_errors_total{direction, socket}`, matching the shape the per-player form already used. Same three series, one family: `{send,connection}`, `{receive,connection}`, `{receive,listener}` — the listening socket only ever receives, so there is no `{send,listener}` child.

## 6. Gaps

### 6a. Jitter — DONE

The standard triad for judging a player's connection is latency / loss / jitter. Loss was well covered (`lost_incoming_chunks`, `resent_chunks`, `duplicate_chunks_received`), latency was well covered (EWMA, spike, histogram). Jitter was absent, and it is frequently what makes a game *feel* bad at latencies that look acceptable.

Implemented as the RFC 6298 RTTVAR companion to the SRTT-style EWMA already in `SampleResponseTime`: mean deviation at 1/4 weight, updated against the *previous* smoothed value (so it is computed before the EWMA moves), seeded at `sample/2` on the first measurement. Exported as `recoil_network_connection_response_time_jitter_milliseconds{playerid}` and `recoil_network_max_response_time_jitter_milliseconds`, resolved lazily under the same `hasResponseSample` gate as the other latency gauges so an unmeasured link leaves the series absent rather than publishing a zero.

Two notes on the implementation:

- The first-sample branch now gates on `responseTimeSampled` rather than the old `responseTimeEwmaMs <= 0.0f`. That fixes a latent bug in the EWMA: a link fast enough to sample 0 ms re-seeded on every ack and never smoothed anything.
- Jitter arguably supersedes `max_response_time_spike_milliseconds` at the instance level. The spike gauge needs an outlier to have landed inside a 15–30 s window and reports one sample; jitter is continuous, smoothed, and does not decay to a stale value. If the aggregate spike gauge is dropped per §5, jitter is the replacement.

Caveat that applies to the whole latency family: the underlying measurement is chunk-send → ack, which includes client scheduling. Jitter computed on it inherits that. That is still useful — a player whose *combined* latency swings by 200 ms has a problem either way — but it is not network jitter until §6b lands.

### 6b. Network-only RTT — already designed, blocked

`doc/net-rtt-timestamp-echo.md` covers this: `response_time` conflates network with client main-thread scheduling, and the timestamp-echo design fixes it, blocked on the arrival-timestamps PR. No action here; noted so the gap is not re-derived.

### 6c. Server-side frame-time debt — DONE

**What the mechanism is.** The server paces the simulation with a fractional-frame accumulator (`CreateNewFrame`, `GameServer.cpp:3263–3273`):

```cpp
spring_time timeElapsed = currentTick - lastNewFrameTick;

if (timeElapsed > spring_msecs(200))
    timeElapsed = spring_msecs(200);        // <-- the clamp

frameTimeLeft += ((GAME_SPEED * 0.001f) * internalSpeed * timeElapsed.toMilliSecsf());
lastNewFrameTick = currentTick;             // <-- advanced by the FULL elapsed time
numNewFrames = (frameTimeLeft > 0.0f) ? int(math::ceil(frameTimeLeft)) : 0;
frameTimeLeft -= numNewFrames;
```

Each pass converts wall time into frames owed, emits that many `NEWFRAME`/`KEYFRAME` broadcasts, and carries the fraction forward. Normal operation: ~5 ms elapsed → 0.15 frames owed → emit a frame every ~6th pass.

**Where the time goes.** The clamp caps `timeElapsed` at 200 ms, but `lastNewFrameTick` is then set to the **unclamped** `currentTick`. If the thread was blocked for 800 ms, only 200 ms is converted into frames; the other 600 ms is credited to nobody. It is not deferred, not carried in `frameTimeLeft` — it is gone. Game time permanently falls 600 ms behind wall clock, and it never catches up.

**The clamp is correct behaviour.** Without it, an 800 ms stall produces `ceil(0.8 * 30) = 24` frames emitted in a single burst, broadcast to every client at once, each of which must then simulate 24 frames back-to-back — which stalls *them*, which lengthens the next server pass, which produces a bigger burst. That is the classic fixed-timestep death spiral. Dropping the debt is the standard guard. The problem is not the clamp; it is that it is **silent**.

**Why it is invisible today.** `internalSpeed` is driven by `LagProtection`, which reacts to *player* lag and *client* cpu usage. A host that cannot schedule its own netcode thread does not move it. So the dashboard reads: `speed_factor` = 1.0, every player's lag low (they are all comfortably keeping up with a server that is running slow), no packet loss, no throttling — while every player in that game is experiencing stutter. A starved host and a healthy host are indistinguishable in the current metric set, which is precisely the discrimination half the stated goal asks for.

**What causes it in production.** On a dedicated server the netcode thread is blocked for >200 ms by: host oversubscription or CPU steal on a shared VM, the box swapping, a demo-recorder flush stalling on disk, or an unusually long `ServerReadNet` pass under a packet burst. On a listen server, add contention on `gameServerMutex` with the main thread. Every one of those is an infrastructure problem, not a player problem, and each currently presents as "the game felt bad and nothing was wrong".

**Why the derived form is not good enough.** `rate(recoil_server_frame[1m])` compared against `30 * recoil_server_speed_factor` does diverge when frames are dropped, so the signal is technically recoverable. But it is a ratio of two 1 Hz gauges across scrape boundaries, and `serverFrameNum` also jumps for pause, `/skip`, and demo playback — so the derived value has several benign ways to look alarming and no way to tell them apart. A counter incremented by `(timeElapsed - 200ms)` exactly at the clamp site has none of that ambiguity: it moves if and only if the server thread lost time, and `rate()` reads directly as "milliseconds of game time dropped per second", i.e. the fraction of real time the host failed to deliver.

**What was implemented.** `recoil_server_dropped_frame_time_milliseconds_total`, incremented by `elapsedMs - 200` at the clamp site, with the literal 200 lifted to a named `maxFrameTimeStepMs` constant beside the other pacing constants so the metric and the clamp cannot drift apart.

Call-site audit, since a stale `lastNewFrameTick` would produce a false positive:

- `Update()` (`GameServer.cpp:878`) — the steady-state caller; runs every loop iteration once `gameHasStarted && !PreSimFrame()`, including while paused, so the timestamp stays fresh.
- `StartGame` (`:2939`) — resets `frameTimeLeft` and `lastNewFrameTick` immediately before its first call, so game start cannot register debt.
- `SkipTo` / `SendDemoData` (`:488`) — restamps `lastNewFrameTick` directly.
- `singlestep` action (`:3166`) and `Game.cpp:1174` (video capture) — both pass `fixedFrameTime = true`, which skips the accumulator block entirely and never reaches the clamp.
- Demo playback returns at the top of `CreateNewFrame` before the accumulator, so no debt is attributed during it.

The only caller outside `GameServer` uses `fixedFrameTime = true`, so the clamp site is server-thread-only; `prometheus::Counter` wraps a `std::atomic<double>` regardless.

**Second, smaller debt path — not counted.** On listen servers only (`#ifndef DEDICATED`, `HasLocalClient()`), `numNewFrames` is clamped again by `maxNewFrames` at `GameServer.cpp:3301` — *after* `frameTimeLeft -= numNewFrames` has already run. Frames removed by that clamp are lost the same way. That one is a deliberate throttle for a host that cannot keep up with its own sim, so it is arguably working as intended, and it is compiled out of the dedicated build. Worth counting separately only if listen-server health ever matters.

(`recoil_network_loop_iterations_total` does a related job and does it correctly — if the loop wedges entirely, the counter goes flat and `rate() == 0` is detectable. It cannot see a loop that is merely *slow*, which is what this counter covers. Its help text can be cut to the wedge-detection statement instead of the current paragraph of caveats.)

### 6d. The queues that actually back up — DONE

`reorder_queue_depth` (inbound head-of-line blocking) and `send_queue_bytes` (outbound backlog against the 64 KB/s per-link cap) replace the deleted `receive_queue_depth`; the loopback demux depth was deliberately declined because it is zero until the incoming throttle is fixed. **§8 has the full queue map, the per-queue justification, and how to read the set together** — it is the reference for anyone touching these.

One implementation note not repeated there: `send_queue_bytes` walks both deques per poll (1 Hz) rather than tracking a running total. The queues are normally near-empty and this is not on a per-packet path, so the incremental-tracking complexity would buy nothing.

### 6e. Joining `playerid` back to something

Labelling by slot id rather than name is the right call (not PII, stable within a game). But nothing exports a mapping, so per-player series are only interpretable with an external join against whatever spawned the instance. A constant-1 `recoil_server_player_info{playerid, allyteam, spectator}` gauge — deliberately without names — would make the per-player families self-sufficient without adding PII. Optional; skip it if the autohost already correlates by port.

### 6f. Splitting the stall signal out of response time — DONE

`GetStats()` used to fold the age of the oldest unacked chunk into the latency family: `responseTimeMs = max(ewma, pendingMs)`, `responseTimeMaxMs = max(bucket0, bucket1, responseMs)`, and `hasResponseSample = (responseTimeSampled || pendingMs > 0)`. The intent was sound — a link that stops acking should not keep advertising the last healthy number it managed to sample.

But it conflated a *measurement* with a *timeout*, and jitter exposed the cost. Jitter has no equivalent fallback: a link that has sent chunks and never received an ack had `responseTimeSampled == false` and `pendingMs > 0`, so `hasResponseSample` was true and jitter published as `0.0`. The dashboard then read **"latency 3000 ms, jitter 0 ms" — a perfectly steady link — precisely when the connection was dying.** That is the exact failure mode `hasResponseSample` was introduced to prevent.

Adding a second `hasJitterSample` flag would have papered over it: two flags gating three fields, and the underlying conflation left in place. The fix is to remove the conflation instead.

- All three response-time fields are now pure measurements, published together under one honest gate (`hasResponseSample == responseTimeSampled`).
- The stall signal became its own metric: `recoil_network_connection_unacked_age_milliseconds` and `recoil_network_max_unacked_age_milliseconds`, from the `OldestUnackedAgeMs()` helper that already existed to feed the old `max()`.

This is strictly *more* readable, not just more correct. Before, `response_time = 3000` was ambiguous — slow link or stalled link? Now:

| reading | meaning |
| --- | --- |
| `response_time` 40 ms, `unacked_age` 0 | healthy |
| `response_time` 400 ms, `unacked_age` low | genuinely slow link |
| `response_time` 40 ms (holding), `unacked_age` climbing | **stalled** — was fine, now nothing is coming back |
| `response_time` absent, `unacked_age` climbing | never got an ack at all; cross-check `loss_factor` for redundancy mode |

`unacked_age` needs no "has a sample" gate — 0 genuinely means nothing is outstanding — and it pairs with `unacked_chunks` as depth-and-age, the same shape as the queue metrics in §8. The RTT design doc anticipated exactly this metric ("ack-entitled stall detector … immune to the 'maybe there was nothing to send' ambiguity of raw inter-packet gaps"), so this also lands a piece of that plan early.

Residual, accepted: `response_time` is now a gauge that holds its last value while a link is stale, and Prometheus cannot distinguish "still 40 ms" from "40 ms, an hour ago". `unacked_age` is the disambiguator, which is the standard pattern for this.

## 7. Correctness notes on metrics that are staying

**`median_lag` / `median_cpu` are zero whenever `SpeedControl=2`.** `LagProtection` zeroes both at `GameServer.cpp:1478–1479` and only fills them under `curSpeedCtrl == 1` (line 1480). The help text documents this, but a metric that silently reads 0 instead of going absent is exactly the failure mode the stack correctly avoided for `response_time` (`hasResponseSample`). The fix is ~6 lines: compute the medians in `UpdateMetrics` from the same loop that already computes `maxLag`/`maxCpu`, instead of reading `LagProtection`'s leftovers. That also removes a second inconsistency — `LagProtection`'s median excludes spectators (`player.spectator` filter at line 1468) while `max_lag` in `UpdateMetrics` includes every INGAME participant, so the two aggregates are over different populations.

**`message_bytes_total` — FIXED.** It had three problems: outgoing counted only `Broadcast()`, so unicast was invisible; it scaled by recipient count at broadcast time regardless of whether each send actually happened; and the `type` label was a raw NETMSG integer with no id→name table anywhere in the tree, so every consumer needed its own copy of the enum.

All three are addressed. `GameParticipant::SendData` now returns whether it handed the packet to the link, and a new `CGameServer::SendTo` is the single choke point every server→client send goes through — so attribution covers unicast (system messages, ping replies, and the reconnect `packetCache` replay, which in a long game is the largest unicast burst there is) and cannot drift from what was actually sent. That also fixed a latent over-count: the old recipient scaling tested only `clientLink != nullptr`, while `SendData` additionally skips participants in `DISCONNECTING`. `NetMessageName()` now lives beside the enum in `NetMessageTypes.h` and yields label values like `newframe`, `luamsg`, `aicommands`; unknown ids collapse to `unknown` rather than echoing a wire byte into a label, so cardinality stays bounded by the known message set.

What remains, by design: this counts **payload** bytes while `sent_bytes_total` counts **wire** bytes including chunk and packet headers, so the two will not sum to each other. The help text now says so explicitly rather than leaving it to be discovered.

**Listen servers fold loopback traffic into the byte totals.** `inc(cm.sentBytes, stats.dataSent)` runs before the `hasLinkQuality` gate (`GameServer.cpp:1315–1323`), and `CLocalConnection` does track `dataSent`/`dataRecv` (`LocalConnection.cpp:58,83`). So on a host with a local client, that client's loopback bytes appear in `recoil_network_sent_bytes_total` as if they crossed a network — while `sent_packets_total` stays 0 for the same link, skewing bytes-per-packet. Dedicated servers are unaffected, so this is a footnote rather than a blocker, but the aggregates are not "network bytes" on a listen server.

**Naming is inconsistent for the per-player families.** Twenty use `recoil_network_connection_*`; three use `recoil_server_player_*` (`player_lag_milliseconds`, `player_cpu_usage`, `player_desyncs_total`). Arguably lag/cpu/desync are properties of the *player*, not the *link*, so the split is defensible — but then `desynced_players_total` (aggregate) vs `player_desyncs_total` (per-player) inverts the word order relative to every other pair. Pick one convention and apply it.

## 8. Reference: the netcode queues and why each is or is not metriced

Seven distinct queues sit between `CGameServer::Broadcast` and `CGameServer::ProcessPacket`. They fail in different ways and are not interchangeable, so this is the map the queue-depth metrics are chosen from.

### Outbound, server → client (all in `UDPConnection`, one set per link)

```
  CGameServer::Broadcast(pkt) / GameParticipant::SendData(pkt)
        │                                    └──► packetCache (replay log, not a send queue)
        ▼
  [1] outgoingData          deque<RawPacket>   whole messages, not yet chunked
        │   Flush(): split into <=254-byte chunks     ── gated by LinkOutgoingBandwidth
        ▼
  [2] newChunks             deque<Chunk>       chunks built, not yet on the wire
        │   SendIfNecessary(): pack into <=mtu datagrams  ── gated by LinkOutgoingBandwidth
        ├─────────────────────────► UDP socket ─────────────────────►  client
        ▼
  [3] unackedChunks         deque<Chunk>       on the wire, awaiting ack
        │   ack (lastContinuous) → AckChunks() pops + samples response time
        │   nak or ack timeout   → RequestResend()
        ▼
  [4] resendRequested       vector<Chunk>      queued for retransmission
        └─────────────────────────► UDP socket ─────────────────────►  client
                                    (capped at 20 * lossFactor per pass)
```

### Inbound, client → server

```
  client ─────► UDP socket ─────► UDPConnection::Update() → ProcessRawPacket()
        ▼
  [5] waitingPackets        vector<(chunkNum, RawPacket)>   chunks held for in-order delivery
        │   contiguous run from lastInOrder+1 is reassembled into messages;
        │   the holes it exposes are what drives nak generation
        ▼
  [6] msgQueue              deque<RawPacket>   complete messages ready to read
        │   ServerReadNet(): `while (GetData())` — drains it completely, every pass
        ▼
  [7] aiClientLinks[k]      one CLoopbackConnection msgQueue per AI id
        │   (k == MAX_AIS is the human player's own stream)
        │   ServerReadNet(): consumed in the SAME pass — unless the incoming
        │   throttle takes the Peek branch and leaves them queued
        ▼
      CGameServer::ProcessPacket()
```

### Metriced

**[1] + [2] → `send_queue_bytes`.** Both stages are gated by `LinkOutgoingBandwidth` (default **64 KB/s per link**), so this is where data piles up when the game produces faster than one client's cap allows. Depth ÷ cap = seconds that client's view is already behind — a latency the player feels that appears nowhere in the response-time family, which measures the link rather than the queue in front of it. Exported in **bytes**, not entry count, because entries range from a few bytes to an MTU and only bytes convert to time. Merged into one metric because [1] and [2] are two stages of one backlog separated by an implementation detail (unchunked vs chunked), not by any difference in what you would do about it.

**[3] → `unacked_chunks`** (pre-existing). Data that *is* on the wire with no ack coming back. Deliberately distinct from [1]/[2]: that is "we have not sent it", this is "we sent it and heard nothing". Different cause, different fix. Also feeds `OldestUnackedAgeMs()`, which is the stall detector that keeps `response_time` climbing on a link that has stopped acking entirely.

**[4] → `resend_queue_depth`** (pre-existing). The send path caps retransmissions at `20 * netLossFactor` per pass (`UDPConnection.cpp:989`) specifically because an unbounded resend queue floods the link. A queue growing past that cap *is* that hazard materialising: retransmissions are being generated faster than they can go out, which is positive feedback. This is also the one queue where our own policy can be the cause — redundancy mode duplicates every chunk — which is why `redundant_chunks_total` exists to separate the policy cost from genuine loss.

**[5] → `reorder_queue_depth`.** Inbound head-of-line blocking: chunk N is missing, N+1..N+k have arrived, nothing can be delivered until N is retransmitted. `UpdateWaitingPackets()` strips delivered entries at the end of every `ProcessRawPacket`, and the in-order run consumes contiguously from `lastInOrder + 1`, so everything left is strictly past a hole. It is the same state the netcode itself acts on — `droppedPackets`, and therefore nak generation, is derived by scanning this vector for gaps. Added to close an asymmetry: outbound had both a loss rate and a depth, inbound had only the rate (`lost_incoming_chunks_total`). That counter says loss *happened*; this gauge says delivery is stalled on it *right now*, which the player experiences as commands arriving in bursts rather than uniformly late.

### Not metriced, and why

**[6] msgQueue — deleted, was `receive_queue_depth`.** `ServerReadNet` drains it with an unbounded `while ((packet = playerLink->GetData()) != nullptr)` (`GameServer.cpp:2605`), and `UpdateMetrics` runs from `Update()` in the same loop iteration immediately after, so it is empty at every sampling instant. Note this is a property of the *server's* drain-everything pattern, not of the queue: on a client, which reads it at frame rate, the same depth would be meaningful.

**[7] loopback demux queues — deliberately declined.** This looks like the natural replacement for [6], and it is the queue `LinkIncomingMaxWaitingPackets` nominally protects. But `ServerReadNet` fills and consumes these in a single pass; the only thing that leaves packets behind is the incoming throttle taking the `Peek` branch instead of `GetData`, and that throttle is dead (§4a). Adding it today buys a third permanently-zero gauge. **It becomes worth adding the same day the `bandwidthUsage` write-back is fixed** — recorded here so the dependency is not lost.

**`packetCache` — different failure mode, wrong family.** Grows for the entire game whenever `canReconnect || allowSpecJoin` (the normal BAR configuration) and is never trimmed — it is the replay log a reconnecting client is caught up from, not a transmission queue. It gates nothing and adds no latency, so it is not a link-health signal. It *is* an unbounded memory-growth signal, and if host RSS ever matters it belongs in a memory metric family rather than this one.

### Depth is not enough: throughput and duration coverage

A depth gauge is sampled at 1 Hz while these queues turn over at the netcode loop rate — `UDPListener::Update()` calls `UDPConnection::Update()` once per iteration, so ~200 Hz at the default `ServerSleepTime=5`. **A queue that fills and drains inside one second is completely invisible to its gauge.** That is the argument for pairing each depth with something cumulative: a counter integrates, a gauge samples. It is also why the existing stack already chose a time-based counter for outbound throttling rather than a depth alone.

| queue | depth (gauge) | drain (counter) | time-backlogged (counter) | residence time |
| --- | --- | --- | --- | --- |
| [1]+[2] send | `send_queue_bytes` | `sent_bytes_total` | **`outgoing_throttled_milliseconds_total`** | `send_queue_bytes / outgoing_bandwidth_bytes_per_second` |
| [3] unacked | `unacked_chunks` | `response_time_milliseconds_count` | — | **measured directly — it is `response_time`** |
| [4] resend | `resend_queue_depth` | `resent_chunks_total` + `redundant_chunks_total` | — | `depth / rate(resent + redundant)` |
| [5] reorder | `reorder_queue_depth` | — | **`reorder_stall_milliseconds_total`** | **not derivable — use the duration counter** |

Notes on why no *fill* counter is needed for [1]–[4]:

- **[1]+[2].** Fill is application demand, which equals `sent_bytes_total` except while the link is saturated — and saturation is already signalled by `outgoing_throttled_milliseconds_total`. The drain counter is wire bytes (`dataSent += sendBuffer.size()`) against a payload-byte depth, so the ratio carries a few percent of chunk/packet header overhead; fine for a residence-time estimate, not for exact accounting.
- **[3].** Residence time in this queue *is* the send→ack response time, measured per sample rather than inferred, with `OldestUnackedAgeMs()` folded in as a floor so a link that stops acking climbs instead of freezing. Little's law would be strictly worse than what is already there. `response_time_milliseconds_count` equals chunks acked (every chunk gets a `sendTime` on first transmission, and every pop with one is sampled) — but only instance-wide, since the histogram is aggregate-only; there is no per-link chunks-acked counter.
- **[4].** Fill = drain + cancellations + Δdepth. Cancellations — a chunk acked before its queued resend went out, erased via `erasedResendChunks` — are uncounted, but they are the *benign* path. The drain side already splits the diagnostically important distinction (`resent` = loss-suspected vs `redundant` = policy duplication) via the sticky `lossSuspected` flag.

### The gap that was closed: `reorder_stall_milliseconds_total` — DONE

[5] had neither a throughput counter nor a duration counter, and its residence time was not derivable from what existed: `lost_incoming_chunks_total` counts gap *events* while `reorder_queue_depth` counts *chunks*, so the division has no meaning. That mattered because the depth alone cannot separate a 20 ms hiccup (invisible to the player) from a 2 s freeze (very visible), and at 1 Hz sampling it would miss most of them entirely.

Added `recoil_network_reorder_stall_milliseconds_total` (+ `recoil_network_connection_reorder_stall_milliseconds_total`), accumulated in `UDPConnection::Update()` while `waitingPackets` is non-empty. `rate()` reads as the fraction of the interval that player's inbound stream was blocked on a hole.

Implementation notes:

- The delta is now computed once per `Update()` into a local and shared with the outbound throttle accumulator sitting immediately above it; `lastThrottleSampleTime` was renamed `lastDurationSampleTime` to reflect that it serves both. The two duration counters are the only loop-rate-sampled metrics in the stack and reading them together makes that obvious.
- Safe to test `waitingPackets.empty()` here: delivered entries are stripped by `UpdateWaitingPackets()` at the end of every `ProcessRawPacket`, and this block runs *before* this pass reads the socket, so the container is always in its cleaned state. In a healthy link every chunk is consumed by the in-order run in the same call that queued it, so the vector is empty and the counter never advances — no false positives.
- Publishing goes through a shared `incDuration` lambda in `UpdateMetrics` alongside the throttle counter, with the same reconnect clamp the `DeltaCounter`s use, and `lastReorderStallMs` is reset in `ResetDeltas()`.

A fill/drain pair was the alternative and is worse: it yields a *mean* residence over the poll interval, which smears one 2 s freeze into nothing if the rest of the interval was clean. The duration counter integrates the stall exactly, at one accumulator instead of two counters.

### Reading them together

The point of keeping four rather than one is that the combination is a differential diagnosis:

| pattern | reading |
| --- | --- |
| `send_queue_bytes` high, `unacked_chunks` low | we cannot push data out — outgoing cap too low for this game size, or one link is far slower than the rest. Server/config side. |
| `send_queue_bytes` low, `unacked_chunks` + `resend_queue_depth` high | data leaves fine but is not arriving or not being acked. Player's downlink. |
| `reorder_stall_milliseconds_total` rising, `lost_incoming_chunks_total` rising | player's uplink is losing packets; their commands stall in bursts. Read the stall counter's `rate()` — the fraction of time they were actually blocked — before `reorder_queue_depth`, which misses any stall shorter than a scrape interval. |
| `resend_queue_depth` high with `loss_factor` > 0 | redundancy mode's own duplication, not loss — cross-check `redundant_chunks_total` against `resent_chunks_total` before blaming the link. |
| all four low, `response_time` / jitter high | latency without loss or backlog — routing or distance, nothing the server can fix. |

## 9. Order of work

**Done** (70 → 75 families; nothing that survives is structurally dead except the §4a set):

- Namespace split into `recoil_network_` / `recoil_server_`, plus the `connections_*` lifecycle funnel (§1).
- Global `gameid` constant label on every family (§0).
- Deleted `receive_queue_depth` (both forms), replaced by `reorder_queue_depth` + `reorder_stall_milliseconds_total` and `send_queue_bytes`; deliberately did *not* add loopback demux depth (§4b, §6d, §8).
- Added response-time jitter, per-player and aggregate, plus the 0 ms-sample EWMA re-seed fix it exposed (§6a).
- Deleted the aggregate `max_response_time_spike_milliseconds`, now covered by jitter (§5).
- Added `recoil_server_dropped_frame_time_milliseconds_total` at the `CreateNewFrame` clamp (§6c).
- Split the stall signal out of the response-time family into `unacked_age_milliseconds` (§6f).
- `message_bytes_total` made usable and complete: NETMSG names, unicast coverage via `CGameServer::SendTo` (§7).
- Deleted `recoil_network_connections` (§5); collapsed three socket-error families into `socket_errors_total{direction, socket}` (§7).

**Purely mechanical, no judgement calls:**

1. Compute `median_lag` / `median_cpu` in `UpdateMetrics` over the same population as `max_lag` / `max_cpu` (§7).
2. Trim `loop_iterations_total`'s help text to the wedge-detection statement (§6c).

**Needs a decision, then small:**

3. Incoming limiter: fix the write-back **or** delete the nine metrics (§4a). Do not ship as-is. Fixing it also makes loopback demux depth worth adding (§6d).

**Fleet-cost decision, largest payoff:**

4. Resolve §3. If aggregate-only is a hedge rather than a target, deleting it removes ~17 families and a third of `UpdateMetrics`.

**Not yet done, and the largest remaining risk:** none of this has ever been compiled. `--target tests` is the cheapest gate — `test_UDPListener` recompiles `UDPConnection.cpp` directly (see the HACK comment in `test/CMakeLists.txt`), so it covers the file carrying most of the new logic. There is still no test that *exercises* any of it.

Net effect if the rest lands: 75 families → roughly 66 with aggregate-only kept, or ~49 without it, and every remaining family reads a live value.
