#pragma once
#include <charconv>
#include <cstdint>
#include <optional>
#include <string_view>
namespace storm::render {
struct SceneLoadAction { uint64_t generation; bool cancel; };
inline std::optional<SceneLoadAction> ParseSceneLoadAction(std::string_view wire) {
    if (!wire.starts_with("location-load:")) return {};
    wire.remove_prefix(14); const auto tab = wire.find('\t');
    if (tab == std::string_view::npos || tab == 0) return {};
    uint64_t generation = 0; const auto parsed = std::from_chars(wire.data(), wire.data()+tab, generation);
    if (parsed.ec != std::errc{} || parsed.ptr != wire.data()+tab || generation == 0) return {};
    const auto verb = wire.substr(tab+1);
    if (verb != "retry" && verb != "cancel") return {};
    return SceneLoadAction{generation, verb == "cancel"};
}
}
