#include "tx_file.hpp"

#include "formats.hpp"
#include "storm/fx/relief_store.hpp"

#include <algorithm>
#include <array>
#include <fstream>

namespace storm::relief
{
namespace
{
constexpr uint32_t kTxFlagPalette = 1, kTxFlagCubemap = 2;
constexpr uint64_t kHeaderBytes = 24;

// Level `level`'s offset after the header: each level a quarter of the one above (the renderer's layout).
uint64_t Offset(uint32_t top_bytes, int level)
{
    uint64_t offset = 0;
    for (int l = 0; l < level; ++l)
        offset += uint64_t(top_bytes) >> (2 * l);
    return offset;
}
} // namespace

bool TxFile::Fail(const char *reason, bool unreadable_level)
{
    selected_ = {};
    probe_ = {};
    error_ = reason;
    unreadable = unreadable_level;
    return false;
}

bool TxFile::Load(const std::filesystem::path &path, const Choose &choose)
{
    selected_ = {};
    probe_ = {};
    error_.clear();
    unsupported_cube = unreadable = false;
    flags = width = height = levels = format = top_bytes = 0;
    first = 0;
    std::ifstream in(path, std::ios::binary);
    std::array<uint32_t, 6> header{};
    if (!in.read(reinterpret_cast<char *>(header.data()), sizeof(header)))
        return Fail("short header");
    flags = header[0];
    width = header[1];
    height = header[2];
    levels = header[3];
    format = header[4];
    top_bytes = header[5];
    if (!texture::tx_format_known(format) || width == 0 || height == 0 || width > 8192 || height > 8192 || levels == 0 ||
        top_bytes != texture::d3d_level_bytes(format, width, height))
        return Fail("invalid header");
    uint32_t chain = 1; // the levels down to 1x1
    for (uint32_t side = std::max(width, height); side > 1; side >>= 1)
        ++chain;
    if (levels > chain)
        return Fail("more levels than the dimensions have");
    in.seekg(0, std::ios::end);
    const std::streamoff end = in.tellg();
    if (end < 0)
        return Fail("file size unavailable", true);
    const uint64_t size = static_cast<uint64_t>(end);
    if (flags == kTxFlagCubemap)
    {
        // Stock Ammo/EnvAmmo is classified as metal, but its cube layout is intentionally unsupported. Distinguish that
        // from a damaged file only after validating all six faces' payload extent.
        if (width != height)
            return Fail("non-square cube map");
        uint64_t face = 0;
        for (uint32_t level = 0; level < levels; ++level)
            face += texture::d3d_level_bytes(format, std::max(1u, width >> level), std::max(1u, height >> level));
        unsupported_cube = size >= kHeaderBytes + 6 * face;
        return Fail(unsupported_cube ? "cube map" : "truncated cube map", !unsupported_cube);
    }
    if (flags & (kTxFlagPalette | kTxFlagCubemap))
        return Fail("palette or unknown flags");
    first = std::clamp(choose(*this), 0, static_cast<int>(levels) - 1);
    const int probe = ProbeLevel(*this);
    const auto read = [&](Slice &slice, int level) {
        const uint64_t count = uint64_t(top_bytes) >> (2 * level), offset = kHeaderBytes + Offset(top_bytes, level);
        if (count == 0 || offset > size || count > size - offset)
            return false;
        slice.bytes.resize(static_cast<size_t>(count));
        in.clear();
        in.seekg(static_cast<std::streamoff>(offset));
        if (!in.read(reinterpret_cast<char *>(slice.bytes.data()), static_cast<std::streamsize>(count)))
            return false;
        slice.level = level;
        return true;
    };
    if (!read(selected_, first))
        return Fail("truncated level", true);
    if (probe > first && !read(probe_, probe))
        return Fail("truncated probe level", true);
    return true;
}

bool TxFile::LoadProbe(const std::filesystem::path &path)
{
    return Load(path, [](const TxFile &tx) { return std::max(ProbeLevel(tx), 0); });
}

int TxFile::ProbeLevel(const TxFile &tx)
{
    return fx::ReliefProbeLevel(static_cast<int>(tx.width), static_cast<int>(tx.height), static_cast<int>(tx.levels));
}

std::span<const uint8_t> TxFile::Level(int level) const
{
    if (level == selected_.level)
        return selected_.bytes;
    if (level == probe_.level)
        return probe_.bytes;
    return {};
}
} // namespace storm::relief
