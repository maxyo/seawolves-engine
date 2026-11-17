// background_read.cpp and geom_texture_names.hpp: see run.py.
#include "background_read.hpp"
#include "geom_texture_names.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace
{
int failures = 0;
void require(bool condition, const char *what)
{
    if (!condition)
    {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

std::string write(const std::filesystem::path &path, const std::string &bytes)
{
    std::ofstream(path, std::ios::binary) << bytes;
    return path.string();
}

std::string text(const storm::io::FileBytes &bytes)
{
    return bytes ? std::string(bytes->begin(), bytes->end()) : std::string("<none>");
}

void reads(const std::filesystem::path &directory)
{
    using namespace storm::io;
    const std::string a = write(directory / "a.tx", "hello");
    const std::string b = write(directory / "b.tx", "world");
    const std::string empty = write(directory / "empty.tx", "");
    const std::string missing = (directory / "missing.tx").string(), missing2 = (directory / "missing2.tx").string();

    auto second = ReadInBackground({missing, a, b});
    auto none = ReadInBackground({missing, missing2});
    auto nothing = ReadInBackground({empty});
    // A read nobody waits for any more is dropped; the reader goes on with the others.
    ReadInBackground({b});
    second->wait();
    none->wait();
    nothing->wait();
    require(second->ready() && second->found() == 1 && text(second->bytes()) == "hello", "the first candidate that opens");
    require(none->found() == 2 && !none->bytes(), "no candidate opens");
    require(nothing->found() == 0 && nothing->bytes() && nothing->bytes()->empty(), "an empty file is read, empty");

    require(FindPrefetched(a) == nullptr, "nothing is handed out without a scope");
    {
        PrefetchScope scope;
        scope.add(*second);
        scope.add(*none);
        const Prefetched *before = FindPrefetched(missing), *read = FindPrefetched(a), *after = FindPrefetched(b);
        require(before && before->missing && !before->bytes, "a candidate before the one read is missing");
        require(read && !read->missing && text(read->bytes) == "hello", "the candidate read has its bytes");
        require(after == nullptr, "a candidate after the one read was not tried: the reader opens it itself");
        const Prefetched *none2 = FindPrefetched(missing2);
        require(none2 && none2->missing, "every candidate of a read that found none is missing");
    }
    require(FindPrefetched(a) == nullptr && FindPrefetched(missing) == nullptr, "the entries go with the scope");

    // Many at once come back in order, each with its own file.
    std::vector<std::shared_ptr<BackgroundRead>> many;
    for (int i = 0; i < 64; ++i)
        many.push_back(ReadInBackground({write(directory / ("m" + std::to_string(i)), std::to_string(i))}));
    many.back()->wait();
    for (int i = 0; i < 64; ++i)
        require(many[size_t(i)]->ready() && text(many[size_t(i)]->bytes()) == std::to_string(i), "a queued read");
}

// A model file with the given texture name offsets into its names.
std::vector<char> model(const std::string &names, const std::vector<int32_t> &textures, int32_t indices = 2)
{
    RDF_HEAD head{};
    head.version = RDF_VERSION;
    head.name_size = static_cast<int32_t>(names.size());
    head.names = indices;
    head.ntextures = static_cast<int32_t>(textures.size());
    std::vector<char> file(sizeof(head));
    std::memcpy(file.data(), &head, sizeof(head));
    file.insert(file.end(), names.begin(), names.end());
    file.resize(file.size() + size_t(indices) * sizeof(int32_t));
    for (const int32_t offset : textures)
    {
        const char *bytes = reinterpret_cast<const char *>(&offset);
        file.insert(file.end(), bytes, bytes + sizeof(offset));
    }
    return file;
}

void textureNames()
{
    using storm::geometry::TextureNames;
    const std::string names("body.tga\0\0hair.tga\0open", 23);
    const auto listed = TextureNames(model(names, {0, 9, 10, 99, 19}));
    require(listed == std::vector<std::string>({"body.tga", "hair.tga"}),
            "the names, without the empty slot, an offset outside the names or an unterminated name");
    auto truncated = model(names, {0, 10});
    truncated.resize(truncated.size() - 1);
    require(TextureNames(truncated).empty(), "a file shorter than its header says lists none");
    auto version = model(names, {0});
    version[0] ^= 1;
    require(TextureNames(version).empty(), "another file version lists none");
    require(TextureNames({}).empty(), "an empty file lists none");
}
} // namespace

int main(int argc, char **argv)
{
    if (argc < 2)
        return 2;
    reads(argv[1]);
    textureNames();
    if (failures)
        return 1;
    std::puts("background reads: candidates in order, prefetch scope, texture names of a model file");
    return 0;
}
