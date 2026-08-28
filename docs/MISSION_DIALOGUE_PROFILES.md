# Mission dialogue profiles

This document is the admission contract for synchronized local Story dialogue.
Every catalogued Story MissionData entry participates in the same automatic
mission-, epoch-, and checkpoint-scoped cue/ready protocol. There is no
per-mission F9 arm gate: the host starting any catalogued Story mission sends
its exact ID and asks whether that same mission is startable in the guest save.

The build-1436 source-derived candidates are tracked separately in
[Dialogue profile candidates](MISSION_DIALOGUE_PROFILE_CANDIDATES.md). A
small source-mapped set has an experimental host-only audio presenter. It creates only
catalogue-owned conversations using bridge-owned, hidden voice proxies when
the guest is not running the mission VM; it does not start, alter, or replace
the guest's Story mission.

The bridge must never synthesize ambient speech, guess a conversation root, or
advance a Rockstar conversation line. An unmapped segment keeps the guest's
normal presentation, plus the existing host objective and camera.

## Profile identity

The stable profile ID for every one of the 80 catalogued Story missions is its
MissionData hash. This prevents a cue for one mission from being accepted in
another. A profile with no admitted conversation root intentionally retains
its local vanilla dialogue; it never tries to manufacture a root or alter
playback.

Each approved segment has one immutable profile:

| Field | Requirement |
| --- | --- |
| Mission | Exact catalog MissionData ID and runtime script. |
| Build | Exact RDR2 executable version and SHA-256 used for observation. |
| Segment key | Stable mission-local transition name; never a generated decompiler function number. |
| Conversation root | Exact non-empty root string passed to the public scripted-conversation API. |
| Roles | Exact named roles, expected model hashes, and the local-ped source used to bind each role. |
| Lines | Ordered zero-based native line indices, speaker role, and an observed subtitle/audio fingerprint. |
| Checkpoint scope | The host checkpoint generation in which the profile is valid. |
| Proof | Host and guest logs/video showing the same root, roles and ordered lines. |

## Required two-PC capture

For each candidate segment:

1. Begin from matching, incomplete disposable saves and pass the existing
   matching-instance barrier.
2. Record the host mission epoch and checkpoint generation at segment entry.
3. Record the local scripted-conversation root, each cast role and model, the
   first line index, every transition, and the time relative to the host cue.
4. On the guest, confirm the matching local conversation is loaded with the
   same root and cast before any cue is accepted.
5. Repeat after a host checkpoint retry and after reconnect. A cue from the
   old epoch or checkpoint generation must be rejected.
6. Admit a profile only when the normal local conversation produces matching
   audio, subtitle, animation and lip-sync without an injected ambient line.

## Wire contract for an admitted profile

The eventual host-to-guest dialogue cue is bounded and carries only:

```
missionEpoch, checkpointGeneration, dialogueSequence,
profileId, rootId, lineIndex, hostStartTick
```

For a guest running the matching local mission, the guest accepts it only when
all of these are true:

- it has a released matching mission instance;
- mission epoch and checkpoint generation equal the current host state;
- the profile exists for the pinned game build;
- its local conversation root and role/model bindings are ready; and
- the sequence is newer than the last accepted cue.

For a companion-only guest, an admitted root may instead start the bridge-owned
audio-only scene with the profile's explicit cast names. It never starts the
guest MissionData entry, injects ambient speech, guesses a root, or changes a
vanilla conversation. Its owned scene is stopped and its hidden proxies are
deleted on a checkpoint, cinematic/loading transition, mission end, disconnect,
or unload.

## Drift policy

Before a profile starts, the guest responds `ready` only after its local root
and roles are ready. The host schedules the line at a future tick. If either
side is not ready before that tick, the profile enters its explicitly mapped
dialogue barrier; otherwise the segment falls back to local dialogue. A retry,
mission end, disconnect, epoch change, or checkpoint-generation change clears
all pending cues and readiness.

## Tester coverage

| Mission | Segment | Root | Roles | Lines | Build proof | Status |
| --- | --- | --- | --- | --- | --- | --- |
| Every catalogued Story mission | Automatic exact-ID offer, guest startability check, matching-instance barrier, completion/reward mapping | — | — | — | automated protocol coverage; live validation still needed |
| `HNT1` | host-only source-mapped presentation prototype | `RH1_TRACK_CHAT`, `RH1_TRK_FND1`–`RH1_TRK_FND3` | Arthur/Hosea | native line index | pending two-PC validation |
| `FUD1` | host-only source-mapped presentation prototype | `FUD1_FISHTALK1`–`FUD1_FISHTALK4` | Arthur/Dutch/Hosea | native line index | pending two-PC validation |
