using CoopStory.Protocol;

namespace CoopStory.Sidecar.Session;

internal enum WorldGraphApplyDisposition
{
    Applied,
    Duplicate,
    Stale,
    CapacityRejected,
    Ignored
}

internal readonly record struct WorldGraphRegistrySnapshot(
    int Nodes,
    int Edges,
    long Revision,
    long Applied,
    long Duplicate,
    long Stale,
    long CapacityRejected,
    long CascadedDespawns);

/// <summary>
/// Sidecar copy of the host-authoritative desired world. It never stores RDR2
/// handles: only stable NetEntityId nodes and their parent edges. The bridge
/// remains responsible for local ped handles and rendering.
/// </summary>
internal sealed class AuthoritativeWorldGraphRegistry
{
    private sealed record Node(
        WorldEntityStatePayload State,
        uint Sequence,
        ulong Tick);

    private readonly object _sync = new();
    private readonly Dictionary<NetEntityId, Node> _nodes = [];
    private readonly Dictionary<NetEntityId, uint> _sequences = [];
    private readonly int _maximumNodes;
    private long _revision;
    private long _applied;
    private long _duplicate;
    private long _stale;
    private long _capacityRejected;
    private long _cascadedDespawns;

    public AuthoritativeWorldGraphRegistry(int maximumNodes = 48)
    {
        if (maximumNodes <= 0)
        {
            throw new ArgumentOutOfRangeException(nameof(maximumNodes));
        }

        _maximumNodes = maximumNodes;
    }

    public WorldGraphApplyDisposition Apply(ProtocolEnvelope envelope)
    {
        ArgumentNullException.ThrowIfNull(envelope);
        lock (_sync)
        {
            return envelope.Type switch
            {
                MessageType.EntitySpawn or MessageType.EntityUpdate =>
                    ApplyStateLocked(envelope),
                MessageType.EntityDespawn => ApplyDespawnLocked(envelope),
                _ => WorldGraphApplyDisposition.Ignored
            };
        }
    }

    public IReadOnlyList<ProtocolEnvelope> CaptureSpawnSnapshot()
    {
        lock (_sync)
        {
            var order = _nodes.Keys
                .OrderBy(DependencyDepthLocked)
                .ThenBy(static id => id.Value)
                .ToArray();
            var result = new List<ProtocolEnvelope>(order.Length);
            foreach (var entityId in order)
            {
                var node = _nodes[entityId];
                result.Add(new ProtocolEnvelope(
                    MessageType.EntitySpawn,
                    node.Sequence,
                    node.Tick,
                    BinaryPayloadCodec.EncodeWorldEntityState(node.State)));
            }

            return result;
        }
    }

    public void Clear()
    {
        lock (_sync)
        {
            _nodes.Clear();
            _sequences.Clear();
            _revision++;
        }
    }

    public WorldGraphRegistrySnapshot ReadSnapshot()
    {
        lock (_sync)
        {
            return new WorldGraphRegistrySnapshot(
                _nodes.Count,
                _nodes.Values.Count(static node =>
                    node.State.ParentEntityId.IsValid),
                _revision,
                _applied,
                _duplicate,
                _stale,
                _capacityRejected,
                _cascadedDespawns);
        }
    }

    private WorldGraphApplyDisposition ApplyStateLocked(
        ProtocolEnvelope envelope)
    {
        var state = BinaryPayloadCodec.DecodeWorldEntityState(
            envelope.Payload.Span);
        var sequenceDisposition = ObserveSequenceLocked(
            state.EntityId,
            envelope.Sequence);
        if (sequenceDisposition is not WorldGraphApplyDisposition.Applied)
        {
            return sequenceDisposition;
        }

        if (!_nodes.ContainsKey(state.EntityId) &&
            _nodes.Count >= _maximumNodes)
        {
            _capacityRejected++;
            return WorldGraphApplyDisposition.CapacityRejected;
        }

        _nodes[state.EntityId] = new Node(
            state,
            envelope.Sequence,
            envelope.Tick);
        _revision++;
        _applied++;
        return WorldGraphApplyDisposition.Applied;
    }

    private WorldGraphApplyDisposition ApplyDespawnLocked(
        ProtocolEnvelope envelope)
    {
        var despawn = BinaryPayloadCodec.DecodeEntityDespawn(
            envelope.Payload.Span);
        var sequenceDisposition = ObserveSequenceLocked(
            despawn.EntityId,
            envelope.Sequence);
        if (sequenceDisposition is not WorldGraphApplyDisposition.Applied)
        {
            return sequenceDisposition;
        }

        var subtree = DescendantsLocked(despawn.EntityId)
            .OrderByDescending(DependencyDepthLocked)
            .ThenBy(static id => id.Value)
            .ToArray();
        foreach (var entityId in subtree)
        {
            if (entityId != despawn.EntityId)
            {
                _sequences[entityId] = envelope.Sequence;
                _cascadedDespawns++;
            }
            _nodes.Remove(entityId);
        }

        _revision++;
        _applied++;
        TrimTombstonesLocked();
        return WorldGraphApplyDisposition.Applied;
    }

    private WorldGraphApplyDisposition ObserveSequenceLocked(
        NetEntityId entityId,
        uint sequence)
    {
        if (!_sequences.TryGetValue(entityId, out var previous))
        {
            _sequences[entityId] = sequence;
            return WorldGraphApplyDisposition.Applied;
        }
        if (sequence == previous)
        {
            _duplicate++;
            return WorldGraphApplyDisposition.Duplicate;
        }
        if (!SequenceNumber.IsNewer(sequence, previous))
        {
            _stale++;
            return WorldGraphApplyDisposition.Stale;
        }

        _sequences[entityId] = sequence;
        return WorldGraphApplyDisposition.Applied;
    }

    private IEnumerable<NetEntityId> DescendantsLocked(NetEntityId root)
    {
        if (!_nodes.ContainsKey(root))
        {
            return [];
        }

        var result = new HashSet<NetEntityId> { root };
        var changed = true;
        while (changed)
        {
            changed = false;
            foreach (var (entityId, node) in _nodes)
            {
                if (!result.Contains(entityId) &&
                    result.Contains(node.State.ParentEntityId))
                {
                    result.Add(entityId);
                    changed = true;
                }
            }
        }

        return result;
    }

    private int DependencyDepthLocked(NetEntityId entityId)
    {
        var depth = 0;
        var visited = new HashSet<NetEntityId>();
        while (_nodes.TryGetValue(entityId, out var node) &&
               node.State.ParentEntityId.IsValid &&
               visited.Add(entityId) &&
               depth <= _nodes.Count)
        {
            depth++;
            entityId = node.State.ParentEntityId;
        }

        return depth;
    }

    private void TrimTombstonesLocked()
    {
        const int maximumTombstones = 128;
        if (_sequences.Count <= maximumTombstones)
        {
            return;
        }

        foreach (var entityId in _sequences.Keys
                     .Where(id => !_nodes.ContainsKey(id))
                     .Take(_sequences.Count - maximumTombstones)
                     .ToArray())
        {
            _sequences.Remove(entityId);
        }
    }
}
