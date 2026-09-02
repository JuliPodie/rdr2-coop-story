# Issue 1: `Environment.TickCount64` heartbeat timeout causes spurious disconnects under variable latency

**Component:** `src/CoopStory.Sidecar/Networking/LanSessionHost.cs` and `src/CoopStory.Sidecar/Networking/LanSessionGuest.cs`
**Severity:** Medium (operational, not data-loss)
**Affects:** Hamachi/virtual-network users primarily; mild effect on busy LAN

## Summary

Both `LanSessionHost.TimeoutLoopAsync` and `LanSessionGuest.TimeoutLoopAsync` use `Environment.TickCount64 - peer.LastReceivedTimestamp` as the heartbeat timeout. This clock is monotonic per-process but **not network-aware**, and it does not account for bursts of RDR2-side latency (script VM suspension, streaming/loading, photo-mode, pause-menu stalls).

When two peers are connected over Hamachi or a congested LAN, a 3-5 second tick gap on one side (e.g. RDR2 hits a loading screen and the bridge stops pumping frames) causes the other side to hit `HeartbeatTimeoutMs` and tear down the TCP connection, even though UDP may still be flowing and the session is otherwise healthy.

## Repro

1. Two PCs on a Hamachi network (variable latency 5-40 ms RTT)
2. Connect session, get into Story Mode
3. On host, open the in-game menu (full-screen pause) for ~5 seconds
4. Watch the guest's `network.host.disconnected` event fire, followed by reconnect-with-backoff

This also reproduces on LAN if host has any background CPU pressure (Discord screen share, antivirus scan, etc).

## Suggested fix direction

A simple improvement would be to:
1. Treat the heartbeat stream as the *primary* liveness signal (which it already is), but
2. Allow the timeout to absorb a known-good UDP sequence gap: if UDP packets with **newer** sequence numbers are still arriving within the same TCP session, give the TCP heartbeat a longer leash
3. Or, expose `HeartbeatTimeoutMs` to the per-session config so users on flaky networks can raise it

The current `ReconnectMinMs/MaxMs` exponential backoff in `LanSessionGuest.RunAsync` handles recovery gracefully — the issue is the *frequency* of false tear-downs, not recovery failure.

## Observed behaviour

- Frequent reconnect cycles (every 30-90s) on Hamachi with one PC under CPU load
- Doesn't lose mission progress (journals are durable), but does cause cosmetic stuttering and breaks immersion

## Notes

The `TimeoutLoopAsync` is implemented as a `PeriodicTimer` driven by `HeartbeatIntervalMs`. Both heartbeat sender and timeout checker share that timer, so the timeouts are coherent. The issue is purely in *what counts as "no heartbeat recently."*