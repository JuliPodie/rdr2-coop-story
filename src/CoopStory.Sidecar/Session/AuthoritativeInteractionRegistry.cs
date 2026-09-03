using System.Numerics;
using CoopStory.Protocol;

namespace CoopStory.Sidecar.Session;

internal readonly record struct InteractionAuthorityResolution(
    InteractionResultPayload Result,
    RestraintStatePayload? RestraintState = null);

internal readonly record struct InteractionAuthoritySnapshot(
    int ActiveInteractions,
    int RestrainedPlayers,
    long Accepted,
    long Completed,
    long Rejected,
    long Duplicate,
    long Stale,
    long Cancelled);

/// <summary>
/// The only state machine allowed to approve cross-player interactions.
/// It is intentionally engine-agnostic: the host validates stable identities, freshness, distance and lifecycle, then both bridges apply the same semantic result to their own process-local handles.
/// </summary>
internal sealed class AuthoritativeInteractionRegistry
{
    // Interaction decisions require recent player transforms.
    // The host refuses an old position instead of allowing a revive/mount action from far away.
    private const long MaximumPlayerStateAgeMs = 2_000;
    private const float PlayerInteractionDistanceMeters = 2.0f;
    private const float MountInteractionDistanceMeters = 3.5f;
    private const long SustainFreshnessMs = 500;

    internal sealed record ActiveInteraction(
        InteractionIntentPayload Intent,
        long StartedAtMs,
        long LastSustainAtMs);

    internal sealed record Cursor(
        uint InteractionId,
        ushort Revision,
        InteractionResultPayload Result);

    /// <summary>
    /// Opaque rollback point used while a peer-originated authority decision is being delivered to the exact local game-pipe generation.
    /// The peer control gate serializes every host mutation, so restoring this complete snapshot cannot overwrite another committed interaction transaction.
    /// </summary>
    internal sealed class TransactionSnapshot
    {
        internal readonly Dictionary<NetEntityId, ActiveInteraction> _active;
        internal readonly Dictionary<NetEntityId, Cursor> _cursors;
        internal readonly Dictionary<NetEntityId, RestraintStatePayload>
            _restraints;
        internal readonly Dictionary<NetEntityId, byte>
            _restraintOwnerSlots;
        internal readonly long _accepted;
        internal readonly long _completed;
        internal readonly long _rejected;
        internal readonly long _duplicate;
        internal readonly long _stale;
        internal readonly long _cancelled;

        internal TransactionSnapshot(
            Dictionary<NetEntityId, ActiveInteraction> active,
            Dictionary<NetEntityId, Cursor> cursors,
            Dictionary<NetEntityId, RestraintStatePayload> restraints,
            Dictionary<NetEntityId, byte> restraintOwnerSlots,
            long accepted,
            long completed,
            long rejected,
            long duplicate,
            long stale,
            long cancelled)
        {
            _active = active;
            _cursors = cursors;
            _restraints = restraints;
            _restraintOwnerSlots = restraintOwnerSlots;
            _accepted = accepted;
            _completed = completed;
            _rejected = rejected;
            _duplicate = duplicate;
            _stale = stale;
            _cancelled = cancelled;
        }
    }

    private readonly object _sync = new();
    private readonly Dictionary<NetEntityId, ActiveInteraction> _active = [];
    private readonly Dictionary<NetEntityId, Cursor> _cursors = [];
    private readonly Dictionary<NetEntityId, RestraintStatePayload> _restraints = [];
    private readonly Dictionary<NetEntityId, byte> _restraintOwnerSlots = [];
    private long _accepted;
    private long _completed;
    private long _rejected;
    private long _duplicate;
    private long _stale;
    private long _cancelled;

    public InteractionAuthorityResolution Resolve(
        InteractionIntentPayload intent,
        long nowMs,
        Func<NetEntityId, ReplicatedPlayerSnapshot?> lookupPlayer)
    {
        ArgumentNullException.ThrowIfNull(lookupPlayer);
        lock (_sync)
        {
            // Return an identical result for a retry, but reject stale IDs or revisions before they can repeat an old physical interaction.
            if (_cursors.TryGetValue(intent.ActorEntityId, out var cursor))
            {
                if (cursor.InteractionId == intent.InteractionId &&
                    cursor.Revision == intent.Revision)
                {
                    _duplicate++;
                    return new InteractionAuthorityResolution(cursor.Result);
                }

                var newerInteraction = SequenceNumber.IsNewer(
                    intent.InteractionId,
                    cursor.InteractionId);
                var newerRevision =
                    cursor.InteractionId == intent.InteractionId &&
                    intent.Revision > cursor.Revision;
                if (!newerInteraction && !newerRevision)
                {
                    _stale++;
                    return RejectLocked(
                        intent,
                        InteractionRejectReason.Stale,
                        nowMs);
                }
            }

            // The host validates identity, freshness, matching slots and range before the intent is allowed to change authoritative state.
            if (!TryValidateParticipantsLocked(
                    intent,
                    lookupPlayer,
                    out var actor,
                    out var target,
                    out var rejection))
            {
                return RejectLocked(intent, rejection, nowMs);
            }

            // A cancel ends the actor's currently held operation and is itself remembered, so duplicate cancel packets remain idempotent.
            if (intent.Phase == InteractionIntentPhase.Cancel)
            {
                _active.Remove(intent.ActorEntityId);
                var cancelled = Result(
                    intent,
                    InteractionResultStatus.Cancelled,
                    InteractionRejectReason.None,
                    InteractionResultFlags.Authoritative,
                    0,
                    0);
                RememberLocked(intent, cancelled);
                _cancelled++;
                return new InteractionAuthorityResolution(cancelled);
            }

            if (intent.Kind == InteractionKind.ReleaseRestraint)
            {
                if (!_restraints.TryGetValue(
                        intent.TargetEntityId,
                        out var restraint) ||
                    restraint.State == PlayerRestraintState.Free)
                {
                    return RejectLocked(
                        intent,
                        InteractionRejectReason.InvalidState,
                        nowMs);
                }
                var free = SetRestraintLocked(
                    intent.TargetEntityId,
                    NetEntityId.None,
                    intent.InteractionId,
                    PlayerRestraintState.Free,
                    engineOwned: false,
                    snapshot: false);
                var completed = Result(
                    intent,
                    InteractionResultStatus.Completed,
                    InteractionRejectReason.None,
                    InteractionResultFlags.Authoritative |
                        InteractionResultFlags.StateChanged,
                    0,
                    0);
                RememberLocked(intent, completed);
                _completed++;
                return new InteractionAuthorityResolution(completed, free);
            }

            // Revive is a hold-to-complete host clock, not a client claim.
            // Each sustain packet must arrive within its short freshness interval.
            if (intent.Kind == InteractionKind.Revive)
            {
                if (actor is null || target is null ||
                    actor.Value.State.Lifecycle != PlayerLifecycle.Alive ||
                    target.Value.State.Lifecycle is not (
                        PlayerLifecycle.Downed or
                        PlayerLifecycle.Reviving))
                {
                    return RejectLocked(
                        intent,
                        InteractionRejectReason.InvalidState,
                        nowMs);
                }

                if (intent.Phase == InteractionIntentPhase.Begin)
                {
                    if (_active.TryGetValue(
                            intent.ActorEntityId,
                            out var occupied) &&
                        (occupied.Intent.InteractionId != intent.InteractionId ||
                         occupied.Intent.TargetEntityId != intent.TargetEntityId))
                    {
                        return RejectLocked(
                            intent,
                            InteractionRejectReason.Busy,
                            nowMs);
                    }
                    _active[intent.ActorEntityId] = new ActiveInteraction(
                        intent,
                        nowMs,
                        nowMs);
                    var accepted = Result(
                        intent,
                        InteractionResultStatus.Accepted,
                        InteractionRejectReason.None,
                        InteractionResultFlags.Authoritative |
                            InteractionResultFlags.HoldRequired,
                        0,
                        DownedStateMachine.ReviveDurationMs);
                    RememberLocked(intent, accepted);
                    _accepted++;
                    return new InteractionAuthorityResolution(accepted);
                }

                if (!_active.TryGetValue(
                        intent.ActorEntityId,
                        out var active) ||
                    active.Intent.InteractionId != intent.InteractionId ||
                    active.Intent.TargetEntityId != intent.TargetEntityId ||
                    nowMs < active.StartedAtMs ||
                    nowMs - active.LastSustainAtMs > SustainFreshnessMs)
                {
                    _active.Remove(intent.ActorEntityId);
                    return RejectLocked(
                        intent,
                        InteractionRejectReason.InvalidState,
                        nowMs);
                }

                active = active with { LastSustainAtMs = nowMs };
                _active[intent.ActorEntityId] = active;
                var progress = checked((uint)Math.Min(
                    DownedStateMachine.ReviveDurationMs,
                    nowMs - active.StartedAtMs));
                if (progress >= DownedStateMachine.ReviveDurationMs)
                {
                    _active.Remove(intent.ActorEntityId);
                    var completed = Result(
                        intent,
                        InteractionResultStatus.Completed,
                        InteractionRejectReason.None,
                        InteractionResultFlags.Authoritative |
                            InteractionResultFlags.StateChanged |
                            InteractionResultFlags.HoldRequired,
                        DownedStateMachine.ReviveDurationMs,
                        DownedStateMachine.ReviveDurationMs);
                    RememberLocked(intent, completed);
                    _completed++;
                    return new InteractionAuthorityResolution(completed);
                }

                var inProgress = Result(
                    intent,
                    InteractionResultStatus.InProgress,
                    InteractionRejectReason.None,
                    InteractionResultFlags.Authoritative |
                        InteractionResultFlags.HoldRequired,
                    progress,
                    DownedStateMachine.ReviveDurationMs);
                RememberLocked(intent, inProgress);
                return new InteractionAuthorityResolution(inProgress);
            }

            // Mount/dismount and emergency recovery are immediate semantic transactions.
            // The bridges still report a native apply failure in their diagnostics, but they never invent a different target.
            var immediate = Result(
                intent,
                InteractionResultStatus.Completed,
                InteractionRejectReason.None,
                InteractionResultFlags.Authoritative |
                    InteractionResultFlags.StateChanged,
                0,
                0);
            RememberLocked(intent, immediate);
            _completed++;
            RestraintStatePayload? recoveryRelease = null;
            if (intent.Kind == InteractionKind.EmergencyRecover &&
                _restraints.TryGetValue(intent.TargetEntityId, out var stuck) &&
                stuck.State != PlayerRestraintState.Free)
            {
                recoveryRelease = SetRestraintLocked(
                    intent.TargetEntityId,
                    NetEntityId.None,
                    intent.InteractionId,
                    PlayerRestraintState.Free,
                    engineOwned: false,
                    snapshot: false);
            }
            return new InteractionAuthorityResolution(
                immediate,
                recoveryRelease);
        }
    }

    public RestraintStatePayload? ObserveAuthoritativePlayerAction(
        PlayerActionPayload action)
    {
        lock (_sync)
        {
            var terminal = action.Phase is PlayerActionPhase.End or
                PlayerActionPhase.Cancel or
                PlayerActionPhase.Reject;
            if (terminal && action.Kind is (
                    PlayerActionKind.Lasso or
                    PlayerActionKind.Hogtie))
            {
                // RDR2 clears the target handle before the final action sample.
                // Consequently End normally arrives with target=0 and without TargetEntityValid/PhysicalTargetEffect.
                // Resolve that terminal by its stable actor + action id instead of rejecting it as an invalid physical sample, otherwise Lassoed remains latched forever on both bridges.
                if (TryFindTerminalRestraintLocked(
                        action,
                        out var releaseable))
                {
                    return SetRestraintLocked(
                        releaseable.SubjectEntityId,
                        NetEntityId.None,
                        action.ActionId,
                        PlayerRestraintState.Free,
                        engineOwned: false,
                        snapshot: false);
                }
                return null;
            }

            var targetValid = action.Flags.HasFlag(
                PlayerActionFlags.TargetEntityValid);
            var physical = action.Flags.HasFlag(
                PlayerActionFlags.PhysicalTargetEffect);
            if (!targetValid || !physical ||
                action.Kind is not (
                    PlayerActionKind.Lasso or
                    PlayerActionKind.Hogtie))
            {
                return null;
            }

            var desiredState = action.Kind == PlayerActionKind.Hogtie
                ? PlayerRestraintState.Hogtied
                : PlayerRestraintState.Lassoed;
            if (_restraints.TryGetValue(
                    action.TargetEntityId,
                    out var current) &&
                current.State == desiredState &&
                current.OwnerEntityId == action.ActorEntityId &&
                current.SourceInteractionId == action.ActionId)
            {
                // A same-revision snapshot is a lease heartbeat.
                // It keeps a legitimate held lasso alive in the bridges without creating a false state transition or incrementing the authority revision on every Sustain packet.
                if (desiredState == PlayerRestraintState.Lassoed &&
                    action.Phase is PlayerActionPhase.Active or
                        PlayerActionPhase.Attached or
                        PlayerActionPhase.Sustain)
                {
                    return current with
                    {
                        Flags = current.Flags | RestraintStateFlags.Snapshot
                    };
                }
                return null;
            }

            return SetRestraintLocked(
                action.TargetEntityId,
                action.ActorEntityId,
                action.ActionId,
                desiredState,
                engineOwned: true,
                snapshot: false,
                ownerSlot: action.ActorSlot);
        }
    }

    /// <summary>
    /// Allows an already-authoritative lasso/hogtie to be released even when its remote PlayerState mapping lease has expired.
    /// This never authorizes a new physical action: the actor and action id must match a restraint that is still active in the host registry.
    /// </summary>
    internal bool HasMatchingTerminalRestraint(PlayerActionPayload action)
    {
        lock (_sync)
        {
            // This proof is the only path allowed to bypass a temporarily missing PlayerState mapping for a peer-originated terminal.
            // The owner slot was captured when the original physical action still had a fresh authenticated mapping; forgeable ActorSlot bits on the terminal payload are never sufficient by themselves.
            return TryFindTerminalRestraintLocked(
                action,
                out _,
                requiredOwnerSlot: (byte)SessionRole.Guest);
        }
    }

    public IReadOnlyList<RestraintStatePayload> CaptureRestraints()
    {
        lock (_sync)
        {
            return _restraints.Values
                .Select(static state => state with
                {
                    Flags = state.Flags | RestraintStateFlags.Snapshot
                })
                .OrderBy(static state => state.SubjectEntityId.Value)
                .ToArray();
        }
    }

    internal TransactionSnapshot CaptureTransactionSnapshot()
    {
        lock (_sync)
        {
            return new TransactionSnapshot(
                new Dictionary<NetEntityId, ActiveInteraction>(_active),
                new Dictionary<NetEntityId, Cursor>(_cursors),
                new Dictionary<NetEntityId, RestraintStatePayload>(
                    _restraints),
                new Dictionary<NetEntityId, byte>(_restraintOwnerSlots),
                _accepted,
                _completed,
                _rejected,
                _duplicate,
                _stale,
                _cancelled);
        }
    }

    internal void RestoreTransactionSnapshot(TransactionSnapshot snapshot)
    {
        ArgumentNullException.ThrowIfNull(snapshot);
        lock (_sync)
        {
            _active.Clear();
            foreach (var entry in snapshot._active)
            {
                _active.Add(entry.Key, entry.Value);
            }

            _cursors.Clear();
            foreach (var entry in snapshot._cursors)
            {
                _cursors.Add(entry.Key, entry.Value);
            }

            _restraints.Clear();
            foreach (var entry in snapshot._restraints)
            {
                _restraints.Add(entry.Key, entry.Value);
            }
            _restraintOwnerSlots.Clear();
            foreach (var entry in snapshot._restraintOwnerSlots)
            {
                _restraintOwnerSlots.Add(entry.Key, entry.Value);
            }

            _accepted = snapshot._accepted;
            _completed = snapshot._completed;
            _rejected = snapshot._rejected;
            _duplicate = snapshot._duplicate;
            _stale = snapshot._stale;
            _cancelled = snapshot._cancelled;
        }
    }

    public bool ApplyAuthoritativeRestraint(
        RestraintStatePayload state)
    {
        lock (_sync)
        {
            if (_restraints.TryGetValue(state.SubjectEntityId, out var previous) &&
                state.Revision != previous.Revision &&
                !SequenceNumber.IsNewer(state.Revision, previous.Revision))
            {
                _stale++;
                return false;
            }
            if (_restraints.TryGetValue(state.SubjectEntityId, out previous) &&
                state.Revision == previous.Revision)
            {
                _duplicate++;
                return false;
            }
            _restraints[state.SubjectEntityId] = state;
            if (state.State != PlayerRestraintState.Free)
            {
                // A wire RestraintState does not carry owner provenance.
                // Do not let provenance retained from an older local authority decision authorize a future mapping-bypass terminal.
                _restraintOwnerSlots.Remove(state.SubjectEntityId);
            }
            return true;
        }
    }

    public IReadOnlyList<RestraintStatePayload> Clear(bool emitFreeStates)
    {
        lock (_sync)
        {
            var activeRestraints = _restraints.Values
                .Where(static state =>
                    state.State != PlayerRestraintState.Free)
                .ToArray();
            var released = emitFreeStates
                ? activeRestraints
                    .Select(state => SetRestraintLocked(
                        state.SubjectEntityId,
                        NetEntityId.None,
                        state.SourceInteractionId,
                        PlayerRestraintState.Free,
                        engineOwned: false,
                        snapshot: false))
                    .ToArray()
                : [];
            _active.Clear();
            _cursors.Clear();
            if (!emitFreeStates)
            {
                _restraints.Clear();
                _restraintOwnerSlots.Clear();
            }
            return released;
        }
    }

    public InteractionAuthoritySnapshot ReadSnapshot()
    {
        lock (_sync)
        {
            return new InteractionAuthoritySnapshot(
                _active.Count,
                _restraints.Values.Count(static restraint =>
                    restraint.State != PlayerRestraintState.Free),
                _accepted,
                _completed,
                _rejected,
                _duplicate,
                _stale,
                _cancelled);
        }
    }

    private bool TryValidateParticipantsLocked(
        InteractionIntentPayload intent,
        Func<NetEntityId, ReplicatedPlayerSnapshot?> lookupPlayer,
        out ReplicatedPlayerSnapshot? actor,
        out ReplicatedPlayerSnapshot? target,
        out InteractionRejectReason rejection)
    {
        actor = lookupPlayer(intent.ActorEntityId);
        var selfRecovery =
            intent.Kind == InteractionKind.EmergencyRecover &&
            intent.ActorEntityId == intent.TargetEntityId;
        target = selfRecovery
            ? actor
            : intent.Flags.HasFlag(InteractionIntentFlags.TargetPlayer)
                ? lookupPlayer(intent.TargetEntityId)
                : null;
        if (actor is null ||
            actor.Value.State.Slot != intent.ActorSlot ||
            actor.Value.AgeMilliseconds > MaximumPlayerStateAgeMs)
        {
            rejection = InteractionRejectReason.InvalidActor;
            return false;
        }
        if (intent.Flags.HasFlag(InteractionIntentFlags.TargetPlayer) &&
            (target is null ||
             (!selfRecovery &&
              target.Value.State.Slot == intent.ActorSlot) ||
             target.Value.AgeMilliseconds > MaximumPlayerStateAgeMs))
        {
            rejection = InteractionRejectReason.InvalidTarget;
            return false;
        }

        // Distance is evaluated against the host's latest snapshots, with a slightly larger allowance only for mount attachment operations.
        if (target is not null && !selfRecovery)
        {
            var distance = Vector3.Distance(
                actor.Value.State.Position,
                target.Value.State.Position);
            var limit = intent.Kind is
                InteractionKind.DismountPeer or
                InteractionKind.MountDriver or
                InteractionKind.MountPassenger
                    ? MountInteractionDistanceMeters
                    : PlayerInteractionDistanceMeters;
            if (!float.IsFinite(distance) || distance > limit)
            {
                rejection = InteractionRejectReason.TooFar;
                return false;
            }
        }
        rejection = InteractionRejectReason.None;
        return true;
    }

    private bool TryFindTerminalRestraintLocked(
        PlayerActionPayload action,
        out RestraintStatePayload releaseable,
        byte? requiredOwnerSlot = null)
    {
        var terminal = action.Phase is PlayerActionPhase.End or
            PlayerActionPhase.Cancel or
            PlayerActionPhase.Reject;
        if (!terminal || action.Kind is not (
                PlayerActionKind.Lasso or PlayerActionKind.Hogtie))
        {
            releaseable = default;
            return false;
        }

        releaseable = _restraints.Values.FirstOrDefault(state =>
            state.State != PlayerRestraintState.Free &&
            state.OwnerEntityId == action.ActorEntityId &&
            state.SourceInteractionId == action.ActionId &&
            (!requiredOwnerSlot.HasValue ||
             (_restraintOwnerSlots.TryGetValue(
                  state.SubjectEntityId,
                  out var ownerSlot) &&
              ownerSlot == requiredOwnerSlot.Value)) &&
            (state.State == PlayerRestraintState.Lassoed ||
             action.Phase is PlayerActionPhase.Cancel or
                 PlayerActionPhase.Reject));
        return releaseable.SubjectEntityId.IsValid;
    }

    private InteractionAuthorityResolution RejectLocked(
        InteractionIntentPayload intent,
        InteractionRejectReason reason,
        long nowMs)
    {
        _ = nowMs;
        _active.Remove(intent.ActorEntityId);
        var result = Result(
            intent,
            InteractionResultStatus.Rejected,
            reason,
            InteractionResultFlags.Authoritative,
            0,
            0);
        RememberLocked(intent, result);
        _rejected++;
        return new InteractionAuthorityResolution(result);
    }

    private void RememberLocked(
        InteractionIntentPayload intent,
        InteractionResultPayload result) =>
        _cursors[intent.ActorEntityId] = new Cursor(
            intent.InteractionId,
            intent.Revision,
            result);

    private RestraintStatePayload SetRestraintLocked(
        NetEntityId subject,
        NetEntityId owner,
        uint sourceInteractionId,
        PlayerRestraintState state,
        bool engineOwned,
        bool snapshot,
        byte? ownerSlot = null)
    {
        // Each restraint update increments its subject revision so an older free/bound packet cannot overwrite a newer authoritative decision.
        var revision = _restraints.TryGetValue(subject, out var previous)
            ? unchecked(previous.Revision + 1)
            : 1;
        if (revision == 0)
        {
            revision = 1;
        }
        var flags = RestraintStateFlags.Authoritative;
        if (engineOwned)
        {
            flags |= RestraintStateFlags.EngineOwned;
        }
        if (snapshot)
        {
            flags |= RestraintStateFlags.Snapshot;
        }
        var payload = new RestraintStatePayload(
            subject,
            owner,
            sourceInteractionId,
            revision,
            state,
            flags);
        _restraints[subject] = payload;
        if (ownerSlot.HasValue &&
            state != PlayerRestraintState.Free)
        {
            _restraintOwnerSlots[subject] = ownerSlot.Value;
        }
        return payload;
    }

    private static InteractionResultPayload Result(
        InteractionIntentPayload intent,
        InteractionResultStatus status,
        InteractionRejectReason reason,
        InteractionResultFlags flags,
        uint progress,
        uint required) =>
        new(
            intent.ActorEntityId,
            intent.TargetEntityId,
            intent.SecondaryEntityId,
            intent.InteractionId,
            intent.Revision,
            intent.Kind,
            status,
            reason,
            flags,
            progress,
            required);
}
