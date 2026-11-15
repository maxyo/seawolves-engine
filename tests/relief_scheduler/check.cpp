// Real ReliefGenerator::run/queues/entry transitions. Only inference, file producers, and thermal sensors are fake.
// HasUpscaled is the production cache lookup; a cache hit is not a GPU upload and never claims pixels are ready.
#include "effects/relief_generator.hpp"
#include "effects/normal_maps.hpp"
#include "effects/upscale.hpp"
#include "effects/thermal_api.hpp"
#include "relief.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <thread>
using namespace storm;
using namespace std::chrono_literals;
namespace {
std::atomic<int> status{2};
std::atomic<float> forecast{0.95f};
std::atomic<bool> hot{true}, block_job{false}, cancelled{false}, slow_open{false};
std::atomic<int> opened{0}, started{0}, pause_after{0}, idle_releases{0};
std::atomic<bool> cpu_detour{false};
std::atomic<int64_t> clock_offset{0};

std::atomic<bool> background_enabled{false}, background_complete{false};
std::mutex events_mutex;
std::vector<std::string> events;
std::filesystem::path cache;
void expect(bool value, const char *message) {
    if (!value) { std::fprintf(stderr,"FAIL: %s\n",message); std::exit(1); }
    std::printf("ok: %s\n",message); std::fflush(stdout);
}
template<class Predicate> void until(Predicate predicate, const char *message) {
    const auto end=std::chrono::steady_clock::now()+7s;
    while (!predicate() && std::chrono::steady_clock::now()<end) std::this_thread::sleep_for(5ms);
    expect(predicate(),message);
}
void reset() {
    status=2; forecast=0.95f; hot=true; block_job=false; cancelled=false; slow_open=false; opened=0; started=0; pause_after=0; background_enabled=false; background_complete=false;
    std::lock_guard lock(events_mutex); events.clear();
}
relief::Outcome infer(const std::filesystem::path &file, const char *lane) {
    ++started;
    { std::lock_guard lock(events_mutex); events.emplace_back(std::string(lane)+file.string()); }
    while (block_job && !cancelled) std::this_thread::sleep_for(1ms);
    if (cancelled || file.string().find("fail")!=std::string::npos) return relief::Outcome::Failed;
    std::this_thread::sleep_for(2ms);
    if (pause_after > 0 && started == pause_after) { status=2; forecast=0.95f; }
    return relief::Outcome::Prepared;
}
std::shared_ptr<effects::NormalMapEntry> normal(const char *name, uint64_t key, uint32_t drawn=10) {
    auto e=std::make_shared<effects::NormalMapEntry>();e->name=name;e->file=name;e->key=key;e->drawn=drawn;
    e->state=effects::NormalMapEntry::Waiting;return e;
}
std::shared_ptr<effects::UpscaleEntry> upscale(const char *name, uint64_t key) {
    auto e=std::make_shared<effects::UpscaleEntry>();e->name=name;e->file=name;e->key=key;e->width=e->height=1024;
    e->state=effects::UpscaleEntry::Waiting;return e;
}
void configure(effects::ReliefGenerator &g) {
    g.configure({cache,cache,[](const char*){return std::vector<uint8_t>{1};}});
    g.set_display(true,cache,false);
}
}
std::chrono::steady_clock::time_point test_now() { return std::chrono::steady_clock::now()+std::chrono::seconds(clock_offset.load()); }
namespace storm::shim { void logf(const char*,...) {} }
namespace storm::effects {
NormalMapEntry::~NormalMapEntry()=default;
UpscaleEntry::~UpscaleEntry()=default;
const char *background_wait_reason() {
    const bool value=thermal_hot(hot.load(),status.load(),forecast.load());hot=value;
    return value?"synthetic thermal hold":nullptr;
}
}
namespace storm::fx {
bool HasReliefMap(const std::filesystem::path&,uint64_t) { return true; }
bool HasGlossMap(const std::filesystem::path&,uint64_t) { return true; }
}
namespace storm::relief {
struct Session::Impl { Stats stats; std::chrono::steady_clock::time_point since=test_now(); bool free=false; };
Session::Session(std::unique_ptr<Impl> impl):impl_(std::move(impl)) {}
Session::~Session()=default;
std::unique_ptr<Session> Session::Open(const Models&,int,const std::filesystem::path&,std::string&) {
    ++opened; if (slow_open) std::this_thread::sleep_for(150ms);
    auto impl=std::make_unique<Impl>();impl->stats.gpu_possible=cpu_detour;return std::unique_ptr<Session>(new Session(std::move(impl)));
}
const Session::Stats &Session::stats() const { return impl_->stats; }
void Session::Cancel() { cancelled=true; }
void Session::SetGpuFree(bool value) {impl_->free=value;}
void Session::SetLoadingRetry(std::shared_ptr<LoadingRetry>) {}
void Session::ReleaseIdleModels() {++idle_releases;}
bool Session::WaitsForGpu() const { return cpu_detour && !impl_->stats.gpu && !impl_->free && test_now()-impl_->since<std::chrono::seconds(60); }
Outcome Session::PrepareFile(const std::filesystem::path&,const std::filesystem::path &file,
    const std::filesystem::path&,int,std::string&,const UpscaleOutput*) { if(cpu_detour && !WaitsForGpu()) impl_->stats.gpu=true; return infer(file,"N:"); }
Outcome Session::Upscale(const std::filesystem::path&,const std::filesystem::path &file,
    const UpscaleOutput&,int,std::string&) { return infer(file,"U:"); }
bool ReleaseGpu() { return false; }
std::vector<std::filesystem::path> List(const std::filesystem::path&) {
    return background_enabled ? std::vector<std::filesystem::path>{"background"} : std::vector<std::filesystem::path>{};
}
int Prune(const std::filesystem::path&,const std::vector<std::filesystem::path>&,const std::filesystem::path&,int) { return 0; }
void MarkComplete(const std::filesystem::path&,int,size_t) { background_complete=true; }
bool Complete(const std::filesystem::path&,int,size_t,int*) { return !background_enabled || background_complete; }
}
int main(int argc,char **argv) {
    expect(argc==2,"isolated cache supplied");cache=std::filesystem::path(argv[1])/"cache";
    std::filesystem::create_directories(cache);
    auto &g=effects::ReliefGenerator::instance();
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
    expect(!g.gpu_free(),"unobserved renderer is not assumed idle");
    g.frame(true);
    for (int i=0;i<29;++i) g.frame(false);
    expect(!g.gpu_free(),"one scene frame keeps the 30-frame window busy");
    g.frame(false);
    expect(g.gpu_free(),"thirty scene-free frames permit GPU retry");
    g.frame(true);
    expect(!g.gpu_free(),"a new perspective scene becomes busy immediately");
    g.shutdown();configure(g);
    expect(!g.gpu_free(),"reconfigure clears stale scene-free state");
    g.shutdown();
    // Full production worker lifecycle with deterministic time: inference objects are released at 10s,
    // but background admission must retain the same CPU detour until its 60s GPU retry is due.
    reset();status=0;forecast=0.5f;hot=false;cpu_detour=true;idle_releases=0;configure(g);
    auto anchor=normal("retry-anchor",91);g.request(anchor,"retry-anchor");
    until([&]{return g.stats().made==1;},"CPU-detour anchor completes");
    background_enabled=true;g.set_active(true);
    auto advance=[&](int seconds) {
        std::lock_guard lock(g.mutex_);clock_offset+=seconds;g.wake_.notify_all();
    };
    advance(11);
    until([&]{std::lock_guard lock(g.mutex_);return idle_releases>0 || !g.session_ || opened>1 || started>1;},"10-second idle deadline processed");
    expect(idle_releases==1 && opened==1 && started==1,"idle releases inference objects without reopening or CPU background work");
    advance(48);
    {std::lock_guard lock(g.mutex_);g.wake_.notify_all();}
    std::this_thread::sleep_for(30ms);
    expect(opened==1 && started==1 && !background_complete,"59-second blocked detour still admits no background job");
    advance(2);
    until([&]{return background_complete.load();},"61-second retry admits background and completes");
    expect(opened==1 && started==2 && g.session_->stats().gpu,"same policy session retries GPU without re-admitting CPU background");
    g.shutdown();cpu_detour=false;clock_offset=0;
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
    std::this_thread::sleep_for(5200ms);
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
    // Make background work genuinely due while both foreground lanes wait. Backdate only the quiet deadline,
    // under the worker's own mutex, to avoid a 30-second idle test. All selection and preparation paths are real.
    reset();status=1;forecast=0.5f;block_job=true;configure(g);
    active=normal("background-anchor",650,1);g.request(active,"background-anchor");
    until([&]{return started==1;},"background test active job started");
    auto live=normal("foreground-normal",651,1);auto live_upscale=upscale("foreground-upscale",652);
    g.request(live,"foreground-normal");expect(g.request_upscale(live_upscale),"both foreground lanes queued before background");
    background_enabled=true;g.set_active(true);
    {
        std::lock_guard lock(g.mutex_);
        g.last_request_=std::chrono::steady_clock::now()-31s;
    }
    block_job=false;
    until([&]{return background_complete.load();},"due background work eventually completes");
    {
        std::lock_guard lock(events_mutex);
        expect(events==std::vector<std::string>{"N:background-anchor","U:foreground-upscale","N:foreground-normal","N:background"},
               "background begins only after both loaded foreground lanes drain");
    }
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
    std::puts("relief_scheduler: PASS");
}
