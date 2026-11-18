#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

// A square byte map (island shadow/depth) stored as 8x8 blocks: a block of one value is kept in its word-table entry
// (0x8000 | value), any other block as an index into the real data.
class MapZipper
{
  private:
    uint32_t dwSizeX = 0;
    uint32_t dwDX = 0;
    uint32_t dwBlockSize = 0, dwBlockShift = 0;
    uint32_t dwShiftNumBlocksX = 0;
    uint32_t dwNumRealBlocks = 0;

    std::vector<uint16_t> wordTable;
    std::vector<uint8_t> realData;

    bool Read(std::fstream &fileS, const std::string &name);

  public:
    uint32_t GetSizeX() const
    {
        return dwSizeX;
    }

    void UnInit();
    void DoZip(const uint8_t *pSrc, uint32_t dwSizeX);
    uint8_t Get(uint32_t dwX, uint32_t dwY) const;

    // Save never touches the game folder: it writes to a path of the cache (ISLAND::ZapCache).
    bool Save(const std::filesystem::path &file);
    // A game file (through the file service) or a path of the cache. A missing file returns false; a short or
    // inconsistent file returns false with an error logged and leaves the zipper empty.
    bool Load(std::string sFileName);
    bool Load(const std::filesystem::path &file);

    bool isLoaded() const
    {
        return !wordTable.empty();
    }
};
