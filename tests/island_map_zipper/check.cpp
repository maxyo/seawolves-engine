// MapZipper (src/libs/island/src/map_zipper.cpp): the zipped island depth map. A zipper that holds a map and loads
// again from a missing source then from the cache (ISLAND::LoadZap on an island reload) must not free its buffers
// twice; DoZip -> Save -> Load keeps every value; a short or garbled file is refused, loudly, and leaves it empty.
#include "map_zipper.h"
#include "v_file_service.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <vector>

TestFiles files;
TestFiles *fio = &files;

namespace
{
int failures = 0;
void expect(bool ok, const char *what)
{
    std::printf("%s: %s\n", ok ? "ok" : "FAIL", what);
    failures += ok ? 0 : 1;
}

std::vector<uint8_t> Map(uint32_t size)
{
    std::vector<uint8_t> map(size * size);
    for (uint32_t y = 0; y < size; y++)
        for (uint32_t x = 0; x < size; x++)
            map[y * size + x] = x < size / 2 ? 7 : static_cast<uint8_t>((x * 31 + y * 17) & 0xFF); // flat and noisy blocks
    return map;
}

bool Same(const MapZipper &zipper, const std::vector<uint8_t> &map, uint32_t size)
{
    if (!zipper.isLoaded() || zipper.GetSizeX() != size)
        return false;
    for (uint32_t y = 0; y < size; y++)
        for (uint32_t x = 0; x < size; x++)
            if (zipper.Get(x, y) != map[y * size + x])
                return false;
    return true;
}

std::vector<char> Bytes(const std::filesystem::path &file)
{
    std::ifstream in(file, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
}

void Write(const std::filesystem::path &file, const std::vector<char> &bytes)
{
    std::ofstream(file, std::ios::binary | std::ios::trunc).write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}
} // namespace

int main(int argc, char **argv)
{
    namespace fsys = std::filesystem;
    const fsys::path dir = argc > 1 ? fsys::path(argv[1]) : fsys::temp_directory_path() / "storm-map-zipper";
    fsys::create_directories(dir);
    files.root = dir.string();
    const uint32_t size = 64;
    const auto map = Map(size);

    MapZipper zipper;
    expect(!zipper.isLoaded() && zipper.Get(0, 0) == 255, "a new zipper is empty");
    zipper.DoZip(map.data(), size);
    expect(Same(zipper, map, size), "DoZip keeps every value");
    const fsys::path cached = dir / "cache/Island/x.tga.zap";
    expect(zipper.Save(cached), "Save writes into a new directory");

    MapZipper loaded;
    expect(loaded.Load(cached) && Same(loaded, map, size), "Save -> Load keeps every value");

    // ISLAND::LoadZap on a zipper that already holds a map: the shipped file is missing, then the cached one loads.
    expect(!loaded.Load(std::string("missing.tga.zap")), "a missing shipped file is not loaded");
    expect(!loaded.isLoaded(), "a failed load leaves the zipper empty");
    expect(loaded.Load(cached) && Same(loaded, map, size), "the cached file loads after the missing one");
    expect(!loaded.Load(std::string("missing.tga.zap")) && !loaded.Load(std::string("missing.tga.zap")) &&
               loaded.Load(cached) && Same(loaded, map, size),
           "repeated missing loads then a load (no double free under ASan)");

    MapZipper flat;
    const std::vector<uint8_t> uniform(size * size, 3);
    flat.DoZip(uniform.data(), size);
    expect(Same(flat, uniform, size), "a map of flat blocks only is loaded (no real data)");
    const fsys::path flatFile = dir / "flat.zap";
    MapZipper flatLoaded;
    expect(flat.Save(flatFile) && flatLoaded.Load(flatFile) && Same(flatLoaded, uniform, size),
           "a map of flat blocks only round-trips");

    const auto bytes = Bytes(cached);
    const fsys::path bad = dir / "bad.zap";
    Write(bad, std::vector<char>(bytes.begin(), bytes.end() - 1));
    expect(!loaded.Load(bad) && !loaded.isLoaded(), "a file one byte short is refused");
    Write(bad, std::vector<char>(bytes.begin(), bytes.begin() + 10));
    expect(!loaded.Load(bad) && !loaded.isLoaded(), "a short header is refused");
    auto garbled = bytes;
    garbled[0] = 0x7f; // dwSizeX no longer dwDX << dwBlockShift
    Write(bad, garbled);
    expect(!loaded.Load(bad) && !loaded.isLoaded(), "an inconsistent header is refused");
    garbled = bytes;
    garbled[6 * 4 + 2 * (size / 8 - 1)] = 0x7f; // the last block of the first row: a real block, now past the data
    garbled[6 * 4 + 2 * (size / 8 - 1) + 1] = 0x00;
    Write(bad, garbled);
    expect(!loaded.Load(bad) && !loaded.isLoaded(), "a block index past the data is refused");
    expect(loaded.Load(cached) && Same(loaded, map, size), "the zipper loads again after refused files");

    fsys::remove_all(dir);
    std::printf("%s\n", failures ? "FAILED" : "passed");
    return failures ? 1 : 0;
}
