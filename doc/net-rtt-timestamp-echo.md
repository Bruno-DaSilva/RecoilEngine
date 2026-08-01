# Network RTT via timestamp echo

Design note for a wire-protocol extension that gives the game server a per-client **network-only** round-trip-time measurement, suitable for the Prometheus metrics subsystem (`rts/System/Metrics/`, per-`playerid` gauges). Status: design only, blocked on the arrival-timestamps PR landing (see Dependencies).

## Problem

The server currently has no latency measurement that isolates the network from the client machine.

- `recoil_server_player_lag_milliseconds` (frame-response lag) measures the whole pipeline: network + client sim + client frame processing. Causally ambiguous: a slow laptop and a bad link look identical.
- Naive ack-timing RTT does not work in this engine: during gameplay the client's connection is pumped from the main thread (`clientNet->Update()` in `CGame::Update`, `rts/Game/Game.cpp`), so ack generation is delayed by up to one client frame — and by the full length of any main-thread hitch. During loading it is worse: the heartbeat thread (`CNetProtocol::UpdateLoop`, spawned in `rts/Game/LoadScreen.cpp`) pumps at a fixed 100 ms. Any ack-derived number is therefore transport + client scheduling, i.e. a sibling of frame-response lag, not a network measure.

Goal: an RTT metric where a 300 ms client hitch does **not** show up as a 300 ms "network" spike.

## Key enablers

1. **Kernel receive timestamps.** The kernel stamps each datagram when it is delivered into the socket buffer (interrupt/softirq time), independent of when userspace reads it. The packet can sit in the buffer through an entire sim frame; when the main thread finally drains the socket via `recvmsg`, the true arrival time comes out with it as ancillary data. This capture machinery was already designed, audited and build-verified by the arrival-timestamps program: `arrivalNanos` on `netcode::RawPacket`, the `recvmsg` swap at the single drain choke point in `UDPConnection`, per-batch kernel-clock→`spring_time` mapping with monotonic clamps, Linux `SO_TIMESTAMPNS` + Windows `SIO_TIMESTAMPING`/`WSARecvMsg` legs, and symmetric stamping on the server's `UDPListener`. This design is its second consumer.
2. **Exact engine-version lock.** The server rejects clients whose version does not match (`CGameServer::BindConnection` reference-version check; `NETWORK_VERSION` check in `HandleConnectionAttempts`, defined in `rts/Net/Protocol/BaseNetProtocol.h`). Server and clients always run identical builds, so the wire format can change atomically with an engine release — no negotiation, no compatibility matrix.

## Wire format

Piggyback on the existing UDP packet header (`netcode::Packet` in `rts/System/Net/UDPConnection.h`: lastContinuous + nak + checksum today).

- **Server → client:** +4 bytes. `ts` = server monotonic time in milliseconds, truncated to 32 bits (wraps at ~49 days; irrelevant at game length).
- **Client → server:** +7 bytes. `echoedTs` (4 B, the most recent `ts` received), `holdMs` (2 B, saturating), `flags` (1 B; bit 0 = stamp source, kernel vs fallback).

If per-packet overhead ever matters, stamp every Nth packet instead; at Recoil packet sizes and rates this is noise.

## The math

Each machine only ever compares its own clock; the client shares a duration, never a clock value, so there is no cross-machine clock synchronization.

```
client:  holdMs = send_time - arrival_time(packet that carried echoedTs)
server:  RTT    = arrival_time_of_echo_packet - echoedTs - holdMs
```

- `arrival_time` on the client is the kernel stamp (`arrivalNanos`). The client computes `holdMs` at reply serialization time, so main-thread delay between arrival and reply lands inside `holdMs` and is subtracted out. A 300 ms sim hitch inflates hold time, not RTT.
- `arrival_time_of_echo_packet` on the server is likewise the kernel stamp (the server's `UDPListener`/`UDPConnection` stamping), removing the server read-loop bias (1–5 ms, `ServerSleepTime`) as well. Both directions are then network-only.
- Stale echoes are still valid samples: if the client has not received a new `ts` for a while, `holdMs` simply grows to cover the gap and the arithmetic stays exact. Every client packet yields a usable RTT sample.
- Client state required: the most recent `(ts, arrivalNanos)` pair per connection, replaced whenever a newer stamped packet arrives.

## Fallback policy (honesty over coverage)

When the kernel stamp is unavailable for a datagram (Windows < 10 2004, per-adapter gaps — the arrival-timestamps design already handles cmsg-absent datagrams individually), the client falls back to stamping at socket-drain time. That under-measures hold and re-contaminates RTT with main-thread delay. The `flags` stamp-source bit marks these samples; the server treats kernel-stamped samples as authoritative and either discards fallback samples or tracks them as a second-class series. No silent blending.

## Server-side metric output

Sampling and smoothing live in `UDPConnection` (server side), exposed through the existing `GetStats()`/`UpdateMetrics` machinery in `CGameServer`:

- `recoil_network_connection_rtt_milliseconds{playerid}` — smoothed (e.g. EWMA) over kernel-stamped samples.
- `recoil_network_connection_rtt_min_milliseconds{playerid}` — min over a fixed rolling window (~15–30 s, maintained server-side; the pull exposer cannot signal scrape boundaries).

With these, the diagnostic differential closes: RTT high + client cpu low → network. Frame lag high + RTT flat + cpu high → client machine. Everyone degraded at once → host (cross-check the instance sim-health gauges).

Related metrics that pair with RTT but need no protocol change (can land independently): unacked-chunk backlog depth (`unackedChunks` gauge — congestion/loss buildup) and oldest-unacked-chunk age (ack-entitled stall detector: data was sent, an ack is contractually due, and it has not come — immune to the "maybe there was nothing to send" ambiguity of raw inter-packet gaps).

## Non-impacts

- **Demos:** unaffected. Demos record the NETMSG stream; raw UDP packet headers are never demo-serialized.
- **Sync:** unaffected. Nothing synced touches these fields; no NETMSG carries a stamp (arrival-timestamps program invariant).
- **Timeouts:** unchanged. `CheckTimeout` stays on `spring_gettime()` (same invariant).
- **Clients:** clients could reuse the same fields to display their own network-only ping (the reverse computation), but that is out of scope here.

## Dependencies and ordering

1. The trimmed arrival-timestamps PR lands (`arrivalNanos` on `RawPacket` + the Socket.cpp kernel-stamp legs). Not on master as of 2026-07-24.
2. This extension: header serialize/deserialize + client echo state + server sampling, shipped with a `NETWORK_VERSION`-bumping release.
3. Metrics gauges on top (small; slots into the existing per-connection metrics plumbing in `CGameServer::UpdateMetrics`).

## Open questions

- Echo cadence: every packet vs every Nth (bandwidth vs sample density; every packet is the simple default).
- Windows fallback series: drop fallback samples entirely, or export them under a separate metric name for partial visibility on old clients.
- Whether the client-side hold bookkeeping should live in `UDPConnection` (protocol-level, engine-wide) or `CNetProtocol` (client-only); protocol-level is symmetric and lets the server answer probes from other servers in future relay topologies.
