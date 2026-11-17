// A script aref whose attribute is deleted behind it (see README.md): every use is a reported script error and
// reads no freed memory (ASan); live arefs behave as before.
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "data.h"
#include "s_postevents.h"

namespace
{
void require(bool condition, const char *what)
{
    if (!condition)
    {
        std::cerr << "FAIL: " << what << '\n';
        std::exit(1);
    }
}

class Codec final : public VSTRING_CODEC
{
    std::vector<std::string> names;

  public:
    uint32_t GetNum() override { return names.size(); }
    uint32_t Convert(const char *name) override
    {
        if (!name)
            return 0xffffffff;
        auto found = std::find(names.begin(), names.end(), name);
        if (found != names.end())
            return found - names.begin();
        names.emplace_back(name);
        return names.size() - 1;
    }
    uint32_t Convert(const char *name, int32_t length) override { return Convert(std::string(name, length).c_str()); }
    const char *Convert(uint32_t code) override { return names.at(code).c_str(); }
    std::optional<uint32_t> Find(std::string_view name) const override
    {
        auto found = std::find(names.begin(), names.end(), name);
        return found == names.end() ? std::nullopt : std::optional<uint32_t>(found - names.begin());
    }
    void VariableChanged() override {}
};

Codec codec; // outlives every DATA and ATTRIBUTES of the test

struct TraceOnly
{
    void Trace(const char *, ...) { require(false, "unexpected DATA trace"); }
} core_internal;

class Compiler final : public VIRTUAL_COMPILER
{
  public:
    std::vector<std::string> errors;
    VSTRING_CODEC *GetVSC() override { return &codec; }
    void SetError(const char *format, ...) override
    {
        char text[512];
        va_list args;
        va_start(args, format);
        vsnprintf(text, sizeof(text), format, args);
        va_end(args);
        errors.emplace_back(text);
    }
    size_t Dangling()
    {
        const auto count = std::count_if(errors.begin(), errors.end(), [](const std::string &line) {
            return line.find("deleted attribute") != std::string::npos;
        });
        errors.clear();
        return count;
    }
};

// COMPILER's posting of events (method.inc: verbatim from compiler.cpp).
class Posting
{
    Compiler &compiler_;

  public:
    POSTEVENTS_LIST EventMsg;
    explicit Posting(Compiler &compiler) : compiler_(compiler) {}
    template <typename... Args> void SetError(const char *format, Args... args) { compiler_.SetError(format, args...); }
    void AddPostEvent(S_EVENTMSG *pEM);
    void ForgetDeletedAttributes(S_EVENTMSG *pEM);
};

#include "method.inc"

DATA variable(Compiler &compiler, S_TOKEN_TYPE type)
{
    DATA data(type);
    data.SetVCompiler(&compiler);
    return data;
}

ATTRIBUTES *at(DATA &object, const char *path)
{
    return object.GetAClass()->CreateSubAClass(object.GetAClass(), path);
}

// reload.c: `reload_locator_ref` names the quest reload's fake locator, which DoDeleteFakeLocation deletes
// (DeleteAttribute(&Locations[i], "reload.fake1")) before ReloadEndFade reads the aref.
void deletedBehindAref()
{
    Compiler compiler;
    DATA location = variable(compiler, VAR_OBJECT);
    at(location, "reload.fake1.go")->SetValue("BOARDING_MEDIUM_DECK");
    at(location, "reload.l1.go")->SetValue("Town");
    DATA locator = variable(compiler, VAR_AREFERENCE);
    locator.SetAReference(at(location, "reload.fake1"));
    DATA door = variable(compiler, VAR_AREFERENCE);
    door.SetAReference(at(location, "reload.l1"));
    DATA stackCopy = variable(compiler, UNKNOWN); // an argument `aref reload_data` of ReloadToLocation
    stackCopy.Copy(&locator);
    require(locator.GetAClass() == at(location, "reload.fake1") && compiler.errors.empty(), "a live aref failed");

    ATTRIBUTES *root = location.GetAClass();
    require(root->DeleteAttributeClassX(at(location, "reload.fake1")), "fixture: delete failed");
    // The allocator hands the memory on: the slot and probably the address of the deleted attribute are reused.
    std::vector<std::unique_ptr<DATA>> reuse;
    for (int i = 0; i < 64; ++i)
    {
        reuse.push_back(std::make_unique<DATA>(variable(compiler, VAR_OBJECT)));
        at(*reuse.back(), "a.b.c")->SetValue("other");
    }

    require(locator.AReferenceDangling() && stackCopy.AReferenceDangling(), "the deleted attribute's aref is not dangling");
    require(locator.GetAClass() == nullptr && compiler.Dangling() == 1, "a dangling aref was used without an error");
    require(stackCopy.GetAClass() == nullptr && compiler.Dangling() == 1, "a copied dangling aref was used without an error");
    const char *value = nullptr;
    require(!locator.Get("go", value) && compiler.Dangling() == 1, "reading through a dangling aref was not reported");
    locator.Set("go", "x");
    require(compiler.Dangling() == 1, "writing through a dangling aref was not reported");
    DATA text = variable(compiler, VAR_STRING);
    text.Set(std::string("to "));
    text.Plus(&locator);
    require(compiler.Dangling() == 1, "a dangling aref added to a string was not reported");

    DATA again = variable(compiler, UNKNOWN);
    again.Copy(&locator);
    require(again.AReferenceDangling() && compiler.errors.empty(), "copying a dangling aref is not a plain copy");
    std::vector<DATA> grown{again}; // DATA copy (a growing stack)
    grown.reserve(16);
    require(grown.front().AReferenceDangling() && compiler.errors.empty(), "a stack copy of a dangling aref changed");

    require(door.GetAClass() == at(location, "reload.l1") && door.GetAClass()->GetAttributeClass("go")->GetValue() == "Town",
            "a live aref of the same object was affected");
    locator.SetAReference(at(location, "reload.l1"));
    require(!locator.AReferenceDangling() && locator.GetAClass() == door.GetAClass() && compiler.errors.empty(),
            "a dangling aref set again stays broken");
}

// Other ways an attribute goes: a branch replaced by a copy of another object (`obj = other`), an object
// destroyed (a local object at the end of its function).
void replacedAndDestroyed()
{
    Compiler compiler;
    DATA hero = variable(compiler, VAR_OBJECT);
    DATA other = variable(compiler, VAR_OBJECT);
    at(hero, "ship.cannons")->SetValue("16");
    at(other, "ship.cannons")->SetValue("24");
    DATA cannons = variable(compiler, VAR_AREFERENCE);
    cannons.SetAReference(at(hero, "ship.cannons"));
    DATA root = variable(compiler, VAR_AREFERENCE);
    root.SetAReference(hero.GetAClass());
    hero.Copy(&other);
    require(cannons.AReferenceDangling() && cannons.GetAClass() == nullptr && compiler.Dangling() == 1,
            "an aref into a replaced branch is not dangling");
    require(root.GetAClass() == hero.GetAClass() && root.GetAClass()->GetAttributeClass("ship")->GetAttributeClass("cannons")->GetValue() == "24",
            "the aref of the object kept by the copy was affected");

    DATA local = variable(compiler, VAR_AREFERENCE);
    {
        DATA object = variable(compiler, VAR_OBJECT);
        local.SetAReference(at(object, "x"));
        require(local.GetAClass() != nullptr, "fixture: local aref");
    }
    require(local.GetAClass() == nullptr && compiler.Dangling() == 1, "an aref of a destroyed object is not dangling");
}

// A posted event keeps its attribute arguments until its time (GetEventData makes them arefs).
void postedEvent()
{
    Compiler compiler;
    DATA character = variable(compiler, VAR_OBJECT);
    at(character, "quest.timer")->SetValue("1");
    Posting posting(compiler);
    auto *message = new MESSAGE;
    message->Reset("lal", 3, at(character, "quest.timer"), 7);
    posting.AddPostEvent(new S_EVENTMSG("Timer", message, 1000));
    auto *live = new MESSAGE;
    live->Reset("a", at(character, "quest"));
    posting.AddPostEvent(new S_EVENTMSG("Live", live, 1000));
    require(character.GetAClass()->DeleteAttributeClassX(at(character, "quest.timer")), "fixture: delete failed");

    posting.ForgetDeletedAttributes(posting.EventMsg.Read(0));
    require(compiler.Dangling() == 1, "a deleted event attribute was not reported");
    message->Move2Start();
    require(message->Long() == 3 && message->AttributePointer() == nullptr && message->Long() == 7,
            "a deleted event attribute was handed on");
    posting.ForgetDeletedAttributes(posting.EventMsg.Read(1));
    live->Move2Start();
    require(compiler.errors.empty() && live->AttributePointer() == at(character, "quest"), "a live event attribute was lost");
}
} // namespace

int main()
{
    deletedBehindAref();
    replacedAndDestroyed();
    postedEvent();
    std::cout << "PASS a deleted attribute behind an aref is reported on every use and never read\n";
}
