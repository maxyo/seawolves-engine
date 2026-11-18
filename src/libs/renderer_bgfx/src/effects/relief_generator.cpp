#include "storm/fx/cpu_admission.hpp"
#include "loading_retry.hpp"
#include "effects/relief_generator.hpp"

#include "effects/normal_maps.hpp"
#include "effects/thermal.hpp"
#include "backend.hpp"
#include "effects/upscale.hpp"
#include "relief.hpp"
#include "finalize_queue.hpp"
#include "shim/shim.hpp"
#include "storm/fx/relief_store.hpp"

#include <algorithm>
#include <chrono>
#include <limits>

#if defined(__linux__) || defined(__ANDROID__)
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace storm::effects
{

namespace
{
using Clock = std::chrono::steady_clock;
constexpr auto kIdleRelease = std::chrono::seconds(10);
// Separate <=128MiB estimates for pending CPU tail and next compute: <=256MiB combined,
// excluding source file bytes, model/runtime arenas, upload/GPU memory. Not a process RSS bound.
constexpr size_t kAddedStageBytes = size_t{128} << 20;
constexpr auto kWaitPause = std::chrono::seconds(5); // asked again this often while the work must wait

double Ms(Clock::time_point since)
{
    return std::chrono::duration<double, std::milli>(Clock::now() - since).count();
}

} // namespace

ReliefGenerator &ReliefGenerator::instance()
{
    fx::SharedCpuAdmission(); // destroyed after this worker has joined
    static ReliefGenerator generator;
    return generator;
}

ReliefGenerator::~ReliefGenerator()
{
    shutdown();
}

void ReliefGenerator::configure(ReliefSetup setup)
{
    shutdown();
    std::lock_guard lock(mutex_);
    setup_ = std::move(setup);
    stopping_ = false;
    stats_ = {};
    model_failed_ = false;
    std::fill(std::begin(scene_draws_), std::end(scene_draws_), false);
    scene_frame_ = 0;
    scene_quiet_ = false;
    scene_shown_ = false;
    last_frame_us_ = 0;
    device_logged_ = false;
    next_upscale_ = false;
    admitted_ = 0;
    next_finalize_ = 0; finalizing_.reset(); computing_ = computing_normal_ = computing_gloss_ = false;
    auto finalizer = std::make_shared<relief::Finalizer>();
    finalizer_queue_ = std::make_unique<relief::FinalizeQueue>(
        [finalizer](std::unique_ptr<relief::ComputedTexture> product) { return finalizer->Run(std::move(product)); },
        [this] {
            // Completion is stored under the queue's mutex, whereas the consumer waits under ours.
            // Take ours before notification so completion cannot land between its predicate check
            // and the atomic release/wait, losing the only wakeup for an occupied slot.
            std::lock_guard lock(mutex_);
            wake_.notify_all();
        }, [] {
#if defined(__linux__) || defined(__ANDROID__)
            setpriority(PRIO_PROCESS, static_cast<id_t>(syscall(SYS_gettid)), 10);
#endif
        });
    idle_since_ = Clock::now();
    shim::logf("effects: relief generation: only the textures the current scene draws with their relief or gloss");
    worker_ = std::thread([this] { run(); });
}

void ReliefGenerator::shutdown()
{
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
        if (loading_retry_) loading_retry_->active = false;
        loading_retry_.reset(); loading_normal_.clear(); loading_upscale_.clear();
        if (running_)
            running_->Cancel();
        if (finalizer_queue_) finalizer_queue_->request_stop();
        for (const Request &request : requests_)
            if (const std::shared_ptr<NormalMapEntry> entry = request.entry.lock())
                entry->state = NormalMapEntry::Missing;
        requests_.clear();
        for (const std::weak_ptr<UpscaleEntry> &weak : upscales_)
            if (const std::shared_ptr<UpscaleEntry> entry = weak.lock())
                entry->state = UpscaleEntry::Missing;
        upscales_.clear();
    }
    wake_.notify_all();
    if (worker_.joinable())
        worker_.join();
    if (finalizer_queue_) finalizer_queue_->stop();
    std::lock_guard lock(mutex_);
    if (finalizing_) {
        if (finalizing_->request) if (auto entry = finalizing_->request->entry.lock())
            if (entry->state == NormalMapEntry::Waiting) entry->state = NormalMapEntry::Missing;
        if (auto entry = finalizing_->upscale.lock())
            if (entry->state == UpscaleEntry::Waiting) entry->state = UpscaleEntry::Missing;
    }
    finalizing_.reset(); finalizer_queue_.reset(); inline_finalizer_.reset();
    computing_ = computing_normal_ = computing_gloss_ = false;
    running_ = nullptr;
    session_.reset();
    // The Vulkan instance closes with the generator, not in ncnn's exit handler: by then the driver's own exit
    // handlers have run and the process crashed in it.
    relief::ReleaseGpu();
    for (std::vector<uint8_t> &bytes : models_)
        bytes = {};
}

bool ReliefGenerator::request_upscale(const std::shared_ptr<UpscaleEntry> &entry)
{
    {
        std::lock_guard lock(mutex_);
        if (!worker_.joinable() || stopping_ || model_failed_ || !display_)
            return false;
        std::erase_if(upscales_, [](const std::weak_ptr<UpscaleEntry> &weak) { return weak.expired(); });
        if (upscales_.size() >= kUpscaleQueue)
            return false;
        entry->queued_after = admitted_;
        entry->queued_scene = generation_focus().snapshot().scene;
        upscales_.push_back(entry);
    }
    wake_.notify_all();
    return true;
}

void ReliefGenerator::set_display(bool on, std::filesystem::path directory, bool bc)
{
    std::lock_guard lock(mutex_);
    display_ = on;
    display_directory_ = std::move(directory);
    display_bc_ = bc;
    if (on)
        return;
    size_t dropped = 0;
    for (const std::weak_ptr<UpscaleEntry> &weak : upscales_)
        if (const std::shared_ptr<UpscaleEntry> entry = weak.lock(); entry && entry->state == UpscaleEntry::Waiting)
        {
            entry->state = UpscaleEntry::Missing;
            ++dropped;
        }
    upscales_.clear();
    if (dropped || (computing_ && !computing_normal_))
        shim::logf("effects: relief generation: ×2 textures switched off: %zu queued dropped%s", dropped,
                   computing_ && !computing_normal_ ? ", the one in progress stops" : "");
}

void ReliefGenerator::set_wanted(bool relief, bool gloss)
{
    std::lock_guard lock(mutex_);
    want_relief_ = relief;
    want_gloss_ = gloss;
    wake_.notify_all();
    size_t dropped = 0;
    std::erase_if(requests_, [&](const Request &request) {
        const std::shared_ptr<NormalMapEntry> entry = request.entry.lock();
        if (!entry)
            return true;
        if (wanted(*entry))
            return false;
        if (entry->state == NormalMapEntry::Waiting)
            entry->state = NormalMapEntry::Unqueued; // looked up again when its product is switched on
        ++dropped;
        return true;
    });
    const bool stops = computing_normal_ && !running_wanted();
    if (dropped || stops)
        shim::logf("effects: relief generation: %s off: %zu queued textures dropped%s",
                   !relief && !gloss ? "relief and gloss maps" : !relief ? "relief maps" : "gloss maps", dropped,
                   stops ? ", the one in progress stops" : "");
}

void ReliefGenerator::drop_queue(const char *why)
{
    std::lock_guard lock(mutex_);
    size_t dropped = 0;
    for (const Request &request : requests_)
        if (const std::shared_ptr<NormalMapEntry> entry = request.entry.lock();
            entry && entry->state == NormalMapEntry::Waiting)
        {
            entry->state = NormalMapEntry::Unqueued; // the next draw that reads it asks again
            ++dropped;
        }
    requests_.clear();
    if (dropped)
        shim::logf("effects: relief generation: %s: %zu queued textures dropped (asked again when drawn)", why, dropped);
}

bool ReliefGenerator::wanted(const NormalMapEntry &entry) const
{
    return entry.gloss != fx::GlossClass::None ? want_gloss_ : want_relief_;
}

bool ReliefGenerator::running_wanted() const
{
    if (!computing_)
        return true;
    if (computing_normal_)
        return computing_gloss_ ? want_gloss_ : want_relief_;
    return display_;
}

void ReliefGenerator::request(const std::shared_ptr<NormalMapEntry> &entry, std::filesystem::path file)
{
    {
        std::lock_guard lock(mutex_);
        if (!worker_.joinable() || stopping_)
        {
            entry->state = NormalMapEntry::Missing;
            return;
        }
        if (!wanted(*entry))
        {
            entry->state = NormalMapEntry::Unqueued; // switched off meanwhile: looked up again when switched on
            return;
        }
        entry->queued_after = admitted_;
        entry->queued_scene = generation_focus().snapshot().scene;
        requests_.push_back({entry, std::move(file), order_++});
    }
    wake_.notify_all();
}

ReliefGenerator::Stats ReliefGenerator::stats()
{
    std::lock_guard lock(mutex_);
    Stats stats = stats_;
    stats.requested = static_cast<uint32_t>(std::count_if(requests_.begin(), requests_.end(), [](const Request &request) {
        return !request.entry.expired();
    }));
    stats.requested += (computing_normal_ ? 1u : 0u) + (finalizing_ && !finalizing_->is_upscale ? 1u : 0u);
    stats.working = computing_ || finalizing_.has_value();
    return stats;
}

namespace
{
template<class Entry> auto rank(const std::shared_ptr<Entry> &entry, GenerationSnapshot at, uint64_t admitted)
{
    auto value = entry ? generation_rank(entry->priority, at, admitted - entry->queued_after)
                       : std::pair<uint32_t, float>{UINT32_MAX, std::numeric_limits<float>::infinity()};
    if (entry && value.first == UINT32_MAX && entry->queued_scene == at.scene)
        --value.first; // unseen current-scene load before retained entries from a previous scene
    return value;
}
}

std::optional<ReliefGenerator::Request> ReliefGenerator::next_request()
{
    std::erase_if(requests_, [](const Request &request) { return request.entry.expired(); });
    if (requests_.empty())
        return std::nullopt;
    const auto at = generation_focus().snapshot();
    const auto available = [this](const Request &request) {
        const auto entry = request.entry.lock();
        return entry && (!finalizing_ || finalizing_->is_upscale ||
            (finalizing_->relief_key != entry->key && finalizing_->gloss_key != entry->key));
    };
    const auto better = [&](const Request &a, const Request &b) {
        const auto ea = a.entry.lock(), eb = b.entry.lock();
        const auto ra = rank(ea, at, admitted_), rb = rank(eb, at, admitted_);
        if (ra != rb) return ra < rb;
        if (ra.second < 0.0f) return a.order < b.order; // oldest admitted-waiter first
        if (std::isfinite(ra.second)) // unseen/stale work gains no screen priority
        {
            const auto sa = ea->screen.score(at), sb = eb->screen.score(at);
            if (sa != sb) return sa > sb;
        }
        const uint32_t da = ea ? ea->drawn.load() : 0, db = eb ? eb->drawn.load() : 0;
        return da != db ? da > db : a.order < b.order;
    };
    auto best = requests_.end();
    for (auto it = requests_.begin(); it != requests_.end(); ++it)
        if (available(*it) && (best == requests_.end() || better(*it, *best))) best = it;
    if (best == requests_.end()) return {};
    Request request = std::move(*best);
    requests_.erase(best);
    return request;
}

std::shared_ptr<UpscaleEntry> ReliefGenerator::next_upscale()
{
    if (upscales_.empty()) return {};
    const auto at = generation_focus().snapshot();
    const auto better = [&](const auto &a, const auto &b) {
        const auto ea = a.lock(), eb = b.lock();
        const auto ra = rank(ea, at, admitted_), rb = rank(eb, at, admitted_);
        if (ra != rb) return ra < rb;
        return ra.second < 0.0f && ea && eb && ea->queued_after < eb->queued_after;
    };
    auto best = upscales_.end();
    for (auto it = upscales_.begin(); it != upscales_.end(); ++it) {
        const auto entry = it->lock();
        if (entry && finalizing_ && finalizing_->is_upscale && entry->key == finalizing_->key) continue;
        if (best == upscales_.end() || better(*it, *best)) best = it;
    }
    if (best == upscales_.end()) return {};
    auto result = best->lock();
    upscales_.erase(best);
    return result;
}

void ReliefGenerator::reconcile_upscales(std::unique_lock<std::mutex> &lock)
{
    // A normal-map job can also write this exact display texture. Publishing that completed file is not inference:
    // do it before the thermal gate, or a warm phone can leave an already prepared texture invisible indefinitely.
    // The texture worker still validates its header, dimensions and payload before any GPU replacement is bound.
    // The file checks run with mutex_ released: request() takes it on the game thread.
    std::vector<std::shared_ptr<UpscaleEntry>> waiting;
    std::erase_if(upscales_, [&](const std::weak_ptr<UpscaleEntry> &weak) {
        const auto entry = weak.lock();
        if (!entry || entry->state != UpscaleEntry::Waiting)
            return true;
        waiting.push_back(entry);
        return false;
    });
    if (!display_ || waiting.empty())
        return;
    const std::filesystem::path directory = display_directory_;
    lock.unlock();
    std::erase_if(waiting, [&](const std::shared_ptr<UpscaleEntry> &entry) { return !fx::HasUpscaled(directory, entry->key); });
    lock.lock();
    if (waiting.empty() || !display_ || display_directory_ != directory)
        return;
    for (const std::shared_ptr<UpscaleEntry> &entry : waiting)
        if (entry->state == UpscaleEntry::Waiting)
            entry->state = UpscaleEntry::Stored;
    std::erase_if(upscales_, [](const std::weak_ptr<UpscaleEntry> &weak) {
        const auto entry = weak.lock();
        return !entry || entry->state != UpscaleEntry::Waiting;
    });
}

void ReliefGenerator::run()
{
#if defined(__linux__) || defined(__ANDROID__)
    // Below the game's threads; ncnn's CPU thread pool (simpleomp), created on this thread at its first inference,
    // inherits it.
    setpriority(PRIO_PROCESS, static_cast<id_t>(syscall(SYS_gettid)), 10);
#endif
    for (;;)
    {
        drain_finalized();
        std::optional<Request> request;
        std::shared_ptr<UpscaleEntry> upscale;
        std::shared_ptr<relief::LoadingRetry> loading_retry;
        {
            std::unique_lock lock(mutex_);
            for (;;)
            {
                if (stopping_)
                    return;
                if (finalizer_queue_->completed()) {
                    lock.unlock(); drain_finalized(); lock.lock();
                    continue;
                }
                if (trim_)
                {
                    trim_ = false;
                    release_models("the system is low on memory");
                }
                const Clock::time_point now = Clock::now();
                reconcile_upscales(lock);
                // Everything below is read again after the file checks; only these two were checked before them.
                if (stopping_)
                    return;
                if (finalizer_queue_->completed())
                    continue;
                // Reselect from fresh player-distance observations before every job; keep lane fairness.
                request = next_request();
                upscale = next_upscale();
                if (request && upscale)
                {
                    const auto normal = request->entry.lock();
                    const auto at = generation_focus().snapshot();
                    const auto nr = rank(normal, at, admitted_), ur = rank(upscale, at, admitted_);
                    bool choose_upscale = next_upscale_;
                    if (nr.first != ur.first) choose_upscale = ur.first < nr.first;
                    else if ((nr.second < 0.0f) != (ur.second < 0.0f)) choose_upscale = ur.second < 0.0f;
                    else if (nr.second < 0.0f && normal && normal->queued_after != upscale->queued_after)
                        choose_upscale = upscale->queued_after < normal->queued_after;
                    if (choose_upscale)
                    {
                        requests_.push_front(std::move(*request));
                        request.reset();
                    }
                    else
                    {
                        upscales_.push_front(upscale);
                        upscale.reset();
                    }
                }
                if (request || upscale)
                    break;
                // Nothing else of the game, in any state: idle until a draw asks for a map.
                Clock::time_point until = Clock::time_point::max();
                if (session_)
                {
                    if (now - idle_since_ >= kIdleRelease)
                    {
                        release_models("idle");
                        continue;
                    }
                    until = std::min(until, idle_since_ + kIdleRelease);
                }
                if (until == Clock::time_point::max())
                    wake_.wait(lock);
                else
                    wake_.wait_until(lock, until);
            }
            if (must_wait())
            {
                if (request)
                    requests_.push_front(std::move(*request));
                if (upscale)
                    upscales_.push_front(upscale);
                wake_.wait_for(lock, kWaitPause, [this] { return stopping_.load(); });
                continue;
            }
            next_upscale_ = request.has_value(); // pauses and cache-only completion consume no turn
            ++admitted_;
            // Only the current location's drawn requests can claim this permission.
            if (loading_retry_ && ((request && std::find(loading_normal_.begin(), loading_normal_.end(), request->entry.lock()) != loading_normal_.end()) ||
                (upscale && std::find(loading_upscale_.begin(), loading_upscale_.end(), upscale) != loading_upscale_.end())))
                loading_retry = loading_retry_;
            stats_.working = true;
            computing_ = true; computing_normal_ = request.has_value();
            const std::shared_ptr<NormalMapEntry> normal = request ? request->entry.lock() : nullptr;
            computing_gloss_ = normal && normal->gloss != fx::GlossClass::None;
            if (!running_wanted())
            {
                // Its product was switched off after it was taken: nothing is loaded or computed for it.
                if (normal && normal->state == NormalMapEntry::Waiting)
                    normal->state = NormalMapEntry::Unqueued;
                if (upscale && upscale->state == UpscaleEntry::Waiting)
                    upscale->state = UpscaleEntry::Missing;
                stats_.working = false; computing_ = computing_normal_ = computing_gloss_ = false;
                continue;
            }
        }
        if (!fx::SharedCpuAdmission().wait_for_opaque([this] { return stopping_.load(); })) {
            std::lock_guard lock(mutex_);
            if (request) requests_.push_front(std::move(*request));
            if (upscale) upscales_.push_front(upscale);
            stats_.working = false; computing_ = computing_normal_ = computing_gloss_ = false;
            continue; // generation changed: reselect; shutdown checked at loop top
        }
        if (!open_session())
        {
            std::lock_guard lock(mutex_);
            if (request)
                if (const std::shared_ptr<NormalMapEntry> entry = request->entry.lock())
                    entry->state = NormalMapEntry::Missing;
            if (upscale)
                upscale->state = stopping_ ? UpscaleEntry::Missing : UpscaleEntry::Failed;
            for (const Request &r : requests_)
                if (const std::shared_ptr<NormalMapEntry> entry = r.entry.lock())
                    entry->state = NormalMapEntry::Missing;
            requests_.clear();
            stats_.working = false; computing_ = computing_normal_ = computing_gloss_ = false;
            continue;
        }
        session_->SetComputeAdmission([this](size_t workspace) {
            {
                std::lock_guard lock(mutex_);
                if (!running_wanted())
                    return false; // its product was switched off: the job stops here (finish)
            }
            return workspace <= kAddedStageBytes ? !stopping_.load() : wait_finalizer();
        });
        session_->SetCpuBudget(fx::SharedCpuAdmission().threads());
        session_->SetLoadingRetry(loading_retry);
        try {
            if (request) prepare_request(*request);
            else if (upscale) prepare_upscale(upscale);
        } catch (const std::exception &e) {
            shim::logf("ERROR: effects: texture preparation exception: %s", e.what());
            if (request) if (auto entry = request->entry.lock())
                entry->state = stopping_ ? NormalMapEntry::Missing : NormalMapEntry::Failed;
            if (upscale) upscale->state = stopping_ ? UpscaleEntry::Missing : UpscaleEntry::Failed;
        }
        std::lock_guard lock(mutex_);
        idle_since_ = Clock::now();
        stats_.working = false; computing_ = computing_normal_ = computing_gloss_ = false;
    }
}

bool ReliefGenerator::open_session()
{
    if (session_)
        return true;
    if (model_failed_)
        return false;
    if (!setup_.model)
    {
        // A host without engine.ini relief_models: a configuration, not a defect.
        model_failed_ = true;
        shim::logf("effects: relief generation off: no relief models configured (engine.ini relief_models): textures "
                   "without a map get no relief");
        return false;
    }
    const Clock::time_point start = Clock::now();
    std::string error = "the relief models could not be read";
    bool read = true;
    static_assert(std::tuple_size_v<decltype(models_)> == std::size(relief::kModelFiles));
    for (size_t i = 0; i < std::size(relief::kModelFiles); ++i)
    {
        models_[i] = setup_.model(relief::kModelFiles[i]);
        if (models_[i].empty())
        {
            read = false;
            error = std::string("cannot read the model file ") + relief::kModelFiles[i];
        }
    }
    const int threads = static_cast<int>(fx::SharedCpuAdmission().threads());
    const relief::Models models{models_[0], models_[1], models_[2], models_[3], models_[4], models_[5], models_[6], models_[7]};
    std::unique_ptr<relief::Session> session =
        read ? relief::Session::Open(models, threads, setup_.directory, error, [this] { return stopping_.load(); }) : nullptr;
    if (!session)
    {
        model_failed_ = true;
        for (std::vector<uint8_t> &bytes : models_)
            bytes = {};
        shim::logf("ERROR: effects: relief generation unavailable (%s): textures without a map get no relief",
                   error.c_str());
        return false;
    }
    size_t bytes = 0;
    for (const std::vector<uint8_t> &model : models_)
        bytes += model.size();
    shim::logf("effects: relief generation: PBRify models loaded (%.1f MB) on %s in %.0f ms",
               static_cast<double>(bytes) / 1048576.0, session->stats().provider.c_str(), Ms(start));
    // Into the game's log (and so the diagnostics): the library's own log reaches only logcat.
    if (!session->stats().gpu_crash.empty())
        shim::logf("ERROR: effects: relief GPU inference crashed the previous run on %s: using the CPU",
                   session->stats().gpu_crash.c_str());
    std::lock_guard lock(mutex_);
    if (stopping_)
        return false;
    stats_.device = session->stats().provider;
    // The device of each texture follows the game's state (relief device_choice.hpp), read live on this thread.
    session->SetGame([this] { return gpu_free(); }, [this] { return scene_shown(); });
    session_ = std::move(session);
    running_ = session_.get();
    return true;
}

void ReliefGenerator::prepare_request(const Request &request)
{
    const std::shared_ptr<NormalMapEntry> entry = request.entry.lock();
    if (!entry)
        return; // unloaded meanwhile
    std::optional<relief::UpscaleOutput> display;
    {
        std::lock_guard lock(mutex_);
        if (display_)
            display = relief::UpscaleOutput{display_directory_, display_bc_};
    }
    const Clock::time_point start = Clock::now();
    std::string failure;
    auto computed = session_->ComputeFile(setup_.textures, request.file, setup_.directory, fx::kReliefMapSize,
                                          failure, display ? &*display : nullptr);
    note_device();
    if (computed.outcome == relief::Outcome::Yielded)
    {
        // Stopped on the GPU because the scene shows: back to the queue, for the CPU (the scene is shown now).
        std::lock_guard lock(mutex_);
        if (!stopping_ && entry->state == NormalMapEntry::Waiting && wanted(*entry))
        {
            requests_.push_front(request);
            shim::logf("effects: relief generation: %s stopped on the GPU (the scene shows): back to the queue for the CPU",
                       entry->name.c_str());
        }
        else if (entry->state == NormalMapEntry::Waiting)
            entry->state = stopping_ ? NormalMapEntry::Missing : NormalMapEntry::Unqueued;
        return;
    }
    PendingFinalize pending; pending.id = ++next_finalize_; pending.key = entry->key; pending.request = request;
    if (computed.product) stage(std::move(computed.product), std::move(pending));
    else {
        relief::FinalizeResult result; result.outcome = computed.outcome; result.failure = std::move(failure);
        result.total_ms = Ms(start); finish(pending, result);
    }
}

bool ReliefGenerator::wait_finalizer()
{
    for (;;) {
        drain_finalized();
        std::unique_lock lock(mutex_);
        if (stopping_) return false;
        if (!finalizer_queue_->occupied()) return true;
        wake_.wait(lock, [this] { return stopping_ || finalizer_queue_->completed(); });
    }
}

void ReliefGenerator::stage(std::unique_ptr<relief::ComputedTexture> product, PendingFinalize pending)
{
    pending.relief_key = product->kind == relief::ComputedTexture::Kind::Relief ? product->key : std::nullopt;
    pending.gloss_key = product->gloss_key;
    if (!wait_finalizer()) {
        relief::FinalizeResult cancelled; cancelled.failure = "cancelled"; finish(pending, cancelled); return;
    }
    if (product->staging_bytes() > kAddedStageBytes) {
        shim::logf("effects: relief finalizer: %zu owned bytes, %zu estimated stage bytes exceeds %zu bound; synchronous progress",
                   product->owned_bytes(), product->staging_bytes(), kAddedStageBytes);
        if (!inline_finalizer_) inline_finalizer_ = std::make_unique<relief::Finalizer>();
        relief::FinalizeResult result;
        try { result = inline_finalizer_->Run(std::move(product)); }
        catch (const std::exception &e) { result.failure = e.what(); }
        catch (...) { result.failure = "finalizer exception"; }
        if (session_) session_->AccountFinalized(result);
        finish(pending, result);
        return;
    }
    std::unique_lock lock(mutex_);
    if (stopping_) {
        lock.unlock(); relief::FinalizeResult cancelled; cancelled.failure = "cancelled";
        finish(pending, cancelled); return;
    }
    finalizing_ = pending;
    if (!finalizer_queue_->submit(pending.id, product)) {
        finalizing_.reset(); lock.unlock();
        relief::FinalizeResult failed; failed.failure = "finalizer slot unavailable";
        finish(pending, failed); return; // never publish an uncommitted entry
    }
    computing_ = computing_normal_ = computing_gloss_ = false;
}

void ReliefGenerator::drain_finalized()
{
    if (!finalizer_queue_) return;
    auto done = finalizer_queue_->take();
    if (!done) return;
    std::optional<PendingFinalize> pending;
    {
        std::lock_guard lock(mutex_);
        if (finalizing_ && finalizing_->id == done->id) pending = std::move(finalizing_);
        finalizing_.reset();
    }
    if (done->exception) {
        try { std::rethrow_exception(done->exception); }
        catch (const std::exception &e) { done->result.failure = e.what(); }
        catch (...) { done->result.failure = "finalizer exception"; }
        done->result.outcome = relief::Outcome::Failed;
    }
    if (session_) session_->AccountFinalized(done->result); // only this compute owner touches Session
    if (pending) finish(*pending, done->result);
}

void ReliefGenerator::finish(const PendingFinalize &pending, const relief::FinalizeResult &result)
{
    // The committed file is looked up before mutex_ is taken (request() takes it on the game thread). setup_ changes
    // only in configure(), with this worker joined; the display directory is compared again under the lock.
    const bool produced = result.outcome == relief::Outcome::Prepared || result.outcome == relief::Outcome::Existing;
    std::filesystem::path upscale_directory;
    bool present = false;
    if (pending.is_upscale) {
        {
            std::lock_guard lock(mutex_);
            if (display_) upscale_directory = display_directory_;
        }
        present = (produced || result.upscale_written == pending.key) && !upscale_directory.empty() &&
                  fx::HasUpscaled(upscale_directory, pending.key);
    } else {
        if (const auto entry = pending.request ? pending.request->entry.lock() : nullptr) {
            const bool gloss = entry->gloss != fx::GlossClass::None;
            const bool committed = (gloss ? result.gloss_written : result.relief_written) == pending.key;
            present = (produced || committed) &&
                      (gloss ? fx::HasGlossMap(setup_.directory, pending.key) : fx::HasReliefMap(setup_.directory, pending.key));
        }
    }
    std::lock_guard lock(mutex_);
    if (pending.is_upscale) {
        const auto entry = pending.upscale.lock();
        if (!entry || entry->key != pending.key || entry->state != UpscaleEntry::Waiting) return;
        if (stopping_ || !display_) { entry->state = UpscaleEntry::Missing; return; } // switched off: asked again when on
        if (display_directory_ != upscale_directory) { entry->state = UpscaleEntry::Missing; return; } // moved meanwhile
        if (present) {
            if (result.outcome == relief::Outcome::Prepared) { ++stats_.upscaled; stats_.upscale_ms += result.total_ms; }
            entry->state = UpscaleEntry::Stored;
        } else {
            shim::logf("ERROR: effects upscale: ×2 of %s not made (%s)", entry->name.c_str(), result.failure.c_str());
            entry->state = UpscaleEntry::Failed;
        }
        return;
    }
    const auto entry = pending.request ? pending.request->entry.lock() : nullptr;
    if (!entry || entry->key != pending.key || entry->state != NormalMapEntry::Waiting) return;
    if (stopping_) { entry->state = NormalMapEntry::Missing; return; }
    if (!wanted(*entry)) { entry->state = NormalMapEntry::Unqueued; return; } // switched off: looked up again when on
    if (result.outcome == relief::Outcome::Prepared) {
        ++stats_.made; stats_.made_ms += result.total_ms;
        stats_.slowest_ms = std::max(stats_.slowest_ms, result.total_ms);
    }
    const bool gloss = entry->gloss != fx::GlossClass::None;
    if (present) {
        entry->state = NormalMapEntry::Stored;
    } else {
        shim::logf("ERROR: effects: %s map of %s not made (%s)", gloss ? "gloss" : "relief", entry->name.c_str(),
                   result.failure.empty() ? "no committed map under its expected key" : result.failure.c_str());
        entry->state = NormalMapEntry::Failed;
    }
}

void ReliefGenerator::loading_window(uint64_t generation, uint64_t deadline_ms,
    std::vector<std::shared_ptr<NormalMapEntry>> normal, std::vector<std::shared_ptr<UpscaleEntry>> upscale)
{
    std::lock_guard lock(mutex_);
    if (loading_retry_ && generation == loading_generation_ && deadline_ms == loading_retry_->deadline_ms) return;
    if (loading_retry_) loading_retry_->active = false;
    loading_retry_.reset(); loading_generation_ = generation;
    loading_normal_ = std::move(normal); loading_upscale_ = std::move(upscale);
    if (deadline_ms && (!loading_normal_.empty() || !loading_upscale_.empty()))
        loading_retry_ = std::make_shared<relief::LoadingRetry>(deadline_ms, generation);
    wake_.notify_all();
}

void ReliefGenerator::frame(bool scene_drawn, bool loading)
{
    last_frame_us_ = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now().time_since_epoch()).count();
    scene_shown_ = scene_drawn && !loading;
    scene_draws_[scene_frame_++ % std::size(scene_draws_)] = scene_drawn;
    const bool quiet = std::none_of(std::begin(scene_draws_), std::end(scene_draws_), [](bool drawn) { return drawn; }); // a loading or interface screen draws no scene (a menu over the scene still does)
    if (quiet != scene_quiet_)
        shim::logf(quiet ? "effects: relief generation: the game draws no scene (a loading or interface screen): the GPU "
                           "is free for the relief"
                         : "effects: relief generation: the game draws its scene again: the GPU is the game's");
    scene_quiet_ = quiet;
}

namespace
{
// No frame for a second: the game is in the background.
bool FramesStopped(int64_t last)
{
    const int64_t now = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now().time_since_epoch()).count();
    return last != 0 && now - last > 1000000;
}
}

bool ReliefGenerator::gpu_free() const
{
    return scene_quiet_ || FramesStopped(last_frame_us_);
}

bool ReliefGenerator::scene_shown() const
{
    return scene_shown_ && !FramesStopped(last_frame_us_);
}

void ReliefGenerator::note_device()
{
    const relief::Session::Stats &session = session_->stats();
    std::lock_guard lock(mutex_);
    stats_.device = session.provider;
    if (session.device_why == relief::DeviceReason::None || (device_logged_ && device_gpu_ == session.gpu))
        return;
    shim::logf("effects: relief generation now on the %s (%s): %s", session.gpu ? "GPU" : "CPU",
               relief::DeviceReasonText(session.device_why), session.provider.c_str());
    device_logged_ = true;
    device_gpu_ = session.gpu;
}

void ReliefGenerator::trim()
{
    {
        std::lock_guard lock(mutex_);
        trim_ = true;
    }
    wake_.notify_all();
}

void ReliefGenerator::release_models(const char *why)
{
    const bool had = session_ != nullptr;
    running_ = nullptr;
    session_.reset();
    for (std::vector<uint8_t> &bytes : models_)
        bytes = {};
    const bool gpu = relief::ReleaseGpu();
    if (had || gpu)
        shim::logf("effects: relief generation (%s): models released%s", why, gpu ? ", the GPU's Vulkan instance closed" : "");
}

void ReliefGenerator::prepare_upscale(const std::shared_ptr<UpscaleEntry> &entry)
{
    relief::UpscaleOutput display;
    {
        std::lock_guard lock(mutex_);
        if (!display_)
        {
            entry->state = UpscaleEntry::Missing; // switched off meanwhile
            return;
        }
        display = {display_directory_, display_bc_};
    }
    const Clock::time_point start = Clock::now();
    std::string failure;
    auto computed = session_->ComputeUpscale(setup_.textures, entry->file, display,
        static_cast<int>(std::max(entry->width, entry->height)), failure);
    note_device();
    if (computed.outcome == relief::Outcome::Yielded)
    {
        std::lock_guard lock(mutex_); // as prepare_request
        if (!stopping_ && display_ && entry->state == UpscaleEntry::Waiting)
        {
            upscales_.push_front(entry);
            shim::logf("effects: relief generation: ×2 of %s stopped on the GPU (the scene shows): back to the queue for the CPU",
                       entry->name.c_str());
        }
        else if (entry->state == UpscaleEntry::Waiting)
            entry->state = UpscaleEntry::Missing;
        return;
    }
    PendingFinalize pending; pending.id = ++next_finalize_; pending.key = entry->key;
    pending.upscale = entry; pending.is_upscale = true;
    if (computed.product) stage(std::move(computed.product), std::move(pending));
    else {
        relief::FinalizeResult result; result.outcome = computed.outcome; result.failure = std::move(failure);
        result.total_ms = Ms(start); finish(pending, result);
    }
}

bool ReliefGenerator::must_wait()
{
    const char *why = background_wait_reason();
    if (why && stats_.paused != why)
        shim::logf("effects: relief generation paused: %s", why);
    else if (!why && !stats_.paused.empty())
        shim::logf("effects: relief generation resumed (was paused: %s)", stats_.paused.c_str());
    stats_.paused = why ? why : "";
    return why != nullptr;
}

} // namespace storm::effects
