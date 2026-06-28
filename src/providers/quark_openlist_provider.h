#pragma once

#include "cloud_provider.h"
#include "cloud_provider_base.h"
#include "json.h"

#include <memory>
#include <mutex>
#include <string>
#include <vector>

#ifdef _WIN32
#include <Windows.h>
#endif

class QuarkOpenListProvider : public ICloudProvider {
public:
    const char* Name() const override { return "Quark OpenList"; }

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
        std::string cookie;
        std::string openListExe;
        std::string dataDir;
        std::string adminPassword;
        std::string baseUrl = "http://127.0.0.1:5244";
        std::string mountPath = "/Quark";
        std::string remoteRootPath = "/Quark/CloudRedirect";
        bool forceStorageUpdate = false;
    };

    bool LoadConfig(const std::string& configPath);
    bool SaveConfig();
    void ApplyDefaults();
    std::string GenerateAdminPassword() const;
    bool EnsureDataDir();
    bool SetAdminPassword();
    bool StartOpenList();
    bool LoginWithRetry();
    bool ConfigureStorage();
    bool EnsureRemoteRoot();
    bool HealthCheck();

    bool ValidateRelativePath(const std::string& relPath) const;
    std::string RemotePathFor(const std::string& relPath) const;
    std::string ParentPath(const std::string& remotePath) const;
    std::string LeafName(const std::string& remotePath) const;
    std::vector<std::string> ApiHeaders(const std::vector<std::string>& extra = {}) const;
    HttpUtil::HttpResp RequestUrl(const char* method, const std::string& url,
                                  const std::string& body = {},
                                  const std::vector<std::string>& headers = {});
    HttpUtil::HttpResp ApiRequest(const char* method, const std::string& path,
                                  const Json::Value& body,
                                  const std::vector<std::string>& extraHeaders = {});
    bool Mkdir(const std::string& remotePath);
    bool RemoveRemote(const std::string& remotePath);
    void CleanupLegacyHealthFiles();
    bool ListRecursive(const std::string& remotePath, const std::string& relPrefix,
                       std::vector<FileInfo>& outFiles, bool* outComplete, int depth = 0);
    bool BuildDownloadCandidates(const Json::Value& data, const std::string& remotePath,
                                 std::vector<std::pair<std::string, std::vector<std::string>>>& outCandidates) const;
    bool DownloadRemotePath(const std::string& remotePath, std::vector<uint8_t>& outData);

    Config m_config;
    std::string m_configPath;
    std::string m_apiToken;
    bool m_initialized = false;
    bool m_authenticated = false;
    std::unique_ptr<IHttpTransport> m_transport;
    mutable std::mutex m_mutex;
#ifdef _WIN32
    PROCESS_INFORMATION m_processInfo{};
    bool m_ownsProcess = false;
#endif
};
