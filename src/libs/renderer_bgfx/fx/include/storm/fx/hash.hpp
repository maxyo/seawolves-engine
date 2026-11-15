// A 64-bit content hash for the bakes' cache keys: eight bytes a step (a megabyte of geometry in well under a
// millisecond), every bit of the input reaching the result. Not for anything an attacker chooses.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <type_traits>

namespace storm::fx
{

class Hasher
{
  public:
    Hasher &Bytes(const void *data, size_t size)
    {
        const auto *p = static_cast<const uint8_t *>(data);
        Mix(size);
        for (; size >= 8; size -= 8, p += 8)
        {
            uint64_t word;
            std::memcpy(&word, p, 8);
            Mix(word);
        }
        if (size)
        {
            uint64_t word = 0;
            std::memcpy(&word, p, size);
            Mix(word);
        }
        return *this;
    }
    template <class T> Hasher &Value(const T &value)
    {
        static_assert(std::is_arithmetic_v<T> || std::is_enum_v<T>, "a struct's padding would be hashed");
        return Bytes(&value, sizeof(value));
    }
    // Arrays of numbers, or of std::array of numbers (positions, texture coordinates): no padding in them.
    template <class T> Hasher &Span(std::span<const T> values) { return Bytes(values.data(), values.size_bytes()); }
    uint64_t Result() const
    {
        uint64_t h = state_;
        h ^= h >> 32;
        h *= 0xd6e8feb86659fd93ull;
        h ^= h >> 32;
        return h;
    }

  private:
    void Mix(uint64_t word)
    {
        state_ = (state_ ^ word) * 0x9e3779b97f4a7c15ull;
        state_ ^= state_ >> 29;
    }
    uint64_t state_ = 0x243f6a8885a308d3ull;
};

} // namespace storm::fx
