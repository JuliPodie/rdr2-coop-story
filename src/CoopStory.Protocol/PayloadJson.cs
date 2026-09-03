using System.Text.Json;
using System.Text.Json.Serialization;

namespace CoopStory.Protocol;

// JSON is reserved for small human-readable control payloads such as heartbeat data.
// Realtime state uses BinaryPayloadCodec to keep packet size predictable.
public static class PayloadJson
{
    public static JsonSerializerOptions Options { get; } = CreateOptions();

    public static byte[] Serialize<T>(T value) =>
        JsonSerializer.SerializeToUtf8Bytes(value, Options);

    public static T Deserialize<T>(ReadOnlySpan<byte> payload)
    {
        try
        {
            return JsonSerializer.Deserialize<T>(payload, Options)
                ?? throw new ProtocolException(
                    $"JSON payload for {typeof(T).Name} contained null.");
        }
        catch (JsonException exception)
        {
            throw new ProtocolException(
                $"Invalid JSON payload for {typeof(T).Name}.",
                exception);
        }
    }

    private static JsonSerializerOptions CreateOptions()
    {
        // Strict options make a peer's JSON schema explicit: no comments, unknown members, trailing commas, or case-insensitive aliases.
        var options = new JsonSerializerOptions(JsonSerializerDefaults.Web)
        {
            AllowTrailingCommas = false,
            PropertyNameCaseInsensitive = false,
            ReadCommentHandling = JsonCommentHandling.Disallow,
            UnmappedMemberHandling = JsonUnmappedMemberHandling.Disallow,
            WriteIndented = false
        };
        options.Converters.Add(new JsonStringEnumConverter());
        return options;
    }
}
