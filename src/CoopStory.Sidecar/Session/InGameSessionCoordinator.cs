using System.Net;
using System.Net.NetworkInformation;
using System.Net.Sockets;
using CoopStory.Protocol;
using CoopStory.Sidecar.Configuration;

namespace CoopStory.Sidecar.Session;

// Complete configuration produced by the menu's Host/Join choice.
// The caller passes this to the running session; it does not itself open sockets.
public sealed record SessionActivation(
    SidecarConfig Config,
    SessionCredentials Credentials,
    string InviteCode);

// Converts local menu input into validated host or guest session settings.
// Keeping this outside the game bridge makes joining testable without RDR2.
public static class InGameSessionCoordinator
{
    public static SessionActivation CreateHost(SidecarConfig bootstrap)
    {
        ArgumentNullException.ThrowIfNull(bootstrap);
        // Prefer the saved adapter choice, otherwise offer the first active private/virtual-LAN IPv4 address that can be shared with a guest.
        var address = SuggestedLanAddress(bootstrap.HostAddress) ??
            throw new ConfigurationException(
                "No private host IPv4 address was detected. Connect the computer " +
                "to a LAN or Hamachi network and try again.");
        return CreateHost(bootstrap, address);
    }

    public static SessionActivation CreateHost(
        SidecarConfig bootstrap,
        string advertisedAddress)
    {
        ArgumentNullException.ThrowIfNull(bootstrap);
        var address = advertisedAddress.Trim();
        _ = HostAddressValidator.Validate(address, requireRemote: true);
        // A host creates a new session ID and secret for every fresh invite.
        var credentials = SessionCredentials.Generate();
        var config = bootstrap with
        {
            Role = SessionRole.Host,
            HostAddress = address,
            SessionToken = credentials.ExportToken()
        };
        config.Validate();
        // The compact code contains connection coordinates plus the credential token that the later TCP/UDP authentication proof relies on.
        var code = SessionInviteCodeCodec.Encode(
            new SessionInviteCode(
                address,
                checked((ushort)config.TcpPort),
                checked((ushort)config.UdpPort),
                config.SessionToken));
        return new SessionActivation(config, credentials, code);
    }

    public static SessionActivation CreateGuest(
        SidecarConfig bootstrap,
        string inviteCode)
    {
        ArgumentNullException.ThrowIfNull(bootstrap);
        SidecarConfig config;
        SessionCredentials credentials;

        // A guest must join from a complete host-produced invite, rather than attempting to reuse partial menu text or a stale blank value.
        if (string.IsNullOrWhiteSpace(inviteCode))
        {
            throw new ConfigurationException(
                "The clipboard does not contain an R2C1 code. Copy a fresh code from the host " +
                "and select JOIN again.");
        }
        else
        {
            SessionInviteCode invite;
            try
            {
                invite = SessionInviteCodeCodec.Decode(inviteCode);
            }
            catch (FormatException exception)
            {
                throw new ConfigurationException(
                    "The invitation code is invalid. Copy the complete code beginning with R2C1.",
                    exception);
            }

            // The invite is input from another machine: validate both normal host syntax and the product's private-LAN-only policy.
            _ = HostAddressValidator.Validate(
                invite.HostAddress,
                requireRemote: true);
            RequirePrivateLanAddress(invite.HostAddress);
            credentials = SessionCredentials.ParseToken(invite.SessionToken);
            config = bootstrap with
            {
                Role = SessionRole.Guest,
                HostAddress = invite.HostAddress,
                TcpPort = invite.TcpPort,
                UdpPort = invite.UdpPort,
                SessionToken = invite.SessionToken
            };
        }

        config.Validate();
        return new SessionActivation(config, credentials, string.Empty);
    }

    public static string? SuggestedLanAddress(string? preferred = null)
    {
        try
        {
            var activeAddresses = NetworkInterface.GetAllNetworkInterfaces()
                .Where(network =>
                    network.OperationalStatus == OperationalStatus.Up &&
                    network.NetworkInterfaceType != NetworkInterfaceType.Loopback)
                .SelectMany(network =>
                    network.GetIPProperties().UnicastAddresses)
                .Where(address =>
                    address.Address.AddressFamily ==
                        AddressFamily.InterNetwork &&
                    !IPAddress.IsLoopback(address.Address) &&
                    address.IPv4Mask is not null)
                .Select(address => address.Address);
            return SelectSuggestedLanAddress(preferred, activeAddresses);
        }
        catch (NetworkInformationException)
        {
            return null;
        }
    }

    internal static string? SelectSuggestedLanAddress(
        string? preferred,
        IEnumerable<IPAddress> activeLocalAddresses)
    {
        ArgumentNullException.ThrowIfNull(activeLocalAddresses);
        // Adapter discovery can include VPNs; Hamachi's 25.x range is admitted deliberately alongside the ordinary private IPv4 ranges.
        var candidates = activeLocalAddresses
            .Where(address =>
                address.AddressFamily == AddressFamily.InterNetwork &&
                !IPAddress.IsLoopback(address) &&
                IsPrivateOrVirtualLanAddress(address))
            .Select(address => address.ToString())
            .Distinct(StringComparer.Ordinal)
            .ToArray();
        var preferredAddress = preferred?.Trim();
        if (!string.IsNullOrEmpty(preferredAddress) &&
            candidates.Contains(
                preferredAddress,
                StringComparer.Ordinal))
        {
            return preferredAddress;
        }
        return candidates.FirstOrDefault();
    }

    private static bool IsPrivateOrVirtualLanAddress(IPAddress address)
    {
        var bytes = address.GetAddressBytes();
        // 25.x is Hamachi. The other ranges are RFC1918 plus link-local.
        return bytes[0] == 25 ||
               bytes[0] == 10 ||
               (bytes[0] == 172 && bytes[1] is >= 16 and <= 31) ||
               (bytes[0] == 192 && bytes[1] == 168) ||
               (bytes[0] == 169 && bytes[1] == 254);
    }

    private static void RequirePrivateLanAddress(string hostAddress)
    {
        if (!IPAddress.TryParse(hostAddress, out var address) ||
            address.AddressFamily != AddressFamily.InterNetwork ||
            !IsPrivateOrVirtualLanAddress(address))
        {
            throw new ConfigurationException(
                "The R2C1 code must point to a private LAN IPv4 address " +
                "or a Hamachi 25.x.x.x IPv4 address.");
        }
    }
}
