using System.Net;
using System.Net.NetworkInformation;
using System.Net.Sockets;
using System.Text.Json;

namespace CoopStory.Launcher;

public sealed record CoopInvite
{
    public int SchemaVersion { get; init; } = 1;

    public required string HostAddress { get; init; }

    public int TcpPort { get; init; } = 43120;

    public int UdpPort { get; init; } = 43121;

    public required string SessionToken { get; init; }

    public DateTimeOffset CreatedAtUtc { get; init; } = DateTimeOffset.UtcNow;
}

public static class InviteService
{
    public static void Export(string path, LauncherSettings settings)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(path);
        SessionCodeService.Validate(settings.SessionToken);
        _ = ValidateRemoteHost(settings.HostAddress);
        var invite = new CoopInvite
        {
            HostAddress = settings.HostAddress.Trim(),
            SessionToken = settings.SessionToken.Trim()
        };
        AtomicFile.WriteJson(path, invite, JsonSupport.Options);
    }

    public static LauncherSettings Import(string path, LauncherSettings current)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(path);
        try
        {
            var invite = JsonSerializer.Deserialize<CoopInvite>(
                File.ReadAllBytes(path),
                JsonSupport.Options);
            if (invite is null ||
                invite.SchemaVersion != 1 ||
                invite.TcpPort != 43120 ||
                invite.UdpPort != 43121)
            {
                throw new LauncherException(
                    "Plik zaproszenia ma nieobsługiwaną wersję albo porty.");
            }

            _ = ValidateRemoteHost(invite.HostAddress);
            SessionCodeService.Validate(invite.SessionToken);
            return current with
            {
                Role = LauncherRole.Guest,
                HostAddress = invite.HostAddress.Trim(),
                SessionToken = invite.SessionToken.Trim()
            };
        }
        catch (JsonException exception)
        {
            throw new LauncherException(
                "Plik zaproszenia jest uszkodzony albo nie jest JSON-em.", exception);
        }
    }

    public static string SuggestedLanAddress()
    {
        var candidates = NetworkInterface.GetAllNetworkInterfaces()
            .Where(network =>
                network.OperationalStatus == OperationalStatus.Up &&
                network.NetworkInterfaceType != NetworkInterfaceType.Loopback)
            .SelectMany(network => network.GetIPProperties().UnicastAddresses)
            .Where(address =>
                address.Address.AddressFamily == AddressFamily.InterNetwork &&
                !IPAddress.IsLoopback(address.Address) &&
                address.IPv4Mask is not null &&
                IsPrivateOrHamachi(address.Address))
            .Select(address => address.Address)
            .ToArray();
        return SelectSuggestedLanAddress(candidates);
    }

    internal static string SelectSuggestedLanAddress(
        IEnumerable<IPAddress> candidates) =>
        candidates
            .Where(address =>
                address.AddressFamily == AddressFamily.InterNetwork &&
                !IPAddress.IsLoopback(address) &&
                IsPrivateOrHamachi(address))
            .Distinct()
            .OrderByDescending(IsHamachiAddress)
            .ThenBy(static address => address.ToString(), StringComparer.Ordinal)
            .Select(static address => address.ToString())
            .FirstOrDefault() ?? string.Empty;

    private static bool IsHamachiAddress(IPAddress address) =>
        address.GetAddressBytes()[0] == 25;

    private static bool IsPrivateOrHamachi(IPAddress address)
    {
        var bytes = address.GetAddressBytes();
        return bytes[0] == 25 ||
               bytes[0] == 10 ||
               (bytes[0] == 172 && bytes[1] is >= 16 and <= 31) ||
               (bytes[0] == 192 && bytes[1] == 168) ||
               (bytes[0] == 169 && bytes[1] == 254);
    }

    public static string ValidateRemoteHost(string value)
    {
        var host = value.Trim();
        if (host.Length is < 1 or > 253 ||
            !string.Equals(host, value, StringComparison.Ordinal) ||
            host.IndexOfAny(['/', '\\', ':', '@', '?', '#']) >= 0)
        {
            throw new LauncherException(
                "Brak prawidłowego adresu LAN hosta. Wpisz np. 192.168.1.25.");
        }

        if (IPAddress.TryParse(host, out var address))
        {
            if (address.AddressFamily != AddressFamily.InterNetwork)
            {
                throw new LauncherException(
                    "Protokół PoC obsługuje obecnie tylko adres IPv4 hosta.");
            }

            var octets = address.GetAddressBytes();
            if (IPAddress.IsLoopback(address) ||
                address.Equals(IPAddress.Any) ||
                address.Equals(IPAddress.Broadcast) ||
                octets[0] == 0 ||
                octets[3] == 255 ||
                octets[0] is >= 224 and <= 239)
            {
                throw new LauncherException(
                    "Adres hosta nie może być localhostem, 0.0.0.0, " +
                    "broadcastem ani adresem multicast.");
            }

            return host;
        }

        if (Uri.CheckHostName(host) != UriHostNameType.Dns ||
            host.StartsWith(".", StringComparison.Ordinal) ||
            host.EndsWith(".", StringComparison.Ordinal) ||
            host.Contains("..", StringComparison.Ordinal) ||
            host.Equals("localhost", StringComparison.OrdinalIgnoreCase) ||
            host.EndsWith(".localhost", StringComparison.OrdinalIgnoreCase) ||
            host.Equals("localhost.localdomain", StringComparison.OrdinalIgnoreCase))
        {
            throw new LauncherException(
                "Brak prawidłowego adresu LAN hosta. Wpisz np. 192.168.1.25.");
        }

        return host;
    }
}
