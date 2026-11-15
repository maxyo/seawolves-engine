#pragma once
// Script variables the interface model's collectors read directly, by name (an integer, a text, or
// which object a reference points at). Attributes are not among them: the script state generation
// watches the attribute trees a collection read (core script_state.hpp). XINTERFACE notes every
// such read of a collection with what it gave and reads them again each frame: the model is
// collected again when one reads differently (xinterface.cpp MobileModelDue).
#include <algorithm>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace storm::mobile
{
class VariableReads
{
  public:
    enum class Kind : char
    {
        Integer = 'i',
        Text = 't',
        Object = 'o', // the attributes object of an object variable, or of a reference's target
    };
    struct Read
    {
        std::string name;
        Kind kind;
        std::optional<std::string> value; // nullopt: no such variable, or nothing it points at
        bool operator==(const Read &) const = default;
    };

    // A collection starts: its reads replace the last collection's when it ends.
    void Begin()
    {
        noting_ = true;
        next_.clear();
    }
    void End()
    {
        noting_ = false;
        reads_.swap(next_);
        next_.clear();
    }
    // What a collector read (outside a collection: nothing is noted, e.g. an action's check).
    void Note(const char *name, Kind kind, const std::optional<std::string> &value)
    {
        if (!noting_ || std::any_of(next_.begin(), next_.end(), [&](const Read &read) { return read.kind == kind && read.name == name; }))
            return;
        next_.push_back({name, kind, value});
    }
    // True when a variable the last collection read reads differently now. `read(name, kind)`
    // gives what it reads now.
    template <typename Reader> bool Changed(Reader &&read) const
    {
        return std::any_of(reads_.begin(), reads_.end(), [&](const Read &noted) { return read(noted.name.c_str(), noted.kind) != noted.value; });
    }
    size_t Count() const
    {
        return reads_.size();
    }

  private:
    bool noting_ = false;
    std::vector<Read> reads_, next_;
};
} // namespace storm::mobile
