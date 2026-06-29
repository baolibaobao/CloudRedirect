using System;
using System.IO;
using System.Reflection;

namespace CloudRedirect.Services;

internal static class EmbeddedCli
{
    private const string CliResourceName = "cloud_redirect_cli.exe";
    private const string DllResourceName = "cloud_redirect.dll";
    private static string? _cachedExtractedPath;

    public static string? EnsureExtracted()
    {
        string appDirCli = Path.Combine(AppContext.BaseDirectory, "cloud_redirect_cli.exe");
        if (File.Exists(appDirCli))
        {
            _cachedExtractedPath = appDirCli;
            return appDirCli;
        }

        if (_cachedExtractedPath != null && File.Exists(_cachedExtractedPath))
            return _cachedExtractedPath;

        var assembly = Assembly.GetExecutingAssembly();
        using var cliStream = assembly.GetManifestResourceStream(CliResourceName);
        using var dllStream = assembly.GetManifestResourceStream(DllResourceName);
        if (cliStream == null || dllStream == null)
            return null;

        string baseDir = Path.Combine(Path.GetTempPath(), "CloudRedirect", ComputeResourceHash(cliStream, dllStream));
        Directory.CreateDirectory(baseDir);

        string exePath = Path.Combine(baseDir, "cloud_redirect_cli.exe");
        string dllPath = Path.Combine(baseDir, "cloud_redirect.dll");
        cliStream.Position = 0;
        using (var ms = new MemoryStream(checked((int)cliStream.Length)))
        {
            cliStream.CopyTo(ms);
            FileUtils.AtomicWriteAllBytes(exePath, ms.ToArray());
        }

        dllStream.Position = 0;
        using (var ms = new MemoryStream(checked((int)dllStream.Length)))
        {
            dllStream.CopyTo(ms);
            FileUtils.AtomicWriteAllBytes(dllPath, ms.ToArray());
        }

        _cachedExtractedPath = exePath;
        return exePath;
    }

    private static string ComputeResourceHash(params Stream[] streams)
    {
        using var sha = System.Security.Cryptography.SHA256.Create();
        var buffer = new byte[81920];
        foreach (var stream in streams)
        {
            stream.Position = 0;
            int read;
            while ((read = stream.Read(buffer, 0, buffer.Length)) > 0)
                sha.TransformBlock(buffer, 0, read, null, 0);
        }
        sha.TransformFinalBlock(Array.Empty<byte>(), 0, 0);
        var hash = sha.Hash ?? [];
        return Convert.ToHexString(hash).Substring(0, 16);
    }
}
