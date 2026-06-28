using System.Diagnostics;
using System.IO;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Controls;
using CloudRedirect.Resources;

namespace CloudRedirect.Pages;

public partial class CloudProviderPage : Page
{
    private Services.OAuthService? _oauth;
    private CancellationTokenSource? _authCts;
    private bool _isAuthenticating;
    private bool _loading;
    private readonly StringBuilder _logBuffer = new();

    public CloudProviderPage()
    {
        InitializeComponent();
        Loaded += async (_, _) =>
        {
            try { await LoadCurrentConfigAsync(); }
            catch { }
        };
        // Cancel in-flight OAuth when the user navigates away mid-auth.
        // Without this, the loopback HTTP listener in OAuthService keeps
        // running until the user closes their browser tab (or forever if
        // they don't), the auth state machine continuation keeps `this`
        // alive via the closure, and the per-message log callback
        // (msg => Dispatcher.BeginInvoke(...)) keeps marshaling work onto
        // a detached LogOutput. Cancelling _authCts triggers the existing
        // finally block in SignIn_Click which disposes _oauth + _authCts
        // and resets the UI state.
        Unloaded += (_, _) =>
        {
            if (_isAuthenticating)
                _authCts?.Cancel();
        };
    }

    /// <summary>
    /// Snapshot of everything LoadCurrentConfigAsync gathers off the UI
    /// thread. Pre-resolving the default local path here means the
    /// dispatcher continuation never has to fall back to a synchronous
    /// FindSteamPath() (registry + file probes) on Loaded.
    /// </summary>
    private sealed record LoadedConfigSnapshot(
        Services.CloudConfig? Config,
        string DefaultLocalPath,
        string PathTextOverride,
        Services.TokenStatus? TokenStatus);

    private sealed record WebDavSettings(string ServerUrl, string Username, string Password, string RemoteRootPath);
    private sealed record QuarkSettings(string Cookie, string OpenListExe, string DataDir, string AdminPassword,
        string BaseUrl, string MountPath, string RemoteRootPath, bool ForceStorageUpdate);

    // M14: Move SteamDetector.ReadConfig + FindSteamPath + OAuth token
    // status check off the UI thread. Loaded used to call them
    // synchronously; on a slow disk or stalled DPAPI prompt that froze
    // the dispatcher long enough for the page to render with a blank
    // status line. We now resolve the snapshot in Task.Run and apply it
    // to controls in the dispatcher continuation, mirroring
    // DashboardPage.LoadStatusAsync.
    private async Task LoadCurrentConfigAsync()
    {
        // _loading must be true the entire time we touch ProviderCombo /
        // TokenPathBox so the SelectionChanged handler doesn't fire
        // user-gesture branches against a partially-initialized UI. Set
        // it on the UI thread before launching the I/O.
        _loading = true;
        try
        {
            var snapshot = await Task.Run(() =>
            {
                var config = Services.SteamDetector.ReadConfig();
                var steamPath = Services.SteamDetector.FindSteamPath();
                var defaultLocal = steamPath != null
                    ? Path.Combine(steamPath, "localcloud")
                    : "";

                string pathOverride = "";
                if (config != null)
                {
                    if (config.TokenPath != null)
                        pathOverride = config.TokenPath;
                    else if (config.SyncPath != null)
                        pathOverride = config.SyncPath;
                }

                Services.TokenStatus? tokenStatus = null;
                if (config?.TokenPath != null && config.Provider is "gdrive" or "onedrive")
                    tokenStatus = Services.OAuthService.CheckTokenStatus(config.TokenPath);

                return new LoadedConfigSnapshot(config, defaultLocal, pathOverride, tokenStatus);
            });

            ApplyLoadedSnapshot(snapshot);
        }
        catch (Exception ex)
        {
            AuthStatus.Text = S.Format("CloudProvider_ErrorReadingConfig", ex.Message);
        }
        finally
        {
            _loading = false;
        }
    }

    private void ApplyLoadedSnapshot(LoadedConfigSnapshot snap)
    {
        if (snap.Config == null)
        {
            AuthStatus.Text = S.Get("CloudProvider_NoConfigFound");
            SelectProviderByTag("local");
            if (!string.IsNullOrEmpty(snap.DefaultLocalPath))
                TokenPathBox.Text = snap.DefaultLocalPath;
            return;
        }

        SelectProviderByTag(snap.Config.Provider);

        if (!string.IsNullOrEmpty(snap.PathTextOverride))
            TokenPathBox.Text = snap.PathTextOverride;
        else if (snap.Config.IsLocal || snap.Config.IsFolder)
        {
            if (!string.IsNullOrEmpty(snap.DefaultLocalPath))
                TokenPathBox.Text = snap.DefaultLocalPath;
        }

        UpdateProviderUI();
        if (snap.Config.Provider == "webdav")
            LoadWebDavSettings(TokenPathBox.Text);
        else if (snap.Config.Provider == "quark")
            LoadQuarkSettings(TokenPathBox.Text);
        // Use the pre-resolved token status so the dispatcher path never
        // re-enters CheckTokenStatus synchronously on Loaded. Only reach
        // the slow path on later user gestures (Provider change, Browse).
        UpdateAuthStatus(snap.TokenStatus);
    }

    private void SelectProviderByTag(string provider)
    {
        for (int i = 0; i < ProviderCombo.Items.Count; i++)
        {
            if (ProviderCombo.Items[i] is ComboBoxItem item && item.Tag as string == provider)
            {
                ProviderCombo.SelectedIndex = i;
                return;
            }
        }
    }

    /// <summary>
    /// Sets the path box to the default local storage path: &lt;steamdir&gt;/localcloud.
    /// Synchronous fallback for non-Loaded callers (BrowseToken, provider switch);
    /// the Loaded path uses the pre-resolved snapshot instead.
    /// </summary>
    private void SetDefaultLocalPath()
    {
        var steamPath = Services.SteamDetector.FindSteamPath();
        if (steamPath != null)
            TokenPathBox.Text = Path.Combine(steamPath, "localcloud");
    }

    private static string DefaultWebDavConfigPath() => Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData),
        "CloudRedirect", "webdav.json");

    private static string DefaultQuarkConfigPath() => Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData),
        "CloudRedirect", "quark_openlist.json");

    private static string DefaultQuarkDataDir() => Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData),
        "CloudRedirect", "openlist-data");

    private void ProviderCombo_SelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        if (_loading) return;

        UpdateProviderUI();

        if (ProviderCombo.SelectedItem is ComboBoxItem item)
        {
            var tag = item.Tag as string;
            if (tag == "gdrive")
            {
                TokenPathBox.Text = Path.Combine(
                    Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData),
                    "CloudRedirect", "google_tokens.json");
            }
            else if (tag == "onedrive")
            {
                TokenPathBox.Text = Path.Combine(
                    Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData),
                    "CloudRedirect", "onedrive_tokens.json");
            }
            else if (tag == "webdav")
            {
                TokenPathBox.Text = DefaultWebDavConfigPath();
                LoadWebDavSettings(TokenPathBox.Text);
            }
            else if (tag == "quark")
            {
                TokenPathBox.Text = DefaultQuarkConfigPath();
                LoadQuarkSettings(TokenPathBox.Text);
            }
            else if (tag is "local" or "folder")
            {
                SetDefaultLocalPath();
            }
        }

        UpdateAuthStatus();
    }

    /// <summary>
    /// Updates labels, enabled state, and hints for the selected provider.
    /// </summary>
    private void UpdateProviderUI()
    {
        if (ProviderCombo.SelectedItem is not ComboBoxItem item) return;

        var tag = item.Tag as string;
        bool needsTokens = tag is "gdrive" or "onedrive";
        bool isWebDav = tag == "webdav";
        bool isQuark = tag == "quark";
        bool isFolder = tag == "folder";
        bool isLocal = tag == "local";
        bool needsPath = needsTokens || isWebDav || isQuark || isFolder;

        TokenPathBox.IsEnabled = needsPath;
        BrowseButton.IsEnabled = needsPath;
        SignInButton.Visibility = needsTokens ? Visibility.Visible : Visibility.Collapsed;
        TestConnectionButton.Visibility = (needsTokens || isWebDav || isQuark) ? Visibility.Visible : Visibility.Collapsed;
        WebDavSettingsPanel.Visibility = isWebDav ? Visibility.Visible : Visibility.Collapsed;
        QuarkSettingsPanel.Visibility = isQuark ? Visibility.Visible : Visibility.Collapsed;

        // Update labels based on provider type
        if (isFolder)
        {
            PathLabel.Text = S.Get("CloudProvider_SyncFolderPath");
            TokenPathBox.PlaceholderText = S.Get("CloudProvider_SyncFolderPlaceholder");
            PathHint.Text = S.Get("CloudProvider_SyncFolderHint");
        }
        else if (isLocal)
        {
            PathLabel.Text = S.Get("CloudProvider_LocalStoragePath");
            TokenPathBox.PlaceholderText = "";
            PathHint.Text = S.Get("CloudProvider_LocalStorageHint");
            TokenPathBox.IsEnabled = false;
            BrowseButton.IsEnabled = false;
        }
        else if (needsTokens)
        {
            PathLabel.Text = S.Get("CloudProvider_TokenFilePath");
            TokenPathBox.PlaceholderText = S.Get("CloudProvider_TokenPlaceholder");
            PathHint.Text = "";
        }
        else if (isWebDav)
        {
            PathLabel.Text = S.Get("CloudProvider_WebDAVConfigPath");
            TokenPathBox.PlaceholderText = S.Get("CloudProvider_WebDAVConfigPlaceholder");
            PathHint.Text = S.Get("CloudProvider_WebDAVConfigHint");
        }
        else if (isQuark)
        {
            PathLabel.Text = S.Get("CloudProvider_QuarkConfigPath");
            TokenPathBox.PlaceholderText = S.Get("CloudProvider_QuarkConfigPlaceholder");
            PathHint.Text = S.Get("CloudProvider_QuarkConfigHint");
        }
        else
        {
            PathLabel.Text = S.Get("CloudProvider_TokenFilePath");
            TokenPathBox.PlaceholderText = "";
            PathHint.Text = "";
        }
    }

    private void BrowseToken_Click(object sender, RoutedEventArgs e)
    {
        var provider = GetSelectedProvider();

        if (provider == "folder")
        {
            var dialog = new Microsoft.Win32.OpenFolderDialog
            {
                Title = S.Get("CloudProvider_SelectSyncFolder"),
                Multiselect = false
            };

            if (!string.IsNullOrEmpty(TokenPathBox.Text) && Directory.Exists(TokenPathBox.Text))
                dialog.InitialDirectory = TokenPathBox.Text;

            if (dialog.ShowDialog() == true)
            {
                TokenPathBox.Text = dialog.FolderName;
                UpdateAuthStatus();
            }
        }
        else
        {
            var dialog = new Microsoft.Win32.OpenFileDialog
            {
                Title = S.Get("CloudProvider_SelectTokenFile"),
                Filter = "JSON files (*.json)|*.json|All files (*.*)|*.*",
                CheckFileExists = false
            };

            if (dialog.ShowDialog() == true)
            {
                TokenPathBox.Text = dialog.FileName;
                UpdateAuthStatus();
            }
        }
    }

    private async void SignIn_Click(object sender, RoutedEventArgs e)
    {
        if (_isAuthenticating) return;

        var provider = GetSelectedProvider();
        if (provider is "local" or "folder") return;

        var tokenPath = TokenPathBox.Text?.Trim();
        if (string.IsNullOrEmpty(tokenPath))
        {
            await Services.Dialog.ShowWarningAsync(S.Get("CloudProvider_MissingPath"),
                S.Get("CloudProvider_MissingPathMessage"));
            return;
        }

        _isAuthenticating = true;
        _authCts = new CancellationTokenSource();
        _oauth = new Services.OAuthService();

        // Update UI state
        SignInButton.IsEnabled = false;
        CancelAuthButton.Visibility = Visibility.Visible;
        ProviderCombo.IsEnabled = false;
        LogBorder.Visibility = Visibility.Visible;
        _logBuffer.Clear();
        LogOutput.Text = "";

        try
        {
            bool success = await _oauth.AuthorizeAsync(
                provider,
                tokenPath,
                msg => Dispatcher.BeginInvoke(() => AppendLog(msg)),
                _authCts.Token);

            if (success)
            {
                // Also save the config so the DLL picks up the new provider + token path
                await SaveConfigSilent();
            }
        }
        catch (OperationCanceledException)
        {
            AppendLog("认证已取消。");
        }
        catch (Exception ex)
        {
            AppendLog($"错误：{ex.Message}");
        }
        finally
        {
            _oauth?.Dispose();
            _oauth = null;
            _authCts?.Dispose();
            _authCts = null;
            _isAuthenticating = false;

            SignInButton.IsEnabled = true;
            CancelAuthButton.Visibility = Visibility.Collapsed;
            ProviderCombo.IsEnabled = true;

            UpdateAuthStatus();
        }
    }

    private void CancelAuth_Click(object sender, RoutedEventArgs e)
    {
        _authCts?.Cancel();
        // Don't dispose _oauth here -- the SignIn_Click finally block handles cleanup
        // after the async operation observes cancellation.
    }

    private async void SaveConfig_Click(object sender, RoutedEventArgs e)
    {
        if (await SaveConfigSilent())
        {
            await Services.Dialog.ShowInfoAsync(S.Get("CloudProvider_Saved"), S.Get("CloudProvider_SavedMessage"));
        }
    }

    private async void TestConnection_Click(object sender, RoutedEventArgs e)
    {
        var provider = GetSelectedProvider();
        if (provider is "local" or "folder") return;

        if (!await SaveConfigSilent())
            return;

        LogBorder.Visibility = Visibility.Visible;
        _logBuffer.Clear();
        LogOutput.Text = "";
        AppendLog(S.Format("CloudProvider_TestingProvider", provider));
        AuthStatus.Text = S.Get("CloudProvider_TestingConnection");
        AuthIcon.Symbol = Wpf.Ui.Controls.SymbolRegular.ShieldKeyhole24;

        TestConnectionButton.IsEnabled = false;
        SignInButton.IsEnabled = false;
        try
        {
            var result = await RunProviderAuthStatusAsync(provider);
            AppendLog(result.CommandLine);
            if (!string.IsNullOrWhiteSpace(result.StdErr))
                AppendLog(result.StdErr.Trim());
            AppendLog(result.StdOut.Trim());

            bool authenticated = false;
            string? error = null;
            try
            {
                using var doc = JsonDocument.Parse(result.StdOut);
                var root = doc.RootElement;
                if (root.TryGetProperty("authenticated", out var authProp) && authProp.ValueKind is JsonValueKind.True or JsonValueKind.False)
                    authenticated = authProp.GetBoolean();
                if (root.TryGetProperty("error", out var errorProp))
                    error = errorProp.GetString();
            }
            catch (JsonException ex)
            {
                error = S.Format("CloudProvider_InvalidCliResponse", ex.Message);
            }

            if (result.ExitCode == 0 && authenticated)
            {
                AuthStatus.Text = S.Get("CloudProvider_TestConnectionSuccess");
                AuthIcon.Symbol = Wpf.Ui.Controls.SymbolRegular.ShieldCheckmark24;
            }
            else
            {
                AuthStatus.Text = !string.IsNullOrWhiteSpace(error)
                    ? S.Format("CloudProvider_TestConnectionFailedWithError", error)
                    : S.Format("CloudProvider_TestConnectionFailedWithError", result.ExitCode.ToString());
                AuthIcon.Symbol = Wpf.Ui.Controls.SymbolRegular.ShieldDismiss24;
            }
        }
        catch (Exception ex)
        {
            AppendLog(ex.ToString());
            AuthStatus.Text = S.Format("CloudProvider_TestConnectionFailedWithError", ex.Message);
            AuthIcon.Symbol = Wpf.Ui.Controls.SymbolRegular.ShieldDismiss24;
        }
        finally
        {
            TestConnectionButton.IsEnabled = true;
            SignInButton.IsEnabled = true;
        }
    }

    private sealed record CliResult(int ExitCode, string StdOut, string StdErr, string CommandLine);

    private static async Task<CliResult> RunProviderAuthStatusAsync(string provider)
    {
        string? cliPath = Services.EmbeddedCli.EnsureExtracted();
        if (string.IsNullOrEmpty(cliPath) || !File.Exists(cliPath))
            throw new FileNotFoundException(S.Get("CloudProvider_CliUnavailable"));

        string arguments = $"auth-status {provider}";
        using var process = new Process
        {
            StartInfo = new ProcessStartInfo
            {
                FileName = cliPath,
                Arguments = arguments,
                UseShellExecute = false,
                RedirectStandardOutput = true,
                RedirectStandardError = true,
                CreateNoWindow = true,
            }
        };
        process.Start();
        var stdout = process.StandardOutput.ReadToEndAsync();
        var stderr = process.StandardError.ReadToEndAsync();
        await process.WaitForExitAsync();
        return new CliResult(process.ExitCode, await stdout, await stderr, $"{cliPath} {arguments}");
    }

    private static string ReadString(JsonElement root, string name, string fallback = "")
    {
        return root.TryGetProperty(name, out var prop) && prop.ValueKind == JsonValueKind.String
            ? prop.GetString() ?? fallback
            : fallback;
    }

    private void LoadWebDavSettings(string path)
    {
        WebDavServerUrlBox.Text = "";
        WebDavUsernameBox.Text = "";
        WebDavPasswordBox.Password = "";
        WebDavRemoteRootBox.Text = "/CloudRedirect";
        try
        {
            if (!File.Exists(path)) return;
            using var doc = JsonDocument.Parse(File.ReadAllText(path));
            var root = doc.RootElement;
            WebDavServerUrlBox.Text = ReadString(root, "server_url");
            WebDavUsernameBox.Text = ReadString(root, "username");
            WebDavPasswordBox.Password = ReadString(root, "password");
            WebDavRemoteRootBox.Text = ReadString(root, "remote_root_path", "/CloudRedirect");
        }
        catch { }
    }

    private void LoadQuarkSettings(string path)
    {
        QuarkCookieBox.Text = "";
        QuarkOpenListPathBox.Text = Services.EmbeddedOpenList.DefaultExtractedPath;
        QuarkDataDirBox.Text = DefaultQuarkDataDir();
        QuarkRemoteRootBox.Text = "/Quark/CloudRedirect";
        QuarkForceStorageUpdateBox.IsChecked = false;
        try
        {
            if (!File.Exists(path)) return;
            using var doc = JsonDocument.Parse(File.ReadAllText(path));
            var root = doc.RootElement;
            QuarkCookieBox.Text = ReadString(root, "cookie");
            QuarkOpenListPathBox.Text = ReadString(root, "openlist_exe", Services.EmbeddedOpenList.DefaultExtractedPath);
            QuarkDataDirBox.Text = ReadString(root, "data_dir", DefaultQuarkDataDir());
            QuarkRemoteRootBox.Text = ReadString(root, "remote_root_path", "/Quark/CloudRedirect");
            if (root.TryGetProperty("force_storage_update", out var force) && force.ValueKind is JsonValueKind.True or JsonValueKind.False)
                QuarkForceStorageUpdateBox.IsChecked = force.GetBoolean();
        }
        catch { }
    }

    private static string ExistingString(string path, string name, string fallback = "")
    {
        try
        {
            if (!File.Exists(path)) return fallback;
            using var doc = JsonDocument.Parse(File.ReadAllText(path));
            return ReadString(doc.RootElement, name, fallback);
        }
        catch { return fallback; }
    }

    private static string GenerateOpenListAdminPassword()
    {
        const string alphabet = "ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz23456789";
        Span<char> chars = stackalloc char[28];
        for (var i = 0; i < chars.Length; i++)
            chars[i] = alphabet[RandomNumberGenerator.GetInt32(alphabet.Length)];
        return new string(chars);
    }

    private static void WriteJsonFile(string path, Action<Utf8JsonWriter> write)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(path)!);
        using var ms = new MemoryStream();
        using (var writer = new Utf8JsonWriter(ms, new JsonWriterOptions { Indented = true }))
        {
            writer.WriteStartObject();
            write(writer);
            writer.WriteEndObject();
        }
        Services.FileUtils.AtomicWriteAllText(path, Encoding.UTF8.GetString(ms.ToArray()));
    }

    private void SaveWebDavSettings(string path)
    {
        var existingPassword = ExistingString(path, "password");
        var password = WebDavPasswordBox.Password ?? "";
        if (string.IsNullOrEmpty(password))
            password = existingPassword;

        WriteJsonFile(path, writer =>
        {
            writer.WriteString("server_url", WebDavServerUrlBox.Text?.Trim() ?? "");
            writer.WriteString("username", WebDavUsernameBox.Text?.Trim() ?? "");
            writer.WriteString("password", password);
            writer.WriteString("remote_root_path", string.IsNullOrWhiteSpace(WebDavRemoteRootBox.Text) ? "/CloudRedirect" : WebDavRemoteRootBox.Text.Trim());
        });
    }

    private void SaveQuarkSettings(string path)
    {
        var openListPath = Services.EmbeddedOpenList.EnsureExtracted() ?? QuarkOpenListPathBox.Text?.Trim() ?? Services.EmbeddedOpenList.DefaultExtractedPath;
        if (string.IsNullOrWhiteSpace(QuarkOpenListPathBox.Text) || !File.Exists(QuarkOpenListPathBox.Text.Trim()))
            QuarkOpenListPathBox.Text = openListPath;

        var existingPassword = ExistingString(path, "admin_password");
        if (string.IsNullOrWhiteSpace(existingPassword))
            existingPassword = GenerateOpenListAdminPassword();
        var existingCookie = ExistingString(path, "cookie");
        var enteredCookie = QuarkCookieBox.Text?.Trim() ?? "";
        var cookie = enteredCookie;
        if (string.IsNullOrEmpty(cookie))
            cookie = existingCookie;
        var forceStorageUpdate = QuarkForceStorageUpdateBox.IsChecked == true
            || (!string.IsNullOrEmpty(enteredCookie)
                && !enteredCookie.Equals(existingCookie, StringComparison.Ordinal));

        WriteJsonFile(path, writer =>
        {
            writer.WriteString("cookie", cookie);
            writer.WriteString("openlist_exe", QuarkOpenListPathBox.Text?.Trim() ?? openListPath);
            writer.WriteString("data_dir", string.IsNullOrWhiteSpace(QuarkDataDirBox.Text) ? DefaultQuarkDataDir() : QuarkDataDirBox.Text.Trim());
            writer.WriteString("admin_password", existingPassword);
            writer.WriteString("base_url", "http://127.0.0.1:5244");
            writer.WriteString("mount_path", "/Quark");
            writer.WriteString("remote_root_path", string.IsNullOrWhiteSpace(QuarkRemoteRootBox.Text) ? "/Quark/CloudRedirect" : QuarkRemoteRootBox.Text.Trim());
            writer.WriteBoolean("force_storage_update", forceStorageUpdate);
        });
    }

    /// <summary>
    /// Writes config.json without showing a dialog. Returns true on success.
    /// </summary>
    private async Task<bool> SaveConfigSilent()
    {
        var configDir = Services.SteamDetector.GetConfigDir();

        Directory.CreateDirectory(configDir);

        var provider = GetSelectedProvider();
        var tokenPath = TokenPathBox.Text?.Trim() ?? "";

        // "local" in the UI maps to "folder" provider in the DLL with the
        // default localcloud path, so the DLL has a concrete storage location.
        var configProvider = provider;
        if (provider == "local")
            configProvider = "folder";

        var configPath = Path.Combine(configDir, "config.json");

        try
        {
            if (provider == "webdav")
                SaveWebDavSettings(tokenPath);
            else if (provider == "quark")
                SaveQuarkSettings(tokenPath);

            Services.ConfigHelper.SaveConfig(configPath,
                new[] { "provider", "sync_path", "token_path" },
                writer =>
                {
                    writer.WriteString("provider", configProvider);
                    if (configProvider == "folder")
                        writer.WriteString("sync_path", tokenPath);
                    else if (configProvider is not "local")
                        writer.WriteString("token_path", tokenPath);
                });
            return true;
        }
        catch (Exception ex)
        {
            await Services.Dialog.ShowErrorAsync(S.Get("Common_Error"), S.Format("CloudProvider_FailedSaveConfig", ex.Message));
            return false;
        }
    }

    private string GetSelectedProvider()
    {
        if (ProviderCombo.SelectedItem is ComboBoxItem item)
            return item.Tag as string ?? "local";
        return "local";
    }

    private void UpdateAuthStatus(Services.TokenStatus? preCheckedStatus = null)
    {
        if (ProviderCombo.SelectedItem is not ComboBoxItem item) return;

        var tag = item.Tag as string;

        if (tag == "local")
        {
            var localPath = TokenPathBox.Text?.Trim();
            if (!string.IsNullOrEmpty(localPath))
                AuthStatus.Text = S.Format("CloudProvider_LocalModeStored", localPath);
            else
                AuthStatus.Text = S.Get("CloudProvider_LocalModeNoSync");
            AuthIcon.Symbol = Wpf.Ui.Controls.SymbolRegular.ShieldCheckmark24;
            return;
        }

        if (tag == "folder")
        {
            var folderPath = TokenPathBox.Text?.Trim();
            if (string.IsNullOrEmpty(folderPath))
            {
                AuthStatus.Text = S.Get("CloudProvider_NoSyncFolder");
                AuthIcon.Symbol = Wpf.Ui.Controls.SymbolRegular.ShieldKeyhole24;
            }
            else if (Directory.Exists(folderPath))
            {
                AuthStatus.Text = S.Format("CloudProvider_FolderAccessible", folderPath);
                AuthIcon.Symbol = Wpf.Ui.Controls.SymbolRegular.ShieldCheckmark24;
            }
            else
            {
                AuthStatus.Text = S.Format("CloudProvider_FolderNotFound", folderPath);
                AuthIcon.Symbol = Wpf.Ui.Controls.SymbolRegular.ShieldDismiss24;
            }
            return;
        }

        var tokenPath = TokenPathBox.Text?.Trim();
        if (tag == "webdav" || tag == "quark")
        {
            bool isQuarkConfig = tag == "quark";
            if (string.IsNullOrEmpty(tokenPath))
            {
                AuthStatus.Text = S.Get(isQuarkConfig ? "CloudProvider_NoQuarkConfigPath" : "CloudProvider_NoWebDAVConfigPath");
                AuthIcon.Symbol = Wpf.Ui.Controls.SymbolRegular.ShieldKeyhole24;
            }
            else if (File.Exists(tokenPath))
            {
                AuthStatus.Text = S.Format(isQuarkConfig ? "CloudProvider_QuarkConfigFound" : "CloudProvider_WebDAVConfigFound", tokenPath);
                AuthIcon.Symbol = Wpf.Ui.Controls.SymbolRegular.ShieldCheckmark24;
            }
            else
            {
                AuthStatus.Text = S.Format(isQuarkConfig ? "CloudProvider_QuarkConfigMissing" : "CloudProvider_WebDAVConfigMissing", tokenPath);
                AuthIcon.Symbol = Wpf.Ui.Controls.SymbolRegular.ShieldDismiss24;
            }
            return;
        }

        if (string.IsNullOrEmpty(tokenPath))
        {
            AuthStatus.Text = S.Get("CloudProvider_NoTokenFilePath");
            AuthIcon.Symbol = Wpf.Ui.Controls.SymbolRegular.ShieldKeyhole24;
            return;
        }

        // Caller (the Loaded path) may pass a status that was already
        // resolved off the UI thread; otherwise we hit DPAPI + file I/O
        // synchronously. The user-gesture callers (Browse, provider
        // switch, post-OAuth) accept the synchronous cost in exchange
        // for keeping their flow simple -- those events are already
        // tied to a click and the user has paid attention.
        var status = preCheckedStatus ?? Services.OAuthService.CheckTokenStatus(tokenPath);
        AuthStatus.Text = status.Message;
        AuthIcon.Symbol = status.IsAuthenticated
            ? Wpf.Ui.Controls.SymbolRegular.ShieldCheckmark24
            : Wpf.Ui.Controls.SymbolRegular.ShieldKeyhole24;
    }

    private void AppendLog(string message)
    {
        if (_logBuffer.Length > 0)
            _logBuffer.AppendLine();
        _logBuffer.Append(message);
        LogOutput.Text = _logBuffer.ToString();
        LogScroll.ScrollToEnd();
    }
}
