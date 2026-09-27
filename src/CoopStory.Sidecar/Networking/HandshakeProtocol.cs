using System.Net.Sockets;
using CoopStory.Protocol;

namespace CoopStory.Sidecar.Networking;

// The only peer identity that is trusted after the proof exchange completes.
// ControlSequence becomes the starting point for replay/order validation.
internal readonly record struct AuthenticatedPeerHandshake(
    Guid InstanceId,
    uint ControlSequence);

// Performs the TCP challenge-response handshake before any gameplay packet is accepted.
// Knowing an invite's session ID alone is not enough: the secret is needed to prove membership in the session.
internal static class HandshakeProtocol
{
    public static async Task<AuthenticatedPeerHandshake> AcceptGuestAsync(
        ControlConnection connection,
        SessionCredentials credentials,
        Guid hostInstanceId,
        Func<uint> nextSequence,
        CancellationToken cancellationToken)
    {
        // A guest must identify itself first; no arbitrary gameplay frame may establish a TCP connection's identity.
        var envelope = await connection.ReceiveAsync(cancellationToken).ConfigureAwait(false)
            ?? throw new ProtocolException("Peer closed before sending Hello.");
        if (envelope.Type != MessageType.Hello)
        {
            throw new ProtocolException("First TCP frame must be Hello.");
        }

        var hello = PayloadJson.Deserialize<HelloPayload>(envelope.Payload.Span);
        // Verify protocol version, session ID, declared role, and HMAC proof before allocating this TCP connection to a guest.
        var rejection = ValidateHello(hello, credentials);
        if (rejection is not null)
        {
            var rejected = new HelloAckPayload(
                credentials.SessionId,
                hostInstanceId,
                Accepted: false,
                ServerNonce: string.Empty,
                Proof: string.Empty,
                Reason: rejection);
            await connection.SendAsync(
                new ProtocolEnvelope(
                    MessageType.HelloAck,
                    nextSequence(),
                    NetworkClock.Tick,
                    PayloadJson.Serialize(rejected)),
                cancellationToken).ConfigureAwait(false);
            throw new ProtocolException($"Guest handshake rejected: {rejection}");
        }

        // Use a fresh server nonce so a captured old HelloAck cannot be replayed as a successful response to this guest's connection attempt.
        var serverNonce = SessionCredentials.CreateNonce();
        var proof = credentials.CreateServerProof(
            hostInstanceId,
            hello.InstanceId,
            hello.Nonce,
            serverNonce);
        var accepted = new HelloAckPayload(
            credentials.SessionId,
            hostInstanceId,
            Accepted: true,
            serverNonce,
            proof);
        await connection.SendAsync(
            new ProtocolEnvelope(
                MessageType.HelloAck,
                nextSequence(),
                NetworkClock.Tick,
                PayloadJson.Serialize(accepted)),
            cancellationToken).ConfigureAwait(false);
        return new AuthenticatedPeerHandshake(
            hello.InstanceId,
            envelope.Sequence);
    }

    public static async Task<AuthenticatedPeerHandshake> ConnectToHostAsync(
        ControlConnection connection,
        SessionCredentials credentials,
        Guid guestInstanceId,
        Func<uint> nextSequence,
        CancellationToken cancellationToken)
    {
        // The guest supplies a new unpredictable challenge for this particular host connection attempt.
        var nonce = SessionCredentials.CreateNonce();
        var hello = new HelloPayload(
            credentials.SessionId,
            guestInstanceId,
            SessionRole.Guest,
            nonce,
            credentials.CreateClientProof(guestInstanceId, SessionRole.Guest, nonce));
        await connection.SendAsync(
            new ProtocolEnvelope(
                MessageType.Hello,
                nextSequence(),
                NetworkClock.Tick,
                PayloadJson.Serialize(hello)),
            cancellationToken).ConfigureAwait(false);

        // Do not consider the host connected until its reply proves it knows both the shared secret and the nonce just generated above.
        var envelope = await connection.ReceiveAsync(cancellationToken).ConfigureAwait(false)
            ?? throw new ProtocolException("Host closed before sending HelloAck.");
        if (envelope.Type != MessageType.HelloAck)
        {
            throw new ProtocolException("Host did not answer with HelloAck.");
        }

        var ack = PayloadJson.Deserialize<HelloAckPayload>(envelope.Payload.Span);
        if (!ack.Accepted)
        {
            throw new ProtocolException($"Host rejected handshake: {ack.Reason ?? "unspecified"}");
        }

        // Reject a valid-looking acknowledgement from another version/session, or one whose proof was not made for this exact guest nonce.
        if (ack.ProtocolVersion != ProtocolConstants.Version ||
            ack.SessionId != credentials.SessionId ||
            !credentials.VerifyServerProof(
                ack.HostInstanceId,
                guestInstanceId,
                nonce,
                ack.ServerNonce,
                ack.Proof))
        {
            throw new ProtocolException("Host handshake proof is invalid.");
        }

        return new AuthenticatedPeerHandshake(
            ack.HostInstanceId,
            envelope.Sequence);
    }

    private static string? ValidateHello(
        HelloPayload hello,
        SessionCredentials credentials)
    {
        // Return a terse machine-readable reason because this answer is sent to an unauthenticated peer and must not disclose session details.
        if (hello.ProtocolVersion != ProtocolConstants.Version)
        {
            return "protocol-version";
        }

        if (hello.SessionId != credentials.SessionId)
        {
            return "session-id";
        }

        if (hello.InstanceId == Guid.Empty || hello.Role != SessionRole.Guest)
        {
            return "role-or-instance";
        }

        if (string.IsNullOrWhiteSpace(hello.Nonce) ||
            !credentials.VerifyClientProof(
                hello.InstanceId,
                hello.Role,
                hello.Nonce,
                hello.Proof))
        {
            return "authentication";
        }

        return null;
    }
}
