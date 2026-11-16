#pragma once

#include "relief.hpp"
#include "storm/fx/gloss_map.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>

namespace storm::texture { class EacEncoder; }
namespace storm::relief
{
// Internal owned stage payload. Mutable only before dispatch; move, never borrow Session/model storage.
struct ComputedTexture
{
    enum class Kind { Relief, Upscale } kind = Kind::Relief;
    uint64_t session = 0;
    std::shared_ptr<std::atomic<bool>> cancelled;
    std::filesystem::path output;
    std::string name, provider, per_model;
    std::optional<uint64_t> key, gloss_key;
    fx::ReliefClass relief = fx::ReliefClass::None;
    fx::GlossClass gloss_class = fx::GlossClass::None;
    fx::UpscaleTarget target = fx::UpscaleTarget::None;
    UpscaleOutput display;
    int width = 0, height = 0, model_scale = 1;
    int mw = 0, mh = 0, fw = 0, fh = 0, gw = 0, gh = 0;
    bool height_class = false, gloss_wanted = false;
    unsigned threads = 1;
    std::vector<uint8_t> rgba;
    std::vector<float> normal_planes, height_planes, gloss_height, roughness_planes;
    std::chrono::steady_clock::time_point started, computed_at;
    double read_ms = 0, decode_ms = 0, upscale_ms = 0, model_ms = 0, infer_ms = 0;
    size_t owned_bytes() const {
        return rgba.capacity() + sizeof(float) * (normal_planes.capacity() + height_planes.capacity() +
            gloss_height.capacity() + roughness_planes.capacity());
    }
    size_t staging_bytes() const {
        // Owning vectors + conservative CPU scratch. Network activations and renderer copies are separate;
        // this is not a process RSS bound. Oversized jobs retain quality and drain before synchronous progress.
        const auto area = [](int w, int h) { return size_t(std::max(w, 0)) * size_t(std::max(h, 0)); };
        const size_t scratch = kind == Kind::Upscale ? 32 * area(width * 2, height * 2) :
            256 * std::max(area(fw, fh), area(gw, gh));
        return owned_bytes() + scratch;
    }
    ComputedTexture() = default;
    ComputedTexture(const ComputedTexture &) = delete;
    ComputedTexture &operator=(const ComputedTexture &) = delete;
};

struct ComputeResult
{
    Outcome outcome = Outcome::Failed; // meaningful only when product is null
    std::unique_ptr<ComputedTexture> product;
    ComputeResult(Outcome value) : outcome(value) {}
    ComputeResult(std::unique_ptr<ComputedTexture> value) : product(std::move(value)) {}
};

struct FinalizeResult
{
    Outcome outcome = Outcome::Failed;
    uint64_t session = 0;
    bool upscale = false;
    std::optional<uint64_t> relief_written, gloss_written, upscale_written;
    std::string failure, provider;
    double total_ms = 0, finalize_ms = 0;
};

// One caller at a time. Its encoder/scratch belong to this owner, never to Session.
class Finalizer
{
  public:
    Finalizer();
    ~Finalizer();
    Finalizer(const Finalizer &) = delete;
    Finalizer &operator=(const Finalizer &) = delete;
    FinalizeResult Run(std::unique_ptr<ComputedTexture> product);
  private:
    unsigned encoder_threads_ = 0;
    std::unique_ptr<texture::EacEncoder> encoder_;
};
} // namespace storm::relief
