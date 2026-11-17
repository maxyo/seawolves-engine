#pragma once
#include <optional>
#ifdef __ANDROID__
#include <SDL2/SDL.h>
#include <SDL2/SDL_system.h>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <jni.h>
#include <set>
#include <spdlog/spdlog.h>
#include <android/log.h>
#include <sys/system_properties.h>
#include <string>
#include <vector>
#include "android_hud_channel.hpp"
#include "android_ship_telemetry.hpp"
#include "android_spyglass.hpp"
#include "android_sea_navigator.hpp"
#include "android_sea_orders.hpp"
#include "android_sea_quick_action.hpp"
#include "android_land_commands.hpp"
#include "android_land_vitals.hpp"
#include "android_worldmap.hpp"
#include "android_interface_presentation.hpp"
#include "android_ui_sink.hpp"
#include "scene_load_status.hpp"

namespace storm::android
{
// All native state is accessed only by the game thread. Java marshals its UI queue.
inline std::string mobileScreen;
// The engine's plain text as a JSON string (what the text channels carry), control characters escaped.
inline std::string uiJsonText(std::string_view text)
{
    std::string out = "\"";
    for (const unsigned char c : text)
    {
        if (c == '"' || c == '\\') { out += '\\'; out += char(c); }
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else if (c == '\t') out += "\\t";
        else if (c < 32) { char code[8]; std::snprintf(code, sizeof code, "\\u%04x", c); out += code; }
        else out += char(c);
    }
    return out + '"';
}
// What a channel's data lost on its way to the UI (cut, replaced, not published): logged once
// per channel and reason, so a mismatch between the scripts and the UI is never silent and
// never floods the log.
inline void logChannelDefect(const char *channel, const std::string &reason)
{
    static std::set<std::string> logged;
    if (logged.insert(std::string(channel) + '\x1f' + reason).second)
        spdlog::error("UI channel {}: {} (logged once)", channel, reason);
}
inline uint64_t mobileScreenRevision = 1;
inline InterfacePresentation interfacePresentation;
inline std::deque<std::string> dialogueActions;
// A widget action of the touch UI for the open interface (XINTERFACE::DoControl applies it). `value`
// is the wire its node reads ("select@<opening>:<row>", storm::ui::WidgetWire); `request` is the UI's
// request number: every action is answered (ui::answer), 0 = nobody waits for the answer.
struct UiWidgetAction { std::string screen, node, value; int index; uint64_t request = 0; };
inline std::deque<UiWidgetAction> uiWidgetActions;
inline std::string contextAction, requestedContextAction;
// Opt-in, bounded diagnostics for the Android interaction bridge. Game thread only.
inline void traceContextAction(const char *stage, const std::string &action, const std::string &detail)
{
    // adb shell setprop log.tag.StormInput DEBUG, read once (before the game starts).
    static const bool enabled = [] {
        char setting[PROP_VALUE_MAX] = {};
        __system_property_get("log.tag.StormInput", setting);
        return std::string_view(setting) == "DEBUG";
    }();
    static unsigned budget = 192;
    if (!enabled || !budget) return;
    --budget;
    __android_log_print(ANDROID_LOG_DEBUG, "StormInput", "context %s action=%s %s",
                        stage, action.c_str(), detail.c_str());
}
inline int cinematicState = -1;
// The game's death picture is up (xinterface info_handler.cpp): the death screen's choice may
// end it at once ("widget:game_over\tEND", main.cpp).
inline bool gameOverShown = false;
// Gameplay state: channel "gameplay" of the UI host, and the activity's copy for its own controls.
inline HudChannel<decltype(std::declval<const GameplayState &>().publicationKey())> gameplayChannel;
inline void sendGameplayState(const std::string &json)
{
    ui::state("gameplay", json);
    // The activity keeps the gameplay mode for its own controls (look surface, virtual keys).
    auto *env = static_cast<JNIEnv *>(SDL_AndroidGetJNIEnv());
    auto activity = static_cast<jobject>(SDL_AndroidGetActivity());
    if (!env || !activity) return;
    auto type = env->GetObjectClass(activity);
    auto method = type ? env->GetMethodID(type, "showGameplayState", "([B)V") : nullptr;
    auto bytes = env->NewByteArray(static_cast<jsize>(json.size()));
    if (bytes) env->SetByteArrayRegion(bytes, 0, static_cast<jsize>(json.size()), reinterpret_cast<const jbyte *>(json.data()));
    if (method && bytes) env->CallVoidMethod(activity, method, bytes);
    else spdlog::error("gameplay state: GameActivity.showGameplayState is missing");
    if (env->ExceptionCheck()) { env->ExceptionDescribe(); env->ExceptionClear(); }
    if (bytes) env->DeleteLocalRef(bytes);
    if (type) env->DeleteLocalRef(type);
    env->DeleteLocalRef(activity);
}
// The JSON is built only for a new gameplay revision.
inline void publishGameplayState(const GameplayState &state)
{
    const auto *json = gameplayChannel.publish(state.publicationKey(), [&] {
        auto json = state.json();
        if (json.size() <= 8192) return json;
        spdlog::error("gameplay state of {} bytes is over the 8192-byte bound: not published", json.size());
        return gameplayChannel.json(); // the last one sent: nothing to publish
    });
    if (json) sendGameplayState(*json);
}
// Live ship telemetry (channel "ship"). Independent from action revisions; rendering
// acknowledgements are never inherited by a new UI host.
inline uint64_t shipTelemetryHost = 0;
inline void publishShipTelemetry()
{
    if (shipTelemetryHost != ui::generation())
    {
        shipTelemetryHost = ui::generation();
        shipTelemetry.resetActivity(); // a new UI host: nothing drawn or answered by the old one counts
    }
    const auto tick = SDL_GetTicks();
    if (const auto json = shipTelemetry.publication(tick))
    {
        if (json->size() > 8192)
        {
            logChannelDefect("ship", "state over the 8192-byte bound: not published");
            return;
        }
        ui::state("ship", *json);
        shipTelemetry.published(*json, tick);
    }
}
inline uint64_t worldMapHost = 0;
inline uint32_t worldMapPublishedAt = 0;
inline uint64_t worldMapPublishedRevision = UINT64_MAX;
inline void publishWorldMapHud()
{
    if(worldMapHost!=ui::generation()){worldMapHost=ui::generation();worldMapHud.resetActivity();worldMapPublishedAt=0;}
    const auto tick=SDL_GetTicks();
    if(tick-worldMapPublishedAt>=200 || worldMapPublishedRevision!=worldMapHud.revision())
    {
        ui::state("worldMap",worldMapHud.json());
        worldMapPublishedAt=tick;worldMapPublishedRevision=worldMapHud.revision();
    }
}
inline uint64_t seaOrdersHost = 0;
inline HudChannel<decltype(seaOrders.publicationKey())> seaOrdersChannel;
inline void publishSeaOrders()
{
    if (seaOrdersHost != ui::generation()) { seaOrdersHost = ui::generation(); seaOrdersChannel.reset(); seaOrders.resetActivity(); }
    if (const auto *json = seaOrdersChannel.publish(seaOrders.publicationKey(), [] {
            if (const char *defect = seaOrders.defect()) logChannelDefect("seaOrders", defect);
            return seaOrders.json();
        }))
        ui::state("seaOrders", *json);
}
// The sea quick action (U1). A new UI host gets a new activity generation: nothing drawn or
// tapped by the old one counts.
inline uint64_t seaQuickActionHost = 0;
inline HudChannel<decltype(seaQuickAction.publicationKey())> seaQuickActionChannel;
inline void publishSeaQuickAction()
{
    if (seaQuickActionHost != ui::generation()) { seaQuickActionHost = ui::generation(); seaQuickActionChannel.reset(); seaQuickAction.resetActivity(); }
    if (const auto *json = seaQuickActionChannel.publish(seaQuickAction.publicationKey(), [] { return seaQuickAction.json(); }))
        ui::state("seaQuickAction", *json);
}
inline uint64_t landCommandsHost = 0;
inline HudChannel<decltype(landCommands.publicationKey())> landCommandsChannel;
inline void publishLandCommands()
{
    if (landCommandsHost != ui::generation()) { landCommandsHost = ui::generation(); landCommandsChannel.reset(); landCommands.resetActivity(); }
    if (const auto *json = landCommandsChannel.publish(landCommands.publicationKey(), [] {
            if (const char *defect = landCommands.defect()) logChannelDefect("landCommands", defect);
            return landCommands.json();
        }))
        ui::state("landCommands", *json);
}
// The land hero's bars and status (android_land_vitals.hpp), nothing when the engine draws no land
// hero; a change goes at most every 100 ms, the hero's disappearance at once. `picture` gives the
// JSON of a portrait's picture (battle_interface land_status.hpp); the JSON is built only for a
// changed state.
inline HudChannel<std::tuple<const std::optional<LandVitals> &>> vitalsChannel;
inline uint32_t lastVitalsTick=0;
template <class Picture> inline void publishVitals(const std::optional<LandVitals> &vitals, Picture &&picture) {
    const auto now=SDL_GetTicks();
    const auto key=std::tie(vitals);
    if(vitalsChannel.same(key) || (vitals && now-lastVitalsTick<100))return;
    const auto *json=vitalsChannel.publish(key,[&]{return vitals?landVitalsJson(*vitals,picture):std::string();});
    if(!json)return;
    lastVitalsTick=now;
    // Nothing at all when the engine draws no land hero: JSON null for the UI.
    ui::state("vitals",json->empty()?std::string("null"):*json);
}
inline void publishVitals(std::nullopt_t) {
    publishVitals(std::optional<LandVitals>(), [](const LandPortrait &) { return std::string("null"); });
}
inline std::string lastWorldInfo;
inline void publishWorldInfo(const std::string &text) {
    if (lastWorldInfo == text) return;
    lastWorldInfo=text;
    ui::state("worldInfo",uiJsonText(text)); // the engine's text as the JSON string the channel carries
}
// A passing line of the game's event log (Log_Info and alike): the portable UI shows it as
// a toast (the HUD's top-centre log over a free game). Every line goes; the UI counts a
// repeat of a shown one («×2») instead of stacking it.
// > 0 while the engine runs a script action whose log line the HUD already shows otherwise
// (the time scale: its button carries the multiplier).
inline int gameMessagesMuted = 0;
// `target`: where a tap on the toast leads ("" = nowhere; game_message_target.hpp tags it at the
// script function that wrote the line).
inline void publishGameMessage(const std::string &text, const std::string &target = {}) {
    if (gameMessagesMuted > 0 || text.empty()) return;
    ui::toast(text, target);
}
// Set from the game thread's command queue ("graphics:reload", sent by the platform host after
// the settings screen stored new renderer preferences); the renderer re-applies the graphics
// effects from the preferences at its next frame (renderer.cpp).
inline std::atomic<bool> graphicsSettingsChanged{false};
// A debuggable build of the app (ApplicationInfo.FLAG_DEBUGGABLE: the debug APK): developer looks and notices.
// False when there is no activity to ask.
inline bool debuggable() {
    auto *env=static_cast<JNIEnv *>(SDL_AndroidGetJNIEnv());
    auto activity=static_cast<jobject>(SDL_AndroidGetActivity());
    if (!env || !activity) return false;
    bool result=false;
    auto type=env->GetObjectClass(activity);
    auto method=type ? env->GetMethodID(type,"getApplicationInfo","()Landroid/content/pm/ApplicationInfo;") : nullptr;
    auto info=method ? env->CallObjectMethod(activity,method) : nullptr;
    auto info_type=info ? env->GetObjectClass(info) : nullptr;
    auto flags=info_type ? env->GetFieldID(info_type,"flags","I") : nullptr;
    constexpr jint kFlagDebuggable=2; // ApplicationInfo.FLAG_DEBUGGABLE
    if (flags) result=(env->GetIntField(info,flags)&kFlagDebuggable)!=0;
    if (env->ExceptionCheck()) { env->ExceptionDescribe(); env->ExceptionClear(); result=false; }
    if (info_type) env->DeleteLocalRef(info_type);
    if (info) env->DeleteLocalRef(info);
    if (type) env->DeleteLocalRef(type);
    env->DeleteLocalRef(activity);
    return result;
}
inline int graphicsSetting(const char *key, int fallback) {
    auto *env=static_cast<JNIEnv *>(SDL_AndroidGetJNIEnv());
    auto activity=static_cast<jobject>(SDL_AndroidGetActivity());
    if (!env || !activity) return fallback;
    auto type=env->GetObjectClass(activity);
    auto method=type ? env->GetMethodID(type,"getGraphicsSetting","(Ljava/lang/String;I)I") : nullptr;
    auto name=env->NewStringUTF(key);
    int value=fallback;
    if (method && name) value=env->CallIntMethod(activity,method,name,static_cast<jint>(fallback));
    if (env->ExceptionCheck()) { env->ExceptionDescribe(); env->ExceptionClear(); value=fallback; }
    if (name) env->DeleteLocalRef(name);
    if (type) env->DeleteLocalRef(type);
    env->DeleteLocalRef(activity);
    return value;
}
// The automatic graphics preset chose `preset` (renderer auto_preset.hpp, measurement `version`, `report` its
// 0: the preset's own): GameActivity.autoPresetChosen stores it unless the player chose a preset meanwhile, tells the
// player in the game's `language` (the string service's, as the interfaces' models carry it) and asks for
// "graphics:reload". False (logged) when the host could not be told.
inline bool reportAutoPreset(int preset, int render_height, int version, const std::string &report, const std::string &language) {
    auto *env=static_cast<JNIEnv *>(SDL_AndroidGetJNIEnv());
    auto activity=static_cast<jobject>(SDL_AndroidGetActivity());
    if (!env || !activity) { spdlog::error("graphics auto preset: no game activity to report preset {} to", preset); return false; }
    auto type=env->GetObjectClass(activity);
    auto method=type ? env->GetMethodID(type,"autoPresetChosen","(IIILjava/lang/String;Ljava/lang/String;)V") : nullptr;
    auto text=env->NewStringUTF(report.c_str());
    auto lang=env->NewStringUTF(language.c_str());
    bool reported=false;
    if (method && text && lang) { env->CallVoidMethod(activity,method,static_cast<jint>(preset),static_cast<jint>(render_height),static_cast<jint>(version),text,lang); reported=true; }
    if (env->ExceptionCheck()) { env->ExceptionDescribe(); env->ExceptionClear(); reported=false; }
    if (!reported) spdlog::error("graphics auto preset: GameActivity.autoPresetChosen failed; preset {} ({}-line scene) is not stored", preset, render_height);
    if (text) env->DeleteLocalRef(text);
    if (lang) env->DeleteLocalRef(lang);
    if (type) env->DeleteLocalRef(type);
    env->DeleteLocalRef(activity);
    return reported;
}
// The graphics API the game's device runs and why (renderer_bgfx frame_owner.hpp RenderBackendChoice), after each
// device creation: GameActivity.renderBackendChosen keeps it for the graphics settings screen. Logged when the host
// could not be told (the "render backend:" log line stays the record either way).
inline void reportRenderBackend(const std::string &backend, const std::string &reason) {
    auto *env=static_cast<JNIEnv *>(SDL_AndroidGetJNIEnv());
    auto activity=static_cast<jobject>(SDL_AndroidGetActivity());
    if (!env || !activity) { spdlog::error("render backend: no game activity to report {} to", backend); return; }
    auto type=env->GetObjectClass(activity);
    auto method=type ? env->GetMethodID(type,"renderBackendChosen","(Ljava/lang/String;Ljava/lang/String;)V") : nullptr;
    auto name=env->NewStringUTF(backend.c_str());
    auto why=env->NewStringUTF(reason.c_str());
    bool reported=false;
    if (method && name && why) { env->CallVoidMethod(activity,method,name,why); reported=true; }
    if (env->ExceptionCheck()) { env->ExceptionDescribe(); env->ExceptionClear(); reported=false; }
    if (!reported) spdlog::error("render backend: GameActivity.renderBackendChosen failed; the settings do not show {}", backend);
    if (name) env->DeleteLocalRef(name);
    if (why) env->DeleteLocalRef(why);
    if (type) env->DeleteLocalRef(type);
    env->DeleteLocalRef(activity);
}
// The door the hero stands at (the touch HUD's bottom hint): where its reload locator leads and
// whether it is locked now (the game's "Closed" quick action), null = at no door with a name.
inline HudChannel<std::tuple<const std::string &, const bool &>> reloadPlaceChannel;
inline void publishReloadPlace(const std::string &place, bool locked) {
    const auto *json = reloadPlaceChannel.publish(std::tie(place, locked), [&] {
        return place.empty() ? std::string("null") : "{\"place\":" + uiJsonText(place) + ",\"locked\":" + (locked ? "true" : "false") + "}";
    });
    if (json) ui::state("reloadPlace", *json);
}
inline void publishContextAction(const std::string &name) {
    if (contextAction == name) return;
    traceContextAction("publish", name, "previous=" + contextAction);
    contextAction = name; requestedContextAction.clear();
    ui::state("contextAction",uiJsonText(name));
}
// The host starts this engine straight at the game's main menu (GameActivity.startsAtMainMenu,
// after the pause menu's exit): until the first interface opens, the videos are broken off
// through the game's own video break (CAviPlayer, ICancel as a tap sends).
inline bool startAtMainMenu = false;
inline void readStartAtMainMenu() {
    auto *env=static_cast<JNIEnv *>(SDL_AndroidGetJNIEnv());
    auto activity=static_cast<jobject>(SDL_AndroidGetActivity());
    if (!env || !activity) { spdlog::error("Start at the main menu: no game activity to ask"); return; }
    auto type=env->GetObjectClass(activity);
    auto method=type ? env->GetMethodID(type,"startsAtMainMenu","()Z") : nullptr;
    if (method) startAtMainMenu=env->CallBooleanMethod(activity,method);
    if (env->ExceptionCheck() || !method) {
        env->ExceptionClear(); startAtMainMenu=false;
        spdlog::error("Start at the main menu: GameActivity.startsAtMainMenu() is unavailable; the game starts with its videos");
    }
    if (type) env->DeleteLocalRef(type);
    env->DeleteLocalRef(activity);
    if (startAtMainMenu) spdlog::info("Start at the main menu: the videos before it are skipped");
}
inline int loadingState = -1;
inline void publishLoading(bool active) {
    if (loadingState == static_cast<int>(active)) return;
    loadingState=active;
    ui::loading(active);
    auto *env=static_cast<JNIEnv *>(SDL_AndroidGetJNIEnv());
    auto activity=static_cast<jobject>(SDL_AndroidGetActivity());
    if (!env || !activity) return;
    auto type=env->GetObjectClass(activity);
    auto method=type ? env->GetMethodID(type,"showLoading","(Z)V") : nullptr;
    if (method) env->CallVoidMethod(activity,method,static_cast<jboolean>(active));
    if (env->ExceptionCheck()) { env->ExceptionDescribe(); env->ExceptionClear(); }
    if (type) env->DeleteLocalRef(type);
    env->DeleteLocalRef(activity);
}
// Loading overlay consumer is supplied by the app. Synchronous cancel acknowledgement sets its restart reason
// before the engine loop exits; the Activity marshals only view updates to its UI thread.
// Selective mailbox read: no gameplay, shortcut, settings or stale-generation
// command is consumed. Missing app support is distinct from an empty mailbox.
inline std::optional<bool> pollLocationLoadCancel(uint64_t generation)
{
    auto *env = static_cast<JNIEnv *>(SDL_AndroidGetJNIEnv());
    auto activity = static_cast<jobject>(SDL_AndroidGetActivity());
    if (!env || !activity) return std::nullopt;
    const auto type = env->GetObjectClass(activity);
    const auto method = type ? env->GetMethodID(type, "pollLocationLoadCancel", "(J)Z") : nullptr;
    std::optional<bool> result;
    if (method) {
        const auto value = env->CallBooleanMethod(activity, method, static_cast<jlong>(generation));
        if (!env->ExceptionCheck()) result = value == JNI_TRUE;
    }
    if (env->ExceptionCheck()) env->ExceptionClear();
    if (type) env->DeleteLocalRef(type);
    env->DeleteLocalRef(activity);
    return result;
}
inline bool publishLocationReadiness(const char *ack = "", bool cancelled = false) {
    const auto r = render::GetSceneLoadStatus();
    const std::string json = "{\"generation\":" + std::to_string(r.generation) +
        ",\"active\":" + ((r.active && !cancelled) ? "true" : "false") + ",\"state\":" + uiJsonText(cancelled ? "cancelled" : r.state) +
        ",\"retryable\":" + ((r.retryable && !cancelled) ? "true" : "false") + ",\"textures\":" + std::to_string(r.textures) + ",\"relief\":" + std::to_string(r.relief) +
        ",\"shadows\":" + std::to_string(r.shadows) + ",\"failed\":" + std::to_string(r.failed) +
        ",\"detail\":" + uiJsonText(r.detail) + ",\"phase\":" + uiJsonText(r.phase) +
        ",\"actionAck\":" + uiJsonText(ack) + "}";
    // A location load under way (or being cancelled) covers the game like the loading screen.
    ui::locationLoad((r.active && !cancelled) || cancelled);
    static std::string previous;
    if (previous == json && !cancelled) return true;
    auto *env = static_cast<JNIEnv *>(SDL_AndroidGetJNIEnv());
    auto activity = static_cast<jobject>(SDL_AndroidGetActivity());
    if (!env || !activity) return false;
    auto type = env->GetObjectClass(activity);
    auto method = type ? env->GetMethodID(type, "onLocationReadiness", "(Ljava/lang/String;)V") : nullptr;
    bool delivered = false;
    if (method) {
        auto text = env->NewStringUTF(json.c_str());
        if (text) {
            env->CallVoidMethod(activity, method, text);
            delivered = !env->ExceptionCheck();
            env->DeleteLocalRef(text);
        }
    }
    if (env->ExceptionCheck()) env->ExceptionClear(); // older app: no consumer, never permission to reveal
    if (type) env->DeleteLocalRef(type);
    env->DeleteLocalRef(activity);
    if (delivered) previous = json;
    return delivered;
}
// A state channel of the portable UI (JSON as is, "null" = nothing), for channels without a
// publisher of their own here.
inline void publishUiState(const char *channel, const std::string &json) {
    ui::state(channel,json);
}
// Channel "spyglass" (android_spyglass.hpp): sent through the generic state channel when it changes.
inline HudChannel<decltype(spyglass.publicationKey())> spyglassChannel;
inline void publishSpyglass() {
    if(const auto *json=spyglassChannel.publish(spyglass.publicationKey(),[]{return spyglass.json();}))
        publishUiState("spyglass",*json);
}
// Channel "video": {"fullscreen": true} while a game video plays over the whole screen (the
// scripts' StartVideo player), null when it ends. A player embedded in an interface node
// (xi_video_rect, SetShowVideo(false)) never sets it: its screen stays. Back is then the
// game's Cancel to the video (main.cpp).
inline const void *fullscreenVideoOwner = nullptr;
inline void publishFullscreenVideo(const void *owner, bool active) {
    if (active) {
        if (fullscreenVideoOwner == owner) return;
        fullscreenVideoOwner = owner;
        publishUiState("video", "{\"fullscreen\":true}");
    } else if (fullscreenVideoOwner == owner) {
        fullscreenVideoOwner = nullptr;
        publishUiState("video", "null");
    }
}
inline bool fullscreenVideo() { return fullscreenVideoOwner != nullptr; }
inline void publishCinematic(bool active) {
    if (cinematicState == static_cast<int>(active)) return;
    cinematicState=active;
    ui::cinematic(active);
    auto *env=static_cast<JNIEnv *>(SDL_AndroidGetJNIEnv());
    auto activity=static_cast<jobject>(SDL_AndroidGetActivity());
    if (!env || !activity) return;
    auto type=env->GetObjectClass(activity);
    auto method=type ? env->GetMethodID(type,"showCinematic","(Z)V") : nullptr;
    if (method) env->CallVoidMethod(activity,method,static_cast<jboolean>(active));
    if (env->ExceptionCheck()) { env->ExceptionDescribe(); env->ExceptionClear(); }
    if (type) env->DeleteLocalRef(type);
    env->DeleteLocalRef(activity);
}


inline void showMobileScreen(const std::string &name, double release_ms = 0.0, double load_ms = 0.0)
{
    auto screen = name;
    std::replace(screen.begin(), screen.end(), '\\', '/');
    screen = screen.substr(screen.find_last_of('/') + 1);
    const auto suffix = screen.find_last_of('.');
    if (suffix != std::string::npos) screen.resize(suffix);
    for (auto &c : screen) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    mobileScreen = screen;
    if (startAtMainMenu && !screen.empty()) {
        startAtMainMenu = false;
        spdlog::info("Start at the main menu: interface {} opened", screen);
    }
    ++mobileScreenRevision;
    interfacePresentation.begin(mobileScreenRevision, !screen.empty(), interfacePresentation.negotiated());
    for (const auto &action : uiWidgetActions) // queued for the interface that went away
        ui::answer(action.request, false, "the interface changed before it was applied");
    uiWidgetActions.clear();
    ui::screen(screen, mobileScreenRevision, release_ms, load_ms);
    auto *env = static_cast<JNIEnv *>(SDL_AndroidGetJNIEnv());
    auto activity = static_cast<jobject>(SDL_AndroidGetActivity());
    if (!env || !activity) return;
    const auto type = env->GetObjectClass(activity);
    // The activity ships with this engine (tools/android/app_contract.json): a missing method is an error.
    const auto method = type ? env->GetMethodID(type, "showMobileScreen", "(Ljava/lang/String;J)V") : nullptr;
    if (!method)
    {
        if (env->ExceptionCheck()) env->ExceptionClear();
        __android_log_print(ANDROID_LOG_ERROR, "StormUI", "the activity has no showMobileScreen(String, long): interface %s not reported",
                            screen.c_str());
    }
    interfacePresentation.begin(mobileScreenRevision, !screen.empty(), method != nullptr);
    const auto value = env->NewStringUTF(screen.c_str());
    if (method && value) env->CallVoidMethod(activity, method, value, static_cast<jlong>(mobileScreenRevision));
    if (env->ExceptionCheck()) { env->ExceptionDescribe(); env->ExceptionClear(); }
    if (value) env->DeleteLocalRef(value);
    if (type) env->DeleteLocalRef(type);
    env->DeleteLocalRef(activity);
}

inline uint32_t mobileShipHudMask()
{
    if (!mobileScreen.empty() || loadingState != 0 || cinematicState == 1) return 0;
    return shipTelemetry.mask();
}

inline std::vector<std::string> pollMobileActions()
{
    std::vector<std::string> result;
    auto *env = static_cast<JNIEnv *>(SDL_AndroidGetJNIEnv());
    auto activity = static_cast<jobject>(SDL_AndroidGetActivity());
    if (!env || !activity) return result;
    // Polled every engine frame: the method id is looked up once (the activity class ships with
    // this engine and stays loaded; tools/android/app_contract.json). Missing is an error.
    static const jmethodID method = [&] {
        const auto type = env->GetObjectClass(activity);
        const auto found = type ? env->GetMethodID(type, "pollMobileAction", "()[B") : nullptr;
        if (!found)
        {
            if (env->ExceptionCheck()) env->ExceptionClear();
            spdlog::error("GameActivity.pollMobileAction()[B is missing: no UI action of the activity reaches the engine");
        }
        if (type) env->DeleteLocalRef(type);
        return found;
    }();
    if (method)
    {
        for (int i = 0; i < 32 && !env->ExceptionCheck(); ++i)
        {
            const auto value = static_cast<jbyteArray>(env->CallObjectMethod(activity, method));
            if (!value) break;
            const auto size = env->GetArrayLength(value);
            if (size <= 8192)
            {
                std::string text(size, '\0');
                env->GetByteArrayRegion(value, 0, size, reinterpret_cast<jbyte *>(text.data()));
                if (!env->ExceptionCheck()) result.push_back(std::move(text));
            }
            else
                logChannelDefect("mailbox", "an action over 8192 bytes is dropped");
            env->DeleteLocalRef(value);
        }
    }
    if (env->ExceptionCheck()) { env->ExceptionDescribe(); env->ExceptionClear(); }
    env->DeleteLocalRef(activity);
    return result;
}
} // namespace storm::android
#endif
