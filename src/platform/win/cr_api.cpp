#define CR_API_EXPORTS
#include "cr_api.h"
#include "cloud_intercept.h"
#include "rpc_handlers.h"
#include "protobuf.h"
#include "pending_ops_journal.h"
#include "app_state.h"
#include "cloud_storage.h"
#include "local_storage.h"
#include "log.h"
#include "file_util.h"
#include "http_server.h"
#include "metadata_sync.h"

#include <atomic>
#include <mutex>
#include <string>

static std::mutex g_crInitMutex;
static std::atomic<bool> g_crInitDone{false};

bool CR_InitCloudSave(const char* steamPath, CR_NotifyFn notify) {
    if (!steamPath) return false;
    if (g_crInitDone.load(std::memory_order_acquire)) return true;

    std::lock_guard<std::mutex> lock(g_crInitMutex);
    if (g_crInitDone.load(std::memory_order_relaxed)) return true;

    try {
        std::string path(steamPath);
        if (!path.empty() && path.back() != '\\' && path.back() != '/')
            path += '\\';

        std::string logPath = path + "cloud_redirect.log";
        Log::Init(logPath.c_str());

        LOG("CloudRedirect loaded via CR_InitCloudSave (third-party client), PID=%u",
            GetCurrentProcessId());
        LOG("Steam path: %s", path.c_str());

        CloudIntercept::Init(path, /*cloudSaveOnly=*/true, notify);

        g_crInitDone.store(true, std::memory_order_release);
        LOG("CR_InitCloudSave complete");
        return true;
    } catch (const std::exception& ex) {
        LOG("CR_InitCloudSave FAILED: %s", ex.what());
        return false;
    } catch (...) {
        LOG("CR_InitCloudSave FAILED: unknown exception");
        return false;
    }
}

bool CR_HandleCloudRpc(const char* method, uint32_t appId,
                       uint32_t accountId,
                       const uint8_t* reqBody, uint32_t reqLen,
                       uint8_t* respBuf, uint32_t respMaxLen,
                       uint32_t* respLen, int32_t* eresult) {
    if (!respLen || !eresult) return false;
    *respLen = 0;
    *eresult = 2; // EResult::Fail
    if (!g_crInitDone.load(std::memory_order_acquire)) return false;
    if (!method || !respBuf) return false;
    if (!reqBody && reqLen > 0) return false;

    if (!CloudIntercept::IsNamespaceApp(appId)) return false;

    if (accountId != 0 && CloudIntercept::GetAccountId() == 0) {
        CloudIntercept::SetAccountId(accountId);
        HttpServer::SetAccountId(accountId);
    }

    auto fields = PB::Parse(reqBody, reqLen);

    using namespace CloudIntercept;
    std::optional<RpcResult> result;

    if (strcmp(method, RPC_GET_CHANGELIST) == 0)       result = HandleGetChangelist(appId, fields);
    else if (strcmp(method, RPC_LAUNCH_INTENT) == 0)   result = HandleLaunchIntent(appId, fields);
    else if (strcmp(method, RPC_SUSPEND_SESSION) == 0)  result = HandleSuspendSession(appId, fields);
    else if (strcmp(method, RPC_RESUME_SESSION) == 0)   result = HandleResumeSession(appId, fields);
    else if (strcmp(method, RPC_QUOTA_USAGE) == 0)     result = HandleQuotaUsage(appId, fields);
    else if (strcmp(method, RPC_BEGIN_BATCH) == 0)      result = HandleBeginBatch(appId, fields);
    else if (strcmp(method, RPC_BEGIN_UPLOAD) == 0)     result = HandleBeginFileUpload(appId, fields);
    else if (strcmp(method, RPC_COMMIT_UPLOAD) == 0)    result = HandleCommitFileUpload(appId, fields);
    else if (strcmp(method, RPC_COMPLETE_BATCH) == 0)   result = HandleCompleteBatch(appId, fields);
    else if (strcmp(method, RPC_FILE_DOWNLOAD) == 0)    result = HandleFileDownload(appId, fields);
    else if (strcmp(method, RPC_DELETE_FILE) == 0)      result = HandleDeleteFile(appId, fields);
    else if (strcmp(method, RPC_EXIT_SYNC) == 0 ||
             strcmp(method, RPC_SYNC_STATS) == 0) {

        if (strcmp(method, RPC_EXIT_SYNC) == 0) {
            uint64_t clientId = 0;
            bool uploadsCompleted = false, uploadsRequired = false;
            if (auto* f = PB::FindField(fields, 2)) clientId = f->varintVal;
            if (auto* f = PB::FindField(fields, 3)) uploadsCompleted = f->varintVal != 0;
            if (auto* f = PB::FindField(fields, 4)) uploadsRequired = f->varintVal != 0;
            if (accountId != 0) {
                if (!uploadsCompleted && uploadsRequired && CloudStorage::IsCloudActive()) {
                    auto stateResult = CloudStorage::FetchCloudState(accountId, appId);
                    if (stateResult.status == CloudStorage::StateFetchStatus::Ok) {
                        uint64_t localCN = LocalStorage::GetChangeNumber(accountId, appId);
                        if (stateResult.state.cn >= localCN) {
                            uploadsCompleted = true;
                            uploadsRequired = false;
                            LOG("[CR_API] ExitSyncDone app=%u: suppressing stale pending upload (cloudCN=%llu localCN=%llu)",
                                appId, (unsigned long long)stateResult.state.cn,
                                (unsigned long long)localCN);
                        }
                    }
                }
                PendingOpsJournal::RecordExitSyncState(accountId, appId,
                    uploadsCompleted, uploadsRequired, clientId);
                CloudStorage::ReleaseCloudSession(accountId, appId, clientId);
            }
            LOG("[CR_API] ExitSyncDone app=%u", appId);
        }
        *respLen = 0;
        *eresult = 1;
        return true;
    }

    if (!result.has_value()) return false;

    auto respData = result->body.Data();
    if (respData.size() > respMaxLen) {
        LOG("[CR_API] Response too large: %zu > %u", respData.size(), respMaxLen);
        return false;
    }

    memcpy(respBuf, respData.data(), respData.size());
    *respLen = static_cast<uint32_t>(respData.size());
    *eresult = result->eresult;
    return true;
}

void CR_AddApp(uint32_t appId) {
    CloudIntercept::AddNamespaceApp(appId);
}

void CR_RemoveApp(uint32_t appId) {
    CloudIntercept::RemoveNamespaceApp(appId);
    if (g_crInitDone.load(std::memory_order_acquire))
        LOG("[CR_API] Removed namespace app %u", appId);
}

bool CR_IsApp(uint32_t appId) {
    return CloudIntercept::IsNamespaceApp(appId);
}

void CR_SetApps(const uint32_t* appIds, uint32_t count) {
    if (count != 0 && appIds == nullptr) count = 0;
    size_t added = 0, removed = 0;
    CloudIntercept::SetNamespaceApps(appIds, count, &added, &removed);
    if (g_crInitDone.load(std::memory_order_acquire))
        LOG("[CR_API] SetApps: %u app(s) (%zu added, %zu removed)",
            count, added, removed);
}

bool CR_InstallVtableHooks(void) {
    if (!g_crInitDone.load(std::memory_order_acquire)) return false;
    bool ok = CloudIntercept::InstallVtableHooksForHost();
    LOG("[CR_API] InstallVtableHooks requested -> %d", ok);
    return ok;
}

void CR_EnableStatsSync(bool achievements, bool playtime) {
    if (!g_crInitDone.load(std::memory_order_acquire)) return;
    LOG("[CR_API] EnableStatsSync host=%d/%d config=%d/%d",
        achievements ? 1 : 0, playtime ? 1 : 0,
        MetadataSync::syncAchievements.load(std::memory_order_relaxed) ? 1 : 0,
        MetadataSync::syncPlaytime.load(std::memory_order_relaxed) ? 1 : 0);
}

void CR_NotifyAppRunning(uint32_t appId, bool running) {
    if (!g_crInitDone.load(std::memory_order_acquire)) return;
    CloudIntercept::NotifyHostAppRunning(appId, running);
}

void CR_NotifyStatsStored(uint32_t appId) {
    if (!g_crInitDone.load(std::memory_order_acquire)) return;
    CloudIntercept::NotifyHostStatsStored(appId);
}

void CR_Shutdown(void) {
    if (g_crInitDone.load(std::memory_order_acquire)) {
        LOG("[CR_API] Shutdown requested");
        CloudIntercept::Shutdown();
    }
}
