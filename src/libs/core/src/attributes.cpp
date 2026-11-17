#include "attributes.h"
#include "script_state.hpp"

#include "string_compare.hpp"
#include "platform/frame_work.hpp"
#include "platform/platform.hpp"

#include "vm_verify.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <unordered_set>

namespace storm
{
namespace
{
std::atomic<uint64_t> scriptStateGeneration{0};
std::unordered_set<const ATTRIBUTES *> watchedTrees; // ScriptStateReads since the last ScriptStateForget
bool watching = false; // an adapter ran since the last ScriptStateForget: changes of watchedTrees count
std::unordered_set<const ATTRIBUTES *> *readTrees = nullptr; // an open ScriptStateReads

const ATTRIBUTES *TreeOf(const ATTRIBUTES *attribute)
{
    while (const ATTRIBUTES *parent = attribute->GetParent())
        attribute = parent;
    return attribute;
}

// An attribute of `attribute`'s tree changed.
void TreeChanged(const ATTRIBUTES *attribute)
{
    if (!watching || readTrees)
        return; // no cached facts to keep, or an adapter's own output
    if (watchedTrees.count(TreeOf(attribute)))
        ScriptStateChanged();
}

void TreeRead(const ATTRIBUTES *attribute)
{
    if (readTrees)
        readTrees->insert(TreeOf(attribute));
}

ScriptNameLookups nameLookups;
bool scriptRunning = false;
uint64_t scriptStateEpoch = 1;

bool InHud() noexcept
{
    return platform::frame_work::state().current == platform::frame_work::Phase::Hud;
}

// Engine code looks an attribute up by name.
void NameLookup() noexcept
{
    if (scriptRunning)
        return;
    nameLookups.attributes++;
    if (readTrees)
        nameLookups.adapterAttributes++;
    if (InHud())
        nameLookups.hudAttributes++;
}
} // namespace

ScriptNameLookups &NameLookups() noexcept
{
    return nameLookups;
}

void ScriptRunning(bool running) noexcept
{
    scriptRunning = running;
    if (running)
        ScriptStateMayChange();
}

uint64_t ScriptStateEpoch() noexcept
{
    return scriptStateEpoch;
}

void ScriptStateMayChange() noexcept
{
    scriptStateEpoch++;
}

void CountVariableLookup() noexcept
{
    if (scriptRunning)
        return;
    nameLookups.variables++;
    if (InHud())
        nameLookups.hudVariables++;
}

uint64_t ScriptStateGeneration() noexcept
{
    return scriptStateGeneration.load(std::memory_order_relaxed);
}

void ScriptStateChanged() noexcept
{
    scriptStateGeneration.fetch_add(1, std::memory_order_relaxed);
}

ScriptStateReads::ScriptStateReads() : nested_(readTrees != nullptr)
{
    if (!nested_)
        readTrees = new std::unordered_set<const ATTRIBUTES *>;
}

ScriptStateReads::~ScriptStateReads()
{
    if (nested_)
        return;
    watchedTrees.insert(readTrees->begin(), readTrees->end());
    watching = true;
    delete readTrees;
    readTrees = nullptr;
}

void ScriptStateForget()
{
    watchedTrees.clear();
    watching = false;
}
} // namespace storm

namespace
{
// The generation of every attribute slot (ATTRIBUTES::Handle). A destroyed attribute's slot gets the next generation
// and is reused by a later attribute. Script attributes live on the script thread; another thread would race the
// tables, so it stops the engine.
class Lifetimes
{
  public:
    uint32_t Acquire()
    {
        CheckThread("created");
        if (free_.empty())
        {
            generations_.push_back(0);
            return static_cast<uint32_t>(generations_.size() - 1);
        }
        const uint32_t slot = free_.back();
        free_.pop_back();
        return slot;
    }

    void Release(uint32_t slot)
    {
        CheckThread("destroyed");
        generations_[slot]++;
        free_.push_back(slot);
    }

    [[nodiscard]] uint32_t Generation(uint32_t slot) const noexcept
    {
        return generations_[slot];
    }

  private:
    void CheckThread(const char *what) const
    {
        if (std::this_thread::get_id() == owner_)
            return;
        std::fprintf(stderr, "script attributes: an attribute %s on another thread than the script thread\n", what);
        std::abort();
    }

    std::thread::id owner_ = std::this_thread::get_id();
    std::vector<uint32_t> generations_;
    std::vector<uint32_t> free_;
};

// Never destroyed: attributes of static objects are destroyed after every other static.
Lifetimes &lifetimes()
{
    static auto *instance = new Lifetimes;
    return *instance;
}

constexpr size_t kNoIndex = static_cast<size_t>(-1);
// An object with this many attributes is worth a hash index (a scan of fewer codes is as fast)...
constexpr size_t kIndexedChildren = 32;
// ... once it was looked up this many times since its children last moved: a script that deletes an attribute and
// looks another up in turn would otherwise rebuild the index for every lookup.
constexpr uint8_t kScansBeforeIndex = 4;

size_t SlotOf(uint32_t code, size_t mask) noexcept
{
    return (static_cast<uint64_t>(code) * 0x9e3779b97f4a7c15ull >> 32) & mask;
}

// The first position of `code`, or kNoIndex. Eight codes per step without a branch between them (the compiler makes
// it one vector comparison); an object of the game has up to a few hundred attributes.
size_t IndexOfCode(const std::vector<uint32_t> &codes, uint32_t code) noexcept
{
    const uint32_t *const data = codes.data();
    const size_t size = codes.size();
    size_t n = 0;
    for (; n + 8 <= size; n += 8)
    {
        uint32_t any = 0;
        for (size_t k = 0; k < 8; k++)
            any |= static_cast<uint32_t>(data[n + k] == code);
        if (any)
            break;
    }
    for (; n < size; n++)
        if (data[n] == code)
            return n;
    return kNoIndex;
}
} // namespace

ATTRIBUTES::ATTRIBUTES(VSTRING_CODEC *p): ATTRIBUTES(*p)
{
}

ATTRIBUTES::ATTRIBUTES(ATTRIBUTES &&other) noexcept
    : stringCodec_(other.stringCodec_), nameCode_(other.stringCodec_.Convert("root")), slot_(lifetimes().Acquire()),
      value_(std::move(other.value_)),
      attributes_(std::move(other.attributes_)), codes_(std::move(other.codes_)), break_(other.break_)
{
    AdoptChildren();
}

// Children moved in from another object still named it as their parent; it may be a temporary (Copy()) that is
// destroyed right after the move, and every later walk up from a child (GetParent, the script state's tree of a
// write) would read freed memory.
void ATTRIBUTES::AdoptChildren() noexcept
{
    for (const auto &attribute : attributes_)
        attribute->parent_ = this;
}

ATTRIBUTES & ATTRIBUTES::operator=(ATTRIBUTES &&other) noexcept
{
    stringCodec_ = other.stringCodec_;
    // Do not update name code
    value_ = std::move(other.value_);
    attributes_ = std::move(other.attributes_);
    codes_ = std::move(other.codes_);
    ChildrenReordered();
    AdoptChildren();
    // Do not update parent
    break_ = other.break_;
    storm::TreeChanged(this);
    return *this;
}

ATTRIBUTES::~ATTRIBUTES()
{
    // A watched tree going away is a change; its address may be reused by a tree nobody read.
    if (!parent_ && (storm::watching || storm::readTrees))
    {
        if (storm::readTrees)
            storm::readTrees->erase(this); // a script's local object: gone when the adapter ends
        if (storm::watchedTrees.erase(this) && !storm::readTrees)
            storm::ScriptStateChanged();
    }
    Release();
    lifetimes().Release(slot_);
}

ATTRIBUTES::Handle ATTRIBUTES::GetHandle() const noexcept
{
    return {slot_, lifetimes().Generation(slot_)};
}

bool ATTRIBUTES::IsAlive(Handle handle) noexcept
{
    return lifetimes().Generation(handle.slot) == handle.generation;
}

ATTRIBUTES *ATTRIBUTES::GetParent() const
{
    return parent_;
}

bool ATTRIBUTES::operator==(const char *str) const
{
    storm::TreeRead(this);
    if (!str || !str[0])
        return false;
    return storm::iEquals(stringCodec_.Convert(nameCode_), str);
}

const char *ATTRIBUTES::GetThisName() const
{
    storm::TreeRead(this);
    return stringCodec_.Convert(nameCode_);
}

bool ATTRIBUTES::HasValue() const noexcept
{
    storm::TreeRead(this);
    return value_.has_value();
}

const std::string & ATTRIBUTES::GetValue() const
{
    storm::TreeRead(this);
    return *value_;
}

ATTRIBUTES::LegacyProxy ATTRIBUTES::GetThisAttr() const
{
    storm::TreeRead(this);
    return value_;

}

void ATTRIBUTES::SetName(const std::string_view &new_name)
{
    nameCode_ = stringCodec_.Convert(new_name.data());
    RenamedInParent();
    storm::TreeChanged(this);
}

void ATTRIBUTES::SetValue(const char *new_value)
{
    if (new_value == nullptr)
    {
        if (value_)
            storm::TreeChanged(this);
        value_.reset();
    }
    else {
        if (!value_ || *value_ != new_value)
            storm::TreeChanged(this);
        value_ = new_value;
    }

    if (break_)
        stringCodec_.VariableChanged();
}

void ATTRIBUTES::SetValue(const std::string_view &new_value)
{
    if (!value_ || *value_ != new_value)
        storm::TreeChanged(this);
    value_ = new_value;

    if (break_)
        stringCodec_.VariableChanged();
}

size_t ATTRIBUTES::GetAttributesNum() const
{
    storm::TreeRead(this);
    return attributes_.size();
}

ATTRIBUTES * ATTRIBUTES::GetAttributeClass(const std::string_view &name) const
{
    storm::TreeRead(this);
    return FirstWithName(name);
}

ATTRIBUTES * ATTRIBUTES::GetAttributeClass(uint32_t n) const
{
    storm::TreeRead(this);
    return n >= attributes_.size() ? nullptr : attributes_[n].get();
}

ATTRIBUTES *ATTRIBUTES::VerifyAttributeClass(const std::string_view &name)
{
    storm::TreeRead(this);
    auto *const pTemp = GetAttributeClass(name);
    return (pTemp) ? pTemp : CreateAttribute(name, "");
}

const char *ATTRIBUTES::GetAttributeName(size_t n) const
{
    storm::TreeRead(this);
    if (n < attributes_.size())
    {
        return attributes_[n]->GetThisName();
    }
    else
    {
        return nullptr;
    }
}

ATTRIBUTES::LegacyProxy ATTRIBUTES::GetAttribute(size_t n) const
{
    storm::TreeRead(this);
    if (n < attributes_.size()) {
        return attributes_[n]->value_;
    }
    else {
        return {};
    }
}


ATTRIBUTES::LegacyProxy ATTRIBUTES::GetAttribute(const std::string_view &name) const
{
    storm::TreeRead(this);
    if (const ATTRIBUTES *attribute = FirstWithName(name))
        return attribute->value_;
    return {};
}

uint32_t ATTRIBUTES::GetAttributeAsDword(const char *name, uint32_t def) const
{
    storm::TreeRead(this);
    uint32_t vDword = def;
    if (name)
    {
        const char *pAttribute = GetAttribute(name);
        if (pAttribute)
            vDword = atol(pAttribute);
    }
    else
    {
        vDword = atol( value_->c_str());
    }
    return vDword;
}

float ATTRIBUTES::GetAttributeAsFloat(const char *name, float def) const
{
    storm::TreeRead(this);
    float vFloat = def;
    if (name)
    {
        if (const char *pAttribute = GetAttribute(name))
            vFloat = static_cast<float>(atof(pAttribute));
    }
    else
    {
        vFloat = static_cast<float>(atof(value_->c_str()));
    }
    return vFloat;
}

bool ATTRIBUTES::SetAttributeUseDword(const char *name, uint32_t val)
{
    std::string buffer;
    buffer = std::to_string(val);
    if (name)
        return SetAttribute(name, buffer) != 0;
    SetValue(buffer);
    return true;
}

bool ATTRIBUTES::SetAttributeUseFloat(const char *name, float val)
{
    char buffer[128];
    sprintf_s(buffer, "%g", val);
    if (name)
        return SetAttribute(name, buffer) != 0;
    SetValue(buffer);

    return true;
}

ATTRIBUTES & ATTRIBUTES::CreateAttribute(const std::string_view &name)
{
    return *Append(stringCodec_.Convert(name.data()));
}

ATTRIBUTES * ATTRIBUTES::CreateAttribute(const std::string_view &name, const char *attribute)
{
    return CreateAttribute(stringCodec_.Convert(name.data()), attribute);
}

size_t ATTRIBUTES::SetAttribute(const std::string_view &name, const char *attribute)
{
    return SetAttribute(stringCodec_.Convert(name.data()), attribute);
}

size_t ATTRIBUTES::SetAttribute(const std::string_view &name, const std::string_view &attribute)
{
    return SetAttribute(stringCodec_.Convert(name.data()), attribute);
}

bool ATTRIBUTES::DeleteAttributeClassX(ATTRIBUTES *pA)
{
    if (pA == nullptr)
        return false;
    if (pA == this)
    {
        if (!attributes_.empty())
            storm::TreeChanged(this);
        attributes_.clear();
        codes_.clear();
        ChildrenReordered();
    }
    else
    {
        // The target already knows its owner. Validate that it is below this receiver,
        // then erase there instead of searching every unrelated character attribute.
        auto *owner = pA->parent_;
        auto *ancestor = owner;
        while (ancestor && ancestor != this)
            ancestor = ancestor->parent_;
        if (!ancestor)
            return false;
        for (auto it = owner->attributes_.begin(); it != owner->attributes_.end(); ++it)
        {
            if (it->get() == pA)
            {
                owner->codes_.erase(owner->codes_.begin() + (it - owner->attributes_.begin()));
                owner->ChildrenReordered();
                owner->attributes_.erase(it);
                storm::TreeChanged(owner);
                return true;
            }
        }
    }
    return false;
}

ATTRIBUTES * ATTRIBUTES::CreateSubAClass(ATTRIBUTES *pRoot, const char *access_string)
{
    uint32_t dwNameCode;
    uint32_t n = 0;
    ATTRIBUTES *pResult = nullptr;
    ATTRIBUTES *pTemp;
    if (pRoot == nullptr)
        return nullptr;
    if (access_string == nullptr)
        return nullptr;

    while (true)
    {
        switch (access_string[n])
        {
        case '.':
            dwNameCode = stringCodec_.Convert(access_string, n);
            pTemp = pRoot->GetAttributeClassByCode(dwNameCode);
            if (!pTemp)
                pTemp = pRoot->Append(dwNameCode);
            pResult = CreateSubAClass(pTemp, &access_string[n + 1]);
            return pResult;

        case 0:
            dwNameCode = stringCodec_.Convert(access_string);
            pResult = pRoot->GetAttributeClassByCode(dwNameCode);
            return (pResult) ? pResult : pRoot->Append(dwNameCode);
        default: ;
        }
        n++;
    }
    return nullptr;
}

ATTRIBUTES * ATTRIBUTES::FindAClass(ATTRIBUTES *pRoot, const char *access_string)
{
    if (pRoot)
        storm::TreeRead(pRoot);
    storm::NameLookup();
    uint32_t n = 0;
    ATTRIBUTES *pResult = nullptr;
    ATTRIBUTES *pTemp = nullptr;

    if (!pRoot || !access_string)
        return nullptr;
    if (!access_string[0])
        return pRoot;

    while (true)
    {
        switch (access_string[n])
        {
        case '.':
            pTemp = pRoot->GetAttributeClassByCode(stringCodec_.Convert(access_string, n));
            if (!pTemp)
                return nullptr;
            pResult = FindAClass(pTemp, &access_string[n + 1]);
            return pResult;
        case 0:
            pResult = pRoot->GetAttributeClassByCode(stringCodec_.Convert(access_string));
            return pResult;
        default: ;
        }
        n++;
    }
    return nullptr;
}

ATTRIBUTES * ATTRIBUTES::GetAttributeClassByCode(uint32_t name_code) const
{
    storm::TreeRead(this);
    return FirstWithCode(name_code);
}

ATTRIBUTES * ATTRIBUTES::VerifyAttributeClassByCode(uint32_t name_code)
{
    storm::TreeRead(this);
    ATTRIBUTES *pTemp = GetAttributeClassByCode(name_code);
    if (pTemp)
        return pTemp;
    return CreateAttribute(name_code, "");
}

ATTRIBUTES * ATTRIBUTES::CreateAttribute(uint32_t name_code, const char *attribute)
{
    ATTRIBUTES *attr = Append(name_code);
    if (attribute)
    {
        attr->value_ = attribute;
    }
    return attr;
}

size_t ATTRIBUTES::SetAttribute(uint32_t name_code, const char *attribute)
{
    if (const size_t n = ChildIndex(name_code); n != kNoIndex)
    {
        auto &value = attributes_[n]->value_;
        if (attribute ? !value || *value != attribute : value.has_value())
            storm::TreeChanged(this);
        if (attribute)
        {
            value = attribute;
        }
        else
        {
            value.reset();
        }
        return n;
    }

    ATTRIBUTES *attr = Append(name_code);
    if (attribute)
    {
        attr->value_ = attribute;
    }
    return attributes_.size() - 1;
}

size_t ATTRIBUTES::SetAttribute(uint32_t name_code, const std::string_view &attribute)
{
    if (const size_t n = ChildIndex(name_code); n != kNoIndex)
    {
        auto &value = attributes_[n]->value_;
        if (!value || *value != attribute)
            storm::TreeChanged(this);
        value = attribute;
        return n;
    }

    Append(name_code)->value_ = attribute;
    return attributes_.size() - 1;
}

uint32_t ATTRIBUTES::GetThisNameCode() const noexcept
{
    storm::TreeRead(this);
    return nameCode_;
}

void ATTRIBUTES::SetNameCode(uint32_t n) noexcept
{
    if (nameCode_ != n)
        storm::TreeChanged(this);
    nameCode_ = n;
    RenamedInParent();
}

ATTRIBUTES * ATTRIBUTES::Append(uint32_t name_code)
{
    storm::TreeChanged(this);
    codes_.push_back(name_code);
    if (index_)
    {
        // The new child is the last one: it enters the index unless an earlier child has its code.
        if (codes_.size() * 2 > index_->size())
            ChildrenReordered(); // full: rebuilt larger by the next lookups
        else if (IndexedChild(name_code) == kNoIndex)
        {
            const size_t mask = index_->size() - 1;
            size_t at = SlotOf(name_code, mask);
            while ((*index_)[at] != 0)
                at = (at + 1) & mask;
            (*index_)[at] = static_cast<uint32_t>(codes_.size());
        }
    }
    return attributes_.emplace_back(new ATTRIBUTES(stringCodec_, this, name_code)).get();
}

// The position of the first child named by `name_code`, or kNoIndex.
size_t ATTRIBUTES::ChildIndex(uint32_t name_code) const
{
    size_t n;
    if (codes_.size() < kIndexedChildren)
        n = IndexOfCode(codes_, name_code);
    else
    {
        if (!index_ && ++scansSinceChange_ >= kScansBeforeIndex)
            BuildIndex();
        n = index_ ? IndexedChild(name_code) : IndexOfCode(codes_, name_code);
    }
#ifdef STORM_VM_VERIFY
    storm::vm_verify::Count(index_ ? storm::vm_verify::Check::AttributeByIndex : storm::vm_verify::Check::AttributeByCode);
    size_t reference = 0;
    while (reference < attributes_.size() && attributes_[reference]->nameCode_ != name_code)
        reference++;
    if ((n == kNoIndex ? attributes_.size() : n) != reference || codes_.size() != attributes_.size())
        storm::vm_verify::Mismatch("attribute by code %u (%s): index %zu, the children say %zu of %zu", name_code,
                                   stringCodec_.Convert(name_code), n, reference, attributes_.size());
#endif
    return n;
}

size_t ATTRIBUTES::IndexedChild(uint32_t name_code) const noexcept
{
    const std::vector<uint32_t> &slots = *index_;
    const size_t mask = slots.size() - 1;
    for (size_t at = SlotOf(name_code, mask);; at = (at + 1) & mask)
    {
        const uint32_t slot = slots[at];
        if (slot == 0)
            return kNoIndex;
        if (codes_[slot - 1] == name_code)
            return slot - 1;
    }
}

// At most a quarter of the slots are taken, so appended children fit until the count doubles.
void ATTRIBUTES::BuildIndex() const
{
    size_t size = 64;
    while (size < codes_.size() * 4)
        size *= 2;
    index_ = std::make_unique<std::vector<uint32_t>>(size, 0u);
    const size_t mask = size - 1;
    for (size_t n = 0; n < codes_.size(); n++)
    {
        size_t at = SlotOf(codes_[n], mask);
        while ((*index_)[at] != 0 && codes_[(*index_)[at] - 1] != codes_[n])
            at = (at + 1) & mask;
        if ((*index_)[at] == 0) // else an earlier child has the code: the first one is the answer
            (*index_)[at] = static_cast<uint32_t>(n + 1);
    }
}

// A child was removed, renamed or moved: the positions in the index are stale.
void ATTRIBUTES::ChildrenReordered() noexcept
{
    index_.reset();
    scansSinceChange_ = 0;
}

ATTRIBUTES *ATTRIBUTES::FirstWithCode(uint32_t name_code) const
{
    const size_t n = ChildIndex(name_code);
    return n == kNoIndex ? nullptr : attributes_[n].get();
}

// A name no attribute was ever given has no code, and no child bears it.
ATTRIBUTES *ATTRIBUTES::FirstWithName(const std::string_view &name) const
{
    storm::NameLookup();
    const std::optional<uint32_t> code = stringCodec_.Find(name);
    ATTRIBUTES *const found = code ? FirstWithCode(*code) : nullptr;
#ifdef STORM_VM_VERIFY
    storm::vm_verify::Count(storm::vm_verify::Check::AttributeByName);
    ATTRIBUTES *reference = nullptr;
    for (const auto &attribute : attributes_)
        if (storm::iEquals(name, stringCodec_.Convert(attribute->nameCode_)))
        {
            reference = attribute.get();
            break;
        }
    if (found != reference)
        storm::vm_verify::Mismatch("attribute by name '%.*s': %p, the children's names say %p", static_cast<int>(name.size()),
                                   name.data(), static_cast<void *>(found), static_cast<void *>(reference));
#endif
    return found;
}

void ATTRIBUTES::RenamedInParent() noexcept
{
    if (!parent_)
        return;
    for (size_t n = 0; n < parent_->attributes_.size(); n++)
        if (parent_->attributes_[n].get() == this)
        {
            parent_->codes_[n] = nameCode_;
            parent_->ChildrenReordered();
            return;
        }
}

void ATTRIBUTES::SortByName()
{
    std::sort(std::begin(attributes_), std::end(attributes_),
              [](const std::unique_ptr<ATTRIBUTES> &lhs, const std::unique_ptr<ATTRIBUTES> &rhs) {
                  return strcmp(lhs->GetThisName(), rhs->GetThisName()) < 0;
              });
    for (size_t n = 0; n < attributes_.size(); n++)
        codes_[n] = attributes_[n]->nameCode_;
    ChildrenReordered();
}

ATTRIBUTES::ATTRIBUTES(VSTRING_CODEC &p) : ATTRIBUTES(p, nullptr, "root")
{
}

ATTRIBUTES::ATTRIBUTES(VSTRING_CODEC &string_codec, ATTRIBUTES *parent, const std::string_view &name)
    : ATTRIBUTES(string_codec, parent, string_codec.Convert(name.data()))
{
}

ATTRIBUTES::ATTRIBUTES(VSTRING_CODEC &string_codec, ATTRIBUTES *parent, const uint32_t name_code)
    : stringCodec_(string_codec), nameCode_(name_code), slot_(lifetimes().Acquire()), parent_(parent)
{
}

ATTRIBUTES ATTRIBUTES::Copy() const
{
    // A detached tree: no script state changes until it is assigned somewhere.
    ATTRIBUTES result(stringCodec_, nullptr, nameCode_);
    const auto copy = [](const auto &self, const ATTRIBUTES &from, ATTRIBUTES &to) -> void {
        to.value_ = from.value_;
        to.codes_ = from.codes_;
        to.attributes_.reserve(from.attributes_.size());
        for (const auto &attribute : from.attributes_)
            self(self, *attribute, *to.attributes_.emplace_back(new ATTRIBUTES(from.stringCodec_, &to, attribute->nameCode_)));
    };
    copy(copy, *this, result);
    return result;
}

void ATTRIBUTES::Release() const noexcept
{
    if (break_)
        stringCodec_.VariableChanged();
}
