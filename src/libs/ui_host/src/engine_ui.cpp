// The game's portable UI host: one storm::ui::Host in the engine process, on the game
// thread, drawn by the bgfx backend in the frame owner's UI views (after the game picture,
// native resolution). See ../README.md.
//
// Engine code publishes states and interface models through platform/android_ui_sink.hpp;
// they reach the host directly. The Java activity keeps what is Android's: it routes each
// pointer at pointer-down (WantsTouch), sends the pointers the UI takes, its presentation
// state and the system insets through the EngineUi natives below (queued, applied at the
// top of the next engine frame), and receives what only Android can do (pause dialog,
// shortcuts mailbox, system editor, About).
#include "logging.hpp"
#include "platform/android_ui.hpp"
#include "platform/android_ui_sink.hpp"
#include "platform/display_options.hpp"

#include "storm_ui/asset_files.h"
#include "storm_ui/backend_bgfx.h"
#include "storm_ui/interface_model.h"
#include "storm_ui/ui.h"

#include <storm/render/frame_owner.hpp>

#include <SDL2/SDL.h>
#include <SDL2/SDL_system.h>
#include <android/asset_manager_jni.h>
#include <android/log.h>
#include <jni.h>
#include <sys/system_properties.h>
#include <time.h>

#include <array>
#include <cstdio>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace storm::android::ui
{
namespace
{

constexpr const char *kTag = "StormUi";
// Interface screens close one menu a frame before they open the next: the interface is kept
// over a gap this short, so the game does not flash between them.
constexpr uint32_t kCloseGraceMs = 150;
// Frame owner client order: after the renderer's own clients.
constexpr int kClientOrder = 1000;

// The clock of MotionEvent times (SystemClock.uptimeMillis is CLOCK_MONOTONIC), in ms.
double MonotonicMs()
{
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    return now.tv_sec * 1000.0 + now.tv_nsec / 1e6;
}

// adb shell setprop log.tag.<tag> DEBUG. Read once per process (EngineUi's flags), before the
// first state: set it before the game starts.
bool Loggable(const char *tag)
{
    char value[PROP_VALUE_MAX] = {};
    const std::string name = std::string("log.tag.") + tag;
    __system_property_get(name.c_str(), value);
    return std::string_view(value) == "DEBUG" || std::string_view(value) == "VERBOSE";
}

// JNI call into the game activity (game thread). Missing methods are errors: the activity
// and this host ship together.
template <typename Call> void CallActivity(const char *name, const char *signature, Call call)
{
    auto *env = static_cast<JNIEnv *>(SDL_AndroidGetJNIEnv());
    auto activity = static_cast<jobject>(SDL_AndroidGetActivity());
    if (!env || !activity)
        return;
    const jclass type = env->GetObjectClass(activity);
    const jmethodID method = type ? env->GetMethodID(type, name, signature) : nullptr;
    if (method)
        call(env, activity, method);
    else
        __android_log_print(ANDROID_LOG_ERROR, kTag, "GameActivity.%s%s is missing", name, signature);
    if (env->ExceptionCheck())
    {
        env->ExceptionDescribe();
        env->ExceptionClear();
    }
    if (type)
        env->DeleteLocalRef(type);
    env->DeleteLocalRef(activity);
}

jstring Utf(JNIEnv *env, const std::string &text)
{
    return env->NewStringUTF(text.c_str());
}

std::string Text(JNIEnv *env, jstring value)
{
    if (!value)
        return {};
    const char *chars = env->GetStringUTFChars(value, nullptr);
    std::string text = chars ? chars : "";
    if (chars)
        env->ReleaseStringUTFChars(value, chars);
    return text;
}

std::string Bytes(JNIEnv *env, jbyteArray value)
{
    const jsize size = env->GetArrayLength(value);
    std::string text(static_cast<size_t>(size), '\0');
    env->GetByteArrayRegion(value, 0, size, reinterpret_cast<jbyte *>(text.data()));
    return text;
}

class EngineUi
{
  public:
    static EngineUi &Get()
    {
        static EngineUi instance;
        return instance;
    }

    // ---- game thread -------------------------------------------------------------

    void State(std::string_view channel, std::string json)
    {
        if (trace_states_)
            __android_log_print(ANDROID_LOG_DEBUG, "StormUiState", "%.*s %s", int(channel.size()), channel.data(), json.c_str());
        std::string name(channel);
        if (host_)
            host_->SetState(name, json);
        states_[name] = std::move(json); // replayed when a new host starts
    }

    void Screen(const std::string &screen, uint64_t revision, double release_ms, double load_ms)
    {
        if (screen.empty())
            __android_log_print(ANDROID_LOG_INFO, kTag, "interface %s closed (revision %llu)", screen_.c_str(), static_cast<unsigned long long>(revision));
        else // the game's part of a screen switch ("UI switch" follows with the UI's)
            __android_log_print(ANDROID_LOG_INFO, kTag,
                                "interface %s opened (revision %llu) %llu engine frames after the last tap: the game released the old one in %.0f ms and "
                                "loaded it in %.0f ms",
                                screen.c_str(), static_cast<unsigned long long>(revision), static_cast<unsigned long long>(loop_ - tap_loop_), release_ms,
                                load_ms);
        screen_ = screen;
        if (!screen.empty())
        {
            close_at_ = 0;
            presentation_.screen = screen;
            PublishPresentation();
            // Its model comes once the interface has loaded, seconds later at times: the UI
            // owns the touches from now and holds them for the screen (Host::ExpectInterface).
            if (host_)
                host_->ExpectInterface();
            return;
        }
        close_at_ = SDL_GetTicks() + kCloseGraceMs;
        if (close_at_ == 0)
            close_at_ = 1;
    }

    void Interface(storm::ui::InterfaceModel &&model)
    {
        if (screen_.empty())
            return; // a late model of a screen that closed
        Record(model);
        TraceInput("model", screen_ + " revision " + std::to_string(model.revision) + ", " + std::to_string(model.widgets.size()) + " widgets");
        if (host_)
            host_->SetInterface(model);
        interface_ = std::move(model);
        PublishInterfaceActive();
    }

    void GameMessage(std::string text, std::string target)
    {
        if (host_)
            host_->GameMessage(text, target);
        else
            __android_log_print(ANDROID_LOG_ERROR, kTag, "no UI host yet, game message not shown: %s", text.c_str());
    }

    // The engine's answer to a widget action. One the engine gives while the UI is still handling the
    // event that sent the action (OnAction) waits for the next Tick: the press has no busy mark yet.
    void Answer(uint64_t request, bool applied, std::string reason)
    {
        if (!request)
            return;
        TraceInput("answer", std::to_string(request) + (applied ? " applied" : " refused: " + reason));
        if (in_action_ || !host_)
        {
            answers_.push_back({request, applied, std::move(reason)});
            return;
        }
        host_->Answer(request, applied, reason);
    }

    void Loading(bool active)
    {
        // A load starts (the loading picture comes next, then the game thread blocks in it): no menu follows the one
        // that just closed, so it goes now instead of after its grace, which the blocked loop would only see after
        // the load's first frames (the title over the loading picture after «Продолжить»).
        if (active && close_at_)
        {
            __android_log_print(ANDROID_LOG_INFO, kTag, "interface closed at the start of a load (not after its %u ms grace)",
                                kCloseGraceMs);
            CloseNow();
        }
        Set(presentation_.loading, active);
    }
    void Cinematic(bool active)
    {
        Set(presentation_.cinematic, active);
    }
    void Dialogue(bool active)
    {
        Set(presentation_.dialogue, active);
    }
    void LocationLoad(bool blocks)
    {
        Set(presentation_.location_load, blocks);
    }

    // The closed screen's grace is over (Tick) or a load starts (Loading): the interface goes.
    void CloseNow()
    {
        close_at_ = 0;
        interface_.reset();
        if (host_)
            host_->CloseInterface();
        PublishInterfaceActive();
        presentation_.screen.clear(); // after the same grace: a menu that follows at once keeps it
        PublishPresentation();
    }

    void AlertString(const std::string &key)
    {
        if (host_)
            host_->AlertString(key);
        else
            __android_log_print(ANDROID_LOG_ERROR, kTag, "no UI host yet, alert not shown: %s", key.c_str());
    }

    void Tick()
    {
        ++loop_;
        if (!host_ && !failed_)
            Start();
        if (close_at_ && static_cast<int32_t>(SDL_GetTicks() - close_at_) >= 0)
            CloseNow();
        std::vector<Step> work;
        std::optional<bool> live;
        {
            const std::lock_guard lock(inbox_mutex_);
            work.swap(inbox_);
            live = std::exchange(live_, std::nullopt);
        }
        if (live)
        {
            presentation_.live = *live;
            presentation_.live_known = true;
            PublishPresentation();
        }
        if (!host_)
            return; // nothing of it applies to a host that is not there (touches of no UI)
        for (auto &[request, applied, reason] : std::exchange(answers_, {}))
            host_->Answer(request, applied, reason);
        const double now_ms = MonotonicMs();
        for (Step &step : work)
        {
            if (step.call)
            {
                step.call(*host_);
                continue;
            }
            // With the trace every sample: where, and how long ago it happened (input latency).
            if (trace_input_)
                TraceInput("touch", std::to_string(step.phase) + " " + std::to_string(int(step.x)) + "," + std::to_string(int(step.y)) + " sampled at " +
                                        std::to_string(int64_t(step.time_ms)) + " ms, age " + std::to_string(int(now_ms - step.time_ms)) + " ms");
            if (step.phase == static_cast<int>(storm::ui::TouchPhase::Up))
                tap_loop_ = loop_;
            // The sample's own time on the host's clock: how long ago it happened. The moves of
            // this iteration are samples: each pointer is placed once, below, for this frame.
            const auto phase = static_cast<storm::ui::TouchPhase>(step.phase);
            host_->Touch(phase == storm::ui::TouchPhase::Move ? storm::ui::TouchPhase::Sample : phase, step.pointer, step.x, step.y,
                         host_->Now() - (now_ms - step.time_ms) / 1000.0);
        }
        host_->PlaceTouches(host_->Now());
        PublishInterfaceActive();
    }

    // adb shell setprop log.tag.StormUiInput DEBUG (before the game starts): when touches and
    // the commands they produce reach the engine, in engine loop iterations (one per frame).
    void TraceInput(const char *what, const std::string &detail) const
    {
        if (trace_input_)
            __android_log_print(ANDROID_LOG_DEBUG, "StormUiInput", "loop %llu: %s %s", static_cast<unsigned long long>(loop_), what,
                                detail.c_str());
    }

    std::vector<std::string> TakeCommands()
    {
        std::vector<std::string> out;
        out.swap(commands_);
        slots_.clear();
        return out;
    }

    uint64_t Generation() const
    {
        return generation_;
    }

    void Shutdown()
    {
        if (auto *owner = storm::render::FrameOwner::Get(); owner && backend_)
        {
            owner->RemoveClient(backend_.get());
            if (trace_input_)
                owner->RemoveClient(&frame_trace_);
        }
        {
            const std::lock_guard lock(host_mutex_);
            host_.reset();
        }
        backend_.reset();
        files_.reset();
        if (auto *env = static_cast<JNIEnv *>(SDL_AndroidGetJNIEnv()); env && assets_)
            env->DeleteGlobalRef(assets_);
        assets_ = nullptr;
        failed_ = true; // no new host after this
    }

    // ---- platform thread (EngineUi natives) ---------------------------------------

    void Post(std::function<void(storm::ui::Host &)> call)
    {
        const std::lock_guard lock(inbox_mutex_);
        inbox_.push_back({std::move(call)});
    }

    // A pointer sample with its time (MotionEvent's, milliseconds of CLOCK_MONOTONIC). Every
    // sample is kept, the historical ones of a batched move too: drags and flings measure the
    // finger's speed from them (storm::ui TouchScroll).
    void Touch(int phase, int pointer, float x, float y, double time_ms)
    {
        const std::lock_guard lock(inbox_mutex_);
        inbox_.push_back({{}, phase, pointer, x, y, time_ms});
    }

    // Answered from the layout of the last frame (Host is thread-safe here).
    bool WantsTouch(float x, float y)
    {
        const std::lock_guard lock(host_mutex_);
        return host_ && host_->WantsTouch(x, y);
    }

    bool WantsBack()
    {
        const std::lock_guard lock(host_mutex_);
        return host_ && host_->WantsBack();
    }

    bool InterfaceActive() const
    {
        return interface_active_.load();
    }

    // A channel Android writes ("presentation", "appSettings"), from the activity's thread. It is
    // kept as well as posted: the activity writes some before the engine has a host (the settings
    // at start), and what is posted to no host is dropped; a new host gets the latest of each.
    void PlatformState(std::string channel, std::string json)
    {
        if (channel == "presentation")
        {
            // Android's part of the presentation: {"live": true|false}. The rest is the engine's
            // (Presentation): it reaches the UI in the frame it changes, not through this thread.
            bool on = json == R"({"live":true})", off = json == R"({"live":false})";
            if (!on && !off)
            {
                // Documented exception, until every application in use sends {"live"} only: an older
                // activity still sends the whole presentation; only its `live` is taken (said once).
                on = json.find(R"("live":true)") != std::string::npos;
                off = json.find(R"("live":false)") != std::string::npos;
                static bool said = false;
                if (on != off && !said)
                {
                    said = true;
                    __android_log_print(ANDROID_LOG_WARN, kTag, "presentation: the activity sends its old form; only \"live\" is taken: %s",
                                        json.c_str());
                }
            }
            if (on == off)
            {
                __android_log_print(ANDROID_LOG_ERROR, kTag, "presentation from the activity has no \"live\": %s", json.c_str());
                return;
            }
            const std::lock_guard lock(inbox_mutex_);
            live_ = on;
            return;
        }
        {
            const std::lock_guard lock(inbox_mutex_);
            platform_states_[channel] = json;
        }
        if (channel == "appSettings")
            DisplayOptionsFrom(json);
        Post([channel = std::move(channel), json = std::move(json)](storm::ui::Host &host) { host.SetState(channel, json); });
    }

    // «Размер интерфейса» and «Прозрачность кнопок» of the app's settings (platform/display_options.hpp), to the host.
    void DisplayOptionsFrom(const std::string &app_settings)
    {
        const storm::android::DisplayOptions options = storm::android::ReadDisplayOptions(app_settings);
        if (!options.problems.empty())
            __android_log_print(ANDROID_LOG_ERROR, kTag, "display options: %s", options.problems.c_str());
        const float scale = static_cast<float>(options.ui_scale) / 100.0f;
        const float opacity = 1.0f - static_cast<float>(options.hud_transparency) / 100.0f;
        const bool left_handed = options.left_handed != 0;
        if (shown_ui_scale_ != options.ui_scale || shown_hud_transparency_ != options.hud_transparency || shown_left_handed_ != options.left_handed)
            __android_log_print(ANDROID_LOG_INFO, kTag, "display options: UI size %d%%, HUD buttons %d%% see-through, %s", options.ui_scale,
                                options.hud_transparency, left_handed ? "left-handed HUD" : "right-handed HUD");
        shown_ui_scale_ = options.ui_scale;
        shown_hud_transparency_ = options.hud_transparency;
        shown_left_handed_ = options.left_handed;
        Post([scale, opacity, left_handed](storm::ui::Host &host) { host.SetDisplayOptions(scale, opacity, left_handed); });
    }

    // Safe-area insets in pixels; the host takes dp.
    void SafeArea(float left, float top, float right, float bottom)
    {
        Post([left, top, right, bottom](storm::ui::Host &host) {
            const auto *owner = storm::render::FrameOwner::Get();
            const float density = owner && owner->Surface().density > 0.0f ? owner->Surface().density : 1.0f;
            host.SetSafeArea(left / density, top / density, right / density, bottom / density);
        });
        const std::lock_guard lock(inbox_mutex_);
        safe_area_ = {left, top, right, bottom};
        has_safe_area_ = true;
    }

  private:
    void Start()
    {
        auto *owner = storm::render::FrameOwner::Get();
        if (!owner || !owner->DeviceReady())
            return; // not yet: the renderer starts first
        auto *env = static_cast<JNIEnv *>(SDL_AndroidGetJNIEnv());
        auto activity = static_cast<jobject>(SDL_AndroidGetActivity());
        if (!env || !activity)
            return;
        {
            const jclass type = env->GetObjectClass(activity);
            const jmethodID get_assets = env->GetMethodID(type, "getAssets", "()Landroid/content/res/AssetManager;");
            const jobject assets = env->CallObjectMethod(activity, get_assets);
            assets_ = env->NewGlobalRef(assets);
            env->DeleteLocalRef(assets);
            env->DeleteLocalRef(type);
            env->DeleteLocalRef(activity);
        }
        files_ = storm::ui::AssetFiles(AAssetManager_fromJava(env, assets_), "ui");
        std::string error;
        storm::ui::BgfxBackendOptions options;
        options.owner = owner;
        backend_ = storm::ui::BgfxBackend::Create(options, &error);
        const storm::render::SurfaceMetrics surface = owner->Surface();
        storm::ui::Config config;
        config.files = files_.get();
        config.backend = backend_.get();
        config.width = int(surface.width_px);
        config.height = int(surface.height_px);
        config.density = surface.density;
        config.game_root = std::filesystem::current_path().string(); // prepareGameDirectory
        config.show_errors = storm::android::debugBuild();
        config.log_taps = Loggable("StormUiTaps"); // device crawls: the tap map in the log (Config::log_taps)
        // The flicker check (Config::trace_actions): debug builds and device crawls report every action that showed a
        // transient frame ("UI flicker" in logcat and Logs/ui.log).
        config.trace_actions = config.show_errors || config.log_taps;
        // The UI's lines (what it showed, its errors) in Logs/ui.log, its errors in error.log too; logcat has them
        // from the UI already (tag StormUi).
        config.on_log = [logger = storm::logging::getOrCreateLogger("ui", spdlog::level::trace, false)](
                            storm::ui::LogLevel level, const std::string &line) {
            logger->log(level == storm::ui::LogLevel::Error     ? spdlog::level::err
                        : level == storm::ui::LogLevel::Warning ? spdlog::level::warn
                                                                : spdlog::level::info,
                        "{}", line);
        };
        config.on_action = [this](const storm::ui::Action &action) { OnAction(action); };
        config.on_text_request = [](const storm::ui::TextRequest &request) {
            CallActivity("onUiTextRequest", "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;ZZIIII)V",
                         [&](JNIEnv *env, jobject activity, jmethodID method) {
                             jstring token = Utf(env, request.token), text = Utf(env, request.text), title = Utf(env, request.title);
                             env->CallVoidMethod(activity, method, token, text, title, jboolean(request.numeric), jboolean(request.secret),
                                                 jint(request.x), jint(request.y), jint(request.width), jint(request.height));
                             env->DeleteLocalRef(token);
                             env->DeleteLocalRef(text);
                             env->DeleteLocalRef(title);
                         });
        };
        std::unique_ptr<storm::ui::Host> host = backend_ ? storm::ui::Host::Create(config, &error) : nullptr;
        if (!host)
        {
            // No other renderer exists for the game's screens: the player is told.
            failed_ = true;
            __android_log_print(ANDROID_LOG_ERROR, kTag, "UI host failed: %s", error.c_str());
            const std::string reason = "The UI runtime failed to start: " + error;
            CallActivity("uiFailed", "(Ljava/lang/String;)V", [&](JNIEnv *env, jobject activity, jmethodID method) {
                jstring text = Utf(env, reason);
                env->CallVoidMethod(activity, method, text);
                env->DeleteLocalRef(text);
            });
            backend_.reset();
            return;
        }
        {
            const std::lock_guard lock(host_mutex_);
            host_ = std::move(host);
        }
        ++generation_;
        if (trace_input_)
            owner->AddClient(&frame_trace_, kClientOrder + 1);
        backend_->AttachHost(host_.get());
        owner->AddClient(backend_.get(), kClientOrder); // OnSurfaceChanged: the host lays out at the surface's size and density
        for (const auto &[channel, json] : states_)
            host_->SetState(channel, json);
        std::map<std::string, std::string> platform;
        {
            const std::lock_guard lock(inbox_mutex_);
            platform = platform_states_;
        }
        for (const auto &[channel, json] : platform)
            host_->SetState(channel, json);
        if (const auto settings = platform.find("appSettings"); settings != platform.end())
        {
            const storm::android::DisplayOptions options = storm::android::ReadDisplayOptions(settings->second);
            host_->SetDisplayOptions(static_cast<float>(options.ui_scale) / 100.0f, 1.0f - static_cast<float>(options.hud_transparency) / 100.0f,
                                     options.left_handed != 0);
        }
        if (interface_)
            host_->SetInterface(*interface_);
        else if (!screen_.empty())
            host_->ExpectInterface();
        {
            const std::lock_guard lock(inbox_mutex_);
            if (has_safe_area_)
            {
                const auto [left, top, right, bottom] = safe_area_;
                inbox_.insert(inbox_.begin(), Step{[left, top, right, bottom](storm::ui::Host &host) {
                    const auto *frame_owner = storm::render::FrameOwner::Get();
                    const float density = frame_owner ? frame_owner->Surface().density : 1.0f;
                    host.SetSafeArea(left / density, top / density, right / density, bottom / density);
                }});
            }
        }
        __android_log_print(ANDROID_LOG_INFO, kTag, "UI host started in the engine frame (%ux%u, density %.2f)", surface.width_px,
                            surface.height_px, surface.density);
    }

    // What the UI asks for: engine commands stay in this process (read at the top of the next
    // loop iteration, or this one for input applied at its top); what is Android's goes to
    // the activity.
    void OnAction(const storm::ui::Action &action)
    {
        if (action.screen == "hud" && (action.verb == "shortcut" || action.verb == "presence"))
        {
            CallActivity("onUiHudInput", "(Ljava/lang/String;Ljava/lang/String;)V", [&](JNIEnv *env, jobject activity, jmethodID method) {
                jstring verb = Utf(env, action.verb), value = Utf(env, action.value);
                env->CallVoidMethod(activity, method, verb, value);
                env->DeleteLocalRef(verb);
                env->DeleteLocalRef(value);
            });
            return;
        }
        if (storm::ui::AnsweredAction(action))
        {
            // A widget of the open interface: the action itself goes to the interface (XINTERFACE::DoControl
            // applies and answers it), with the wire its node reads.
            in_action_ = true;
            TraceInput("action", action.screen + " " + action.node + " " + action.verb + " request " + std::to_string(action.request));
            if (action.screen != screen_)
                Answer(action.request, false, "the interface " + action.screen + " is not open");
            else
                storm::android::uiWidgetActions.push_back(
                    {action.screen, action.node, storm::ui::WidgetWire(action), action.index, action.request});
            in_action_ = false;
            return;
        }
        const std::string command = storm::ui::EngineCommand(action);
        TraceInput("action", action.screen + " " + action.node + " " + action.verb + " -> " + command);
        if (!command.empty())
        {
            if (action.screen == "hud" && !action.node.empty())
            {
                // A latest-value slot (stick position, coverage acknowledgement): only its newest command counts.
                const auto found = slots_.find(action.node);
                if (found != slots_.end())
                {
                    commands_[found->second] = command;
                    return;
                }
                slots_[action.node] = commands_.size();
            }
            commands_.push_back(command);
            return;
        }
        CallActivity("onUiAction", "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)V",
                     [&](JNIEnv *env, jobject activity, jmethodID method) {
                         jstring node = Utf(env, action.node), verb = Utf(env, action.verb), value = Utf(env, action.value);
                         env->CallVoidMethod(activity, method, node, verb, value);
                         env->DeleteLocalRef(node);
                         env->DeleteLocalRef(verb);
                         env->DeleteLocalRef(value);
                     });
    }

    // State channel "presentation" (hud::Presentation), on a change, once Android has said whether the
    // game is live.
    void Set(bool &field, bool value)
    {
        if (field == value)
            return;
        field = value;
        PublishPresentation();
    }

    void PublishPresentation()
    {
        if (!presentation_.live_known)
            return;
        const auto flag = [](bool value) { return value ? "true" : "false"; };
        std::string screen;
        for (const char c : presentation_.screen) // an interface id: the ini name in lower case
            if (c != '"' && c != '\\' && static_cast<unsigned char>(c) >= 32)
                screen += c;
        std::string json = std::string("{\"screen\":\"") + screen + "\",\"loading\":" + flag(presentation_.loading || presentation_.location_load) + ",\"cinematic\":" +
                           flag(presentation_.cinematic) + ",\"dialogue\":" + flag(presentation_.dialogue) + ",\"live\":" + flag(presentation_.live) + "}";
        if (json == presented_)
            return;
        presented_ = json;
        State("presentation", std::move(json));
    }

    void PublishInterfaceActive()
    {
        interface_active_ = host_ && host_->InterfaceActive();
    }

    // adb shell setprop log.tag.StormUiModels DEBUG: the newest model of each screen goes to
    // <external files>/ui-models/<screen>.json, to record real models for the screen tests.
    void Record(const storm::ui::InterfaceModel &model) const
    {
        if (screen_.empty() || !record_models_)
            return;
        const std::string json = storm::ui::InterfaceModelJson(model);
        const char *external = SDL_AndroidGetExternalStoragePath();
        if (!external)
            return;
        std::error_code error;
        const std::filesystem::path folder = std::filesystem::path(external) / "ui-models";
        std::filesystem::create_directories(folder, error);
        std::ofstream out(folder / (screen_ + ".json"), std::ios::binary);
        out.write(json.data(), std::streamsize(json.size()));
        if (!out)
            __android_log_print(ANDROID_LOG_ERROR, kTag, "Cannot record the interface %s in %s", screen_.c_str(), folder.c_str());
    }

    std::unique_ptr<storm::ui::FileSource> files_;
    jobject assets_ = nullptr; // keeps the Java AssetManager alive
    std::unique_ptr<storm::ui::BgfxBackend> backend_;
    std::unique_ptr<storm::ui::Host> host_;
    std::mutex host_mutex_; // the host's lifetime against the platform thread's queries
    bool failed_ = false;
    uint64_t generation_ = 0;
    uint64_t loop_ = 0;     // engine loop iterations (Tick)
    uint64_t tap_loop_ = 0; // the iteration that applied the newest touch end
    // The debug log tags (Loggable), read once.
    const bool trace_input_ = Loggable("StormUiInput");
    const bool trace_states_ = Loggable("StormUiState");
    const bool record_models_ = Loggable("StormUiModels");
    // With the input trace: which loop iteration each presented UI frame belongs to, and every
    // 5 s what the UI frames cost on the CPU (GPU work is the renderer's, in RenderMetrics).
    struct FrameTrace final : storm::render::FrameClient
    {
        void OnUiRender(const storm::render::FrameContext &frame) override
        {
            EngineUi &ui = EngineUi::Get();
            char at[64];
            std::snprintf(at, sizeof at, "%llu at %.1f ms", static_cast<unsigned long long>(frame.frame_index), MonotonicMs());
            ui.TraceInput("frame", at);
            if (!ui.host_)
                return;
            const storm::ui::FrameStats stats = ui.host_->LastFrameStats();
            ++frames;
            if (stats.number != counted) // a frame the host made (not the last one replayed unchanged)
            {
                counted = stats.number;
                // The frame timeline: every frame the host made, with what it showed and what it cost.
                const double now_ms = MonotonicMs();
                __android_log_print(ANDROID_LOG_DEBUG, "StormUiInput",
                                    "loop %llu: made frame %llu at %.1f ms (+%.1f): shows %s after %d held | update %.1f = apply %.1f + rebuild %.1f (%d, %d touched) + "
                                    "style %.1f + layout %.1f (%d boxes, %d passes) + pictures %.1f (%d), render %.1f (%d meshes, %d textures)",
                                    static_cast<unsigned long long>(ui.loop_), static_cast<unsigned long long>(stats.number), now_ms,
                                    made_ms ? now_ms - made_ms : 0.0, stats.shown.c_str(), stats.held, stats.update_ms, stats.apply_ms, stats.rebuild_ms,
                                    stats.rebuilds, stats.touched, stats.style_ms, stats.layout_ms, stats.laid_out, stats.layout_passes,
                                    stats.picture_ms, stats.pictures, stats.render_ms, stats.geometry, stats.textures);
                made_ms = now_ms;
                ++made;
                update_ms += stats.update_ms;
                render_ms += stats.render_ms;
                draws += stats.draws;
                layers += stats.layers;
                layouts += stats.layout_passes;
                touched += stats.touched;
                boxes += stats.laid_out;
                meshes += stats.geometry;
            }
            const uint32_t now = SDL_GetTicks();
            if (!since)
                since = now;
            if (now - since < 5000)
                return;
            const double per = made ? double(made) : 1.0;
            __android_log_print(ANDROID_LOG_DEBUG, "StormUiInput",
                                "%u UI frames in %.1f s, %u made (the rest replayed unchanged): per made frame CPU update %.2f ms + render %.2f ms, "
                                "%.0f draws, %.1f layers, %.1f elements touched, %.0f boxes laid out, %.1f meshes made; %ld layout passes",
                                frames, (now - since) / 1000.0, made, update_ms / per, render_ms / per, double(draws) / per, double(layers) / per,
                                double(touched) / per, double(boxes) / per, double(meshes) / per, layouts);
            frames = made = 0, update_ms = render_ms = 0.0, draws = layers = layouts = touched = boxes = meshes = 0, since = now;
        }
        uint32_t since = 0, frames = 0, made = 0;
        uint64_t counted = 0; // FrameStats::number of the last frame summed
        double made_ms = 0.0; // when the last made frame was traced
        double update_ms = 0.0, render_ms = 0.0;
        long draws = 0, layers = 0, layouts = 0, touched = 0, boxes = 0, meshes = 0;
    } frame_trace_;
    std::map<std::string, std::string> states_;
    std::map<std::string, std::string> platform_states_; // guarded by inbox_mutex_
    std::string screen_;
    // Channel "presentation": the engine's facts as they change, `live` from the activity.
    struct
    {
        std::string screen;
        bool loading = true, cinematic = false, dialogue = false, live = false;
        bool location_load = false; // a location load blocks the game: loading for the UI
        bool live_known = false;
    } presentation_;
    std::string presented_;
    std::optional<bool> live_; // from the platform thread, guarded by inbox_mutex_
    std::optional<storm::ui::InterfaceModel> interface_; // the open interface's newest model (replayed to a new host)
    uint32_t close_at_ = 0; // SDL ticks; 0 = no close pending
    std::atomic<bool> interface_active_{false};
    std::vector<std::string> commands_;
    bool in_action_ = false; // inside OnAction: answers wait for the next Tick
    struct PendingAnswer
    {
        uint64_t request;
        bool applied;
        std::string reason;
    };
    std::vector<PendingAnswer> answers_;
    std::map<std::string, size_t> slots_; // slot -> index in commands_
    // What the platform thread sent: a call, or (no call) a pointer step.
    struct Step
    {
        std::function<void(storm::ui::Host &)> call;
        int phase = 0, pointer = 0;
        float x = 0.0f, y = 0.0f;
        double time_ms = 0.0; // CLOCK_MONOTONIC (SystemClock.uptimeMillis)
    };
    std::mutex inbox_mutex_;
    std::vector<Step> inbox_;
    std::array<float, 4> safe_area_{};
    bool has_safe_area_ = false;
    int shown_ui_scale_ = -1, shown_hud_transparency_ = -1, shown_left_handed_ = -1; // the display options last logged
};

} // namespace

void state(std::string_view channel, std::string json)
{
    EngineUi::Get().State(channel, std::move(json));
}

void screen(const std::string &screen, uint64_t revision, double release_ms, double load_ms)
{
    EngineUi::Get().Screen(screen, revision, release_ms, load_ms);
}

void interfaceModel(storm::ui::InterfaceModel &&model)
{
    EngineUi::Get().Interface(std::move(model));
}

void toast(std::string text, std::string target)
{
    EngineUi::Get().GameMessage(std::move(text), std::move(target));
}

void answer(uint64_t request, bool applied, std::string reason)
{
    EngineUi::Get().Answer(request, applied, std::move(reason));
}

void loading(bool active)
{
    EngineUi::Get().Loading(active);
}

void cinematic(bool active)
{
    EngineUi::Get().Cinematic(active);
}

void dialogue(bool active)
{
    EngineUi::Get().Dialogue(active);
}

void locationLoad(bool blocks)
{
    EngineUi::Get().LocationLoad(blocks);
}

void alert(std::string string_key)
{
    EngineUi::Get().AlertString(string_key);
}

void tick()
{
    EngineUi::Get().Tick();
}

std::vector<std::string> takeCommands()
{
    return EngineUi::Get().TakeCommands();
}

uint64_t generation()
{
    return EngineUi::Get().Generation();
}

void shutdown()
{
    EngineUi::Get().Shutdown();
}

} // namespace storm::android::ui

// ---- org.stormengine.port.EngineUi (platform threads) ----------------------------------
using storm::android::ui::EngineUi;

extern "C" {

JNIEXPORT jboolean JNICALL Java_org_stormengine_port_EngineUi_nativeWantsTouch(JNIEnv *, jclass, jfloat x, jfloat y)
{
    return EngineUi::Get().WantsTouch(x, y) ? JNI_TRUE : JNI_FALSE;
}

// phase: 0 down, 1 move, 2 up, 3 cancel; physical pixels of the surface; time: the sample's
// MotionEvent time (SystemClock.uptimeMillis base)
JNIEXPORT void JNICALL Java_org_stormengine_port_EngineUi_nativeTouch(JNIEnv *, jclass, jint phase, jint pointer, jfloat x, jfloat y, jlong time)
{
    if (phase < 0 || phase > 3)
        return;
    EngineUi::Get().Touch(phase, pointer, x, y, static_cast<double>(time));
}

JNIEXPORT void JNICALL Java_org_stormengine_port_EngineUi_nativeCancelInput(JNIEnv *, jclass)
{
    EngineUi::Get().Post([](storm::ui::Host &host) { host.CancelInput(); });
}

JNIEXPORT jboolean JNICALL Java_org_stormengine_port_EngineUi_nativeWantsBack(JNIEnv *, jclass)
{
    return EngineUi::Get().WantsBack() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL Java_org_stormengine_port_EngineUi_nativeBack(JNIEnv *, jclass)
{
    EngineUi::Get().Post([](storm::ui::Host &host) { host.Back(); });
}

JNIEXPORT jboolean JNICALL Java_org_stormengine_port_EngineUi_nativeInterfaceActive(JNIEnv *, jclass)
{
    return EngineUi::Get().InterfaceActive() ? JNI_TRUE : JNI_FALSE;
}

// The channels the platform writes itself: "presentation", "appSettings".
JNIEXPORT void JNICALL Java_org_stormengine_port_EngineUi_nativeSetState(JNIEnv *env, jclass, jstring channel, jbyteArray json)
{
    std::string name = storm::android::ui::Text(env, channel), text = storm::android::ui::Bytes(env, json);
    EngineUi::Get().PlatformState(std::move(name), std::move(text));
}

JNIEXPORT void JNICALL Java_org_stormengine_port_EngineUi_nativeToast(JNIEnv *env, jclass, jstring text, jstring tone)
{
    std::string line = storm::android::ui::Text(env, text), kind = storm::android::ui::Text(env, tone);
    EngineUi::Get().Post([line = std::move(line), kind = std::move(kind)](storm::ui::Host &host) { host.Toast(line, kind); });
}

JNIEXPORT void JNICALL Java_org_stormengine_port_EngineUi_nativeSubmitText(JNIEnv *env, jclass, jstring token, jstring text, jboolean accepted)
{
    std::string id = storm::android::ui::Text(env, token), answer = storm::android::ui::Text(env, text);
    EngineUi::Get().Post([id = std::move(id), answer = std::move(answer), accepted](storm::ui::Host &host) {
        host.SubmitText(id, answer, accepted == JNI_TRUE);
    });
}

JNIEXPORT void JNICALL Java_org_stormengine_port_EngineUi_nativeSetSafeArea(JNIEnv *, jclass, jfloat left, jfloat top, jfloat right, jfloat bottom)
{
    EngineUi::Get().SafeArea(left, top, right, bottom);
}

} // extern "C"
