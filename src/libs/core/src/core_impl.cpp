#include "load_watch.hpp"
#include "loading_transaction.hpp"
#include "scene_load_status.hpp"
#ifdef __ANDROID__
#include "android_newgame.hpp"
#include "platform/android_ui.hpp"
#endif
#include "core_impl.h"
#include "scene_hold.hpp"

#include "compiler.h"
#include "controls.h"
#include "fs.h"
#include "platform/frame_work.hpp"
#include "script_state.hpp"
#include "platform/host_hooks.hpp"
#include "profiler.hpp"
#include "save_file.hpp"
#include "save_state.hpp"
#include "save_trash.hpp"

#include <fstream>
#ifdef STORM_TRACY
#include <cxxabi.h>
#include <string>
#include <typeindex>
#include <unordered_map>
#endif

#include "string_compare.hpp"
#include <SDL2/SDL.h>

Core &core = core_internal;

uint64_t get_performance_counter()
{
    return SDL_GetPerformanceCounter();
}

#ifdef STORM_TRACY
namespace
{
// The class of an entity for its profiler zone, demangled once per type.
const std::string &EntityClassName(Entity &entity)
{
    static std::unordered_map<std::type_index, std::string> names;
    auto [it, inserted] = names.try_emplace(typeid(entity));
    if (inserted)
    {
        int status = 0;
        char *demangled = abi::__cxa_demangle(typeid(entity).name(), nullptr, nullptr, &status);
        it->second = status == 0 && demangled ? demangled : typeid(entity).name();
        std::free(demangled);
    }
    return it->second;
}
} // namespace
#define STORM_PROFILE_ENTITY(entity)                                                                                   \
    STORM_PROFILE_ZONE("entity");                                                                                      \
    const std::string &entityName = EntityClassName(entity);                                                           \
    STORM_PROFILE_ZONE_TEXT(entityName.data(), entityName.size())
#else
#define STORM_PROFILE_ENTITY(entity) static_cast<void>(0)
#endif

namespace storm
{
namespace
{
ENGINE_VERSION getTargetEngineVersion(const std::string_view &version)
{
    using namespace std::string_view_literals;

    if (iEquals(version, "sd"sv))
    {
        return ENGINE_VERSION::SEA_DOGS;
    }
    else if (iEquals(version, "potc"sv))
    {
        return ENGINE_VERSION::PIRATES_OF_THE_CARIBBEAN;
    }
    else if (iEquals(version, "ct"sv))
    {
        return ENGINE_VERSION::CARIBBEAN_TALES;
    }
    else if (iEquals(version, "coas"sv))
    {
        return ENGINE_VERSION::CITY_OF_ABANDONED_SHIPS;
    }
    else if (iEquals(version, "teho"sv))
    {
        return ENGINE_VERSION::TO_EACH_HIS_OWN;
    }
    else if (iEquals(version, "latest"sv))
    {
        return ENGINE_VERSION::LATEST;
    }

    return ENGINE_VERSION::UNKNOWN;
}
} // namespace
} // namespace storm

typedef struct
{
    uint32_t code;
    void *pointer;
} CODE_AND_POINTER;

void CoreImpl::ResetCore()
{
    Initialized = false;
    bEngineIniProcessed = false;

    ReleaseServices();

    Services_List.Release();

    delete[] State_file_name;
    State_file_name = nullptr;
}

void CoreImpl::CleanUp()
{
    Initialized = false;
    bEngineIniProcessed = false;
    // Entity destructors still need renderer/geometry/animation services and
    // script attributes. Also empty the realization layers before another run.
    EraseEntities();
    ReleaseServices();
    Compiler->Release();
    Services_List.Release();
    Services_List.Release();
    delete[] State_file_name;
}

void CoreImpl::SetWindow(std::shared_ptr<storm::OSWindow> window)
{
    window_ = std::move(window);
}

void CoreImpl::Init()
{
    Initialized = false;
    bEngineIniProcessed = false;
    State_file_name = nullptr;
    Exit_flag = false;
    State_loading = false;
    Memory_Leak_flag = false;
    Controls = nullptr;
    fTimeScale = 1.0f;
    Compiler = new COMPILER;
#ifdef STORM_SCENE_SWEEP
    // tools/vm_verify --state compares the script state of two engines: the game's time must not follow the clock.
    if (const char *states = std::getenv("STORM_VM_STATE_DIR"); states && *states)
        Timer.FixedElapsed = 20.0f;
#endif

    /* TODO: place this outside CoreImpl */
    SetLayerType(EXECUTE, layer_type_t::execute);
    SetLayerType(REALIZE, layer_type_t::realize);
    SetLayerType(SEA_EXECUTE, layer_type_t::execute);
    SetLayerType(SEA_REALIZE, layer_type_t::realize);
    SetLayerType(INTERFACE_EXECUTE, layer_type_t::execute);
    SetLayerType(INTERFACE_REALIZE, layer_type_t::realize);
    SetLayerType(FADER_EXECUTE, layer_type_t::execute);
    SetLayerType(FADER_REALIZE, layer_type_t::realize);
    SetLayerType(LIGHTER_EXECUTE, layer_type_t::execute);
    SetLayerType(LIGHTER_REALIZE, layer_type_t::realize);
    SetLayerType(VIDEO_EXECUTE, layer_type_t::execute);
    SetLayerType(VIDEO_REALIZE, layer_type_t::realize);
    SetLayerType(EDITOR_REALIZE, layer_type_t::realize);
    SetLayerType(INFO_REALIZE, layer_type_t::realize);
    SetLayerType(SOUND_DEBUG_REALIZE, layer_type_t::realize);
}

void CoreImpl::InitBase()
{
    LoadClassesTable();
}

void CoreImpl::ReleaseBase()
{
    Compiler->Token.Release();
}

bool CoreImpl::Run()
{
    stopFrameProcessing_ = false;

    const auto bDebugWindow = true;
    if (bDebugWindow && core_internal.Controls && core_internal.Controls->GetDebugAsyncKeyState(VK_F7) < 0)
        DumpEntitiesInfo();

    if (Exit_flag)
        return false; // exit
    storm::ScriptStateMayChange(); // the frame's scripts, entities and loading

    Timer.Run(); // calc delta time
    storm::save::trash().tick(); // finalize save deletes whose grace period has passed

    auto *pVCTime = static_cast<VDATA *>(core_internal.GetScriptVariable("iRealDeltaTime"));
    if (pVCTime)
        pVCTime->Set(static_cast<int32_t>(GetRDeltaTime()));

    auto tt = std::time(nullptr);
    auto local_tm = *std::localtime(&tt);

    auto *pVYear = static_cast<VDATA *>(core_internal.GetScriptVariable("iRealYear"));
    auto *pVMonth = static_cast<VDATA *>(core_internal.GetScriptVariable("iRealMonth"));
    auto *pVDay = static_cast<VDATA *>(core_internal.GetScriptVariable("iRealDay"));

    if (pVYear)
        pVYear->Set(local_tm.tm_year + 1900);
    if (pVMonth)
        pVMonth->Set(local_tm.tm_mon + 1); // tm_mon belongs [0, 11]
    if (pVDay)
        pVDay->Set(local_tm.tm_mday);

    if (Controls && Controls->GetDebugAsyncKeyState('R') < 0)
        Timer.Delta_Time *= 10;
    if (Controls && Controls->GetDebugAsyncKeyState('Y') < 0)
        Timer.Delta_Time = static_cast<uint32_t>(Timer.Delta_Time * 0.2f);

    Timer.Delta_Time = static_cast<uint32_t>(Timer.Delta_Time * fTimeScale);
    Timer.fDeltaTime *= fTimeScale;

    auto *pVData = static_cast<VDATA *>(GetScriptVariable("fHighPrecisionDeltaTime", nullptr));
    if (pVData)
        pVData->Set(Timer.fDeltaTime * 0.001f);

    if (!Initialized)
    {
        Initialize(); // initialization at start or after reset
    }
    if (!bEngineIniProcessed)
        ProcessEngineIniFile();

    {
        STORM_PROFILE_ZONE("script: frame");
        const storm::platform::frame_work::Scope phase(storm::platform::frame_work::Phase::Script);
        Compiler->ProcessFrame(Timer.GetDeltaTime());
        Compiler->ProcessEvent("frame");
    }
#ifdef __ANDROID__
    storm::android::ApplyNewGameChoiceAfterFrame();
#endif

    {
        STORM_PROFILE_ZONE("save/load state");
        const storm::platform::frame_work::Scope phase(storm::platform::frame_work::Phase::Load);
        ProcessStateLoading();
        ReportFailedLoad();
        ReportFailedSave();
    }
    // A scene the scripts opened this frame without the loading screen, so without the ReloadProgressEnd that runs
    // the stock loading transaction (a quest's sea login: SeaLogin alone; the sea's Tele reload): prepared here by the
    // same transaction, behind the same screen, before its first frame is executed or shown. It waits for the same
    // resources and quality products (kSceneProductBudgetMs) and logs the same ready line as every other load.
    if (!storm::loading::active && !storm::loading::screen_up && storm::loading::screen &&
        storm::render::GetSceneLoadStatus().active && !DeferFrozenScene(storm::render::GetSceneLoadStatus()))
    {
        STORM_PROFILE_ZONE("load: scene opened without the loading screen");
        Trace("Stock loading transaction for a scene the scripts opened without the loading screen: prepared behind it");
        storm::loading::screen(true);
        const bool prepared = PrepareLoadingScene();
        storm::loading::screen(false);
        if (!prepared)
            return false; // aborted: the engine exits (AbortLoadingTransaction)
    }

    {
        STORM_PROFILE_ZONE("services: run start");
        const storm::platform::frame_work::Scope phase(storm::platform::frame_work::Phase::Services);
        ProcessRunStart(SECTION_ALL);
    }
    if (stopFrameProcessing_)
    {
        // service asked to skip current frame processing
        return true;
    }

    // ExitProgram() runs the script's ExitMain, which tears the world down (weather, its event
    // handlers...), and only then requests the exit. The rest of that frame must not run: entities
    // would execute and render against the torn-down world.
    if (Exit_flag)
        return false;
    ProcessExecute(); // transfer control to objects via Execute() function
    if (Exit_flag)
        return false;
    ProcessRealize(); // transfer control to objects via Realize() function

    {
        STORM_PROFILE_ZONE("controls");
        const storm::platform::frame_work::Scope phase(storm::platform::frame_work::Phase::Controls);
        if (Controls)
            Controls->Update(Timer.rDelta_Time);

        if (Controls)
            ProcessControls();
    }

    entity_manager_.NewLifecycle();

    {
        STORM_PROFILE_ZONE("services: run end (present)");
        const storm::platform::frame_work::Scope phase(storm::platform::frame_work::Phase::Present);
        ProcessRunEnd(SECTION_ALL);
    }

    return true;
}

bool CoreImpl::AbortLoadingTransaction(storm::loading::Outcome outcome, uint64_t generation)
{
    using storm::loading::Outcome;
    const bool user = outcome == Outcome::Cancel || outcome == Outcome::Quit;
    Trace("%sStock loading transaction aborted generation=%llu outcome=%s: the engine exits", user ? "" : "ERROR: ",
          static_cast<unsigned long long>(generation), storm::loading::OutcomeName(outcome));
    // No exception crosses the script VM: the running scripts (the stock loader and whatever called it) return
    // at once, and the frame ends with the ordinary engine exit.
    if (Compiler)
        Compiler->AbortExecution();
    Exit_flag = true;
    return false;
}

bool CoreImpl::DeferFrozenScene(const storm::render::SceneLoadStatus &status)
{
    if (!status.active || !status.owner || !IsEntityValid(status.owner))
        return false;
    const auto layers = entity_manager_.GetRealizeLayers(status.owner);
    if (layers.in == 0 || layers.frozen != layers.in)
        return false;
    if (deferred_scene_generation_ != status.generation)
    {
        // Stock OnLoad of a world-map save made at sea: wdmCreateWorldMap's own ReloadProgressEnd runs while REALIZE
        // is still frozen by the sea (CreateSeaEnvironment); OnLoad's EngineLayersOffOn lets it draw just after, and
        // its final ReloadProgressEnd prepares the map. Waiting here instead held the loading screen for good.
        std::string frozen;
        for (uint32_t index = 0; index < 32; ++index)
            if (layers.frozen >> index & 1)
                frozen += (frozen.empty() ? "" : ", ") + std::to_string(index);
        Trace("Stock loading transaction deferred generation=%llu: the scene's owner is only in realize layers the "
              "scripts froze (%s); prepared once they let it draw", static_cast<unsigned long long>(status.generation),
              frozen.c_str());
        deferred_scene_generation_ = status.generation;
    }
    return true;
}

bool CoreImpl::LoadingSceneDeferred() const
{
    const auto status = storm::render::GetSceneLoadStatus();
    if (!status.active || !status.owner || !IsEntityValid(status.owner))
        return false;
    const auto layers = entity_manager_.GetRealizeLayers(status.owner);
    return layers.in != 0 && layers.frozen == layers.in;
}

bool CoreImpl::PrepareLoadingScene()
{
    const auto initial = storm::render::GetSceneLoadStatus();
    if (!initial.active) return true;
    if (storm::loading::active) return AbortLoadingTransaction(storm::loading::Outcome::Reentrant, initial.generation);
    if (DeferFrozenScene(initial)) return true;
    storm::loading::Scope loading;
    struct CpuAdmissionReset { uint64_t generation; ~CpuAdmissionReset() { storm::render::EndSceneCpuBudget(generation); } } cpu_admission_reset{initial.generation};
    const auto loading_started = std::chrono::steady_clock::now();
    uint64_t loading_frames = 0;
    Trace("Stock loading transaction enter generation=%llu", static_cast<unsigned long long>(initial.generation));
    storm::platform::LoadingPlatformTransaction platform;
    struct LoadingScheduleReset {
        uint64_t generation;
        ~LoadingScheduleReset() {
            storm::platform::PumpLoadingSchedule(generation, storm::platform::LoadingSchedulePhase::Ended);
        }
    } schedule_reset{initial.generation};
    struct TimerBaseline {
        TIMER &timer;
        ~TimerBaseline() {
            timer.Current = timer.Previous = std::chrono::high_resolution_clock::now();
            timer.Carry = 0;
        }
    } baseline{Timer};
    // GetService stores SERVICE pointers; avoid pulling renderer backend headers into core.
    auto *renderer = static_cast<SERVICE *>(GetService("renderer"));
    auto *animation = static_cast<SERVICE *>(GetService("AnimationServiceImp"));
    if (!renderer) return AbortLoadingTransaction(storm::loading::Outcome::Unavailable, initial.generation);
    // A preparation that stops progressing (no change of what the reveal waits for) is reported loudly once; the
    // loading screen shows it as slow (SceneReadiness::slow) with its Cancel. It never ends the game by itself.
    std::string progress_signature;
    auto progress_at = loading_started;
    bool stall_reported = false;
    while (true) {
        const auto status = storm::render::GetSceneLoadStatus();
        if (!status.active || status.generation != initial.generation)
            return AbortLoadingTransaction(storm::loading::Outcome::Invalidated, initial.generation);
        if (status.owner && !IsEntityValid(status.owner)) {
            storm::render::AbandonSceneLoad(initial.generation);
            Trace("ERROR: Stock loading transaction abandoned generation=%llu: the scene's owner was deleted before its "
                  "reveal (its load failed); the game goes on", static_cast<unsigned long long>(initial.generation));
            return true;
        }
        const std::string signature = status.state + '/' + std::to_string(status.textures) + '/' +
            std::to_string(status.shadows) + '/' + std::to_string(status.failed);
        const auto now = std::chrono::steady_clock::now();
        if (signature != progress_signature) {
            progress_signature = signature;
            progress_at = now;
            stall_reported = false;
        }
        else if (!stall_reported && now - progress_at >= kLoadingStall) {
            stall_reported = true;
            Trace("ERROR: Stock loading transaction made no progress for %lld s: state=%s textures=%u shadows=%u "
                  "failed=%u (%s); still waiting, the player can cancel", static_cast<long long>(
                      std::chrono::duration_cast<std::chrono::seconds>(kLoadingStall).count()),
                  status.state.c_str(), status.textures, status.shadows, status.failed, status.detail.c_str());
        }
        const auto host = storm::platform::PumpLoadingPlatform(initial.generation);
        if (host == storm::platform::LoadingPlatformState::Suspended) {
            storm::render::SuspendSceneCpuBudget(initial.generation);
            storm::platform::PumpLoadingSchedule(initial.generation, storm::platform::LoadingSchedulePhase::Suspended);
            SDL_Delay(2);
            continue;
        }
        if (host != storm::platform::LoadingPlatformState::Continue)
            return AbortLoadingTransaction(host == storm::platform::LoadingPlatformState::Cancel
                ? storm::loading::Outcome::Cancel : host == storm::platform::LoadingPlatformState::Quit
                ? storm::loading::Outcome::Quit : storm::loading::Outcome::Unavailable, initial.generation);
        const auto wait = storm::platform::PumpLoadingSchedule(initial.generation, storm::platform::LoadingSchedulePhase::Poll);
        if (wait) {
            // The next iteration pumps lifecycle/cancel again. No fake render
            // observation or ordinary frame is earned while the worker uses its window.
            SDL_Delay(std::min(wait, 2u));
            continue;
        }
        storm::platform::PumpLoadingSchedule(initial.generation, storm::platform::LoadingSchedulePhase::FrameStarted);
        renderer->RunStart();
        // Animation service explicitly builds the already-selected pose in this context.
        if (animation) animation->RunStart();
        // Preparation is opt-in and unique across all loaded entities: WEATHER
        // can be execute-only, and cameras need their configured transform before
        // drawing. Never call ordinary Execute or change any layer's frozen flag.
        std::vector<entid_t> prepare_ids;
        ForEachEntity([&](entptr_t entity) { prepare_ids.push_back(entity->GetId()); });
        for (auto id : prepare_ids) if (auto *entity = GetEntityPointerSafe(id))
            entity->ProcessStage(Entity::Stage::loading_prepare, 0);
        // Render only the original eligible realize layers (including SEA_REALIZE).
        // Copy the shared scratch vector before any consumer can reuse it.
        const auto ids = GetEntityIds(layer_type_t::realize);
        for (auto id : ids) if (auto *entity = GetEntityPointerSafe(id))
            entity->ProcessStage(Entity::Stage::loading_realize, 0);
        renderer->RunEnd(); // stock artwork, actual Present, upload/shadow/resource pumps
        storm::platform::PumpLoadingSchedule(initial.generation, storm::platform::LoadingSchedulePhase::FramePresented);
        ++loading_frames;
        storm::loading::first_frame = false;
        if (storm::loading::ready) {
            storm::render::CompleteSceneReveal();
            if (!storm::render::GetSceneLoadStatus().active) {
                Trace("Stock loading transaction ready-exit generation=%llu frames=%llu elapsed-ms=%lld",
                      static_cast<unsigned long long>(initial.generation), static_cast<unsigned long long>(loading_frames),
                      static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now() - loading_started).count()));
                return true;
            }
        }
    }
}

void CoreImpl::ProcessControls()
{
    CONTROL_STATE cs;
    USER_CONTROL uc;

    if (!Controls)
        return;

    for (int32_t n = 0; n < Controls->GetControlsNum(); n++)
    {
        Controls->GetControlState(n, cs);
        if (cs.state == CST_ACTIVATED)
        {
            Controls->GetControlDesc(n, uc);
            Core::Event("Control Activation", "s", uc.name);
        }
        else if (cs.state == CST_INACTIVATED)
        {
            Controls->GetControlDesc(n, uc);
            Core::Event("Control Deactivation", "s", uc.name);
        }
    }
}

// internal functions
bool CoreImpl::Initialize()
{
    ResetCore();

    Initialized = true;

    return true;
}

void CoreImpl::ProcessEngineIniFile()
{
    char String[MAX_PATH];

    bEngineIniProcessed = true;

    auto engine_ini = fio->OpenIniFile(fs::ENGINE_INI_FILE_NAME);
    if (!engine_ini)
        throw std::runtime_error("no 'engine.ini' file");

    auto res = engine_ini->ReadString(nullptr, "program_directory", String, sizeof(String), "");
    if (res)
    {
        Compiler->SetProgramDirectory(String);
    }

    res = engine_ini->ReadString(nullptr, "controls", String, sizeof(String), "");
    if (res)
    {
        core_internal.Controls = static_cast<CONTROLS *>(MakeClass(String));
        if (core_internal.Controls == nullptr)
            core_internal.Controls = static_cast<CONTROLS *>(MakeClass("controls"));
    }
    else
    {
        delete Controls;
        Controls = nullptr;

        core_internal.Controls = new CONTROLS;
    }

    loadCompatibilitySettings(*engine_ini);

    res = engine_ini->ReadString(nullptr, "run", String, sizeof(String), "");
    if (res)
    {
        if (!Compiler->CreateProgram(String))
            throw std::runtime_error("fail to create program");
        if (!Compiler->Run())
            throw std::runtime_error("fail to run program");

        // Script version test
        if (targetVersion_ >= storm::ENGINE_VERSION::LATEST)
        {
            int32_t iScriptVersion = 0xFFFFFFFF;
            auto *pVScriptVersion = static_cast<VDATA *>(core_internal.GetScriptVariable("iScriptVersion"));
            if (pVScriptVersion)
                pVScriptVersion->Get(iScriptVersion);

            if (iScriptVersion != ENGINE_SCRIPT_VERSION)
            {
                spdlog::error("dialog \"Error\": Wrong script version (scripts {}, engine {})", iScriptVersion,
                              ENGINE_SCRIPT_VERSION);
                SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Error", "Wrong script version", nullptr);
                Compiler->ExitProgram();
            }
        }
    }
}

bool CoreImpl::LoadClassesTable()
{
    for (auto *c : __STORM_CLASSES_REGISTRY)
    {
        const auto hash = MakeHashValue(c->GetName());
        c->SetHash(hash);
    }

    return true;
}

void CoreImpl::CheckAutoExceptions(uint32_t = 0) const
{
    spdlog::warn("exception thrown");
}

void CoreImpl::Exit()
{
    Exit_flag = true;
}

storm::OSWindow *CoreImpl::GetWindow()
{
    return window_.get();
}

void CoreImpl::SetTimeScale(float _scale)
{
    fTimeScale = _scale;
}

// transfer message arguments and program control to entity, specified by Destination id
uint64_t CoreImpl::Send_Message(entid_t Destination, const char *Format, ...)
{
    MESSAGE message;
    auto *const ptr = GetEntityPointerSafe(Destination); // check for valid destination
    if (!ptr)
        return 0;

    va_list args;
    va_start(args, Format);
    try { message.ResetVA(Format, args); }
    catch (...) { va_end(args); throw; }
    va_end(args);
    const auto classCode = GetClassCode(Destination);
    try { return static_cast<Entity *>(ptr)->ProcessMessage(message); }
    catch (MessageFormatError &error)
    {
        auto *receiver = FindVMA(classCode);
        error.AppendContext(fmt::format("native Send_Message destination={} receiver={} classHash={}",
            Destination, receiver && receiver->GetName() ? receiver->GetName() : "<unknown>", classCode));
        throw;
    }
}

uint32_t CoreImpl::PostEvent(const char *Event_name, uint32_t post_time, const char *Format, ...)
{
    MESSAGE *pMS;
    MESSAGE message;

    entid_t id;

    if (Format != nullptr)
    {
        pMS = new MESSAGE();
        va_list args;
        va_start(args, Format);
        message.ResetVA(Format, args);
        pMS->Reset(Format);

        auto bAction = true;
        while (bAction)
        {
            switch (message.GetCurrentFormatType())
            {
            case 'l':
                int32_t v;
                v = message.Long();
                pMS->Set(v);
                break;
            case 'f':
                float f;
                f = message.Float();
                pMS->Set(f);
                break;
            case 'i':
                id = message.EntityID();
                pMS->SetEntity(id);
                break;
            case 'e':
                VDATA *e;
                e = message.ScriptVariablePointer();
                pMS->Set(e);
                break;
            case 's': {
                const std::string &s = message.StringPointer();
                pMS->Set(s);
                break;
            }
            case 'a':
                ATTRIBUTES *a;
                a = message.AttributePointer();
                pMS->Set(a);
                break;

            default:
                bAction = false;
                break;
            }
        }
        va_end(args);
    }
    else
        pMS = nullptr;

    auto *pEM = new S_EVENTMSG(Event_name, pMS, post_time);
    pEM->bProcess = true;
    Compiler->AddPostEvent(pEM);
    return 0;
}

bool CoreImpl::EventPosted(const char *Event_name)
{
    return Compiler->EventPosted(Event_name);
}

VDATA *CoreImpl::Event(const std::string_view &event_name)
{
    MESSAGE message;
    return Event(event_name, message);
}

VDATA *CoreImpl::Event(const std::string_view &event_name, MESSAGE &message)
{
    // The phase the event is raised from (the HUD adapters, an entity's Execute or Realize, ...) names its origin.
    Compiler->SetEventOrigin(storm::platform::frame_work::state().current);
    const storm::platform::frame_work::Scope phase(storm::platform::frame_work::Phase::Script);
    return Compiler->ProcessEvent(event_name.data(), message);
}

void *CoreImpl::MakeClass(const char *class_name)
{
    const int32_t hash = MakeHashValue(class_name);
    for (auto *const c : __STORM_CLASSES_REGISTRY)
        if (c->GetHash() == hash && storm::iEquals(class_name, c->GetName()))
            return c->CreateClass();

    return nullptr;
}

void CoreImpl::ReleaseServices()
{
    for (auto *const c : __STORM_CLASSES_REGISTRY)
        if (c->Service())
            c->Clear();

    Controls = nullptr;
}

VMA *CoreImpl::FindVMA(const char *class_name)
{
    const int32_t hash = MakeHashValue(class_name);
    for (auto *const c : __STORM_CLASSES_REGISTRY)
        if (c->GetHash() == hash && storm::iEquals(class_name, c->GetName()))
            return c;

    return nullptr;
}

VMA *CoreImpl::FindVMA(int32_t hash)
{
    for (auto *const c : __STORM_CLASSES_REGISTRY)
        if (c->GetHash() == hash)
            return c;

    return nullptr;
}

void *CoreImpl::GetService(const char *service_name)
{
    auto *pClass = FindVMA(service_name);
    if (pClass == nullptr)
    {
        CheckAutoExceptions(0);
        return nullptr;
    }

    if (pClass->GetHash() == 0)
    {
        CheckAutoExceptions(0);
        return nullptr;
    }

    if (pClass->GetReference() > 0)
        return pClass->CreateClass();

    auto *service_PTR = static_cast<SERVICE *>(pClass->CreateClass());

    const auto class_code = MakeHashValue(service_name);
    pClass->SetHash(class_code);

    bool initialized;
    try
    {
        initialized = service_PTR->Init();
    }
    catch (...)
    {
        // CreateClass caches the singleton before Init. Never hand a failed
        // instance to a later caller merely because its reference count is set.
        pClass->Clear();
        throw;
    }
    if (!initialized)
    {
        pClass->Clear();
        CheckAutoExceptions(0);
        return nullptr;
    }

    Services_List.Add(class_code, class_code, service_PTR);

    return service_PTR;
}

void CoreImpl::Trace(const char *format, ...)
{
    static char buffer_4k[4096];

    va_list args;
    va_start(args, format);
    vsnprintf(buffer_4k, sizeof(buffer_4k) - 4, format, args);
    va_end(args);
    spdlog::info(buffer_4k);
}

// Transfer programm control to objects via Execute() functions
void CoreImpl::ProcessExecute()
{
    uint64_t ticks;
    STORM_PROFILE_ZONE("execute: game logic, physics");
    const storm::platform::frame_work::Scope phase(storm::platform::frame_work::Phase::Execute);

    ProcessRunStart(SECTION_EXECUTE);

    const auto deltatime = Timer.GetDeltaTime();
    const auto &entIds = core.GetEntityIds(layer_type_t::execute);
    for (auto id : entIds)
    {
        if (auto *ptr = core.GetEntityPointerSafe(id))
        {
            STORM_PROFILE_ENTITY(*ptr);
            const storm::platform::frame_work::EntityScope entity(typeid(*ptr), false);
            ptr->ProcessStage(Entity::Stage::execute, deltatime);
        }
    }

    ProcessRunEnd(SECTION_EXECUTE);
}

void CoreImpl::ProcessRealize()
{
    uint64_t ticks;
    STORM_PROFILE_ZONE("realize: render submit");
    const storm::platform::frame_work::Scope phase(storm::platform::frame_work::Phase::Realize);
    ProcessRunStart(SECTION_REALIZE);

    const auto deltatime = Timer.GetDeltaTime();
    const auto &entIds = core.GetEntityIds(layer_type_t::realize);
    for (auto id : entIds)
    {
        if (auto *ptr = core.GetEntityPointerSafe(id))
        {
            STORM_PROFILE_ENTITY(*ptr);
            const storm::platform::frame_work::EntityScope entity(typeid(*ptr), true);
            ptr->ProcessStage(Entity::Stage::realize, deltatime);
        }
    }

    ProcessRunEnd(SECTION_REALIZE);
}

bool CoreImpl::SaveState(const char *file_name)
{
    if (!file_name)
    {
        throw std::logic_error("Bad file name of save");
    }
    // Lets the Android host's bounded exit wait out this save; see host_hooks.hpp.
    const storm::platform::SaveInProgress saving;

    // Resolve the path first: file_name usually points into a script stack value,
    // and serializing runs the script OnSave handler, which reuses that storage.
    const std::string name = file_name;
    const auto path = std::filesystem::u8path(fio->ConvertPathResource(name.c_str()));

    // The state is serialized in memory and swapped in atomically
    // (temporary file -> fsync -> verify -> rename); see save_file.hpp.
    storm::save::Bytes image;
    if (!Compiler->SaveState(image))
    {
        storm::save::trash().rollback(); // nothing was written: undo the slot delete that preceded this save
        save_failure_ = SaveFailure{name, "the script state could not be serialized"};
        return false;
    }

    const auto validate = [](const storm::save::Bytes &stored, std::string &why) {
        storm::save::Bytes scratch;
        return storm::save::readState(stored, scratch, why);
    };
    std::string error;
    if (std::error_code ec; !path.parent_path().empty())
    {
        std::filesystem::create_directories(path.parent_path(), ec);
    }
    if (!storm::save::writeAtomic(path, image, validate, error))
    {
        spdlog::error("Cannot save '{}' (resolved '{}'): {}; the previous save, if any, is unchanged", name,
                      path.string(), error);
        storm::save::trash().rollback();
        std::error_code ec;
        const auto space = std::filesystem::space(path.parent_path().empty() ? std::filesystem::path(".") : path.parent_path(), ec);
        const bool full = error.find(std::generic_category().message(ENOSPC)) != std::string::npos ||
                          error.find(std::generic_category().message(EDQUOT)) != std::string::npos ||
                          (!ec && space.available < image.size());
        save_failure_ = SaveFailure{name, error, full};
        return false;
    }

    // The new save is durable: deletes made on the way to it (slot overwrite) are final.
    storm::save::trash().commit();
    return true;
}

// force core to load state file at the start of next game loop, return false if no state file
std::string CoreImpl::StateFileProblem(const char *file_name)
{
    if (!file_name)
        return "no file name";
    const auto path = std::filesystem::u8path(fio->ConvertPathResource(file_name));
    storm::save::Bytes image, raw;
    std::string error;
    const auto source = storm::save::loadWithRecovery(path, image, raw, error);
    if (source == storm::save::Source::None)
        return error.empty() ? std::string("unreadable") : error;
    if (source == storm::save::Source::Backup)
        spdlog::warn("Save '{}': {}", file_name, error);
    storm::save::ParsedState state;
    if (!storm::save::parseState(raw.data(), raw.size(), state, error))
        return "the script state is damaged: " + error;
    return {};
}

bool CoreImpl::InitiateStateLoading(const char *file_name)
{
    if (!file_name)
    {
        return false;
    }

    // Reject missing or damaged saves here, while the caller can still react,
    // instead of discovering it after the running game has been torn down.
    if (const std::string problem = StateFileProblem(file_name); !problem.empty())
    {
        spdlog::error("Cannot load save '{}' (resolved '{}'): {}", file_name, fio->ConvertPathResource(file_name), problem);
        storm::loadWatch().Refused(file_name, "the record cannot be read: " + problem);
        return false;
    }

    delete[] State_file_name;

    const auto len = strlen(file_name) + 1;
    State_file_name = static_cast<char *>(new char[len]);
    strcpy_s(State_file_name, len, file_name);
    return true;
}

void CoreImpl::ProcessStateLoading()
{
    if (!State_file_name)
    {
        return;
    }

    // Read and validate before erasing anything: a save that became unreadable
    // since InitiateStateLoading must not cost the player the current session.
    const auto path = std::filesystem::u8path(fio->ConvertPathResource(State_file_name));
    storm::save::Bytes image, raw;
    std::string error;
    const auto source = storm::save::loadWithRecovery(path, image, raw, error);
    if (source == storm::save::Source::None)
    {
        spdlog::error("Save '{}' is unreadable, load cancelled: {}", State_file_name, error);
        storm::loadWatch().Refused(State_file_name, "the record became unreadable: " + error);
        delete[] State_file_name;
        State_file_name = nullptr;
        State_loading = false;
        return;
    }
    image = {};
    // The whole script state is read before the game is erased: a state that does not parse
    // costs nothing but this load (save_state.hpp).
    storm::save::ParsedState state;
    if (!storm::save::parseState(raw.data(), raw.size(), state, error))
    {
        spdlog::error("Save '{}' has a damaged script state, load cancelled: {}", State_file_name, error);
        storm::loadWatch().Refused(State_file_name, "the script state is damaged: " + error);
        delete[] State_file_name;
        State_file_name = nullptr;
        State_loading = false;
        return;
    }

    State_loading = true;
    EraseEntities();

    if (!Compiler->LoadState(raw, state))
    {
        spdlog::error("Save '{}' could not be applied", State_file_name);
        storm::loadWatch().Refused(State_file_name, "the game could not apply the record");
    }
    else
    {
        if (storm::loadWatch().Armed())
            spdlog::info("load watch: '{}' applied; its state was taken {:.1f} s (wall clock) after it was asked for", State_file_name,
                         storm::loadWatch().Waited());
        storm::loadWatch().Loaded();
    }

    delete[] State_file_name;
    State_file_name = nullptr;
    State_loading = false;
}

// The load watchdog (load_watch.hpp): a load the player asked for that was refused or never
// started. Said aloud in the log, then the game's scripts bring the menu back (the event
// AndroidLoadFailed: app script interface/android_load_failure.c) and the UI tells the player.
void CoreImpl::ReportFailedLoad()
{
    // Wall-clock time: the core's frame delta is clamped to 100 ms and would stretch the
    // deadline tenfold at the emulator's frame rates.
    static uint64_t last = SDL_GetTicks64();
    const uint64_t now = SDL_GetTicks64();
    const auto failure = storm::loadWatch().Poll(static_cast<double>(now - last) * 0.001);
    last = now;
    if (!failure)
        return;
    spdlog::error("LOAD FAILED: record '{}': {}; the menu is brought back", failure->record, failure->reason);
    if (!Compiler->HasActiveEventHandler("AndroidLoadFailed", "AndroidLoadFailed"))
        spdlog::error("LOAD FAILED: no script handles AndroidLoadFailed; nothing brings the menu back");
    Core::Event("AndroidLoadFailed", "ss", failure->record.c_str(), failure->reason.c_str());
#ifdef __ANDROID__
    const auto quoted = [](const std::string &text) {
        std::string out = "\"";
        for (const char c : text)
        {
            if (c == '"' || c == '\\')
                out += '\\';
            if (static_cast<unsigned char>(c) < 0x20)
                out += ' ';
            else
                out += c;
        }
        return out + '"';
    };
    storm::android::publishUiState("loadFailure", "{\"record\":" + quoted(failure->record) + ",\"reason\":" + quoted(failure->reason) + "}");
#endif
}

// Said in the log, the scripts told (AndroidSaveFailed(record, reason): app script
// interface/android_save_failure.c brings the save screen back after a save of the player) and
// the UI tells the player (channel "saveFailure" {record, reason, space}, once: null follows).
void CoreImpl::ReportFailedSave()
{
    if (!save_failure_)
        return;
    const SaveFailure failure = std::move(*save_failure_);
    save_failure_.reset();
    spdlog::error("SAVE FAILED: record '{}': {}{}", failure.record, failure.reason, failure.space ? " (the storage is full)" : "");
    if (!Compiler->HasActiveEventHandler("AndroidSaveFailed", "AndroidSaveFailed"))
        spdlog::error("SAVE FAILED: no script handles AndroidSaveFailed");
    Core::Event("AndroidSaveFailed", "ss", failure.record.c_str(), failure.reason.c_str());
#ifdef __ANDROID__
    storm::android::publishUiState("saveFailure", "{\"record\":" + storm::android::uiJsonText(failure.record) +
                                                      ",\"reason\":" + storm::android::uiJsonText(failure.reason) +
                                                      ",\"space\":" + (failure.space ? "true" : "false") + "}");
    storm::android::publishUiState("saveFailure", "null");
#endif
}

void CoreImpl::ProcessRunStart(uint32_t section_code)
{
    uint32_t class_code;
    SERVICE *service_PTR = Services_List.GetService(class_code);
    while (service_PTR)
    {
        const uint32_t section = service_PTR->RunSection();
        if (section == section_code)
        {
            service_PTR->RunStart();
        }
        service_PTR = Services_List.GetServiceNext(class_code);
    }
}

void CoreImpl::ProcessRunEnd(uint32_t section_code)
{
    uint32_t class_code;
    SERVICE *service_PTR = Services_List.GetService(class_code);
    while (service_PTR)
    {
        const uint32_t section = service_PTR->RunSection();
        if (section == section_code)
        {
            service_PTR->RunEnd();
        }
        service_PTR = Services_List.GetServiceNext(class_code);
    }
}

uint32_t CoreImpl::EngineFps()
{
    return Timer.fps;
}

void CoreImpl::SetDeltaTime(int32_t delta_time)
{
    Timer.SetDelta(delta_time);
}

uint32_t CoreImpl::GetDeltaTime()
{
    return storm::loading::active ? 0 : Timer.GetDeltaTime();
}

uint32_t CoreImpl::GetRDeltaTime()
{
    return storm::loading::active ? 0 : Timer.rDelta_Time;
}

ATTRIBUTES *CoreImpl::Entity_GetAttributeClass(entid_t id_PTR, const char *name)
{
    Entity *pE = GetEntityPointer(id_PTR);
    if (pE == nullptr)
        return nullptr;
    if (pE->AttributesPointer == nullptr)
        return nullptr;
    return pE->AttributesPointer->FindAClass(pE->AttributesPointer, name);
}

const char *CoreImpl::Entity_GetAttribute(entid_t id_PTR, const char *name)
{
    Entity *pE = GetEntityPointer(id_PTR);
    if (pE == nullptr)
        return nullptr;
    if (pE->AttributesPointer == nullptr)
        return nullptr;
    return pE->AttributesPointer->GetAttribute(name);
}

uint32_t CoreImpl::Entity_GetAttributeAsDword(entid_t id_PTR, const char *name, uint32_t def)
{
    Entity *pE = GetEntityPointer(id_PTR);
    if (pE == nullptr)
        return def;
    if (pE->AttributesPointer == nullptr)
        return def;
    return pE->AttributesPointer->GetAttributeAsDword(name, def);
}

float CoreImpl::Entity_GetAttributeAsFloat(entid_t id_PTR, const char *name, float def)
{
    Entity *pE = GetEntityPointer(id_PTR);
    if (pE == nullptr)
        return def;
    if (pE->AttributesPointer == nullptr)
        return def;
    return pE->AttributesPointer->GetAttributeAsFloat(name, def);
}

bool CoreImpl::Entity_SetAttribute(entid_t id_PTR, const char *name, const char *attribute)
{
    Entity *pE = GetEntityPointer(id_PTR);
    if (pE == nullptr)
        return false;
    if (pE->AttributesPointer == nullptr)
        return false;
    return pE->AttributesPointer->SetAttribute(name, attribute);
}

bool CoreImpl::Entity_SetAttributeUseDword(entid_t id_PTR, const char *name, uint32_t val)
{
    Entity *pE = GetEntityPointer(id_PTR);
    if (pE == nullptr)
        return false;
    if (pE->AttributesPointer == nullptr)
        return false;
    return pE->AttributesPointer->SetAttributeUseDword(name, val);
}

bool CoreImpl::Entity_SetAttributeUseFloat(entid_t id_PTR, const char *name, float val)
{
    Entity *pE = GetEntityPointer(id_PTR);
    if (pE == nullptr)
        return false;
    if (pE->AttributesPointer == nullptr)
        return false;
    return pE->AttributesPointer->SetAttributeUseFloat(name, val);
}

void CoreImpl::Entity_SetAttributePointer(entid_t id_PTR, ATTRIBUTES *pA)
{
    Entity *pE = GetEntityPointer(id_PTR);
    if (pE == nullptr)
        return;
    pE->AttributesPointer = pA;
    if (pA)
        pE->attributesHandle = pA->GetHandle();
    pE->attributesGoneReported = false;
}

uint32_t CoreImpl::Entity_AttributeChanged(entid_t id_PTR, ATTRIBUTES *pA)
{
    Entity *pE = GetEntityPointer(id_PTR);
    if (pE == nullptr)
        return 0;
    return pE->AttributeChanged(pA);
}

ATTRIBUTES *CoreImpl::Entity_GetAttributePointer(entid_t id_PTR)
{
    Entity *pE = GetEntityPointer(id_PTR);
    if (pE == nullptr)
        return nullptr;
    if (pE->AttributesPointer != nullptr && !ATTRIBUTES::IsAlive(pE->attributesHandle))
    {
        if (!pE->attributesGoneReported)
            spdlog::warn("entity {:x}: the script object it was created with is gone: its arefs are empty",
                         static_cast<uint64_t>(id_PTR));
        pE->attributesGoneReported = true;
        return nullptr;
    }
    return pE->AttributesPointer;
}

void CoreImpl::EraseEntities()
{
    entity_manager_.EraseAll();
}

void CoreImpl::ClearEvents()
{
    Compiler->ClearEvents();
}

void CoreImpl::AppState(bool state)
{
    if (Controls)
        Controls->AppState(state);
}

uint32_t CoreImpl::MakeHashValue(const char *string)
{
    uint32_t hval = 0;

    while (*string != 0)
    {
        char v = *string++;
        if ('A' <= v && v <= 'Z')
            v += 'a' - 'A';

        hval = (hval << 4) + static_cast<uint32_t>(v);
        const uint32_t g = hval & (static_cast<uint32_t>(0xf) << (32 - 4));
        if (g != 0)
        {
            hval ^= g >> (32 - 8);
            hval ^= g;
        }
    }
    return hval;
}

void CoreImpl::DumpEntitiesInfo()
{
}

void *CoreImpl::GetSaveData(const char *file_name, int32_t &data_size)
{
    return Compiler->GetSaveData(file_name, data_size);
}

bool CoreImpl::SetSaveData(const char *file_name, void *data_ptr, int32_t data_size)
{
    return Compiler->SetSaveData(file_name, data_ptr, data_size);
}

uint32_t CoreImpl::SetScriptFunction(IFUNCINFO *pFuncInfo)
{
    return Compiler->SetScriptFunction(pFuncInfo);
}

const char *CoreImpl::EngineIniFileName()
{
    return fs::ENGINE_INI_FILE_NAME;
}

int CoreImpl::ScriptCallDepth(const char *func_name)
{
    return Compiler ? Compiler->CallDepth(func_name) : -1;
}

VDATA *CoreImpl::ScriptCallArgument(const char *func_name, uint32_t index)
{
    return Compiler ? Compiler->CallArgument(func_name, index) : nullptr;
}

bool CoreImpl::HasActiveEventHandler(const char *event_name, const char *func_name)
{
    return Compiler && Compiler->HasActiveEventHandler(event_name, func_name);
}

std::optional<int32_t> CoreImpl::GetScriptIntegerDefine(const char *name)
{
    return Compiler->FindIntegerDefine(name);
}

void *CoreImpl::GetScriptVariable(const char *pVariableName, uint32_t *pdwVarIndex)
{
    const VarInfo *real_var;

    storm::CountVariableLookup();
    const auto dwVarIndex = Compiler->VarTab.FindVar(pVariableName);
    if (dwVarIndex == INVALID_VAR_CODE)
    {
        return nullptr;
    }

    real_var = Compiler->VarTab.GetVar(dwVarIndex);
    if (real_var == nullptr)
    {
        return nullptr;
    }

    if (pdwVarIndex)
        *pdwVarIndex = dwVarIndex;

    return real_var->value.get();
}

storm::ENGINE_VERSION CoreImpl::GetTargetEngineVersion() const noexcept
{
    return targetVersion_;
}

ScreenSize CoreImpl::GetScreenSize() const noexcept
{
    switch (targetVersion_)
    {
    case storm::ENGINE_VERSION::PIRATES_OF_THE_CARIBBEAN: {
        return {640, 480};
    }
    default: {
        return {800, 600};
    }
    }
}

void CoreImpl::stopFrameProcessing()
{
    stopFrameProcessing_ = true;
}

void CoreImpl::AddToLayer(layer_index_t index, entid_t id, priority_t priority)
{
    entity_manager_.AddToLayer(index, id, priority);
}

void CoreImpl::EraseEntity(entid_t entity)
{
    entity_manager_.EraseEntity(entity);
}

entid_t CoreImpl::CreateEntity(const char *name, ATTRIBUTES *attr)
{
    return entity_manager_.CreateEntity(name, attr);
}

entptr_t CoreImpl::GetEntityPointer(entid_t id) const
{
    return entity_manager_.GetEntityPointer(id);
}

entptr_t CoreImpl::GetEntityPointerSafe(entid_t id) const
{
    return entity_manager_.IsEntityValid(id) ? GetEntityPointer(id) : nullptr;
}

entid_t CoreImpl::GetEntityId(const char *name) const
{
    return entity_manager_.GetEntityId(name);
}

bool CoreImpl::IsEntityValid(entid_t id) const
{
    return entity_manager_.IsEntityValid(id);
}

entity_container_cref CoreImpl::GetEntityIds(layer_type_t type) const
{
    return entity_manager_.GetEntityIds(type);
}

entity_container_cref CoreImpl::GetEntityIds(layer_index_t index) const
{
    return entity_manager_.GetEntityIds(index);
}

entity_container_cref CoreImpl::GetEntityIds(const char *name) const
{
    return entity_manager_.GetEntityIds(name);
}

void CoreImpl::SetLayerType(layer_index_t index, layer_type_t type)
{
    entity_manager_.SetLayerType(index, type);
}

void CoreImpl::HoldSceneRealize(bool held)
{
    entity_manager_.SetLayerHeld(REALIZE, held);
    entity_manager_.SetLayerHeld(SEA_REALIZE, held);
}

void storm::core::HoldSceneRealize(bool held)
{
    core_internal.HoldSceneRealize(held);
}

void CoreImpl::SetLayerFrozen(layer_index_t index, bool freeze)
{
    entity_manager_.SetLayerFrozen(index, freeze);
}

void CoreImpl::RemoveFromLayer(layer_index_t index, entid_t id)
{
    entity_manager_.RemoveFromLayer(index, id);
}

hash_t CoreImpl::GetClassCode(entid_t id) const
{
    return entity_manager_.GetClassCode(id);
}

bool CoreImpl::IsLayerFrozen(layer_index_t index) const
{
    return entity_manager_.IsLayerFrozen(index);
}

void CoreImpl::ForEachEntity(const std::function<void(entptr_t)> &f)
{
    entity_manager_.ForEachEntity(f);
}

void CoreImpl::loadCompatibilitySettings(INIFILE &inifile)
{
    using namespace storm;

    std::array<char, 128> strBuffer{};
    inifile.ReadString("compatibility", "target_version", strBuffer.data(), strBuffer.size(), "latest");
    const std::string_view target_engine_version = strBuffer.data();

    targetVersion_ = getTargetEngineVersion(target_engine_version);
    if (targetVersion_ == ENGINE_VERSION::UNKNOWN)
    {
        spdlog::warn("Unknown target version '{}' in engine compatibility settings", target_engine_version);
        targetVersion_ = ENGINE_VERSION::LATEST;
    }
}
