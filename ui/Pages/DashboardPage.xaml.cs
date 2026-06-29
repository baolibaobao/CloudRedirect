using System;
using System.Diagnostics;
using System.IO;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Controls;
using CloudRedirect.Resources;

namespace CloudRedirect.Pages;

public partial class DashboardPage : Page
{
    public DashboardPage()
    {
        InitializeComponent();
        Loaded += async (_, _) =>
        {
            try { await LoadStatusAsync(); }
            catch { }
        };
    }

    // M16: Gather data off the UI thread, update controls on dispatcher
    private async Task LoadStatusAsync()
    {
        var data = await Task.Run(() =>
        {
            var steamPath = Services.SteamDetector.FindSteamPath();
            bool dllExists = false;
            bool? dllCurrent = null;
            Services.CloudConfig config = null;
            int appCount = 0;
            Services.TokenStatus tokenStatus = null;
            Services.InjectionBackend backend = Services.InjectionBackend.SteamTools;
            Services.OpenSteamToolStatus ostStatus = null;

            if (steamPath != null)
            {
                backend = Services.DeploymentBackendService.GetBackend(steamPath);
                if (backend == Services.InjectionBackend.OpenSteamTool)
                    ostStatus = Services.DeploymentBackendService.GetOpenSteamToolStatus(steamPath);

                var dllPath = Path.Combine(steamPath, "cloud_redirect.dll");
                dllExists = File.Exists(dllPath);
                if (dllExists)
                    dllCurrent = Services.EmbeddedDll.IsDeployedCurrent(dllPath);
                config = Services.SteamDetector.ReadConfig();

                var storagePath = Path.Combine(steamPath, "cloud_redirect", "storage");
                if (Directory.Exists(storagePath))
                {
                    foreach (var accountDir in Directory.GetDirectories(storagePath))
                        appCount += Directory.GetDirectories(accountDir).Length;
                }

                // M9: Check OAuth token status off the UI thread (DPAPI + file I/O)
                if (config?.TokenPath != null && config.Provider is "gdrive" or "onedrive")
                    tokenStatus = Services.OAuthService.CheckTokenStatus(config.TokenPath);
            }

            return (steamPath, dllExists, dllCurrent, config, appCount, tokenStatus, backend, ostStatus);
        });

        // Update UI on dispatcher thread
        SteamStatus.Text = data.steamPath ?? S.Get("Dashboard_NotFound");

        if (data.steamPath != null)
        {
            if (!data.dllExists)
            {
                DllStatus.Text = S.Get("Dashboard_DllNotInstalled");
                DllIcon.Symbol = Wpf.Ui.Controls.SymbolRegular.PlugDisconnected24;
            }
            else if (data.dllCurrent == false)
            {
                DllStatus.Text = S.Get("Dashboard_DllInstalled");
                DllIcon.Symbol = Wpf.Ui.Controls.SymbolRegular.ArrowSync24;
            }
            else
            {
                DllStatus.Text = S.Get("Dashboard_DllInstalled");
                DllIcon.Symbol = Wpf.Ui.Controls.SymbolRegular.PlugConnected24;
            }

            if (data.config != null)
                UpdateProviderAuthStatus(data.config, data.tokenStatus);

            AppCount.Text = S.Format("Dashboard_AppCountFormat", data.appCount);
            UpdateBackendStatus(data.backend, data.ostStatus);
        }
    }

    private void UpdateBackendStatus(Services.InjectionBackend backend, Services.OpenSteamToolStatus ostStatus)
    {
        if (backend == Services.InjectionBackend.OpenSteamTool)
        {
            if (ostStatus == null)
            {
                BackendStatus.Text = "OpenSteamTool 模式：等待检测";
                BackendIcon.Symbol = Wpf.Ui.Controls.SymbolRegular.PlugDisconnected24;
                return;
            }

            var ready = ostStatus.OpenSteamToolDllExists
                && ostStatus.SupportsCloudRedirect
                && ostStatus.ConfigExists
                && ostStatus.CloudEnabled
                && ostStatus.LibraryConfigured;

            BackendStatus.Text = ready
                ? "OpenSteamTool 模式已就绪"
                : !ostStatus.OpenSteamToolDllExists
                    ? "OpenSteamTool 模式未完成：未检测到 OpenSteamTool.dll"
                    : !ostStatus.SupportsCloudRedirect
                        ? "OpenSteamTool 模式不可用：当前 OpenSteamTool.dll 不支持 CloudRedirect [cloud]"
                        : "OpenSteamTool 模式未完成：请到安装设置页写入 [cloud] 配置";
            BackendIcon.Symbol = ready
                ? Wpf.Ui.Controls.SymbolRegular.PlugConnected24
                : Wpf.Ui.Controls.SymbolRegular.Warning24;
            return;
        }

        BackendStatus.Text = "SteamTools 模式";
        BackendIcon.Symbol = Wpf.Ui.Controls.SymbolRegular.Wrench24;
    }

    private void UpdateProviderAuthStatus(Services.CloudConfig config, Services.TokenStatus preCheckedStatus)
    {
        if (config.IsLocal)
        {
            ProviderStatus.Text = config.DisplayName;
            ProviderIcon.Symbol = Wpf.Ui.Controls.SymbolRegular.CloudCheckmark24;
            return;
        }

        if (config.IsFolder)
        {
            if (config.SyncPath != null)
            {
                if (Directory.Exists(config.SyncPath))
                {
                    ProviderStatus.Text = S.Format("Dashboard_FolderAccessible", config.DisplayName);
                    ProviderIcon.Symbol = Wpf.Ui.Controls.SymbolRegular.CloudCheckmark24;
                }
                else
                {
                    ProviderStatus.Text = S.Format("Dashboard_FolderNotFound", config.DisplayName);
                    ProviderIcon.Symbol = Wpf.Ui.Controls.SymbolRegular.CloudDismiss24;
                }
            }
            else
            {
                ProviderStatus.Text = S.Format("Dashboard_NoSyncFolder", config.DisplayName);
                ProviderIcon.Symbol = Wpf.Ui.Controls.SymbolRegular.CloudOff24;
            }
            return;
        }

        if (config.Provider is "webdav" or "quark")
        {
            bool isQuark = config.Provider == "quark";
            if (string.IsNullOrEmpty(config.TokenPath))
            {
                ProviderStatus.Text = S.Get(isQuark ? "CloudProvider_NoQuarkConfigPath" : "CloudProvider_NoWebDAVConfigPath");
                ProviderIcon.Symbol = Wpf.Ui.Controls.SymbolRegular.CloudOff24;
            }
            else if (File.Exists(config.TokenPath))
            {
                ProviderStatus.Text = S.Format(isQuark ? "CloudProvider_QuarkConfigFound" : "CloudProvider_WebDAVConfigFound", config.TokenPath);
                ProviderIcon.Symbol = Wpf.Ui.Controls.SymbolRegular.CloudCheckmark24;
            }
            else
            {
                ProviderStatus.Text = S.Format(isQuark ? "CloudProvider_QuarkConfigMissing" : "CloudProvider_WebDAVConfigMissing", config.TokenPath);
                ProviderIcon.Symbol = Wpf.Ui.Controls.SymbolRegular.CloudDismiss24;
            }
            return;
        }

        // OAuth providers (gdrive, onedrive)
        if (config.TokenPath != null && preCheckedStatus != null)
        {
            ProviderStatus.Text = preCheckedStatus.IsAuthenticated
                ? S.Format("Dashboard_Authenticated", config.DisplayName)
                : S.Format("Dashboard_AuthStatus", config.DisplayName, preCheckedStatus.Message);
            ProviderIcon.Symbol = preCheckedStatus.IsAuthenticated
                ? Wpf.Ui.Controls.SymbolRegular.CloudCheckmark24
                : Wpf.Ui.Controls.SymbolRegular.CloudOff24;
        }
        else
        {
            ProviderStatus.Text = S.Format("Dashboard_NoTokenPath", config.DisplayName);
            ProviderIcon.Symbol = Wpf.Ui.Controls.SymbolRegular.CloudOff24;
        }
    }

    private async void OpenLog_Click(object sender, RoutedEventArgs e)
    {
        var logPath = Services.SteamDetector.GetLogPath();
        if (logPath != null && File.Exists(logPath))
        {
            Process.Start(new ProcessStartInfo
            {
                FileName = logPath,
                UseShellExecute = true
            })?.Dispose();
        }
        else
        {
            await Services.Dialog.ShowInfoAsync(S.Get("Common_Info"),
                S.Get("Dashboard_LogNotFound"));
        }
    }

    private async void RestartSteam_Click(object sender, RoutedEventArgs e)
    {
        var steamPath = Services.SteamDetector.FindSteamPath();
        if (steamPath == null)
        {
            await Services.Dialog.ShowErrorAsync(S.Get("Common_Error"), S.Get("Dashboard_CouldNotFindSteam"));
            return;
        }

        var steamExe = Path.Combine(steamPath, "steam.exe");
        if (!File.Exists(steamExe))
        {
            await Services.Dialog.ShowErrorAsync(S.Get("Common_Error"), S.Get("Dashboard_SteamExeNotFound"));
            return;
        }

        if (!Services.SteamDetector.IsSteamRunning())
        {
            return;
        }

        var confirmed = await Services.Dialog.ConfirmAsync(S.Get("Dashboard_RestartSteam"),
            S.Get("Dashboard_RestartSteamPrompt"));

        if (!confirmed) return;

        var button = (Wpf.Ui.Controls.Button)sender;
        button.IsEnabled = false;
        var originalContent = button.Content;
        button.Content = S.Get("Dashboard_ShuttingDownSteam");

        try
        {
            // Ask Steam to shut down correctly
            Process.Start(new ProcessStartInfo
            {
                FileName = steamExe,
                Arguments = "-shutdown",
                UseShellExecute = true
            })?.Dispose();

            // Poll until Steam processes exit (up to 15 seconds)
            bool exited = await Task.Run(async () =>
            {
                for (int i = 0; i < 30; i++) // 30 x 500ms = 15s
                {
                    await Task.Delay(500);
                    var procs = Process.GetProcessesByName("steam");
                    bool any = procs.Length > 0;
                    foreach (var p in procs) p.Dispose();
                    if (!any) return true;
                }
                return false;
            });

            if (!exited)
            {
                // Graceful shutdown didn't work -- offer force-kill
                var forceKill = await Services.Dialog.ConfirmAsync(S.Get("Dashboard_SteamStillRunning"),
                    S.Get("Dashboard_SteamStillRunningPrompt"));

                if (forceKill)
                {
                    button.Content = S.Get("Dashboard_ForceKilling");
                    await Task.Run(() =>
                    {
                        foreach (var proc in Process.GetProcessesByName("steam"))
                        {
                            try { proc.Kill(); }
                            catch { /* already exited */ }
                            finally { proc.Dispose(); }
                        }
                    });

                    // Brief wait for process table cleanup
                    await Task.Delay(1000);
                }
                else
                {
                    return; // User cancelled
                }
            }

            // Start Steam
            button.Content = S.Get("Dashboard_StartingSteam");
            Process.Start(new ProcessStartInfo
            {
                FileName = steamExe,
                UseShellExecute = true
            })?.Dispose();
        }
        catch (Exception ex)
        {
            await Services.Dialog.ShowErrorAsync(S.Get("Common_Error"), S.Format("Dashboard_FailedRestartSteam", ex.Message));
        }
        finally
        {
            button.Content = originalContent;
            button.IsEnabled = true;
        }
    }

}
