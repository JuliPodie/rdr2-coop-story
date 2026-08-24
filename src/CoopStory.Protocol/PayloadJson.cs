using System.Text.Json;
using System.Text.Json.Serialization;

namespace CoopStory.Protocol;

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
