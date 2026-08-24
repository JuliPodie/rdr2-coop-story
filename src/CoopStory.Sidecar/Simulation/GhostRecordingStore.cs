using System.Numerics;
using System.Text.Json;
using System.Text.Json.Serialization;
using CoopStory.Protocol;

namespace CoopStory.Sidecar.Simulation;

internal sealed record GhostRecordingDocument(
    int FormatVersion,
    DateTimeOffset RecordedAtUtc,
    int SnapshotRateHz,
    IReadOnlyList<GhostRecordedFrame> Frames);

internal sealed record GhostRecordedFrame(
    int OffsetMilliseconds,
    byte Lifecycle,
    float PositionX,
    float PositionY,
    float PositionZ,
    float VelocityX,
    float VelocityY,
    float VelocityZ,
    float Heading,
    float HealthFraction,
    uint PlayerFlags,
    float AimTargetX,
    float AimTargetY,
    float AimTargetZ,
    uint FireSequence,
    uint WeaponHash,
    uint Ammo,
    uint EquipmentFlags,
    float MovementHeading = 0,
    float LocalForwardSpeed = 0,
    float LocalRightSpeed = 0,
    float DesiredMoveBlend = 0,
    ushort LocomotionEpoch = 0,
    ushort TraversalActionId = 0,
    byte TraversalKind = 0,
    byte LocomotionMode = 0,
    float TraversalAnchorX = 0,
    float TraversalAnchorY = 0,
    float TraversalAnchorZ = 0,
    float TraversalHeading = 0)
{
    public PlayerStatePayload ToPlayerState(NetEntityId entityId)
    {
        var flags = (PlayerStateFlags)PlayerFlags;
        flags &= ~PlayerStateFlags.OnlineModeDetected;
        flags |= PlayerStateFlags.SyntheticTest;
        return new PlayerStatePayload(
            entityId,
            Slot: (byte)SessionRole.Guest,
            Enum.IsDefined((PlayerLifecycle)Lifecycle)
                ? (PlayerLifecycle)Lifecycle
                : PlayerLifecycle.Alive,
            new Vector3(PositionX, PositionY, PositionZ),
            new Vector3(VelocityX, VelocityY, VelocityZ),
            Heading,
            HealthFraction,
            flags,
            new Vector3(AimTargetX, AimTargetY, AimTargetZ),
            FireSequence,
            MovementHeading,
            LocalForwardSpeed,
            LocalRightSpeed,
            DesiredMoveBlend,
            LocomotionEpoch,
            TraversalActionId,
            Enum.IsDefined((PlayerTraversalKind)TraversalKind)
                ? (PlayerTraversalKind)TraversalKind
                : PlayerTraversalKind.None,
            Enum.IsDefined((PlayerLocomotionMode)LocomotionMode)
                ? (PlayerLocomotionMode)LocomotionMode
                : PlayerLocomotionMode.Grounded,
            new Vector3(
                TraversalAnchorX,
                TraversalAnchorY,
                TraversalAnchorZ),
            TraversalHeading);
    }

    public EquipmentStatePayload ToEquipmentState(NetEntityId entityId) =>
        new(
            entityId,
            WeaponHash,
            Ammo,
            (EquipmentStateFlags)EquipmentFlags);
}

internal sealed class GhostRecordingStore
{
    internal const int CurrentFormatVersion = 2;
    internal const int MaximumFrames = 12_000;
    internal const int MaximumDurationMilliseconds = 10 * 60 * 1_000;
    private const long MaximumFileBytes = 16L * 1024 * 1024;
    private static readonly JsonSerializerOptions JsonOptions = new()
    {
        PropertyNamingPolicy = JsonNamingPolicy.CamelCase,
        WriteIndented = false,
        DefaultIgnoreCondition = JsonIgnoreCondition.Never
    };

    private readonly object _gate = new();
    private readonly string _path;
    private readonly int _snapshotRateHz;
    private readonly List<GhostRecordedFrame> _frames = [];
    private EquipmentStatePayload? _latestEquipment;
    private long _firstObservedAtMilliseconds;
    private bool _recording;

    public GhostRecordingStore(string path, int snapshotRateHz)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(path);
        if (snapshotRateHz is < 1 or > 120)
        {
            throw new ArgumentOutOfRangeException(nameof(snapshotRateHz));
        }

        _path = System.IO.Path.GetFullPath(path);
        _snapshotRateHz = snapshotRateHz;
    }

    public string Path => _path;

    public void Begin()
    {
        lock (_gate)
        {
            _frames.Clear();
            _firstObservedAtMilliseconds = 0;
            _recording = true;
        }
    }

    public void ObserveEquipment(EquipmentStatePayload equipment)
    {
        lock (_gate)
        {
            _latestEquipment = equipment;
        }
    }

    public void Capture(PlayerStatePayload state, long observedAtMilliseconds)
    {
        lock (_gate)
        {
            if (!_recording || _frames.Count >= MaximumFrames)
            {
                return;
            }

            if (_firstObservedAtMilliseconds == 0)
            {
                _firstObservedAtMilliseconds = observedAtMilliseconds;
            }
            var offset = checked((int)Math.Clamp(
                observedAtMilliseconds - _firstObservedAtMilliseconds,
                0,
                MaximumDurationMilliseconds));
            if (offset >= MaximumDurationMilliseconds && _frames.Count > 0)
            {
                return;
            }

            var equipment = _latestEquipment;
            var frame = new GhostRecordedFrame(
                offset,
                (byte)state.Lifecycle,
                state.Position.X,
                state.Position.Y,
                state.Position.Z,
                state.Velocity.X,
                state.Velocity.Y,
                state.Velocity.Z,
                state.Heading,
                state.HealthFraction,
                (uint)(state.Flags & ~PlayerStateFlags.OnlineModeDetected),
                state.AimTarget.X,
                state.AimTarget.Y,
                state.AimTarget.Z,
                state.FireSequence,
                equipment?.WeaponHash ?? 0,
                equipment?.Ammo ?? 0,
                (uint)(equipment?.Flags ?? EquipmentStateFlags.None),
                state.MovementHeading,
                state.LocalForwardSpeed,
                state.LocalRightSpeed,
                state.DesiredMoveBlend,
                state.LocomotionEpoch,
                state.TraversalActionId,
                (byte)state.TraversalKind,
                (byte)state.LocomotionMode,
                state.TraversalAnchor.X,
                state.TraversalAnchor.Y,
                state.TraversalAnchor.Z,
                state.TraversalHeading);
            ValidateFrame(frame, _frames.Count == 0 ? 0 : _frames[^1].OffsetMilliseconds);
            _frames.Add(frame);
        }
    }

    public GhostRecordingDocument CompleteAndSave()
    {
        GhostRecordingDocument document;
        lock (_gate)
        {
            _recording = false;
            if (_frames.Count < 2)
            {
                throw new InvalidOperationException(
                    "Ghost Record nie ma jeszcze wystarczajaco danych. " +
                    "Wczytaj save, przejdz trase i dopiero zatrzymaj nagranie.");
            }
            document = new GhostRecordingDocument(
                CurrentFormatVersion,
                DateTimeOffset.UtcNow,
                _snapshotRateHz,
                _frames.ToArray());
        }

        SaveAtomically(document);
        return document;
    }

    public GhostRecordingDocument Load()
    {
        var info = new FileInfo(_path);
        if (!info.Exists)
        {
            throw new InvalidOperationException(
                "Brak nagrania Ghost Record. Najpierw nagraj i zatrzymaj trase.");
        }
        if (info.Length is <= 0 or > MaximumFileBytes)
        {
            throw new InvalidDataException(
                "Plik Ghost Record ma nieprawidlowy rozmiar.");
        }

        using var stream = new FileStream(
            _path,
            FileMode.Open,
            FileAccess.Read,
            FileShare.Read,
            bufferSize: 64 * 1024,
            FileOptions.SequentialScan);
        var document = JsonSerializer.Deserialize<GhostRecordingDocument>(
                           stream,
                           JsonOptions)
                       ?? throw new InvalidDataException(
                           "Plik Ghost Record jest pusty lub uszkodzony.");
        ValidateDocument(document);
        return document;
    }

    private void SaveAtomically(GhostRecordingDocument document)
    {
        ValidateDocument(document);
        var parent = System.IO.Path.GetDirectoryName(_path)
            ?? throw new InvalidDataException(
                "Sciezka Ghost Record nie ma katalogu nadrzednego.");
        Directory.CreateDirectory(parent);
        var temporaryPath = System.IO.Path.Combine(
            parent,
            $".{System.IO.Path.GetFileName(_path)}.{Guid.NewGuid():N}.tmp");
        try
        {
            var bytes = JsonSerializer.SerializeToUtf8Bytes(document, JsonOptions);
            if (bytes.LongLength > MaximumFileBytes)
            {
                throw new InvalidDataException(
                    "Nagranie Ghost Record przekracza bezpieczny limit 16 MB.");
            }
            using (var stream = new FileStream(
                       temporaryPath,
                       FileMode.CreateNew,
                       FileAccess.Write,
                       FileShare.None,
                       bufferSize: 64 * 1024,
                       FileOptions.WriteThrough))
            {
                stream.Write(bytes);
                stream.Flush(flushToDisk: true);
            }
            File.Move(temporaryPath, _path, overwrite: true);
        }
        finally
        {
            if (File.Exists(temporaryPath))
            {
                File.Delete(temporaryPath);
            }
        }
    }

    private static void ValidateDocument(GhostRecordingDocument document)
    {
        if (document.FormatVersion is < 1 or > CurrentFormatVersion)
        {
            throw new InvalidDataException(
                $"Nieobslugiwana wersja Ghost Record: {document.FormatVersion}.");
        }
        if (document.SnapshotRateHz is < 1 or > 120 ||
            document.Frames is null ||
            document.Frames.Count is < 2 or > MaximumFrames)
        {
            throw new InvalidDataException(
                "Plik Ghost Record ma nieprawidlowe parametry lub liczbe klatek.");
        }

        var previousOffset = 0;
        for (var index = 0; index < document.Frames.Count; index++)
        {
            var frame = document.Frames[index];
            ValidateFrame(frame, index == 0 ? 0 : previousOffset);
            if (index == 0 && frame.OffsetMilliseconds != 0)
            {
                throw new InvalidDataException(
                    "Pierwsza klatka Ghost Record nie zaczyna sie od zera.");
            }
            previousOffset = frame.OffsetMilliseconds;
        }
    }

    private static void ValidateFrame(
        GhostRecordedFrame frame,
        int previousOffset)
    {
        if (frame.OffsetMilliseconds < previousOffset ||
            frame.OffsetMilliseconds > MaximumDurationMilliseconds ||
            !Enum.IsDefined((PlayerLifecycle)frame.Lifecycle) ||
            !IsFinite(frame.PositionX) ||
            !IsFinite(frame.PositionY) ||
            !IsFinite(frame.PositionZ) ||
            !IsFinite(frame.VelocityX) ||
            !IsFinite(frame.VelocityY) ||
            !IsFinite(frame.VelocityZ) ||
            !IsFinite(frame.Heading) ||
            !IsFinite(frame.HealthFraction) ||
            frame.HealthFraction is < 0 or > 1 ||
            !IsFinite(frame.AimTargetX) ||
            !IsFinite(frame.AimTargetY) ||
            !IsFinite(frame.AimTargetZ) ||
            !IsFinite(frame.MovementHeading) ||
            !IsFinite(frame.LocalForwardSpeed) ||
            !IsFinite(frame.LocalRightSpeed) ||
            MathF.Abs(frame.LocalForwardSpeed) > 50f ||
            MathF.Abs(frame.LocalRightSpeed) > 50f ||
            !IsFinite(frame.DesiredMoveBlend) ||
            frame.DesiredMoveBlend is < 0 or > 3 ||
            !Enum.IsDefined((PlayerTraversalKind)frame.TraversalKind) ||
            !Enum.IsDefined((PlayerLocomotionMode)frame.LocomotionMode) ||
            !IsFinite(frame.TraversalAnchorX) ||
            !IsFinite(frame.TraversalAnchorY) ||
            !IsFinite(frame.TraversalAnchorZ) ||
            !IsFinite(frame.TraversalHeading) ||
            ((PlayerTraversalKind)frame.TraversalKind != PlayerTraversalKind.None &&
             frame.TraversalActionId == 0) ||
            ((EquipmentStateFlags)frame.EquipmentFlags &
             ~(EquipmentStateFlags.Equipped | EquipmentStateFlags.Reloading)) != 0)
        {
            throw new InvalidDataException(
                "Ghost Record zawiera nieprawidlowa klatke ruchu.");
        }
    }

    private static bool IsFinite(float value) => float.IsFinite(value);
}
