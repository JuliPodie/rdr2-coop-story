# Repeating Shotgun shop-unlock probe

This is a one-player Story Mode developer test for the real game unlock API.
It does not grant a weapon, change money, touch inventory, modify a save file,
or send anything to the other co-op player.

The bridge derives the unlock record at runtime from
`WEAPON_SHOTGUN_REPEATING` through `_GET_WEAPON_UNLOCK`. It then uses only the
matching `UNLOCK` getters/setters. The value printed as `unlockHash` in the log
is the concrete live unlock record for this item on the running game build.

## Prepare

1. Use a disposable Story Mode save where the Repeating Shotgun is not yet
   purchasable.
2. Close RDR2 and deploy a build containing this change. Do not test in Red
   Dead Online.
3. Start Story Mode and stand somewhere safe, outside a mission/cutscene.

## Probe before changing anything

Open the bridge menu with `F9`. Press Right once to select the test-tools
column, then press Down six times and Enter on **Probe Repeating Shotgun shop
unlock**.

The bridge log must contain one line similar to:

```text
[INFO][SHOP_UNLOCK] item=Repeating Shotgun, weaponHash=..., unlockHash=...,
mode=probe, visible=0->0, unlocked=0->0
```

## Co-op weapon entitlements

After this probe succeeded, the normal host equipment stream is treated as a
capability signal. When the guest sees a host-equipped valid weapon, its local
bridge marks that weapon visible and purchasable using the same native path.
It never gives the guest the weapon, ammunition, money, or weapon upgrades.
The regular equipment heartbeat repeats the signal after a reconnect; RDR2
persists a successfully applied entitlement in the guest's normal save.

Recipes/pamphlets are intentionally excluded: they require their own proven
live native before they can use this mechanism.

`visible` and `unlocked` can already be `1` if the player has reached that
campaign gate. In that case use a fresh earlier test save; the enable test is
not evidence if the item was already available.

The persistent log is:

```text
%LOCALAPPDATA%\RDR2CoopStory\launcher\logs\bridge.log
```

## Enable and verify live

Return to `F9`, Right once, Down seven times, and Enter on **Enable Repeating
Shotgun shop unlock (test)**. The log must change from `visible=0->1` and
`unlocked=0->1`. Then visit a gunsmith and verify that the Repeating Shotgun is
shown as purchasable. Do not buy it for this test; the purpose is purchase
eligibility only.

## Verify persistence normally

Create a normal in-game save, return to the title screen, close and restart
RDR2, then load the same save and check the gunsmith again. Record the before
and after log lines. A successful live result that does not persist is not yet
a co-op entitlement solution.

## What happens next

Only after this test proves both immediate availability and normal
save/restart persistence will the project add a host-authorized network
entitlement event. That event will contain the derived unlock hash from a
small allowlist and will be applied only to the guest. It will not copy host
money, owned weapons, consumables, horse state, or inventory quantities.
