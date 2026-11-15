#pragma once

// The unchanged save extension: two little-endian int32 sizes, a terminated description,
// then the original 128x128 A8R8G8B8 image. Views never own or modify save data.
#include <cstdint>
#include <charconv>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace storm::save::thumbnail
{
inline constexpr uint32_t kWidth = 128, kHeight = 128, kBytes = kWidth * kHeight * 4;
inline constexpr std::string_view kPrefix = "save-preview/";

struct Extension
{
    bool valid = false;                    // the description is readable: the save stays listed and loadable
    std::string_view description;
    std::span<const uint8_t> pixels;       // empty: no picture (a description-only save or a malformed picture)
    const char *picture_problem = nullptr; // why a picture the record carries is not shown (callers log it)
};

inline Extension Decode(std::span<const uint8_t> extension)
{
    if (extension.size() < 9) return {};
    const auto word = [&](size_t at) {
        return uint32_t(extension[at]) | uint32_t(extension[at + 1]) << 8 |
               uint32_t(extension[at + 2]) << 16 | uint32_t(extension[at + 3]) << 24;
    };
    const uint32_t description = word(0), image = word(4);
    if (!description || description > extension.size() - 8 || extension[8 + description - 1] != 0) return {};
    const std::string_view text(reinterpret_cast<const char *>(extension.data() + 8), description - 1);
    if (text.find('\0') != text.npos) return {};
    // The picture is optional: a record whose picture is not the expected 128x128 A8R8G8B8 keeps its description
    // (the game refuses to load a save without one), only the picture is dropped.
    Extension result{true, text};
    const size_t rest = extension.size() - 8 - description;
    if (image == 0 && rest == 0) return result;
    if (image != kBytes) result.picture_problem = "its picture is not 128x128 A8R8G8B8";
    else if (rest != image) result.picture_problem = "its picture size does not match the record";
    else result.pixels = extension.subspan(8 + description, image);
    return result;
}

inline std::span<const uint8_t> Pixels(std::span<const uint8_t> extension)
{
    return Decode(extension).pixels;
}

inline bool Component(std::string_view text)
{
    return !text.empty() && text != "." && text != ".." && text.size() <= 1024 &&
           text.find_first_of("/\\:") == text.npos && text.find('\0') == text.npos;
}

inline std::string Hex(std::string_view text)
{
    constexpr char digits[] = "0123456789abcdef";
    std::string result;
    for (unsigned char byte : text) { result += digits[byte >> 4]; result += digits[byte & 15]; }
    return result;
}

inline std::string Unhex(std::string_view text)
{
    if (text.empty() || text.size() % 2 || text.size() > 2048) return {};
    const auto digit = [](char c) -> int { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; };
    std::string result;
    for (size_t i = 0; i < text.size(); i += 2)
    {
        const int hi = digit(text[i]), lo = digit(text[i + 1]);
        if (hi < 0 || lo < 0) return {};
        result += static_cast<char>((hi << 4) | lo);
    }
    return Component(result) ? result : std::string();
}

// Opening and exact write-time distinguish replaced records and a reopened UI's texture lifetime.
// The provider can read only one record under SAVE, never an arbitrary path from the UI.
inline std::string Source(std::string_view save_path, uint64_t opening, uint64_t written)
{
    if (!save_path.starts_with("SAVE\\")) return {};
    save_path.remove_prefix(5);
    const size_t slash = save_path.find('\\');
    if (slash == save_path.npos) return {};
    const auto profile = save_path.substr(0, slash), file = save_path.substr(slash + 1);
    if (!Component(profile) || !Component(file)) return {};
    return std::string(kPrefix) + std::to_string(opening) + "/" + std::to_string(written) + "/" + Hex(profile) + "/" + Hex(file);
}

inline std::string Path(std::string_view source)
{
    if (!source.starts_with(kPrefix)) return {};
    source.remove_prefix(kPrefix.size());
    for (int i = 0; i < 2; ++i)
    {
        const size_t slash = source.find('/');
        if (!slash || slash == source.npos || slash > 20 || source.substr(0, slash).find_first_not_of("0123456789") != source.npos) return {};
        source.remove_prefix(slash + 1);
    }
    const size_t slash = source.find('/');
    if (slash == source.npos) return {};
    const std::string profile = Unhex(source.substr(0, slash)), file = Unhex(source.substr(slash + 1));
    return profile.empty() || file.empty() ? std::string() : "SAVE\\" + profile + "\\" + file;
}

inline std::optional<uint64_t> Written(std::string_view source)
{
    if (Path(source).empty()) return {};
    source.remove_prefix(kPrefix.size());
    source.remove_prefix(source.find('/') + 1);
    source = source.substr(0, source.find('/'));
    uint64_t result = 0;
    const auto parsed = std::from_chars(source.data(), source.data() + source.size(), result);
    return parsed.ec == std::errc{} && parsed.ptr == source.data() + source.size() ? std::optional(result) : std::nullopt;
}
} // namespace storm::save::thumbnail
