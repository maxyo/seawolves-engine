#include "effects/normal_maps.hpp"

#include "effects/relief_generator.hpp"
#include "relief.hpp"
#include "shim/device.hpp"
#include "storm/fx/height_map.hpp"
#include "storm/fx/parallax_trust.hpp"
#include "storm/fx/lighting.hpp"
#include "storm/fx/relief_store.hpp"
#include "texture/core/formats.hpp"
#include "texture/core/service.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <mutex>
#include <optional>

namespace storm::effects
{

namespace
{
// The relief directory (ConfigureRelief), shared with the worker tasks.
std::mutex directory_mutex;
std::filesystem::path relief_directory;

std::filesystem::path directory()
{
    std::lock_guard lock(directory_mutex);
    return relief_directory;
}

std::filesystem::path user_exclusions_file;
} // namespace

void ConfigureParallaxExclusions(const std::filesystem::path &user_file)
{
    std::lock_guard lock(directory_mutex);
    user_exclusions_file = user_file;
}

void ConfigureRelief(ReliefSetup setup)
{
    {
        std::lock_guard lock(directory_mutex);
        relief_directory = setup.directory;
    }
    ReliefGenerator::instance().configure(std::move(setup));
}

NormalMapEntry::~NormalMapEntry()
{
    if (!shim::Context::instance().initialised())
        return;
    if (bgfx::isValid(handle))
        bgfx::destroy(handle);
    if (bgfx::isValid(material))
        bgfx::destroy(material);
}

void NormalMaps::configure(bool enabled, int max_size, bool material, bool characters)
{
    const bool readers = enabled != enabled_ || characters != characters_;
    const bool changed = enabled != enabled_ || max_size != max_size_ || material != material_;
    enabled_ = enabled;
    max_size_ = max_size;
    material_ = material;
    characters_ = characters;
    // The generator takes no request of a product that is off, and drops its queue (and the job in progress) when it
    // goes off.
    ReliefGenerator::instance().set_wanted(enabled_, gloss_enabled_);
    if (readers)
        forget_drawn("the draws that read the relief changed");
    if (!changed)
        return;
    std::erase_if(entries_, [](const std::weak_ptr<NormalMapEntry> &entry) { return entry.expired(); });
    for (const std::weak_ptr<NormalMapEntry> &weak : entries_)
        if (const std::shared_ptr<NormalMapEntry> entry = weak.lock())
            release(*entry); // read back (or looked up) when a draw reads it again
}

void NormalMaps::forget_drawn(const char *why)
{
    for (auto *list : {&entries_, &gloss_entries_})
        for (const std::weak_ptr<NormalMapEntry> &weak : *list)
            if (const std::shared_ptr<NormalMapEntry> entry = weak.lock())
                entry->drawn = 0;
    ReliefGenerator::instance().drop_queue(why);
}

void NormalMaps::begin_scene(const char *scene)
{
    // The previous scene's line: what its draws looked up, by why (the class a draw read the relief of, or the gloss).
    const SceneTally &t = *tally_;
    uint32_t requested = 0, stored = 0;
    std::string why;
    for (size_t c = 0; c < t.relief_requested.size(); ++c)
    {
        requested += t.relief_requested[c];
        stored += t.relief_stored[c];
        if (const uint32_t n = t.relief_requested[c])
            why += (why.empty() ? "" : ", ") + std::string(fx::ReliefClassName(static_cast<fx::ReliefClass>(c))) + " " +
                   std::to_string(n);
    }
    if (!scene_.empty())
        shim::logf("effects: relief generation in %s: queued %u relief maps (drawn with relief: %s) and %u gloss maps "
                   "(drawn with gloss); %u relief and %u gloss maps the draws looked up were on disk; nothing else queued",
                   scene_.c_str(), requested, why.empty() ? "none" : why.c_str(), t.gloss_requested.load(), stored,
                   t.gloss_stored.load());
    scene_ = scene ? scene : "";
    tally_ = std::make_shared<SceneTally>();
    forget_drawn("a new scene");
}

bool NormalMaps::reads(const NormalMapEntry &entry) const
{
    return entry.gloss != fx::GlossClass::None ? gloss_enabled_ : enabled_;
}

void NormalMaps::release(NormalMapEntry &entry)
{
    if (entry.state != NormalMapEntry::Uploaded)
        return;
    if (bgfx::isValid(entry.handle))
        bgfx::destroy(entry.handle);
    if (bgfx::isValid(entry.material))
        bgfx::destroy(entry.material);
    entry.handle = entry.material = BGFX_INVALID_HANDLE;
    entry.depth = 0.0f;
    entry.bytes = entry.material_bytes = 0;
    entry.wanted = false;
    entry.state = NormalMapEntry::Stored; // read back from the relief directory when drawn again
}

void NormalMaps::on_texture_loaded(shim::TexStore &store, const char *path, const std::filesystem::path &file)
{
    if (!path || store.kind != shim::TexKind::Tex2D || store.subs.empty())
        return;
    // Every texture of a relief class gets its entry (its key needs the probe level, which only the load has), relief
    // on or off; it is looked up now or when relief is switched on (configure).
    const std::string name = fx::NormalizeTexturePath(path);
    const uint32_t format = static_cast<uint32_t>(store.format);
    const bool readable = texture::tx_format_known(format);
    // The probe level: the key's part of the texture, and where "bump" names are checked for grey (fx/relief_store.hpp).
    const int probe = fx::ReliefProbeLevel(static_cast<int>(store.width), static_cast<int>(store.height),
                                           static_cast<int>(store.levels));
    const uint32_t pw = probe >= 0 ? store.level_width(static_cast<uint32_t>(probe)) : 0,
                   ph = probe >= 0 ? store.level_height(static_cast<uint32_t>(probe)) : 0;
    const std::vector<uint8_t> *probe_data = probe >= 0 ? &store.sub(0, static_cast<uint32_t>(probe)).data : nullptr;
    const size_t probe_bytes = readable && probe >= 0 ? texture::d3d_level_bytes(format, pw, ph) : 0;
    std::vector<uint8_t> small;
    if (fx::NameSuggestsHeightMap(name) && probe_bytes && probe_data->size() >= probe_bytes)
    {
        small.resize(size_t(pw) * ph * 4);
        texture::decode_rgba8(format, probe_data->data(), pw, ph, small.data());
    }
    const fx::ReliefVerdict verdict = classifier_.Classify(path, small);
    // A texture of a relief class with a height has its gloss in its relief's material texture; the others their own
    // gloss map.
    if (readable && probe_bytes && probe_data->size() >= probe_bytes && !fx::ReliefHasHeight(verdict.relief))
        add_gloss(store, name, {probe_data->data(), probe_bytes}, pw, ph, file);
    if (verdict.relief == fx::ReliefClass::None)
        return;
    if (verdict.relief == fx::ReliefClass::HeightMap)
    {
        // A grey "bump" texture is height data: a normal derived from its brightness is never used for shading, so
        // none is made. Its surface has no relief.
        shim::logf("effects: relief %s: a grey bump texture: no relief from it (brightness-derived normals are not used)",
                   name.c_str());
        return;
    }
    if (!readable)
    {
        if (!format_reported_)
            shim::logf("ERROR: effects: texture %s (relief %s) has D3D format %u, which the relief does not read: no "
                       "relief for it (and textures like it)",
                       name.c_str(), fx::ReliefClassName(verdict.relief), format);
        format_reported_ = true;
        return;
    }
    if (!probe_bytes || probe_data->size() < probe_bytes)
        return; // smaller than a 4x4 block below 64 px: no relief (the preparation skips it too)
    if (directory().empty())
    {
        if (!directory_reported_)
            shim::logf("ERROR: effects: no relief directory (render::ConfigureRelief): no relief at all");
        directory_reported_ = true;
        return;
    }

    auto entry = std::make_shared<NormalMapEntry>();
    entry->name = name;
    entry->relief = verdict.relief;
    entry->material_gloss = fx::ReliefHasHeight(verdict.relief) ? fx::ClassifyGlossName(name).gloss : fx::GlossClass::None;
    entry->file = file;
    entry->key = fx::ReliefKey(name, {probe_data->data(), probe_bytes}, static_cast<int>(pw), static_cast<int>(ph),
                               fx::ReliefMethod(verdict.relief), fx::kReliefMapSize);
    store.normal_map = entry;
    entries_.push_back(entry); // looked up when a draw reads it (upload)
}

void NormalMaps::look_up(const std::shared_ptr<NormalMapEntry> &entry)
{
    entry->state = NormalMapEntry::Pending;
    entry->cache_task = std::make_shared<texture::CacheTask>();
    if (!texture::Service::instance().post_cache(entry->cache_task,
        [entry, relief_dir = directory(), tally = tally_] {
        // Which map the texture takes, for the diagnostics: its key and the method it was (or will be) made with.
        const unsigned long long key = entry->key, method = fx::ReliefMethod(entry->relief);
        const size_t c = std::min(static_cast<size_t>(entry->relief), tally->relief_requested.size() - 1);
        if (fx::HasReliefMap(relief_dir, entry->key))
        {
            shim::logf("effects: relief %s (%s): map %016llx of method %016llx", entry->name.c_str(),
                       fx::ReliefClassName(entry->relief), key, method);
            ++tally->relief_stored[c];
            entry->state = NormalMapEntry::Stored;
            return;
        }
        shim::logf("effects: relief %s (%s): no map %016llx of method %016llx yet: drawn with its relief, requested",
                   entry->name.c_str(), fx::ReliefClassName(entry->relief), key, method);
        ++tally->relief_requested[c];
        entry->state = NormalMapEntry::Waiting; // counted in the "effects relief:" line
        ReliefGenerator::instance().request(entry, entry->file);
    })) entry->state = NormalMapEntry::Unqueued;
}

void NormalMaps::read_stored(const std::shared_ptr<NormalMapEntry> &entry)
{
    entry->state = NormalMapEntry::Pending;
    entry->cache_task = std::make_shared<texture::CacheTask>();
    if (!texture::Service::instance().post_cache(entry->cache_task,
        [weak = std::weak_ptr<NormalMapEntry>(entry), relief_dir = directory()] {
        const auto entry = weak.lock();
        if (!entry) return;
        const auto file = entry->gloss != fx::GlossClass::None ? fx::GlossFile(relief_dir, entry->key) : fx::ReliefFile(relief_dir, entry->key);
        std::error_code size_error;
        const auto bytes = std::filesystem::file_size(file, size_error);
        if (size_error) {
            if (size_error != std::errc::no_such_file_or_directory)
                shim::logf("ERROR: relief cache stat failed: %s: %s", entry->name.c_str(), size_error.message().c_str());
            entry->state = size_error == std::errc::no_such_file_or_directory ? NormalMapEntry::Unqueued : NormalMapEntry::Failed;
            return;
        }
        {
            const auto admission = texture::Service::instance().reserve_cache_bytes(entry->cache_task, bytes);
            if (admission != texture::Service::CacheBytes::Reserved) {
                if (admission == texture::Service::CacheBytes::TooLarge)
                    shim::logf("ERROR: relief cache read exceeds 256 MiB budget: %s", entry->name.c_str());
                entry->state = admission == texture::Service::CacheBytes::Busy ? NormalMapEntry::Stored : NormalMapEntry::Failed;
                return;
            }
        }
        std::string error;
        std::optional<fx::NormalMap> map = entry->gloss != fx::GlossClass::None
                                               ? fx::ReadGlossMap(relief_dir, entry->key, &error, bytes)
                                               : fx::ReadReliefMap(relief_dir, entry->key, &error, bytes);
        if (!map)
        {
            entry->cache_task->bytes.reset();
            if (error == "cache read reservation changed") { entry->state = NormalMapEntry::Stored; return; }
            // Gone from the cache while the texture stayed loaded (the system evicted it, «Очистить кэш») or damaged
            // (deleted by the read): made again like a map that never was (the next draw looks it up).
            if (!error.empty())
                shim::logf("ERROR: effects: %s: made again", error.c_str());
            else
                shim::logf("effects: relief %s: its map %016llx left the cache: made again", entry->name.c_str(),
                           static_cast<unsigned long long>(entry->key));
            entry->state = NormalMapEntry::Unqueued;
            return;
        }
        entry->map = std::move(*map);
        entry->state = NormalMapEntry::Derived;
    })) entry->state = NormalMapEntry::Stored;
}

void NormalMaps::load_exclusions()
{
    exclusions_loaded_ = true;
    std::vector<std::string> problems;
    exclusions_.LoadShipped(fx::ParallaxExclusions::ShippedText(), &problems);
    for (const std::string &problem : problems)
        shim::logf("ERROR: effects: parallax exclusions: %s", problem.c_str());
    std::filesystem::path user;
    {
        std::lock_guard lock(directory_mutex);
        user = user_exclusions_file;
    }
    bool readable = true;
    if (!user.empty())
        readable = exclusions_.LoadUser(user);
    shim::logf("effects: parallax exclusions: %zu textures in the shipped list, %zu in the user list %s%s", exclusions_.shipped_count(),
               exclusions_.user_count(), user.empty() ? "(none configured)" : user.string().c_str(),
               readable ? "" : " (ERROR: cannot be read)");
}

bool NormalMaps::parallax_allowed(const NormalMapEntry &entry)
{
    if (!exclusions_loaded_)
        load_exclusions();
    switch (exclusions_.Of(entry.name))
    {
    case fx::ParallaxExclusions::Verdict::Excluded:
        return false;
    case fx::ParallaxExclusions::Verdict::Included:
        return true;
    case fx::ParallaxExclusions::Verdict::Default:
        break;
    }
    return fx::ReliefTakesParallax(entry.relief) && entry.trust_coverage >= fx::kParallaxMinCoverage;
}

ParallaxInfo NormalMaps::parallax_info(const std::string &name)
{
    ParallaxInfo info;
    for (const std::weak_ptr<NormalMapEntry> &weak : entries_)
    {
        const std::shared_ptr<NormalMapEntry> entry = weak.lock();
        if (!entry || entry->name != name)
            continue;
        info.found = true;
        info.uploaded = entry->state == NormalMapEntry::Uploaded;
        info.coverage = entry->trust_coverage;
        info.trust = entry->trust;
        info.trust_width = entry->trust_width;
        info.trust_height = entry->trust_height;
        if (!exclusions_loaded_)
            load_exclusions();
        const auto verdict = exclusions_.Of(name);
        info.parallax = info.uploaded && bgfx::isValid(entry->material) && parallax_allowed(*entry);
        if (!info.uploaded)
            info.reason = "its relief is not made yet";
        else if (!bgfx::isValid(entry->material))
            info.reason = "no height";
        else if (verdict == fx::ParallaxExclusions::Verdict::Excluded)
            info.reason = "excluded by a list";
        else if (!fx::ReliefTakesParallax(entry->relief))
            info.reason = "its class (furniture, wood, props...) takes none";
        else if (!info.parallax)
            info.reason = "too few trusted texels";
        break;
    }
    return info;
}

bool NormalMaps::toggle_parallax(const std::string &name, bool &parallax_now)
{
    const ParallaxInfo info = parallax_info(name);
    if (!info.found)
        return false;
    const bool wanted = !info.parallax;
    if (!exclusions_.SetUser(name, wanted))
    {
        shim::logf("ERROR: effects: parallax of %s: the user list cannot be written", name.c_str());
        return false;
    }
    parallax_now = wanted;
    shim::logf("effects: parallax of %s: switched %s by the user (appended to the user list)", name.c_str(), wanted ? "on" : "off");
    return true;
}

void NormalMaps::retry_scene(shim::TexStore &store)
{
    for (const auto &entry : {store.normal_map, store.gloss_map}) if (entry) {
        const int state = entry->state.load();
        if (state == NormalMapEntry::Failed || state == NormalMapEntry::Missing) {
            entry->state = NormalMapEntry::Unqueued; entry->failure_reported = false;
        }
    }
}

int NormalMaps::prepare_scene(shim::TexStore &store, fx::Lighting &lighting, bool *inference_pending)
{
    if (inference_pending) *inference_pending = false;
    // No entry was constructed when the required cache was not configured. That omission is
    // not evidence that the enabled method has no work for this location.
    if ((enabled_ || gloss_enabled_) && directory().empty()) return 3;
    bool waiting = false, failed = false, reload_required = false;
    const auto prepare = [&](const std::shared_ptr<NormalMapEntry> &entry, int size) {
        if (!entry) return;
        auto &service = texture::Service::instance();
        service.promote_cache(entry->cache_task);
        const Relief map = upload(entry, lighting, size, false);
        service.promote_cache(entry->cache_task);
        const int state = entry->state.load(std::memory_order_acquire);
        if (inference_pending && state == NormalMapEntry::Waiting) *inference_pending = true;
        if (state == NormalMapEntry::Failed && (bgfx::isValid(entry->handle) || bgfx::isValid(entry->material)))
            reload_required = true; // partial upload: do not overwrite a still-owned handle on retry
        else if (state == NormalMapEntry::Failed || state == NormalMapEntry::Missing) failed = true;
        else if (state != NormalMapEntry::Uploaded || map.fade < 1.0f) waiting = true;
        else if (!bgfx::isValid(map.handle) ||
                 (material_ && entry->depth > 0.0f && !entry->no_height_reported && !bgfx::isValid(map.material))) reload_required = true;
    };
    // Height-class gloss is carried by normal_map.material, even with normal shading disabled.
    // Match Lighting::bind's consumer rather than assuming every gloss map has a separate entry.
    // Only what a draw of this scene read: a texture merely loaded starts nothing on the loading screen.
    const auto drawn = [](const std::shared_ptr<NormalMapEntry> &entry) { return entry && entry->drawn != 0; };
    if (drawn(store.normal_map) &&
        (enabled_ || (gloss_enabled_ && store.normal_map->material_gloss != fx::GlossClass::None)))
        prepare(store.normal_map, max_size_);
    if (gloss_enabled_ && drawn(store.gloss_map)) prepare(store.gloss_map, gloss_max_size_);
    return reload_required ? 3 : failed ? 2 : waiting ? 1 : 0;
}

NormalMaps::Relief NormalMaps::handle(shim::TexStore &store, fx::Lighting &lighting)
{
    return store.normal_map ? upload(store.normal_map, lighting, max_size_) : Relief{};
}

NormalMaps::Selection NormalMaps::select(shim::TexStore *colour, shim::TexStore *detail, fx::Lighting &lighting)
{
    Selection selected;
    shim::TexStore *stages[] = {colour, detail};
    for (const int stage : {1, 0})
    {
        shim::TexStore *store = stages[stage];
        if (!store || !store->normal_map)
            continue;

        const Relief map = handle(*store, lighting);
        if (!selected.entry || bgfx::isValid(map.handle))
            selected = {map, store->normal_map.get(), stage};
        if (bgfx::isValid(map.handle))
            break;
    }
    return selected;
}

NormalMaps::Relief NormalMaps::gloss(shim::TexStore &store, fx::Lighting &lighting)
{
    return store.gloss_map ? upload(store.gloss_map, lighting, gloss_max_size_) : Relief{};
}

NormalMaps::Relief NormalMaps::upload(const std::shared_ptr<NormalMapEntry> &entry, fx::Lighting &lighting, int max_size,
                                      bool draw)
{
    entry->wanted = true;
    if (draw)
        entry->drawn = frame_;
    switch (entry->state.load())
    {
    case NormalMapEntry::Unqueued: // no draw read it yet, its request was dropped, or its map left the cache
        if (!reads(*entry))
            return {}; // its product is off: nothing is looked up or made for it
        if (entry->gloss != fx::GlossClass::None)
            look_up_gloss(entry);
        else
            look_up(entry);
        return {};
    case NormalMapEntry::Stored:
        read_stored(entry);
        return {};
    case NormalMapEntry::Derived:
    {
        fx::RefreshReliefDepth(entry->map, entry->relief);
        fx::DropTopMips(entry->map, max_size);
        entry->handle = lighting.CreateNormalMap(entry->map); // logs why when it cannot
        entry->bytes = entry->map.rg.size();
        entry->width = entry->map.width;
        entry->height_texels = entry->map.height;
        entry->trust_coverage = entry->map.trust_coverage;
        entry->trust = std::move(entry->map.trust);
        entry->trust_width = fx::TrustThumbnailSize(entry->map.width);
        entry->trust_height = fx::TrustThumbnailSize(entry->map.height);
        if (material_ && !entry->map.material.empty())
        {
            const fx::NormalMap material{entry->map.width, entry->map.height, entry->map.mip_count, entry->map.format,
                                         std::move(entry->map.material)};
            entry->material = lighting.CreateNormalMap(material); // logs why when it cannot
            entry->depth = entry->map.depth;
            entry->material_bytes = bgfx::isValid(entry->material) ? material.rg.size() : 0;
            entry->bytes += entry->material_bytes;
        }
        else if (material_ && fx::ReliefHasHeight(entry->relief) && !entry->no_height_reported)
        {
            // Flat (the preparation logs "flat, no height"): this texture has no parallax, cavity or gloss.
            entry->no_height_reported = true;
            shim::logf("effects: relief %s (%s): its map has no material texture: no parallax, cavity or gloss for it",
                       entry->name.c_str(), fx::ReliefClassName(entry->relief));
        }
        entry->map = {};
        entry->state = bgfx::isValid(entry->handle) ? NormalMapEntry::Uploaded : NormalMapEntry::Failed;
        entry->uploaded = now_;
        if (entry->cache_task) entry->cache_task->bytes.reset();
        break;
    }
    default:
        break;
    }
    if (entry->state == NormalMapEntry::Failed && !entry->failure_reported)
    {
        entry->failure_reported = true;
        if (entry->gloss != fx::GlossClass::None)
            shim::logf("ERROR: effects: no gloss map for texture %s (gloss %s): derivation, reading or upload failed",
                       entry->name.c_str(), fx::GlossClassName(entry->gloss));
        else
            shim::logf("ERROR: effects: no normal map for texture %s (relief %s): derivation, reading or upload failed",
                       entry->name.c_str(), fx::ReliefClassName(entry->relief));
    }
    if (entry->state != NormalMapEntry::Uploaded)
        return {};
    const float fade = std::chrono::duration<float>(now_ - entry->uploaded).count() /
                       std::chrono::duration<float>(kReliefFade).count();
    return {entry->handle,   std::clamp(fade, 0.0f, 1.0f), entry->material,          entry->depth,
            parallax_allowed(*entry), entry->width,          entry->height_texels};
}

void NormalMaps::configure_gloss(bool enabled, int max_size)
{
    const bool readers = enabled != gloss_enabled_;
    const bool changed = enabled != gloss_enabled_ || max_size != gloss_max_size_;
    gloss_enabled_ = enabled;
    gloss_max_size_ = max_size;
    ReliefGenerator::instance().set_wanted(enabled_, gloss_enabled_);
    if (readers)
        forget_drawn("the draws that read the gloss changed");
    if (!changed)
        return;
    std::erase_if(gloss_entries_, [](const std::weak_ptr<NormalMapEntry> &entry) { return entry.expired(); });
    for (const std::weak_ptr<NormalMapEntry> &weak : gloss_entries_)
        if (const std::shared_ptr<NormalMapEntry> entry = weak.lock())
            release(*entry); // read back (or looked up) when a draw reads it again
}

void NormalMaps::add_gloss(shim::TexStore &store, const std::string &name, std::span<const uint8_t> probe, uint32_t pw,
                           uint32_t ph, const std::filesystem::path &file)
{
    const fx::GlossVerdict verdict = fx::ClassifyGlossName(name);
    if (std::find(gloss_logged_.begin(), gloss_logged_.end(), name) == gloss_logged_.end())
    {
        gloss_logged_.push_back(name);
        if (verdict.rule)
            shim::logd("effects: texture %s: gloss %s (rule %s)", name.c_str(), fx::GlossClassName(verdict.gloss), verdict.rule);
        else
            shim::logd("effects: texture %s: no gloss map (no rule knows it)", name.c_str());
    }
    if (verdict.gloss == fx::GlossClass::None || directory().empty())
        return; // no directory: reported once by the relief
    auto entry = std::make_shared<NormalMapEntry>();
    entry->name = name;
    entry->gloss = verdict.gloss;
    entry->file = file;
    entry->key = fx::ReliefKey(name, probe, static_cast<int>(pw), static_cast<int>(ph), fx::GlossMethod(verdict.gloss), fx::kGlossMapSize);
    store.gloss_map = entry;
    gloss_entries_.push_back(entry); // looked up when a draw reads it (upload)
}

void NormalMaps::look_up_gloss(const std::shared_ptr<NormalMapEntry> &entry)
{
    entry->state = NormalMapEntry::Pending;
    entry->cache_task = std::make_shared<texture::CacheTask>();
    if (!texture::Service::instance().post_cache(entry->cache_task,
        [entry, relief_dir = directory(), tally = tally_] {
        if (fx::HasGlossMap(relief_dir, entry->key))
        {
            ++tally->gloss_stored;
            entry->state = NormalMapEntry::Stored;
            return;
        }
        // Made by PBRify's roughness network with the texture's relief, in the generator's queue.
        shim::logf("effects: gloss %s (%s): no map %016llx yet: drawn with its gloss, requested", entry->name.c_str(),
                   fx::GlossClassName(entry->gloss), static_cast<unsigned long long>(entry->key));
        ++tally->gloss_requested;
        entry->state = NormalMapEntry::Waiting;
        ReliefGenerator::instance().request(entry, entry->file);
    })) entry->state = NormalMapEntry::Unqueued;
}

NormalMaps::Stats NormalMaps::stats()
{
    Stats stats;
    std::erase_if(entries_, [](const std::weak_ptr<NormalMapEntry> &entry) { return entry.expired(); });
    for (const std::weak_ptr<NormalMapEntry> &weak : entries_)
        if (const std::shared_ptr<NormalMapEntry> entry = weak.lock())
        {
            if (entry->state == NormalMapEntry::Uploaded)
            {
                ++stats.resident;
                stats.bytes += entry->bytes;
                if (bgfx::isValid(entry->material))
                {
                    ++stats.materials;
                    stats.material_bytes += entry->material_bytes;
                }
            }
            stats.unqueued += entry->state == NormalMapEntry::Unqueued ? 1 : 0;
            stats.waiting += entry->state == NormalMapEntry::Waiting ? 1 : 0;
            stats.missing += entry->state == NormalMapEntry::Missing ? 1 : 0;
        }
    std::erase_if(gloss_entries_, [](const std::weak_ptr<NormalMapEntry> &entry) { return entry.expired(); });
    for (const std::weak_ptr<NormalMapEntry> &weak : gloss_entries_)
        if (const std::shared_ptr<NormalMapEntry> entry = weak.lock())
        {
            if (entry->state == NormalMapEntry::Uploaded)
            {
                ++stats.gloss_resident;
                stats.gloss_bytes += entry->bytes;
            }
            stats.gloss_unqueued += entry->state == NormalMapEntry::Unqueued ? 1 : 0;
            stats.gloss_waiting += entry->state == NormalMapEntry::Waiting ? 1 : 0;
            stats.gloss_missing += entry->state == NormalMapEntry::Missing ? 1 : 0;
        }
    return stats;
}

void NormalMaps::end_frame()
{
    ++frame_;
    now_ = std::chrono::steady_clock::now();
    if (++frames_ < 300)
        return;
    frames_ = 0;
    const Stats now = stats();
    const ReliefGenerator::Stats generator = ReliefGenerator::instance().stats();
    const bool gloss_same = now.gloss_resident == logged_.gloss_resident && now.gloss_bytes == logged_.gloss_bytes &&
                            now.gloss_unqueued == logged_.gloss_unqueued && now.gloss_waiting == logged_.gloss_waiting &&
                            now.gloss_missing == logged_.gloss_missing;
    if (gloss_same && now.resident == logged_.resident && now.materials == logged_.materials && now.bytes == logged_.bytes && now.waiting == logged_.waiting &&
        now.missing == logged_.missing && now.unqueued == logged_.unqueued &&
        generator.made == logged_made_ && generator.device == logged_device_)
        return;
    logged_ = now;
    logged_made_ = generator.made;
    logged_device_ = generator.device;
    shim::logf("effects relief: %u normal maps resident (%.1f MB, max %d px; %u with a material texture (height, gloss), %.1f MB of "
               "it); %u loaded textures waiting for their map%s; %u maps made (%.0f ms, slowest %.0f ms); %u without "
               "a map (generation unavailable); %u not looked up (no draw read them); models on %s",
               now.resident, static_cast<double>(now.bytes) / 1048576.0, max_size_, now.materials,
               static_cast<double>(now.material_bytes) / 1048576.0, now.waiting,
               generator.paused.empty() ? "" : (" (paused: " + generator.paused + ")").c_str(), generator.made, generator.made_ms,
               generator.slowest_ms, now.missing, now.unqueued,
               generator.device.empty() ? "(not loaded)" : generator.device.c_str());
    if (!gloss_same)
        shim::logf("effects gloss: %u gloss maps resident (%.1f MB, max %d px); %u loaded textures waiting for their map; "
                   "%u without a map (generation unavailable); %u not looked up (no draw read them)",
                   now.gloss_resident, static_cast<double>(now.gloss_bytes) / 1048576.0, gloss_max_size_, now.gloss_waiting,
                   now.gloss_missing, now.gloss_unqueued);
}

} // namespace storm::effects
