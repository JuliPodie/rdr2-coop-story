# Poison Throwing Knife pamphlet probe

This is a guarded, throwaway-save Story Mode test of one native recipe record.
It does not copy money, inventory, consumables, weapons, horse state, or any
other private player state.

## Verified local result

On 2026-08-28, the local host ran the test with the synthetic guest active.
The bridge resolved `DOCUMENT_PAMPHLET_POISON_THROWING_KNIFE` to
`0x366089E7` (`912296423`) and recorded:

```text
mode=probe,  visible=0->0, unlocked=0->0
mode=enable, visible=0->1, unlocked=0->1
mode=probe,  visible=1->1, unlocked=1->1
```

The disposable session must be exited without saving. This proves the host's
guarded native query and setter for this game build. It does **not** prove
guest-native application, acknowledgement, reconnect replay, or save/restart
persistence.

## Repeat safely

1. Use a disposable Story Mode save where the pamphlet is unavailable.
2. Open the bridge menu with `F9` and select **Probe Poison Throwing Knife
   pamphlet**. Confirm that both values are `0->0`.
3. In a session that will be discarded, select **Enable Poison Throwing Knife
   pamphlet (test)**. Confirm that both values become `0->1`.
4. Re-run the probe and require `1->1` for both values.
5. Exit without saving. Never perform this test in Red Dead Online.

The current runtime allowlist permits only this recipe record and the proven
Repeating Shotgun entitlement. Any other capability record is logged and
rejected before journaling, network delivery, or native application.
