#include "map_zipper.h"

#include "debug-trap.h"
#include "v_file_service.h"

#include <spdlog/spdlog.h>

namespace
{

uint32_t Number2Shift(uint32_t dwNumber)
{
    for (uint32_t i = 0; i < 31; i++)
        if (static_cast<uint32_t>(1 << i) == dwNumber)
            return i;
    return 0;
}

constexpr uint32_t kMaxShift = 15;

} // namespace

void MapZipper::UnInit()
{
    dwSizeX = dwDX = dwBlockSize = dwBlockShift = dwShiftNumBlocksX = dwNumRealBlocks = 0;
    wordTable = {};
    realData = {};
}

void MapZipper::DoZip(const uint8_t *pSrc, uint32_t _dwSizeX)
{
    uint32_t i, j, k, x, y, xx, yy;

    UnInit();

    uint32_t dwRealIndex = 0;

    dwSizeX = _dwSizeX;

    dwBlockSize = 8;
    dwBlockShift = Number2Shift(dwBlockSize);

    dwDX = dwSizeX >> dwBlockShift;

    dwShiftNumBlocksX = Number2Shift(dwDX);

    wordTable.assign(dwDX * dwDX, 0);
    realData.assign(dwSizeX * dwSizeX, 0);
    for (i = 0; i < dwDX * dwDX; i++)
    {
        y = i / dwDX;
        x = i - y * dwDX;
        const auto dwStart = (y << dwBlockShift) * dwSizeX + (x << dwBlockShift);

        auto bTest = true;
        uint8_t byTest = 0;
        for (j = 0; j < dwBlockSize * dwBlockSize; j++)
        {
            yy = j >> dwBlockShift;
            xx = j - (yy << dwBlockShift);
            const auto byRes = pSrc[dwStart + yy * dwSizeX + xx];
            if (j == 0)
                byTest = byRes;
            if (byTest != byRes)
            {
                bTest = false;
                wordTable[i] = static_cast<uint16_t>(dwRealIndex);
                for (k = 0; k < dwBlockSize * dwBlockSize; k++)
                {
                    yy = k >> dwBlockShift;
                    xx = k - (yy << dwBlockShift);
                    realData[dwRealIndex * dwBlockSize * dwBlockSize + k] = pSrc[dwStart + yy * dwSizeX + xx];
                }
                dwRealIndex++;
                break;
            }
        }
        if (bTest)
            wordTable[i] = static_cast<uint16_t>(0x8000) | static_cast<uint16_t>(byTest);
    }
    dwNumRealBlocks = dwRealIndex;
    realData.resize(dwRealIndex * dwBlockSize * dwBlockSize);
    realData.shrink_to_fit();

    for (y = 0; y < _dwSizeX; y++)
        for (x = 0; x < _dwSizeX; x++)
        {
            if (Get(x, y) != pSrc[x + y * _dwSizeX])
                psnip_trap();
        }
}

uint8_t MapZipper::Get(uint32_t dwX, uint32_t dwY) const
{
    if (wordTable.empty())
        return 255;
    const auto wRes = wordTable[((dwY >> dwBlockShift) << dwShiftNumBlocksX) + (dwX >> dwBlockShift)];
    if (wRes & 0x8000)
        return static_cast<uint8_t>(wRes & 0xFF);
    const auto x = dwX - ((dwX >> dwBlockShift) << dwBlockShift);
    const auto y = dwY - ((dwY >> dwBlockShift) << dwBlockShift);

    return realData[((static_cast<uint32_t>(wRes) << dwBlockShift) << dwBlockShift) + (y << dwBlockShift) + x];
}

bool MapZipper::Load(std::string sFileName)
{
    auto fileS = fio->_CreateFile(sFileName.c_str(), std::ios::binary | std::ios::in);
    return Read(fileS, sFileName);
}

bool MapZipper::Load(const std::filesystem::path &file)
{
    std::fstream fileS(file, std::ios::binary | std::ios::in);
    return Read(fileS, file.string());
}

bool MapZipper::Read(std::fstream &fileS, const std::string &name)
{
    UnInit();
    if (!fileS.is_open())
        return false;

    const auto fail = [&](const char *what) {
        spdlog::error("island: the zipped map {} is unreadable ({}), ignoring it", name, what);
        fio->_CloseFile(fileS);
        UnInit();
        return false;
    };

    uint32_t header[6];
    if (!fio->_ReadFile(fileS, header, sizeof(header)))
        return fail("short header");
    dwSizeX = header[0];
    dwDX = header[1];
    dwBlockSize = header[2];
    dwBlockShift = header[3];
    dwShiftNumBlocksX = header[4];
    dwNumRealBlocks = header[5];
    // Get() indexes by these shifts: the header must describe exactly the layout DoZip writes.
    if (dwBlockShift > kMaxShift || dwShiftNumBlocksX > kMaxShift || dwBlockSize != (1u << dwBlockShift) ||
        dwDX != (1u << dwShiftNumBlocksX) || dwSizeX != (dwDX << dwBlockShift) ||
        dwNumRealBlocks > static_cast<uint64_t>(dwDX) * dwDX || dwNumRealBlocks > 0x8000)
        return fail("inconsistent header");

    wordTable.resize(static_cast<size_t>(dwDX) * dwDX);
    if (!fio->_ReadFile(fileS, wordTable.data(), static_cast<std::streamsize>(sizeof(uint16_t) * wordTable.size())))
        return fail("short block table");
    for (const auto word : wordTable)
        if (!(word & 0x8000) && word >= dwNumRealBlocks)
            return fail("a block index past the data");

    realData.resize(static_cast<size_t>(dwNumRealBlocks) * dwBlockSize * dwBlockSize);
    if (!realData.empty() &&
        !fio->_ReadFile(fileS, realData.data(), static_cast<std::streamsize>(realData.size())))
        return fail("short block data");
    fio->_CloseFile(fileS);
    return true;
}

bool MapZipper::Save(const std::filesystem::path &file)
{
    std::error_code error;
    std::filesystem::create_directories(file.parent_path(), error);
    std::fstream fileS(file, std::ios::binary | std::ios::out);
    if (!fileS.is_open())
        return false;
    const uint32_t header[6] = {dwSizeX, dwDX, dwBlockSize, dwBlockShift, dwShiftNumBlocksX, dwNumRealBlocks};
    auto ok = fio->_WriteFile(fileS, header, sizeof(header));
    ok = ok && fio->_WriteFile(fileS, wordTable.data(),
                               static_cast<std::streamsize>(sizeof(uint16_t) * wordTable.size()));
    ok = ok && (realData.empty() ||
                fio->_WriteFile(fileS, realData.data(), static_cast<std::streamsize>(realData.size())));
    fio->_CloseFile(fileS);
    return ok && !fileS.fail();
}
