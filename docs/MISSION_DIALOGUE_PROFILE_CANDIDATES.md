# Dialogue profile candidates from build 1436

This is **source evidence**, not a promise that every listed root plays on the
peer. The installed game is pinned to RDR2 `1.0.1491.50`; the decompiled
scripts below are from build 1436. The bridge uses a small approved subset as a
read-only host probe: it observes created/loaded/playing/line state and
exchanges an epoch- and checkpoint-scoped cue. A matching guest retains its own
vanilla playback. A companion-only guest can use only an approved,
bridge-created audio presentation with bridge-owned hidden proxies. A candidate
must still pass the 1491.50 capture described in
[Mission dialogue profiles](MISSION_DIALOGUE_PROFILES.md) before it can be
treated as a verified profile.

## What the script evidence proves

The public build-1436 scripts `script_rel/hunting1.c` and
`script_rel/feud1.c` directly pass the following strings to their local helper
which calls `AUDIO::CREATE_NEW_SCRIPTED_CONVERSATION`, binds the script-owned
cast with `AUDIO::ADD_PED_TO_CONVERSATION`, then calls
`AUDIO::START_SCRIPT_CONVERSATION` or
`AUDIO::START_PRELOADED_CONVERSATION`.

Therefore these strings are scripted-conversation roots, rather than inferred
subtitle keys. The scripts also show that line position is observable through
`AUDIO::GET_CURRENT_SCRIPTED_CONVERSATION_LINE(root)`.

This evidence does **not** prove that the roots, line numbering, or the
mission-owned cast bindings are unchanged in 1491.50. It does not authorize the
bridge to mutate a Rockstar-owned mission conversation. The separate
companion-only presentation may create and clean up only its own reviewed root
and hidden proxies; it never pauses, restarts, skips, or stops a game-owned
conversation.

## Local native-reference validation

The non-vendored local checkout of Ked29's `rdr2-scripting-environment` was
used as a declaration reference for the bridge probe. Its newer `natives.h`
confirms these exact public signatures and hashes:

| Bridge operation | Reference declaration |
| --- | --- |
| root created | `_IS_SCRIPTED_CONVERSATION_CREATED(const char*)` / `0xD89504D9D7D5057D` |
| root loaded | `IS_SCRIPTED_CONVERSATION_LOADED(const char*)` / `0xDF0D54BE7A776737` |
| root playing | `IS_SCRIPTED_CONVERSATION_PLAYING(const char*)` / `0x1ECC76792F661CF5` |
| line index | `GET_CURRENT_SCRIPTED_CONVERSATION_LINE(const char*)` / `0x480357EE890C295A` |

The host observation probe invokes only those four read-only operations. The
separate companion-only presentation uses reviewed public conversation calls
only for a bridge-owned root and bridge-created hidden proxies; it does not use
them on a game-owned conversation.
The checkout is deliberately not copied into this repository, release ZIP, or
game directory; it is a local validation source only. The reference itself
still labels these declarations `b1207`, so it is not evidence of compatibility
with the pinned 1491.50 game and does not replace runtime observation.

## Highest-value initial candidates

| Mission | Segment key | Conversation roots | Why this is a good first capture |
| --- | --- | --- | --- |
| `HNT1` / `hunting1` | `bear-tracking` | `RH1_TRACK_CHAT`, `RH1_TRK_CL1`, `RH1_TRK_FCLUE`, `RH1_TRK_FND1`–`RH1_TRK_FND4`, `RH1_TRK_FOL1`–`RH1_TRK_FOL3`, `RH1_TRK_SP1`–`RH1_TRK_SP2`, `RH1_TRK_WHOS1`–`RH1_TRK_WHOS2` | Explicit roots, a compact sequential set, and a mission-owned Hosea companion. |
| `FUD1` / `feud1` | `fishing-talk` | `FUD1_FISHTALK1`, `FUD1_FISHTALK2`, `FUD1_FISHTALK3`, `FUD1_FISHTALK4` | The script gates the roots in order and waits for the preceding root to finish. This is the best first dialogue barrier candidate. |
| `FUD1` / `feud1` | `fishing-reactions` | `FUD1_HPBITE`, `FUD1_HPHOOK`, `FUD1_HPLOSE`, `FUD1_HPCATCH`, `FUD1_HPTHROW`, `FUD1_HPKEEP`, plus the corresponding `DP*` variants | Explicit local reactions that should remain local unless a two-PC capture shows deterministic timing. |

## Direct root inventory

The following is intentionally an inventory, not a promise that every entry
will be synchronized. Random branches, player-choice branches, and reactive
barks should normally remain in the vanilla fallback path.

### HNT1 roots used by `hunting1.c`

```text
RH1_ARTH_MOUNT, RH1_BAIT_DAWDLE, RH1_BAIT_TALK, RH1_BAIT_WAIT,
RH1_BEAR_NOTRCK, RH1_BEAR_POOP, RH1_BEAR_RIDE1, RH1_BEAR_SPOT,
RH1_BEAR_WRONG, RH1_CAMP_BAIT, RH1_CAMP_LEAVE, RH1_CAMP_PACKED,
RH1_CAMP_RIDE1, RH1_CAMP_RIDE2, RH1_CAMP_WAIT, RH1_CAMP_WANDER,
RH1_CAMPARRIVE, RH1_CHECK_BAIT, RH1_COOK_EAT, RH1_DEADRAB_DWD,
RH1_EXT_LEADINH, RH1_GOODBYE, RH1_HOSEA_BEAR, RH1_HOSEA_FISH,
RH1_HSHOP_APRCH, RH1_HSHOP_DAWD, RH1_HSHOP_MOUNT, RH1_ISITGONE,
RH1_LEAVECAMP, RH1_MCS2_LO_A, RH1_MNT_WRONG, RH1_MOUNT_DAWD,
RH1_RAB_CTH, RH1_RAB_YES, RH1_RHNT_AKILL, RH1_RHNT_WEAPON,
RH1_SAD_CORRECT, RH1_SADDLEBANT, RH1_SADDLEWHY, RH1_SCREAM,
RH1_SETUP_BAIT, RH1_SPLT_DAWDLE, RH1_TRACK_CHAT, RH1_TRAIL_END,
RH1_TRK_CL1, RH1_TRK_FCLUE, RH1_TRK_FND1, RH1_TRK_FND2,
RH1_TRK_FND3, RH1_TRK_FND4, RH1_TRK_FOL1, RH1_TRK_FOL2,
RH1_TRK_FOL3, RH1_TRK_SP1, RH1_TRK_SP2, RH1_TRK_WHOS1,
RH1_TRK_WHOS2, RH1_TRNS_SDDL, RH1_UNRLYHORSE2, RH1_VRIDEBANT1,
RHUNT_HSLEEP, RHUNT_IG0_GEN, RHUNT_IG0_SELL, RHUNT_IG10_ENTA,
RHUNT_PSLEEP, RHUNT_SILVER
```

### FUD1 initial direct roots

The full FUD1 inventory is deliberately kept in the source capture rather
than copied as an executable allowlist. The useful first sequence is:

```text
FUD1_FISHTALK1, FUD1_FISHTALK2, FUD1_FISHTALK3, FUD1_FISHTALK4
```

Nearby, explicitly scripted fishing roots include `FUD1_HOS_TEASE`,
`FUD1_DUT_TEASE`, `FUD1_HWHYNOFSH`, `FUD1_DWHYNOFSH`, `FUD1_HPBITE`,
`FUD1_DPBITE`, `FUD1_HPHOOK`, `FUD1_DPHOOK`, `FUD1_HPLOSE`,
`FUD1_DPLOSE`, `FUD1_HPCATCH`, `FUD1_DPCATCH`, `FUD1_HPTHROW`,
`FUD1_DPTHROW`, `FUD1_HPKEEP`, and `FUD1_DPKEEP`.

## Current runtime guard

The current narrow approved set is `HNT1` tracking roots and `FUD1` fishing
talk roots listed in [Mission dialogue profiles](MISSION_DIALOGUE_PROFILES.md).
For every current or future root, the guard is:

1. Check whether the script-owned conversation is created/loaded/playing.
2. Record its current line index and the active mission epoch/checkpoint.
3. Require the guest to report the same root as locally loaded before the
   host emits a future-timestamped cue.
4. Let a matching guest start and animate its own conversation. A
   companion-only guest may start only the bridge-owned reviewed presentation;
   never mutate a game-owned conversation from a cue.

If either local game does not prove the root in the pinned build, that segment
falls back to vanilla dialogue and the host objective/camera presentation.
