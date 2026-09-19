# Train replication (Protocol 35)

The latest committed patch before this change (`904100f`) mirrored peds and
AnimScene objects. Its occupied-vehicle path did not discover ambient trains or
replicate the other carriages. Trains were therefore missing from world sync.

The host now samples streamed train vehicles within 350 m (or the configured
world radius if larger), including locomotives and detached carriages, subject
to the existing world graph's node budget. Stable IDs, model changes, ordered
despawn, and reconnect replay use the world graph. Each car carries position,
velocity, and full Euler rotation, so the guest preserves slopes and curves.

The guest streams each model and creates a collidable vehicle proxy. Each car
follows the host transform with up to 250 ms of prediction, then stops predicting
on stale input. The local train simulation does not own proxy movement. Train
cars bypass all pedestrian spawning, outfits, tasks, and deletion. An occupied
train is resolved against the world car instead of spawning another wagon;
occupied seats are not taken from a local player or NPC.

Once train proxies exist and the authoritative world population is ready,
guest-local train vehicles in the same radius are reversibly hidden with
collision disabled. The player's occupied local vehicle and same-process test
sources are excluded. Original vehicles are restored when outside the radius,
when no train proxies remain, and at disconnect. Proxy cleanup deletes only
bridge-created vehicles.

This is host-owned train presentation. Independent train AI, couplers, damage,
switches, braking controls, cargo, whistle audio, and passenger scripts are not
replicated as deterministic gameplay. Walking on moving cars and native
individual-car creation require a two-PC game check; automated tests cannot
establish their physics behavior. Train replication was introduced in Protocol
35 and is retained by Protocol 36 with horse gear replication. Both PCs need the
current Protocol 36 bridge and sidecar; replacing only the ASI causes a version
mismatch.

## Verification

- C++ regression covers a locomotive and two cars, rotation and velocity wire
  round trips, stale prediction, updates, late packets, reconnect replay, model
  handle reuse, despawn, and invalid train state rejection.
- Managed codec checks cover train wire parity and rejection of ped-only state,
  weapons, invalid tasks, and non-finite/out-of-range rotation.
- Compile the native ASI and run the C++ and managed self-tests.
- With two PCs, approach a moving freight/passenger train from the host while
  the guest is nearby. Compare locomotive, carriage count, spacing, curves,
  slopes, stops, and reversals. Confirm `train-cars=` in the host world sample
  log and successful entity creation on the guest (no creation retry loop).
- Join with the train already nearby; reconnect while it moves; then leave the
  radius and disconnect. Check for missing cars, duplicate local trains,
  lingering proxies, or local vehicles left hidden.
- Test entering/leaving seats, standing on cars, and transferring between cars.
  Repeat with a mission train and a detached carriage on disposable saves.

Native signatures were checked against the locally supplied Script Hook SDK
and [alloc8or's RDR3 native database](https://github.com/alloc8or/rdr3-nativedb-data).
