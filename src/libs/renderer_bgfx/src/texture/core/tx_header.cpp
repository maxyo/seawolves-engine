#include "tx_header.hpp"

#include "formats.hpp"

#include <algorithm>

namespace storm::texture
{
const char *tx_header_problem(const TxHeader &header)
{
    if (!tx_format_known(header.format) || header.width == 0 || header.height == 0 || header.width > 8192 ||
        header.height > 8192 || header.levels == 0 ||
        header.top_bytes != d3d_level_bytes(header.format, header.width, header.height))
        return "invalid header";
    uint32_t chain = 1; // the levels down to 1x1
    for (uint32_t side = std::max(header.width, header.height); side > 1; side >>= 1)
        ++chain;
    if (header.levels > chain)
        return "more levels than the dimensions have";
    if (header.flags & kTxFlagPalette)
        return "palette";
    if (header.cube() && header.width != header.height)
        return "non-square cube map";
    return nullptr;
}

uint64_t tx_level_offset(uint32_t top_bytes, uint32_t level)
{
    uint64_t offset = 0;
    for (uint32_t l = 0; l < level && l < 32; ++l)
        offset += uint64_t(top_bytes) >> (2 * l);
    return offset;
}

uint64_t tx_payload_bytes(const TxHeader &header, uint32_t first_level)
{
    if (first_level >= header.levels)
        return 0;
    const uint64_t face = tx_level_offset(header.top_bytes, header.levels) - tx_level_offset(header.top_bytes, first_level);
    return header.cube() ? 6 * face : face;
}
} // namespace storm::texture
