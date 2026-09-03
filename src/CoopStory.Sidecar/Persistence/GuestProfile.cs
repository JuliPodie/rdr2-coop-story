namespace CoopStory.Sidecar.Persistence;

// The guest's locally stored Story-profile shape.
// Validation keeps damaged or unsupported profile data from entering the local multiplayer helper.
public sealed record GuestProfile
{
    public const int CurrentSchemaVersion = 1;

    public int SchemaVersion { get; init; } = CurrentSchemaVersion;

    public Guid GuestId { get; init; }

    public string DisplayName { get; init; } = "Guest";

    public decimal Money { get; init; }

    public List<WeaponEntry> Loadout { get; init; } = [];

    public Dictionary<string, int> Ammunition { get; init; } =
        new(StringComparer.OrdinalIgnoreCase);

    public HorseProfile Horse { get; init; } = new();

    public long UpdatedAtUnixMilliseconds { get; init; } =
        DateTimeOffset.UtcNow.ToUnixTimeMilliseconds();

    public GuestProfile Validate()
    {
        if (SchemaVersion != CurrentSchemaVersion)
        {
            throw new GuestProfileException(
                $"Unsupported guest profile schema {SchemaVersion}.");
        }

        if (GuestId == Guid.Empty)
        {
            throw new GuestProfileException("GuestId cannot be empty.");
        }

        if (string.IsNullOrWhiteSpace(DisplayName) || DisplayName.Length > 64)
        {
            throw new GuestProfileException("DisplayName must contain 1-64 characters.");
        }

        if (Money < 0)
        {
            throw new GuestProfileException("Money cannot be negative.");
        }

        if (Loadout.Count > 64 ||
            Loadout.Any(entry =>
                string.IsNullOrWhiteSpace(entry.WeaponId) ||
                entry.WeaponId.Length > 128 ||
                entry.Condition is < 0f or > 1f ||
                !float.IsFinite(entry.Condition)))
        {
            throw new GuestProfileException("Loadout contains an invalid weapon entry.");
        }

        if (Ammunition.Count > 256 ||
            Ammunition.Any(pair =>
                string.IsNullOrWhiteSpace(pair.Key) ||
                pair.Key.Length > 128 ||
                pair.Value < 0))
        {
            throw new GuestProfileException("Ammunition contains an invalid entry.");
        }

        Horse.Validate();
        return this;
    }

    public static GuestProfile Create(Guid guestId, string displayName) =>
        new()
        {
            GuestId = guestId,
            DisplayName = displayName
        };
}

public sealed record WeaponEntry(string WeaponId, float Condition = 1f);

public sealed record HorseProfile
{
    public string Name { get; init; } = "Guest Horse";

    public string Model { get; init; } = string.Empty;

    public int BondingLevel { get; init; }

    public void Validate()
    {
        if (string.IsNullOrWhiteSpace(Name) ||
            Name.Length > 64 ||
            Model.Length > 128 ||
            BondingLevel is < 0 or > 4)
        {
            throw new GuestProfileException("Horse profile is invalid.");
        }
    }
}

public sealed class GuestProfileException : Exception
{
    public GuestProfileException(string message)
        : base(message)
    {
    }

    public GuestProfileException(string message, Exception innerException)
        : base(message, innerException)
    {
    }
}

public sealed record GuestProfileLoadResult(
    GuestProfile Profile,
    bool RecoveredFromBackup);
