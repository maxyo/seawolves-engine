#include "tx_file.hpp"

#include "formats.hpp"
#include "tx_header.hpp"
#include "storm/fx/relief_store.hpp"

#include <algorithm>
#include <fstream>

namespace storm::relief
{

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
    texture::TxHeader header;
    if (!in.read(reinterpret_cast<char *>(&header), sizeof(header)))
        return Fail("short header");
    flags = header.flags;
    width = header.width;
    height = header.height;
    levels = header.levels;
    format = header.format;
    top_bytes = header.top_bytes;
    if (const char *problem = texture::tx_header_problem(header))
        return Fail(problem);
    in.seekg(0, std::ios::end);
    const std::streamoff end = in.tellg();
    if (end < 0)
        return Fail("file size unavailable", true);
    const uint64_t size = static_cast<uint64_t>(end);
    if (header.cube())
    {
        // Stock Ammo/EnvAmmo is classified as metal, but its cube layout is intentionally unsupported. Distinguish that
        // from a damaged file only after validating all six faces' payload extent (whole blocks for the small levels of
        // a compressed format).
        uint64_t face = 0;
        for (uint32_t level = 0; level < levels; ++level)
            face += texture::d3d_level_bytes(format, std::max(1u, width >> level), std::max(1u, height >> level));
        unsupported_cube = size >= texture::kTxHeaderBytes + 6 * face;
        return Fail(unsupported_cube ? "cube map" : "truncated cube map", !unsupported_cube);
    }
    first = std::clamp(choose(*this), 0, static_cast<int>(levels) - 1);
    const int probe = ProbeLevel(*this);
    const auto read = [&](Slice &slice, int level) {
        const uint64_t count = uint64_t(top_bytes) >> (2 * level), offset = texture::kTxHeaderBytes + texture::tx_level_offset(top_bytes, static_cast<uint32_t>(level));
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
