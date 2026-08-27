namespace CoopStory.Sidecar.Session;

// Shared campaign permissions only.  Private possessions, currency, horse
// state and consumables must never enter this journal.
public enum CapabilityKind : byte
{
    WeaponShopEligibility = 1,
    Recipe = 2,
    CapacityUpgrade = 3,
    ActivityGate = 4
}

public sealed record CapabilityGrant(
    string Id,
    CapabilityKind Kind,
    uint RecordHash,
    ulong HostEventId,
    long GrantedAtUnixMilliseconds,
    long? GuestAcknowledgedAtUnixMilliseconds = null)
{
    public CapabilityGrant Validate()
    {
        if (string.IsNullOrWhiteSpace(Id) || Id.Length > 128 ||
            RecordHash == 0 || HostEventId == 0 ||
            !Enum.IsDefined(Kind) || GrantedAtUnixMilliseconds <= 0)
        {
            throw new ArgumentException("Capability grant is invalid.");
        }
        if (GuestAcknowledgedAtUnixMilliseconds is <= 0)
        {
            throw new ArgumentException("Capability acknowledgement timestamp is invalid.");
        }
        return this;
    }
}

public sealed class CapabilityJournal
{
    private readonly object _gate = new();
    private readonly Dictionary<ulong, CapabilityGrant> _byEvent = [];
    private readonly Dictionary<(CapabilityKind, uint), CapabilityGrant> _effective = [];

    public bool Record(CapabilityGrant grant)
    {
        grant.Validate();
        lock (_gate)
        {
            if (_byEvent.ContainsKey(grant.HostEventId)) return false;
            _byEvent.Add(grant.HostEventId, grant);
            _effective[(grant.Kind, grant.RecordHash)] = grant;
            return true;
        }
    }

    public IReadOnlyList<CapabilityGrant> CaptureReplay() { lock (_gate) return _effective.Values.OrderBy(x => x.Kind).ThenBy(x => x.RecordHash).ToArray(); }
    public IReadOnlyList<CapabilityGrant> CaptureState() { lock (_gate) return _byEvent.Values.OrderBy(x => x.HostEventId).ToArray(); }

    // An acknowledgement proves only that the guest's local native accepted
    // a shared capability. It never reports ownership, money or inventory.
    public bool Acknowledge(ulong hostEventId, long acknowledgedAtUnixMilliseconds)
    {
        if (hostEventId == 0 || acknowledgedAtUnixMilliseconds <= 0)
        {
            throw new ArgumentException("Capability acknowledgement is invalid.");
        }
        lock (_gate)
        {
            if (!_byEvent.TryGetValue(hostEventId, out var grant) ||
                grant.GuestAcknowledgedAtUnixMilliseconds.HasValue)
            {
                return false;
            }
            var acknowledged = grant with
            {
                GuestAcknowledgedAtUnixMilliseconds = acknowledgedAtUnixMilliseconds
            };
            _byEvent[hostEventId] = acknowledged;
            var key = (acknowledged.Kind, acknowledged.RecordHash);
            if (_effective.TryGetValue(key, out var effective) &&
                effective.HostEventId == hostEventId)
            {
                _effective[key] = acknowledged;
            }
            return true;
        }
    }

    public void Restore(IEnumerable<CapabilityGrant> grants)
    {
        ArgumentNullException.ThrowIfNull(grants);
        lock (_gate)
        {
            _byEvent.Clear();
            _effective.Clear();
            foreach (var grant in grants.OrderBy(x => x.HostEventId))
            {
                if (!Record(grant)) throw new ArgumentException("Capability journal contains a duplicate host event.");
            }
        }
    }
}
