// A call whose argument count differs from the function's. CheckCallArguments (compile time) and
// DropStrayStackValues (after the call) come verbatim from compiler.cpp.
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include <fmt/format.h>

#include "data.h"
#include "s_functab.h"
#include "s_stack.h"

#ifndef INVALID_SEGMENT_ID
#define INVALID_SEGMENT_ID 0xffffffff
#endif

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
        const auto found = std::find(names.begin(), names.end(), name);
        return found == names.end() ? std::nullopt : std::optional<uint32_t>(found - names.begin());
    }
    void VariableChanged() override {}
};

Codec codec;

struct TraceOnly
{
    void Trace(const char *, ...) { require(false, "unexpected DATA trace"); }
} core_internal;

struct ErrorLog
{
    std::vector<std::string> lines;
    template <typename... Args> void warn(fmt::format_string<Args...> format, Args &&...args)
    {
        lines.push_back(fmt::format(format, std::forward<Args>(args)...));
    }
};

constexpr uint32_t kSendMessage = 7; // the one variable-count internal function of the harness
} // namespace

class Harness final : public VIRTUAL_COMPILER
{
  public:
    S_STACK SStack;
    std::unique_ptr<ErrorLog> logError_ = std::make_unique<ErrorLog>();
    char DebugSourceFileName[260] = "dialogs\\russian\\Enc_Officer_dialog.c";
    uint32_t DebugSourceLine = 270;
    bool bDebugExpressionRun = false;

    Harness() { SStack.SetVCompiler(this); }
    VSTRING_CODEC *GetVSC() override { return &codec; }
    void SetError(const char *, ...) override { require(false, "unexpected script error"); }
    void FindErrorSource() {}
    bool IsIntFuncVarArgsNum(uint32_t code) { return code == kSendMessage; }
    void CheckCallArguments(const FuncInfo &fi, uint32_t func_code, uint32_t func_args);
    void DropStrayStackValues(uint32_t base, DATA *&result);
};

#include "method.inc"

namespace
{
FuncInfo function(const char *name, uint32_t segment_id, uint32_t offset, uint32_t arguments, uint32_t extern_arguments = 0)
{
    FuncInfo fi;
    fi.name = name;
    fi.segment_id = segment_id;
    fi.offset = offset;
    fi.arguments = arguments;
    fi.extern_arguments = extern_arguments;
    return fi;
}

size_t warnings(const FuncInfo &fi, uint32_t code, uint32_t args)
{
    Harness h;
    h.CheckCallArguments(fi, code, args);
    return h.logError_->lines.size();
}

void compileTime()
{
    Harness h;
    h.CheckCallArguments(function("RandPhraseSimple", 3, 120, 2), 400, 3);
    require(h.logError_->lines.size() == 1, "a script function called with 3 of 2 arguments is not reported");
    require(h.logError_->lines[0] == "COMPILE WARNING in dialogs\\russian\\Enc_Officer_dialog.c(271): function "
                                     "'RandPhraseSimple' declared with 2 arguments is called with 3",
            "report text");
    require(warnings(function("LinkRandPhrase", 3, 140, 3), 401, 2) == 1, "too few arguments not reported");
    require(warnings(function("LinkRandPhrase", 3, 140, 3), 401, 3) == 0, "a matching call reported");
    require(warnings(function("sti", INTERNAL_SEGMENT_ID, 40, 1), 40, 2) == 1, "internal function count not checked");
    require(warnings(function("SendMessage", INTERNAL_SEGMENT_ID, kSendMessage, 0), kSendMessage, 5) == 0,
            "a variable-count internal function reported");
    require(warnings(function("Native", IMPORTED_SEGMENT_ID, INVALID_FUNC_OFFSET, 2), 500, 3) == 0,
            "an imported function reported");
    require(warnings(function("Unloaded", INVALID_SEGMENT_ID, INVALID_FUNC_OFFSET, 2), 501, 3) == 0,
            "a function of an unloaded segment reported");
    require(warnings(function("Extern", 4, INVALID_FUNC_OFFSET, 0, 3), 502, 3) == 0, "a matching extern call reported");
    require(warnings(function("Extern", 4, INVALID_FUNC_OFFSET, 0, 3), 502, 2) == 1, "an extern mismatch not reported");
    Harness debug;
    debug.bDebugExpressionRun = true;
    debug.CheckCallArguments(function("RandPhraseSimple", 3, 120, 2), 400, 3);
    require(debug.logError_->lines.empty(), "a debugger expression reported");
}

DATA *push(Harness &h, const char *value)
{
    DATA *v = h.SStack.Push();
    v->Set(value);
    return v;
}

std::string text(DATA *v)
{
    const char *s = nullptr;
    require(v->GetType() == VAR_STRING && v->Get(s), "not a string");
    return s;
}

// `"a" + RandPhraseSimple(x, y, z)`: the operand "a", three arguments pushed (check_sp = 1); the callee's frame took
// y and z and left its result, x stays under it.
void strayArgument()
{
    Harness h;
    push(h, "a");
    const uint32_t check_sp = h.SStack.GetDataNum();
    push(h, "x");
    DATA *result = push(h, "z");
    h.DropStrayStackValues(check_sp, result);
    require(h.SStack.GetDataNum() == 2, "stray argument not dropped");
    require(result == h.SStack.Read(), "result is not on top");
    require(text(result) == "z", "result changed");
    require(text(h.SStack.Read(0, 0)) == "a", "the caller's operand changed");
}

// A reference result keeps referring to its variable.
void referenceResult()
{
    Harness h;
    DATA object;
    object.SetVCompiler(&h);
    object.SetType(VAR_OBJECT);
    object.GetAClass()->SetAttribute("id", "Blaze");
    push(h, "a");
    const uint32_t check_sp = h.SStack.GetDataNum();
    push(h, "x");
    DATA *result = h.SStack.Push();
    result->SetType(VAR_REFERENCE);
    result->SetReference(&object);
    h.DropStrayStackValues(check_sp, result);
    require(h.SStack.GetDataNum() == 2 && result == h.SStack.Read(), "reference result not moved");
    require(result->GetType() == VAR_REFERENCE && result->GetVarPointer() == &object, "reference lost");
    auto *id = object.GetAClass()->GetAttributeClass("id");
    require(id && id->GetValue() == "Blaze", "referenced object changed");
}

void noResult()
{
    Harness h;
    push(h, "a");
    const uint32_t check_sp = h.SStack.GetDataNum();
    push(h, "x");
    DATA *result = nullptr;
    h.DropStrayStackValues(check_sp, result);
    require(h.SStack.GetDataNum() == 1 && result == nullptr, "void call: stray argument not dropped");
}

// Balanced, or short of values (too few arguments took the caller's): left as it is.
void untouched()
{
    Harness h;
    push(h, "a");
    push(h, "b");
    DATA *result = push(h, "r");
    h.DropStrayStackValues(2, result);
    require(h.SStack.GetDataNum() == 3 && result == h.SStack.Read() && text(result) == "r", "balanced call changed");
    h.DropStrayStackValues(3, result);
    require(h.SStack.GetDataNum() == 3 && text(h.SStack.Read(0, 1)) == "b", "short stack changed");
}
} // namespace

int main()
{
    compileTime();
    strayArgument();
    referenceResult();
    noResult();
    untouched();
    std::cout << "PASS calls with a wrong argument count: reported at compile time, the stack balanced after\n";
}
