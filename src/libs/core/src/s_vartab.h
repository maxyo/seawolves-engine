#pragma once

#include "data.h"
#include "fnv_hash.hpp"
#include "string_compare.hpp"
#include <memory>
#include <string_view>
#include <unordered_map>
#include <vector>

// when segment_id is INVALID_SEGMENT_ID, variable segment is unloaded
// and variable value and type undefined

#define INVALID_VAR_CODE 0xffffffff
#define INVALID_SEGMENT_ID 0xffffffff

// The name tables' hash and comparison: ASCII case folded, and transparent, so a lookup by a C string or a view
// builds no std::string (the engine asks for globals by name every frame: CoreImpl::GetScriptVariable).
struct ScriptNameHash
{
    using is_transparent = void;
    size_t operator()(std::string_view name) const noexcept
    {
        uint64_t hash = storm::hash::kFnvOffset;
        for (const char c : name)
            hash = storm::hash::fnvRound(hash, storm::FoldCase(c));
        return static_cast<size_t>(hash ^ (hash >> 32));
    }
};

struct ScriptNameEqual
{
    using is_transparent = void;
    bool operator()(std::string_view left, std::string_view right) const noexcept
    {
        return storm::iEquals(left, right);
    }
};

struct VarInfo
{
    VarInfo();
    VarInfo(const VarInfo &other);
    VarInfo(VarInfo &&other) = default;
    VarInfo &operator=(const VarInfo &other);

    bool IsArray() const
    {
        return elements > 1;
    }

    std::string name;
    S_TOKEN_TYPE type;
    std::unique_ptr<DATA> value;
    size_t elements;
    uint32_t segment_id;
};

class VarTable
{
  public:
    VarTable() = default;
    ~VarTable();

    size_t GetVarNum() const // get var table size
    {
        return vars_.size();
    };

    // add var to table, returns var index
    size_t AddVar(const VarInfo &vi);
    // get var by index, returns non-null ptr if var is registered and loaded
    const VarInfo *GetVar(size_t var_index) const;
    // get var by index, returns non-null ptr if var is registered
    const VarInfo *GetVarX(size_t var_index) const;
    // invalidate all segment's vars
    void InvalidateBySegmentID(uint32_t segment_id);
    size_t FindVar(std::string_view var_name) const;
    // set var's array size, returns true if successful
    bool SetElementsNum(size_t var_index, size_t elements_num);
    void Release();

    void SetVCompiler(VIRTUAL_COMPILER *vc)
    {
        vc_ = vc;
    }

  private:
    std::vector<VarInfo> vars_;
    std::unordered_map<std::string, size_t, ScriptNameHash, ScriptNameEqual> hash_table_; // name to index mapping
    VIRTUAL_COMPILER *vc_;
};
