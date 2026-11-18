// Actual production generator + one-slot queue, inert Session/model/finalization and explicit cache publication.
// Every wait is on progress, never on a clock: a step that cannot finish hangs here and run.py's deadlock guard
// reports the last "step:" line, so the check passes or fails the same way idle and under a loaded land queue.
#include "effects/relief_generator.hpp"
#include "effects/normal_maps.hpp"
#include "effects/upscale.hpp"
#include "finalize_queue.hpp"
#include "backend.hpp"
#include "storm/fx/cpu_admission.hpp"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <future>
#include <set>
#include <stdexcept>
#include <thread>
// Finalizer is inert in this generator/queue control; only complete its owned type.
namespace storm::texture { class EacEncoder {}; }
using namespace storm;
using namespace std::chrono_literals;
namespace {
std::mutex gate_mutex;
std::condition_variable changed;
std::set<uint64_t> committed;
std::atomic<int> computes{0}, tails{0}, active_tails{0}, max_tails{0};
std::atomic<bool> released{false};
// Which thread computed key 101 and which runs its finalization; whether the oversized (902) compute has asked for
// admission and whether that admission returned while a finalization still held the slot.
std::atomic<std::thread::id> compute_thread, tail_thread;
std::atomic<bool> oversize_admitting{false}, oversize_admitted_early{false};
void expect(bool ok,const char *why) { if(!ok){std::fprintf(stderr,"%s\n",why);std::fflush(stderr);std::_Exit(23);} }
void step(const char *what){std::printf("step: %s\n",what);std::fflush(stdout);}
// Polls the generator's observable state until it holds; no deadline (run.py's deadlock guard names the step).
template<class P>void await(const char *what,P predicate) {
 step(what);
 while(!predicate())std::this_thread::sleep_for(1ms);
}
bool has(uint64_t key){std::lock_guard lock(gate_mutex);return committed.contains(key);}
auto entry(uint64_t key) {auto p=std::make_shared<effects::NormalMapEntry>();p->name=std::to_string(key);p->file=p->name;p->key=key;p->state=effects::NormalMapEntry::Waiting;return p;}
void configure(effects::ReliefGenerator&g) {
 {std::lock_guard lock(gate_mutex);committed.clear();}
 computes=tails=active_tails=max_tails=0;released=false;oversize_admitting=oversize_admitted_early=false;
 g.configure({"unused","unused",[](const char*){return std::vector<uint8_t>{1};}});
 g.set_wanted(true,true); // relief and gloss drawn (NormalMaps::configure in the renderer)
}
}
namespace storm::shim {void logf(const char*,...) {}}
namespace storm::effects {
NormalMapEntry::~NormalMapEntry()=default;UpscaleEntry::~UpscaleEntry()=default;
const char *background_wait_reason(){return nullptr;}
bool device_hot(){return false;}
}
namespace storm::fx {
CpuAdmission &SharedCpuAdmission(){static CpuAdmission value(1);return value;}
bool HasReliefMap(const std::filesystem::path&,uint64_t key){return has(key);}
bool HasGlossMap(const std::filesystem::path&,uint64_t key){return has(key);}
bool HasUpscaled(const std::filesystem::path&,uint64_t key){return has(key);}
}
namespace storm::relief {

struct Session::Impl {Stats stats;std::shared_ptr<std::atomic<bool>>cancel=std::make_shared<std::atomic<bool>>(false);std::function<bool(size_t)>admit;};
Session::Session(std::unique_ptr<Impl>p):impl_(std::move(p)){}Session::~Session()=default;
std::unique_ptr<Session>Session::Open(const Models&,int,const std::filesystem::path&,std::string&,std::function<bool()>cancelled){
 if(cancelled&&cancelled())return {};
 return std::unique_ptr<Session>(new Session(std::make_unique<Impl>()));}
const Session::Stats&Session::stats()const{return impl_->stats;}
void Session::Cancel(){*impl_->cancel=true;}
void Session::SetGame(std::function<bool()>,std::function<bool()>){}void Session::SetLoadingRetry(std::shared_ptr<LoadingRetry>){}
void Session::SetCpuBudget(unsigned){}
void Session::SetComputeAdmission(std::function<bool(size_t)>f){impl_->admit=std::move(f);}
void Session::AccountFinalized(const FinalizeResult&r){if(r.outcome==Outcome::Prepared)++impl_->stats.prepared;}
ComputeResult Session::ComputeFile(const std::filesystem::path&,const std::filesystem::path&file,const std::filesystem::path&,int,std::string&,const UpscaleOutput*){
 const auto key=std::stoull(file.string());if(has(key))return Outcome::Existing;
 if(key==902)oversize_admitting=true;
 if(impl_->admit&&!impl_->admit(key==902?(size_t{129}<<20):1024))return Outcome::Failed;
 if(key==902&&active_tails>0)oversize_admitted_early=true; // its workspace admitted beside a running finalization
 if(key==101)compute_thread=std::this_thread::get_id();
 ++computes;auto p=std::make_unique<ComputedTexture>();p->key=key;if(key==101)p->gloss_key=201;
 p->session=1;p->cancelled=impl_->cancel;p->fw=p->fh=16;
 if(key==903)p->fw=p->fh=1024; // estimated tail too large, must drain and run synchronously
 return p;
}
ComputeResult Session::ComputeUpscale(const std::filesystem::path&a,const std::filesystem::path&b,const UpscaleOutput&,int,std::string&e){return ComputeFile(a,b,a,0,e,nullptr);}
Outcome Session::PrepareFile(const std::filesystem::path&a,const std::filesystem::path&b,const std::filesystem::path&c,int d,std::string&e,const UpscaleOutput*f){auto p=ComputeFile(a,b,c,d,e,f);if(!p.product)return p.outcome;Finalizer finalizer;return finalizer.Run(std::move(p.product)).outcome;}
Outcome Session::Upscale(const std::filesystem::path&a,const std::filesystem::path&b,const UpscaleOutput&c,int d,std::string&e){auto p=ComputeUpscale(a,b,c,d,e);if(!p.product)return p.outcome;Finalizer finalizer;return finalizer.Run(std::move(p.product)).outcome;}
bool ReleaseGpu(){return false;}
Finalizer::Finalizer()=default;Finalizer::~Finalizer()=default;
FinalizeResult Finalizer::Run(std::unique_ptr<ComputedTexture>p){
 if(p->key==101)tail_thread=std::this_thread::get_id();
 ++tails;const int active=++active_tails;max_tails=std::max(max_tails.load(),active);
 struct Exit{~Exit(){--active_tails;}}exit;
 if(p->key==103)throw std::runtime_error("controlled finalization failure");
 while(!released&&!*p->cancelled)std::this_thread::sleep_for(1ms);
 FinalizeResult r;r.session=p->session;
 if(*p->cancelled){r.failure="cancelled";return r;}
 {std::lock_guard lock(gate_mutex);if(p->key)committed.insert(*p->key);if(p->gloss_key)committed.insert(*p->gloss_key);}
 r.outcome=Outcome::Prepared;r.relief_written=p->key;r.gloss_written=p->gloss_key;return r;
}
}
int main(){
 auto&g=effects::ReliefGenerator::instance();configure(g);
 auto a=entry(101),b=entry(102),paired=entry(201);paired->gloss=fx::GlossClass::Stone;
 g.request(a,a->file);await("first tail admitted",[]{return tails==1;});
 expect(a->state==effects::NormalMapEntry::Waiting&&g.stats().working,"EARLY_STORED_OR_IDLE");
#ifndef STORM_STAGED_BASELINE
 expect(g.stats().requested>0,"ACTIVE_REQUEST_NOT_COUNTED");
#endif
 // The held tail of 101 occupies the thread that computed it: no second compute can overlap it (the old generator).
 expect(tail_thread.load()!=compute_thread.load(),"SECOND_COMPUTE_DID_NOT_OVERLAP");
 g.request(b,b->file);await("second compute overlaps the held tail",[]{return computes==2;});
 g.request(paired,paired->file);
 expect(tails==1&&a->state==effects::NormalMapEntry::Waiting&&b->state==effects::NormalMapEntry::Waiting,"SECOND_SLOT_OR_EARLY_PUBLICATION");
 released=true;changed.notify_all();
 await("completions published",[&]{return a->state==effects::NormalMapEntry::Stored&&b->state==effects::NormalMapEntry::Stored&&paired->state==effects::NormalMapEntry::Stored;});
 expect(computes==2&&tails==2&&max_tails==1,"PAIRED_DUPLICATE_OR_UNBOUNDED_FINALIZERS");g.shutdown();
 // Current finalizer occupied + next product removed from input queue: both must terminate on shutdown.
 configure(g);a=entry(101);b=entry(102);g.request(a,a->file);await("cancel setup: tail held",[]{return tails==1;});
 g.request(b,b->file);await("cancel setup: next product computed",[]{return computes==2;});step("shutdown cancels both");g.shutdown();
 expect(a->state==effects::NormalMapEntry::Missing&&b->state==effects::NormalMapEntry::Missing,"CANCEL_LEFT_WAITING");
 configure(g);released=true;a=entry(103);b=entry(104);g.request(a,a->file);g.request(b,b->file);
 await("failed finalization does not poison the next",[&]{return a->state==effects::NormalMapEntry::Failed&&b->state==effects::NormalMapEntry::Stored;});g.shutdown();
 // Conservative workspace blocks before next compute; a later oversized tail still makes progress without overlap.
 configure(g);a=entry(101);b=entry(902);g.request(a,a->file);await("oversize setup: tail held",[]{return tails==1;});
 // The tail stays held until 902 asks for admission; an admission that returns while it is still held is recorded
 // by ComputeFile itself, so a generator that does not drain first fails whatever the scheduling.
 g.request(b,b->file);await("oversized compute asks for admission",[]{return oversize_admitting.load();});
 released=true;await("oversized compute progresses after the drain",[&]{return b->state==effects::NormalMapEntry::Stored;});
 expect(!oversize_admitted_early,"OVERSIZE_COMPUTED_BEFORE_DRAIN");
 auto c=entry(903);g.request(c,c->file);await("oversized tail progresses",[&]{return c->state==effects::NormalMapEntry::Stored;});
 expect(max_tails==1,"OVERSIZE_PARALLEL_FINALIZERS");g.shutdown();
 std::puts("actual generator staged overlap, deferred Stored, paired-key reuse, cancel/reset, exception and oversized progress PASS");
}
