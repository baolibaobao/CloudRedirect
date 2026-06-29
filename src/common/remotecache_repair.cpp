#include "remotecache_repair.h"
#include "steam_root_ids.h"
#include "vdf.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <limits>
#include <string_view>
#include <unordered_set>

namespace CloudIntercept {

uint32_t TokenToRootId(const std::string& token) {
    if (token.empty()) return 0;
    for (const auto& e : SteamRootIds::kEntries) {
        if (token == e.token) return e.rootId;
    }
    return 0;  // k_eFileRootDefault
}

static std::string ShaToHex(const std::vector<uint8_t>& sha) {
    static const char kHex[] = "0123456789abcdef";
    std::string out;
    out.resize(sha.size() * 2);
    for (size_t i = 0; i < sha.size(); ++i) {
        out[2 * i]     = kHex[(sha[i] >> 4) & 0xF];
        out[2 * i + 1] = kHex[sha[i] & 0xF];
    }
    return out;
}

static std::string ToLowerAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
}

static std::string_view TrimLine(std::string_view line) {
    size_t start = line.find_first_not_of(" \t\r\n");
    if (start == std::string_view::npos) return {};
    size_t end = line.find_last_not_of(" \t\r\n");
    return line.substr(start, end - start + 1);
}

static bool ParseQuotedKey(std::string_view line, std::string& key, bool& hasValue) {
    auto trimmed = TrimLine(line);
    hasValue = false;
    if (trimmed.size() < 3 || trimmed[0] != '"') return false;

    size_t keyEnd = trimmed.find('"', 1);
    if (keyEnd == std::string_view::npos) return false;
    key.assign(trimmed.data() + 1, keyEnd - 1);

    size_t valueStart = trimmed.find('"', keyEnd + 1);
    hasValue = valueStart != std::string_view::npos;
    return true;
}

static std::string GetFieldValue(std::string_view section, const char* field) {
    size_t pos = 0;
    while (pos < section.size()) {
        size_t lineEnd = section.find('\n', pos);
        if (lineEnd == std::string_view::npos) lineEnd = section.size();
        auto trimmed = TrimLine(section.substr(pos, lineEnd - pos));

        std::string key;
        bool hasValue = false;
        if (ParseQuotedKey(trimmed, key, hasValue) && hasValue && key == field) {
            size_t keyEnd = trimmed.find('"', 1);
            size_t valueStart = trimmed.find('"', keyEnd + 1);
            size_t valueEnd = trimmed.find('"', valueStart + 1);
            if (valueStart != std::string_view::npos && valueEnd != std::string_view::npos) {
                return std::string(trimmed.data() + valueStart + 1, valueEnd - valueStart - 1);
            }
        }

        pos = lineEnd + 1;
    }
    return {};
}

static uint64_t ParseUintField(std::string_view section, const char* field) {
    std::string value = GetFieldValue(section, field);
    if (value.empty()) return 0;
    char* end = nullptr;
    unsigned long long parsed = std::strtoull(value.c_str(), &end, 10);
    return end && *end == '\0' ? static_cast<uint64_t>(parsed) : 0;
}

static std::string DetectFieldIndent(const std::string& vdfContent,
                                     size_t sectionStart,
                                     size_t sectionEnd) {
    size_t pos = sectionStart;
    while (pos < sectionEnd) {
        size_t lineEnd = vdfContent.find('\n', pos);
        if (lineEnd == std::string::npos || lineEnd > sectionEnd) lineEnd = sectionEnd;
        std::string_view line(vdfContent.data() + pos, lineEnd - pos);
        auto trimmed = TrimLine(line);
        if (!trimmed.empty() && trimmed != "{" && trimmed != "}") {
            size_t indentEnd = 0;
            while (indentEnd < line.size() &&
                   (line[indentEnd] == '\t' || line[indentEnd] == ' ')) {
                ++indentEnd;
            }
            return std::string(line.data(), indentEnd);
        }
        pos = lineEnd + 1;
    }
    return "\t\t";
}

static bool SetFieldInSection(std::string& vdfContent,
                              const char* const* sectionPath,
                              size_t pathLen,
                              const char* field,
                              const std::string& value) {
    bool found = false;
    bool changed = false;
    VdfUtil::ForEachFieldInSection(vdfContent, sectionPath, static_cast<int>(pathLen),
        [&](const VdfUtil::FieldInfo& fi) {
            if (fi.key == field) {
                found = true;
                if (fi.value != value) {
                    vdfContent.replace(fi.valStart, fi.valEnd - fi.valStart, value);
                    changed = true;
                }
                return false;
            }
            return true;
        });
    if (found) return changed;

    size_t sectionStart = 0;
    size_t sectionEnd = 0;
    if (!VdfUtil::FindVdfSectionRange(vdfContent, sectionPath, pathLen,
                                      sectionStart, sectionEnd)) {
        return false;
    }

    std::string indent = DetectFieldIndent(vdfContent, sectionStart, sectionEnd);
    vdfContent.insert(sectionEnd,
                      indent + "\"" + field + "\"\t\t\"" + value + "\"\n");
    return true;
}

struct ChildSectionRange {
    size_t start = 0;
    size_t end = 0;
};

static std::vector<ChildSectionRange> FindDirectChildSections(
        const std::string& vdfContent,
        size_t sectionStart,
        size_t sectionEnd,
        const std::string& childName) {
    std::vector<ChildSectionRange> ranges;
    int depth = 0;
    std::string pendingKey;
    size_t pendingStart = 0;
    size_t currentStart = 0;
    std::string currentKey;

    size_t pos = sectionStart;
    while (pos < sectionEnd) {
        size_t lineEnd = vdfContent.find('\n', pos);
        if (lineEnd == std::string::npos || lineEnd > sectionEnd) lineEnd = sectionEnd;
        size_t nextLine = lineEnd < vdfContent.size() ? lineEnd + 1 : lineEnd;
        std::string_view line(vdfContent.data() + pos, lineEnd - pos);
        auto trimmed = TrimLine(line);

        if (trimmed == "{") {
            if (depth == 0 && !pendingKey.empty()) {
                currentKey = pendingKey;
                currentStart = pendingStart;
                pendingKey.clear();
            }
            ++depth;
        } else if (trimmed == "}") {
            if (depth > 0) {
                --depth;
                if (depth == 0) {
                    if (currentKey == childName) {
                        ranges.push_back({currentStart, nextLine});
                    }
                    currentKey.clear();
                    currentStart = 0;
                }
            }
        } else if (depth == 0 && !trimmed.empty()) {
            std::string key;
            bool hasValue = false;
            if (ParseQuotedKey(trimmed, key, hasValue)) {
                if (!hasValue) {
                    pendingKey = std::move(key);
                    pendingStart = pos;
                } else {
                    pendingKey.clear();
                }
            }
        }

        pos = nextLine;
    }

    return ranges;
}

static int64_t ScoreRemotecacheSection(std::string_view section, uint32_t expectedRootId) {
    std::string syncState = GetFieldValue(section, "syncstate");
    uint64_t rootId = ParseUintField(section, "root");
    uint64_t newestTime = std::max({
        ParseUintField(section, "localtime"),
        ParseUintField(section, "time"),
        ParseUintField(section, "remotetime")
    });

    int64_t score = static_cast<int64_t>(std::min<uint64_t>(newestTime, 999999999ULL));
    if (!syncState.empty() && syncState != "1") score += 2000000000LL;
    if (rootId == expectedRootId) score += 1000000000LL;
    return score;
}

static bool SectionMatchesCandidate(std::string_view section,
                                    const std::string& expectedSha,
                                    uint32_t expectedRootId,
                                    uint64_t expectedSize,
                                    bool requireRoot,
                                    bool requireSize) {
    if (!expectedSha.empty() &&
        ToLowerAscii(GetFieldValue(section, "sha")) != expectedSha) {
        return false;
    }
    if (requireRoot && ParseUintField(section, "root") != expectedRootId) {
        return false;
    }
    if (requireSize && expectedSize > 0 &&
        ParseUintField(section, "size") != expectedSize) {
        return false;
    }
    return true;
}

static size_t DeduplicateCandidateSections(std::string& vdfContent,
                                           uint32_t appId,
                                           const std::vector<RemotecacheCandidate>& candidates) {
    size_t removed = 0;
    std::string appIdStr = std::to_string(appId);
    const char* topSection = appIdStr.c_str();

    for (const auto& candidate : candidates) {
        if (candidate.cleanName.empty()) continue;

        size_t sectionStart = 0;
        size_t sectionEnd = 0;
        if (!VdfUtil::FindVdfSectionRange(vdfContent, &topSection, 1, sectionStart, sectionEnd)) {
            return removed;
        }

        auto ranges = FindDirectChildSections(vdfContent, sectionStart, sectionEnd, candidate.cleanName);
        if (ranges.size() <= 1) continue;

        uint32_t expectedRootId = TokenToRootId(candidate.token);
        std::string expectedSha = candidate.sha.size() == 20 ? ShaToHex(candidate.sha) : std::string();
        size_t keepIndex = 0;

        bool foundPreferred = false;
        const struct {
            bool requireRoot;
            bool requireSize;
        } passes[] = {
            { true,  true  },
            { true,  false },
            { false, true  },
            { false, false },
        };
        for (const auto& pass : passes) {
            for (size_t i = 0; i < ranges.size(); ++i) {
                std::string_view section(vdfContent.data() + ranges[i].start,
                                         ranges[i].end - ranges[i].start);
                if (SectionMatchesCandidate(section, expectedSha, expectedRootId,
                                            candidate.rawSize,
                                            pass.requireRoot, pass.requireSize)) {
                    keepIndex = i;
                    foundPreferred = true;
                    break;
                }
            }
            if (foundPreferred) break;
        }

        if (!foundPreferred) {
            int64_t bestScore = std::numeric_limits<int64_t>::min();
            for (size_t i = 0; i < ranges.size(); ++i) {
                std::string_view section(vdfContent.data() + ranges[i].start,
                                         ranges[i].end - ranges[i].start);
                int64_t score = ScoreRemotecacheSection(section, expectedRootId);
                if (score > bestScore) {
                    bestScore = score;
                    keepIndex = i;
                }
            }
        }

        for (size_t i = ranges.size(); i-- > 0;) {
            if (i == keepIndex) continue;
            vdfContent.erase(ranges[i].start, ranges[i].end - ranges[i].start);
            ++removed;
        }
    }

    return removed;
}

static size_t NormalizeMatchingCandidateSections(
        std::string& vdfContent,
        uint32_t appId,
        const std::vector<RemotecacheCandidate>& candidates) {
    size_t normalized = 0;
    std::string appIdStr = std::to_string(appId);
    const char* topSection = appIdStr.c_str();

    for (const auto& candidate : candidates) {
        if (candidate.cleanName.empty() || candidate.sha.size() != 20) continue;

        size_t appSectionStart = 0;
        size_t appSectionEnd = 0;
        if (!VdfUtil::FindVdfSectionRange(vdfContent, &topSection, 1,
                                          appSectionStart, appSectionEnd)) {
            return normalized;
        }

        auto ranges = FindDirectChildSections(vdfContent, appSectionStart, appSectionEnd,
                                              candidate.cleanName);
        if (ranges.empty()) continue;

        std::string expectedSha = ShaToHex(candidate.sha);
        uint32_t expectedRootId = TokenToRootId(candidate.token);

        for (const auto& range : ranges) {
            std::string_view section(vdfContent.data() + range.start, range.end - range.start);
            std::string currentSha = ToLowerAscii(GetFieldValue(section, "sha"));
            if (currentSha != expectedSha) continue;

            const char* fileSection[] = { appIdStr.c_str(), candidate.cleanName.c_str() };
            bool changed = false;
            changed |= SetFieldInSection(vdfContent, fileSection, 2, "root",
                                         std::to_string(expectedRootId));
            changed |= SetFieldInSection(vdfContent, fileSection, 2, "size",
                                         std::to_string(candidate.rawSize));
            if (candidate.timestamp > 0) {
                std::string ts = std::to_string(candidate.timestamp);
                changed |= SetFieldInSection(vdfContent, fileSection, 2, "localtime", ts);
                changed |= SetFieldInSection(vdfContent, fileSection, 2, "time", ts);
                changed |= SetFieldInSection(vdfContent, fileSection, 2, "remotetime", ts);
            }
            changed |= SetFieldInSection(vdfContent, fileSection, 2, "syncstate", "1");
            changed |= SetFieldInSection(vdfContent, fileSection, 2, "persiststate", "0");
            changed |= SetFieldInSection(vdfContent, fileSection, 2, "platformstosync2", "-1");

            if (changed) ++normalized;
            break;
        }
    }

    return normalized;
}

bool ApplyRemotecacheRepair(const std::string& original,
                            uint32_t appId,
                            const std::vector<RemotecacheCandidate>& candidates,
                            std::string& outRepaired,
                            size_t& outAdded) {
    outRepaired = original;
    outAdded = 0;
    if (candidates.empty()) return true;

    std::string appIdStr = std::to_string(appId);
    const char* topSection = appIdStr.c_str();
    size_t sectionStart = 0;
    size_t sectionEnd = 0;
    if (!VdfUtil::FindVdfSectionRange(outRepaired, &topSection, 1, sectionStart, sectionEnd)) {
        return false;
    }

    outAdded += DeduplicateCandidateSections(outRepaired, appId, candidates);
    outAdded += NormalizeMatchingCandidateSections(outRepaired, appId, candidates);
    outAdded += DeduplicateCandidateSections(outRepaired, appId, candidates);
    if (!VdfUtil::FindVdfSectionRange(outRepaired, &topSection, 1, sectionStart, sectionEnd)) {
        return false;
    }

    // Enumerate every direct child of the appid section: both scalar fields
    // ("ChangeNumber" "42") AND sub-section headers ("<filename>" { ... }).
    // File entries in remotecache.vdf are sub-sections, so ForEachChildInSection
    // (which reports both kinds) is the primitive we need here --
    // ForEachFieldInSection would skip file headers entirely.
    std::unordered_set<std::string> existing;
    VdfUtil::ForEachChildInSection(outRepaired, &topSection, 1,
        [&](std::string_view name) {
            existing.emplace(name);
            return true;
        });

    std::string insertions;
    for (const auto& c : candidates) {
        if (existing.count(c.cleanName)) continue;
        if (c.cleanName.empty()) continue;

        uint32_t rootId = TokenToRootId(c.token);
        std::string shaHex = c.sha.size() == 20 ? ShaToHex(c.sha) : std::string();

        // Match Steam's own field formatting: tab indent, double tab between
        // key and value, LF line endings. Steam reads CRLF too but writes LF;
        // matching its style avoids gratuitous diff churn on the file.
        insertions += "\t\"" + c.cleanName + "\"\n";
        insertions += "\t{\n";
        insertions += "\t\t\"root\"\t\t\"" + std::to_string(rootId) + "\"\n";
        insertions += "\t\t\"size\"\t\t\"" + std::to_string(c.rawSize) + "\"\n";
        insertions += "\t\t\"localtime\"\t\t\"" + std::to_string(c.timestamp) + "\"\n";
        insertions += "\t\t\"time\"\t\t\"" + std::to_string(c.timestamp) + "\"\n";
        insertions += "\t\t\"remotetime\"\t\t\"" + std::to_string(c.timestamp) + "\"\n";
        if (!shaHex.empty()) {
            insertions += "\t\t\"sha\"\t\t\"" + shaHex + "\"\n";
        }
        insertions += "\t\t\"syncstate\"\t\t\"1\"\n";          // synced
        insertions += "\t\t\"persiststate\"\t\t\"0\"\n";       // persisted
        insertions += "\t\t\"platformstosync2\"\t\t\"-1\"\n";  // all
        insertions += "\t}\n";
        ++outAdded;
    }

    if (!insertions.empty()) {
        outRepaired.insert(sectionEnd, insertions);
    }
    return true;
}

bool UpdateRemotecacheChangeNumber(const std::string& original,
                                   uint32_t appId,
                                   uint64_t newChangeNumber,
                                   std::string& outUpdated) {
    outUpdated = original;
    
    std::string appIdStr = std::to_string(appId);
    const char* topSection = appIdStr.c_str();
    
    // Find the ChangeNumber field in the app section
    bool found = false;
    size_t valStart = 0;
    size_t valEnd = 0;
    
    VdfUtil::ForEachFieldInSection(outUpdated, &topSection, 1,
        [&](const VdfUtil::FieldInfo& fi) {
            if (fi.key == "ChangeNumber") {
                valStart = fi.valStart;
                valEnd = fi.valEnd;
                found = true;
                return false; // stop iteration
            }
            return true;
        });
    
    if (!found) return false;
    
    // Replace the value in-place
    std::string newVal = std::to_string(newChangeNumber);
    outUpdated.replace(valStart, valEnd - valStart, newVal);
    return true;
}

bool MarkRemotecacheSynced(const std::string& original,
                           uint32_t appId,
                           uint64_t newChangeNumber,
                           std::string& outUpdated) {
    if (!UpdateRemotecacheChangeNumber(original, appId, newChangeNumber, outUpdated)) {
        return false;
    }

    std::string appIdStr = std::to_string(appId);
    const char* appSection[] = { appIdStr.c_str() };
    std::vector<std::string> fileSections;
    if (!VdfUtil::ForEachChildInSection(outUpdated, appSection, 1,
        [&](std::string_view name) {
            if (name != "ChangeNumber" && name != "OSType") {
                fileSections.emplace_back(name);
            }
            return true;
        })) {
        return false;
    }

    for (const auto& filename : fileSections) {
        const char* fileSection[] = { appIdStr.c_str(), filename.c_str() };
        bool foundSyncState = false;
        VdfUtil::ForEachFieldInSection(outUpdated, fileSection, 2,
            [&](const VdfUtil::FieldInfo& fi) {
                if (fi.key == "syncstate") {
                    outUpdated.replace(fi.valStart, fi.valEnd - fi.valStart, "1");
                    foundSyncState = true;
                    return false;
                }
                return true;
            });

        if (foundSyncState) continue;

        size_t sectionStart = 0;
        size_t sectionEnd = 0;
        if (!VdfUtil::FindVdfSectionRange(outUpdated, fileSection, 2,
                                          sectionStart, sectionEnd)) {
            continue;
        }
        std::string indent = "\t\t";
        size_t lineStart = outUpdated.rfind('\n', sectionEnd);
        if (lineStart != std::string::npos) {
            ++lineStart;
            size_t indentEnd = lineStart;
            while (indentEnd < outUpdated.size() &&
                   (outUpdated[indentEnd] == '\t' || outUpdated[indentEnd] == ' ')) {
                ++indentEnd;
            }
            indent.assign(outUpdated.data() + lineStart, indentEnd - lineStart);
            indent.push_back('\t');
        }
        outUpdated.insert(sectionEnd, indent + "\"syncstate\"\t\t\"1\"\n");
    }

    return true;
}

} // namespace CloudIntercept
