using CoopStory.Protocol;

namespace CoopStory.Sidecar.Session;

/// <summary>
/// Host-owned lifecycle gate for actions which can take ownership of a ped's physics or task graph.
/// The sender reports intent, but only an ordered, legal transition is allowed to become an authoritative action on either game bridge.
/// </summary>
internal sealed class AuthoritativePlayerActionStateMachine
{
    // Each actor/action-kind gets an independent ordered cursor, allowing an aim and a lasso to be tracked without accepting stale phase rewrites.
    private readonly object _sync = new();
    private readonly Dictionary<ActionChannel, ActionCursor> _channels = [];

    internal readonly record struct ActionChannel(
        NetEntityId ActorEntityId,
        PlayerActionKind Kind);

    internal readonly record struct ActionCursor(
        uint ActionId,
        ushort Revision,
        PlayerActionPhase Phase);

    // Peer-to-bridge delivery is generation-bound and can roll back.
    // Keep the action cursor in that same transaction so a failed delivery never burns a revision which the reconnecting bridge has not actually received.
    internal sealed record TransactionSnapshot(
        Dictionary<ActionChannel, ActionCursor> Channels);

    internal bool TryAuthorize(
        PlayerActionPayload action,
        out string rejection)
    {
        lock (_sync)
        {
            rejection = string.Empty;
            // Authorise semantic phase order before a bridge task can gain physics ownership or claim a physical target effect.
            var key = new ActionChannel(action.ActorEntityId, action.Kind);
            var terminal = IsTerminal(action.Phase);

            if ((action.Flags & PlayerActionFlags.PhysicalTargetEffect) != 0 &&
                !CanApplyPhysicalTargetEffect(action.Kind))
            {
                rejection = "physical-effect-not-valid-for-action";
                return false;
            }

            // The first frame must be a legal begin (or persistent resync snapshot); a peer cannot jump directly into an impact/bound state.
            if (!_channels.TryGetValue(key, out var current))
            {
            if (!IsStart(action))
            {
                rejection = terminal
                    ? "terminal-without-active-action"
                    : "action-must-start-with-begin-or-resync-snapshot";
                return false;
            }

            _channels[key] = new ActionCursor(
                action.ActionId,
                action.Revision,
                action.Phase);
            return true;
            }

            // Same action ID advances strictly by revision and legal phase edge.
            if (action.ActionId == current.ActionId)
            {
            if (action.Revision <= current.Revision)
            {
                rejection = "stale-or-duplicate-action-revision";
                return false;
            }
            if (IsTerminal(current.Phase))
            {
                rejection = "continuation-after-terminal-action";
                return false;
            }
            if (!CanTransition(action.Kind, current.Phase, action.Phase))
            {
                rejection = "illegal-action-phase-transition";
                return false;
            }

            _channels[key] = new ActionCursor(
                action.ActionId,
                action.Revision,
                action.Phase);
            return true;
            }

            // A successor action is also required to start cleanly; modular ID comparison keeps this correct after uint action-ID wraparound.
            if (!IsNewer(action.ActionId, current.ActionId) || !IsStart(action))
            {
            rejection = IsNewer(action.ActionId, current.ActionId)
                ? "new-action-must-start-with-begin-or-resync-snapshot"
                : "older-action-id";
            return false;
            }

            _channels[key] = new ActionCursor(
                action.ActionId,
                action.Revision,
                action.Phase);
            return true;
        }
    }

    internal void Clear()
    {
        lock (_sync)
        {
            _channels.Clear();
        }
    }

    internal TransactionSnapshot CaptureTransactionSnapshot() =>
        CaptureSnapshot();

    private TransactionSnapshot CaptureSnapshot()
    {
        lock (_sync)
        {
            return new TransactionSnapshot(
                new Dictionary<ActionChannel, ActionCursor>(_channels));
        }
    }

    // A failed generation-bound bridge delivery rolls the semantic cursor back so the peer may retry instead of being rejected as accidentally stale.
    internal void RestoreTransactionSnapshot(TransactionSnapshot snapshot)
    {
        ArgumentNullException.ThrowIfNull(snapshot);
        lock (_sync)
        {
            _channels.Clear();
            foreach (var entry in snapshot.Channels)
            {
                _channels.Add(entry.Key, entry.Value);
            }
        }
    }

    private static bool IsStart(PlayerActionPayload action) =>
        action.Phase == PlayerActionPhase.Begin ||
        (action.Phase == PlayerActionPhase.Snapshot &&
         action.Flags.HasFlag(PlayerActionFlags.ResyncSnapshot) &&
         action.Flags.HasFlag(PlayerActionFlags.Persistent));

    private static bool IsTerminal(PlayerActionPhase phase) => phase is
        PlayerActionPhase.End or PlayerActionPhase.Cancel or PlayerActionPhase.Reject;

    private static bool IsNewer(uint candidate, uint current)
    {
        var delta = candidate - current;
        return delta != 0 && delta < 0x80000000U;
    }

    private static bool CanApplyPhysicalTargetEffect(PlayerActionKind kind) => kind is
        PlayerActionKind.MeleeAttack or
        PlayerActionKind.Grapple or
        PlayerActionKind.Lasso or
        PlayerActionKind.Hogtie or
        PlayerActionKind.Knockdown;

    private static bool CanTransition(
        PlayerActionKind kind,
        PlayerActionPhase from,
        PlayerActionPhase to)
    {
        if (IsTerminal(to))
        {
            return true;
        }
        if (to == PlayerActionPhase.Snapshot)
        {
            // A snapshot only refreshes an already host-approved persistent action; it may not manufacture a new one (handled by IsStart).
            return from is PlayerActionPhase.Active or
                PlayerActionPhase.Attached or PlayerActionPhase.Sustain or
                PlayerActionPhase.Bound;
        }

        return kind switch
        {
            PlayerActionKind.Aim => from == PlayerActionPhase.Begin &&
                to is PlayerActionPhase.Active or PlayerActionPhase.Sustain ||
                from == PlayerActionPhase.Active && to == PlayerActionPhase.Sustain ||
                from == PlayerActionPhase.Sustain && to == PlayerActionPhase.Active,
            PlayerActionKind.MeleeAttack => from == PlayerActionPhase.Begin &&
                to is PlayerActionPhase.Active or PlayerActionPhase.Impact or PlayerActionPhase.Recover ||
                from == PlayerActionPhase.Active &&
                to is PlayerActionPhase.Impact or PlayerActionPhase.Recover ||
                from == PlayerActionPhase.Impact && to == PlayerActionPhase.Recover,
            PlayerActionKind.MeleeBlock => from == PlayerActionPhase.Begin &&
                to is PlayerActionPhase.Active or PlayerActionPhase.Sustain ||
                from == PlayerActionPhase.Active && to == PlayerActionPhase.Sustain ||
                from == PlayerActionPhase.Sustain && to == PlayerActionPhase.Active,
            PlayerActionKind.Grapple => from == PlayerActionPhase.Begin &&
                to is PlayerActionPhase.Active or PlayerActionPhase.Impact or PlayerActionPhase.Attached or PlayerActionPhase.Sustain ||
                from == PlayerActionPhase.Active &&
                to is PlayerActionPhase.Impact or PlayerActionPhase.Attached or PlayerActionPhase.Sustain ||
                from == PlayerActionPhase.Impact && to == PlayerActionPhase.Attached ||
                from == PlayerActionPhase.Attached &&
                to is PlayerActionPhase.Sustain or PlayerActionPhase.Recover ||
                from == PlayerActionPhase.Sustain &&
                to is PlayerActionPhase.Attached or PlayerActionPhase.Recover,
            PlayerActionKind.Lasso => from == PlayerActionPhase.Begin &&
                to is PlayerActionPhase.Active or PlayerActionPhase.Impact or PlayerActionPhase.Attached or PlayerActionPhase.Sustain ||
                from == PlayerActionPhase.Active &&
                to is PlayerActionPhase.Impact or PlayerActionPhase.Attached or PlayerActionPhase.Sustain ||
                from == PlayerActionPhase.Impact && to == PlayerActionPhase.Attached ||
                from == PlayerActionPhase.Attached &&
                to is PlayerActionPhase.Sustain or PlayerActionPhase.Recover ||
                from == PlayerActionPhase.Sustain &&
                to is PlayerActionPhase.Attached or PlayerActionPhase.Recover,
            PlayerActionKind.Hogtie => from == PlayerActionPhase.Begin &&
                to is PlayerActionPhase.Active or PlayerActionPhase.Impact or PlayerActionPhase.Attached or PlayerActionPhase.Bound or PlayerActionPhase.Sustain ||
                from == PlayerActionPhase.Active &&
                to is PlayerActionPhase.Impact or PlayerActionPhase.Attached or PlayerActionPhase.Bound ||
                from == PlayerActionPhase.Impact &&
                to is PlayerActionPhase.Attached or PlayerActionPhase.Bound ||
                from == PlayerActionPhase.Attached &&
                to is PlayerActionPhase.Bound or PlayerActionPhase.Recover ||
                from == PlayerActionPhase.Bound && to == PlayerActionPhase.Recover,
            PlayerActionKind.Knockdown => from == PlayerActionPhase.Begin &&
                to is PlayerActionPhase.Impact or PlayerActionPhase.Recover ||
                from == PlayerActionPhase.Impact && to == PlayerActionPhase.Recover,
            PlayerActionKind.Crafting => from == PlayerActionPhase.Begin &&
                to is PlayerActionPhase.Active or PlayerActionPhase.Sustain ||
                from == PlayerActionPhase.Active && to == PlayerActionPhase.Sustain ||
                from == PlayerActionPhase.Sustain && to == PlayerActionPhase.Active,
            _ => false
        };
    }
}
