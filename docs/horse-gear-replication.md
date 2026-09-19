# Horse gear replication (Protocol 36)

Previously, player mounts and world horses sent only their model and physical
state. The guest called the random outfit initializer after creating each horse,
so the replica could have no saddle or other source gear.

Both horse paths now include an optional bounded list of portable shop-component
hashes. The native sampler reads ready horses once per second, including an
outfit resolver fallback for script-created horses without cached shop entries.
Missing observations retain the previous sample. An explicitly observed empty
set is distinct from an unavailable appearance and can remove old gear.

The receiving bridge reconciles the list against the actual replica's shop
components: remove obsolete items, apply the source items in their original
order, and update the MetaPed variation. Sampling and application are restricted
to tack categories (saddles, blankets, bags, bridles and related accessories).
Body, mane and tail components are excluded, and it never resets the entire body.
It checks again every 500 ms, so pending asset streaming does not get mistaken
for successful application. Source changes and native respawns are retried.
Only bridge-created horses are modified; original local horses, borrowed local
scenario actors, and actors currently owned by an exact AnimScene retain their
native appearance ownership.

Components travel with the mount/world state. Spawn, update, late-join replay,
and reconnect snapshots therefore retain the gear without a separate appearance
message arriving before its horse. Both peers need the Protocol 36 bridge and
sidecar; this revision also retains the train changes from Protocol 35.

## Validation

Regression coverage includes native/managed wire parity, unknown versus empty
sets, component replacement/removal, duplicate/zero/oversized/truncated input,
non-horse rejection, local mount sampling, guest world-horse upserts and updates,
and host/sidecar reconnect snapshots retaining the newest gear.

Two-PC game validation remains required. Compare a saddled player horse in both
directions and a nearby NPC horse; change saddle/bags, remove the saddle, reconnect,
and recreate the proxy by leaving and returning. Confirm the source gear appears,
removed items disappear, and the horse's body remains visible. Inspect
`[HORSE_GEAR]` diagnostics if components fail to apply. Mounted inventory,
carried pelts/carcasses, holstered weapons, and custom raw asset/tint edits are
not represented by the shop-component list.

Native component query, resolver, apply, and remove signatures were checked
against the local SDK and the
[RDR3 native database](https://github.com/alloc8or/rdr3-nativedb-data).
The tack category allowlist follows the extracted
[horse clothing categories](https://github.com/femga/rdr3_discoveries/blob/master/clothes/cloth_hash_names.lua).
