using System.IO;
using System.Reflection;

namespace CloudRedirect.Services;

internal static class EmbeddedOpenList
{
    private const string ResourceName = "payloads/openlist/openlist.exe";

    public static string DefaultExtractedPath => Path.Combine(
        SteamDetector.GetConfigDir(), "openlist", "openlist.exe");

    public static bool IsEmbedded()
    {
        return Assembly.GetExecutingAssembly().GetManifestResourceInfo(ResourceName) != null;
    }

    public static string? EnsureExtracted()
    {
        if (File.Exists(DefaultExtractedPath))
            return DefaultExtractedPath;

        using var stream = Assembly.GetExecutingAssembly().GetManifestResourceStream(ResourceName);
        if (stream == null)
            return null;

        using var ms = new MemoryStream(checked((int)stream.Length));
        stream.CopyTo(ms);
        Directory.CreateDirectory(Path.GetDirectoryName(DefaultExtractedPath)!);
        FileUtils.AtomicWriteAllBytes(DefaultExtractedPath, ms.ToArray());
        return DefaultExtractedPath;
    }
}
