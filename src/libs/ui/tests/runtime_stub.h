// The whole runtime without a GL context, shared by the runtime checks: a renderer that
// draws nothing and file helpers for the synthetic packs they derive from tests/pack.
#pragma once

#include "storm_ui/render_backend.h"

#include <RmlUi/Core/RenderInterface.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

class NoRenderer final : public Rml::RenderInterface
{
  public:
    Rml::CompiledGeometryHandle CompileGeometry(Rml::Span<const Rml::Vertex>, Rml::Span<const int>) override
    {
        return 1;
    }
    void RenderGeometry(Rml::CompiledGeometryHandle, Rml::Vector2f, Rml::TextureHandle) override
    {
    }
    void ReleaseGeometry(Rml::CompiledGeometryHandle) override
    {
    }
    // Every pack image "loads" as a pixel: what is drawn does not matter, only that it exists.
    Rml::TextureHandle LoadTexture(Rml::Vector2i &dimensions, const Rml::String &) override
    {
        dimensions = {1, 1};
        return 1;
    }
    // Counts the textures holding the runtime's danger red (the crossed "missing icon" marker).
    Rml::TextureHandle GenerateTexture(Rml::Span<const Rml::byte> data, Rml::Vector2i) override
    {
        for (size_t i = 0; i + 3 < data.size(); i += 4)
            if (data[i] == 0xc4 && data[i + 1] == 0x47 && data[i + 2] == 0x3a && data[i + 3] == 0xff)
            {
                ++marker_textures;
                break;
            }
        return 1;
    }
    void ReleaseTexture(Rml::TextureHandle) override
    {
    }
    void EnableScissorRegion(bool) override
    {
    }
    void SetScissorRegion(Rml::Rectanglei) override
    {
    }
    // Layers, filters and shaders as every backend has them (RmlUi's box shadows and the
    // runtime's filtered-layer cache need a saved layer): handles that are never drawn.
    Rml::LayerHandle PushLayer() override
    {
        return ++layers_;
    }
    void PopLayer() override
    {
        --layers_;
    }
    Rml::TextureHandle SaveLayerAsTexture() override
    {
        return 1;
    }
    Rml::CompiledFilterHandle SaveLayerAsMaskImage() override
    {
        return 1;
    }
    Rml::CompiledFilterHandle CompileFilter(const Rml::String &, const Rml::Dictionary &) override
    {
        return 1;
    }
    Rml::CompiledShaderHandle CompileShader(const Rml::String &, const Rml::Dictionary &) override
    {
        return 1;
    }

    int marker_textures = 0;

  private:
    Rml::LayerHandle layers_ = 0;
};

class NoBackend final : public storm::ui::RenderBackend
{
  public:
    Rml::RenderInterface &Interface() override
    {
        return renderer_;
    }
    void SetViewport(int, int) override
    {
    }
    void BeginFrame(bool) override
    {
    }
    void EndFrame() override
    {
    }
    PictureState AcquireGamePicture(const std::string &) override
    {
        return PictureState::Unavailable; // no game files in the checks
    }
    NoRenderer &Renderer()
    {
        return renderer_;
    }

  private:
    NoRenderer renderer_;
};

inline std::string Read(const std::filesystem::path &path)
{
    std::ifstream file(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

inline void Write(const std::filesystem::path &path, const std::string &text)
{
    std::ofstream(path, std::ios::binary) << text;
}

