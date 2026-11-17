// mobile_save_list.hpp expects the engine's core and interface messages included first.
#include "core.h"
#include "shared/interface/messages.h"
#include "v_data.h"
#include "v_file_service.h"

#include "mobile_save_facts.hpp"
#include "save_thumbnail.hpp"
#include "utf8.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <ctime>
#include <filesystem>
#include <utility>
#include <memory>
#include <cstring>

namespace storm::mobile
{
std::optional<std::array<std::string, 4>> CurrentSaveIdentity()
{
    VDATA *result = core.Event("AndroidSaveIdentity");
    const char *text = nullptr;
    if (!result || !result->Get(text) || !text)
    {
        static bool reported = false;
        if (!std::exchange(reported, true))
            spdlog::error("save list: no AndroidSaveIdentity handler answered; records lose hero and location");
        return {};
    }
    const auto fields = SplitSaveDescription(text);
    if (fields.size() != 4 || fields[0].empty()) return {};
    return std::array<std::string, 4>{fields[0], fields[1], fields[2], fields[3]};
}

std::optional<MobileWidget> GameIdentityWidget(uint64_t opening)
{
    static std::pair<uint64_t, std::optional<std::array<std::string, 4>>> cached{~0ull, {}};
    if (cached.first != opening) cached = {opening, CurrentSaveIdentity()};
    if (!cached.second) return {};
    const auto &identity = *cached.second;
    MobileWidget widget{"ANDROID_GAME", "text", identity[0] + " · " + identity[1], "", -1, -1, true};
    SaveFacts facts;
    SaveStamp(identity[3], facts);
    widget.context = {{"hero", identity[0]}, {"location", identity[1]}};
    if (!facts.gameDate.empty()) widget.context["gameDate"] = facts.gameDate, widget.context["gameTime"] = facts.gameTime;
    return widget;
}

std::optional<MobileWidget> QuickLoadWidget()
{
    auto *profile = static_cast<VDATA *>(core.GetScriptVariable("PlayerProfile"));
    auto *attributes = profile ? profile->GetAClass() : nullptr;
    auto *name = attributes ? attributes->FindAClass(attributes, "name") : nullptr;
    if (!name || !name->HasValue() || name->GetValue().empty()) return {};
    auto *index = attributes->FindAClass(attributes, "QuickSaveIndex");
    const std::string file = std::string(name->GetValue()) + " QuickSave " + (index && index->HasValue() ? std::string(index->GetValue()) : "1");
    if (!fio->_FileOrDirectoryExists(("SAVE\\" + std::string(name->GetValue()) + "\\" + file).c_str())) return {};
    return MobileWidget{"ANDROID_QUICK_LOAD", "text", file, "", -1, -1, true};
}

// The description of a record file, read once per file time.
namespace
{
class SaveFactsCache
{
    struct Entry { uint64_t written = 0; bool present = false; SaveFacts facts; bool thumbnail = false; };
    std::map<std::string, Entry> entries_;
public:
    const Entry &get(const std::string &path)
    {
        const bool exists = fio->_FileOrDirectoryExists(path.c_str());
        const auto stamp = exists ? fio->_GetLastWriteTime(path.c_str()) : std::filesystem::file_time_type{};
        const uint64_t written = exists ? static_cast<uint64_t>(stamp.time_since_epoch().count()) : 0;
        auto found = entries_.find(path);
        if (found != entries_.end() && found->second.written == written && found->second.present == exists) return found->second;
        Entry entry{written, exists};
        if (exists)
        {
            int32_t size = 0;
            const std::unique_ptr<char[]> data(static_cast<char *>(core.GetSaveData(path.c_str(), size)));
            if (data && size > 0)
            {
                const auto extension = save::thumbnail::Decode({reinterpret_cast<const uint8_t *>(data.get()), static_cast<size_t>(size)});
                if (extension.valid)
                {
                    std::string description(extension.description);
                    if (!utf8::IsValidUtf8(description.c_str()))
                    {
                        utf8::FixInvalidUtf8(description.data());
                        description.resize(std::strlen(description.c_str()));
                    }
                    entry.facts = ParseSaveDescription(description);
                    entry.thumbnail = !extension.pixels.empty();
                    if (extension.picture_problem)
                        spdlog::error("Save '{}': {}: listed without its picture, still loadable", path, extension.picture_problem);
                }
                else
                    spdlog::error("Save '{}' has no readable description: listed without its facts", path);
            }
            const std::time_t local_time = fio->_ToTimeT(stamp);
            std::tm local{};
            localtime_r(&local_time, &local);
            char created[24];
            std::strftime(created, sizeof(created), "%Y-%m-%d %H:%M", &local);
            entry.facts.created = created;
        }
        return entries_[path] = std::move(entry);
    }
};
SaveFactsCache saveFacts;
} // namespace

std::map<std::string, std::string> SaveRowContext(const SaveListSnapshot &state, size_t index)
{
    std::map<std::string, std::string> context;
    if (index >= state.records.size())
    {
        // The record about to be written: the game in progress, no dates yet.
        static std::pair<uint64_t, std::optional<std::array<std::string, 4>>> identity{~0ull, {}};
        if (identity.first != state.revision) identity = {state.revision, CurrentSaveIdentity()};
        if (const auto &game = identity.second)
        {
            context["hero"] = (*game)[0];
            context["location"] = (*game)[1];
            if (const int minutes = SavePlayMinutes((*game)[2]); minutes >= 0) context["playMinutes"] = std::to_string(minutes);
        }
        return context;
    }
    const std::string path = "SAVE\\" + state.records[index].profile + "\\" + state.records[index].file;
    const auto &entry = saveFacts.get(path);
    const SaveFacts &facts = entry.facts;
    for (const auto &[key, value] : {std::pair<const char *, const std::string &>{"hero", facts.hero}, {"location", facts.location},
                                     {"gameDate", facts.gameDate}, {"gameTime", facts.gameTime}, {"created", facts.created}})
        if (!value.empty()) context[key] = value;
    if (entry.thumbnail) context["preview"] = save::thumbnail::Source(path, state.revision, entry.written);
    if (facts.playMinutes >= 0) context["playMinutes"] = std::to_string(facts.playMinutes);
    return context;
}

std::vector<SaveRecord> SaveRecordsOfAllProfiles()
{
    struct Found
    {
        SaveRecord record;
        std::filesystem::file_time_type written;
    };
    std::vector<Found> found;
    for (const auto &folder : fio->_GetFsPathsByMask("SAVE", nullptr, true, true, false))
    {
        const auto folderUtf8 = folder.filename().u8string();
        const std::string profile(folderUtf8.begin(), folderUtf8.end());
        // The game's profile list leaves the network save folder out (XI_FindFoldersWithoutNetsave).
        std::string lower = profile;
        for (char &c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (profile.empty() || lower == "netsave") continue;
        for (const auto &path : fio->_GetFsPathsByMask(("SAVE\\" + profile).c_str(), nullptr, true))
        {
            const auto utf8 = path.filename().u8string();
            const std::string name(utf8.begin(), utf8.end());
            const auto ends = [&name](const std::string &suffix) {
                return name.size() >= suffix.size() && name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0;
            };
            std::error_code error;
            const auto written = std::filesystem::last_write_time(path, error);
            if (error || ends(".storm-tmp") || ends(".storm-bak")) continue;
            found.push_back({{profile, name}, written});
        }
    }
    std::stable_sort(found.begin(), found.end(), [](const Found &a, const Found &b) { return a.written > b.written; });
    std::vector<SaveRecord> records;
    for (Found &entry : found)
        records.push_back(std::move(entry.record));
    return records;
}

std::optional<SaveRecord> ContinueRecord(uint64_t opening)
{
    static std::pair<uint64_t, std::optional<SaveRecord>> newest{~0ull, {}};
    if (newest.first != opening)
    {
        const auto records = SaveRecordsOfAllProfiles();
        newest = {opening, records.empty() ? std::nullopt : std::optional<SaveRecord>(records.front())};
    }
    return newest.second;
}

std::optional<MobileWidget> ContinueWidget(uint64_t opening)
{
    const auto newest = ContinueRecord(opening);
    if (!newest) return {};
    const SaveFacts &facts = saveFacts.get("SAVE\\" + newest->profile + "\\" + newest->file).facts;
    // A record whose description cannot be read is still the one «Продолжить» loads: its file name names it.
    const std::string label = facts.location.empty() ? newest->file : facts.hero.empty() ? facts.location : facts.hero + " · " + facts.location;
    MobileWidget widget{"ANDROID_CONTINUE", "text", label, "", -1, -1, true};
    if (!facts.location.empty()) widget.context["location"] = facts.location;
    if (!facts.hero.empty()) widget.context["hero"] = facts.hero;
    return widget;
}
} // namespace storm::mobile
