// The neural ×2 of a colour texture (fx/upscale.hpp): PBRify's 4x-PBRify_UpscalerSPANV4 (model/: pbrify-upscale.ncnn.*)
// on the device the relief's networks run on (backend.hpp), in equal tiles of at most fx::kUpscaleTile texels (and of what the
// network runs at once: Network::TileCeiling) padded by fx::kUpscalePad (wrapped around for a texture that
// tiles, mirrored for an atlas), its ×4 box-filtered to ×2 in linear light. The alpha, which the network does not see,
// is stretched bilinearly.
#pragma once

#include "backend.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace storm::relief
{

class Network; // backend.hpp
struct LoadingRetry;

class Upscaler
{
  public:
    // `network`: the 4x model on the CPU or the GPU (backend.hpp; a GPU network with fx::kUpscalePad as its halo).
    explicit Upscaler(std::unique_ptr<Network> network);
    ~Upscaler();
    // `rgba`: width x height RGBA8 (R first); `out`: 2 width x 2 height RGBA8. False on an inference error or Cancel.
    bool Run(std::span<const uint8_t> rgba, int width, int height, bool wrap, std::vector<uint8_t> &out, std::string &error);
    // Any thread: the running inference stops with an error, and every later one.
    void Cancel();
    void SetLoadingRetry(std::shared_ptr<LoadingRetry> retry); // worker only; underlying network owns the lease
    void SetYield(std::function<bool()> yield); // worker only: backend.hpp Network::SetYield
    void SetCpuAdmission(std::function<std::shared_ptr<void>()> admission); // worker only
    double infer_ms() const { return infer_ms_; } // inference time of the last Run

  private:
    std::unique_ptr<Network> network_;
    double infer_ms_ = 0.0;
};

} // namespace storm::relief
