#include "game_pictures.hpp"

#include "renderer_impl.h"
#include "core.h"
#include "save_thumbnail.hpp"
#include "v_file_service.h"

#include <storm/render/d3d_interop.hpp>

#include <string>
#include <cstring>
#include <memory>

namespace storm
{

bool GamePictures::Load(std::string_view path, Loaded &out)
{
    int32_t id = -1;
    if (path.starts_with(save::thumbnail::kPrefix))
    {
        const std::string record = save::thumbnail::Path(path);
        const auto written = save::thumbnail::Written(path);
        if (record.empty() || !written || !fio->_FileOrDirectoryExists(record.c_str()) ||
            static_cast<uint64_t>(fio->_GetLastWriteTime(record.c_str()).time_since_epoch().count()) != *written) return false;
        int32_t size = 0;
        const std::unique_ptr<char[]> data(static_cast<char *>(core.GetSaveData(record.c_str(), size)));
        if (!data || size <= 0 || !fio->_FileOrDirectoryExists(record.c_str()) ||
            static_cast<uint64_t>(fio->_GetLastWriteTime(record.c_str()).time_since_epoch().count()) != *written) return false;
        const auto pixels = save::thumbnail::Pixels({reinterpret_cast<const uint8_t *>(data.get()), static_cast<size_t>(size)});
        if (pixels.empty()) return false;
        id = renderer_.TextureCreate(save::thumbnail::kWidth, save::thumbnail::kHeight, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED);
        D3DLOCKED_RECT lock{};
        if (id < 0) return false;
        if (!renderer_.TextureLock(id, 0, lock, 0)) { renderer_.TextureRelease(id); return false; }
        const bool fits = lock.pBits && lock.Pitch >= static_cast<int32_t>(save::thumbnail::kWidth * 4);
        if (fits)
            for (uint32_t y = 0; y < save::thumbnail::kHeight; ++y)
                std::memcpy(static_cast<uint8_t *>(lock.pBits) + size_t(y) * lock.Pitch,
                            pixels.data() + size_t(y) * save::thumbnail::kWidth * 4, save::thumbnail::kWidth * 4);
        renderer_.TextureUnlock(id, 0);
        if (!fits) { renderer_.TextureRelease(id); return false; }
    }
    else
    {
        // Ordinary game pictures keep the existing RESOURCE/Textures .tx route.
        const std::string name = std::string(path) + ".tga";
        id = renderer_.TextureCreate(name.c_str());
    }
    if (id < 0)
        return false;
    uint16_t width = 0, height = 0;
    const uint16_t handle = render::BgfxTextureOf(renderer_.GetTextureFromID(id), &width, &height);
    if (handle == UINT16_MAX)
    {
        renderer_.TextureRelease(id);
        return false;
    }
    out.token = static_cast<uint32_t>(id);
    out.bgfx_texture = handle;
    out.width = width;
    out.height = height;
    return true;
}

void GamePictures::Unload(uint32_t token)
{
    renderer_.TextureRelease(static_cast<int32_t>(token));
}

} // namespace storm
