#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace CloudIntercept {

// One synthesized remotecache.vdf row.
struct RemotecacheCandidate {
    std::string cleanName;     // e.g. "UnderTheIsland/save_data_01.ini"
    std::string token;         // e.g. "%WinAppDataLocal%" or empty for default
    std::vector<uint8_t> sha;  // 20 bytes SHA1 of the cloud blob (may be empty)
    uint64_t timestamp = 0;
    uint64_t rawSize = 0;
};

// Map a wire root token like "%WinAppDataLocal%" to Steam's
// ERemoteStorageFileRoot numeric id. Returns 0 (k_eFileRootDefault) for empty
// or unknown tokens.
uint32_t TokenToRootId(const std::string& token);

// Pure transform: given existing remotecache.vdf text and a candidate list for
// `appId`, return the rewritten text and how many entries were normalized.
// Existing entries are only modified when their SHA already matches the cloud
// candidate, which lets us clear stale Steam dirty state without masking real
// local save changes. Returns false if `original` lacks the expected top-level
// "<appId>" section, in which case `outRepaired` is left equal to `original`
// and `outAdded == 0`.
bool ApplyRemotecacheRepair(const std::string& original,
                            uint32_t appId,
                            const std::vector<RemotecacheCandidate>& candidates,
                            std::string& outRepaired,
                            size_t& outAdded);

// Update the ChangeNumber field in remotecache.vdf for the given app.
// Returns true if successful, false if the section or field wasn't found.
bool UpdateRemotecacheChangeNumber(const std::string& original,
                                   uint32_t appId,
                                   uint64_t newChangeNumber,
                                   std::string& outUpdated);

// Mark all file rows in an app section as synchronized and update ChangeNumber.
// This is used after a local AutoCloud fallback has already uploaded/published
// the same data, but Steam's own HTTP upload path timed out before it could
// clean up remotecache.vdf.
bool MarkRemotecacheSynced(const std::string& original,
                           uint32_t appId,
                           uint64_t newChangeNumber,
                           std::string& outUpdated);

} // namespace CloudIntercept
