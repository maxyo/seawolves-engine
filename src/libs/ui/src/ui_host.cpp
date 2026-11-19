#include "storm_ui/ui.h"

#include "storm_ui/render_backend.h"

#include "action_trace.h"
#include "components.h"
#include "touch_motion.h"
#include "elements.h"
#include "frame_renderer.h"
#include "hud/hud.h"
#include "screens/screen.h"
#include "storm_ui/interface_model.h"

#include <RmlUi/Core.h>
#include <plutovg.h>

#if defined(__ANDROID__)
#include <android/log.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <unordered_map>

namespace storm::ui
{

namespace
{

class DirectorySource final : public FileSource
{
  public:
    explicit DirectorySource(std::string root) : root_(std::move(root))
    {
    }
    bool Read(const std::string &path, std::vector<uint8_t> &out) override
    {
        if (path.find("..") != std::string::npos)
            return false;
        std::ifstream file(root_ + "/" + path, std::ios::binary);
        if (!file)
            return false;
        out.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
        return true;
    }

  private:
    std::string root_;
};

class PackFiles final : public Rml::FileInterface
{
  public:
    explicit PackFiles(FileSource &source) : source_(source)
    {
    }

    Rml::FileHandle Open(const Rml::String &path) override
    {
        auto file = std::make_unique<Opened>();
        if (!source_.Read(Normalise(path), file->bytes))
            return 0;
        return reinterpret_cast<Rml::FileHandle>(file.release());
    }
    void Close(Rml::FileHandle handle) override
    {
        delete reinterpret_cast<Opened *>(handle);
    }
    size_t Read(void *buffer, size_t size, Rml::FileHandle handle) override
    {
        auto *file = reinterpret_cast<Opened *>(handle);
        const size_t count = std::min(size, file->bytes.size() - file->position);
        std::memcpy(buffer, file->bytes.data() + file->position, count);
        file->position += count;
        return count;
    }
    bool Seek(Rml::FileHandle handle, long offset, int origin) override
    {
        auto *file = reinterpret_cast<Opened *>(handle);
        const long base = origin == SEEK_SET ? 0 : origin == SEEK_CUR ? static_cast<long>(file->position) : static_cast<long>(file->bytes.size());
        const long target = base + offset;
        if (target < 0 || static_cast<size_t>(target) > file->bytes.size())
            return false;
        file->position = static_cast<size_t>(target);
        return true;
    }
    size_t Tell(Rml::FileHandle handle) override
    {
        return reinterpret_cast<Opened *>(handle)->position;
    }

    // Collapses "a/../b" so that documents can link styles relative to themselves.
    static std::string Normalise(const std::string &path)
    {
        std::vector<std::string> parts;
        size_t at = 0;
        while (at <= path.size())
        {
            size_t end = path.find('/', at);
            if (end == std::string::npos)
                end = path.size();
            const std::string part = path.substr(at, end - at);
            if (part == "..")
            {
                if (!parts.empty())
                    parts.pop_back();
            }
            else if (!part.empty() && part != ".")
                parts.push_back(part);
            at = end + 1;
        }
        std::string out;
        for (const std::string &part : parts)
            out += (out.empty() ? "" : "/") + part;
        return out;
    }

  private:
    struct Opened
    {
        std::vector<uint8_t> bytes;
        size_t position = 0;
    };
    FileSource &source_;
};

class System final : public Rml::SystemInterface
{
  public:
    std::function<double()> clock;
    std::function<void(LogLevel, const std::string &)> sink;

    double GetElapsedTime() override
    {
        if (clock)
            return clock();
        using namespace std::chrono;
        static const steady_clock::time_point start = steady_clock::now();
        return duration<double>(steady_clock::now() - start).count();
    }

    bool LogMessage(Rml::Log::Type type, const Rml::String &message) override
    {
#if defined(__ANDROID__)
        const int priority = type == Rml::Log::LT_ERROR || type == Rml::Log::LT_ASSERT ? ANDROID_LOG_ERROR
                             : type == Rml::Log::LT_WARNING                            ? ANDROID_LOG_WARN
                                                                                       : ANDROID_LOG_INFO;
        __android_log_print(priority, "StormUi", "%s", message.c_str());
#else
        std::fprintf(stderr, "[ui %d] %s\n", static_cast<int>(type), message.c_str());
#endif
        if (sink)
            sink(type == Rml::Log::LT_ERROR || type == Rml::Log::LT_ASSERT ? LogLevel::Error
                 : type == Rml::Log::LT_WARNING                            ? LogLevel::Warning
                                                                           : LogLevel::Info,
                 message);
        return true;
    }
};

bool ReadJson(FileSource &files, const std::string &path, Json &out, std::string *error)
{
    std::vector<uint8_t> bytes;
    if (!files.Read(path, bytes))
    {
        if (error)
            *error = "UI pack lacks " + path;
        return false;
    }
    out = Json::parse(bytes.begin(), bytes.end(), nullptr, false);
    if (out.is_discarded())
    {
        if (error)
            *error = "UI pack: malformed " + path;
        return false;
    }
    return true;
}

// Every document of the context: counts its layout passes (FrameStats::laid_out), so that a
// re-layout of plain elements (not only of components) makes Prepare draw a frame.
class CountedDocument final : public Rml::ElementDocument
{
  public:
    CountedDocument(const Rml::String &tag, FrameStats &stats) : Rml::ElementDocument(tag), stats_(stats)
    {
    }

  protected:
    void OnLayoutStart() override // patches/rmlui-document-layout-start.patch
    {
        layout_started_ = std::chrono::steady_clock::now();
    }
    void OnLayout() override
    {
        Rml::ElementDocument::OnLayout();
        ++stats_.laid_out;
        ++stats_.layout_passes;
        stats_.layout_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - layout_started_).count();
    }

  private:
    FrameStats &stats_;
    std::chrono::steady_clock::time_point layout_started_;
};

class CountedDocumentInstancer final : public Rml::ElementInstancer
{
  public:
    explicit CountedDocumentInstancer(FrameStats &stats) : stats_(stats)
    {
    }
    Rml::ElementPtr InstanceElement(Rml::Element *, const Rml::String &tag, const Rml::XMLAttributes &) override
    {
        return Rml::ElementPtr(new CountedDocument(tag, stats_));
    }
    void ReleaseElement(Rml::Element *element) override
    {
        delete element;
    }

  private:
    FrameStats &stats_;
};

double Milliseconds(std::chrono::steady_clock::time_point from, std::chrono::steady_clock::time_point to)
{
    return std::chrono::duration<double, std::milli>(to - from).count();
}

// The one live host of this process (Host::Create).
std::atomic<bool> g_host_alive{false};

class HostImpl final : public Host, public Services, public screens::Surface, public hud::Mount
{
  public:
    bool owns_process_slot = false;

    ~HostImpl() override
    {
        captures_.clear();
        ticking_.clear();
        router_.reset();
        hud_.reset();
        if (context_)
        {
            context_->UnloadAllDocuments();
            Rml::RemoveContext(context_->GetName());
        }
        ShutdownGameImages();
        SetGameImageRenderer(nullptr);
        ReleaseIconTextures();
        ReleaseDrawTextures();
        if (initialised_)
            Rml::Shutdown();
        registry_.reset();
        if (owns_process_slot)
            g_host_alive = false;
    }

    bool Start(const Config &config, std::string *error)
    {
        config_ = config;
        system_.clock = config.clock;
        system_.sink = config.on_log;
        if (!config.files)
        {
            if (error)
                *error = "no UI pack";
            return false;
        }
        backend_ = config.backend;
        if (!backend_)
        {
            if (error)
                *error = "no render backend given";
            return false;
        }
        files_ = std::make_unique<PackFiles>(*config.files);
        Rml::SetSystemInterface(&system_);
        Rml::SetFileInterface(files_.get());
        renderer_ = std::make_unique<FrameRenderer>(backend_->Interface(), counters_);
        renderer_->Record(config.verify_unchanged);
        Rml::SetRenderInterface(renderer_.get());
        if (!Rml::Initialise())
        {
            if (error)
                *error = "RmlUi failed to start";
            return false;
        }
        initialised_ = true;
        document_instancer_ = std::make_unique<CountedDocumentInstancer>(counters_);
        Rml::Factory::RegisterElementInstancer("body", document_instancer_.get());

        Json pack, components, icons;
        if (!ReadJson(*config.files, "pack.json", pack, error) ||
            !ReadJson(*config.files, pack.value("components", std::string("components.json")), components, error) ||
            !ReadJson(*config.files, pack.value("icons", std::string("icons.json")), icons, error))
            return false;
        settings_ = pack.value("settings", Json::object());
        RegisterBuiltinBehaviors();
        RegisterDrawElements();
        RegisterIconElement();
        RegisterGameImageElement();
        LoadIconPack(icons);
        registry_ = std::make_unique<ComponentRegistry>(*this);
        registry_->Load(components);
        SetGameImageRenderer(backend_);
        SetGameImageRoot(config.game_root);

        bool fallback = true;
        const Json fonts = pack.value("fonts", Json::array());
        for (const Json &font : fonts)
        {
            if (!Rml::LoadFontFace(font.get<std::string>(), fallback))
                Rml::Log::Message(Rml::Log::LT_ERROR, "Font %s failed to load", font.get<std::string>().c_str());
            fallback = false;
        }

        backend_->SetViewport(config.width, config.height);
        context_ = Rml::CreateContext("storm-ui", {config.width, config.height});
        if (!context_)
        {
            if (error)
                *error = "RmlUi context failed";
            return false;
        }
        display_density_ = config.density;
        context_->SetDensityIndependentPixelRatio(config.density);
        // UI strings: one table per shipped language, chosen by the game language.
        const Json string_files = pack.value("strings", Json::object());
        for (const auto &entry : string_files.items())
        {
            Json table;
            if (!ReadJson(*config.files, entry.value().get<std::string>(), table, error))
                return false;
            strings_[entry.key()] = std::move(table);
        }
        SetTemplateStrings([this](const std::string &key) { return Text(key); });
        SetTemplateStrict(config.strict_templates);
        screen_path_ = pack.value("screen", std::string());
        // The HUD: its document and where the elements of every scene sit. A pack that
        // names a layout must hold a readable one; a pack without a HUD (none shipped
        // today) answers every HUD state with an error.
        hud_path_ = pack.value("hud", std::string());
        const std::string layout_path = pack.value("hudLayout", std::string());
        if (!layout_path.empty())
        {
            Json layout;
            if (!ReadJson(*config.files, layout_path, layout, error) || !hud::ParseLayout(layout, layout_, error))
                return false;
        }
        hud_ = std::make_unique<hud::Hud>(*this);
        router_ = std::make_unique<screens::Router>(*this);
        if (config_.game_hud)
            router_->AttachHud(hud_.get());
        overlay_path_ = pack.value("overlay", std::string());
        if (!overlay_path_.empty())
        {
            overlay_ = LoadPackDocument(overlay_path_);
            if (overlay_)
                overlay_->Show();
        }
        return true;
    }

    // --- Host ---------------------------------------------------------------------

    bool LoadDocument(const std::string &path) override
    {
        game_interface_ = false;
        game_screen_.clear();
        router_->CloseInterface();
        const bool opened = OpenDocument(path);
        PublishTouchMap();
        return opened;
    }

    // Every pack document: debug builds (Config::show_errors) mark its root `ui-debug`, the one switch
    // for the inline marks of a broken component (the `.ui-debug .ui-error` stripes of the pack's
    // stylesheet, the crossed icon of icons.cpp). Release builds draw nothing in their place; the
    // error itself is logged in both.
    Rml::ElementDocument *LoadPackDocument(const std::string &path)
    {
        Rml::ElementDocument *document = context_->LoadDocument(path);
        if (document && config_.show_errors)
            document->SetClass(kDebugMarksClass, true);
        return document;
    }

    bool OpenDocument(const std::string &path)
    {
        changed_ = true;
        captures_.clear();
        screen_root_ = nullptr;
        if (document_)
        {
            document_->Close();
            document_ = nullptr;
        }
        document_ = LoadPackDocument(path);
        if (!document_)
            return false;
        ReportUnknownComponents(document_);
        document_->Show();
        if (overlay_)
            overlay_->PullToFront();
        return true;
    }

    void Resize(int width, int height, float density) override
    {
        changed_ = true;
        config_.width = width;
        config_.height = height;
        display_density_ = density;
        config_.density = density * ui_scale_; // every dp of the layout and the touch slop
        backend_->SetViewport(width, height);
        context_->SetDimensions({width, height});
        context_->SetDensityIndependentPixelRatio(config_.density);
    }

    void SetDisplayOptions(float ui_scale, float hud_button_opacity, bool left_handed) override
    {
        if (left_handed != left_handed_)
        {
            left_handed_ = left_handed;
            // Mounted anew with the other placement and props: restyling the mounted elements in place left the
            // touch regions of their parts at the old place (RmlUi keeps a child's absolute offset until it
            // lays it out itself), so every element is created again. A held control is released first.
            if (hud_document_ && hud_document_->IsVisible())
                HideHud();
            hud_props_.clear();
            if (hud_)
                hud_->Rebuild();
        }
        if (ui_scale != ui_scale_)
        {
            ui_scale_ = ui_scale;
            Resize(config_.width, config_.height, display_density_);
            ApplySafeArea(); // the insets came in the display's dp
        }
        if (hud_button_opacity != hud_button_opacity_)
        {
            hud_button_opacity_ = hud_button_opacity;
            ApplyHudButtonOpacity();
        }
    }

    void SetSafeArea(float left, float top, float right, float bottom) override
    {
        display_safe_ = {left, top, right, bottom};
        ApplySafeArea();
    }

    // «Прозрачность кнопок»: a class on the HUD document's body the HUD buttons' style answers,
    // hud-buttons-<opacity percent> (none at full opacity; the pack styles each offered step).
    void ApplyHudButtonOpacity()
    {
        if (!hud_document_)
            return;
        if (!hud_button_class_.empty())
            hud_document_->SetClass(hud_button_class_, false);
        const int percent = static_cast<int>(std::lround(hud_button_opacity_ * 100.0f));
        hud_button_class_ = percent >= 100 ? std::string() : "hud-buttons-" + std::to_string(percent);
        if (!hud_button_class_.empty())
            hud_document_->SetClass(hud_button_class_, true);
        changed_ = true;
    }

    // The insets in the layout's dp: the display's dp shrink by the player's UI scale.
    void ApplySafeArea()
    {
        const auto [left, top, right, bottom] = display_safe_;
        const float scale = ui_scale_ > 0.0f ? ui_scale_ : 1.0f;
        const Json safe = {{"left", left / scale}, {"top", top / scale}, {"right", right / scale}, {"bottom", bottom / scale}};
        if (safe == safe_)
            return;
        safe_ = safe;
        changed_ = true;
        // Rebuild every component: the insets reach the templates through their scope.
        std::vector<Rml::Element *> pending = {document_, overlay_, hud_document_};
        while (!pending.empty())
        {
            Rml::Element *element = pending.back();
            pending.pop_back();
            if (!element)
                continue;
            if (auto *component = dynamic_cast<ComponentElement *>(element))
                component->SetProps(Json::object());
            for (int i = 0; i < element->GetNumChildren(); ++i)
                pending.push_back(element->GetChild(i));
        }
        PlaceToasts();
    }

    // The host's toast stack: at the top edge (8 dp: its offset in base.rcss), clear
    // of a top status of the HUD that is shown (LayoutElement::toast_clear: the cabin captain,
    // the boarding bars), whose layout dp grow by the player's UI scale, and of a shown screen's
    // header (its tabs stay visible and pressable under a toast).
    void PlaceToasts()
    {
        const auto [left, top, right, bottom] = display_safe_;
        const float scale = ui_scale_ > 0.0f ? ui_scale_ : 1.0f;
        if (overlay_)
            if (Rml::Element *stack = overlay_->GetElementById("toasts"))
            {
                stack->SetProperty(Rml::PropertyId::Left, Rml::Property(left, Rml::Unit::DP));
                stack->SetProperty(Rml::PropertyId::Right, Rml::Property(right, Rml::Unit::DP));
                const float clear = hud_toast_clear_ > 0.0f ? (hud_toast_clear_ + kToastStatusGap) * scale : 0.0f;
                const float header = screen_toast_clear_ > 0.0f ? screen_toast_clear_ + kToastStatusGap : 0.0f;
                stack->SetProperty(Rml::PropertyId::Top, Rml::Property(std::max(top + std::max(8.0f, clear), header), Rml::Unit::DP));
            }
    }
    static constexpr float kToastStatusGap = 6.0f; // dp between a top status and the toasts under it

    // The bottom of the shown screen's header in layout dp (0: none shown); true when it moved.
    bool ScreenHeaderMoved()
    {
        float bottom = 0.0f;
        if (screen_root_ && document_ && document_->IsVisible())
            if (Rml::Element *header = document_->QuerySelector("c-header"); header && header->IsVisible(true))
                bottom = (header->GetAbsoluteOffset(Rml::BoxArea::Border).y + header->GetBox().GetSize(Rml::BoxArea::Border).y) /
                         config_.density;
        if (std::fabs(bottom - screen_toast_clear_) < 0.5f)
            return false;
        screen_toast_clear_ = bottom;
        PlaceToasts();
        return true;
    }

    void SetToastClear(float clear)
    {
        if (clear == hud_toast_clear_)
            return;
        hud_toast_clear_ = clear;
        changed_ = true;
        PlaceToasts();
    }

    void SetGameRoot(const std::string &game_root) override
    {
        changed_ = true;
        config_.game_root = game_root;
        SetGameImageRoot(game_root);
    }

    using Host::Touch;

    // PlaceTouches places a pointer where it was this long before the frame's time, as Android's
    // own resampling of batched touches does: at the usual sample rates the place then lies
    // between two samples, not past the newest.
    static constexpr double kTouchPlaceLatency = 0.005;
    struct Capture
    {
        Pointer pointer;
        Rml::ObserverPtr<Rml::Element> component;
        bool exclusive = false;
        // A drag (a slider) on a list that scrolls vertically: the first movement past the slop
        // decides — sideways the component takes the pointer, up or down the list scrolls; a
        // tap without movement goes to the component.
        bool deferred = false;
        struct Sample
        {
            Rml::Vector2f at;
            double time = 0.0;
        };
        std::vector<Sample> trail; // the newest samples of the pointer, oldest first
        bool unplaced = false;     // samples came since the pointer was last placed (PlaceTouches)

        void Remember(Rml::Vector2f at, double time)
        {
            if (trail.size() == 8)
                trail.erase(trail.begin());
            trail.push_back({at, time});
        }
        // Where the pointer was at `time`: on the line between the two samples around it; past
        // the newest sample, on along the last two for at most half their distance in time and
        // 8 ms, when they are 2 to 20 ms apart (the rules of Android's touch resampling:
        // InputConsumer). Otherwise the nearest sample.
        Sample At(double time) const
        {
            const Sample &newest = trail.back();
            if (trail.size() < 2)
                return newest;
            if (time >= newest.time)
            {
                const Sample &before = trail[trail.size() - 2];
                const double apart = newest.time - before.time;
                if (apart < 0.002 || apart > 0.020)
                    return newest;
                const double ahead = std::min({time - newest.time, apart * 0.5, 0.008});
                return {newest.at + (newest.at - before.at) * float(ahead / apart), newest.time + ahead};
            }
            for (size_t i = trail.size() - 1; i > 0; --i)
                if (time >= trail[i - 1].time)
                {
                    const Sample &a = trail[i - 1], &b = trail[i];
                    const double apart = b.time - a.time;
                    return {apart > 0.0 ? a.at + (b.at - a.at) * float((time - a.time) / apart) : b.at, time};
                }
            return trail.front();
        }
    };

    void Touch(TouchPhase phase, int pointer, float x, float y, double time) override
    {
        changed_ = true;
        if (config_.trace_actions && phase == TouchPhase::Down)
            Traced(tracer_.Finish(true)); // a new gesture: what the last one did is over
        else if (config_.trace_actions && phase == TouchPhase::Up)
            BeginTrace("tap " + std::to_string(int(x)) + "," + std::to_string(int(y)));
        // A pointer that went down on what is not laid out yet (an interface whose model is
        // still to come, a sheet opened since the last frame) waits for that layout, with the
        // rest of its gesture (ReleaseHeld).
        if (phase == TouchPhase::Down ? Holding() : held_pointers_.count(pointer) > 0)
        {
            if (phase == TouchPhase::Down)
            {
                held_pointers_.insert(pointer);
                Log("touch held for " + std::string(opening_ != Opening::None ? "an interface being opened" : between_ ? "what comes after the transition" : "the next layout") +
                    ": " + RoutingFacts());
            }
            // Applied later, all at once: a sample is then a move like any other.
            held_.push_back({phase == TouchPhase::Sample ? TouchPhase::Move : phase, pointer, x, y, time});
            return;
        }
        if (phase == TouchPhase::Sample)
            Sampled(pointer, x, y, time);
        else
            Apply(phase, pointer, x, y, time);
    }

    // A move sample whose place comes with PlaceTouches.
    void Sampled(int pointer, float x, float y, double time)
    {
        const auto found = captures_.find(pointer);
        if (found == captures_.end())
            return;
        Capture &capture = found->second;
        capture.Remember({x, y}, time);
        capture.unplaced = true;
        if (!capture.exclusive && pointer == scroll_pointer_)
            scroll_.Sample({x, y}, time);
    }

    void PlaceTouches(double time) override
    {
        // What a move does may end any capture (a screen that closes): by pointer, looked up again.
        std::vector<int> pointers;
        for (const auto &entry : captures_)
            if (entry.second.unplaced)
                pointers.push_back(entry.first);
        for (const int pointer : pointers)
        {
            const auto found = captures_.find(pointer);
            if (found == captures_.end() || !found->second.unplaced)
                continue;
            found->second.unplaced = false;
            const Capture::Sample place = found->second.At(time - kTouchPlaceLatency);
            MoveTo(found->second, pointer, place.at, place.time, false);
        }
    }

    void Apply(TouchPhase phase, int pointer, float x, float y, double time)
    {
        const Rml::Vector2f at = {x, y};
        if (phase == TouchPhase::Down)
        {
            Capture capture;
            capture.pointer.id = pointer;
            capture.pointer.position = capture.pointer.start = at;
            capture.pointer.time = time;
            capture.Remember(at, time);
            capture.pointer.target = context_->GetElementAtPoint(at);
            // The HUD takes input only while the player can act (Presentation::live).
            if (capture.pointer.target && capture.pointer.target->GetOwnerDocument() == hud_document_ && !hud_->Interactive())
                return;
            ComponentElement *component = InteractiveComponentAt(capture.pointer.target, capture.pointer);
            // Claimed for something that is gone when it arrives (a load ended in gameplay in
            // between): nothing takes it, said once per gesture.
            if (!component && !InterfaceActive() && !hud_->Interactive())
                Log("touch reached nothing of the UI: " + RoutingFacts());
            bool deferred = false;
            if (component && component->PointerExclusive(capture.pointer) && scroll_pointer_ < 0)
            {
                scroll_.Down(capture.pointer.target, at, time, config_.density, Slop());
                deferred = scroll_.ScrollsVertically() && !scroll_.Caught();
                if (deferred || scroll_.Caught())
                    scroll_pointer_ = pointer;
                else
                    scroll_.Up(time, true); // no list to scroll: the drag is the component's
                if (deferred)
                {
                    capture.deferred = true;
                    capture.component = component->GetObserverPtr();
                }
                else if (scroll_.Caught())
                    component = nullptr; // a finger that stops a list in motion drags it
            }
            if (!deferred && component && component->PointerExclusive(capture.pointer))
            {
                capture.exclusive = true;
                capture.component = component->GetObserverPtr();
                component->PointerDown(capture.pointer);
            }
            else if (!deferred)
            {
                if (scroll_pointer_ < 0)
                {
                    scroll_pointer_ = pointer;
                    scroll_.Down(capture.pointer.target, at, time, config_.density, Slop());
                }
                // A finger that stops a list in motion drags it: it presses nothing.
                const bool caught = pointer == scroll_pointer_ && scroll_.Caught();
                if (component && !caught && component->PointerDown(capture.pointer))
                    capture.component = component->GetObserverPtr();
            }
            captures_[pointer] = std::move(capture);
            return;
        }

        if (phase == TouchPhase::Up) // the end of a tap: what a screen switch answers (ReportSwitch)
            last_tap_ = Clock::now() - std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(std::max(0.0, Now() - time)));
        auto found = captures_.find(pointer);
        if (found == captures_.end())
            return;
        if (phase == TouchPhase::Move)
        {
            found->second.Remember(at, time);
            found->second.unplaced = false;
            MoveTo(found->second, pointer, at, time, true);
            return;
        }
        // The gesture ends where its newest sample was, not where it was last placed.
        if (found->second.unplaced)
        {
            found->second.unplaced = false;
            const Capture::Sample newest = found->second.trail.back();
            MoveTo(found->second, pointer, newest.at, newest.time, false);
            found = captures_.find(pointer);
            if (found == captures_.end())
                return;
        }
        Capture &capture = found->second;
        capture.pointer.position = at;
        capture.pointer.time = time;
        auto *component = static_cast<ComponentElement *>(capture.component.get());
        if (capture.deferred && component && phase != TouchPhase::Cancel)
        {
            // A tap on the slider: it goes there as it would have outside a list.
            capture.deferred = false;
            capture.exclusive = true;
            scroll_.Up(time, true);
            scroll_pointer_ = -1;
            Pointer down = capture.pointer;
            down.position = down.start;
            component->PointerDown(down);
        }
        if (component && !capture.deferred)
            component->PointerUp(capture.pointer, phase == TouchPhase::Cancel);
        if (!capture.exclusive && pointer == scroll_pointer_)
        {
            scroll_.Up(time, phase == TouchPhase::Cancel);
            scroll_pointer_ = -1;
        }
        captures_.erase(pointer);
    }

    // A tap that travels this far is a drag (a scroll), no longer a press.
    float Slop() const
    {
        return 10.0f * config_.density;
    }

    // The pointer is at `at`: what it drags follows. `sampled`: a sample of the touch screen
    // (it counts for a list's release velocity), not a place between or past samples.
    void MoveTo(Capture &capture, int pointer, Rml::Vector2f at, double time, bool sampled)
    {
        capture.pointer.position = at;
        capture.pointer.time = time;
        auto *component = static_cast<ComponentElement *>(capture.component.get());
        if (capture.deferred && (at - capture.pointer.start).SquaredMagnitude() > Slop() * Slop())
        {
            capture.deferred = false;
            const Rml::Vector2f moved = at - capture.pointer.start;
            if (component && std::abs(moved.x) >= std::abs(moved.y))
            {
                // Sideways: the component's drag, from where the finger went down.
                scroll_.Up(time, true);
                scroll_pointer_ = -1;
                capture.exclusive = true;
                Pointer down = capture.pointer;
                down.position = down.start;
                component->PointerDown(down);
            }
            else
            {
                capture.component = {};
                component = nullptr;
            }
        }
        else if (capture.deferred)
        {
            if (pointer == scroll_pointer_)
                scroll_.Move(at, time, sampled);
            return;
        }
        if (capture.exclusive)
        {
            if (component)
                component->PointerMove(capture.pointer);
            return;
        }
        const bool scrolling = pointer == scroll_pointer_ && scroll_.Move(at, time, sampled);
        if (!component)
            return;
        if (scrolling || (at - capture.pointer.start).SquaredMagnitude() > Slop() * Slop())
        {
            component->PointerUp(capture.pointer, true); // the press is withdrawn
            capture.component = {};
        }
        else
            component->PointerMove(capture.pointer);
    }

    bool Prepare(double &delay) override
    {
        const auto started = std::chrono::steady_clock::now();
        const double style_before = counters_.style_ms;
        const double now = system_.GetElapsedTime();
        // A list in motion after the finger left: placed where it is at this frame's time.
        if (scroll_.Update(now))
            changed_ = true;
        const std::vector<Rml::ObserverPtr<Rml::Element>> ticking = ticking_;
        for (const auto &entry : ticking)
            if (auto *component = static_cast<ComponentElement *>(entry.get()))
                component->Tick(now);
        ExpireToasts(now);
        const bool images = GameImagesChanged();
        const double routing = config_.game_hud ? std::min(router_->Tick(), hud_->Tick()) : router_->Tick();

        UpdateContext();
        // Scroll edges move only with a layout, a touch or a scroll in motion: the walk over
        // every element is skipped on frames without any of them.
        const bool moved = changed_ || counters_.laid_out > 0 || context_->GetNextUpdateDelay() <= 0.0;
        const bool edges = moved && ScrollEdgesChanged(context_->GetRootElement());
        const bool clamps = TextClampsChanged() || edges;
        const bool header = moved && ScreenHeaderMoved();
        // Components that measure their layout (columns) set styles from it.
        bool measured = SettleLaidOutComponents();
        if (edges)
            regions_moved_ = true; // a scroll moved what is drawn without a layout
        // clamped text was re-cut for a new width, a scrolling edge changed, the toasts moved under a header
        const bool relaid = clamps || measured || header;
        if (relaid)
            UpdateContext();
        // A measurement is drawn with the layout it makes. Measuring settles in one more pass
        // (a scrollbar it adds: two); one that keeps changing is an error of the component.
        for (int pass = 1; measured && pass <= 2; ++pass)
            if ((measured = SettleLaidOutComponents()))
                UpdateContext();
        if (measured && SettleLaidOutComponents())
            Rml::Log::Message(Rml::Log::LT_ERROR, "UI: a component's measured layout does not settle (it changes on every pass)");
        // A pass above laid out again (re-cut text, a measurement): a list's height can have changed with
        // it, so its scroll edge and fade are placed from that layout, in this frame (else the frame showed the fade
        // where the layout before had it, and the next one moved it).
        if (relaid)
            for (int pass = 0; pass < 2 && ScrollEdgesChanged(context_->GetRootElement()); ++pass)
                UpdateContext();
        // Debug and contract hosts: what a game interface's screen draws for a tap is mapped (else the tap would be
        // reported only when the player makes it, and do nothing).
        if (screen_root_ && counters_.touched > 0 && (config_.show_errors || config_.strict_templates))
            CheckTaps(screen_root_);
        if (config_.log_taps && (moved || counters_.touched > 0))
            LogTaps();
        delay = context_->GetNextUpdateDelay();
        // What makes the shown frame out of date: input and host calls (changed_), component
        // rebuilds that changed elements (a render equal to the shown one changes nothing),
        // layout, ticking behaviours, animations and transitions, game
        // pictures that arrived, re-cut text.
        if (images || clamps || !ticking_.empty() || delay <= 0.0 || counters_.touched > 0 || counters_.laid_out > 0)
            changed_ = true;
        if (changed_)
            delay = 0.0;
        else if (GameImagesPending())
            delay = std::min(delay, 0.05); // a decoded picture is shown on the next frame
        delay = std::min(delay, routing);
        for (const Toast_ &toast : toasts_)
            if (toast.held < 0.0)
                delay = std::min(delay, std::max(0.0, (toast.leaving >= 0.0 ? toast.leaving + ToastLeaveSeconds() : toast.until) - now));
        TakeGameImageCost(counters_.picture_ms, counters_.pictures);
        if (tracer_.Active())
            Traced(tracer_.Poll(system_.GetElapsedTime(), opening_ != Opening::None || router_->Answering()));
        const bool draw = changed_ || !drawn_;
        if (draw)
            counters_.update_ms += Milliseconds(started, std::chrono::steady_clock::now());
        else
        {
            // Nothing to draw: this pass is not part of the next frame's cost (a thousand idle
            // passes summed made a tap's frame look like it took a second of styles).
            counters_.style_ms = style_before;
            if (switch_.drawn)
                ReportSwitch(true); // nothing more to draw: the screen has settled
            // The frame on the screen still shows everything: acknowledgements that wait
            // for a drawn frame are given for it.
            hud_->Drawn();
            PublishTouchMap(true);
            ReleaseHeld();
        }
        return draw;
    }

    // Context::Update, its time split for FrameStats: layout (the documents' own hooks), component
    // rebuilds and game pictures (theirs); the rest is styles.
    void UpdateContext()
    {
        TakeGameImageCost(counters_.picture_ms, counters_.pictures);
        const double rebuilt = counters_.rebuild_ms, acquired = counters_.picture_ms, laid_out = counters_.layout_ms;
        const auto started = std::chrono::steady_clock::now();
        context_->Update();
        const double ms = Milliseconds(started, std::chrono::steady_clock::now());
        TakeGameImageCost(counters_.picture_ms, counters_.pictures); // acquired in their elements' update
        counters_.style_ms +=
            std::max(0.0, ms - (counters_.rebuild_ms - rebuilt) - (counters_.picture_ms - acquired) - (counters_.layout_ms - laid_out));
    }

    double Frame() override
    {
        double delay = 0.0;
        const bool predicted = Prepare(delay);
        const auto updated = std::chrono::steady_clock::now();
        renderer_->BeginFrame();
        backend_->BeginFrame(true);
        context_->Render();
        backend_->EndFrame();
        renderer_->EndFrame();
        counters_.render_ms += Milliseconds(updated, std::chrono::steady_clock::now());
        if (backend_->FrameHeld())
        {
            // Not shown: the backend is still making textures this frame draws (the box shadows
            // of a screen's panels) and left the frame before it on the screen. Drawn again
            // next time; what it cost goes to the frame that is shown, and nothing that waits
            // for a drawn frame (held touches, the HUD's acknowledgements, the switch report)
            // is told.
            renderer_->Dropped();
            ++counters_.held;
            changed_ = true;
            return 0.0;
        }
        counters_.mispredicted = !predicted && config_.verify_unchanged && renderer_->FrameHash() != shown_hash_;
        if (counters_.mispredicted)
            Error("UI frame predicted unchanged drew something else: a change that does not ask for a frame; " + renderer_->Difference());
        shown_hash_ = renderer_->FrameHash();
        // Textures made or handed out while drawing (a font atlas that grew for new glyphs, a
        // box shadow made again) make RmlUi re-encode meshes and re-apply its clip state in
        // the next frame: that frame is drawn too.
        changed_ = counters_.textures > 0 || counters_.textures_shared > 0;
        drawn_ = true;
        counters_.number = last_frame_.number + 1;
        counters_.shown = Shown();
        if (tracer_.Active())
            tracer_.Presented(TraceContent(counters_.shown));
        ReportSwitch(false);
        ReportSlowFrame();
        last_frame_ = counters_;
        counters_ = FrameStats(); // what happens until the next frame counts for it
        hud_->Drawn();
        if (last_frame_.laid_out > 0 || last_frame_.touched > 0)
            regions_moved_ = true; // boxes moved, or elements came, went or changed their classes
        PublishTouchMap(true);
        ReleaseHeld();
        return delay;
    }

    FrameStats LastFrameStats() const override
    {
        return last_frame_;
    }

    void Toast(const std::string &text, const std::string &tone) override
    {
        Show("toast", text, tone);
    }

    // On the overlay whatever is shown: over the game, over a screen, and through the change
    // from one to the other (a failed load: the HUD's log would vanish with the menu that comes back).
    void Alert(const std::string &text) override
    {
        ShowAlert("alert", text);
    }

    void GameMessage(const std::string &text, const std::string &target) override
    {
        Shown(target.empty() ? "game log" : "game log, opens " + target, "info", text);
        if (hud_->Notice(text, "info", target)) // over the game: the HUD's log, a tap opens the target
            return;
        OverlayToast(text, "info", hud::ToastSeconds(text)); // over a screen: only shown
    }

    void AlertString(const std::string &key) override
    {
        ShowAlert("alert " + key, Text(key));
    }

    // Every text the host puts on the screen goes through Show, ShowAlert or GameMessage, which log it first
    // (Shown): what the player saw is in the log, with where it came from. `source` names the caller.
    void Show(const std::string &source, const std::string &text, const std::string &tone)
    {
        Shown(source, tone, text);
        ShowLogged(text, tone);
    }

    void ShowAlert(const std::string &source, const std::string &text)
    {
        Shown(source, "danger", text);
        OverlayToast(text, "danger", Setting("alertMs", 6000.0) / 1000.0);
    }

    // A toast whose text is in the log already.
    void ShowLogged(const std::string &text, const std::string &tone)
    {
        if (hud_->Notice(text, tone)) // over the game, events go to the HUD's top-centre log
            return;
        OverlayToast(text, tone, hud::ToastSeconds(text));
    }

    void Shown(const std::string &source, const std::string &tone, const std::string &text) const override
    {
        if (text.empty())
            return;
        const auto type = tone == "danger" ? Rml::Log::LT_ERROR : tone == "warn" ? Rml::Log::LT_WARNING : Rml::Log::LT_INFO;
        Rml::Log::Message(type, "shown %s (%s): %s", tone.c_str(), source.c_str(), text.c_str());
    }

    // `seconds`: how long it stays (hud::ToastSeconds for a toast, longer for an alert); a repeat
    // starts it again, a finger on it stops it (behaviour toast: hold / release).
    void OverlayToast(const std::string &text, const std::string &tone, double seconds)
    {
        if (!overlay_ || text.empty())
            return;
        // One place for every toast: the top edge (the HUD's event lines are there too).
        Rml::Element *stack = overlay_->GetElementById("toasts");
        if (!stack)
        {
            Rml::Log::Message(Rml::Log::LT_ERROR, "overlay.rml has no #toasts stack: toast dropped: %s", text.c_str());
            return;
        }
        const double until = system_.GetElapsedTime() + seconds;
        for (Toast_ &shown : toasts_)
            if (shown.text == text && shown.element && shown.leaving < 0.0)
            {
                shown.element->SetAttribute("text", hud::RepeatedText(text, ++shown.count));
                if (shown.held >= 0.0)
                    shown.held = seconds;
                else
                    shown.until = until;
                changed_ = true;
                return;
            }
        // At most three shown: the oldest shown one starts to leave.
        if (std::count_if(toasts_.begin(), toasts_.end(), [](const Toast_ &toast) { return toast.leaving < 0.0; }) >= 3)
            for (Toast_ &shown : toasts_)
                if (shown.leaving < 0.0)
                {
                    Leave(shown, system_.GetElapsedTime());
                    break;
                }
        Rml::ElementPtr notice = overlay_->CreateElement("c-notice");
        notice->SetAttribute("variant", "toast");
        notice->SetAttribute("place", "none");
        notice->SetAttribute("tone", tone.empty() ? "info" : tone);
        notice->SetAttribute("text", text);
        notice->SetAttribute("compact", "");
        changed_ = true;
        Toast_ toast;
        toast.element = stack->AppendChild(std::move(notice))->GetObserverPtr();
        toast.until = until;
        toast.text = text;
        toasts_.push_back(std::move(toast));
    }

    // A finger on an overlay toast (behaviour toast): its timer stops while held.
    void ToastTouched(ComponentElement &source, const std::string &verb)
    {
        const double now = system_.GetElapsedTime();
        for (Toast_ &toast : toasts_)
            if (toast.element.get() == &source && toast.leaving < 0.0)
            {
                if (verb == "hold" && toast.held < 0.0)
                    toast.held = std::max(0.0, toast.until - now);
                else if (verb == "release" && toast.held >= 0.0)
                {
                    toast.until = now + toast.held;
                    toast.held = -1.0;
                    changed_ = true;
                }
            }
    }

    bool SetProps(const std::string &element_id, const std::string &json) override
    {
        changed_ = true;
        Rml::Element *element = document_ ? document_->GetElementById(element_id) : nullptr;
        auto *component = dynamic_cast<ComponentElement *>(element);
        const Json props = Json::parse(json, nullptr, false);
        if (!component || !props.is_object())
            return false;
        component->SetProps(props);
        return true;
    }

    void ScrollTo(const std::string &element_id) override
    {
        changed_ = true;
        if (Rml::Element *element = document_ ? document_->GetElementById(element_id) : nullptr)
            element->ScrollIntoView(Rml::ScrollIntoViewOptions(Rml::ScrollAlignment::Start));
    }

    // --- game interfaces -----------------------------------------------------------

    void Answer(uint64_t request, bool applied, const std::string &reason) override
    {
        const auto started = std::chrono::steady_clock::now();
        router_->Answer(request, applied, reason);
        counters_.apply_ms += Milliseconds(started, std::chrono::steady_clock::now());
    }

    bool SetInterface(const std::string &json) override
    {
        InterfaceModel model;
        std::string error;
        if (!ParseInterfaceModel(json, model, &error))
        {
            Error(error);
            return false;
        }
        SetInterface(model);
        return true;
    }

    void SetInterface(const InterfaceModel &model) override
    {
        const auto started = std::chrono::steady_clock::now();
        if (model.opening != switch_.opening)
        {
            if (!switch_.open)
                switch_.Begin(started, false);
            switch_.opening = model.opening;
            switch_.screen = model.screen;
            switch_.modelled = started;
        }
        game_interface_ = true;
        game_screen_ = model.screen;
        router_->SetInterface(model);
        counters_.apply_ms += Milliseconds(started, std::chrono::steady_clock::now());
        if (opening_ == Opening::AwaitModel)
            opening_ = Opening::AwaitFrame;
        PublishTouchMap();
    }

    void CloseInterface() override
    {
        switch_.open = false;
        game_interface_ = false;
        router_->CloseGameInterface(game_screen_);
        game_screen_.clear();
        if (opening_ != Opening::None)
        {
            opening_ = Opening::None;
            if (!held_.empty())
                Log(std::to_string(held_.size()) + " touch events dropped: the interface closed before it was shown; " + RoutingFacts());
            held_.clear();
            held_pointers_.clear();
        }
        PublishTouchMap();
    }

    void ExpectInterface() override
    {
        if (!switch_.open)
            switch_.Begin(std::chrono::steady_clock::now(), true);
        changed_ = true;
        opening_ = Opening::AwaitModel;
        PublishTouchMap();
    }

    bool SetState(const std::string &channel, const std::string &json) override
    {
        const Json state = Json::parse(json, nullptr, false);
        if (state.is_discarded())
        {
            Error("state channel " + channel + ": malformed JSON");
            return false;
        }
        if (channel == "video")
            return SetVideo(state);
        const auto started = std::chrono::steady_clock::now();
        try
        {
            router_->SetState(channel, state);
            counters_.apply_ms += Milliseconds(started, std::chrono::steady_clock::now());
        }
        catch (const Json::exception &error)
        {
            Error("state channel " + channel + ": " + error.what());
            return false;
        }
        PublishTouchMap(); // a sheet the state opened owns the touches from now
        return true;
    }

    void SubmitText(const std::string &token, const std::string &text, bool accepted) override
    {
        changed_ = true;
        router_->SubmitText(token, text, accepted);
    }

    bool Mirrored() const override
    {
        return left_handed_;
    }
    bool ScreenMounted() const override
    {
        return router_->Active() || document_ != nullptr;
    }

    bool InterfaceActive() const override
    {
        // hud::Mount too: a plain document covers the game as well; a full-screen video covers both
        return !video_ && (router_->Active() || document_ != nullptr);
    }

    // Channel "video" ({"fullscreen": true} or null, engine aviplayer.cpp): while a game video
    // plays over the whole screen the interface screen is hidden and takes no touch, so the
    // video is seen and a tap reaches the game (the player's tap-to-skip). The screen itself
    // stays mounted: the script that started the video may leave its interface open (the main
    // menu's «Создатели» exits without a release) and it returns as it was.
    bool SetVideo(const Json &state)
    {
        if (!state.is_null() && !(state.is_object() && state.contains("fullscreen") && state["fullscreen"].is_boolean()))
        {
            Error("state channel video: expected {fullscreen: bool} or null, got " + state.dump());
            return false;
        }
        const bool video = state.is_object() && state["fullscreen"].get<bool>();
        if (video == video_)
            return true;
        video_ = video;
        if (video_)
            CancelInput();
        ShowDocument();
        PublishTouchMap();
        return true;
    }

    void ShowDocument()
    {
        changed_ = true;
        if (document_)
            video_ ? document_->Hide() : document_->Show();
    }

    bool WantsTouch(float x, float y) const override
    {
        const std::lock_guard<std::mutex> lock(touch_mutex_);
        if (touch_map_.everywhere)
            return true;
        for (const Rml::Rectanglef &region : touch_map_.regions)
            if (region.Contains({x, y}))
                return true;
        return false;
    }

    void CancelInput() override
    {
        changed_ = true;
        if (!held_.empty())
            Log(std::to_string(held_.size()) + " touch events dropped: the platform cancelled the input while they waited; " + RoutingFacts());
        held_.clear();
        held_pointers_.clear();
        std::vector<std::pair<int, Rml::Vector2f>> held;
        for (const auto &capture : captures_)
            held.emplace_back(capture.first, capture.second.pointer.position);
        for (const auto &pointer : held)
            Touch(TouchPhase::Cancel, pointer.first, pointer.second.x, pointer.second.y);
        hud_->Released();
    }

    bool WantsBack() const override
    {
        const std::lock_guard<std::mutex> lock(touch_mutex_);
        return touch_map_.back;
    }

    void Back() override
    {
        changed_ = true;
        if (config_.trace_actions)
        {
            BeginTrace("back");
            tracer_.Named("back");
        }
        bool hud_sheet = false;
        {
            const std::lock_guard<std::mutex> lock(touch_mutex_);
            hud_sheet = touch_map_.back;
        }
        if (router_->WantsBack()) // a screen's own modal (the dialogue's answer editor)
            router_->Back();
        else if (hud_sheet || !router_->Active())
            hud_->Back();
        else
            router_->Back();
    }

    // --- hud::Mount ---------------------------------------------------------------

    bool ShowHud(const hud::View &view) override
    {
        const std::string problem = hud::CheckView(layout_, view);
        if (!problem.empty())
        {
            // The scene rebuilds the same view each frame: the error once, not a toast ×N.
            if (problem != hud_problem_)
                Error(problem);
            hud_problem_ = problem;
            return false;
        }
        if (!hud_problem_.empty())
            Log("HUD view drawable again after: " + hud_problem_);
        hud_problem_.clear();
        if (!hud_document_)
        {
            hud_document_ = hud_path_.empty() ? nullptr : LoadPackDocument(hud_path_);
            if (!hud_document_)
            {
                Error("UI pack has no HUD document");
                return false;
            }
            ApplyHudButtonOpacity();
            if (overlay_)
                overlay_->PullToFront();
        }
        auto *root = dynamic_cast<ComponentElement *>(hud_document_->GetElementById("hud"));
        if (root && !ComponentSlot(*root))
            root->RebuildNow();
        Rml::Element *layer = root ? ComponentSlot(*root) : nullptr;
        if (!layer)
        {
            Error("UI pack: the HUD document has no <s-hud id=\"hud\"> layer");
            return false;
        }
        if (!hud_document_->IsVisible())
        {
            changed_ = true;
            hud_document_->Show(Rml::ModalFlag::None, Rml::FocusFlag::None);
        }
        // Elements are patched in place: one that both the old and the new view have stays
        // mounted (a held stick survives explore ↔ combat), only its place and props follow.
        const std::vector<hud::LayoutElement> &elements = layout_.at(view.layout);
        std::unordered_map<std::string, std::string> wanted; // id → what it must have been created as
        for (const hud::LayoutElement &element : elements)
            if (view.elements.contains(element.id))
            {
                std::string made = element.component + "\n" + element.behavior + "\n" + element.data.dump();
                for (const hud::LayoutElement &child : element.children)
                    if (view.elements.contains(child.id))
                        made += "\n" + child.id + " " + child.component;
                wanted[element.id] = std::move(made);
            }
        for (int i = layer->GetNumChildren(); i-- > 0;)
        {
            Rml::Element *mounted = layer->GetChild(i);
            const auto keep = wanted.find(mounted->GetId());
            if (mounted->GetId() == "sheet" || mounted->GetId() == "overlay" || (keep != wanted.end() && hud_made_[mounted->GetId()] == keep->second))
                continue;
            RemoveHudElement(layer, mounted);
        }
        bool complete = true;
        Rml::Element *before = layer->GetElementById("overlay"); // layout order: later elements are drawn on top
        if (!before)
            before = layer->GetElementById("sheet");
        for (size_t i = elements.size(); i-- > 0;)
        {
            const hud::LayoutElement &element = elements[i];
            if (!wanted.count(element.id))
                continue;
            Rml::Element *mounted = hud_document_->GetElementById(element.id);
            if (!mounted)
            {
                Rml::ElementPtr created = CreateHudElement(element);
                for (const hud::LayoutElement &child : element.children)
                    if (view.elements.contains(child.id))
                    {
                        Rml::ElementPtr inner = CreateHudElement(child);
                        inner->SetAttribute("style", "width: 100%;");
                        created->AppendChild(std::move(inner));
                    }
                mounted = before ? layer->InsertBefore(std::move(created), before) : layer->AppendChild(std::move(created));
                hud_made_[element.id] = wanted[element.id];
            }
            before = mounted;
            const std::string &style = left_handed_ ? element.rcss_mirrored : element.rcss;
            if (mounted->GetAttribute<Rml::String>("style", "") != style)
            {
                changed_ = true;
                mounted->SetAttribute("style", style);
            }
            complete = SetHudProps(element, view) && complete;
            for (const hud::LayoutElement &child : element.children)
                if (view.elements.contains(child.id))
                    complete = SetHudProps(child, view) && complete;
            const bool dim = std::find(view.dimmed.begin(), view.dimmed.end(), element.id) != view.dimmed.end();
            if (mounted->IsClassSet("dim") != dim)
            {
                changed_ = true;
                mounted->SetClass("dim", dim);
            }
        }
        // The overlay (the spyglass card) lies over the scene's elements and under the sheet.
        Rml::Element *overlay = hud_document_->GetElementById("overlay");
        if (overlay && overlay->GetTagName() != view.overlay)
        {
            RemoveHudElement(layer, overlay);
            overlay = nullptr;
        }
        if (!view.overlay.empty())
        {
            if (!overlay)
            {
                Rml::ElementPtr created = hud_document_->CreateElement(view.overlay);
                created->SetId("overlay");
                Rml::Element *above = layer->GetElementById("sheet");
                overlay = above ? layer->InsertBefore(std::move(created), above) : layer->AppendChild(std::move(created));
            }
            if (auto *component = dynamic_cast<ComponentElement *>(overlay))
                component->SetProps(view.overlay_props);
            else
            {
                Error("UI pack has no HUD overlay " + view.overlay);
                complete = false;
            }
        }
        // The sheet lies over everything else of the scene.
        Rml::Element *sheet = hud_document_->GetElementById("sheet");
        if (sheet && sheet->GetTagName() != view.sheet)
        {
            RemoveHudElement(layer, sheet);
            sheet = nullptr;
        }
        if (!view.sheet.empty())
        {
            if (!sheet)
            {
                changed_ = true;
                Rml::ElementPtr created = hud_document_->CreateElement(view.sheet);
                created->SetId("sheet");
                sheet = layer->AppendChild(std::move(created));
            }
            if (auto *component = dynamic_cast<ComponentElement *>(sheet))
            {
                Json &shown = hud_props_["sheet"]; // forgotten with the sheet (RemoveHudElement)
                if (shown != view.sheet_props)
                {
                    component->SetProps(view.sheet_props);
                    shown = view.sheet_props;
                }
            }
            else
            {
                Error("UI pack has no HUD sheet " + view.sheet);
                complete = false;
            }
        }
        float clear = 0.0f;
        for (const hud::LayoutElement &element : elements)
            if (element.toast_clear > 0.0f && view.elements.contains(element.id))
                clear = std::max(clear, element.toast_clear);
        SetToastClear(clear);
        return complete;
    }

    bool Measure(const std::string &id, const std::string &selector, hud::Placement &out) const override
    {
        Rml::Element *element = hud_document_ && hud_document_->IsVisible() ? hud_document_->GetElementById(id) : nullptr;
        if (element && !selector.empty())
            element = element->QuerySelector(selector);
        if (!element || !element->IsVisible(true))
            return false;
        const Rml::Vector2f origin = element->GetAbsoluteOffset(Rml::BoxArea::Border), size = element->GetBox().GetSize(Rml::BoxArea::Border);
        if (size.x <= 0.0f || size.y <= 0.0f)
            return false;
        auto *root = dynamic_cast<ComponentElement *>(hud_document_->GetElementById("hud"));
        Rml::Element *layer = root ? ComponentSlot(*root) : nullptr;
        const Rml::Vector2f corner = layer ? layer->GetAbsoluteOffset(Rml::BoxArea::Border) : Rml::Vector2f(0.0f, 0.0f);
        const Rml::Vector2f extent = layer ? layer->GetBox().GetSize(Rml::BoxArea::Border)
                                           : Rml::Vector2f(static_cast<float>(config_.width), static_cast<float>(config_.height));
        out = {origin.x, origin.y, size.x, size.y, corner.x, corner.y, static_cast<float>(config_.width), static_cast<float>(config_.height), config_.density,
               extent.x, extent.y};
        return true;
    }

    void HideHud() override
    {
        if (!hud_document_)
            return;
        if (hud_document_->IsVisible())
            changed_ = true;
        auto *root = dynamic_cast<ComponentElement *>(hud_document_->GetElementById("hud"));
        if (Rml::Element *layer = root ? ComponentSlot(*root) : nullptr)
            while (layer->GetNumChildren() > 0)
                RemoveHudElement(layer, layer->GetLastChild());
        hud_document_->Hide();
        SetToastClear(0.0f);
    }

    // --- screens::Surface ---------------------------------------------------------

    void Show(const std::string &tag, const Json &props) override
    {
        changed_ = true;
        if (!screen_root_ && (screen_path_.empty() || !OpenDocument(screen_path_)))
        {
            Error("UI pack has no screen document");
            return;
        }
        screen_root_ = document_;
        Rml::Element *mounted = document_->GetElementById("screen");
        if (!mounted || mounted->GetTagName() != tag)
        {
            if (mounted)
                document_->RemoveChild(mounted);
            Rml::ElementPtr created = document_->CreateElement(tag);
            created->SetId("screen");
            mounted = document_->AppendChild(std::move(created));
        }
        if (auto *component = dynamic_cast<ComponentElement *>(mounted))
            component->SetProps(props);
        else
            Error("UI pack has no screen " + tag);
        if (video_)
            ShowDocument();
        PublishTouchMap();
    }

    void Hide() override
    {
        changed_ = true;
        captures_.clear();
        if (screen_root_ && document_)
        {
            document_->Close();
            document_ = nullptr;
        }
        screen_root_ = nullptr;
        PublishTouchMap();
    }

    void Send(const Action &action) override
    {
        if (config_.on_action)
            config_.on_action(action);
    }

    void RequestText(TextRequest request, const std::string &anchor) override
    {
        if (Rml::Element *element = document_ ? document_->GetElementById(anchor) : nullptr)
        {
            // A field marks the box the text sits in (c-field: data-action="focus"); the
            // editor goes exactly over it, not over the label and error lines.
            if (Rml::Element *box = element->QuerySelector("[data-action=focus]"))
                element = box;
            const Rml::Vector2f origin = element->GetAbsoluteOffset(Rml::BoxArea::Border);
            const Rml::Vector2f size = element->GetBox().GetSize(Rml::BoxArea::Border);
            request.x = origin.x;
            request.y = origin.y;
            request.width = size.x;
            request.height = size.y;
        }
        if (config_.on_text_request)
            config_.on_text_request(request);
        else
            router_->SubmitText(request.token, {}, false);
    }

    void Toast(const std::string &text) override
    {
        Show("screen " + router_->Using(), text, "info");
    }

    void SetLanguage(const std::string &game_language) override
    {
        static const char *const codes[][2] = {{"russian", "ru"}, {"english", "en"}, {"german", "de"},
                                               {"french", "fr"},  {"spanish", "es"}, {"italian", "it"}};
        std::string code;
        for (const auto &pair : codes)
            if (SameNode(game_language, pair[0]))
                code = pair[1];
        if (!strings_.count(code))
        {
            // A game language the pack has no strings for (a mod, an empty name): English,
            // and said so once per language.
            if (unknown_languages_.insert(game_language).second)
                Error("no UI strings for the game language '" + game_language + "'; using en");
            code = "en";
        }
        language_ = code;
    }

    std::string Text(const std::string &key) override
    {
        const auto table = strings_.find(language_);
        if (table != strings_.end())
        {
            const auto found = table->second.find(key);
            if (found != table->second.end() && found->is_string())
                return found->get<std::string>();
        }
        if (missing_strings_.insert(language_ + ":" + key).second)
            Error("UI string '" + key + "' is missing for language '" + language_ + "'");
        return key;
    }

    void Log(const std::string &line) override
    {
        Rml::Log::Message(Rml::Log::LT_INFO, "%s", line.c_str());
    }

    void Error(const std::string &line) override
    {
        Rml::Log::Message(Rml::Log::LT_ERROR, "%s", line.c_str());
        if (config_.show_errors)
            ShowLogged(line, "danger");
    }

    // A modal of the screen refused a tap beside it or the back key (Bindings::Modal without a
    // cancel): its component shows it (C-Dialog shakes).
    void Refuse(const std::string &id) override
    {
        auto *component = document_ ? dynamic_cast<ComponentElement *>(document_->GetElementById(id)) : nullptr;
        if (!component || !component->Refuse())
            Error("refused modal " + id + ": no component with that id can show it");
        changed_ = true;
    }

    // Every tap the screen document offers (Behavior::Taps: id as Emit finds it, verb) is one its screen maps
    // (Router::Maps): an unmapped one is an error, once per screen, element and verb.
    void CheckTaps(Rml::Element *element)
    {
        if (auto *component = dynamic_cast<ComponentElement *>(element); component && component->HasBehaviors())
        {
            std::vector<Tap> taps;
            component->Taps(taps);
            std::string id;
            for (Rml::Element *at = component; at && id.empty(); at = at->GetParentNode())
                id = at->GetId();
            for (const Tap &tap : taps)
                if (!router_->Maps(id, tap.verb) && unmapped_taps_.insert(router_->Using() + ' ' + id + ' ' + tap.verb).second)
                    Error("screen " + router_->Using() + ": " + component->GetTagName() + " " + (id.empty() ? "(no id)" : id) + " " + tap.verb +
                          " is drawn with no mapping: a tap on it does nothing");
        }
        for (int i = 0; i < element->GetNumChildren(); ++i)
            CheckTaps(element->GetChild(i));
    }

    // Config::log_taps: every touch region of every drawn component, one line each, as a block logged when it
    // differs from the last one. `at` is the middle of what of the region its scrolling ancestors leave visible
    // ("-" when nothing), `reach` whether a pointer going down there is taken by this component (not by a
    // scrim, a modal or another component over it), `hold` whether it acts only when held (hold-to-confirm), `st` a
    // hash of the component's props (what it shows changed).
    void LogTaps()
    {
        std::vector<std::string> lines;
        for (int d = 0; d < context_->GetNumDocuments(); ++d)
            if (Rml::ElementDocument *document = context_->GetDocument(d); document && document->IsVisible())
                MapTaps(document, document == hud_document_ ? "hud" : document == document_ ? (router_->Using().empty() ? "document" : router_->Using()) : "layer", lines);
        std::string joined;
        for (const std::string &line : lines)
            joined += line + '\n';
        if (joined == logged_taps_)
            return;
        logged_taps_ = joined;
        ++taps_block_;
        Log("taps begin " + std::to_string(taps_block_) + " " + std::to_string(lines.size()) + " " + std::to_string(config_.width) + "x" +
            std::to_string(config_.height));
        for (const std::string &line : lines)
            Log("taps " + std::to_string(taps_block_) + " " + line);
        Log("taps end " + std::to_string(taps_block_));
    }

    void MapTaps(Rml::Element *element, const std::string &where, std::vector<std::string> &lines)
    {
        if (!element->IsVisible(true))
            return;
        if (auto *component = dynamic_cast<ComponentElement *>(element); component && component->HasBehaviors())
        {
            std::string id;
            for (Rml::Element *at = component; at && id.empty(); at = at->GetParentNode())
                id = at->GetId();
            // The behaviours' touch regions (enabled or not) and every part a tap is offered on (Behavior::Taps: a press
            // row with action parts is a tap besides them too). A region with no offered tap is disabled.
            std::vector<Rml::Element *> parts;
            component->TouchRegions(parts);
            std::vector<Tap> taps;
            component->Taps(taps);
            for (const Tap &tap : taps)
                if (std::find(parts.begin(), parts.end(), tap.part) == parts.end())
                    parts.insert(tap.part == component ? parts.begin() : parts.end(), tap.part);
            const auto untouchable = [](Rml::Element *e) { return e->GetComputedValues().pointer_events() == Rml::Style::PointerEvents::None; };
            for (Rml::Element *part : parts)
            {
                const Rml::Vector2f size = part->GetBox().GetSize(Rml::BoxArea::Border);
                Rml::Element *owner = part;
                while (owner && !dynamic_cast<ComponentElement *>(owner))
                    owner = owner->GetParentNode();
                if (owner != component || size.x <= 0.0f || size.y <= 0.0f || !part->IsVisible(true) || untouchable(part))
                    continue; // a nested component's own part (listed with it); pointer-events: none: it only shows something
                const Rml::Rectanglef box = Rml::Rectanglef::FromPositionSize(part->GetAbsoluteOffset(Rml::BoxArea::Border), size);
                Rml::Rectanglef shown = box;
                for (Rml::Element *clip = part->GetParentNode(); clip; clip = clip->GetParentNode())
                    if (clip->GetComputedValues().overflow_x() != Rml::Style::Overflow::Visible ||
                        clip->GetComputedValues().overflow_y() != Rml::Style::Overflow::Visible)
                        shown = shown.Intersect(Rml::Rectanglef::FromPositionSize(clip->GetAbsoluteOffset(Rml::BoxArea::Padding),
                                                                                  clip->GetBox().GetSize(Rml::BoxArea::Padding)));
                shown = shown.Intersect(Rml::Rectanglef::FromSize({float(config_.width), float(config_.height)}));
                std::string at = "-";
                bool reach = false;
                if (shown.Width() >= 1.0f && shown.Height() >= 1.0f)
                {
                    const Rml::Vector2f middle = shown.Center();
                    at = std::to_string(int(middle.x)) + "," + std::to_string(int(middle.y));
                    Pointer pointer;
                    pointer.position = pointer.start = middle;
                    reach = InteractiveComponentAt(context_->GetElementAtPoint(middle), pointer) == component;
                }
                const auto offered = std::find_if(taps.begin(), taps.end(), [part](const Tap &tap) { return tap.part == part; });
                const std::string verb = offered != taps.end() ? offered->verb
                                         : part == component ? "click"
                                                             : part->GetAttribute<Rml::String>("data-action", "click");
                // Behaviours that report no taps (a stick, a lever, a swipe) take their regions as they are.
                const bool disabled = taps.empty() ? component->Flag("disabled") : offered == taps.end();
                const bool hold = offered != taps.end() && offered->hold;
                std::string text;
                TapText(part, text);
                char line[200];
                std::snprintf(line, sizeof line, " %s %s %s box=%d,%d,%d,%d at=%s reach=%d dis=%d hold=%d st=%08x \"", component->GetTagName().c_str(),
                              id.empty() ? "-" : id.c_str(), verb.c_str(), int(box.Left()), int(box.Top()), int(box.Width()), int(box.Height()),
                              at.c_str(), reach ? 1 : 0, disabled ? 1 : 0, hold ? 1 : 0,
                              unsigned(std::hash<std::string>{}(component->Props().dump()) & 0xffffffffu));
                size_t cut = std::min<size_t>(text.size(), 60);
                while (cut > 0 && cut < text.size() && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80)
                    --cut; // not inside a UTF-8 sequence
                lines.push_back(where + line + text.substr(0, cut) + "\"");
            }
        }
        for (int i = 0; i < element->GetNumChildren(); ++i)
            MapTaps(element->GetChild(i), where, lines);
    }

    static void TapText(Rml::Element *element, std::string &text)
    {
        if (text.size() >= 60 || !element->IsVisible(true))
            return;
        if (auto *node = rmlui_dynamic_cast<Rml::ElementText *>(element))
        {
            for (char c : node->GetText())
                if (c != '"' && c != '\n' && c != '\r' && c != '\t' && !(c == ' ' && (text.empty() || text.back() == ' ')))
                    text += c;
            if (!text.empty() && text.back() != ' ')
                text += ' ';
        }
        for (int i = 0; i < element->GetNumChildren(); ++i)
            TapText(element->GetChild(i), text);
    }

    // --- Services -----------------------------------------------------------------

    double Now() const override
    {
        return const_cast<System &>(system_).GetElapsedTime();
    }
    float Density() const override
    {
        return config_.density;
    }
    Json SafeArea() const override
    {
        return safe_;
    }
    double Setting(const std::string &name, double fallback) const override
    {
        const auto found = settings_.find(name);
        return found != settings_.end() && found->is_number() ? found->get<double>() : fallback;
    }
    void Emit(ComponentElement &source, const std::string &verb, const std::string &value) override
    {
        std::string id;
        for (Rml::Element *element = &source; element && id.empty(); element = element->GetParentNode())
            id = element->GetId();
        if (tracer_.Active())
            tracer_.Named((hud_document_ && source.GetOwnerDocument() == hud_document_ ? std::string("hud")
                           : screen_root_ && source.GetOwnerDocument() == screen_root_ ? ScreenTag()
                                                                                          : std::string("document")) +
                          " " + id + " " + verb);
        if (overlay_ && source.GetOwnerDocument() == overlay_)
        {
            ToastTouched(source, verb); // the overlay holds only the toasts and the tooltip
            return;
        }
        if (hud_document_ && source.GetOwnerDocument() == hud_document_)
        {
            // A stick or lever let go is no tap with an answer: what the HUD shows next is the game's live state.
            if (verb == "change")
                tracer_.Drop();
            // The HUD: its scene maps the event to engine input.
            hud_->OnEvent({id, verb, value});
            return;
        }
        if (screen_root_)
        {
            // A game interface: its screen maps the event to engine actions.
            const uint64_t predicted = router_->Predicted();
            router_->OnEvent({id, verb, value});
            if (router_->Predicted() != predicted)
                tracer_.Predicted();
            return;
        }
        if (!config_.on_action)
            return;
        Action action;
        action.node = id;
        action.verb = verb;
        action.value = value;
        config_.on_action(action);
    }
    void Tooltip(Rml::Element *anchor, const std::string &text, bool below) override
    {
        changed_ = true;
        Rml::Element *tip = overlay_ ? overlay_->GetElementById("tooltip") : nullptr;
        if (!tip)
            return;
        if (!anchor || text.empty())
        {
            tip->SetClass("open", false);
            return;
        }
        const Rml::Vector2f origin = anchor->GetAbsoluteOffset(Rml::BoxArea::Border);
        const Rml::Vector2f size = anchor->GetBox().GetSize(Rml::BoxArea::Border);
        const float gap = 6.0f * config_.density;
        const float width = std::min(static_cast<float>(config_.width) - 2.0f * gap, std::max(size.x, 280.0f * config_.density));
        const float left = std::clamp(origin.x, gap, static_cast<float>(config_.width) - gap - width);
        tip->SetInnerRML(EscapeRml(BindShortWords(text)));
        tip->SetProperty(Rml::PropertyId::Left, Rml::Property(left, Rml::Unit::PX));
        tip->SetProperty(Rml::PropertyId::Width, Rml::Property(width, Rml::Unit::PX));
        if (below)
        {
            tip->SetProperty(Rml::PropertyId::Top, Rml::Property(origin.y + size.y + gap, Rml::Unit::PX));
            tip->RemoveProperty(Rml::PropertyId::Bottom);
        }
        else
        {
            tip->SetProperty(Rml::PropertyId::Bottom, Rml::Property(static_cast<float>(config_.height) - origin.y + gap, Rml::Unit::PX));
            tip->RemoveProperty(Rml::PropertyId::Top);
        }
        tip->SetClass("open", true);
    }
    void RequestTicks(ComponentElement &component, bool enable) override
    {
        const auto found = std::find_if(ticking_.begin(), ticking_.end(), [&](const auto &entry) { return entry.get() == &component; });
        if (enable && found == ticking_.end())
            ticking_.push_back(component.GetObserverPtr());
        else if (!enable && found != ticking_.end())
            ticking_.erase(found);
    }
    void PressStarted(ComponentElement &component) override
    {
        // Game interfaces only (their scripts' click); the HUD and the application screens (the
        // launcher and the downloads, state channels of their own process) have none.
        if (!game_interface_ || !screen_root_ || component.GetOwnerDocument() != screen_root_)
            return;
        // A part that is no button in the game (the interlude: its Esc is silent) sets constants.pressSound false.
        if (!component.Spec().constants.value("pressSound", true))
            return;
        Action action;
        action.screen = "sound";
        action.verb = "press";
        Send(action);
    }

    FrameStats &Counters() override
    {
        return counters_;
    }
    void Forget(ComponentElement &component) override
    {
        ticking_.erase(std::remove_if(ticking_.begin(), ticking_.end(), [&](const auto &entry) { return !entry || entry.get() == &component; }),
                       ticking_.end());
    }

  private:
    // Where a pointer going down belongs to the UI; read by the thread that gets touches.
    struct TouchMap
    {
        bool everywhere = false;
        bool back = false;
        std::vector<Rml::Rectanglef> regions; // physical pixels
    };

    // --- HUD mount ---

    Rml::ElementPtr CreateHudElement(const hud::LayoutElement &element)
    {
        changed_ = true;
        Rml::ElementPtr created = hud_document_->CreateElement(element.component);
        created->SetId(element.id);
        if (!element.behavior.empty())
            created->SetAttribute("data-behavior", element.behavior);
        for (const auto &item : element.data.items())
            created->SetAttribute("data-" + item.key(), ToText(item.value()));
        return created;
    }

    // Layout props with the scene's live ones on top; a rebuild only when they changed.
    bool SetHudProps(const hud::LayoutElement &element, const hud::View &view)
    {
        auto *component = dynamic_cast<ComponentElement *>(hud_document_->GetElementById(element.id));
        if (!component)
        {
            Error("UI pack has no component <" + element.component + "> for HUD element " + element.id);
            return false;
        }
        Json props = element.props;
        if (left_handed_)
            props.update(element.props_mirrored);
        props.update(view.elements[element.id]);
        Json &shown = hud_props_[element.id];
        if (shown != props)
        {
            Json change = props;
            if (shown.is_object()) // a prop the scene no longer gives goes back to its default
                for (const auto &item : shown.items())
                    if (!props.contains(item.key()))
                        change[item.key()] = nullptr;
            component->SetProps(change);
            shown = std::move(props);
        }
        return true;
    }

    // Takes a HUD element off the screen. A pointer held on it ends first, so that its
    // control reports the release (a stick stops, a held button lets go).
    void RemoveHudElement(Rml::Element *layer, Rml::Element *mounted)
    {
        changed_ = true;
        std::vector<std::pair<int, Rml::Vector2f>> held;
        for (const auto &capture : captures_)
            for (Rml::Element *at = capture.second.component.get(); at; at = at->GetParentNode())
                if (at == mounted)
                {
                    held.emplace_back(capture.first, capture.second.pointer.position);
                    break;
                }
        for (const auto &pointer : held)
            Touch(TouchPhase::Cancel, pointer.first, pointer.second.x, pointer.second.y);
        std::vector<Rml::Element *> inside = {mounted};
        while (!inside.empty())
        {
            Rml::Element *element = inside.back();
            inside.pop_back();
            hud_props_.erase(element->GetId());
            for (int i = 0; i < element->GetNumChildren(); ++i)
                inside.push_back(element->GetChild(i));
        }
        hud_made_.erase(mounted->GetId());
        layer->RemoveChild(mounted);
    }

    void CollectRegions(Rml::Element *element, std::vector<Rml::Rectanglef> &out) const
    {
        if (!element->IsVisible(true))
            return;
        if (auto *component = dynamic_cast<ComponentElement *>(element))
        {
            std::vector<Rml::Element *> parts;
            component->TouchRegions(parts);
            for (Rml::Element *part : parts)
            {
                const Rml::Vector2f size = part->GetBox().GetSize(Rml::BoxArea::Border);
                if (size.x > 0.0f && size.y > 0.0f && part->IsVisible(true))
                    out.push_back(Rml::Rectanglef::FromPositionSize(part->GetAbsoluteOffset(Rml::BoxArea::Border), size));
            }
        }
        for (int i = 0; i < element->GetNumChildren(); ++i)
            CollectRegions(element->GetChild(i), out);
    }

    // Called after everything that changes what a touch would hit (a frame, a document,
    // an interface, a state): the answer of WantsTouch never waits for this thread. Outside a
    // frame (`laid_out` false) what now covers the game is not laid out yet: pointers that go
    // down on it are held until the next frame (Touch, ReleaseHeld).
    // The HUD's regions (a walk over its whole document) are collected again only when they
    // can have moved since the last collection (regions_moved_): a frame that laid out or
    // changed elements, a scroll, a host call. Before that, outside a frame,
    // the last laid-out regions stay; a frame or a pass with nothing new reuses them. With
    // Config::verify_unchanged a reuse is checked against a fresh walk.
    void PublishTouchMap(bool laid_out = false)
    {
        TouchMap map;
        const bool hud = hud_->Interactive();
        const bool modal = !video_ && ((hud && hud_->Shown().modal) || router_->WantsBack()); // a modal HUD sheet or a screen's own modal
        map.back = modal || (!video_ && hud && hud_->Shown().back); // or a scene that takes the key without one (the raised spyglass)
        const bool was_between = between_;
        between_ = Between();
        // a document or an interface screen (also one the game opened whose model is still to
        // come), a modal HUD sheet, or nothing the player can use (Between)
        map.everywhere = !video_ && (document_ != nullptr || modal || opening_ != Opening::None || between_);
        if (!laid_out)
            regions_moved_ = true; // a host call: the next frame lays out what it changed
        if (hud && !map.everywhere && hud_document_)
        {
            if (!regions_current_ || (laid_out && regions_moved_))
            {
                hud_regions_.clear();
                CollectRegions(hud_document_, hud_regions_);
                regions_current_ = true;
                if (laid_out)
                    regions_moved_ = false;
            }
            else if (laid_out && config_.verify_unchanged)
            {
                std::vector<Rml::Rectanglef> fresh;
                CollectRegions(hud_document_, fresh);
                if (fresh != hud_regions_)
                    Error("UI touch map out of date: HUD touch regions moved without a layout, an element change, a scroll or a host call");
            }
            map.regions = hud_regions_;
        }
        else
            regions_current_ = false; // the HUD takes no part: collected afresh when it does again
        const std::lock_guard<std::mutex> lock(touch_mutex_);
        // What comes after a transition is not laid out before the next frame either.
        if (!laid_out && map.everywhere && (!touch_map_.everywhere || (was_between && !between_)))
            unlaid_ = true;
        touch_map_ = std::move(map);
    }

    // The game is between scenes and shows nothing the player can use: no interface (nor one
    // being opened), no full-screen video, no telescope, no gameplay mode a HUD
    // scene holds, and the engine says why there is no mode: it is starting, loading, has no
    // scene yet, or launched an interface that is not shown yet (Gameplay::blocked). A touch the
    // game got then would land in whatever comes next unseen, so the UI owns the touches and holds
    // them for that: the next interface gets them, gameplay drops them (ReleaseHeld). Every
    // other reason (a cutscene, a dialogue, a command menu, a control group no mode knows, no
    // native player) leaves the touches to the game as before. States decide it, not a timer.
    // One log line per screen switch ("UI switch"), also in the player's shared log: from the
    // engine action that led to it (when there was one since the last switch) through the game
    // opening its interface and the model arriving to the screen's first frame, and on until
    // its frames stop rebuilding or laying out (pictures that arrived, re-cut text): the CPU
    // of those frames by stage (FrameStats). GPU time is the renderer's.
    // A drawn frame whose UI work alone took kSlowFrameMs or more, with its split: what a stutter in the game's
    // "ui" phase was. At most one line
    // every two seconds; a screen switch has its own report.
    std::vector<ActionTrace> TakeActionTraces(bool finish_open) override
    {
        if (finish_open)
            Traced(tracer_.Finish(true));
        return std::exchange(traces_, {});
    }

    // --- the flicker check (Config::trace_actions, action_trace.h) ----------------

    std::string ScreenTag() const
    {
        const Rml::Element *mounted = document_ ? document_->GetElementById("screen") : nullptr;
        return mounted ? mounted->GetTagName() : std::string("screen");
    }

    // The content on the screen: the screen or plain document and the HUD (toasts and the tooltip pass over
    // everything and are not part of what an action shows).
    FrameContent TraceContent(std::string shown)
    {
        Rml::ElementDocument *const documents[] = {document_, hud_document_};
        return CaptureContent(documents, float(config_.width), float(config_.height), std::move(shown), system_.GetElapsedTime());
    }

    // A player's action starts with what the screen shows now: the frame on the screen, unless something changed
    // since it was drawn (then that change is the first thing the action shows).
    void BeginTrace(const std::string &what)
    {
        Traced(tracer_.Begin(what, TraceContent(last_frame_.shown), float(config_.width), float(config_.height)));
    }

    void Traced(std::optional<ActionTrace> trace)
    {
        if (!trace)
            return;
        const std::string line = DescribeTrace(*trace);
        if (trace->transient || trace->late)
            Rml::Log::Message(Rml::Log::LT_WARNING, "%s", line.c_str());
        else if (config_.log_taps || trace->transient)
            Log(line);
        traces_.push_back(std::move(*trace));
        if (traces_.size() > 512)
            traces_.erase(traces_.begin());
    }

    void ReportSlowFrame()
    {
        constexpr double kSlowFrameMs = 25.0;
        const FrameStats &f = counters_;
        const auto now = Clock::now();
        if (f.update_ms + f.render_ms < kSlowFrameMs || Milliseconds(slow_reported_, now) < 2000.0)
            return;
        slow_reported_ = now;
        char line[512];
        std::snprintf(line, sizeof line,
                      "UI slow frame: update %.1f + render %.1f ms | apply %.1f, rebuild %.1f (%d, %d elements touched), style %.1f, "
                      "layout %.1f (%d boxes, %d passes), pictures %d in %.1f, render incl. %d meshes, textures %d (%zu KB) in %.1f, "
                      "%d frames held before it",
                      f.update_ms, f.render_ms, f.apply_ms, f.rebuild_ms, f.rebuilds, f.touched, f.style_ms, f.layout_ms, f.laid_out,
                      f.layout_passes, f.pictures, f.picture_ms, f.geometry, f.textures, f.texture_bytes / 1024, f.texture_ms, f.held);
        Log(line);
    }

    void ReportSwitch(bool idle)
    {
        if (!switch_.open || switch_.modelled == Clock::time_point{})
            return;
        const auto now = Clock::now();
        if (!idle)
        {
            const FrameStats &frame = counters_;
            if (!switch_.drawn)
            {
                switch_.drawn = true;
                switch_.first = now;
                switch_.first_frame = frame;
            }
            else
            {
                FrameStats &sum = switch_.settle;
                sum.update_ms += frame.update_ms, sum.render_ms += frame.render_ms, sum.apply_ms += frame.apply_ms;
                sum.rebuild_ms += frame.rebuild_ms, sum.style_ms += frame.style_ms, sum.layout_ms += frame.layout_ms;
                sum.picture_ms += frame.picture_ms, sum.texture_ms += frame.texture_ms, sum.pictures += frame.pictures;
                sum.textures += frame.textures, sum.texture_bytes += frame.texture_bytes, sum.rebuilds += frame.rebuilds;
                sum.laid_out += frame.laid_out, sum.layout_passes += frame.layout_passes;
            }
            ++switch_.frames;
            const bool busy = frame.rebuilds > 0 || frame.laid_out > 0 || frame.pictures > 0 || frame.textures > 0;
            if ((busy || switch_.frames == 1) && switch_.frames < 30)
                return;
        }
        const auto ms = [](Clock::time_point from, Clock::time_point to) { return Milliseconds(from, to); };
        const bool acted = last_tap_ > reported_ && last_tap_ <= switch_.begun;
        const Clock::time_point origin = acted ? last_tap_ : switch_.begun;
        const FrameStats &f = switch_.first_frame, &s = switch_.settle;
        char line[1024];
        std::snprintf(line, sizeof line,
                      "UI switch %s: first frame %.0f ms after %s%s (game: %s%.0f ms to the model; %d frames held for its textures), settled %.0f ms after it in %d more frames | "
                      "first frame CPU: apply %.1f, rebuild %.1f (%d, %d elements touched), style %.1f, layout %.1f (%d boxes, %d passes), pictures %d in %.1f, "
                      "render %.1f incl. %d meshes, textures %d (%zu KB) in %.1f | settling CPU: update %.1f, render %.1f, rebuild %d, layout %.1f (%d passes), "
                      "pictures %d in %.1f",
                      switch_.screen.c_str(), ms(origin, switch_.first), acted ? "the tap" : "the interface opened", "", switch_.expected ? ("interface opened +" + std::to_string(int(ms(origin, switch_.begun))) + " ms, ").c_str() : "",
                      ms(origin, switch_.modelled), f.held, ms(switch_.first, now), switch_.frames - 1, f.apply_ms, f.rebuild_ms, f.rebuilds, f.touched, f.style_ms,
                      f.layout_ms, f.laid_out, f.layout_passes, f.pictures, f.picture_ms, f.render_ms, f.geometry, f.textures, f.texture_bytes / 1024, f.texture_ms,
                      s.update_ms, s.render_ms, s.rebuilds, s.layout_ms, s.layout_passes, s.pictures, s.picture_ms);
        Log(line);
        switch_.open = false;
        reported_ = now;
    }

    // FrameStats::shown.
    // What is shown without the model's revision: the screen and its opening, the HUD, nothing.

    std::string Shown() const
    {
        std::string shown = "nothing";
        if (video_)
            shown = "video";
        else if (Rml::Element *screen = screen_root_ && document_ ? document_->GetElementById("screen") : nullptr)
            shown = screen->GetTagName() + " " + router_->Model().screen + "#" + std::to_string(router_->Model().opening) + "." +
                    std::to_string(router_->Model().revision);
        else if (document_)
            shown = "document";
        else if (hud_document_ && hud_document_->IsVisible())
            shown = "hud";
        return opening_ == Opening::None ? shown : shown + " +opening";
    }

    bool Between() const
    {
        if (video_ || opening_ != Opening::None || InterfaceActive())
            return false;
        const hud::States &states = hud_->Current();
        // Engine states only (gameplay, video, spyglass): they arrive in the engine's own frame.
        // The presentation's cinematic flag is polled (every 200 ms, main.cpp) and stays stale
        // through a load that follows a video (the title).
        if ((states.spyglass.known && states.spyglass.on) || hud_->InGameplay())
            return false;
        const std::string &why = states.gameplay.blocked;
        return why == "starting" || why == "loading" || why == "transition" || why == "no-scene" || why == "interface";
    }

    // What decided a held or dropped touch, for the log (the player's shared log shows a state
    // that was missing).
    std::string RoutingFacts() const
    {
        const hud::States &states = hud_->Current();
        const hud::Presentation &shown = states.presentation;
        return "gameplay " + states.gameplay.mode + (states.gameplay.blocked.empty() ? "" : " (" + states.gameplay.blocked + ")") +
               ", presentation " + (shown.known ? "screen '" + shown.screen + "' loading " + std::to_string(shown.loading) + " cinematic " +
                                                      std::to_string(shown.cinematic) + " dialogue " + std::to_string(shown.dialogue) + " live " +
                                                      std::to_string(shown.live)
                                                : std::string("unknown")) +
               ", interface " + std::to_string(InterfaceActive()) + ", video " + std::to_string(video_);
    }

    bool Holding() const
    {
        return opening_ != Opening::None || unlaid_ || between_;
    }

    // After a frame (its layout): an opened interface that got its model is shown, a new sheet
    // is laid out, and the touches held for them are applied in order. Touches held through a
    // transition that ends in gameplay are dropped instead: the player aimed at a screen, not
    // at the scene that appeared. A finger still down when gameplay comes starts its gesture
    // there and then (the move stick gripped during the fade): its pointer is the UI's (the
    // platform routes a pointer at its pointer-down), so dropping all of it would leave the
    // finger dead until lifted.
    void ReleaseHeld()
    {
        if (opening_ == Opening::AwaitFrame)
            opening_ = Opening::None;
        unlaid_ = false;
        if (opening_ != Opening::None || between_ || held_.empty())
            return;
        const std::vector<HeldTouch> held = std::move(held_);
        held_.clear();
        held_pointers_.clear();
        if (!InterfaceActive())
        {
            std::map<int, const HeldTouch *> down; // the last event of each finger that did not lift
            for (const HeldTouch &touch : held)
                if (touch.phase == TouchPhase::Up || touch.phase == TouchPhase::Cancel)
                    down.erase(touch.pointer);
                else
                    down[touch.pointer] = &touch;
            Log(std::to_string(held.size()) + " touch events dropped: made while the game showed nothing to use, and gameplay came next" +
                (down.empty() ? std::string() : "; " + std::to_string(down.size()) + " finger(s) still down start their gesture now") + "; " +
                RoutingFacts());
            for (const auto &[pointer, touch] : down)
                Apply(TouchPhase::Down, pointer, touch->x, touch->y, touch->time);
            return;
        }
        for (const HeldTouch &touch : held)
            Apply(touch.phase, touch.pointer, touch.x, touch.y, touch.time);
    }

    struct Toast_
    {
        Rml::ObserverPtr<Rml::Element> element;
        double until = 0.0;
        std::string text; // the message; the element shows it with its repeat count
        int count = 1;
        double leaving = -1.0; // when it started to animate out (c-notice leaving), -1 = shown
        double held = -1.0;    // the seconds it had left when a finger stopped it (-1 = running)
    };

    // How long a toast animates out (pack time.toastLeave); the stack closes up meanwhile.
    double ToastLeaveSeconds() const
    {
        return Setting("toastLeave", 0.22);
    }

    void Leave(Toast_ &toast, double now)
    {
        changed_ = true;
        toast.leaving = now;
        if (Rml::Element *element = toast.element.get())
            element->SetAttribute("leaving", "1");
    }

    void RemoveToast(size_t index)
    {
        changed_ = true;
        if (Rml::Element *element = toasts_[index].element.get())
            if (Rml::Element *parent = element->GetParentNode())
                parent->RemoveChild(element);
        toasts_.erase(toasts_.begin() + static_cast<long>(index));
    }

    // An expired toast animates out first; it goes once that has run.
    void ExpireToasts(double now)
    {
        for (size_t i = toasts_.size(); i-- > 0;)
            if (toasts_[i].leaving < 0.0 && toasts_[i].held < 0.0 && now >= toasts_[i].until)
                Leave(toasts_[i], now);
            else if (toasts_[i].leaving >= 0.0 && now >= toasts_[i].leaving + ToastLeaveSeconds())
                RemoveToast(i);
    }

    Config config_;
    System system_;
    std::unique_ptr<PackFiles> files_;
    RenderBackend *backend_ = nullptr;
    std::unique_ptr<FrameRenderer> renderer_;
    std::unique_ptr<CountedDocumentInstancer> document_instancer_; // registered for "body" until Rml::Shutdown
    FrameStats counters_;   // the frame being made, including the work done since the last one
    FrameStats last_frame_; // what the last Frame did
    bool video_ = false;    // a full-screen game video plays (channel "video")
    bool changed_ = true;   // something since the last drawn frame may look different (Prepare)
    bool drawn_ = false;
    uint64_t shown_hash_ = 0; // drawing commands of the frame on the screen
    std::unique_ptr<ComponentRegistry> registry_;
    Rml::Context *context_ = nullptr;
    Rml::ElementDocument *document_ = nullptr;
    Rml::ElementDocument *overlay_ = nullptr;
    std::string overlay_path_;
    std::string screen_path_;
    std::unordered_map<std::string, Json> strings_; // language code → {key: text}
    std::string language_ = "en";
    std::set<std::string> unknown_languages_, missing_strings_;
    std::string logged_taps_; // LogTaps: the last block logged
    int taps_block_ = 0;
    std::set<std::string> unmapped_taps_; // said once each: screen, element id and verb drawn with no mapping (CheckTaps)
    std::unique_ptr<screens::Router> router_;
    Rml::ElementDocument *screen_root_ = nullptr; // the screen document while a game interface is mounted
    // HUD
    std::unique_ptr<hud::Hud> hud_;
    hud::Layout layout_;
    float hud_toast_clear_ = 0.0f; // the shown HUD's top status bottom (layout dp), 0 = none
    float screen_toast_clear_ = 0.0f; // the shown screen's header bottom (layout dp), 0 = none
    std::string hud_path_;
    Rml::ElementDocument *hud_document_ = nullptr;
    std::unordered_map<std::string, Json> hud_props_;       // props last given to each mounted element
    std::unordered_map<std::string, std::string> hud_made_; // what each mounted element was created as
    mutable std::mutex touch_mutex_;
    TouchMap touch_map_;
    std::vector<Rml::Rectanglef> hud_regions_; // the HUD's touch regions as last collected (PublishTouchMap)
    bool regions_current_ = false;             // hud_regions_ belong to the HUD shown now
    bool regions_moved_ = true;                // something since that collection can have moved them
    // An interface the game opened (ExpectInterface): its model is still to come, then its
    // first frame.
    enum class Opening
    {
        None,
        AwaitModel,
        AwaitFrame
    };
    Opening opening_ = Opening::None;
    std::string hud_problem_;       // the last undrawable HUD view's error, logged once
    bool game_interface_ = false; // a game interface model is shown (SetInterface until CloseInterface)
    std::string game_screen_;     // the screen of that game interface model
    // A screen switch, timed from the engine action before it to its first frames (ReportSwitch).
    using Clock = std::chrono::steady_clock;
    struct Switch
    {
        bool open = false;
        bool expected = false;  // the engine announced it (ExpectInterface) before its model
        uint64_t opening = 0;   // InterfaceModel::opening of the screen being timed
        std::string screen;
        Clock::time_point begun, modelled, first;
        bool drawn = false; // its first frame with the model is drawn
        FrameStats first_frame, settle;
        int frames = 0;
        void Begin(Clock::time_point now, bool by_expect)
        {
            *this = Switch{false, by_expect, opening};
            open = true;
            begun = now;
            modelled = {};
        }
    } switch_;
    Clock::time_point last_tap_{}, reported_{}; // the newest touch end (its own time); the end of the last switch reported
    Clock::time_point slow_reported_{};         // the last "UI slow frame" line
    bool unlaid_ = false; // something covers the game since the last frame (PublishTouchMap)
    bool between_ = false; // nothing the player can use is shown (Between)
    struct HeldTouch
    {
        TouchPhase phase;
        int pointer;
        float x, y;
        double time;
    };
    std::vector<HeldTouch> held_;
    std::set<int> held_pointers_;
    Json settings_ = Json::object();
    Json safe_ = {{"left", 0}, {"top", 0}, {"right", 0}, {"bottom", 0}};
    std::array<float, 4> display_safe_{}; // SetSafeArea's insets in the display's dp
    float display_density_ = 1.0f;        // Resize's density; config_.density is it times ui_scale_
    float ui_scale_ = 1.0f;
    float hud_button_opacity_ = 1.0f;
    bool left_handed_ = false; // «Под левую руку»: the HUD mounted mirrored
    std::string hud_button_class_; // the class ApplyHudButtonOpacity set on the HUD document
    std::unordered_map<int, Capture> captures_;
    std::vector<Rml::ObserverPtr<Rml::Element>> ticking_;
    std::vector<Toast_> toasts_;
    int scroll_pointer_ = -1; // the pointer that drags lists (one at a time)
    TouchScroll scroll_;
    ActionTracer tracer_;              // Config::trace_actions
    std::vector<ActionTrace> traces_;  // finished, until TakeActionTraces
    bool initialised_ = false;
};

} // namespace

bool DecodePackImage(const std::string &source, int &width, int &height, std::vector<unsigned char> &rgba)
{
    Rml::FileInterface *files = Rml::GetFileInterface();
    const Rml::FileHandle handle = files->Open(source);
    if (!handle)
        return false;
    std::vector<uint8_t> bytes(files->Length(handle));
    files->Read(bytes.data(), bytes.size(), handle);
    files->Close(handle);
    plutovg_surface_t *surface = plutovg_surface_load_from_image_data(bytes.data(), static_cast<int>(bytes.size()));
    if (!surface)
        return false;
    width = plutovg_surface_get_width(surface);
    height = plutovg_surface_get_height(surface);
    const int stride = plutovg_surface_get_stride(surface);
    const unsigned char *data = plutovg_surface_get_data(surface);
    rgba.resize(static_cast<size_t>(width) * static_cast<size_t>(height) * 4);
    for (int y = 0; y < height; ++y)
    {
        const auto *row = reinterpret_cast<const uint32_t *>(data + static_cast<size_t>(y) * static_cast<size_t>(stride));
        for (int x = 0; x < width; ++x)
        {
            const uint32_t argb = row[x]; // premultiplied
            unsigned char *pixel = &rgba[(static_cast<size_t>(y) * width + x) * 4];
            pixel[0] = static_cast<unsigned char>(argb >> 16);
            pixel[1] = static_cast<unsigned char>(argb >> 8);
            pixel[2] = static_cast<unsigned char>(argb);
            pixel[3] = static_cast<unsigned char>(argb >> 24);
        }
    }
    plutovg_surface_destroy(surface);
    return true;
}

std::unique_ptr<FileSource> DirectoryFiles(std::string root)
{
    return std::make_unique<DirectorySource>(std::move(root));
}

std::unique_ptr<Host> Host::Create(const Config &config, std::string *error)
{
    if (g_host_alive.exchange(true))
    {
        const std::string why = "a UI host already lives in this process: one host per process (RmlUi state is process-global)";
        Rml::Log::Message(Rml::Log::LT_ERROR, "%s", why.c_str());
        if (error)
            *error = why;
        return nullptr;
    }
    auto host = std::make_unique<HostImpl>();
    host->owns_process_slot = true;
    if (!host->Start(config, error))
        return nullptr;
    return host;
}

} // namespace storm::ui
