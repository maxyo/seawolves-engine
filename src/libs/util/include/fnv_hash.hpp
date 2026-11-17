#pragma once

// 64-bit FNV-1a: the engine's hash for cache keys, change signatures and hash tables. Keys written to disk depend on
// these exact values.

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace storm::hash
{
inline constexpr uint64_t kFnvOffset = 14695981039346656037ull;
inline constexpr uint64_t kFnvPrime = 1099511628211ull;

// One round over a whole value (a word, an id, a field) instead of its bytes.
constexpr uint64_t fnvRound(uint64_t hash, uint64_t value)
{
    return (hash ^ value) * kFnvPrime;
}

inline uint64_t fnv1a(const void *data, size_t size, uint64_t hash = kFnvOffset)
{
    const auto *bytes = static_cast<const unsigned char *>(data);
    for (size_t i = 0; i < size; ++i)
        hash = fnvRound(hash, bytes[i]);
    return hash;
}

constexpr uint64_t fnv1a(std::string_view text, uint64_t hash = kFnvOffset)
{
    for (const unsigned char c : text)
        hash = fnvRound(hash, c);
    return hash;
}
} // namespace storm::hash
