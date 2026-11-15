// Completed static-location lamp fields only. This cache is independent of texture/scene caches.
// Bump kVersion when lamp transport, falloff, occlusion or field filtering changes its output.
#pragma once

#include "storm/fx/bounce.hpp"

#include <bit>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <optional>

namespace storm::fx::bounce_cache
{
inline constexpr uint32_t kVersion = 1;
inline constexpr size_t kMaxBytes = 8u << 20;
inline constexpr unsigned kSlots = 4; // at most 32 MiB published + one bounded temporary, no memory LRU
using Key = std::array<uint64_t, 2>;

// Two independent content fingerprints, not pointers, object padding, scene names or layout counters.
// Counts and values are explicitly delimited; floats keep all their IEEE bits (no lossy quantisation).
class Hash
{
  public:
    void bytes(const void *data, size_t n)
    {
        const auto *p = static_cast<const uint8_t *>(data);
        for (size_t i = 0; i < n; ++i)
        {
            key_[0] = (key_[0] ^ p[i]) * 1099511628211ull;
            key_[1] = (key_[1] ^ p[i]) * 0x9e3779b185ebca87ull;
            key_[1] = std::rotl(key_[1], 13);
        }
    }
    void add(uint64_t v) { for (int i = 0; i < 8; ++i) { const uint8_t b = v >> (8 * i); bytes(&b, 1); } }
    void add(uint32_t v) { add(uint64_t(v)); }
    void add(int v) { add(uint32_t(v)); }
    void add(uint8_t v) { add(uint32_t(v)); }
    void add(float v) { add(std::bit_cast<uint32_t>(v)); }
    template<class T, size_t N> void add(const std::array<T, N> &v) { for (const auto &x : v) add(x); }
    template<class T> void add(std::span<const T> v) { add(uint64_t(v.size())); for (const auto &x : v) add(x); }
    template<class T> void add(const std::vector<T> &v) { add(std::span<const T>(v)); }
    Key key() const { return key_; }
  private:
    Key key_{14695981039346656037ull, 0x6a09e667f3bcc909ull};
};

// Hash the ACTUAL baked scene as well as every occluder input. In particular the older scene-cache key does
// not contain alpha cuts: neither that key nor its 32-bit layout is sufficient for a lamp-field lookup.
inline Key SceneKey(const BounceScene &s, std::span<const Vec3> positions, std::span<const uint32_t> indices,
                    std::span<const std::array<float, 2>> uvs, std::span<const Occluders::Cut> cuts)
{
    Hash h;
    h.add(kVersion); h.add(kBounceBakeVersion);
    // Do not exchange floating-point results between different native arithmetic environments.
#if defined(__aarch64__)
    h.add(uint32_t(1));
#elif defined(__x86_64__)
    h.add(uint32_t(2));
#else
    h.add(uint32_t(3));
#endif
#if defined(__ANDROID__)
    h.add(uint32_t(1));
#else
    h.add(uint32_t(0));
#endif
    h.add(s.grid.size); h.add(s.grid.origin); h.add(s.grid.cell); h.add(s.stride); h.add(s.points); h.add(s.rays);
    h.add(s.position); h.add(s.normal); h.add(s.albedo); h.add(s.hit); h.add(s.inside); h.add(s.link);
    h.add(positions); h.add(indices); h.add(uvs);
    h.add(uint64_t(cuts.size()));
    std::map<const AlphaMask *, Key> masks; // hash each shared mask's pixels once, not once per triangle
    for (const auto &cut : cuts)
    {
        h.add(cut.reference); h.add(uint32_t(bool(cut.mask)));
        if (cut.mask)
        {
            auto [it, inserted] = masks.try_emplace(cut.mask.get());
            if (inserted)
            {
                Hash m; m.add(cut.mask->width); m.add(cut.mask->height); m.add(cut.mask->alpha);
                it->second = m.key();
            }
            h.add(it->second);
        }
    }
    return h.key();
}

inline std::optional<Key> LampKey(Key scene, std::span<const Lamp> lamps, std::span<const float> holders,
                                  std::span<const std::vector<uint32_t>> fixtures)
{
    Hash h; h.add(kVersion); h.add(scene); h.add(uint64_t(lamps.size()));
    for (const auto &lamp : lamps)
    {
        h.add(lamp.position); h.add(lamp.color); h.add(lamp.range); h.add(lamp.intensity); h.add(lamp.core);
        const auto *g = lamp.aggregate.get();
        if (lamp.aggregate.snapshot && (!g || !lamp.aggregate.snapshot->valid() || g->moving)) return {};
        h.add(uint32_t(bool(g)));
        if (g)
        {
            h.add(g->center); h.add(g->support); h.add(g->shape.range); h.add(g->shape.intensity); h.add(g->shape.core);
            h.add(g->count);
            for (uint32_t i = 0; i < g->count; ++i)
            {
                const auto &m = lamp.aggregate.snapshot->members[g->first + i];
                h.add(m.position); h.add(m.color);
            }
        }
    }
    h.add(holders); h.add(fixtures);
    return h.key();
}

// Separate from BakeTicket: invalidate on scene/quality replacement, including while a completed field is
// being written. Only the short rename shares this mutex with the main thread, never serialization or IO.
struct Publication
{
    std::mutex mutex;
    bool current = true;
    void cancel() { std::lock_guard lock(mutex); current = false; }
};

inline void Put(std::vector<uint8_t> &out, uint64_t v, unsigned n)
{
    for (unsigned i = 0; i < n; ++i) out.push_back(uint8_t(v >> (8 * i)));
}
inline uint64_t Get(std::span<const uint8_t> in, size_t &at, unsigned n)
{
    uint64_t v = 0;
    for (unsigned i = 0; i < n; ++i) v |= uint64_t(in[at++]) << (8 * i);
    return v;
}
inline std::optional<size_t> Moments(const std::array<int, 3> &points)
{
    size_t n = 12;
    for (int p : points)
    {
        if (p <= 0 || size_t(p) > kMaxBytes / sizeof(float) / n) return {};
        n *= size_t(p);
    }
    return n;
}
inline constexpr size_t kHeader = 80;
inline std::vector<uint8_t> Encode(Key key, const BounceField &field)
{
    const auto n = Moments(field.points);
    if (!n || *n != field.moments.size() || *n > (kMaxBytes - kHeader) / 4 ||
        !(field.spacing > 0) || !std::isfinite(field.spacing)) return {};
    Hash checksum;
    for (float x : field.origin) if (!std::isfinite(x)) return {};
    for (float x : field.moments) { if (!std::isfinite(x)) return {}; checksum.add(x); }
    std::vector<uint8_t> bytes; bytes.reserve(kHeader + *n * 4);
    Put(bytes, 0x31464c50434e4253ull, 8); // SBNCP LF1, dedicated namespace/format
    Put(bytes, kVersion, 4); Put(bytes, kHeader, 4);
    for (auto x : key) Put(bytes, x, 8);
    for (int x : field.points) Put(bytes, uint32_t(x), 4);
    for (float x : field.origin) Put(bytes, std::bit_cast<uint32_t>(x), 4);
    Put(bytes, std::bit_cast<uint32_t>(field.spacing), 4); Put(bytes, *n, 4);
    for (auto x : checksum.key()) Put(bytes, x, 8);
    for (float x : field.moments) Put(bytes, std::bit_cast<uint32_t>(x), 4);
    return bytes;
}
inline std::optional<BounceField> Decode(std::span<const uint8_t> bytes, Key key, const BounceScene &scene)
{
    if (bytes.size() < kHeader || bytes.size() > kMaxBytes) return {};
    size_t at = 0;
    if (Get(bytes, at, 8) != 0x31464c50434e4253ull || Get(bytes, at, 4) != kVersion || Get(bytes, at, 4) != kHeader) return {};
    for (auto x : key) if (Get(bytes, at, 8) != x) return {};
    BounceField field;
    for (int &x : field.points) x = int(uint32_t(Get(bytes, at, 4)));
    for (float &x : field.origin) x = std::bit_cast<float>(uint32_t(Get(bytes, at, 4)));
    field.spacing = std::bit_cast<float>(uint32_t(Get(bytes, at, 4)));
    const size_t count = Get(bytes, at, 4);
    Key checksum; for (auto &x : checksum) x = Get(bytes, at, 8);
    const auto expected = Moments(field.points);
    Vec3 expected_origin;
    for (int k = 0; k < 3; ++k) expected_origin[k] = scene.grid.origin[k] + 0.5f * scene.grid.cell;
    if (!expected || count != *expected || count > (kMaxBytes - kHeader) / 4 || bytes.size() != kHeader + count * 4 ||
        field.points != scene.points || field.origin != expected_origin ||
        !(field.spacing > 0) || field.spacing != scene.grid.cell * scene.stride) return {};
    for (float x : field.origin) if (!std::isfinite(x)) return {};
    field.moments.resize(count);
    Hash h;
    for (float &x : field.moments)
    {
        x = std::bit_cast<float>(uint32_t(Get(bytes, at, 4)));
        if (!std::isfinite(x)) return {};
        h.add(x);
    }
    if (checksum != h.key()) return {};
    return field;
}

inline std::mutex store_mutex;
inline std::filesystem::path Slot(const std::filesystem::path &directory, Key key)
{
    return directory / "bounce-lamps-v1" / (std::to_string(key[0] % kSlots) + ".field");
}
inline std::optional<BounceField> Read(const std::filesystem::path &directory, Key key, const BounceScene &scene)
{
    if (directory.empty()) return {};
    std::lock_guard lock(store_mutex);
    const auto path = Slot(directory, key);
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec || size < kHeader || size > kMaxBytes) return {};
    std::vector<uint8_t> bytes(size);
    std::ifstream file(path, std::ios::binary);
    if (!file.read(reinterpret_cast<char *>(bytes.data()), std::streamsize(size))) return {};
    return Decode(bytes, key, scene); // damaged, wrong-key and incompatible files are simply misses
}
inline bool Write(const std::filesystem::path &directory, Key key, const BounceField &field, Publication &publication,
                  const std::function<void()> &checkpoint = {})
{
    if (directory.empty()) return false;
    auto bytes = Encode(key, field);
    if (bytes.empty()) return false;
    std::lock_guard lock(store_mutex); // one bounded temporary, including concurrent cache writers
    const auto path = Slot(directory, key);
    const auto temp = path.parent_path() / "pending.tmp";
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) return false;
    struct RemoveTemporary
    {
        const std::filesystem::path &path;
        ~RemoveTemporary() { std::error_code ignored; std::filesystem::remove(path, ignored); }
    } cleanup{temp}; // also removes it if a BakeTicket cancellation checkpoint throws
    {
        std::ofstream file(temp, std::ios::binary | std::ios::trunc);
        file.write(reinterpret_cast<const char *>(bytes.data()), std::streamsize(bytes.size()));
        file.close();
        if (!file) return false;
    }
    if (checkpoint) checkpoint(); // cancellation during serialization/IO must not publish
    bool written = false;
    {
        std::lock_guard current(publication.mutex);
        if (publication.current) { std::filesystem::rename(temp, path, ec); written = !ec; }
    }
    return written;
}
} // namespace storm::fx::bounce_cache
