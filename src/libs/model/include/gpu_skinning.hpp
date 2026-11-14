#pragma once
// GPU skinning of GEOS::AVERTEX0 sources (one blend weight, two bone indices).
//
// Each source becomes a static vertex buffer in the AVERTEX0 layout
// (D3DFVF_XYZB2 | D3DFVF_LASTBETA_UBYTE4 | normal | diffuse | uv), uploaded once per source
// lifetime; the pose is a float bone palette bound to D3DVERTEXTEXTURESAMPLER0. The renderer's
// skinned fixed-function vertex shader (renderer_bgfx/shaders/include/ff_vertex.sh, FF_SKIN)
// blends the bones exactly like the CPU kernel in src/libs/model/src/skinning.hpp, which stays for
// the caustic pass only (its vs_1_1 port has no skinned variant).
// Vertex and palette layout: skinning_palette.hpp.
#include "renderer.h"
#include "skinning_palette.hpp"
#include "vertex_source_lifetime.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace storm::model
{

class GpuSkinning
{
    struct Source final : render::VertexSourceLifetime::Dependent
    {
        VRenderer *rs = nullptr;
        int32_t vb = -1;
        void invalidateSource() noexcept override
        {
            if (vb >= 0)
                rs->ReleaseVertexBuffer(vb);
            vb = -1;
        }
        ~Source() override { invalidateSource(); }
    };

    VRenderer *rs_ = nullptr;
    int32_t palette_ = -1; // renderer texture id
    uint32_t paletteBones_ = 0;
    bool poseUploaded_ = false;
    std::unordered_map<const void *, std::shared_ptr<Source>> sources_;

  public:
    // AVERTEX0 copies in the layout above: the skin weight and the torso weight of each vertex, 0..255, from its two
    // bones and its blend weight (as the shader blends the bones). Missing/invalid bone classifications suppress
    // character relief through full skin weight; an unsupported skeleton must never make bare skin fail open.
    static void writeSkinWeights(uint8_t *vertices, uint32_t count, const std::vector<uint8_t> &skin)
    {
        for (uint32_t i = 0; i < count; ++i)
        {
            uint8_t *vertex = vertices + size_t(i) * kSkinnedSourceStride;
            float weight;
            std::memcpy(&weight, vertex + kSkinnedWeightOffset, sizeof(weight));
            const uint8_t *bones = vertex + kSkinnedBonesOffset;
            const auto flag = [&skin](uint8_t bone, uint8_t kind) {
                if (bone >= skin.size())
                    return kind == 1 ? 1.0f : 0.0f; // unknown anatomy: conservative skin, never unmasked clothing
                return skin[bone] == kind ? 1.0f : 0.0f;
            };
            const float w = std::clamp(weight, 0.0f, 1.0f);
            const float value = flag(bones[0], 1) * w + flag(bones[1], 1) * (1.0f - w);
            const float torso = flag(bones[0], 2) * w + flag(bones[1], 2) * (1.0f - w);
            vertex[kSkinnedBonesOffset + kSkinWeightByte] = static_cast<uint8_t>(value * 255.0f + 0.5f);
            vertex[kSkinnedBonesOffset + kTorsoWeightByte] = static_cast<uint8_t>(torso * 255.0f + 0.5f);
        }
    }

    explicit GpuSkinning(VRenderer *rs = nullptr) : rs_(rs) {}
    GpuSkinning(const GpuSkinning &) = delete;
    GpuSkinning &operator=(const GpuSkinning &) = delete;
    ~GpuSkinning() { clear(); }

    void setRenderer(VRenderer *rs) { rs_ = rs; }

    // Releases the vertex buffers and the palette (device lost, new animation, destruction).
    void clear()
    {
        sources_.clear();
        if (palette_ >= 0)
            rs_->TextureRelease(palette_);
        palette_ = -1;
        paletteBones_ = 0;
        poseUploaded_ = false;
    }

    // A new pose: the palette is uploaded again by the next bind().
    void beginPose()
    {
        poseUploaded_ = false;
        std::erase_if(sources_, [](const auto &item) { return item.second->vb < 0; });
    }

    // The static vertex buffer of a source, created and filled on first use. `skin`: per bone 1 where its skin shows
    // (Animation::GetSkinBones, empty: none); each vertex's blend of it goes into the third byte of its bone indices
    // (kSkinWeightByte) and its torso weight into the fourth (kTorsoWeightByte), which the CPU kernel never reads: the
    // characters' relief fades out by them (ff_fragment.sh; on the torso where the colour is the character's skin).
    int32_t vertexBuffer(const void *source, const std::shared_ptr<render::VertexSourceLifetime> &lifetime,
                         uint32_t count, const std::vector<uint8_t> &skin)
    {
        if (!source || !lifetime || !lifetime->valid() || !count)
            throw std::invalid_argument("Invalid skinning source");
        auto &slot = sources_[source];
        if (slot && slot->vb >= 0)
            return slot->vb;
        slot = std::make_shared<Source>();
        slot->rs = rs_;
        lifetime->observe(slot);
        const size_t bytes = size_t(count) * kSkinnedSourceStride;
        slot->vb = rs_->CreateVertexBuffer(kSkinnedSourceFvf, bytes, D3DUSAGE_WRITEONLY);
        auto *data = slot->vb >= 0 ? rs_->LockVertexBuffer(slot->vb) : nullptr;
        if (!data)
            throw std::runtime_error("Skinned source vertex buffer allocation failed");
        std::memcpy(data, source, bytes);
        writeSkinWeights(static_cast<uint8_t *>(data), count, skin);
        rs_->UnLockVertexBuffer(slot->vb);
        return slot->vb;
    }

    // Uploads the pose once per beginPose() and binds the palette for the skinned draws that follow.
    void bind(const glm::mat4 *bones, uint32_t count)
    {
        if (!count)
            throw std::invalid_argument("Skinned draw without bones");
        if (palette_ < 0 || paletteBones_ != count)
        {
            rs_->TextureRelease(palette_);
            palette_ = rs_->TextureCreate(kPaletteTexelsPerBone, count, 1, D3DUSAGE_DYNAMIC, D3DFMT_A32B32G32R32F,
                                          D3DPOOL_DEFAULT);
            if (palette_ < 0)
                throw std::runtime_error("Bone palette allocation failed");
            paletteBones_ = count;
            poseUploaded_ = false;
        }
        if (!poseUploaded_)
        {
            D3DLOCKED_RECT locked{};
            if (!rs_->TextureLock(palette_, 0, locked, D3DLOCK_DISCARD))
                throw std::runtime_error("Bone palette upload failed");
            for (uint32_t i = 0; i < count; ++i)
                writePaletteRow(reinterpret_cast<const float *>(&bones[i]),
                                reinterpret_cast<float *>(static_cast<uint8_t *>(locked.pBits) + size_t(i) * locked.Pitch));
            rs_->TextureUnlock(palette_, 0);
            poseUploaded_ = true;
        }
        rs_->TextureSet(D3DVERTEXTEXTURESAMPLER0, palette_);
    }

    void unbind() { rs_->TextureSet(D3DVERTEXTEXTURESAMPLER0, -1); }
};

} // namespace storm::model
