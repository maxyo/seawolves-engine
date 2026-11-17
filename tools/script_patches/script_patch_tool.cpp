// Host tool for script patch layers (src/libs/util/include/platform/script_patches.hpp),
// with the engine's own parser and applier, so a layer is checked by the code
// that loads it.
//
//   script_patch_tool check <layer>
//       Parses every set; exit 1 if the layer is damaged.
//   script_patch_tool apply <layer> <game dir> <out dir>
//       Applies the layer to a game directory as the engine does at start-up and
//       writes the patched scripts to <out dir>/<game-relative path>: a file is
//       rewritten only when its text changed (its time stays, so a script cache
//       keyed by file times holds), files of earlier runs that are no longer
//       patched are removed. Prints the report. Exit 0 when
//       every fix of the matching set applied, 2 when some did not or no set
//       identifies the game, 1 if the layer is damaged.
//
// Build: c++ -std=c++20 -I src/libs/util/include tools/script_patches/script_patch_tool.cpp
#include "platform/script_patches.hpp"

#include <cstdio>
#include <iostream>
#include <set>

namespace fs = std::filesystem;
using namespace storm::script_patches;

int main(int argc, char **argv)
{
    const std::string command = argc > 1 ? argv[1] : "";
    try
    {
        if (command == "check" && argc == 3)
        {
            for (const auto &set : loadLayer(argv[2]))
                std::cout << "set " << set.id << " (" << set.game << " " << set.version << "): "
                          << set.targets.size() << " target(s), " << set.fixes.size() << " fix(es)\n";
            return 0;
        }
        if (command == "apply" && argc == 5)
        {
            const fs::path game(argv[3]);
            std::unordered_map<std::string, std::string> paths;
            for (const auto &entry : fs::recursive_directory_iterator(game))
                if (entry.is_regular_file())
                    paths[storm::android::resourceKey(entry.path().lexically_relative(game))] = entry.path().string();
            Derived derived;
            const auto report = applyLayer(argv[2], paths, derived);
            const fs::path out(argv[4]);
            std::set<fs::path> written;
            for (const auto &[file, text] : derived)
            {
                const auto relative = file.substr(memoryPath("").size());
                const auto path = out / fs::path(std::u8string(relative.begin(), relative.end()));
                written.insert(path);
                std::string existing;
                if (detail::readFile(path, existing) && existing == text)
                    continue;
                fs::create_directories(path.parent_path());
                std::ofstream stream(path, std::ios::binary | std::ios::trunc);
                stream.write(text.data(), static_cast<std::streamsize>(text.size()));
                if (!stream.flush())
                {
                    std::cerr << "cannot write " << path << "\n";
                    return 1;
                }
            }
            std::vector<fs::path> stale;
            if (fs::is_directory(out))
                for (const auto &entry : fs::recursive_directory_iterator(out))
                    if (!entry.is_directory() && !written.contains(entry.path()))
                        stale.push_back(entry.path());
            for (const auto &path : stale)
                fs::remove(path);
            for (const auto &line : report.info)
                std::cout << line << "\n";
            for (const auto &line : report.errors)
                std::cout << "ERROR " << line << "\n";
            for (const auto &fix : report.unapplied)
                for (const auto &reason : fix.reasons)
                    std::cout << "UNAPPLIED " << fix.set << "/" << fix.fix << ": " << reason << "\n";
            return report.set.empty() || !report.unapplied.empty() ? 2 : 0;
        }
    }
    catch (const LayerError &error)
    {
        std::cerr << "damaged script patch layer: " << error.what() << "\n";
        return 1;
    }
    std::cerr << "usage: script_patch_tool check <layer> | apply <layer> <game dir> <out dir>\n";
    return 64;
}
