#pragma once

#include "cloud_provider.h"
#include "cloud_provider_base.h"

#include <memory>
#include <mutex>
#include <string>
#include <vector>

class WebDavProvider : public ICloudProvider {
public:
    const char* Name() const override { return "WebDAV"; }

    bool Init(const std::string& configPath) override;
    void Shutdown() override;
    bool IsAuthenticated() const override;

    bool Upload(const std::string& path, const uint8_t* data, size_t len) override;
    bool Download(const std::string& path, std::vector<uint8_t>& outData) override;
    bool Remove(const std::string& path) override;
    ExistsStatus CheckExists(const std::string& path) override;
    std::vector<FileInfo> List(const std::string& prefix) override;
    bool ListChecked(const std::string& prefix, std::vector<FileInfo>& outFiles,
                     bool* outComplete = nullptr) override;

private:
    struct Config {
        std::string serverUrl;
        std::string username;
        std::string password;
        std::string remoteRootPath = "/CloudRedirect";
    };

    bool LoadConfig(const std::string& configPath);
    bool ValidateRelativePath(const std::string& relPath) const;
    std::string RemotePathFor(const std::string& relPath) const;
    std::string UrlForRemotePath(const std::string& remotePath) const;
    std::vector<std::string> AuthHeaders(const std::vector<std::string>& extra = {}) const;
    HttpUtil::HttpResp RequestUrl(const char* method, const std::string& url,
                                  const std::string& body = {},
                                  const std::vector<std::string>& headers = {});
    bool EnsureDirectory(const std::string& remoteDir);
    bool EnsureRoot();
    bool IsCollectionBlock(const std::string& responseBlock) const;
    bool ParseListResponse(const std::string& body, const std::string& prefix,
                           std::vector<FileInfo>& outFiles, bool* outComplete) const;

    Config m_config;
    bool m_initialized = false;
    bool m_authenticated = false;
    std::unique_ptr<IHttpTransport> m_transport;
    mutable std::mutex m_mutex;
};
