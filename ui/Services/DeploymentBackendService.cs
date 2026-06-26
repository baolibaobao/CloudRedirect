using System;
using System.Collections.Generic;
using System.IO;
using System.Text;
using System.Text.Json;
using System.Text.RegularExpressions;

namespace CloudRedirect.Services;

public enum InjectionBackend
{
    SteamTools,
    OpenSteamTool
}

public sealed record OpenSteamToolStatus(
    bool OpenSteamToolDllExists,
    bool SupportsCloudRedirect,
    bool ConfigExists,
    bool CloudEnabled,
    bool LibraryConfigured,
    string ConfigPath);

public static class DeploymentBackendService
{
    private const string BackendKey = "injection_backend";
    private const string SteamToolsValue = "steamtools";
    private const string OpenSteamToolValue = "opensteamtool";
    private static readonly Regex AddAppIdRegex = new(
        @"\baddappid\s*\(\s*[""']?(?<id>\d+)[""']?",
        RegexOptions.IgnoreCase | RegexOptions.Compiled);

    public static InjectionBackend GetBackend(string? steamPath)
    {
        var configured = ReadBackendSetting();
        if (configured != null)
            return configured.Value;

        if (!string.IsNullOrWhiteSpace(steamPath)
            && File.Exists(Path.Combine(steamPath, "OpenSteamTool.dll")))
            return InjectionBackend.OpenSteamTool;

        return InjectionBackend.SteamTools;
    }

    public static void SaveBackendSetting(InjectionBackend backend)
    {
        var settingsPath = Path.Combine(SteamDetector.GetConfigDir(), "settings.json");
        ConfigHelper.SaveConfig(settingsPath, [BackendKey], writer =>
        {
            writer.WriteString(BackendKey, ToSettingValue(backend));
        });
    }

    public static string ToSettingValue(InjectionBackend backend)
    {
        return backend == InjectionBackend.OpenSteamTool
            ? OpenSteamToolValue
            : SteamToolsValue;
    }

    public static OpenSteamToolStatus GetOpenSteamToolStatus(string steamPath)
    {
        var configPath = GetOpenSteamToolConfigPath(steamPath);
        var configExists = File.Exists(configPath);
        var cloudEnabled = false;
        var libraryConfigured = false;

        if (configExists)
        {
            var cloud = ReadSection(configPath, "cloud");
            if (cloud.TryGetValue("enabled", out var enabled))
                cloudEnabled = enabled.Equals("true", StringComparison.OrdinalIgnoreCase);
            if (cloud.TryGetValue("library", out var library))
                libraryConfigured = library.Trim('"').Equals("cloud_redirect.dll", StringComparison.OrdinalIgnoreCase);
        }

        return new OpenSteamToolStatus(
            OpenSteamToolDllExists: File.Exists(Path.Combine(steamPath, "OpenSteamTool.dll")),
            SupportsCloudRedirect: OpenSteamToolSupportsCloudRedirect(steamPath),
            ConfigExists: configExists,
            CloudEnabled: cloudEnabled,
            LibraryConfigured: libraryConfigured,
            ConfigPath: configPath);
    }

    public static void ConfigureOpenSteamToolCloud(string steamPath)
    {
        var configPath = GetOpenSteamToolConfigPath(steamPath);
        var dir = Path.GetDirectoryName(configPath);
        if (!string.IsNullOrEmpty(dir))
            Directory.CreateDirectory(dir);

        var lines = File.Exists(configPath)
            ? new List<string>(File.ReadAllLines(configPath, Encoding.UTF8))
            : [];

        UpsertTomlKey(lines, "cloud", "enabled", "true");
        UpsertTomlKey(lines, "cloud", "library", "\"cloud_redirect.dll\"");

        FileUtils.AtomicWriteAllText(configPath, string.Join(Environment.NewLine, lines) + Environment.NewLine);
    }

    public static List<uint> ScanOpenSteamToolLuaAppIds(string steamPath)
    {
        var appIds = new HashSet<uint>();
        foreach (var luaDir in GetLuaDirectories(steamPath))
        {
            if (!Directory.Exists(luaDir)) continue;

            foreach (var file in Directory.GetFiles(luaDir, "*.lua"))
            {
                if (uint.TryParse(Path.GetFileNameWithoutExtension(file), out var fileAppId) && fileAppId != 0)
                    appIds.Add(fileAppId);

                try
                {
                    foreach (var line in File.ReadLines(file, Encoding.UTF8))
                    {
                        var trimmed = line.TrimStart();
                        if (trimmed.StartsWith("--")) continue;

                        var matches = AddAppIdRegex.Matches(line);
                        if (matches.Count == 0) continue;

                        if (uint.TryParse(Path.GetFileNameWithoutExtension(file), out var ownerAppId) && ownerAppId != 0)
                        {
                            appIds.Add(ownerAppId);
                            continue;
                        }

                        foreach (Match match in matches)
                        {
                            if (uint.TryParse(match.Groups["id"].Value, out var appId) && appId != 0)
                                appIds.Add(appId);
                        }
                    }
                }
                catch
                {
                    // Ignore one unreadable Lua file; the rest of the list is still useful.
                }
            }
        }

        var result = new List<uint>(appIds);
        result.Sort();
        return result;
    }

    private static IEnumerable<string> GetLuaDirectories(string steamPath)
    {
        yield return Path.Combine(steamPath, "config", "lua");
        yield return Path.Combine(steamPath, "config", "stplug-in");
    }

    private static string GetOpenSteamToolConfigPath(string steamPath)
    {
        return Path.Combine(steamPath, "opensteamtool.toml");
    }

    private static bool OpenSteamToolSupportsCloudRedirect(string steamPath)
    {
        try
        {
            var dllPath = Path.Combine(steamPath, "OpenSteamTool.dll");
            if (!File.Exists(dllPath)) return false;

            var data = File.ReadAllBytes(dllPath);
            return ContainsAscii(data, "cloud_redirect.dll")
                && ContainsAscii(data, "CR_InitCloudSave")
                && ContainsAscii(data, "CR_HandleCloudRpc");
        }
        catch
        {
            return false;
        }
    }

    private static bool ContainsAscii(byte[] data, string needle)
    {
        var pattern = Encoding.ASCII.GetBytes(needle);
        if (pattern.Length == 0 || data.Length < pattern.Length) return false;

        for (var i = 0; i <= data.Length - pattern.Length; i++)
        {
            var matched = true;
            for (var j = 0; j < pattern.Length; j++)
            {
                if (data[i + j] == pattern[j]) continue;
                matched = false;
                break;
            }
            if (matched) return true;
        }
        return false;
    }

    private static InjectionBackend? ReadBackendSetting()
    {
        try
        {
            var settingsPath = Path.Combine(SteamDetector.GetConfigDir(), "settings.json");
            if (!File.Exists(settingsPath)) return null;

            var json = File.ReadAllText(settingsPath);
            using var doc = JsonDocument.Parse(json);
            if (!doc.RootElement.TryGetProperty(BackendKey, out var prop))
                return null;

            return prop.GetString()?.ToLowerInvariant() switch
            {
                OpenSteamToolValue => InjectionBackend.OpenSteamTool,
                SteamToolsValue => InjectionBackend.SteamTools,
                _ => null
            };
        }
        catch
        {
            return null;
        }
    }

    private static Dictionary<string, string> ReadSection(string configPath, string sectionName)
    {
        var result = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
        var inSection = false;

        foreach (var rawLine in File.ReadLines(configPath, Encoding.UTF8))
        {
            var line = StripComment(rawLine).Trim();
            if (line.Length == 0) continue;

            if (line.StartsWith('[') && line.EndsWith(']'))
            {
                inSection = line.Trim('[', ']').Equals(sectionName, StringComparison.OrdinalIgnoreCase);
                continue;
            }

            if (!inSection) continue;

            var eq = line.IndexOf('=');
            if (eq <= 0) continue;
            result[line[..eq].Trim()] = line[(eq + 1)..].Trim();
        }

        return result;
    }

    private static void UpsertTomlKey(List<string> lines, string sectionName, string key, string value)
    {
        var sectionHeader = $"[{sectionName}]";
        var sectionStart = -1;
        var sectionEnd = lines.Count;

        for (var i = 0; i < lines.Count; i++)
        {
            var trimmed = StripComment(lines[i]).Trim();
            if (!trimmed.StartsWith('[') || !trimmed.EndsWith(']')) continue;

            if (trimmed.Trim('[', ']').Equals(sectionName, StringComparison.OrdinalIgnoreCase))
            {
                sectionStart = i;
                sectionEnd = lines.Count;
                for (var j = i + 1; j < lines.Count; j++)
                {
                    var next = StripComment(lines[j]).Trim();
                    if (next.StartsWith('[') && next.EndsWith(']'))
                    {
                        sectionEnd = j;
                        break;
                    }
                }
                break;
            }
        }

        if (sectionStart < 0)
        {
            if (lines.Count > 0 && !string.IsNullOrWhiteSpace(lines[^1]))
                lines.Add("");
            lines.Add(sectionHeader);
            lines.Add($"{key} = {value}");
            return;
        }

        for (var i = sectionStart + 1; i < sectionEnd; i++)
        {
            var line = StripComment(lines[i]);
            var eq = line.IndexOf('=');
            if (eq <= 0) continue;
            if (!line[..eq].Trim().Equals(key, StringComparison.OrdinalIgnoreCase)) continue;

            lines[i] = $"{key} = {value}";
            return;
        }

        lines.Insert(sectionEnd, $"{key} = {value}");
    }

    private static string StripComment(string line)
    {
        var hash = line.IndexOf('#');
        return hash >= 0 ? line[..hash] : line;
    }
}
