using CoopStory.Protocol;

namespace CoopStory.Sidecar.Session;

/// <summary>
/// Host-authoritative inventory state for a co-op session.
/// A loot source is deliberately not depleted globally: it has one claim cursor per player, so every participating player can claim the same map loot exactly once.
/// </summary>
public sealed class PlayerInventoryRegistry
{
    // Keep inventory balances and per-loot claimant sets in one transaction so a retry cannot award an already claimed source twice.
    private readonly object _sync = new();
    private readonly Dictionary<NetEntityId, PlayerInventory> _inventories = [];
    private readonly Dictionary<string, HashSet<NetEntityId>> _lootClaims =
        new(StringComparer.Ordinal);

    public PlayerInventorySnapshot GetSnapshot(NetEntityId playerId)
    {
        ValidatePlayerId(playerId);
        lock (_sync)
        {
            return _inventories.TryGetValue(playerId, out var inventory)
                ? Snapshot(inventory)
                : new PlayerInventorySnapshot(
                    0m,
                    new Dictionary<string, int>(StringComparer.Ordinal));
        }
    }

    /// <summary>
    /// Claims a map loot source for one player.
    /// Repeating the same request is idempotent and never grants currency or items a second time.
    /// </summary>
    public LootClaimResult ClaimLoot(NetEntityId playerId, MapLoot loot)
    {
        ValidatePlayerId(playerId);
        ArgumentNullException.ThrowIfNull(loot);
        loot.Validate();

        lock (_sync)
        {
            if (!_lootClaims.TryGetValue(loot.LootId, out var claimants))
            {
                claimants = [];
                _lootClaims.Add(loot.LootId, claimants);
            }

            var inventory = GetOrCreateLocked(playerId);
            // Idempotence: a duplicate collection event returns the same view without mutating currency/item counts a second time.
            if (!claimants.Add(playerId))
            {
                return new LootClaimResult(
                    LootClaimStatus.AlreadyClaimed,
                    Snapshot(inventory));
            }

            checked
            {
                inventory.Money += loot.Money;
                foreach (var item in loot.Items)
                {
                    inventory.Items.TryGetValue(item.ItemId, out var current);
                    inventory.Items[item.ItemId] = current + item.Quantity;
                }
            }

            return new LootClaimResult(LootClaimStatus.Granted, Snapshot(inventory));
        }
    }

    /// <summary>Creates an isolated rollback point before a durable delivery.</summary>
    // Host delivery/persistence code can roll a pending mutation back if it fails before becoming durable/visible to the peer's bridge.
    public InventoryTransactionSnapshot CaptureTransactionSnapshot()
    {
        lock (_sync)
        {
            return new InventoryTransactionSnapshot(CaptureStateLocked());
        }
    }

    /// <summary>
    /// Restores a previously captured state after persistence or delivery fails.
    /// The host serializes these mutations, so this cannot undo an unrelated committed inventory transaction.
    /// </summary>
    public void RestoreTransactionSnapshot(InventoryTransactionSnapshot snapshot)
    {
        ArgumentNullException.ThrowIfNull(snapshot);
        lock (_sync)
        {
            RestoreStateLocked(snapshot.State);
        }
    }

    /// <summary>Captures the complete authoritative state for reconnect replay.</summary>
    public InventorySessionState CaptureReconnectState()
    {
        lock (_sync)
        {
            return CaptureStateLocked();
        }
    }

    public void RestoreReconnectState(InventorySessionState state)
    {
        ArgumentNullException.ThrowIfNull(state);
        state.Validate();
        lock (_sync)
        {
            RestoreStateLocked(state);
        }
    }

    // Persist/replay a detached deterministic snapshot, never the mutable dictionaries that continue serving a live session.
    private InventorySessionState CaptureStateLocked() =>
        new(
            _inventories.Select(pair => new PersistedPlayerInventory(
                pair.Key.Value,
                pair.Value.Money,
                new Dictionary<string, int>(pair.Value.Items, StringComparer.Ordinal)))
                .OrderBy(player => player.PlayerId).ToArray(),
            _lootClaims.Select(pair => new PersistedLootClaim(
                pair.Key,
                pair.Value.Select(player => player.Value).Order().ToArray()))
                .OrderBy(claim => claim.LootId, StringComparer.Ordinal).ToArray());

    private void RestoreStateLocked(InventorySessionState state)
    {
        _inventories.Clear();
        _lootClaims.Clear();
        foreach (var player in state.Players)
        {
            var inventory = new PlayerInventory { Money = player.Money };
            foreach (var item in player.Items)
            {
                inventory.Items.Add(item.Key, item.Value);
            }
            _inventories.Add(new NetEntityId(player.PlayerId), inventory);
        }
        foreach (var claim in state.Claims)
        {
            _lootClaims.Add(claim.LootId,
                claim.PlayerIds.Select(id => new NetEntityId(id)).ToHashSet());
        }
    }

    private PlayerInventory GetOrCreateLocked(NetEntityId playerId)
    {
        if (!_inventories.TryGetValue(playerId, out var inventory))
        {
            inventory = new PlayerInventory();
            _inventories.Add(playerId, inventory);
        }
        return inventory;
    }

    private static PlayerInventorySnapshot Snapshot(PlayerInventory inventory) =>
        new(
            inventory.Money,
            new Dictionary<string, int>(inventory.Items, StringComparer.Ordinal));

    private static void ValidatePlayerId(NetEntityId playerId)
    {
        if (!playerId.IsValid)
        {
            throw new ArgumentException("Player ID must be valid.", nameof(playerId));
        }
    }

    private sealed class PlayerInventory
    {
        public decimal Money;
        public Dictionary<string, int> Items { get; } =
            new(StringComparer.Ordinal);
    }
}

public sealed record MapLoot(
    string LootId,
    decimal Money = 0m,
    IReadOnlyList<InventoryItemStack>? ItemStacks = null)
{
    public IReadOnlyList<InventoryItemStack> Items { get; init; } =
        ItemStacks ?? Array.Empty<InventoryItemStack>();

    // Defend the persisted/reconnect format with strict caps before restoring it into live authoritative dictionaries.
    public void Validate()
    {
        if (string.IsNullOrWhiteSpace(LootId) || LootId.Length > 128)
        {
            throw new ArgumentException("Loot ID must contain 1-128 characters.", nameof(LootId));
        }
        if (Money < 0)
        {
            throw new ArgumentException("Loot money cannot be negative.", nameof(Money));
        }
        if (Items.Count > 64 || Items.Any(item =>
                string.IsNullOrWhiteSpace(item.ItemId) ||
                item.ItemId.Length > 128 ||
                item.Quantity <= 0))
        {
            throw new ArgumentException("Loot item stacks are invalid.", nameof(Items));
        }
    }
}

public sealed record InventoryItemStack(string ItemId, int Quantity);

public sealed record PlayerInventorySnapshot(
    decimal Money,
    IReadOnlyDictionary<string, int> Items);

public enum LootClaimStatus
{
    Granted,
    AlreadyClaimed
}

public sealed record LootClaimResult(
    LootClaimStatus Status,
    PlayerInventorySnapshot Inventory);

public sealed record InventoryTransactionSnapshot(InventorySessionState State);

public sealed record InventorySessionState(
    IReadOnlyList<PersistedPlayerInventory> Players,
    IReadOnlyList<PersistedLootClaim> Claims)
{
    public void Validate()
    {
        if (Players.Count > 2 || Claims.Count > 10_000 ||
            Players.Any(player => !new NetEntityId(player.PlayerId).IsValid ||
                player.Money < 0 || player.Items.Count > 256 ||
                player.Items.Any(item => string.IsNullOrWhiteSpace(item.Key) ||
                    item.Key.Length > 128 || item.Value < 0)) ||
            Claims.Any(claim => string.IsNullOrWhiteSpace(claim.LootId) ||
                claim.LootId.Length > 128 || claim.PlayerIds.Count > 2 ||
                claim.PlayerIds.Any(id => !new NetEntityId(id).IsValid)))
        {
            throw new ArgumentException("Inventory session state is invalid.", nameof(InventorySessionState));
        }
    }
}

public sealed record PersistedPlayerInventory(
    ulong PlayerId,
    decimal Money,
    IReadOnlyDictionary<string, int> Items);

public sealed record PersistedLootClaim(
    string LootId,
    IReadOnlyList<ulong> PlayerIds);
