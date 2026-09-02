# Issue 3: `NetworkBridgeDeliveryPump` uses an unbounded channel; UDP/TCP stall can accumulate significant memory before the watchdog fires

**Component:** `src/CoopStory.Sidecar/Session/NetworkBridgeDeliveryPump.cs` and `src/CoopStory.Sidecar/Session/SidecarRuntime.cs`
**Severity:** Low-to-Medium (slow memory climb under specific load patterns; bounded by watchdog abort)
**Affects:** Mission-start scenarios with large `MissionData` catalog replays; any session that hits a network stall while the bridge is pumping snapshots

## Summary

`SidecarRuntime` constructs the network bridge pump with `Channel.CreateUnbounded<NetworkSessionRun>(...)`. When the in-game bridge (`ScriptHook` mod → named pipe → sidecar) floods envelopes faster than the network can drain them — most visibly at mission start, where the host publishes its entire `MissionData` catalog as a burst — the in-process queue grows without bound.

The watchdog (`BridgeSnapshotDeliveryStallAbortMs = 60_000` ms) eventually catches this and aborts the session with `_fatalSessionFailure`, but by then a single stall can allocate tens of MB of queued envelopes.

## Repro

1. Host with 80 registered `MissionData` entries starts a session
2. Throttle the network artificially (e.g. `tc qdisc add dev ham0 root netem delay 500ms loss 20%`)
3. Watch `data/unified/` process working set climb steadily during mission start, then jump when the watchdog aborts at 60s

This is reproducible but timing-dependent: a healthy LAN won't trigger it; a slow Hamachi will.

## Why it happens

`Channel.CreateUnbounded` was the right choice for *normal* steady-state traffic where the producer (bridge) and consumer (network) are matched. But during burst phases — mission start, large AnimScene definition replay, journal replay after reconnect — the producer can outrun the consumer by 10-100x for several seconds. With bounded channels, that pressure would surface as `networkBridgeQueueRejectionEvents` and a clear "slow down" signal back to the bridge; with an unbounded channel, it's just memory.

The `_networkBridgeQueueRejectionEvents` counter is incremented on bounded-channel rejection, but with an unbounded channel it never fires — the counter is essentially dead code in this configuration.

## Suggested fix direction

Switch to `BoundedChannelOptions { FullMode = BoundedChannelFullMode.Wait, Capacity = 1024 }` (or similar) and have the bridge pump's `WriteAsync` await on backpressure. This:

- Bounded memory growth (1024 envelopes × ~200 bytes = 200 KB worst case)
- Spreads the stall into per-write latency instead of an unbounded memory climb
- Lets the existing `_networkBridgeQueueRejectionEvents` counter do its job

Alternative if the latency cost is unacceptable: keep unbounded but add a hard limit (e.g. drop oldest beyond 4096 envelopes and increment a separate `networkBridgeQueueOverflowEvents` counter). The watchdog should be tightened from 60s to maybe 15s for the snapshot path.

## Notes

The watchdog constants (`BridgeCriticalDeliveryStallAbortMs = 2_000`, `BridgeSnapshotDeliveryStallAbortMs = 60_000`) suggest the author already thought about this — the issue is just that the queue itself isn't bounded.