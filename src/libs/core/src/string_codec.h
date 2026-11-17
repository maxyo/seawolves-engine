#pragma once

#include "attributes.h"
#include "istring.hpp"
#include "vm_verify.hpp"

#include <algorithm>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <vector>

// The names of script attributes, each with a 32-bit code.
//
// The codes are part of the save format: a save stores the names in the order of Get()/GetNext() and every attribute
// by its code, and the load registers the names in that order to give each its code back. So the numbering never
// changes: the code is (bucket << 16) | (position in the bucket), where the bucket is the low 9 bits of the name's
// registered by its first lookup, found or not (Convert), so the set of names and their order follow the session.
//
// Looking a name up does not walk its bucket (30-60 names each in a running game): an open-addressing index from
// the case-folded name's hash to its code answers it.
class STRING_CODEC : public VSTRING_CODEC
{
  public:
    static constexpr uint32_t kBuckets = 512; // a power of 2
    static constexpr uint32_t kInvalidCode = 0xffffffff;

    uint32_t GetNum() override
    {
        return count_;
    }

    void Release()
    {
        index_.clear();
        count_ = 0;
        for (auto &bucket : buckets_)
            bucket.clear();
    }

    // At most iLen bytes of pString, stopping at a terminator.
    uint32_t Convert(const char *pString, int32_t iLen) override
    {
        if (pString == nullptr)
            return kInvalidCode;
        size_t length = 0;
        while (iLen > 0 && length < static_cast<size_t>(iLen) && pString[length] != 0)
            ++length;
        return Convert(std::string_view(pString, length));
    }

    uint32_t Convert(const char *pString) override
    {
        if (pString == nullptr)
            return kInvalidCode;
        return Convert(std::string_view(pString));
    }

    // The code of `name`, registered now if it is new.
    uint32_t Convert(std::string_view name)
    {
        const uint32_t hash = FoldedHash(name);
        if (const Slot *slot = FindSlot(name, hash))
            return slot->code;
#ifdef STORM_VM_VERIFY
        if (const auto walked = WalkBucket(name))
            storm::vm_verify::Mismatch("name '%.*s': not in the index, the bucket has it as %u",
                                       static_cast<int>(name.size()), name.data(), *walked);
#endif

        const uint32_t bucketIndex = BucketOf(name);
        auto &bucket = buckets_[bucketIndex];
        // 65536 names of one bucket: the 16-bit position would wrap and two names would share a code.
        if (bucket.size() > 0xffff)
            throw std::runtime_error("script name codec: a bucket is full");
        const uint32_t code = (bucketIndex << 16) | static_cast<uint32_t>(bucket.size());
        bucket.push_back({std::make_unique<char[]>(name.size() + 1), static_cast<uint32_t>(name.size())});
        memcpy(bucket.back().text.get(), name.data(), name.size());
        count_++;
        if (count_ * 2 > index_.size())
            GrowIndex();
        else
            Insert(hash, code);
        return code;
    }

    std::optional<uint32_t> Find(std::string_view name) const override
    {
        const Slot *slot = FindSlot(name, FoldedHash(name));
        return slot ? std::optional<uint32_t>(slot->code) : std::nullopt;
    }

    void VariableChanged() override;

    const char *Convert(uint32_t code) override
    {
        const uint32_t bucketIndex = code >> 16;
        if (bucketIndex >= kBuckets)
        {
            return "ERROR: invalid SCCT index";
        }
        const uint32_t n = code & 0xffff;
        if (n >= buckets_[bucketIndex].size())
        {
            return "INVALID SCC";
        }
        return buckets_[bucketIndex][n].text.get();
    }

    // The names in the order a save stores them: bucket by bucket.
    const char *Get()
    {
        for (iterBucket_ = 0; iterBucket_ < kBuckets; iterBucket_++)
        {
            if (!buckets_[iterBucket_].empty())
            {
                iterName_ = 0;
                return buckets_[iterBucket_][0].text.get();
            }
        }
        return nullptr;
    }

    const char *GetNext()
    {
        iterName_++;
        for (; iterBucket_ < kBuckets; iterBucket_++)
        {
            if (buckets_[iterBucket_].empty())
                continue;
            if (iterName_ >= buckets_[iterBucket_].size())
            {
                iterName_ = 0;
                continue;
            }
            return buckets_[iterBucket_][iterName_].text.get();
        }
        return nullptr;
    }

  private:
    // The PJW hash of the name with ASCII capitals folded, as the saves have always numbered the names.
    static uint32_t BucketOf(std::string_view name)
    {
        uint32_t hval = 0;
        for (char v : name)
        {
            if ('A' <= v && v <= 'Z')
                v += 'a' - 'A';
            hval = (hval << 4) + static_cast<uint32_t>(v);
            const uint32_t g = hval & (static_cast<uint32_t>(0xf) << (32 - 4));
            if (g != 0)
            {
                hval ^= g >> (32 - 8);
                hval ^= g;
            }
        }
        return hval & (kBuckets - 1);
    }

    struct Name
    {
        std::unique_ptr<char[]> text; // zero-terminated
        uint32_t length;
    };

    struct Slot
    {
        uint32_t hash;
        uint32_t code; // kInvalidCode: free
    };

    // Equal for names that differ only in the case of ASCII letters: every byte gets the case bit set (which also
    // merges a few punctuation pairs; the comparison of the names tells those apart). Eight bytes a step.
    static uint32_t FoldedHash(std::string_view name) noexcept
    {
        constexpr uint64_t kCaseBits = 0x2020202020202020ull;
        constexpr uint64_t kMultiplier = 0x9fb21c651e98df25ull;
        uint64_t hash = 0x9e3779b97f4a7c15ull ^ name.size();
        const char *at = name.data();
        size_t left = name.size();
        for (; left >= 8; at += 8, left -= 8)
        {
            uint64_t word;
            memcpy(&word, at, 8);
            hash = (hash ^ (word | kCaseBits)) * kMultiplier;
            hash ^= hash >> 32;
        }
        if (left != 0)
        {
            uint64_t word = 0;
            for (size_t n = 0; n < left; n++)
                word |= static_cast<uint64_t>(static_cast<uint8_t>(at[n])) << (8 * n);
            hash = (hash ^ (word | kCaseBits)) * kMultiplier;
            hash ^= hash >> 32;
        }
        return static_cast<uint32_t>(hash ^ (hash >> 29));
    }

    static bool SameName(const Name &stored, std::string_view name) noexcept
    {
        return storm::traits_cast<storm::ichar_traits<char>>(std::string_view(stored.text.get(), stored.length)) ==
               storm::traits_cast<storm::ichar_traits<char>>(name);
    }

    const Slot *FindSlot(std::string_view name, uint32_t hash) const noexcept
    {
        if (index_.empty())
            return nullptr;
        const size_t mask = index_.size() - 1;
        for (size_t at = hash & mask;; at = (at + 1) & mask)
        {
            const Slot &slot = index_[at];
            if (slot.code == kInvalidCode)
                return nullptr;
            if (slot.hash == hash && SameName(buckets_[slot.code >> 16][slot.code & 0xffff], name))
            {
#ifdef STORM_VM_VERIFY
                storm::vm_verify::Count(storm::vm_verify::Check::NameCode);
                if (WalkBucket(name) != slot.code)
                    storm::vm_verify::Mismatch("name '%.*s': the index says %u, the bucket does not",
                                               static_cast<int>(name.size()), name.data(), slot.code);
#endif
                return &slot;
            }
        }
    }

#ifdef STORM_VM_VERIFY
    // The plain lookup: the names of the bucket compared one by one.
    std::optional<uint32_t> WalkBucket(std::string_view name) const noexcept
    {
        const uint32_t bucketIndex = BucketOf(name);
        const auto &bucket = buckets_[bucketIndex];
        for (uint32_t n = 0; n < bucket.size(); n++)
            if (SameName(bucket[n], name))
                return (bucketIndex << 16) | n;
        return std::nullopt;
    }
#endif

    void Insert(uint32_t hash, uint32_t code) noexcept
    {
        const size_t mask = index_.size() - 1;
        size_t at = hash & mask;
        while (index_[at].code != kInvalidCode)
            at = (at + 1) & mask;
        index_[at] = {hash, code};
    }

    // Twice the slots (at most half of them are taken), every name entered again.
    void GrowIndex()
    {
        index_.assign(std::max<size_t>(1024, index_.size() * 2), Slot{0, kInvalidCode});
        for (uint32_t bucketIndex = 0; bucketIndex < kBuckets; bucketIndex++)
        {
            const auto &bucket = buckets_[bucketIndex];
            for (uint32_t n = 0; n < bucket.size(); n++)
                Insert(FoldedHash(std::string_view(bucket[n].text.get(), bucket[n].length)), (bucketIndex << 16) | n);
        }
    }

    std::vector<Name> buckets_[kBuckets];
    std::vector<Slot> index_; // open addressing, linear probing; a power of 2 slots
    uint32_t count_ = 0;
    uint32_t iterBucket_ = 0;
    uint32_t iterName_ = 0;
};
