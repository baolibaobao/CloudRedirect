#include "webdav_provider.h"

#include "http_util.h"
#include "json.h"
#include "log.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <sstream>

using HttpUtil::HttpResp;
using HttpUtil::UrlDecode;
using HttpUtil::UrlEncode;

namespace {

std::string Trim(std::string s) {
    auto notSpace = [](unsigned char c) { return !std::isspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), notSpace));
    s.erase(std::find_if(s.rbegin(), s.rend(), notSpace).base(), s.end());
    return s;
}

std::string Base64Encode(const std::string& input) {
    static constexpr char table[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    int val = 0;
    int valb = -6;
    for (uint8_t c : input) {
        val = (val << 8) + c;
        valb += 8;
        while (valb >= 0) {
            out.push_back(table[(val >> valb) & 0x3F]);
            valb -= 6;
        }
    }
    if (valb > -6) out.push_back(table[((val << 8) >> (valb + 8)) & 0x3F]);
    while (out.size() % 4) out.push_back('=');
    return out;
}

std::string StripTrailingSlashes(std::string s) {
    while (s.size() > 1 && s.back() == '/') s.pop_back();
    return s;
}

std::string NormalizeRemoteRoot(std::string s) {
    if (s.empty()) return "/CloudRedirect";
    std::replace(s.begin(), s.end(), '\\', '/');
    if (s.front() != '/') s.insert(s.begin(), '/');
    return StripTrailingSlashes(s);
}

std::string ExtractTagValue(const std::string& block, const std::string& localName) {
    size_t pos = 0;
    while (true) {
        size_t lt = block.find('<', pos);
        if (lt == std::string::npos) return {};
        if (lt + 1 < block.size() && block[lt + 1] == '/') { pos = lt + 1; continue; }
        size_t gt = block.find('>', lt);
        if (gt == std::string::npos) return {};
        std::string tag = block.substr(lt + 1, gt - lt - 1);
        size_t space = tag.find_first_of(" \t\r\n");
        if (space != std::string::npos) tag.resize(space);
        size_t colon = tag.find(':');
        std::string name = (colon != std::string::npos) ? tag.substr(colon + 1) : tag;
        if (name != localName) { pos = gt + 1; continue; }
        std::string close1 = "</" + tag + ">";
        size_t close = block.find(close1, gt + 1);
        if (close == std::string::npos) {
            std::string close2 = "</" + localName + ">";
            close = block.find(close2, gt + 1);
        }
        if (close == std::string::npos) return {};
        return block.substr(gt + 1, close - gt - 1);
    }
}

bool HasTag(const std::string& block, const std::string& localName) {
    size_t pos = 0;
    while (true) {
        size_t lt = block.find('<', pos);
        if (lt == std::string::npos) return false;
        if (lt + 1 < block.size() && block[lt + 1] == '/') { pos = lt + 1; continue; }
        size_t gt = block.find('>', lt);
        if (gt == std::string::npos) return false;
        std::string tag = block.substr(lt + 1, gt - lt - 1);
        size_t space = tag.find_first_of(" \t\r\n/");
        if (space != std::string::npos) tag.resize(space);
        size_t colon = tag.find(':');
        std::string name = (colon != std::string::npos) ? tag.substr(colon + 1) : tag;
        if (name == localName) return true;
        pos = gt + 1;
    }
}

uint64_t ParseUint64(const std::string& s) {
    char* end = nullptr;
    unsigned long long v = std::strtoull(s.c_str(), &end, 10);
    return end && *end == 0 ? static_cast<uint64_t>(v) : 0;
}

} // namespace

bool WebDavProvider::LoadConfig(const std::string& configPath) {
    std::ifstream f(configPath, std::ios::binary);
    if (!f) {
        LOG("[WebDAV] Config not found: %s", configPath.c_str());
        return false;
    }
    std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    auto root = Json::Parse(content);
    if (root.type != Json::Type::Object) {
        LOG("[WebDAV] Config is not a JSON object: %s", configPath.c_str());
        return false;
    }

    m_config.serverUrl = StripTrailingSlashes(Trim(root["server_url"].str()));
    m_config.username = root["username"].str();
    m_config.password = root["password"].str();
    m_config.remoteRootPath = NormalizeRemoteRoot(root["remote_root_path"].str());

    if (m_config.serverUrl.empty()) {
        LOG("[WebDAV] server_url missing in config");
        return false;
    }
    if (m_config.serverUrl.rfind("https://", 0) != 0) {
        LOG("[WebDAV] server_url must use HTTPS for generic WebDAV: %s", m_config.serverUrl.c_str());
        return false;
    }
    return true;
}

bool WebDavProvider::Init(const std::string& configPath) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_initialized) return true;
    if (!LoadConfig(configPath)) return false;
    m_transport = CreateHttpTransport("[WebDAV]");
    if (!m_transport || !m_transport->Init()) {
        LOG("[WebDAV] Transport init failed");
        return false;
    }
    m_initialized = true;
    m_authenticated = EnsureRoot();
    LOG("[WebDAV] Initialized (root=%s, authenticated=%s)",
        m_config.remoteRootPath.c_str(), m_authenticated ? "true" : "false");
    return true;
}

void WebDavProvider::Shutdown() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_transport) m_transport->Shutdown();
    m_transport.reset();
    m_initialized = false;
    m_authenticated = false;
    LOG("[WebDAV] Shutdown");
}

bool WebDavProvider::IsAuthenticated() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_initialized && m_authenticated;
}

bool WebDavProvider::ValidateRelativePath(const std::string& relPath) const {
    if (relPath.empty() || relPath[0] == '/' || relPath.find("\\") != std::string::npos ||
        relPath.find("://") != std::string::npos) return false;
    size_t start = 0;
    while (start <= relPath.size()) {
        size_t slash = relPath.find('/', start);
        std::string seg = slash == std::string::npos ? relPath.substr(start) : relPath.substr(start, slash - start);
        if (seg.empty() || seg == "." || seg == "..") return false;
        if (slash == std::string::npos) break;
        start = slash + 1;
    }
    return true;
}

std::string WebDavProvider::RemotePathFor(const std::string& relPath) const {
    if (!ValidateRelativePath(relPath)) return {};
    return m_config.remoteRootPath + "/" + relPath;
}

std::string WebDavProvider::UrlForRemotePath(const std::string& remotePath) const {
    return m_config.serverUrl + UrlEncode(remotePath, true);
}

std::vector<std::string> WebDavProvider::AuthHeaders(const std::vector<std::string>& extra) const {
    std::vector<std::string> headers = extra;
    if (!m_config.username.empty() || !m_config.password.empty()) {
        headers.push_back("Authorization: Basic " + Base64Encode(m_config.username + ":" + m_config.password));
    }
    headers.push_back("User-Agent: CloudRedirect/1.0");
    return headers;
}

HttpResp WebDavProvider::RequestUrl(const char* method, const std::string& url,
                                    const std::string& body,
                                    const std::vector<std::string>& headers) {
    if (!m_transport || !m_transport->IsReady()) return {};
    return m_transport->RequestUrl(method, url, body, AuthHeaders(headers));
}

bool WebDavProvider::EnsureDirectory(const std::string& remoteDir) {
    if (remoteDir.empty() || remoteDir == "/") return true;
    std::string cur;
    size_t start = 1; // remoteDir is absolute
    while (start <= remoteDir.size()) {
        size_t slash = remoteDir.find('/', start);
        std::string seg = slash == std::string::npos ? remoteDir.substr(start) : remoteDir.substr(start, slash - start);
        if (!seg.empty()) {
            cur += "/" + seg;
            auto r = RequestUrl("MKCOL", UrlForRemotePath(cur));
            if (!(r.status == 201 || r.status == 200 || r.status == 405 || r.status == 409)) {
                LOG("[WebDAV] MKCOL failed for %s: HTTP %d", cur.c_str(), r.status);
                return false;
            }
        }
        if (slash == std::string::npos) break;
        start = slash + 1;
    }
    return true;
}

bool WebDavProvider::EnsureRoot() {
    auto r = RequestUrl("PROPFIND", UrlForRemotePath(m_config.remoteRootPath), {}, {"Depth: 0"});
    if (r.status == 200 || r.status == 207) return true;
    if (r.status == 404) return EnsureDirectory(m_config.remoteRootPath);
    LOG("[WebDAV] Root PROPFIND failed: HTTP %d", r.status);
    return false;
}

bool WebDavProvider::Upload(const std::string& path, const uint8_t* data, size_t len) {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::string remote = RemotePathFor(path);
    if (remote.empty()) return false;
    size_t slash = remote.find_last_of('/');
    if (slash != std::string::npos && !EnsureDirectory(remote.substr(0, slash))) return false;
    std::string body(reinterpret_cast<const char*>(data), len);
    auto r = RequestUrl("PUT", UrlForRemotePath(remote), body, {"Content-Type: application/octet-stream"});
    return r.status == 200 || r.status == 201 || r.status == 204;
}

bool WebDavProvider::Download(const std::string& path, std::vector<uint8_t>& outData) {
    std::lock_guard<std::mutex> lock(m_mutex);
    outData.clear();
    std::string remote = RemotePathFor(path);
    if (remote.empty()) return false;
    auto r = RequestUrl("GET", UrlForRemotePath(remote));
    if (r.status != 200) return false;
    outData.assign(r.body.begin(), r.body.end());
    return true;
}

bool WebDavProvider::Remove(const std::string& path) {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::string remote = RemotePathFor(path);
    if (remote.empty()) return false;
    auto r = RequestUrl("DELETE", UrlForRemotePath(remote));
    return r.status == 200 || r.status == 202 || r.status == 204 || r.status == 404;
}

ICloudProvider::ExistsStatus WebDavProvider::CheckExists(const std::string& path) {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::string remote = RemotePathFor(path);
    if (remote.empty()) return ExistsStatus::Error;
    auto r = RequestUrl("PROPFIND", UrlForRemotePath(remote), {}, {"Depth: 0"});
    if (r.status == 200 || r.status == 207) return ExistsStatus::Exists;
    if (r.status == 404) return ExistsStatus::Missing;
    return ExistsStatus::Error;
}

std::vector<ICloudProvider::FileInfo> WebDavProvider::List(const std::string& prefix) {
    std::vector<FileInfo> files;
    ListChecked(prefix, files);
    return files;
}

bool WebDavProvider::IsCollectionBlock(const std::string& responseBlock) const {
    std::string type = ExtractTagValue(responseBlock, "resourcetype");
    return HasTag(type, "collection");
}

bool WebDavProvider::ParseListResponse(const std::string& body, const std::string& prefix,
                                       std::vector<FileInfo>& outFiles, bool* outComplete) const {
    std::string rootPrefix = StripTrailingSlashes(m_config.remoteRootPath) + "/";
    size_t pos = 0;
    bool sawResponse = false;
    while (true) {
        size_t start = body.find("<", pos);
        if (start == std::string::npos) break;
        size_t tagEnd = body.find('>', start);
        if (tagEnd == std::string::npos) { if (outComplete) *outComplete = false; return false; }
        std::string tag = body.substr(start + 1, tagEnd - start - 1);
        size_t space = tag.find_first_of(" \t\r\n");
        if (space != std::string::npos) tag.resize(space);
        size_t colon = tag.find(':');
        std::string name = colon != std::string::npos ? tag.substr(colon + 1) : tag;
        if (name != "response") { pos = tagEnd + 1; continue; }
        std::string close = "</" + tag + ">";
        size_t end = body.find(close, tagEnd + 1);
        if (end == std::string::npos) { if (outComplete) *outComplete = false; return false; }
        std::string block = body.substr(start, end + close.size() - start);
        pos = end + close.size();
        sawResponse = true;

        if (IsCollectionBlock(block)) continue;
        std::string href = UrlDecode(ExtractTagValue(block, "href"));
        if (href.empty()) continue;
        size_t rootPos = href.find(rootPrefix);
        if (rootPos == std::string::npos) continue;
        std::string rel = href.substr(rootPos + rootPrefix.size());
        if (!prefix.empty()) {
            if (rel.rfind(prefix, 0) != 0) continue;
        }
        if (rel.empty()) continue;
        FileInfo fi;
        fi.path = rel;
        fi.size = ParseUint64(ExtractTagValue(block, "getcontentlength"));
        outFiles.push_back(std::move(fi));
    }
    if (!sawResponse) { if (outComplete) *outComplete = false; return false; }
    if (outComplete) *outComplete = true;
    return true;
}

bool WebDavProvider::ListChecked(const std::string& prefix, std::vector<FileInfo>& outFiles,
                                 bool* outComplete) {
    std::lock_guard<std::mutex> lock(m_mutex);
    outFiles.clear();
    if (outComplete) *outComplete = false;
    std::string remote = prefix.empty() ? m_config.remoteRootPath : RemotePathFor(prefix);
    if (remote.empty()) return false;
    auto r = RequestUrl("PROPFIND", UrlForRemotePath(remote), {}, {"Depth: infinity"});
    if (r.status == 404) { if (outComplete) *outComplete = true; return true; }
    if (r.status != 207 && r.status != 200) {
        LOG("[WebDAV] List PROPFIND failed for %s: HTTP %d", prefix.c_str(), r.status);
        return false;
    }
    return ParseListResponse(r.body, prefix, outFiles, outComplete);
}
