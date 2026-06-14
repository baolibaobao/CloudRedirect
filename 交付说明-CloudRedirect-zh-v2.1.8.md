# CloudRedirect zh-v2.1.8 交付说明

更新时间：2026-06-14

## 本阶段完成

- 简体中文资源已补齐，`Strings.resx`、`Strings.zh-CN.resx`、`Strings.es.resx`、`Strings.pt-BR.resx` 的资源 key 已对齐。
- 云提供商页面已支持 WebDAV 与夸克网盘（内置 OpenList）配置。
- 夸克 Cookie 输入框已改为多行长文本框，支持超长 Cookie 粘贴。
- 夸克配置保存已避免空输入框覆盖已有 Cookie；Dashboard 与云提供商页不再把夸克配置当 OAuth token 处理。
- VS2026 Release 构建已通过，构建时会把当前 `build-vs2026/Release` 的 native DLL/CLI 嵌入 UI 资源。
- Steam 端到端上传已验证成功：Steam → `cloud_redirect.dll` → OpenList → 夸克网盘。

## 构建命令

首次生成 VS2026 工程：

```powershell
& "F:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" -B build-vs2026 -G "Visual Studio 18 2026" -A x64
```

Release 构建：

```powershell
& "F:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" --build build-vs2026 --config Release
```

发布产物：

```text
ui/bin/publish/CloudRedirect.exe
build-vs2026/Release/cloud_redirect.dll
build-vs2026/Release/cloud_redirect_cli.exe
```

## 打包命令

```powershell
New-Item -ItemType Directory -Force out | Out-Null
Compress-Archive -Path ui\bin\publish\* -DestinationPath out\CloudRedirect-zh-v2.1.8-win-x64.zip -Force
```

## 夸克使用流程

1. 运行 `ui/bin/publish/CloudRedirect.exe`。
2. 在“安装设置”页部署 DLL 并应用补丁。
3. 在“云提供商”页选择“夸克网盘（OpenList）”。
4. 粘贴夸克 Cookie，默认远端根目录保持 `/Quark/CloudRedirect`。
5. 点击“测试连接”，确认成功。
6. 完全退出并重启 Steam。
7. 启动游戏并触发 Steam 云同步。
8. 在夸克网盘检查 `CloudRedirect/<accountId>/<appId>/...`。

`CloudRedirect.exe` 是配置/部署控制面板，不需要常驻；日常同步由 Steam 进程加载的 `cloud_redirect.dll` 完成。

## 验证点

- `cloud_redirect.log` 中应出现 `Cloud provider 'Quark OpenList' initialized`。
- 成功上传时应出现 `PromoteStagedBatch ... promoted` 与 `CompleteBatch app=<appId> CN=<n>`。
- 夸克目录会包含 `state.cloudredirect`、`cn.cloudredirect`、`blobs/` 等同步状态与内容文件。
- `.healthcheck` 目录为空是正常现象，健康检查文件会上传后删除。

## 已知待补

- 通用 WebDAV provider 仍需要单独端到端验收：上传、下载、列表、删除、XML 解析失败关闭。
- Apps 页面远端列表/删除/孤立 blob 清理建议继续对夸克 provider 做一次完整验收。
- `ui/Services/Patching/Patcher.cs` 的补丁流水日志仍有英文调试文本；不影响中文主 UI，但后续可继续资源化。
- 发布前应确认 OpenList 的再分发许可与最终打包来源。
