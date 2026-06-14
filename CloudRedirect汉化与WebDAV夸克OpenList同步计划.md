# CloudRedirect 汉化与 WebDAV/夸克 OpenList 同步计划（Windows only）

## Context

目标是将 CloudRedirect Windows 版汉化，并添加 WebDAV 同步功能。WebDAV 同步需要重点参考 `F:\ruanjian\yuncundang` 中 GameSaveCloudQt 已验证可用的方法，尤其是“内置 OpenList 网关用于夸克网盘 WebDAV 同步”。本次不考虑 Linux；所有设计以 Windows WPF UI、Windows Steam 注入 DLL、Windows CLI 为准。

CloudRedirect 当前已有适合扩展的结构：

- 原生 C++ 核心通过 `src/common/cloud_provider.h` 的 `ICloudProvider` 抽象云端读写。
- provider 工厂在 `src/common/cloud_storage.cpp`。
- CLI 在 `src/common/cli.cpp`，WPF UI 已通过 `CliUiCloudProvider` 调用 CLI 做远端列表/删除。
- Windows UI 配置在 `%AppData%\CloudRedirect\config.json`，通过 `ui/Services/ConfigHelper.cs` 原子写入并保留未知 key。
- UI 本地化已有资源机制：`ui/Resources/Strings.resx`、`Strings.es.resx`、`Strings.pt-BR.resx`，XAML 用 `{res:Loc Key}`，C# 用 `S.Get(...)`/`S.Format(...)`。

推荐把 WebDAV 与夸克 OpenList 都实现为原生 C++ provider，而不是只做在 UI 层。这样 Steam 启动时 DLL 能直接初始化并同步，CLI 和 WPF 页面也能复用同一套实现。

## 1. Windows WPF 汉化

修改文件：

- `ui/Resources/Strings.zh-CN.resx`：新增简体中文资源文件，镜像 `Strings.resx` 所有 key。
- `ui/Resources/Strings.resx`
- `ui/Resources/Strings.es.resx`
- `ui/Resources/Strings.pt-BR.resx`：补齐新增 WebDAV/夸克相关 key，保持资源 key 对齐。
- `ui/Pages/SettingsPage.xaml.cs`：在 `LanguageOptions` 中添加 `zh-CN`，例如 `Settings_LanguageChineseSimplified`。

沿用现有模式：

- XAML 继续用 `{res:Loc Key}`。
- C# 继续用 `S.Get(...)` / `S.Format(...)`。
- 不改 `S.cs`、`LocExtension.cs` 架构。

## 2. 通用 WebDAV provider

新增/修改：

- `src/providers/webdav_provider.h`
- `src/providers/webdav_provider.cpp`
- `src/common/cloud_storage.cpp`：`CreateCloudProvider` 注册 `webdav`。
- `src/common/cli.cpp`：`GetTokenPath("webdav")` 默认 `%AppData%\CloudRedirect\webdav.json`，仍优先使用 `config.json` 中的 `token_path`。
- `CMakeLists.txt`：加入 WebDAV provider 源文件。

配置形态：

```json
// %AppData%/CloudRedirect/config.json
{
  "provider": "webdav",
  "token_path": "C:\\Users\\<user>\\AppData\\Roaming\\CloudRedirect\\webdav.json"
}
```

```json
// webdav.json
{
  "server_url": "https://example.com/dav",
  "username": "user",
  "password": "password",
  "remote_root_path": "/CloudRedirect"
}
```

实现要点：

- 实现 `ICloudProvider` 的全部方法：`Init`、`Shutdown`、`IsAuthenticated`、`Upload`、`Download`、`Remove`、`CheckExists`、`List`、`ListChecked`。
- CloudRedirect provider 内部路径继续使用现有相对路径：`{accountId}/{appId}/blobs/{filename}`。
- WebDAV 远端路径拼接为：`remote_root_path + "/" + relativePath`。
- 路径安全：拒绝 `..`、反斜杠穿越、绝对 URL、空路径段等。
- WebDAV 方法：
  - `PROPFIND Depth: 0`：测试连接与 `CheckExists`。
  - `MKCOL`：递归创建远端目录。
  - `PUT`：上传，200/201/204 为成功。
  - `GET`：下载，200 为成功，404 为不存在。
  - `DELETE`：删除，200/202/204/404 可视为成功。
  - `PROPFIND Depth: infinity`：`List/ListChecked`。
- `ListChecked` 必须失败关闭：网络错误、认证错误、XML 解析不完整、列表不完整时返回 false 或 `outComplete=false`，避免现有清理逻辑做破坏性误删。
- 可复用 `src/common/cloud_provider_base.h` 中的 `IHttpTransport`/`CreateHttpTransport`；但注意 Windows `RequestUrl` 目前阻止非 HTTPS。通用 WebDAV 可以优先要求 HTTPS，夸克本地 OpenList 需要另行处理 loopback HTTP。

## 3. 夸克网盘：内置 OpenList 网关 provider（必须按 GameSaveCloudQt 已验证方案实现）

这是本次 WebDAV 功能的重点。不能只写一个普通 WebDAV 配置让用户自己跑 OpenList；需要 CloudRedirect 内置并管理 OpenList 网关，Cookie 持久化和 API 使用方式保持与 GameSaveCloudQt 一致，因为该方案已经实测可用。

新增/修改：

- `src/providers/quark_openlist_provider.h`
- `src/providers/quark_openlist_provider.cpp`
- 可按需要拆分 helper：`openlist_gateway.*`、`webdav_xml.*`、`webdav_provider.*` 共享工具。
- `src/common/cloud_storage.cpp`：注册 provider 名称 `quark`。
- `src/common/cli.cpp`：`GetTokenPath("quark")` 默认 `%AppData%\CloudRedirect\quark_openlist.json`。
- `CMakeLists.txt`：加入夸克/OpenList 源文件。

### 3.1 持久化策略：与 GameSaveCloudQt 保持一致

CloudRedirect 顶层配置：

```json
// %AppData%/CloudRedirect/config.json
{
  "provider": "quark",
  "token_path": "C:\\Users\\<user>\\AppData\\Roaming\\CloudRedirect\\quark_openlist.json"
}
```

Quark/OpenList 配置文件：

```json
// %AppData%/CloudRedirect/quark_openlist.json
{
  "cookie": "<quark-cookie>",
  "openlist_exe": "C:\\Users\\<user>\\AppData\\Roaming\\CloudRedirect\\openlist\\openlist.exe",
  "data_dir": "C:\\Users\\<user>\\AppData\\Roaming\\CloudRedirect\\openlist-data",
  "admin_password": "<generated-once>",
  "base_url": "http://127.0.0.1:5244",
  "mount_path": "/Quark",
  "remote_root_path": "/Quark/CloudRedirect",
  "force_storage_update": false
}
```

必须遵守：

- 持久化用户输入的 Quark Cookie。
- 持久化生成的 OpenList 管理员密码，后续启动复用。
- 不持久化 OpenList `/api/auth/login` 返回的 API token；token 只保存在内存里。
- 不把 Cookie、管理员密码、API token、Authorization header、完整 storage payload、`addition` JSON 打到日志。

这对应 GameSaveCloudQt：

- Cookie 存在 `quark/cookie`。
- OpenList 管理员密码存在 `quark/openListAdminPassword`。
- OpenList API token 是 `m_apiToken`，只在内存中。

CloudRedirect 不使用 Qt `QSettings`，但持久化语义要一致；用 `quark_openlist.json` 承载同样的信息。

### 3.2 OpenList 启动流程

`QuarkOpenListProvider::Init(configPath)` 执行：

1. 读取 `quark_openlist.json`，修剪并校验 Cookie。
2. 查找或释放内置 `openlist.exe`。
3. 读取 `admin_password`；不存在则生成一次并写回配置。
4. 确保 `data_dir` 存在。
5. 执行：
   ```text
   openlist.exe admin set <admin_password> --data <data_dir>
   ```
6. 启动：
   ```text
   openlist.exe server --data <data_dir> --log-std
   ```
7. 每约 800ms 轮询登录，最多约 30 次，参考 GameSaveCloudQt。
8. 登录 OpenList：
   ```text
   POST http://127.0.0.1:5244/api/auth/login
   ```
   body：
   ```json
   { "username": "admin", "password": "<admin_password>" }
   ```
9. 从响应 `data.token` 取 API token，仅放内存。
10. 配置/更新 Quark storage。
11. 确保 `/Quark/CloudRedirect` 存在。
12. 做真实健康检查：写入小 JSON/文本文件、读回比对、删除。

默认值：

- `base_url`: `http://127.0.0.1:5244`
- `mount_path`: `/Quark`
- `remote_root_path`: `/Quark/CloudRedirect`
- `data_dir`: `%AppData%\CloudRedirect\openlist-data`
- `openlist_exe`: `%AppData%\CloudRedirect\openlist\openlist.exe`，或应用旁 `engines\openlist\openlist.exe`。

### 3.3 OpenList storage API：与 GameSaveCloudQt 一致

使用的 OpenList API：

```text
GET  /api/admin/storage/list?page=1&per_page=200
POST /api/admin/storage/create
POST /api/admin/storage/update
```

查找已有 storage：

- `driver == "Quark"`
- `mount_path == "/Quark"`

创建/更新 payload 保留 GameSaveCloudQt 的关键字段：

```json
{
  "mount_path": "/Quark",
  "order": 0,
  "remark": "CloudRedirect Quark",
  "cache_expiration": 300,
  "web_proxy": true,
  "webdav_policy": "native_proxy",
  "down_proxy_url": "",
  "extract_folder": "front",
  "enable_sign": true,
  "driver": "Quark",
  "order_by": "name",
  "order_direction": "asc",
  "addition": "{\"cookie\":\"...\",\"root_folder_id\":\"0\",\"order_by\":\"name\",\"order_direction\":\"asc\",\"use_transcoding_address\":false}"
}
```

关键点：Cookie 放在 `addition` JSON 字符串中；不要日志输出该 payload。

### 3.4 OpenList 文件 API：夸克操作优先用 API，不依赖普通 WebDAV

按 GameSaveCloudQt 的实测方式，夸克 provider 的核心文件操作优先使用 OpenList API：

```text
POST /api/fs/mkdir
POST /api/fs/list
POST /api/fs/get
PUT  /api/fs/put
POST /api/fs/remove
```

映射到 `ICloudProvider`：

- `Upload(path, data, len)`：
  1. 将 CloudRedirect 相对路径映射到 `/Quark/CloudRedirect/<path>`。
  2. `POST /api/fs/mkdir` 确保父目录。
  3. 如覆盖需要，必要时先 `/api/fs/remove`。
  4. `PUT /api/fs/put`，header：
     - `Authorization: <OpenList token>`
     - `File-Path: <percent-encoded remote path>`
     - `Content-Type: application/octet-stream`
- `Download(path, outData)`：
  1. `POST /api/fs/get`，body 包含 `path`、`password: ""`、`refresh: true`。
  2. 优先使用本地 OpenList proxy/signed URL 下载。
  3. fallback 到本地 WebDAV：`http://127.0.0.1:5244/dav/...`。
  4. 最后才考虑 raw upstream URL。
- `CheckExists(path)`：`POST /api/fs/get`，必须 `refresh: true`。
- `List(prefix)` / `ListChecked(prefix)`：`POST /api/fs/list`，必须 `refresh: true`；需要递归列子目录以满足 CloudRedirect 现有 provider 语义。
- `Remove(path)`：`POST /api/fs/remove`；文件不存在视为成功。

必须复制的关键经验：

- `list/get` 使用 `refresh: true`，因为夸克/OpenList 缓存可能陈旧，用户在夸克网页删文件后必须能反映。
- 上传/下载/删除串行化，使用 mutex 或内部队列，避免 OpenList/夸克竞争。
- 下载候选顺序优先本地代理和本地 WebDAV，避免直接依赖夸克临时 URL。
- 不复用可能导致 `412 Precondition Failed` 的 conditional/range headers。

### 3.5 本地 HTTP 支持

当前 `src/platform/win/http_transport_win.cpp` 的 `RequestUrl` 会阻止非 HTTPS。夸克 OpenList 必须访问：

```text
http://127.0.0.1:5244
```

计划：

- 不全局放开 HTTP。
- 只允许 loopback HTTP：`127.0.0.1`、`localhost`、`[::1]`。
- 或者在 `QuarkOpenListProvider` 内实现专用 WinHTTP loopback 请求函数。
- 保持外部普通 WebDAV 推荐 HTTPS。

## 4. Windows WPF 配置 UI

修改：

- `ui/Pages/CloudProviderPage.xaml`
- `ui/Pages/CloudProviderPage.xaml.cs`
- `ui/Services/UiCloudProviderFactory.cs`
- `ui/Services/SteamDetector.cs`
- `ui/Resources/*.resx`

新增 provider 选项：

- `webdav`：WebDAV
- `quark`：夸克网盘（内置 OpenList）

UI 方案：

- 保留 `TokenPathBox` 作为高级配置文件路径。
- 对 `webdav` 显示：Server URL、Username、Password、Remote root path。
- 对 `quark` 显示：Quark Cookie、OpenList 路径/状态、Data dir、Remote root path、强制更新 storage 选项。
- Quark Cookie 输入应类似 GameSaveCloudQt：可隐藏/显示，保存后显示“已保存 Cookie/可重新连接”。
- `SaveConfigSilent()`：
  - 顶层 `config.json` 写 `provider` + `token_path`。
  - provider 详情写 `webdav.json` 或 `quark_openlist.json`。
  - 继续用 `ConfigHelper.SaveConfig(...)` 保留未知 key。
- `UpdateAuthStatus()`：
  - `webdav`/`quark` 调用 `cloud_redirect_cli auth-status <provider>`。
  - 夸克状态检查可能启动 OpenList，UI 文案明确提示。
- `UiCloudProviderFactory.TryResolve(...)` 新增：
  - `"webdav" => new CliUiCloudProvider("webdav", log)`
  - `"quark" => new CliUiCloudProvider("quark", log)`

## 5. 内置 OpenList 打包/释放

为满足“内置 OpenList 网关”：

- 将 `openlist.exe` 放入 UI payload，例如 `ui/Resources/payloads/openlist/openlist.exe` 或等价目录。
- 复用现有嵌入资源释放思路（如 `ui/Services/EmbeddedDll.cs`、`ui/Services/EmbeddedCli.cs`），保存夸克配置或首次连接时释放到：
  ```text
  %AppData%\CloudRedirect\openlist\openlist.exe
  ```
- provider 的 `openlist_exe` 默认指向该路径。
- 实施前确认 OpenList 的许可证/再分发要求。

如果第一轮实现需要降低风险，可以先支持手动指定 `openlist.exe`，但最终必须实现内置释放。

## 6. 不做 Linux

本轮明确无视 Linux：

- 不改 `ui-linux`。
- 不为 Linux 做 OpenList 路径、打包、Qt 配置页。
- 原生代码如果顺手保持跨平台可以保留，但验收以 Windows 为准。

## Verification

### 构建验证

1. 原生构建：
   ```text
   cmake -B build -G "Visual Studio 17 2022" -A x64
   cmake --build build --config Release
   ```
2. WPF 构建：
   ```text
   dotnet build ui/CloudRedirect.csproj
   ```

### 汉化验证

1. 启动 WPF UI。
2. Settings 选择“简体中文”。
3. 重启应用，确认导航、设置页、云提供商页、提示对话框中文显示。
4. 切回 System/English，确认已有语言正常。

### 通用 WebDAV 验证

1. 配置 `provider=webdav` 与测试 WebDAV endpoint。
2. 运行：`cloud_redirect_cli auth-status webdav`。
3. 验证上传、下载、列表、删除：
   - 远端目录自动创建。
   - 下载内容与上传内容一致。
   - 删除不存在文件不报致命错误。
   - 网络/XML 解析错误时 `ListChecked` 失败关闭。

### 夸克 OpenList 验证

1. 准备 Quark Cookie，清空或新建 OpenList data 目录。
2. 运行：`cloud_redirect_cli auth-status quark`。
3. 验证：
   - `openlist.exe admin set <password> --data <data_dir>` 成功。
   - OpenList server 正常启动。
   - `/api/auth/login` 成功，API token 不写入磁盘。
   - `/api/admin/storage/list/create/update` 正确配置 `/Quark` Quark storage。
   - `/Quark/CloudRedirect` 被创建。
   - 健康检查能写入、读回、删除测试文件。
4. 重启后再次运行，确认复用同一 Cookie 和 admin password。
5. 手动在夸克网页删除文件，确认 `refresh:true` 的 `list/get` 能反映删除。

### CloudRedirect 端到端验证

1. WPF 选择“夸克网盘（内置 OpenList）”，保存 Cookie。
2. 启动 Steam + CloudRedirect。
3. 触发游戏云存档写入/读取。
4. 确认日志中 provider 初始化成功且没有泄露 Cookie/密码/token。
5. 远端路径应为：
   ```text
   /Quark/CloudRedirect/<accountId>/<appId>/...
   ```
6. Apps 页远端列表/删除通过 `CliUiCloudProvider("quark")` 正常工作。

## Implementation order

1. 添加 `Strings.zh-CN.resx` 与语言选项。
2. 实现并接入通用 `WebDavProvider`。
3. 实现 `QuarkOpenListProvider`，严格参考 GameSaveCloudQt 的 Cookie 持久化、OpenList 启动、storage 配置、API 上传/下载/列表/删除。
4. 扩展 WPF Cloud Provider 页面保存 WebDAV/夸克配置。
5. 加入内置 OpenList 打包/释放。
6. 执行构建、CLI、WPF 和 Steam 端到端验证。

## Main risks and mitigations

- **loopback HTTP**：只允许 `127.0.0.1`/`localhost` HTTP，不能放开任意 HTTP。
- **WebDAV XML 解析**：`ListChecked` 必须失败关闭，避免误删。
- **夸克缓存陈旧**：`/api/fs/list` 与 `/api/fs/get` 使用 `refresh:true`。
- **secret 泄露**：日志中统一脱敏，禁止输出 Cookie、密码、token、payload。
- **OpenList 进程所有权**：只关闭 CloudRedirect 自己启动的 OpenList 进程。
- **与 GameSaveCloudQt 偏离**：夸克路径必须以 GameSaveCloudQt 实测实现为准，尤其是 `/api/admin/storage/*`、`/api/fs/*`、Cookie/admin password 持久化、API token 内存保存。

## 2026-06-14 ????

### ???

- ???????? `Strings.zh-CN.resx` ???????????????? key ???
- ?? WebDAV provider ??? OpenList provider ??? CMake?CLI?WPF UI ? provider ???
- ?? Cookie?OpenList ???data dir??????????? storage ???? WPF ????????
- ?? Cookie ???????????????? Cookie ????
- UI ?????/WebDAV ???? OAuth token ????? `quark_openlist.json` ? DPAPI ????
- VS2026 Release ??????UI publish ????? `build-vs2026/Release` ? DLL/CLI?
- ???????????Steam ???? Quark provider ??????batch promoted?state published??????? `CloudRedirect/<accountId>/<appId>/...`?

### ?????

- ?? WebDAV provider ???? WebDAV endpoint ??????????????????
- ???????Apps ???????? blob ??????????????????
- `ui/Services/Patching/Patcher.cs` ????????????????? UI ????????????????????
- ??????? OpenList ????????????????
