# Issue 2: UDP source-port pinning in `UdpPeerBinding` is sticky after first accept, with no recovery on port change

**Component:** `src/CoopStory.Sidecar/Networking/UdpPeerBinding.cs` and `src/CoopStory.Sidecar/Networking/LanSessionHost.cs` (`UdpReceiveLoopAsync`)
**Severity:** Medium (operational, can persist across the whole session)
**Affects:** Users behind NAT, double-NAT, or aggressive connection-tracking routers

## Summary

`UdpPeerBinding` pins the source endpoint on first successful accept (`_pinnedEndpoint ??= new IPEndPoint(source.Address, source.Port)`), then drops every subsequent frame whose source doesn't match that pinned endpoint — even from the same authenticated TCP peer. If the peer's NAT rebinds the UDP source port mid-session (router bounce, ISP DHCP renew, Hamachi relay re-handshake), every subsequent UDP frame is rejected as `tcp-source-or-pinned-endpoint` and the network silently degrades to TCP-only, with all the bandwidth and latency implications that follow.

## Repro

1. Establish a session between two Hamachi peers
2. Force the guest's Hamachi adapter to renew (e.g. disable/re-enable the Hamachi adapter, or wait for a DHCP lease change)
3. Guest's UDP source port changes
4. Host keeps logging `network.udp.policy-rejected` with reason `tcp-source-or-pinned-endpoint`
5. No reconnect path — only the TCP heartbeat-driven reconnect loop rescues the session, and during that window the guest is effectively invisible

## Why this happens

The pin logic was added to harden against UDP spoofing (a good defensive choice for a multi-player mod with no public internet exposure). But the implementation treats the **first** endpoint observation as canonical for the lifetime of the binding, with no re-pinning path on `NetworkChanged` / Hamachi adapter event, and no graceful recovery when the TCP-side address is still valid but the UDP-side source port has shifted.

## Suggested fix direction

A minimal fix is to keep the pin but add a small recovery window:

- Track a "recently accepted endpoints" set (size 2-4) instead of one pinned endpoint
- Allow a recent endpoint to be re-pinned if it matches the authenticated TCP IP and the *expected* UDP source port has changed at least once in the last N seconds
- On `NetworkChanged` (which .NET can subscribe to via `NetworkChange.NetworkAddressChanged`), invalidate the pin and re-learn

A simpler short-term mitigation: surface a clear diagnostic log when this rejection reason fires more than 10 times in a row, with a hint to "restart session" — currently the rejection counter only logs at power-of-2 thresholds (`(count & (count - 1)) != 0`).

## Notes

The authentication check (`AuthenticatedDatagramCodec.Decode` with `expectedSenderInstanceId`) is what actually prevents spoofing — the pin is belt-and-suspenders. So relaxing the pin to track multiple endpoints is safe as long as the instance-id auth stays strict.