# Protocol 20 overview

Protocol 20 is the final internal wire revision preserved in this archive. The
implementation in `src/CoopStory.Protocol` and the corresponding native bridge
code are the source of truth; this document describes the design rather than a
stable public API.

## Session establishment

1. The launcher derives a private protocol credential from the session
   password and validated host address.
2. Host and guest sidecars perform a versioned authenticated handshake.
3. The peers bind the UDP path to the authenticated session.
4. The local bridge connects to its sidecar and negotiates role, protocol,
   motion mode, and logical session generation.
5. Replication begins only after the required role and generation gates pass.

Mismatched protocol or motion modes fail closed. Credentials are never written
to normal logs or exported diagnostics.

## Transport split

Authenticated TCP carries ordering-sensitive data such as:

- handshake and role control;
- entity spawn/despawn and lifecycle changes;
- reliable player actions and interaction transactions;
- mission/cinematic state and authoritative replay batches;
- reconnect, goodbye, marker, and session-menu control.

Authenticated UDP carries high-frequency replaceable data such as:

- player transforms and interpolation samples;
- animation and motion snapshots;
- camera and world snapshots where a newer state supersedes an older one.

UDP validation rejects unauthenticated, replayed, malformed, oversized, or
wrong-session datagrams.

## Message families

The codebase contains versioned contracts for:

- session hello/goodbye, identity, and lobby state;
- player state, motion, animation graph, action, and traversal data;
- mount and host-authoritative interaction state;
- world entity, equipment, damage intent, and dependency graph updates;
- mission state, objective, camera, cinematic, MetaPed, and AnimScene data;
- peer resynchronization and reconnect replay;
- diagnostic problem markers and session controls.

Every decoder validates bounds, flags, enum values, lengths, and authority
before a payload reaches game-facing logic.

## Ordering and reconnect rules

- Sequence comparison is wrap-safe.
- Logical session generations invalidate queued work from earlier connections.
- The sidecar bounds and coalesces high-frequency delivery instead of allowing
  unbounded queues.
- Reliable authoritative state is replayed in dependency order after a
  reconnect.
- Tombstones prevent a late snapshot from recreating an entity that the host
  already removed.
- Bridge sends and receives are generation-bound across named-pipe reconnects.

## Player presentation

Player transforms are interpolated from recent validated samples. Reliable
actions use explicit begin, sustain, end, and cancel semantics so a missing end
does not leave a remote task active indefinitely. Watchdogs and session reset
logic clear stale actions.

The archived AnimGraph and AnimScene paths are experimental presentation
layers. They do not prove identical game script execution or a synchronized
campaign.

## Authority checks

The host owns world lifecycle and accepted mutations. Guest-originated intents
are validated against:

- authenticated peer identity;
- current logical session generation;
- allowed entity ownership and state;
- bounded distances, identifiers, and transaction phases;
- current feature and motion-mode negotiation.

Messages that cannot be validated are rejected instead of guessed.

## Network defaults

| Transport | Default port | Intended scope |
|---|---:|---|
| TCP | `43120` | Trusted private network only |
| UDP | `43121` | Trusted private network only |

Do not forward these ports to the public internet. There is no public server,
matchmaking service, relay, or supported WAN security model.

## Compatibility

Historical development used:

- RDR2 PC file version `1.0.1491.50`;
- Script Hook RDR2 runtime `1.0.1491.17`;
- Script Hook RDR2 SDK `1.0.1207.73`;
- project protocol `20`.

These values document the development context, not a current compatibility
promise. Do not disable version, online-mode, or protocol gates to force a test.
