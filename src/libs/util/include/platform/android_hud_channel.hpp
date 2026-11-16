// The last publication of one HUD state channel of the portable UI (android_ui.hpp): the typed
// state its JSON was built from and the JSON last published. The main loop asks every channel
// every iteration; a channel whose typed state equals the one its last JSON was built from builds
// nothing and publishes nothing. A changed one builds its JSON and publishes it unless it equals
// the last published JSON (a change the wire format does not carry, such as a cooldown moving
// below its printed precision), so what reaches the UI is exactly what comparing the JSON text
// every loop sent.
//
// The typed state is a tuple of references a state class hands out (publicationKey()): every
// field its json() reads, or a revision counter its every such change advances.
#pragma once
#include <optional>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>

namespace storm::android
{
template <class Tuple> struct HudChannelOwned;
template <class... T> struct HudChannelOwned<std::tuple<T...>>
{
    using type = std::tuple<std::remove_cvref_t<T>...>;
};

template <class KeyRefs> class HudChannel
{
    using Key = typename HudChannelOwned<KeyRefs>::type;
    std::optional<Key> key_;
    std::string json_;

  public:
    // A new UI host: the next publish sends whatever the state is.
    void reset()
    {
        key_.reset();
        json_.clear();
    }
    // The typed state equals the one the last JSON was built from.
    bool same(const KeyRefs &key) const
    {
        return key_ && *key_ == key;
    }
    // The JSON to publish now, or nullptr. `build` runs only when the typed state changed.
    template <class Build> const std::string *publish(const KeyRefs &key, Build &&build)
    {
        if (same(key)) return nullptr;
        key_.emplace(key);
        auto json = std::forward<Build>(build)();
        if (json == json_) return nullptr;
        json_ = std::move(json);
        return &json_;
    }
    // The JSON last published ("" before the first and after reset()).
    const std::string &json() const
    {
        return json_;
    }
};
} // namespace storm::android
