// A game texture file (.tx) as the relief preparation reads it: the header, validated against the file's real size,
// and only the levels it asks for. A damaged file (a header that lies, a truncated level, a mip count beyond its
// dimensions) is refused with a reason before anything is allocated for it; data past the last level is never read.
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <vector>

namespace storm::relief
{
class TxFile
{
  public:
    uint32_t flags = 0, width = 0, height = 0, levels = 0, format = 0, top_bytes = 0;
    bool unsupported_cube = false; // a complete, structurally valid cube map: no relief key, not a read failure
    bool unreadable = false;       // the header is valid but a wanted level is not in the file (truncated, I/O error)
    int first = 0;                 // the level `choose` picked

    using Choose = std::function<int(const TxFile &)>;
    // The header, then the level `choose` picks (given the header) and the probe level when it lies below that one.
    // Levels are laid out as the renderer reads them: each a quarter of the one above.
    bool Load(const std::filesystem::path &path, const Choose &choose);
    // The probe level alone (the keys, the grey test).
    bool LoadProbe(const std::filesystem::path &path);
    static int ProbeLevel(const TxFile &tx);
    // The bytes of a loaded level: `first` or the probe level; empty for any other.
    std::span<const uint8_t> Level(int level) const;
    // Why the last Load failed.
    const std::string &Error() const { return error_; }

  private:
    struct Slice
    {
        int level = -1;
        std::vector<uint8_t> bytes;
    };
    Slice selected_, probe_;
    std::string error_;
    bool Fail(const char *reason, bool unreadable_level = false);
};
} // namespace storm::relief
