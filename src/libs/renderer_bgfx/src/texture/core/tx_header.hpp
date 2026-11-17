// The header of a game texture file (.tx) and its checks, shared by the renderer's texture loader and the relief
// preparation: nothing is allocated for a file before its header has been checked against its dimensions and size.
#pragma once

#include <cstdint>

namespace storm::texture
{
inline constexpr uint64_t kTxHeaderBytes = 24;
inline constexpr uint32_t kTxFlagPalette = 1, kTxFlagCubemap = 2;

struct TxHeader
{
    uint32_t flags = 0, width = 0, height = 0, levels = 0, format = 0, top_bytes = 0;
    bool cube() const { return (flags & kTxFlagCubemap) != 0; }
};
static_assert(sizeof(TxHeader) == kTxHeaderBytes);

// Why the header cannot describe a texture, or nullptr: an unknown format, a side of 0 or above 8192, no levels or more
// than the sides have, a level-0 size other than its format's, a palette, a cube map with unequal sides.
const char *tx_header_problem(const TxHeader &header);
// A level's offset from the first level in the renderer's layout: each level a quarter of the one above.
uint64_t tx_level_offset(uint32_t top_bytes, uint32_t level);
// The bytes after the header the renderer reads from `first_level` on: those levels of every face (six for a cube map).
uint64_t tx_payload_bytes(const TxHeader &header, uint32_t first_level = 0);
} // namespace storm::texture
