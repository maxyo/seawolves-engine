// Host check of relief::TxFile (tx_file.hpp) on synthetic .tx files: the bytes of the chosen and the probe level at the
// renderer's layout, nothing past the last level read, and every kind of damaged file refused before it is allocated
// for (a lying header, too many levels, a truncated level or probe, a truncated cube map). Argument: a scratch directory.
#include "tx_file.hpp"

#include "relief.hpp"

#include <array>
#include <cstdio>
#include <fstream>

namespace
{
using storm::relief::TxFile;

int failures = 0;
#define CHECK(condition)                                                                                               \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(condition))                                                                                              \
        {                                                                                                              \
            std::printf("FAILED %s:%d: %s\n", __FILE__, __LINE__, #condition);                                        \
            ++failures;                                                                                                \
        }                                                                                                              \
    } while (false)

constexpr uint32_t kArgb = 21, kDxt1 = 0x31545844;

uint32_t TopBytes(uint32_t format, uint32_t w, uint32_t h)
{
    return format == kDxt1 ? (w + 3) / 4 * ((h + 3) / 4) * 8 : w * h * 4;
}

// A header and `payload` bytes after it, byte i of the payload being i % 251 (so every level's bytes are known).
void Write(const std::filesystem::path &path, std::array<uint32_t, 6> header, size_t payload)
{
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char *>(header.data()), sizeof(header));
    for (size_t i = 0; i < payload; ++i)
        out.put(static_cast<char>(i % 251));
}

// Level `level` holds payload bytes [offset, offset + top >> 2 * level), offset the sum of the levels above.
bool Holds(std::span<const uint8_t> bytes, uint32_t top, int level)
{
    size_t offset = 0;
    for (int l = 0; l < level; ++l)
        offset += size_t(top) >> (2 * l);
    if (bytes.size() != size_t(top) >> (2 * level))
        return false;
    for (size_t i = 0; i < bytes.size(); ++i)
        if (bytes[i] != (offset + i) % 251)
            return false;
    return true;
}
} // namespace

int main(int argc, char **argv)
{
    if (argc != 2)
    {
        std::printf("usage: storm_relief_tx_check <scratch directory>\n");
        return 2;
    }
    const std::filesystem::path dir = argv[1];
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    const auto file = dir / "input.tx";
    TxFile tx;

    // Valid files: the chosen level and the probe level (the first no larger than 64) at the renderer's layout; no other.
    for (uint32_t format : {kArgb, kDxt1})
        for (uint32_t side : {64u, 128u, 256u})
            for (int first : {0, 1, 2})
            {
                const uint32_t top = TopBytes(format, side, side);
                Write(file, {0, side, side, 4, format, top}, top + top / 4 + top / 16 + top / 64);
                CHECK(tx.Load(file, [first](const TxFile &) { return first; }) && tx.first == first);
                CHECK(Holds(tx.Level(first), top, first));
                const int probe = TxFile::ProbeLevel(tx);
                CHECK(probe >= 0);
                if (probe > first)
                    CHECK(Holds(tx.Level(probe), top, probe));
                for (int level = 0; level < 4; ++level)
                    if (level != first && level != probe)
                        CHECK(tx.Level(level).empty());
            }
    // A choice past the chain is clamped to the last level.
    Write(file, {0, 64, 64, 2, kArgb, 16384}, 16384 + 4096);
    CHECK(tx.Load(file, [](const TxFile &) { return 9; }) && tx.first == 1 && Holds(tx.Level(1), 16384, 1));

    // 4 MB after a 64x64 file's only level are never read: the probe alone is held.
    Write(file, {0, 64, 64, 1, kArgb, 16384}, 4 << 20);
    CHECK(tx.LoadProbe(file) && tx.Level(0).size() == 16384 && Holds(tx.Level(0), 16384, 0));

    // A lying header: refused, not "unreadable" (a file the relief does not read), and nothing of the last load kept.
    const auto refused = [&](std::array<uint32_t, 6> header, size_t payload) {
        Write(file, header, payload);
        return !tx.LoadProbe(file) && !tx.unreadable && !tx.unsupported_cube && tx.Level(0).empty() && !tx.Error().empty();
    };
    CHECK(refused({0, 64, 64, 0xffffffffu, kArgb, 16384}, 16384)); // levels beyond the dimensions
    CHECK(refused({0, 64, 64, 8, kArgb, 16384}, 1 << 16));         // 64 has 7 levels
    CHECK(refused({0, 8193, 64, 1, kArgb, 8193 * 64 * 4}, 0));     // too large
    CHECK(refused({0, 0, 64, 1, kArgb, 0}, 0));
    CHECK(refused({0, 64, 64, 1, kArgb, 16383}, 16384));            // top level size not the format's
    CHECK(refused({0, 64, 64, 1, 50, 16384}, 16384));               // unknown format
    CHECK(refused({1, 64, 64, 1, kArgb, 16384}, 16384));            // palette
    CHECK(refused({0, 64, 64, 0, kArgb, 16384}, 16384));            // no levels
    {
        std::ofstream(file, std::ios::binary).write("short", 5);
        CHECK(!tx.LoadProbe(file) && !tx.unreadable);
        CHECK(!tx.LoadProbe(dir / "missing.tx") && !tx.unreadable);
    }

    // A truncated level or probe: "unreadable" (a damaged texture, reported), nothing kept.
    Write(file, {0, 64, 64, 1, kArgb, 16384}, 16383);
    CHECK(!tx.LoadProbe(file) && tx.unreadable && tx.Level(0).empty());
    Write(file, {0, 128, 128, 2, kArgb, 65536}, 65536); // the 64x64 probe level is missing
    CHECK(!tx.Load(file, [](const TxFile &) { return 0; }) && tx.unreadable && tx.Level(0).empty() && tx.Level(1).empty());
    Write(file, {0, 128, 128, 2, kArgb, 65536}, 65536 + 16384);
    CHECK(tx.Load(file, [](const TxFile &) { return 0; }) && Holds(tx.Level(0), 65536, 0) && Holds(tx.Level(1), 65536, 1));

    // Cube maps: a complete one is a supported refusal; a truncated one is damaged.
    Write(file, {2, 4, 4, 1, kArgb, 64}, 384);
    CHECK(!tx.LoadProbe(file) && tx.unsupported_cube && !tx.unreadable);
    Write(file, {2, 4, 4, 1, kArgb, 64}, 383);
    CHECK(!tx.LoadProbe(file) && !tx.unsupported_cube && tx.unreadable);
    Write(file, {2, 8, 4, 1, kArgb, 128}, 1024);
    CHECK(!tx.LoadProbe(file) && !tx.unsupported_cube && !tx.unreadable);

    // ReadTextureLevel goes through the same reader: the level no larger than asked, damaged files refused.
    {
        Write(file, {0, 128, 128, 2, kArgb, 65536}, 65536 + 16384);
        std::vector<uint8_t> rgba;
        int w = 0, h = 0;
        CHECK(storm::relief::ReadTextureLevel(file, 64, rgba, w, h) && w == 64 && h == 64 && rgba.size() == 16384);
        Write(file, {0, 128, 128, 2, kArgb, 65536}, 65536 + 100);
        CHECK(!storm::relief::ReadTextureLevel(file, 64, rgba, w, h));
    }

    std::filesystem::remove_all(dir);
    if (failures)
        return 1;
    std::printf("PASS: relief TxFile: chosen and probe levels, data past the levels unread, damaged files refused\n");
    return 0;
}
