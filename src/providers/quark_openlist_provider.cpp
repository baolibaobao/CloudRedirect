#include "quark_openlist_provider.h"

#include "file_util.h"
#include "http_util.h"
#include "log.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <random>
#include <thread>
#include <utility>

using HttpUtil::HttpResp;
using HttpUtil::UrlEncode;

namespace {
std::string Trim(std::string s) {
    auto ns = [](unsigned char c) { return !std::isspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), ns));
    s.erase(std::find_if(s.rbegin(), s.rend(), ns).base(), s.end());
    return s;
}
std::string StripTrailingSlash(std::string s) { while (s.size() > 1 && s.back() == '/') s.pop_back(); return s; }
std::string NormalizeRemote(std::string s, const char* fallback) {
    s = Trim(s); std::replace(s.begin(), s.end(), '\\', '/');
    if (s.empty()) s = fallback;
    if (s.front() != '/') s.insert(s.begin(), '/');
    return StripTrailingSlash(s);
}
Json::Value Obj(std::initializer_list<std::pair<std::string, Json::Value>> fields) {
    auto v = Json::Object();
    for (auto& p : fields) v.objVal[p.first] = p.second;
    return v;
}
bool HasOpenListSuccess(const Json::Value& root) {
    return root.type == Json::Type::Object && root["code"].integer() == 200;
}

bool ContainsInsensitive(const std::string& text, const std::string& needle) {
    auto lower = [](std::string s) {
        std::transform(s.begin(), s.end(), s.begin(),
                       [](unsigned char c) { return (char)std::tolower(c); });
        return s;
    };
    return lower(text).find(lower(needle)) != std::string::npos;
}

bool IsOpenListMissing(const Json::Value& root) {
    if (root["code"].integer() == 404) return true;
    std::string msg = root["message"].str();
    return ContainsInsensitive(msg, "not exist") ||
           ContainsInsensitive(msg, "not found") ||
           ContainsInsensitive(msg, "no such");
}
}

void QuarkOpenListProvider::ApplyDefaults() {
#ifdef _WIN32
    char* appData = nullptr;
    size_t len = 0;
    _dupenv_s(&appData, &len, "APPDATA");
    std::string base = appData ? std::string(appData) + "\\CloudRedirect" : ".";
    if (appData) free(appData);
    if (m_config.openListExe.empty()) m_config.openListExe = base + "\\openlist\\openlist.exe";
    if (m_config.dataDir.empty()) m_config.dataDir = base + "\\openlist-data";
#endif
    if (m_config.baseUrl.empty()) m_config.baseUrl = "http://127.0.0.1:5244";
    m_config.baseUrl = StripTrailingSlash(m_config.baseUrl);
    m_config.mountPath = NormalizeRemote(m_config.mountPath, "/Quark");
    m_config.remoteRootPath = NormalizeRemote(m_config.remoteRootPath, "/Quark/CloudRedirect");
}

bool QuarkOpenListProvider::LoadConfig(const std::string& configPath) {
    m_configPath = configPath;
    std::ifstream f(configPath, std::ios::binary);
    if (!f) { LOG("[Quark] Config not found: %s", configPath.c_str()); return false; }
    std::string s((std::istreambuf_iterator<char>(f)), {});
    auto j = Json::Parse(s);
    if (j.type != Json::Type::Object) return false;
    m_config.cookie = Trim(j["cookie"].str());
    m_config.openListExe = j["openlist_exe"].str();
    m_config.dataDir = j["data_dir"].str();
    m_config.adminPassword = j["admin_password"].str();
    m_config.baseUrl = j["base_url"].str();
    m_config.mountPath = j["mount_path"].str();
    m_config.remoteRootPath = j["remote_root_path"].str();
    m_config.forceStorageUpdate = j["force_storage_update"].boolean();
    ApplyDefaults();
    if (m_config.cookie.empty()) { LOG("[Quark] Cookie missing"); return false; }
    if (m_config.adminPassword.empty()) { m_config.adminPassword = GenerateAdminPassword(); SaveConfig(); }
    return true;
}

bool QuarkOpenListProvider::SaveConfig() {
    auto j = Obj({
        {"cookie", Json::String(m_config.cookie)},
        {"openlist_exe", Json::String(m_config.openListExe)},
        {"data_dir", Json::String(m_config.dataDir)},
        {"admin_password", Json::String(m_config.adminPassword)},
        {"base_url", Json::String(m_config.baseUrl)},
        {"mount_path", Json::String(m_config.mountPath)},
        {"remote_root_path", Json::String(m_config.remoteRootPath)},
        {"force_storage_update", Json::Value{Json::Type::Bool, m_config.forceStorageUpdate}}
    });
    std::error_code ec;
    std::filesystem::create_directories(FileUtil::Utf8ToPath(m_configPath).parent_path(), ec);
    return FileUtil::AtomicWriteText(m_configPath, Json::Stringify(j));
}

std::string QuarkOpenListProvider::GenerateAdminPassword() const {
    static constexpr char chars[] = "ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz23456789";
    std::random_device rd; std::mt19937 gen(rd()); std::uniform_int_distribution<> dis(0, (int)sizeof(chars) - 2);
    std::string out; for (int i = 0; i < 28; ++i) out.push_back(chars[dis(gen)]); return out;
}

bool QuarkOpenListProvider::EnsureDataDir() {
    std::error_code ec;
    std::filesystem::create_directories(FileUtil::Utf8ToPath(m_config.dataDir), ec);
    if (ec) LOG("[Quark] Failed creating data dir: %s", ec.message().c_str());
    return !ec;
}

bool QuarkOpenListProvider::SetAdminPassword() {
#ifdef _WIN32
    if (!std::filesystem::exists(FileUtil::Utf8ToPath(m_config.openListExe))) { LOG("[Quark] openlist.exe not found"); return false; }
    std::wstring cmd = L"\"" + HttpUtil::Widen(m_config.openListExe) + L"\" admin set \"" +
        HttpUtil::Widen(m_config.adminPassword) + L"\" --data \"" + HttpUtil::Widen(m_config.dataDir) + L"\"";
    STARTUPINFOW si{}; si.cb = sizeof(si); PROCESS_INFORMATION pi{};
    std::vector<wchar_t> buf(cmd.begin(), cmd.end()); buf.push_back(0);
    if (!CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) return false;
    WaitForSingleObject(pi.hProcess, 30000);
    DWORD code = 1; GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    return code == 0;
#else
    return false;
#endif
}

bool QuarkOpenListProvider::StartOpenList() {
#ifdef _WIN32
    if (m_ownsProcess) return true;
    std::wstring cmd = L"\"" + HttpUtil::Widen(m_config.openListExe) + L"\" server --data \"" +
        HttpUtil::Widen(m_config.dataDir) + L"\" --log-std";
    STARTUPINFOW si{}; si.cb = sizeof(si); std::vector<wchar_t> buf(cmd.begin(), cmd.end()); buf.push_back(0);
    if (!CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &m_processInfo)) {
        LOG("[Quark] Failed to start OpenList: %lu", GetLastError()); return false;
    }
    m_ownsProcess = true;
    return true;
#else
    return false;
#endif
}

std::vector<std::string> QuarkOpenListProvider::ApiHeaders(const std::vector<std::string>& extra) const {
    auto h = extra;
    if (!m_apiToken.empty()) h.push_back("Authorization: " + m_apiToken);
    h.push_back("User-Agent: CloudRedirect/1.0");
    return h;
}
HttpResp QuarkOpenListProvider::RequestUrl(const char* method, const std::string& url, const std::string& body, const std::vector<std::string>& headers) {
    if (!m_transport || !m_transport->IsReady()) return {};
    return m_transport->RequestUrl(method, url, body, ApiHeaders(headers));
}
HttpResp QuarkOpenListProvider::ApiRequest(const char* method, const std::string& path, const Json::Value& body, const std::vector<std::string>& extraHeaders) {
    return RequestUrl(method, m_config.baseUrl + path, Json::Stringify(body), extraHeaders.empty() ? std::vector<std::string>{"Content-Type: application/json"} : extraHeaders);
}

bool QuarkOpenListProvider::LoginWithRetry() {
    auto body = Obj({{"username", Json::String("admin")}, {"password", Json::String(m_config.adminPassword)}});
    for (int i = 0; i < 30; ++i) {
        auto r = RequestUrl("POST", m_config.baseUrl + "/api/auth/login", Json::Stringify(body), {"Content-Type: application/json"});
        if (r.status == 200) {
            auto j = Json::Parse(r.body);
            if (HasOpenListSuccess(j) && !j["data"]["token"].str().empty()) {
                m_apiToken = j["data"]["token"].str();
                return true;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(800));
    }
    LOG("[Quark] OpenList login timed out");
    return false;
}

bool QuarkOpenListProvider::ConfigureStorage() {
    auto list = RequestUrl("GET", m_config.baseUrl + "/api/admin/storage/list?page=1&per_page=200");
    int existingId = 0;
    bool healthy = false;
    if (list.status == 200) {
        auto j = Json::Parse(list.body);
        auto& content = j["data"]["content"];
        for (size_t i = 0; i < content.size(); ++i) {
            auto& it = content[i];
            if (it["driver"].str() == "Quark" && it["mount_path"].str() == m_config.mountPath) {
                existingId = (int)it["id"].integer();
                healthy = it["status"].str() == "work";
                break;
            }
        }
    }
    if (existingId && healthy && !m_config.forceStorageUpdate) return true;

    auto addition = Obj({{"cookie", Json::String(m_config.cookie)}, {"root_folder_id", Json::String("0")},
                         {"order_by", Json::String("name")}, {"order_direction", Json::String("asc")},
                         {"use_transcoding_address", Json::Value{Json::Type::Bool, false}}});
    auto payload = Obj({
        {"mount_path", Json::String(m_config.mountPath)}, {"order", Json::Number(0)},
        {"remark", Json::String("CloudRedirect Quark")}, {"cache_expiration", Json::Number(300)},
        {"web_proxy", Json::Value{Json::Type::Bool, true}}, {"webdav_policy", Json::String("native_proxy")},
        {"down_proxy_url", Json::String("")}, {"extract_folder", Json::String("front")},
        {"enable_sign", Json::Value{Json::Type::Bool, true}}, {"driver", Json::String("Quark")},
        {"order_by", Json::String("name")}, {"order_direction", Json::String("asc")},
        {"addition", Json::String(Json::Stringify(addition))}
    });
    if (existingId) payload.objVal["id"] = Json::Number(existingId);
    auto r = ApiRequest("POST", existingId ? "/api/admin/storage/update" : "/api/admin/storage/create", payload);
    if (r.status != 200 || !HasOpenListSuccess(Json::Parse(r.body))) { LOG("[Quark] Storage configure failed HTTP %d", r.status); return false; }
    return true;
}

bool QuarkOpenListProvider::ValidateRelativePath(const std::string& relPath) const {
    if (relPath.empty() || relPath[0] == '/' || relPath.find('\\') != std::string::npos || relPath.find("://") != std::string::npos) return false;
    size_t p = 0; while (p <= relPath.size()) { size_t s = relPath.find('/', p); auto seg = s == std::string::npos ? relPath.substr(p) : relPath.substr(p, s - p); if (seg.empty() || seg == "." || seg == "..") return false; if (s == std::string::npos) break; p = s + 1; }
    return true;
}
std::string QuarkOpenListProvider::RemotePathFor(const std::string& relPath) const { return ValidateRelativePath(relPath) ? m_config.remoteRootPath + "/" + relPath : std::string(); }
std::string QuarkOpenListProvider::ParentPath(const std::string& p) const { auto s = p.find_last_of('/'); return s == std::string::npos ? std::string() : p.substr(0, s); }
std::string QuarkOpenListProvider::LeafName(const std::string& p) const { auto s = p.find_last_of('/'); return s == std::string::npos ? p : p.substr(s + 1); }

bool QuarkOpenListProvider::Mkdir(const std::string& remotePath) {
    auto r = ApiRequest("POST", "/api/fs/mkdir", Obj({{"path", Json::String(remotePath)}}));
    if (r.status != 200) return false;
    auto j = Json::Parse(r.body);
    if (HasOpenListSuccess(j)) return true;
    auto msg = j["message"].str();
    return msg.find("exist") != std::string::npos || msg.find("存在") != std::string::npos;
}
bool QuarkOpenListProvider::EnsureRemoteRoot() { return Mkdir(m_config.remoteRootPath); }
bool QuarkOpenListProvider::RemoveRemote(const std::string& remotePath) {
    auto r = ApiRequest("POST", "/api/fs/remove", Obj({{"dir", Json::String(ParentPath(remotePath))}, {"names", [&]{ auto a=Json::Array(); a.arrVal.push_back(Json::String(LeafName(remotePath))); return a; }()}}));
    if (r.status == 404) return true;
    if (r.status != 200) return false;
    auto j = Json::Parse(r.body);
    return HasOpenListSuccess(j) || IsOpenListMissing(j);
}

bool QuarkOpenListProvider::Upload(const std::string& path, const uint8_t* data, size_t len) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto remote = RemotePathFor(path); if (remote.empty()) return false;
    if (!Mkdir(ParentPath(remote))) return false;
    std::string body(reinterpret_cast<const char*>(data), len);
    auto r = RequestUrl("PUT", m_config.baseUrl + "/api/fs/put", body,
                        {"File-Path: " + UrlEncode(remote, false), "Content-Type: application/octet-stream"});
    if (r.status == 200 && HasOpenListSuccess(Json::Parse(r.body))) return true;
    RemoveRemote(remote);
    r = RequestUrl("PUT", m_config.baseUrl + "/api/fs/put", body,
                   {"File-Path: " + UrlEncode(remote, false), "Content-Type: application/octet-stream"});
    return r.status == 200 && HasOpenListSuccess(Json::Parse(r.body));
}

bool QuarkOpenListProvider::BuildDownloadCandidates(const Json::Value& data, const std::string& remotePath,
                                                    std::vector<std::pair<std::string, std::vector<std::string>>>& outCandidates) const {
    outCandidates.clear();
    std::vector<std::string> safeHeaders = {"Accept: application/octet-stream,*/*", "Cache-Control: no-cache", "Pragma: no-cache"};

    auto sign = data["sign"].str();
    std::string path = data["path"].str();
    if (path.empty()) path = remotePath;

    if (!path.empty()) {
        std::string encoded = UrlEncode(path, true);
        std::string proxy = m_config.baseUrl + "/p" + encoded + "?d";
        if (!sign.empty()) proxy += "&sign=" + UrlEncode(sign);
        outCandidates.emplace_back(proxy, safeHeaders);

        std::string webdavPath = path;
        if (!webdavPath.empty() && webdavPath.front() == '/') webdavPath.erase(webdavPath.begin());
        outCandidates.emplace_back(m_config.baseUrl + "/dav/" + UrlEncode(webdavPath, true), safeHeaders);

        std::string direct = m_config.baseUrl + "/d" + encoded;
        if (!sign.empty()) direct += "?sign=" + UrlEncode(sign);
        outCandidates.emplace_back(direct, safeHeaders);
    }

    auto raw = data["raw_url"].str();
    if (!raw.empty()) outCandidates.emplace_back(raw, safeHeaders);
    auto url = data["url"].str();
    if (!url.empty() && url != raw) outCandidates.emplace_back(url, safeHeaders);
    return !outCandidates.empty();
}

bool QuarkOpenListProvider::DownloadRemotePath(const std::string& remotePath, std::vector<uint8_t>& outData) {
    outData.clear();
    auto r = ApiRequest("POST", "/api/fs/get", Obj({{"path", Json::String(remotePath)}, {"password", Json::String("")}, {"refresh", Json::Value{Json::Type::Bool, true}}}));
    if (r.status != 200) return false;
    auto j = Json::Parse(r.body); if (!HasOpenListSuccess(j)) return false;
    std::vector<std::pair<std::string, std::vector<std::string>>> candidates;
    if (!BuildDownloadCandidates(j["data"], remotePath, candidates)) return false;
    for (const auto& candidate : candidates) {
        auto dl = RequestUrl("GET", candidate.first, {}, candidate.second);
        if (dl.status == 200) {
            outData.assign(dl.body.begin(), dl.body.end());
            return true;
        }
        LOG("[Quark] Download candidate failed HTTP %d", dl.status);
    }
    return false;
}

bool QuarkOpenListProvider::Download(const std::string& path, std::vector<uint8_t>& outData) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto remote = RemotePathFor(path); if (remote.empty()) return false;
    return DownloadRemotePath(remote, outData);
}

bool QuarkOpenListProvider::Remove(const std::string& path) { std::lock_guard<std::mutex> lock(m_mutex); auto remote=RemotePathFor(path); return !remote.empty() && RemoveRemote(remote); }
ICloudProvider::ExistsStatus QuarkOpenListProvider::CheckExists(const std::string& path) {
    std::lock_guard<std::mutex> lock(m_mutex); auto remote=RemotePathFor(path); if (remote.empty()) return ExistsStatus::Error;
    auto r=ApiRequest("POST","/api/fs/get",Obj({{"path",Json::String(remote)},{"password",Json::String("")},{"refresh",Json::Value{Json::Type::Bool,true}}}));
    if (r.status == 404) return ExistsStatus::Missing;
    if (r.status != 200) return ExistsStatus::Error;
    auto j=Json::Parse(r.body);
    if (HasOpenListSuccess(j)) return ExistsStatus::Exists;
    if (IsOpenListMissing(j)) return ExistsStatus::Missing;
    return ExistsStatus::Error;
}

bool QuarkOpenListProvider::ListRecursive(const std::string& remotePath, const std::string& relPrefix, std::vector<FileInfo>& outFiles, bool* outComplete, int depth) {
    if (depth > 32) { if (outComplete) *outComplete = false; return false; }
    constexpr int perPage = 500;
    for (int page = 1; page <= 1000; ++page) {
        auto r=ApiRequest("POST","/api/fs/list",Obj({{"path",Json::String(remotePath)},{"password",Json::String("")},{"page",Json::Number(page)},{"per_page",Json::Number(perPage)},{"refresh",Json::Value{Json::Type::Bool,true}}}));
        if (r.status == 404) return true;
        if (r.status != 200) return false;
        auto j=Json::Parse(r.body);
        if (!HasOpenListSuccess(j)) return IsOpenListMissing(j);
        auto& content=j["data"]["content"];
        if (content.type != Json::Type::Array) return false;
        for (size_t i=0;i<content.size();++i) { auto& it=content[i]; std::string name=it["name"].str(); if (name.empty()) continue; std::string rel=relPrefix.empty()?name:relPrefix+"/"+name; if (it["is_dir"].boolean()) { if (!ListRecursive(remotePath+"/"+name, rel, outFiles, outComplete, depth+1)) return false; } else { FileInfo fi; fi.path=rel; fi.size=(uint64_t)it["size"].integer(); outFiles.push_back(std::move(fi)); } }
        if (content.size() < (size_t)perPage) return true;
    }
    if (outComplete) *outComplete = false;
    LOG("[Quark] List exceeded pagination safety limit");
    return false;
}
std::vector<ICloudProvider::FileInfo> QuarkOpenListProvider::List(const std::string& prefix) { std::vector<FileInfo> f; ListChecked(prefix,f); return f; }
bool QuarkOpenListProvider::ListChecked(const std::string& prefix, std::vector<FileInfo>& outFiles, bool* outComplete) {
    std::lock_guard<std::mutex> lock(m_mutex); outFiles.clear(); if (outComplete) *outComplete=false;
    std::string remote = prefix.empty()?m_config.remoteRootPath:RemotePathFor(prefix); if (remote.empty()) return false;
    bool ok=ListRecursive(remote,prefix,outFiles,outComplete); if (ok && outComplete) *outComplete=true; return ok;
}

bool QuarkOpenListProvider::HealthCheck() {
    std::string p = m_config.remoteRootPath + "/.healthcheck/cloudredirect-health.json";
    std::string body = "{\"type\":\"cloudredirect-quark-health-check\"}";
    if (!Mkdir(ParentPath(p))) return false;
    auto up=RequestUrl("PUT",m_config.baseUrl+"/api/fs/put",body,{"File-Path: "+UrlEncode(p,false),"Content-Type: application/json"});
    if (up.status != 200 || !HasOpenListSuccess(Json::Parse(up.body))) return false;
    std::vector<uint8_t> downloaded;
    bool ok = DownloadRemotePath(p, downloaded);
    RemoveRemote(p);
    return ok && std::string(downloaded.begin(), downloaded.end()).find("cloudredirect-quark-health-check") != std::string::npos;
}

bool QuarkOpenListProvider::Init(const std::string& configPath) {
    std::lock_guard<std::mutex> lock(m_mutex); if (m_initialized) return true;
    if (!LoadConfig(configPath) || !EnsureDataDir() || !SetAdminPassword()) return false;
    m_transport = CreateHttpTransport("[Quark]"); if (!m_transport || !m_transport->Init()) return false;
    if (!StartOpenList() || !LoginWithRetry() || !ConfigureStorage() || !EnsureRemoteRoot()) return false;
    m_authenticated = HealthCheck(); m_initialized = true; LOG("[Quark] Initialized authenticated=%s", m_authenticated?"true":"false"); return true;
}
void QuarkOpenListProvider::Shutdown() {
    std::lock_guard<std::mutex> lock(m_mutex);
#ifdef _WIN32
    if (m_ownsProcess) { TerminateProcess(m_processInfo.hProcess, 0); CloseHandle(m_processInfo.hProcess); CloseHandle(m_processInfo.hThread); m_ownsProcess=false; }
#endif
    if (m_transport) m_transport->Shutdown(); m_transport.reset(); m_initialized=false; m_authenticated=false; m_apiToken.clear(); LOG("[Quark] Shutdown");
}
bool QuarkOpenListProvider::IsAuthenticated() const { std::lock_guard<std::mutex> lock(m_mutex); return m_initialized && m_authenticated && !m_apiToken.empty(); }
