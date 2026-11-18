// Real ReliefGenerator::run/queues/entry transitions. Only inference, file producers, and thermal sensors are fake.
// HasUpscaled is the production cache lookup; a cache hit is not a GPU upload and never claims pixels are ready.
#include "effects/relief_generator.hpp"
#include "effects/normal_maps.hpp"
#include "effects/upscale.hpp"
#include "effects/thermal_api.hpp"
#include "relief.hpp"
#include "finalize_queue.hpp"
#include "storm/fx/cpu_admission.hpp"
namespace storm::texture { class EacEncoder {}; }
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <map>
#include <thread>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
using namespace storm;
using namespace std::chrono_literals;
namespace {
std::atomic<int> status{2};
std::atomic<float> forecast{0.95f};
std::atomic<bool> hot{true}, block_job{false}, cancelled{false}, slow_open{false};
std::atomic<int> opened{0}, started{0}, pause_after{0};
std::atomic<bool> gpu_usable{false}, device_is_hot{false};
// The fake Session's device per job (G/C), the worker's nice value and CPU budget as the jobs saw them.
std::atomic<bool> hold_gpu{false}; // a GPU job runs until the scene shows (then it yields, as the real GPU networks do)
std::atomic<int> worker_nice{-100};
std::atomic<unsigned> cpu_budget{0};
std::vector<char> devices;
std::atomic<int64_t> clock_offset{0};

std::atomic<int> list_calls{0}, prune_calls{0};
std::mutex events_mutex;
std::vector<std::string> events;
std::map<std::string,uint64_t> upscale_keys;
std::filesystem::path cache;
void expect(bool value, const char *message) {
    if (!value) { std::fprintf(stderr,"FAIL: %s\n",message); std::exit(1); }
    std::printf("ok: %s\n",message); std::fflush(stdout);
}
// Waits on the worker's progress, never on a clock: a step that cannot finish hangs here and run.py's deadlock guard
// reports it (the last "step:" line). The short sleeps before "must not have started" controls only weaken those
// negatives under load; they never fail a correct worker.
template<class Predicate> void until(Predicate predicate, const char *message) {
    std::printf("step: %s\n",message); std::fflush(stdout);
    while (!predicate()) std::this_thread::sleep_for(1ms);
    expect(true,message);
}
void reset() {
    // Independent cases cannot inherit exact-key completed files from a prior case.
    if(!cache.empty()){std::filesystem::remove_all(cache);std::filesystem::create_directories(cache);}
    status=2; forecast=0.95f; hot=true; block_job=false; cancelled=false; slow_open=false; opened=0; started=0; pause_after=0;
    list_calls=0; prune_calls=0; clock_offset=0; device_is_hot=false; gpu_usable=false; hold_gpu=false;
    worker_nice=-100; cpu_budget=0;
    std::lock_guard lock(events_mutex); events.clear(); devices.clear();
}
relief::Outcome infer(const std::filesystem::path &file, const char *lane) {
    ++started;
    { std::lock_guard lock(events_mutex); events.emplace_back(std::string(lane)+file.string()); }
    while (block_job && !cancelled) std::this_thread::sleep_for(1ms);
    if (cancelled || file.string().find("fail")!=std::string::npos) return relief::Outcome::Failed;
    if (file.string().find("have")!=std::string::npos) return relief::Outcome::Existing;
    std::this_thread::sleep_for(2ms);
    if (pause_after > 0 && started == pause_after) { status=2; forecast=0.95f; }
    return relief::Outcome::Prepared;
}
std::shared_ptr<effects::NormalMapEntry> normal(const char *name, uint64_t key, uint32_t drawn=10) {
    auto e=std::make_shared<effects::NormalMapEntry>();e->name=name;e->file=name;e->key=key;e->drawn=drawn;
    e->state=effects::NormalMapEntry::Waiting;return e;
}
std::shared_ptr<effects::UpscaleEntry> upscale(const char *name, uint64_t key) {
    {std::lock_guard lock(events_mutex);upscale_keys[name]=key;}
    auto e=std::make_shared<effects::UpscaleEntry>();e->name=name;e->file=name;e->key=key;e->width=e->height=1024;
    e->state=effects::UpscaleEntry::Waiting;return e;
}
void configure(effects::ReliefGenerator &g) {
    g.configure({cache,cache,[](const char*){return std::vector<uint8_t>{1};}});
    g.set_display(true,cache,false);
    g.set_wanted(true,true);
}
}
std::chrono::steady_clock::time_point test_now() { return std::chrono::steady_clock::now()+std::chrono::seconds(clock_offset.load()); }
namespace storm::shim { void logf(const char*,...) {} }
namespace storm::effects {
NormalMapEntry::~NormalMapEntry()=default;
UpscaleEntry::~UpscaleEntry()=default;
bool device_hot() { return device_is_hot; }
const char *background_wait_reason() {
    const bool value=thermal_hot(hot.load(),status.load(),forecast.load());hot=value;
    return value?"synthetic thermal hold":nullptr;
}
}
namespace storm::fx {
CpuAdmission &SharedCpuAdmission(){static CpuAdmission value(1);return value;}
bool HasReliefMap(const std::filesystem::path&,uint64_t) { return true; }
bool HasGlossMap(const std::filesystem::path&,uint64_t) { return true; }
}
namespace storm::relief {
struct Session::Impl { Stats stats; std::function<bool()> free, shown; };
Session::Session(std::unique_ptr<Impl> impl):impl_(std::move(impl)) {}
Session::~Session()=default;
std::unique_ptr<Session> Session::Open(const Models&,int,const std::filesystem::path&,std::string&,std::function<bool()> cancel) {
    // A slow model load lasts until the worker is cancelled, so shutdown always lands during loading.
    ++opened; if (slow_open) {while(!(cancel&&cancel()))std::this_thread::sleep_for(1ms);return {};}
    if(cancel&&cancel())return {};
    auto impl=std::make_unique<Impl>();impl->stats.gpu_possible=gpu_usable;return std::unique_ptr<Session>(new Session(std::move(impl)));
}
const Session::Stats &Session::stats() const { return impl_->stats; }
void Session::Cancel() { cancelled=true; }
void Session::SetGame(std::function<bool()> free,std::function<bool()> shown) {impl_->free=std::move(free);impl_->shown=std::move(shown);}
void Session::SetLoadingRetry(std::shared_ptr<LoadingRetry>) {}
void Session::SetCpuBudget(unsigned threads) {cpu_budget=threads;}
void Session::SetComputeAdmission(std::function<bool(size_t)>) {}
void Session::AccountFinalized(const FinalizeResult&) {}
ComputeResult Session::ComputeFile(const std::filesystem::path&a,const std::filesystem::path&b,const std::filesystem::path&c,int d,std::string&e,const UpscaleOutput*f){return PrepareFile(a,b,c,d,e,f);}
ComputeResult Session::ComputeUpscale(const std::filesystem::path&a,const std::filesystem::path&b,const UpscaleOutput&c,int d,std::string&e){return Upscale(a,b,c,d,e);}
Finalizer::Finalizer()=default;Finalizer::~Finalizer()=default;
FinalizeResult Finalizer::Run(std::unique_ptr<ComputedTexture>){std::abort();}
// The production device rule (device_choice.hpp) on the generator's live callbacks; a held GPU job yields when the
// scene shows, as the real GPU networks do at their next submission.
bool fake_device(Session::Impl &impl) {
    const DeviceChoice choice=ChooseTextureDevice(impl.stats.gpu_possible,impl.shown&&impl.free,false,impl.shown&&impl.shown(),
                                                  impl.free&&impl.free(),[]{return false;});
    impl.stats.gpu=choice.gpu;impl.stats.device_why=choice.why;impl.stats.provider=choice.gpu?"GPU fake":"CPU fake";
    worker_nice=getpriority(PRIO_PROCESS,static_cast<id_t>(syscall(SYS_gettid)));
    {std::lock_guard lock(events_mutex);devices.push_back(choice.gpu?'G':'C');}
    return choice.gpu;
}
Outcome Session::PrepareFile(const std::filesystem::path&,const std::filesystem::path &file,
    const std::filesystem::path&,int,std::string&,const UpscaleOutput*) {
    if(fake_device(*impl_) && hold_gpu) {
        ++started;{std::lock_guard lock(events_mutex);events.emplace_back("G:"+file.string());}
        while(!impl_->shown() && !cancelled) std::this_thread::sleep_for(1ms);
        return cancelled?Outcome::Failed:Outcome::Yielded;
    }
    return infer(file,"N:"); }
Outcome Session::Upscale(const std::filesystem::path&,const std::filesystem::path &file,
    const UpscaleOutput&,int,std::string&) { fake_device(*impl_);auto result=infer(file,"U:");if(result==Outcome::Prepared){std::lock_guard lock(events_mutex);const uint8_t payload[]={1,2,3};expect(fx::WriteUpscaled(cache,upscale_keys.at(file.string()),payload),"fake completed upscale commits its exact production key");}return result; }
bool ReleaseGpu() { return false; }
// These sentinels make accidental whole-corpus discovery observable, not merely an inference count.
std::vector<std::filesystem::path> List(const std::filesystem::path&) { ++list_calls; return {"background"}; }
int Prune(const std::filesystem::path&,const std::vector<std::filesystem::path>&,const std::filesystem::path&,int) {
    ++prune_calls; return 0;
}

}
int main(int argc,char **argv) {
    expect(argc==2,"isolated cache supplied");cache=std::filesystem::path(argv[1])/"cache";
    std::filesystem::create_directories(cache);
    auto &g=effects::ReliefGenerator::instance();
    // Automatic corpus discovery/preparation is absent in every renderer state, even after long quiet periods.
    {
        reset();status=0;forecast=0.1f;hot=false;configure(g);
        for(int state=0;state<3;++state) {
            if(state==1) g.frame(true,false); // actual gameplay, independent of quality/provider
            if(state==2) for(int i=0;i<30;++i) g.frame(false,false); // loading/interface, formerly corpus eligible
            { std::lock_guard lock(g.mutex_);clock_offset+=3600;g.wake_.notify_all(); }
            std::this_thread::sleep_for(30ms);
            expect(opened==0 && started==0 && list_calls==0 && prune_calls==0,
                   "idle/missing frames, actual3D and no-scene loading never discover or prepare the corpus");
        }
        // A texture can appear after loading/discovery. It still gets its own real demand preparation.
        auto late=normal("late-loaded-normal",920);auto late_u=upscale("late-loaded-upscale",921);
        g.request(late,late->file);expect(g.request_upscale(late_u),"late-loaded x2 request accepted");
        until([&]{return late->state==effects::NormalMapEntry::Stored && late_u->state==effects::UpscaleEntry::Stored;},
              "late-loaded normal and x2 complete");
        expect(started==2 && list_calls==0 && prune_calls==0,
               "loaded demand performs no whole-game inventory/prune/completion and creates no false warm mark");
        g.shutdown();
    }
    // Actual normal-request comparator: projected-size weighting cannot displace physical distance or aged work.
    {
        auto at=effects::generation_focus().snapshot();
        effects::GenerationScreenSize screen;
        screen.observe(1.0f,10.0f,true,at);
        expect(screen.score(at)==19661,"character projected radius/distance is literally weighted three times");
        screen.observe(0.1f,10.0f,true,at);
        expect(screen.score(at)==19661,"shared texture retains its largest screen size in this window");
        auto later=at;later.window=(later.window+1)&effects::kGenerationWindowMask;
        screen.observe(0.1f,10.0f,true,later);
        expect(screen.score(later)==1966,"new window replaces an old large screen observation");
        auto expired=later;expired.window=(expired.window+4)&effects::kGenerationWindowMask;
        auto other=later;++other.scene;
        expect(screen.score(expired)==0 && screen.score(other)==0,"stale and other-scene screen priority expires");
        effects::GenerationScreenSize invalid;
        invalid.observe(-1,1,true,at);invalid.observe(1,-1,true,at);
        invalid.observe(std::numeric_limits<float>::infinity(),1,true,at);
        expect(invalid.score(at)==0,"invalid size and camera observations create no priority");
        auto person=normal("priority-character",2051),prop=normal("priority-prop",2052);
        person->relief=fx::ReliefClass::Character;
        auto choose=[&] {
            g.requests_.clear();g.admitted_=0;
            g.requests_.push_back({prop,"priority-prop",0});g.requests_.push_back({person,"priority-character",1});
            auto picked=g.next_request();g.requests_.clear();return picked->entry.lock();
        };
        person->priority.observe(9.0f,at);prop->priority.observe(2.0f,at);
        person->screen.observe(100,1,true,at);
        expect(choose()==prop,"closer physical player distance dominates any character screen boost");
        // New snapshot clears the minimum of the old window without waiting on wall time.
        effects::generation_focus().set_scene("equal-character-priority");at=effects::generation_focus().snapshot();
        person->priority.observe(5,at);prop->priority.observe(5,at);
        person->screen.observe(1,10,true,at);prop->screen.observe(2,10,false,at);
        expect(choose()==person,"equal distance: character .1 times three outranks prop .2");
        prop->screen.observe(3.1f,10,false,at);
        expect(choose()==prop,"equal distance: larger prop .31 outranks character weighted .3");
        auto unseen=normal("unseen-character",2053);unseen->screen.observe(100,1,true,at);
        g.requests_.push_back({unseen,"unseen-character",0});g.requests_.push_back({prop,"priority-prop",1});
        expect(g.next_request()->entry.lock()==prop,"screen size gives no camera fallback to an unseen player-distance request");g.requests_.clear();
        // Age is admission based, not time based, and always beats projected size.
        person->queued_after=16;prop->queued_after=0;g.admitted_=16;
        g.requests_.push_back({person,"priority-character",0});g.requests_.push_back({prop,"priority-prop",1});
        expect(g.next_request()->entry.lock()==prop,"sixteen-admission aging outranks screen-size tie break");g.requests_.clear();
    }
    // Real renderer scene presence, independent of lit/baked counts or preset.
    reset();configure(g);
    expect(!g.gpu_free() && !g.scene_shown(),"unobserved renderer is neither idle nor showing a scene");
    g.frame(true,false);
    expect(g.scene_shown(),"a scene drawn while no place loads is shown to the player");
    for (int i=0;i<29;++i) g.frame(false,false);
    expect(!g.gpu_free() && !g.scene_shown(),"one scene frame keeps the 30-frame window busy; the scene no longer shows");
    g.frame(false,false);
    expect(g.gpu_free(),"thirty scene-free frames leave the GPU free");
    g.frame(true,false);
    expect(!g.gpu_free() && g.scene_shown(),"a new perspective scene takes the GPU back immediately");
    g.frame(true,true);
    expect(!g.scene_shown(),"a scene drawn while a place loads is under the loading cover, unseen");
    g.frame(true,false);
    { std::lock_guard lock(g.mutex_);clock_offset+=2; }
    expect(g.gpu_free() && !g.scene_shown(),"no frame for a second (the game in the background): GPU free, no scene shown");
    clock_offset=0;
    g.shutdown();configure(g);
    expect(!g.gpu_free() && !g.scene_shown(),"reconfigure clears stale scene state");
    g.shutdown();
    // The CPU's share for the relief while the game is played is bounded: a quarter of the cores, at most two, at
    // least one (fx::CpuAdmission; heat lowers it further, thermal.hpp), and every core only under a loading picture.
    for (unsigned cores : {1u,2u,4u,6u,8u,12u,16u,64u}) {
        fx::CpuAdmission admission(cores);
        const unsigned play=admission.normal_gameplay_threads();
        expect(play>=1 && play<=2 && play<=std::max(1u,cores/4) && admission.threads()==play,
               "gameplay relief threads: at least one, at most two, at most a quarter of the cores");
        admission.set_gameplay_limit(1);
        expect(admission.threads()==1,"a hot device: one thread");
        admission.set_gameplay_limit(0);
        expect(admission.threads()==1 && admission.gameplay_threads()==0,"thermal SEVERE: no new work admitted (a job's pool never none)");
    }
    // The production worker: during play only CPU jobs start, at nice 10 and the gameplay budget; with no scene the
    // GPU; a scene that starts while a texture runs on the GPU sends it back to the queue, and it is made on the CPU.
    reset();status=0;forecast=0.1f;hot=false;gpu_usable=true;configure(g);
    // The game thread: a frame every 5 ms, a scene or not (frames never stop here, so the scene never goes stale).
    std::atomic<bool> drawing{true}, pumping{true};
    std::thread game([&]{while(pumping){g.frame(drawing,false);std::this_thread::sleep_for(5ms);}});
    auto played=normal("played",81);g.request(played,played->file);
    auto played_u=upscale("played-upscale",82);expect(g.request_upscale(played_u),"played x2 queued");
    until([&]{return played->state==effects::NormalMapEntry::Stored && played_u->state==effects::UpscaleEntry::Stored;},"played jobs complete");
    { std::lock_guard lock(events_mutex);expect(devices==std::vector<char>{'C','C'},"during play only CPU jobs start (relief and x2)"); }
#if defined(__linux__)
    expect(worker_nice==10,"the relief worker runs at nice 10, below the game and render threads");
#endif
    expect(cpu_budget>=1 && cpu_budget<=fx::SharedCpuAdmission().normal_gameplay_threads(),"the CPU job's threads are the gameplay budget");
    drawing=false; // a menu or loading screen without a scene
    until([&]{return g.gpu_free();},"thirty frames without a scene");
    auto menu=normal("menu",83);g.request(menu,menu->file);
    until([&]{return menu->state==effects::NormalMapEntry::Stored;},"no-scene job completes");
    { std::lock_guard lock(events_mutex);expect(devices.back()=='G',"with no scene drawn the GPU job may start"); }
    hold_gpu=true;
    auto midway=normal("midway",84);g.request(midway,midway->file);
    until([&]{std::lock_guard lock(events_mutex);return !events.empty() && events.back()=="G:midway";},"a texture runs on the GPU");
    drawing=true; // the scene starts
    until([&]{return midway->state==effects::NormalMapEntry::Stored;},"the stopped texture completes");
    {
        std::lock_guard lock(events_mutex);
        expect(events.size()>=2 && events[events.size()-2]=="G:midway" && events.back()=="N:midway" &&
               devices.size()>=2 && devices[devices.size()-2]=='G' && devices.back()=='C',
               "a scene starting mid-GPU-job: the texture goes back to the queue and is made on the CPU");
    }
    expect(g.stats().made==3,"the yielded texture is made once, not counted as made or failed twice");
    pumping=false;game.join();hold_gpu=false;
    // Idle release: the models go after 10 s without work; new demand opens them again.
    { std::lock_guard lock(g.mutex_);clock_offset+=11;g.wake_.notify_all(); }
    until([&]{std::lock_guard lock(g.mutex_);return !g.session_;},"10-second idle deadline releases the models");
    auto later=normal("later",85);g.request(later,later->file);
    until([&]{return later->state==effects::NormalMapEntry::Stored;},"new demand after the idle release completes");
    expect(opened==2 && list_calls==0 && prune_calls==0,"the models open again for it; no corpus work");
    g.shutdown();clock_offset=0;
    // The file is already prepared by a preceding normal job, but thermal protection now forbids inference.
    reset();configure(g);
    auto ready=upscale("ready",101), wrong_key=upscale("not-ready",102);
    const uint8_t payload[]={1,2,3};
    expect(fx::WriteUpscaled(cache,ready->key,payload),"production atomic cache writer succeeds");
    expect(g.request_upscale(ready) && g.request_upscale(wrong_key),"both cache requests queued");
    until([&]{return ready->state==effects::UpscaleEntry::Stored;},"completed exact-key upscale publishes while hot");
    expect(wrong_key->state==effects::UpscaleEntry::Waiting,"different key stays pending while hot");
    expect(opened==0 && started==0,"cache completion starts no model or inference under thermal hold");
    g.shutdown();expect(wrong_key->state==effects::UpscaleEntry::Missing,"shutdown releases queued request for later request");
    // Recovered character class uses the real worker, admission/idle policy, cache publication and queue drain.
    // Queue far first: the controlled-player observations, not load order or the camera, must make near first.
    reset();status=0;forecast=0.1f;hot=false;block_job=true;configure(g);
    auto character_anchor=normal("character-anchor",2100,1);g.request(character_anchor,"character-anchor");
    until([&]{return started==1;},"CPU anchor in flight while character requests arrive");
    effects::generation_focus().set_scene("character-queue");
    const auto character_at=effects::generation_focus().snapshot();
    auto far_character=normal("characters/far",2101,1);
    auto near_character=normal("characters/near",2102,1);
    auto middle_character=normal("characters/middle",2103,1);
    auto prop_small=normal("prop-small",2104,1),prop_large=normal("prop-large",2105,1);
    prop_small->relief=prop_large->relief=fx::ReliefClass::Stone;
    prop_small->priority.observe(15.0f,character_at);prop_large->priority.observe(15.0f,character_at);
    prop_small->screen.observe(2.0f,10.0f,false,character_at);prop_large->screen.observe(3.1f,10.0f,false,character_at);
    middle_character->screen.observe(1.0f,10.0f,true,character_at);
    far_character->screen.observe(100.0f,1.0f,true,character_at);

    for (const auto &entry : {far_character,near_character,middle_character}) entry->relief=fx::ReliefClass::Character;
    far_character->priority.observe(70.0f,character_at);
    near_character->priority.observe(1.0f,character_at);
    middle_character->priority.observe(15.0f,character_at);
    g.request(far_character,"characters/far");g.request(near_character,"characters/near");g.request(middle_character,"characters/middle");
    g.request(prop_small,"prop-small");g.request(prop_large,"prop-large");
    block_job=false;
    until([&]{return g.stats().made==6 && g.stats().requested==0;},"all requested character maps complete and real queue drains");
    {
        std::lock_guard lock(events_mutex);
        for (const auto &event:events) std::printf("character event: %s\n",event.c_str());
        expect(events==std::vector<std::string>{"N:character-anchor","N:characters/near","N:prop-large","N:characters/middle","N:prop-small","N:characters/far"},
               "real worker: near first, then weighted same-distance ties, far last despite its enormous screen size");
    }
    expect(near_character->state==effects::NormalMapEntry::Stored && middle_character->state==effects::NormalMapEntry::Stored &&
           far_character->state==effects::NormalMapEntry::Stored,"each character completion publishes for renderer upload");
    g.shutdown();
    // Both live lanes remain populated. Expired entries and cached completions must not consume turns.
    reset();configure(g);
    std::vector<std::shared_ptr<effects::NormalMapEntry>> normals;
    std::vector<std::shared_ptr<effects::UpscaleEntry>> upscales;
    for (int i=0;i<8;++i) {
        const auto name=std::string("normal")+std::to_string(i);
        normals.push_back(normal(name.c_str(),200+i,10+i));g.request(normals.back(),name);
    }
    { auto dead=upscale("expired",300);expect(g.request_upscale(dead),"expired upscale queued"); }
    for (int i=0;i<8;++i) {
        const auto name=std::string("upscale")+std::to_string(i);
        upscales.push_back(upscale(name.c_str(),400+i));expect(g.request_upscale(upscales.back()),"live upscale queued");
    }
    until([&]{return !g.stats().paused.empty();},"thermal pause observed before work");
    status=1;forecast=0.8f;
    std::this_thread::sleep_for(50ms); // runner accelerates only the real worker polling interval
    expect(started==0,"LIGHT with forecast above resume threshold remains paused");
    pause_after=2; forecast=0.6f;
    until([&]{return started==2;},"brief cool window admits only two jobs");
    until([&]{return g.stats().made+g.stats().upscaled==2;},"two admitted jobs finish before the thermal hold");
    expect(g.stats().made==1 && g.stats().upscaled==1,"both lanes make progress in the brief cool window");
    until([&]{return !g.stats().paused.empty();},"thermal rise pauses remaining jobs");
    const int held_count=started;
    std::this_thread::sleep_for(50ms);
    expect(started==held_count,"no inference starts while the thermal hold remains");
    status=1; forecast=0.6f;
    until([&]{return g.stats().made==8 && g.stats().upscaled==8;},"both queues complete after subsequent cooling");
    {
        std::lock_guard lock(events_mutex);
        expect(events.size()==16,"no expired request inferred");
        expect(events.front()=="N:normal7","most recently drawn normal runs first");
        for(size_t i=0;i<events.size();++i)expect(events[i][0]==(i%2?'U':'N'),"normal and upscale lanes alternate while both live");
    }
    g.shutdown();
    // Movement reprioritises pending work across both lanes without cancelling the active job.
    reset();status=1;forecast=0.5f;block_job=true;configure(g);
    auto active=normal("active",600,1);g.request(active,"active");
    until([&]{return started==1;},"first normal inference is running");
    auto near_after_move=normal("moves-near",601,1);
    auto formerly_near=upscale("moves-far",602), middle=upscale("moves-middle",603);
    auto at=effects::generation_focus().snapshot();
    near_after_move->priority.observe(100.0f,at);
    formerly_near->priority.observe(2.0f,at);middle->priority.observe(80.0f,at);
    g.request(near_after_move,"moves-near");
    expect(g.request_upscale(formerly_near) && g.request_upscale(middle),"movement candidates queued");
    until([&]{return effects::generation_focus().snapshot().window!=at.window;},"next spatial refresh window");
    at=effects::generation_focus().snapshot();
    near_after_move->priority.observe(1.0f,at);
    formerly_near->priority.observe(80.0f,at);middle->priority.observe(40.0f,at);
    expect(started==1 && !cancelled,"priority refresh never restarts in-flight inference");
    block_job=false;
    until([&]{return g.stats().made==2 && g.stats().upscaled==2;},"movement candidates complete");
    {
        std::lock_guard lock(events_mutex);
        expect(events==std::vector<std::string>{"N:active","N:moves-near","U:moves-middle","U:moves-far"},
               "near normal beats the upscale turn, and upscales reorder after movement");
    }
    g.shutdown();
    // Continuous nearby work cannot starve still-observed far work. Age is admissions, not time spent hot.
    reset();status=1;forecast=0.5f;block_job=true;configure(g);
    active=normal("age-active",610,1);g.request(active,"age-active");
    until([&]{return started==1;},"ageing setup active job started");
    auto far=normal("age-far",611,1);at=effects::generation_focus().snapshot();
    far->priority.observe(100.0f,at);g.request(far,"age-far");
    std::vector<std::shared_ptr<effects::NormalMapEntry>> crowd;
    for(int i=0;i<20;++i) {
        const auto name=std::string("age-near")+std::to_string(i);
        auto entry=normal(name.c_str(),620+i,1);entry->priority.observe(1.0f,at);
        crowd.push_back(entry);g.request(entry,name);
    }
    block_job=false;
    until([&]{return g.stats().made==22;},"finite live queue drains with ageing");
    {
        std::lock_guard lock(events_mutex);
        expect(events.size()==22 && events[17]=="N:age-far","far live texture gets a turn after 16 other admissions");
    }
    g.shutdown();
    // Both loaded lanes preserve their real admission order even when the old corpus quiet deadline is long past.
    reset();status=1;forecast=0.5f;block_job=true;configure(g);
    active=normal("demand-anchor",650,1);g.request(active,"demand-anchor");
    until([&]{return started==1;},"demand anchor starts");
    auto live=normal("foreground-normal",651,1);auto live_upscale=upscale("foreground-upscale",652);
    g.request(live,"foreground-normal");expect(g.request_upscale(live_upscale),"both loaded lanes queued before idle gap");
    { std::lock_guard lock(g.mutex_);clock_offset+=3600;g.wake_.notify_all(); }
    block_job=false;
    until([&]{return live->state==effects::NormalMapEntry::Stored && live_upscale->state==effects::UpscaleEntry::Stored;},
          "both loaded lanes finish after a long idle-clock gap");
    {
        std::lock_guard lock(events_mutex);
        expect(events==std::vector<std::string>{"N:demand-anchor","U:foreground-upscale","N:foreground-normal"},
               "loaded lane fairness is unchanged and unrelated corpus never follows");
    }
    expect(list_calls==0 && prune_calls==0,"neither loaded lane triggers whole-game discovery or completion");
    g.shutdown();
    // A real inference failure is terminal for that entry and does not starve the other lane.
    reset();status=0;forecast=0.5f;hot=false;configure(g);
    auto failed=upscale("fail",500), good=upscale("good",501);
    expect(g.request_upscale(failed)&&g.request_upscale(good),"failure and following request queued");
    until([&]{return good->state==effects::UpscaleEntry::Stored;},"subsequent upscale succeeds after a failure");
    expect(failed->state==effects::UpscaleEntry::Failed,"real failure stays distinct from thermal waiting");
    g.shutdown();
    // Cancellation of an in-flight operation must not leave an entry permanently Waiting.
    for (bool is_upscale : {false,true}) {
        reset();status=0;forecast=0.5f;hot=false;block_job=true;configure(g);
        auto n=normal("cancel-normal",600);auto u=upscale("cancel-upscale",601);
        if(is_upscale)expect(g.request_upscale(u),"in-flight upscale queued");else g.request(n,n->file);
        until([&]{return started==1;},"in-flight operation started");
        g.shutdown();expect(cancelled,"shutdown cancels running inference");
        expect(is_upscale ? u->state==effects::UpscaleEntry::Missing : n->state==effects::NormalMapEntry::Missing,
               "cancelled in-flight entry leaves Waiting");
    }
    reset();configure(g);status=1;forecast=0.5f;slow_open=true;
    auto opening=upscale("opening",900);expect(g.request_upscale(opening),"model-loading request queued");
    until([&]{return opened==1;},"model loading started");g.shutdown();
    expect(opening->state==effects::UpscaleEntry::Missing,"shutdown during model loading releases request");
    expect(started==0,"shutdown during loading starts no inference");
    // A fresh session can retry that cancelled entry; shutdown never poisons the next model instance.
    reset();configure(g);status=1;forecast=0.5f;
    opening->state=effects::UpscaleEntry::Waiting; // the renderer sets Waiting before publishing the request
    expect(g.request_upscale(opening),"cancelled entry can be queued again");
    until([&]{return opening->state==effects::UpscaleEntry::Stored;},"retry completes in the new session");
    g.shutdown();
    // Only drawn textures' requests are made: nothing ahead for a new place, nothing with relief and gloss off
    // (Низкая), and a new scene or a change of what the draws read drops the queue (the job in progress ends).
    {
        const auto idle=[&]{auto s=g.stats();return !s.working && s.requested==0;};
        const auto ran=[&]{std::lock_guard lock(events_mutex);return events;};
        // A new place (scene) and a long idle clock: the worker starts nothing that no draw requested.
        reset();status=0;forecast=0.1f;hot=false;configure(g);
        effects::generation_focus().set_scene("Charles_town");g.frame(true,false);
        { std::lock_guard lock(g.mutex_);clock_offset+=3600;g.wake_.notify_all(); }
        std::this_thread::sleep_for(50ms);
        expect(started==0 && opened==0,"a new place prepares nothing ahead: no neighbour or other texture without a draw");
        g.shutdown();
        // Низкая: relief and gloss off; a request is not taken (its entry Unqueued), nothing is prepared.
        reset();status=0;forecast=0.1f;hot=false;configure(g);g.set_wanted(false,false);
        auto low=normal("low-preset",3100),low_gloss=normal("low-gloss",3101);low_gloss->gloss=fx::GlossClass::Glass;
        g.request(low,low->file);g.request(low_gloss,low_gloss->file);
        std::this_thread::sleep_for(50ms);
        expect(low->state==effects::NormalMapEntry::Unqueued && low_gloss->state==effects::NormalMapEntry::Unqueued &&
               started==0 && opened==0 && g.stats().requested==0,"relief and gloss off (Низкая): nothing is queued or prepared");
        g.shutdown();
        // drop_queue: the queued requests go back to Unqueued; the job in progress ends; nothing more starts.
        reset();status=0;forecast=0.1f;hot=false;block_job=true;configure(g);
        auto drawn=normal("drawn-now",3200,1);g.request(drawn,drawn->file);
        until([&]{return started==1;},"the drawn texture's job runs");
        auto queued_a=normal("old-scene-a",3201),queued_b=normal("old-scene-b",3202);
        g.request(queued_a,queued_a->file);g.request(queued_b,queued_b->file);
        g.drop_queue("a new scene");
        expect(queued_a->state==effects::NormalMapEntry::Unqueued && queued_b->state==effects::NormalMapEntry::Unqueued,
               "dropped requests are unqueued (asked again when drawn)");
        block_job=false;
        until([&]{return drawn->state==effects::NormalMapEntry::Stored && idle();},"the job in progress ends");
        std::this_thread::sleep_for(50ms);
        expect(ran()==std::vector<std::string>{"N:drawn-now"},"nothing dropped is prepared");
        g.shutdown();
        // The preset lowered while a map is made (Высокая -> Низкая): the queue is dropped and the job stops.
        reset();status=0;forecast=0.1f;hot=false;block_job=true;configure(g);
        auto running=normal("lowered-running",3300,1),waiting=normal("lowered-waiting",3301);
        g.request(running,running->file);
        until([&]{return started==1;},"a map is being made when the preset is lowered");
        g.request(waiting,waiting->file);
        g.set_wanted(false,false);
        expect(waiting->state==effects::NormalMapEntry::Unqueued,"lowering the preset drops the queued request");
        { std::lock_guard lock(g.mutex_);expect(!g.running_wanted(),"the job in progress is no longer wanted: it stops at its next stage"); }
        block_job=false;
        until([&]{return idle();},"the stopped job leaves the worker");
        expect(running->state!=effects::NormalMapEntry::Stored && started==1,"the stopped job publishes nothing and nothing else starts");
        g.shutdown();
    }
    std::puts("relief_scheduler: PASS");
}
