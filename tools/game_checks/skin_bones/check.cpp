// The skin bones (src/libs/animation/include/skin_bones.hpp) of every skeleton (.an) of a game: each human skeleton
// (one an .ani file names together with "man", "woman" or a character's own animation) has its neck and two hands;
// the list says which skeletons have none (animals). Exit 1 when a skeleton that the expectation list names as human
// has no skin bones.
//
//   check <game or resource directory> [human skeleton file name]...
#include "skin_bones.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>

namespace fs = std::filesystem;

int main(int argc, char **argv)
{
    if (argc < 2)
    {
        std::fprintf(stderr, "usage: %s <game or resource directory> [human skeleton]...\n", argv[0]);
        return 2;
    }
    std::set<std::string> human(argv + 2, argv + argc);
    std::vector<fs::path> files;
    for (const auto &entry : fs::recursive_directory_iterator(argv[1]))
    {
        std::string ext = entry.path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        if (entry.is_regular_file() && ext == ".an")
            files.push_back(entry.path());
    }
    std::sort(files.begin(), files.end());
    int failed = 0, found = 0;
    for (const fs::path &file : files)
    {
        std::ifstream in(file, std::ios::binary);
        int32_t header[3];
        in.read(reinterpret_cast<char *>(header), sizeof(header));
        const int joints = header[1];
        std::vector<int32_t> parents(joints);
        std::vector<glm::vec3> offsets(joints);
        in.read(reinterpret_cast<char *>(parents.data()), joints * sizeof(int32_t));
        in.read(reinterpret_cast<char *>(offsets.data()), joints * sizeof(glm::vec3));
        if (!in || joints <= 0)
        {
            std::printf("FAIL %s: unreadable\n", file.filename().string().c_str());
            ++failed;
            continue;
        }
        parents[0] = -1;
        const storm::animation::SkinBones bones = storm::animation::FindSkinBones(parents, offsets);
        const std::string name = file.filename().string();
        if (bones.skin.empty())
        {
            const bool expected = human.count(name) != 0;
            failed += expected ? 1 : 0;
            std::printf("%s %s (%d joints): %s\n", expected ? "FAIL" : "none", name.c_str(), joints, bones.failure.c_str());
            continue;
        }
        ++found;
        int skin = 0;
        for (const uint8_t s : bones.skin)
            skin += s;
        std::printf("ok   %s (%d joints): shoulder %d, neck %d, hands %d %d, %d skin bones\n", name.c_str(), joints,
                    bones.shoulder, bones.neck, bones.hands[0], bones.hands[1], skin);
    }
    std::printf("%d skeletons, %d with skin bones, %d failed\n", static_cast<int>(files.size()), found, failed);
    return failed ? 1 : 0;
}
