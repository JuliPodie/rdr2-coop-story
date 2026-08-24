using System.Net;
using System.Net.NetworkInformation;
using System.Net.Sockets;
using CoopStory.Protocol;
using CoopStory.Sidecar.Configuration;

namespace CoopStory.Sidecar.Session;

public sealed record SessionActivation(
    SidecarConfig Config,
    SessionCredentials Credentials,
    string InviteCode);

public static class InGameSessionCoordinator
{
    public static SessionActivation CreateHost(SidecarConfig bootstrap)
    {
        ArgumentNullException.ThrowIfNull(bootstrap);
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
        var credentials = SessionCredentials.Generate();
        var config = bootstrap with
        {
            Role = SessionRole.Host,
            HostAddress = address,
            SessionToken = credentials.ExportToken()
        };
        config.Validate();
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
