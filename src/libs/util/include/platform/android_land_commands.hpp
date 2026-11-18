#pragma once
#include "android_gameplay.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <tuple>

namespace storm::android
{
inline bool landCommandsTextValid(std::string_view value, size_t maximum)
{
    if (value.size() > maximum) return false;
    for (size_t i = 0; i < value.size();)
    {
        const auto lead = static_cast<unsigned char>(value[i]);
        const size_t count = lead >= 32 && lead < 127 ? 1 : lead >= 0xc2 && lead <= 0xdf ? 2 :
            lead >= 0xe0 && lead <= 0xef ? 3 : lead >= 0xf0 && lead <= 0xf4 ? 4 : 0;
        if (!count || i + count > value.size()) return false;
        for (size_t j = 1; j < count; ++j)
            if ((static_cast<unsigned char>(value[i + j]) & 0xc0) != 0x80) return false;
        // Java's strict parser also rejects the Unicode C1 control range.
        if (lead == 0xc2 && static_cast<unsigned char>(value[i + 1]) < 0xa0) return false;
        if (count >= 3)
        {
            const auto second = static_cast<unsigned char>(value[i + 1]);
            if ((lead == 0xe0 && second < 0xa0) || (lead == 0xed && second >= 0xa0) ||
                (lead == 0xf0 && second < 0x90) || (lead == 0xf4 && second > 0x8f)) return false;
        }
        i += count;
    }
    return true;
}
struct LandCommandItem
{
    std::string id, kind, label, source;
    // Identity is native-only and includes the complete command/target tuple.
    std::string identity;
    bool enabled = true;
    double cooldown = 1;
    // The picture the game draws for the item: "<texture file stem>:<cell>" in lower case (a travel
    // place is a cell of battle_interface\LandTarget_SLB: what the place is), "" = not reported.
    std::string picture;
    bool operator==(const LandCommandItem &) const = default;
};
struct LandCommandsFrame
{
    bool available = false, active = false, complete = true;
    std::string language, stage = "commands", pendingLabel, selectedItemId;
    std::string ownerIdentity; // Actual native command-list instance; never an executable Android token.
    int mode = 0;
    std::vector<LandCommandItem> items;
    // The script events of the root commands the menu would offer now (BLI_SetPossibleCommands'
    // enabled Commands), open or not: the touch HUD shows a button per group before the menu opens.
    std::vector<std::string> offered;
    // The travel stage only: the place of the town's table the hero stands in, which the game leaves out of
    // the list (its picture as LandCommandItem::picture, its name as the game's note); "" = none.
    std::string herePicture, hereLabel;
    bool operator==(const LandCommandsFrame &) const = default;
};
// Missing presentation support is not a gameplay prohibition. The original
// native menu may have been opened without any Android ownership at all.
enum class LandCommandSource { Missing, Forbidden, Ready };
struct LandCommandsRequest
{
    uint64_t scene = 0, revision = 0, activity = 0;
    std::string verb, id;
    uint32_t tick = 0;
};
class LandCommandsState
{
    inline static std::atomic<uint64_t> nextEpoch_{0};
    LandCommandsFrame frame_;
    const char *defect_ = nullptr;
    struct Receipt { uint64_t serial = 0; LandCommandsRequest request; bool accepted = false; std::string reason; } receipt_;
    std::deque<Receipt> receipts_;
    std::string sceneKey_;
    uint64_t scene_ = 0, revision_ = 1, activity_ = 0;
    // Whether the stock command list draws is not decided here: while the touch HUD presents the
    // land HUD it never draws (android_hud_presenter.hpp). This state only carries the menu and
    // the authority of the sheet's taps (scene, revision, activity).
    std::deque<LandCommandsRequest> pending_;
    static bool number(std::string_view value, uint64_t &result)
    {
        const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
        return !value.empty() && parsed.ec == std::errc() && parsed.ptr == value.data() + value.size();
    }
public:
    const LandCommandsFrame &frame() const { return frame_; }
    // What the last update cut or replaced in the script's frame, or nullptr: the publisher
    // logs it (android_ui.hpp, once per reason).
    const char *defect() const { return defect_; }
    // A receipt records native dispatch, never that the script began a reload.
    void result(const LandCommandsRequest &request, bool accepted, const std::string &reason)
    {
        if (request.verb != "activateItem") return;
        // A duplicate stale enqueue may not downgrade an accepted dispatch of this
        // exact lease. Another tap's receipt must not erase it before the UI reads it.
        for (const auto &old : receipts_)
            if (old.accepted && old.request.scene == request.scene && old.request.revision == request.revision &&
                old.request.activity == request.activity && old.request.id == request.id) return;
        ++receipt_.serial;
        receipt_.request = request; receipt_.accepted = accepted; receipt_.reason = reason;
        receipts_.push_back(receipt_);
        if (receipts_.size() > 8)
        {
            // Keep the newest accepted dispatch even through a burst of unrelated
            // refusals. Only a later accepted action may retire that final answer.
            const auto newestAccepted = std::find_if(receipts_.rbegin(), receipts_.rend(), [](const auto &r) { return r.accepted; });
            auto drop = receipts_.begin();
            if (newestAccepted != receipts_.rend() && &*drop == &*newestAccepted) ++drop;
            receipts_.erase(drop);
        }
    }
    uint64_t sceneEpoch() const { return scene_; }
    uint64_t revision() const { return revision_; }
    uint64_t activityGeneration() const { return activity_; }
    // A new revision: every tap made for the old one is stale (also after an executed one).
    void invalidate()
    {
        ++revision_;
        for (const auto &request : pending_) result(request, false, "invalidated-before-dispatch");
        pending_.clear();
    }
    void resetActivity()
    {
        activity_ = nextEpoch_.fetch_add(1, std::memory_order_relaxed) + 1;
        invalidate();
        receipt_ = {};
        receipts_.clear();
    }
    void update(std::string sceneKey, LandCommandsFrame frame)
    {
        // Always publish a bounded valid snapshot, including a recoverable
        // active/Back/Close state for incompatible or malformed script data.
        size_t textBytes = 0;
        defect_ = nullptr;
        const auto checkText = [&](std::string &text, size_t limit, bool required = false) {
            if (!landCommandsTextValid(text, limit) || (required && text.find_first_not_of(" ") == std::string::npos))
            { text = "\xE2\x80\xA6"; frame.complete = false; defect_ = "invalid, over-long or empty text shown as \xE2\x80\xA6"; }
            textBytes += text.size();
        };
        checkText(frame.language, 32); checkText(frame.pendingLabel, 1024);
        for (auto &item : frame.items) { checkText(item.label, 1024, true); checkText(item.source, 512); checkText(item.picture, 128); }
        for (auto &event : frame.offered) checkText(event, 128);
        if (!frame.herePicture.empty()) { checkText(frame.herePicture, 128); checkText(frame.hereLabel, 1024, true); }
        if (frame.offered.size() > 64) { frame.offered.clear(); frame.complete = false; defect_ = "over 64 offered events: the list is dropped"; }
        if (frame.items.size() > 256 || textBytes > 24000)
        {
            frame.items.clear(); frame.selectedItemId.clear(); frame.complete = false;
            defect_ = "over 256 items or 24000 text bytes: the items are dropped";
        }
        // JSON escaping can expand otherwise valid text (quotes/backslashes)
        // beyond the JNI/Java limit. Keep a complete recoverable envelope,
        // including current Back/Close ownership, instead of dropping updates.
        // Headroom covers epoch/revision digit growth later in this update.
        if (jsonFor(frame).size() > 64000)
        {
            frame.items.clear(); frame.selectedItemId.clear(); frame.complete = false;
            defect_ = "JSON over 64000 bytes: the items are dropped";
        }
        if (sceneKey_ != sceneKey || !scene_)
        {
            sceneKey_ = std::move(sceneKey);
            scene_ = nextEpoch_.fetch_add(1, std::memory_order_relaxed) + 1;
            invalidate();
        }
        // Fractional cooldown animation must not invalidate every in-flight tap.
        // Enabled state changes still invalidate the list; native confirm checks
        // the freshly sampled cooldown even if a stale UI claims readiness.
        auto stable = frame;
        for (auto &item : stable.items) item.cooldown = item.enabled ? 1 : 0;
        auto old = frame_;
        for (auto &item : old.items) item.cooldown = item.enabled ? 1 : 0;
        if (stable != old) invalidate();
        frame_ = std::move(frame);
    }
    bool releaseUnavailableSource(LandCommandSource source)
    {
        if (source == LandCommandSource::Ready) return false;
        // Revoke all Android authority on disappearance, including an in-flight
        // visual handoff, without closing an unowned/stock command tree.
        update({}, {});
        return source == LandCommandSource::Forbidden;
    }
    bool offer(std::string_view payload, uint32_t tick)
    {
        if (payload.size() > 256 || pending_.size() >= 8) return false;
        std::array<std::string_view, 6> fields;
        for (size_t i = 0; i < fields.size(); ++i)
        {
            const auto tab = payload.find('\t');
            if (i + 1 == fields.size())
            {
                if (tab != std::string_view::npos) return false;
                fields[i] = payload;
            }
            else
            {
                if (tab == std::string_view::npos) return false;
                fields[i] = payload.substr(0, tab);
                payload.remove_prefix(tab + 1);
            }
        }
        LandCommandsRequest request;
        if (fields[0] != "1" || !number(fields[1], request.scene) || !number(fields[2], request.revision) ||
            !number(fields[3], request.activity)) return false;
        request.verb = fields[4]; request.id = fields[5]; request.tick = tick;
        // Only a well-formed activation of this Activity receives a refusal receipt.
        const auto refuse = [&](const char *reason) {
            if (request.activity == activity_ && landCommandsTextValid(request.id, 128)) result(request, false, reason);
            return false;
        };
        if (request.scene != scene_ || request.revision != revision_ || request.activity != activity_ || !activity_)
            return refuse("stale-at-enqueue");
        if (!frame_.available) return refuse("unavailable-at-enqueue");
        if (request.verb != "open" && request.verb != "selectItem" &&
            request.verb != "activateItem" && request.verb != "back" && request.verb != "close") return false;
        if ((request.verb == "back" || request.verb == "close") && !request.id.empty()) return false;
        if (request.verb != "open" && !frame_.active) return refuse("menu-closed-at-enqueue");
        if (request.verb == "open" && frame_.active) return false;
        // "open" enters only a root command offered now; its id is a script event.
        if (request.verb == "open" && !request.id.empty() &&
            std::find(frame_.offered.begin(), frame_.offered.end(), request.id) == frame_.offered.end()) return false;
        if (request.verb == "selectItem" || request.verb == "activateItem")
        {
            if (!frame_.complete) return refuse("incomplete-at-enqueue");
            if (std::none_of(frame_.items.begin(), frame_.items.end(), [&](const auto &item) {
                return item.id == request.id && (request.verb != "activateItem" || (item.enabled && frame_.selectedItemId == request.id));
            })) return refuse("selection-changed-at-enqueue");
        }
        if (!pending_.empty())
        {
            const auto &pending = pending_.front();
            if (request.verb == pending.verb && request.scene == pending.scene && request.revision == pending.revision &&
                request.activity == pending.activity && request.id == pending.id) return false;
            return refuse("another-command-pending");
        }
        pending_.push_back(std::move(request));
        return true;
    }
    std::optional<LandCommandsRequest> take(uint32_t tick)
    {
        while (!pending_.empty())
        {
            auto request = std::move(pending_.front()); pending_.pop_front();
            if (request.scene == scene_ && request.revision == revision_ && request.activity == activity_ &&
                tick - request.tick <= 1500 && frame_.available) return request;
            result(request, false, tick - request.tick > 1500 ? "expired-before-dispatch" : "owner-changed-before-dispatch");
        }
        return {};
    }
    std::string jsonFor(const LandCommandsFrame &frame) const
    {
        const auto quote = [](const std::string &s) { return gameplayJsonString(s); };
        std::string out = "{\"schemaVersion\":1,\"sceneEpoch\":" + std::to_string(scene_) +
            ",\"revision\":" + std::to_string(revision_) + ",\"activityGeneration\":" + std::to_string(activity_) +
            ",\"available\":" + (frame.available ? "true" : "false") + ",\"active\":" + (frame.active ? "true" : "false") +
            ",\"complete\":" + (frame.complete ? "true" : "false") +
            ",\"language\":" + quote(frame.language) + ",\"stage\":" + quote(frame.stage) +
            ",\"mode\":" + std::to_string(frame.mode) +
            ",\"pendingLabel\":" + quote(frame.pendingLabel) + ",\"selectedItemId\":" + quote(frame.selectedItemId) +
            ",\"canBack\":" + (frame.active ? "true" : "false") + ",\"canClose\":" + (frame.active ? "true" : "false") + ",\"items\":[";
        for (size_t i = 0; i < frame.items.size(); ++i)
        {
            const auto &item = frame.items[i]; if (i) out += ',';
            out += "{\"id\":" + quote(item.id) + ",\"kind\":" + quote(item.kind) + ",\"label\":" + quote(item.label) + ",\"source\":" + quote(item.source) + ",\"picture\":" + quote(item.picture) +
                ",\"enabled\":" + (item.enabled ? "true" : "false") + ",\"cooldown\":" +
                std::to_string(std::isfinite(item.cooldown) ? std::clamp(item.cooldown, 0.0, 1.0) : 0.0) + "}";
        }
        out += "],\"offered\":[";
        for (size_t i = 0; i < frame.offered.size(); ++i) out += (i ? "," : "") + quote(frame.offered[i]);
        out += "],\"here\":" + (frame.herePicture.empty() ? std::string("null") :
            "{\"picture\":" + quote(frame.herePicture) + ",\"label\":" + quote(frame.hereLabel) + "}");
        const auto receiptJson = [&](const Receipt &receipt) {
            const auto &r = receipt.request;
            return "{\"serial\":" + std::to_string(receipt.serial) +
                ",\"scene\":" + std::to_string(r.scene) + ",\"revision\":" + std::to_string(r.revision) +
                ",\"activity\":" + std::to_string(r.activity) + ",\"id\":" + quote(r.id) +
                ",\"accepted\":" + (receipt.accepted ? "true" : "false") + ",\"reason\":" + quote(receipt.reason) + "}";
        };
        out += ",\"activation\":" + receiptJson(receipt_) + ",\"activations\":[";
        for (size_t i = 0; i < receipts_.size(); ++i) out += (i ? "," : "") + receiptJson(receipts_[i]);
        return out + "]}";
    }
    std::string json() const { return jsonFor(frame_); }
    // Everything json() reads (HudChannel, android_ui.hpp): every change of the receipts advances
    // the receipt serial, or the activity generation (resetActivity).
    auto publicationKey() const { return std::tie(frame_, scene_, revision_, activity_, receipt_.serial); }
};
inline LandCommandsState landCommands;
// When the touch HUD asks the scripts for the menu again (AndroidLandRefresh, which runs the
// original BLI_SetPossibleCommands). As in the original land interface, nothing polls: the list
// is rebuilt when BIManSign is told it changed (BI_MSG_CARE_COMMANDLIST_UPDATE from
// BLI_RefreshCommandMenu / LI_CareUpdateCommandList, MSG_BATTLE_LAND_START, the menu opening or
// closing, a command executed), and right before a tap acts (executeLandCommand). Besides that
// invalidation, only this presentation's own gating changes the published menu: readiness (a
// dialogue, an interface, loading) and the land interface entity itself.
class LandRefreshGate
{
    bool ready_ = false, forced_ = true, fight_ = false;
    uint64_t owner_ = 0;

public:
    // invalidated: the land interface's own "list changed" flag, taken this iteration. fight: the
    // hero's fight mode, which the scripts' choice of commands depends on (no transitions in a
    // fight) and which the original asks only when its menu opens: the touch HUD shows the
    // transitions button before that.
    bool due(bool ready, uint64_t owner, bool invalidated, bool fight)
    {
        const bool result = forced_ || invalidated || ready != ready_ || owner != owner_ || fight != fight_;
        ready_ = ready;
        owner_ = owner;
        fight_ = fight;
        forced_ = false;
        return result;
    }
    // The menu was changed natively (closeLandCommands): the next poll asks.
    void force() { forced_ = true; }
};
inline LandRefreshGate landRefresh;
// Defined by battle_interface. All entry points run on the game thread and
// resolve the live entity on each call; no retained pointers cross a frame.
// The main loop's entry: refreshes when landRefresh says so.
void pollLandCommands(bool ready);
// Asks the scripts now.
void refreshLandCommands(bool ready);
bool executeLandCommand(const LandCommandsRequest &request);
void closeLandCommands();
} // namespace storm::android
