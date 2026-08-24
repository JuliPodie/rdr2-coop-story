using System.Security.Cryptography;
using System.Text;

namespace CoopStory.Launcher;

public static class Hashing
{
    public static string FileSha256(string path)
    {
        using var stream = new FileStream(
            path,
            FileMode.Open,
            FileAccess.Read,
            FileShare.Read,
            128 * 1024,
            FileOptions.SequentialScan);
        return Convert.ToHexString(SHA256.HashData(stream));
    }

    public static string BytesSha256(ReadOnlySpan<byte> bytes) =>
        Convert.ToHexString(SHA256.HashData(bytes));

    public static string TextSha256(string value) =>
        BytesSha256(Encoding.UTF8.GetBytes(value));
}
