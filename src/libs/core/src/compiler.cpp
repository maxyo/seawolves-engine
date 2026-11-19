#include "script_state.hpp"
#include "compiler.h"
#ifdef __ANDROID__
#include "platform/android_land_commands.hpp"
#include "platform/android_land_diagnostics.hpp"
#include "platform/android_script_errors.hpp"
#endif

#include <chrono>
#include <cstdio>
#include <stdexcept>

#include <zlib.h>

#include "core_impl.h"
#include "debug-trap.h"
#include "fs.h"
#include "logging.hpp"
#include "number_text.hpp"
#include "platform/path_fingerprint.hpp"
#include "save_file.hpp"
#include "save_references.hpp"
#include "save_state.hpp"
#include "script_cache.h"
#include "script_cache_files.hpp"
#include "storm_assert.h"
#include "watermark.hpp"

#include <SDL_timer.h>
#include <unordered_map>

#define SKIP_COMMENT_TRACING
#define TRACE_OFF
#define INVALID_SEGMENT_INDEX 0xffffffff
#ifndef INVALID_ARRAY_INDEX
#define INVALID_ARRAY_INDEX 0xffffffff
#endif
#define INVALID_OFFSET 0xffffffff
#define DSL_INI_VALUE 0
#define SBUPDATE 4
#define DEF_COMPILE_EXPRESSIONS

namespace
{
enum CacheMode : int
{
    kCacheDisabled = 0,             // Disable script caching
    kCacheEnabled = 1,              // Enable script caching and integrity checks
    kCacheEnabledNoRuntimeCheck = 2 // Enable script caching; only check integrity once - should be used if no
                                    // changes(e.g. in the interface scripts) are expected during the session
};

auto GetCacheFolder()
{
    static std::filesystem::path cache_folder = fs::GetCachePath() / "Cache";
    return cache_folder;
}

// Everything that can make cached bytecode stale besides the scripts
// themselves: the engine build (token numbering, internal function table and
// bytecode layout are not stable across builds), the cache layout, and which
// game directory the scripts came from (the cache folder is shared).
constexpr uint64_t kCacheFormatVersion = 3;

uint64_t CalculateCacheKey(const char *program_directory)
{
    const std::string resolved = fio->ConvertPathResource(program_directory ? program_directory : "");
    std::error_code ec;
    const auto absolute = std::filesystem::absolute(std::filesystem::u8path(resolved), ec);
    uint64_t key = storm::hash::fnv1a(STORM_BUILD_WATERMARK_STRING " " __DATE__ " " __TIME__);
    key = storm::fingerprint::fnv1a(kCacheFormatVersion, key);
    key = storm::fingerprint::fnv1a(static_cast<uint64_t>(sizeof(size_t)), key);
    key = storm::hash::fnv1a(ec ? resolved : absolute.lexically_normal().generic_string(), key);
    return storm::fingerprint::mix(key) + fio->GetPathFingerprint(std::filesystem::u8path(resolved));
}

} // namespace

extern INTFUNCDESC IntFuncTable[];

using std::chrono::duration_cast;
using std::chrono::milliseconds;
using std::chrono::system_clock;

COMPILER::COMPILER()
    : bBreakOnError(false), pRunCodeBase(nullptr), CompilerStage(CS_SYSTEM), pEventMessage(nullptr), SegmentsNum(0),
      InstructionPointer(0), pBuffer(nullptr), ProgramDirectory(nullptr), bCompleted(false), bEntityUpdate(true),
      pDebExpBuffer(nullptr), nDebExpBufferSize(0), pRun_fi(nullptr),
      bFirstRun(true), bWriteCodeFile(false),
      bDebugInfo(false), DebugSourceLine(0), pCompileTokenTempBuffer(nullptr), bDebugExpressionRun(false),
      bTraceMode(true), nDebugTraceLineCode(0), nIOBufferSize(0), pIOBuffer(nullptr), rAP(nullptr),
      script_cache_mode_(kCacheDisabled)

{
    LabelTable.SetStringDataSize(sizeof(uint32_t));
    LabelUpdateTable.SetStringDataSize(sizeof(DOUBLE_DWORD));

    //    FuncTab.KeepNameMode(true);    // true for displaying internal functions names
    InitInternalFunctions();

    strcpy_s(DebugSourceFileName, "<no debug information>");

    SStack.SetVCompiler(this);
    VarTab.SetVCompiler(this);
    srand(duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count());

    DebugTraceFileName[0] = 0;

    rAX.SetVCompiler(this);
    rBX.SetVCompiler(this);
    ExpressionResult.SetVCompiler(this);


    using storm::logging::getOrCreateLogger;
    logTrace_ = getOrCreateLogger("compile");
    logError_ = getOrCreateLogger("error");
    logStack_ = getOrCreateLogger("script_stack");
    logStack_->set_pattern("%v");
}

COMPILER::~COMPILER()
{
    Release();
}

void COMPILER::Release()
{
#ifdef __ANDROID__
    ResetAndroidUiAdapter();
#endif
    for (uint32_t n = 0; n < SegmentsNum; n++)
    {
        delete[] SegmentTable[n].pData;
        delete[] SegmentTable[n].pCode;
        if (SegmentTable[n].Files_list)
        {
            SegmentTable[n].Files_list->Release();
            delete SegmentTable[n].Files_list;
        }
        SegmentTable[n].Files_list = nullptr;
    }
    SegmentTable.clear();
    SegmentsNum = 0;
    LabelTable.Release();
    LabelUpdateTable.Release();
    Token.Reset();
    delete[] ProgramDirectory;
    ProgramDirectory = nullptr;
    RunningSegmentID = INVALID_SEGMENT_INDEX;
    delete[] pCompileTokenTempBuffer;
    pCompileTokenTempBuffer = nullptr;

    FuncTab.Release();
    VarTab.Release();
    DefTab.Release();
    SStack.Release();
    EventTab.Release();
    EventMsg.Release();
    SCodec.Release();
    LibriaryFuncs.clear();

    delete[] pDebExpBuffer;
    pDebExpBuffer = nullptr;
    nDebExpBufferSize = 0;


    rAX.Release();
    rBX.Release();
    ExpressionResult.Release();
}

void COMPILER::SetProgramDirectory(const char *dir_name)
{
    delete[] ProgramDirectory;
    ProgramDirectory = nullptr;
    if (dir_name)
    {
        const auto len = strlen(dir_name) + strlen("\\") + 1;
        ProgramDirectory = new char[len];
        strcpy_s(ProgramDirectory, len, dir_name);
        strcat_s(ProgramDirectory, len, "\\");
    }
}

char *COMPILER::LoadFile(const char *file_name, uint32_t &file_size, bool bFullPath)
{
    const char *fName;
    char buffer[MAX_PATH];

    if (!bFullPath)
    {
        std::string EngineDir = "storm-engine\\";
        if (strncmp(file_name, EngineDir.c_str(), EngineDir.length()) == 0)
        {
            std::string ExePath = fio->_GetExecutableDirectory() + "resource\\shared\\";
            strcpy_s(buffer, ExePath.c_str());
            strcat_s(buffer, file_name + EngineDir.length());
        }
        else if (ProgramDirectory)
        {
            strcpy_s(buffer, ProgramDirectory);
            strcat_s(buffer, file_name);
        }
        else
        {
            strcpy_s(buffer, file_name);
        }
    }

    file_size = 0;
    if (bFullPath)
    {
        // f expand

        fName = file_name;
    }
    else
    {
        fName = &buffer[0];
    }

    if (const auto *memory = fio->MemoryFile(fName))
    {
        auto *const pData = new char[memory->size() + 1];
        memcpy(pData, memory->data(), memory->size());
        pData[memory->size()] = 0;
        file_size = static_cast<uint32_t>(memory->size());
        return pData;
    }
    auto fileS = fio->_CreateFile(fName, std::ios::binary | std::ios::in);
    if (!fileS.is_open())
    {
        return nullptr;
    }
    const auto fsize = fio->_GetFileSize(fName);

    auto *const pData = static_cast<char *>(new char[fsize + 1]);
    if (!fio->_ReadFile(fileS, pData, fsize))
    {
        delete[] pData;
        fio->_CloseFile(fileS);
        return nullptr;
    }
    fio->_CloseFile(fileS);
    file_size = fsize;
    pData[fsize] = 0;
    return pData;
}

void COMPILER::DTrace(const char *data_PTR, ...)
{
#ifdef DTRACEOFF
    return;
#endif
    if (data_PTR == nullptr)
        return;
    char LogBuffer[4096];
    va_list args;
    va_start(args, data_PTR);
    vsnprintf(LogBuffer, sizeof(LogBuffer) - 4, data_PTR, args);
    va_end(args);
    logTrace_->trace(LogBuffer);
}

// append one block of code to another
// return new pointer

bool COMPILER::AppendProgram(char *&pBase_program, uint32_t &Base_program_size, const char *pAppend_program,
                             uint32_t &Append_program_size, bool bAddLinefeed)
{
    const auto offset = Base_program_size;
    if (pAppend_program == nullptr)
        return false;
    if (bAddLinefeed)
    {
        // terminating zero, +1 for 0xd +1 for 0xa
        auto *const newPtr = new char[Base_program_size + Append_program_size + 3];
        memcpy(newPtr, pBase_program, Base_program_size);
        delete[] pBase_program;
        pBase_program = newPtr;
        Base_program_size = Base_program_size + Append_program_size + 2;
        memcpy(&pBase_program[offset], pAppend_program, Append_program_size);
        pBase_program[Base_program_size - 2] = 0xd; // code blocks separator to prevent data merging
        pBase_program[Base_program_size - 1] = 0xa; // code blocks separator to prevent data merging
        pBase_program[Base_program_size] = 0;       // terminating zero
        delete[] pAppend_program;
        Append_program_size += 2;
        return true;
    }
    // zero, +1 for ';'
    auto *const newPtr = new char[Base_program_size + Append_program_size + 2];
    memcpy(newPtr, pBase_program, Base_program_size);
    delete[] pBase_program;
    pBase_program = newPtr;
    Base_program_size = Base_program_size + Append_program_size + 1;
    memcpy(&pBase_program[offset], pAppend_program, Append_program_size);
    pBase_program[Base_program_size - 1] = ';'; // code blocks separator to prevent data merging
    pBase_program[Base_program_size] = 0;       // terminating zero
    delete[] pAppend_program;
    Append_program_size += 1;
    return true;
}

void COMPILER::SetError(const char *data_PTR, ...)
{
    if (bDebugExpressionRun)
        return;
    if (data_PTR == nullptr)
        return;
#ifdef __ANDROID__
    ++storm::android::scriptErrorEpoch;
#endif
    char LogBuffer[BUFSIZ];
    char ErrorBuffer[MAX_PATH + BUFSIZ];
    va_list args;
    va_start(args, data_PTR);
    vsnprintf(LogBuffer, sizeof(LogBuffer) - 4, data_PTR, args);
    uint32_t bytes;
    FindErrorSource();

    switch (CompilerStage)
    {
    case CS_SYSTEM:
        sprintf_s(ErrorBuffer, "ERROR in %s(%d): %s", DebugSourceFileName, DebugSourceLine + 1, LogBuffer);
        break;
    case CS_COMPILATION:
        sprintf_s(ErrorBuffer, "COMPILE ERROR in %s(%d): %s", DebugSourceFileName, DebugSourceLine + 1, LogBuffer);
        break;
    case CS_RUNTIME:
        sprintf_s(ErrorBuffer, "RUNTIME ERROR in %s(%d): %s", DebugSourceFileName, DebugSourceLine + 1, LogBuffer);
        break;
    }
    va_end(args);

    logError_->error(ErrorBuffer);

}

void COMPILER::SetWarning(const char *data_PTR, ...)
{
#ifdef WARNINGS_OFF
    return;
#endif
    if (bDebugExpressionRun)
        return;
    char LogBuffer[BUFSIZ];
    char ErrorBuffer[MAX_PATH + BUFSIZ];
    if (data_PTR == nullptr)
        return;
    va_list args;
    va_start(args, data_PTR);
    vsnprintf(LogBuffer, sizeof(LogBuffer) - 4, data_PTR, args);
    uint32_t bytes;
    FindErrorSource();

    sprintf_s(ErrorBuffer, "WARNING in %s(%d): %s", DebugSourceFileName, DebugSourceLine + 1, LogBuffer);
    va_end(args);

    logTrace_->warn(ErrorBuffer);
}

// The statement form of a call refuses a wrong argument count (CompileBlock, `doesnt accept`); a call inside an
// expression was never checked (also in the original engine), and the game's scripts have such calls, so they are
// reported and compiled as before. At run time a script function then takes its last arguments and the call ends
// with a stack error. Functions of unloaded segments (their count is not known) and imported ones are not checked.
void COMPILER::CheckCallArguments(const FuncInfo &fi, uint32_t func_code, uint32_t func_args)
{
    uint32_t declared = fi.arguments;
    if (fi.segment_id == IMPORTED_SEGMENT_ID)
        return;
    if (fi.offset == INVALID_FUNC_OFFSET)
    {
        // an unloaded segment's function has INVALID_SEGMENT_ID (the value of INTERNAL_SEGMENT_ID, whose functions
        // have their table index as offset); a segment's extern declaration has the segment's id
        if (fi.segment_id == INVALID_SEGMENT_ID)
            return;
        declared = fi.extern_arguments;
    }
    else if (fi.segment_id == INTERNAL_SEGMENT_ID && IsIntFuncVarArgsNum(func_code))
        return;
    if (declared == func_args || bDebugExpressionRun)
        return;
    FindErrorSource();
    logError_->warn("COMPILE WARNING in {}({}): function '{}' declared with {} arguments is called with {}",
                    DebugSourceFileName, DebugSourceLine + 1, fi.name, declared, func_args);
}

void COMPILER::LoadPreprocess()
{
    auto engine_ini = fio->OpenIniFile(core_internal.EngineIniFileName());
    if (engine_ini)
    {
        if (engine_ini->GetInt("script", "debuginfo", 0) == 0)
        {
            bDebugInfo = false;
        }
        else
        {
            bDebugInfo = true;
        }
        if (engine_ini->GetInt("script", "codefiles", 0) == 0)
            bWriteCodeFile = false;
        else
            bWriteCodeFile = true;

        // On by default: game scripts do not change while the game runs (see
        // tools/android/README.md, "Compiled script cache").
        script_cache_mode_ = engine_ini->GetInt("script", "cache_mode", kCacheEnabledNoRuntimeCheck);
        if (script_cache_mode_ < kCacheDisabled || script_cache_mode_ > kCacheEnabledNoRuntimeCheck)
        {
            spdlog::warn("engine.ini [script] cache_mode = {} is not 0, 1 or 2; the script cache is off",
                         script_cache_mode_);
            script_cache_mode_ = kCacheDisabled;
        }

    }

}

bool COMPILER::CreateProgram(const char *file_name)
{
    LoadPreprocess();
    const bool bRes = BC_LoadSegment(file_name);

    return bRes;
}

#ifdef __ANDROID__
void COMPILER::ResetAndroidUiAdapter()
{
    androidUiAdapterAttempted_ = false;
    // Script tables and static events are about to disappear. Revoke only our
    // Android leases/tokens; missing presentation support must not close stock.
    storm::android::landCommands.releaseUnavailableSource(storm::android::LandCommandSource::Missing);
    storm::android::traceLandAdapter(0, "script-reset");
}
void COMPILER::LoadAndroidUiAdapter()
{
    // Called after Main/OnLoad return, never inside a frame or an event handler.
    // The app ships the entry point (mobile-ui/PROGRAM/interface/android_ui_bootstrap.c): without it the
    // portable UI has no script side, so a missing or uncompiled bootstrap stops the start with the fatal
    // dialog (the frame loop's ReportFatalError) instead of a game without its interface.
    if (androidUiAdapterAttempted_) return;
    androidUiAdapterAttempted_ = true;
    constexpr const char *segment = "interface/android_ui_bootstrap.c";
    if (BC_SegmentIsLoaded(segment)) {
        storm::android::traceLandAdapter(0, "already-loaded"); return;
    }
    const std::string path = std::string(ProgramDirectory ? ProgramDirectory : "") + segment;
    if (!fio->_FileOrDirectoryExists(path.c_str())) {
        storm::android::traceLandAdapter(0, "missing-entrypoint");
        throw std::runtime_error("The game's interface scripts are incomplete: " + path +
                                 " is missing. Reinstall the game.");
    }
    if (!BC_LoadSegment(segment)) {
        storm::android::traceLandAdapter(0, "compile-failed");
        throw std::runtime_error("The game's interface scripts do not compile: " + path +
                                 " (see the compile log). Reinstall the game.");
    }
    storm::android::traceLandAdapter(0, "loaded");
}
#endif

bool COMPILER::Run()
{
    pRun_fi = nullptr;
    const uint32_t function_code = FuncTab.FindFunc("Main");
    DATA *pResult;
    bFirstRun = false;
    const bool executed = BC_Execute(function_code, pResult);
    pRun_fi = nullptr;
#ifdef __ANDROID__
    if (executed) LoadAndroidUiAdapter();
    else storm::android::traceLandAdapter(0, "main-execution-failed");
#else
    (void)executed;
#endif
#ifdef STORM_SCENE_SWEEP
    LoadSceneSweep();
#endif



    return true;
}

void COMPILER::FindErrorSource()
{
    const uint32_t segment_index = GetSegmentIndex(RunningSegmentID);
    if (segment_index == INVALID_SEGMENT_INDEX)
        return;

    uint32_t index;
    uint32_t token_data_size;
    uint32_t file_line_offset;
    uint32_t file_name_size;
    S_TOKEN_TYPE Token_type;

    uint32_t ip = 0;
    DebugSourceLine = 0;
    file_line_offset = 0;
    char *pCodeBase = SegmentTable[segment_index].pCode;
    do
    {
        Token_type = static_cast<S_TOKEN_TYPE>(pCodeBase[ip]);
        ip++;
        if (static_cast<uint8_t>(pCodeBase[ip]) < 0xff)
        {
            token_data_size = static_cast<uint8_t>(pCodeBase[ip]);
            ip++;
        }
        else
        {
            ip++;
            memcpy(&token_data_size, &pCodeBase[ip], sizeof(uint32_t));
            ip += sizeof(uint32_t);
        }

        if (ip >= InstructionPointer)
        {
            return;
        }
        switch (Token_type)
        {
        case DEBUG_LINE_CODE:
            memcpy(&DebugSourceLine, &pCodeBase[ip], sizeof(uint32_t));
            break;
        case DEBUG_FILE_NAME:
            index = ip;
            memcpy(&file_line_offset, &pCodeBase[index], sizeof(uint32_t));
            index += sizeof(uint32_t);
            memcpy(&file_name_size, &pCodeBase[index], sizeof(uint32_t));
            index += sizeof(uint32_t);
            memcpy(DebugSourceFileName, &pCodeBase[index], file_name_size);
            index += file_name_size;
            DebugSourceFileName[file_name_size] = 0;
            DebugSourceLine = DSL_INI_VALUE;
            break;
        }
        ip += token_data_size; // step to next instruction
    } while (Token_type != END_OF_PROGRAMM);
}

void COMPILER::SetEventHandler(const char *event_name, const char *func_name, int32_t flag, bool bStatic)
{
    FuncInfo fi;

    if (event_name == nullptr)
    {
        SetError("Invalid event name for SetEventHandler");
        return;
    }
    if (func_name == nullptr)
    {
        SetError("Invalid function name for SetEventHandler");
        return;
    }

    const uint32_t func_code = FuncTab.FindFunc(func_name);
    if (func_code == INVALID_FUNC_CODE)
    {
        SetError("Invalid function code in SetEventHandler");
        return;
    }

    if (!FuncTab.GetFunc(fi, func_code))
    {
        SetError("func not found eror");
        return;
    }

    EventTab.AddEventHandler(event_name, func_code, fi.segment_id, flag, bStatic);
}

void COMPILER::DelEventHandler(const char *event_name, const char *func_name)
{
    if (event_name == nullptr)
    {
        SetError("Bad event name");
        return;
    }

    if (func_name == nullptr)
    {
        SetError("Bad func name");
        return;
    }

    const uint32_t func_code = FuncTab.FindFunc(func_name);
    if (func_code == INVALID_FUNC_CODE)
    {
        SetError("Invalid function code in DelEventHandler");
        return;
    }

    EventTab.SetStatus(event_name, func_code, FSTATUS_DELETED);

    for (int32_t n = 0; n < static_cast<int32_t>(EventMsg.GetClassesNum()); n++)
    {
        S_EVENTMSG *pM = EventMsg.Read(n);
        if (!pM->pEventName)
            continue;
        if (pM->ProcessTime(0))
            continue; // skip events, possible executed on this frame
        if (storm::iEquals(pM->pEventName, event_name))
        {
            EventMsg.Del(n);
            n--;
        }
    }
}

// A handler ProcessEvent would run: FSTATUS_NORMAL (a NEW one becomes normal at the next frame,
// a DELETED one is skipped until ProcessFrame removes it).
bool COMPILER::HasActiveEventHandler(const char *event_name, const char *func_name)
{
    if (!event_name || !func_name)
        return false;
    const uint32_t func_code = FuncTab.FindFunc(func_name);
    const uint32_t event_code = EventTab.FindEvent(event_name);
    if (func_code == INVALID_FUNC_CODE || event_code == INVALID_EVENT_CODE)
        return false;
    const EVENTINFO *event = EventTab.GetEvent(event_code);
    if (!event)
        return false;
    for (uint32_t n = 0; n < event->elements; n++)
        if (event->pFuncInfo[n].func_code == func_code && event->pFuncInfo[n].status == FSTATUS_NORMAL)
            return true;
    return false;
}

VDATA *COMPILER::ProcessEvent(const char *event_name)
{
    // TODO: only do if stack debug if enabled (should be runtime configurable)
    // push event name to call stack
    storm::ringbuffer_stack_push_guard push_guard(callStack_);
    push_guard.push(std::make_tuple("", 0U, event_name, kNoArguments, 0U));

    bEventsBreak = false;

    VDATA *pVD = nullptr;
    if (event_name == nullptr)
    {
        SetError("Invalid event name in ProcessEvent");
        return nullptr;
    }

    const uint32_t event_code = EventTab.FindEvent(event_name);
    if (event_code == INVALID_EVENT_CODE)
        return nullptr; // no handlers

    // An event raised by the engine (no script is running): its whole time goes to its name and origin.
    struct Timed
    {
        COMPILER &compiler;
        const uint64_t key;
        const bool outermost;
        const std::chrono::steady_clock::time_point start;
        ~Timed()
        {
            if (!outermost)
                return;
            storm::platform::stall_watch::PublishEvent(nullptr);
            EventWork &work = compiler.event_work_[key];
            work.calls++;
            work.ns += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count();
            compiler.event_origin_ = storm::platform::frame_work::Phase::Script;
        }
    } timed{*this, event_code | (static_cast<uint64_t>(event_origin_) << 32), execute_depth_ == 0,
            execute_depth_ == 0 ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{}};
    // The stall watchdog names the event the game thread is in (platform/stall_watch.hpp).
    if (timed.outermost)
        storm::platform::stall_watch::PublishEvent(event_name);
    // The handlers are read from the table before each call: a handler may add or drop handlers of this event.
    for (uint32_t n = 0;; n++)
    {
        const EVENTINFO *event = EventTab.GetEvent(event_code);
        if (!event || n >= event->elements)
            break;
        if (event->pFuncInfo[n].status != FSTATUS_NORMAL)
            continue;
        const uint32_t func_code = event->pFuncInfo[n].func_code;
        MESSAGE *pMem = pEventMessage;
        if (pMem)
        {
            pMem->Move2Start();
        }

        const uint32_t nStackVars = SStack.GetDataNum(); // remember stack elements num
        DATA *pResult;
        BC_Execute(func_code, pResult);

        pEventMessage = pMem;
        if (pResult)
            pVD = pResult->GetVarPointer();
        if (pResult)
            SStack.Pop();

        if (nStackVars != SStack.GetDataNum())
        {
            SStack.InvalidateFrom(nStackVars); // restore stack state - crash situation
            pVD = nullptr;
            SetError("process event stack error");
        }

        if (bEventsBreak)
            break;
    }

    pRun_fi = nullptr;

    return pVD;
}

void COMPILER::ProcessEvent(const char *event_name, MESSAGE *pMs)
{
    pEventMessage = pMs;
    ProcessEvent(event_name);
    pEventMessage = nullptr;
}

VDATA *COMPILER::ProcessEvent(const char *event_name, MESSAGE message)
{
    pEventMessage = &message;
    VDATA *pVD = ProcessEvent(event_name);
    pEventMessage = nullptr;
    return pVD;
}

uint32_t COMPILER::GetSegmentIndex(uint32_t segment_id)
{
    for (uint32_t n = 0; n < SegmentsNum; n++)
    {
        if (SegmentTable[n].id == segment_id)
            return n;
    }
    return INVALID_SEGMENT_INDEX;
}

// function release global variables, functions and labels reference for segment
// A segment UnloadSegment marked is gone for the scripts at once (its functions, variables
// and events are invalidated) and freed at the next ProcessFrame; until then it stays in the
// table under its name. It is not loaded: a LoadSegment of the same name in that frame
// (interface.c LoadGameOptions twice in one frame: load, call, unload, load again) loads it
// afresh instead of answering "already loaded" with its functions gone ("Invalid function
// call"), and an unload finds the live copy, not the stale one.
static bool LiveSegment(const SEGMENT_DESC &segment, const char *file_name)
{
    return !segment.bUnload && strcmp(segment.name.c_str(), file_name) == 0;
}

// mark segment for subsequent unload (on ProcessFrame)
void COMPILER::UnloadSegment(const char *segment_name)
{

    for (uint32_t n = 0; n < SegmentsNum; n++)
    {
        if (LiveSegment(SegmentTable[n], segment_name))
        {
            const uint32_t segment_id = SegmentTable[n].id;
            SegmentTable[n].bUnload = true;
            storm::ScriptStateChanged();
            FuncTab.InvalidateBySegmentID(segment_id);
            VarTab.InvalidateBySegmentID(segment_id);
            EventTab.InvalidateBySegmentID(segment_id);
            DefTab.InvalidateBySegmentID(segment_id);
            if (SegmentTable[n].Files_list)
            {
                SegmentTable[n].Files_list->Release();
            }
            break;
        }
    }
}

bool COMPILER::BC_SegmentIsLoaded(const char *file_name)
{
    if (file_name == nullptr)
    {
        SetError("Invalid segment name");
        return false;
    }
    for (uint32_t n = 0; n < SegmentsNum; n++)
    {
        if (LiveSegment(SegmentTable[n], file_name))
            return true;
    }
    return false;
}

bool COMPILER::BC_LoadSegment(const char *file_name)
{
    uint32_t n;

    LabelUpdateTable.Release();

    if (file_name == nullptr)
    {
        SetError("Invalid segment name");
        return false;
    }


    for (n = 0; n < SegmentsNum; n++)
    {
        if (LiveSegment(SegmentTable[n], file_name))
        {
            SetWarning("Segment already loaded: %s", file_name);
            return true;
        }
    }
    storm::ScriptStateChanged(); // a segment comes (or, failing, goes): SegmentIsLoaded changes

    uint32_t id = 0;
    bool found = false;
    while (!found)
    {
        found = true;
        for (n = 0; n < SegmentsNum; n++)
        {
            if (SegmentTable[n].id == id)
                found = false;
        }
        if (!found)
            id++;
    }

    const uint32_t index = SegmentsNum;
    SegmentsNum++;
    SegmentTable.resize(SegmentsNum);

    SegmentTable[index].name = strdup(file_name);
    SegmentTable[index].id = id;
    SegmentTable[index].bUnload = false;
    SegmentTable[index].pData = nullptr;
    SegmentTable[index].pCode = nullptr;
    SegmentTable[index].BCode_Program_size = 0;
    SegmentTable[index].BCode_Buffer_size = 0;

    SegmentTable[index].Files_list = new STRINGS_LIST;
    SegmentTable[index].Files_list->SetStringDataSize(sizeof(OFFSET_INFO));
    const auto load_started = std::chrono::steady_clock::now();
    bool result = false;
    bool from_cache = false;
    if (script_cache_mode_ != kCacheDisabled)
    {
        static bool is_calculated;
        static uint64_t calculated_fingerprint;
        if (!is_calculated)
        {
            is_calculated = true;
            calculated_fingerprint = CalculateCacheKey(ProgramDirectory);
        }
        else if (script_cache_mode_ == kCacheEnabled)
        {
            calculated_fingerprint = CalculateCacheKey(ProgramDirectory);
        }

        using storm::script_cache::StateFile;
        uint64_t stored_fingerprint = 0;
        const auto state = storm::script_cache::readState(GetCacheFolder(), stored_fingerprint);
        if (state == StateFile::Read && stored_fingerprint == calculated_fingerprint)
        {
            // attempt to load from cache first
            result = from_cache = LoadSegmentFromCache(SegmentTable[index]);
        }
        else
        {
            // Every outcome is logged: a rebuild costs a full compile at startup.
            if (state == StateFile::Missing)
                spdlog::info("Script cache: none in '{}'; compiling and storing scripts", GetCacheFolder().string());
            else if (state == StateFile::Damaged)
                spdlog::warn("Script cache: state file in '{}' is damaged; rebuilding", GetCacheFolder().string());
            else
                spdlog::info("Script cache: scripts or engine build changed (key {:016x} -> {:016x}); rebuilding",
                             stored_fingerprint, calculated_fingerprint);
            if (const auto error = storm::script_cache::resetFolder(GetCacheFolder(), calculated_fingerprint);
                !error.empty())
            {
                spdlog::error("Script cache: {}; disabled for this session, scripts are compiled from source", error);
                script_cache_mode_ = kCacheDisabled;
            }
        }
    }

    if (!result)
    {
        result = Compile(SegmentTable[index]);
    }
    if (result)
    {
        spdlog::info("Script segment '{}' {} in {} ms", file_name, from_cache ? "loaded from cache" : "compiled",
                     duration_cast<milliseconds>(std::chrono::steady_clock::now() - load_started).count());
    }

    if (!result)
    {
        delete SegmentTable[index].Files_list;
        SegmentsNum--;
        SegmentTable.resize(SegmentsNum);
        FuncTab.InvalidateBySegmentID(id);
        VarTab.InvalidateBySegmentID(id);
        EventTab.InvalidateBySegmentID(id);
        DefTab.InvalidateBySegmentID(id);
    }
    return result;
}

void COMPILER::ProcessFrame(uint32_t DeltaTime)
{
    for (uint32_t n = 0; n < SegmentsNum; n++)
    {
        if (!SegmentTable[n].bUnload)
            continue;
        // unload segment of program
        delete[] SegmentTable[n].pData;
        delete[] SegmentTable[n].pCode;
        delete SegmentTable[n].Files_list;
        SegmentTable[n].Files_list = nullptr;

        for (uint32_t i = n; i < (SegmentsNum - 1); i++)
        {
            SegmentTable[i] = SegmentTable[i + 1];
        }
        SegmentsNum--;
        SegmentTable.resize(SegmentsNum);
        n = 0;
    }

    EventTab.ProcessFrame();

    for (int32_t ln = 0; ln < static_cast<int32_t>(EventMsg.GetClassesNum()); ln++)
    {
        S_EVENTMSG *pMsg = EventMsg.Read(ln);
        if (pMsg->bInvalide)
            continue;
        if (pMsg->bProcess == false)
        {
            pMsg->ProcessTime(DeltaTime);
            continue;
        }
        if (pMsg->ProcessTime(DeltaTime))
        {
            pMsg->Invalidate();
            ForgetDeletedAttributes(pMsg);
            ProcessEvent(pMsg->pEventName, pMsg->pMessageClass);
        }
    }
    EventMsg.RemoveInvalidated();

    work_frames_++;
    if (std::chrono::steady_clock::now() - work_window_ >= std::chrono::seconds(30))
        ReportScriptWork();
}

// One line: the events by time per frame (ms, calls per frame, the phase they were raised from) and what engine
// code looked up by name outside scripts.
void COMPILER::ReportScriptWork()
{
    const auto now = std::chrono::steady_clock::now();
    const double seconds = std::chrono::duration<double>(now - work_window_).count();
    const double frames = work_frames_;
    std::vector<std::pair<uint64_t, EventWork>> events(event_work_.begin(), event_work_.end());
    std::sort(events.begin(), events.end(), [](const auto &a, const auto &b) { return a.second.ns > b.second.ns; });
    double total = 0.0;
    std::array<double, storm::platform::frame_work::kPhases> by_origin{};
    for (const auto &[key, work] : events)
    {
        total += work.ns;
        by_origin[key >> 32] += work.ns;
    }
    std::string line = fmt::format("script work: {:.1f} s, {} frames; events_ms={:.3f}", seconds, work_frames_,
                                   total / 1e6 / frames);
    for (size_t phase = 0; phase < by_origin.size(); phase++)
        if (by_origin[phase] > 0.0)
            line += fmt::format(" from_{}={:.3f}", storm::platform::frame_work::kPhaseNames[phase], by_origin[phase] / 1e6 / frames);
    line += " top=";
    for (size_t n = 0; n < events.size() && n < 12; n++)
    {
        const EVENTINFO *event = EventTab.GetEvent(static_cast<uint32_t>(events[n].first));
        line += fmt::format("{}{}@{}:{:.3f}x{:.2f}", n ? "," : "", event && event->name ? event->name : "?",
                            storm::platform::frame_work::kPhaseNames[events[n].first >> 32],
                            events[n].second.ns / 1e6 / frames, events[n].second.calls / frames);
    }
    storm::ScriptNameLookups &lookups = storm::NameLookups();
    line += fmt::format("; engine lookups by name per frame: attributes={:.1f} (interface adapters {:.1f}, hud {:.1f}) "
                        "variables={:.1f} (hud {:.1f})",
                        lookups.attributes / frames, lookups.adapterAttributes / frames, lookups.hudAttributes / frames,
                        lookups.variables / frames, lookups.hudVariables / frames);
    spdlog::info("{}", line);
    lookups = {};
    event_work_.clear();
    work_frames_ = 0;
    work_window_ = now;
}

void COMPILER::ResizeBCodeBuffer(SEGMENT_DESC &Segment, uint32_t add_size)
{
    int64_t needed_memory = static_cast<int64_t>(Segment.BCode_Program_size) + add_size - Segment.BCode_Buffer_size;
    if (needed_memory >= 0)
    {
        const auto needed_blocks = (needed_memory - 1) / BCODE_BUFFER_BLOCKSIZE + 1;
        Segment.BCode_Buffer_size += needed_blocks * BCODE_BUFFER_BLOCKSIZE;
        auto *const newPtr = new char[Segment.BCode_Buffer_size];
        memcpy(newPtr, Segment.pCode, Segment.BCode_Program_size);
        delete[] Segment.pCode;
        Segment.pCode = newPtr;
    }
}

void COMPILER::CompileToken(SEGMENT_DESC &Segment, S_TOKEN_TYPE Token_type, uint32_t data_blocks_num, ...)
{
    uint32_t data_size;
    uint32_t n;
    va_list args;
    char *data_ptr;


    // write instruction (1 byte) ----------------------------------------------
    uint32_t write_size = 1;
    ResizeBCodeBuffer(Segment, write_size);
    Segment.pCode[Segment.BCode_Program_size] = Token_type;
    Segment.BCode_Program_size += write_size;

    if (data_blocks_num == 0)
    {
        // write byte length of token data -------------------------------------
        write_size = 1;
        ResizeBCodeBuffer(Segment, write_size);
        Segment.pCode[Segment.BCode_Program_size] = 0;
        Segment.BCode_Program_size += write_size;
        return;
    }

    auto *const newPtr = new char[data_blocks_num * (sizeof(char *) + sizeof(uint32_t))];
    delete[] pCompileTokenTempBuffer;
    pCompileTokenTempBuffer = newPtr;

    // gather data blocks information and count total data size ----------------
    va_start(args, data_blocks_num);
    write_size = 0;
    for (n = 0; n < data_blocks_num; n++)
    {
        data_ptr = va_arg(args, char *);
        data_size = va_arg(args, uint32_t);
        memcpy(pCompileTokenTempBuffer + n * (sizeof(char *) + sizeof(uint32_t)), &data_ptr, sizeof(char *));
        memcpy(pCompileTokenTempBuffer + n * (sizeof(char *) + sizeof(uint32_t)) + sizeof(char *), &data_size,
               sizeof(uint32_t));
        write_size += data_size;
    }
    va_end(args);

    if (write_size < 0xff)
    {
        // write byte length of token data
        ResizeBCodeBuffer(Segment, 1);
        Segment.pCode[Segment.BCode_Program_size] = static_cast<uint8_t>(write_size);
        Segment.BCode_Program_size += 1;
    }
    else
    {
        // write uint32_t length of token data
        ResizeBCodeBuffer(Segment, sizeof(uint32_t) + 1);
        Segment.pCode[Segment.BCode_Program_size] = static_cast<uint8_t>(0xff);
        Segment.BCode_Program_size += 1;
        memcpy(&Segment.pCode[Segment.BCode_Program_size], &write_size, sizeof(uint32_t));
        Segment.BCode_Program_size += sizeof(uint32_t);
    }

    ResizeBCodeBuffer(Segment, write_size);
    for (n = 0; n < data_blocks_num; n++)
    {
        memcpy(&data_ptr, pCompileTokenTempBuffer + n * (sizeof(char *) + sizeof(uint32_t)), sizeof(char *));
        memcpy(&data_size, pCompileTokenTempBuffer + n * (sizeof(char *) + sizeof(uint32_t)) + sizeof(char *),
               sizeof(uint32_t));
        memcpy(&Segment.pCode[Segment.BCode_Program_size], data_ptr, data_size);
        Segment.BCode_Program_size += data_size;
    }
}

bool COMPILER::InitInternalFunctions()
{
    FuncInfo fi;

    // register internal functions ------------

    const uint32_t internal_functions_num = GetIntFunctionsNum();
    for (uint32_t n = 0; n < internal_functions_num; n++)
    {
        fi.segment_id = INTERNAL_SEGMENT_ID;
        fi.name = IntFuncTable[n].pName;
        fi.offset = n;
        fi.arguments = GetInternalFunctionArgumentsNum(n);
        fi.decl_file_name = "engine";
        fi.decl_line = 0;
        fi.return_type = IntFuncTable[n].ReturnType;
        uint32_t func_code = FuncTab.AddFunc(fi);

        if (func_code == INVALID_FUNC_CODE)
        {
            func_code = FuncTab.FindFunc(fi.name);
            FuncTab.GetFunc(fi, func_code);
            SetError("Function [%s] already declared in: %s line %d", fi.name.c_str(), fi.decl_file_name.c_str(),
                     fi.decl_line);
            return false;
        }
    }
    return true;
}

bool COMPILER::Compile(SEGMENT_DESC &Segment, char *pInternalCode, uint32_t pInternalCodeSize)
{
    char dbg_file_name[MAX_PATH];
    char file_name[MAX_PATH];
    char func_name[MAX_PATH];
    char var_name[MAX_PATH];
    char *pProgram;
    char *pApend_file;
    char *pSegmentSource;
    FuncInfo fi;
    VarInfo vi = {};
    const VarInfo *real_var = nullptr;
    LocalVarInfo lvi;
    uint32_t SegmentSize;
    uint32_t Program_size;
    uint32_t func_code;
    uint32_t var_code;
    uint32_t def_code;
    uint32_t inout;
    uint32_t bracket_inout;
    uint32_t n;
    uint32_t file_code;
    ptrdiff_t Control_offset;
    uint32_t Current_line;
    uint32_t Append_file_size;
    STRINGS_LIST BlockTable;
    S_TOKEN_TYPE Token_type;
    DEFINFO di;

    CompilerStage = CS_COMPILATION;

    RunningSegmentID = INVALID_SEGMENT_INDEX;

    script_cache_ = storm::ScriptCache();

    bool bFunctionBlock;
    int32_t lvalue;
    float fvalue;

    Control_offset = 0;
    strcpy_s(file_name, Segment.name.c_str());

    if (pInternalCode == nullptr)
    {
        auto is_new = Segment.Files_list->AddUnicalString(file_name);
        file_code = Segment.Files_list->GetStringCode(file_name);
        pProgram = nullptr;
        Program_size = 0;
        pSegmentSource = LoadFile(file_name, SegmentSize);
        if (is_new && script_cache_mode_ != kCacheDisabled)
        {
            script_cache_.files.emplace_back(file_name);
        }
        AppendProgram(pProgram, Program_size, pSegmentSource, SegmentSize, true);
        Segment.pData = pProgram;
        if (pProgram == nullptr)
        {
            SetError("file not found: %s", file_name);
            return false;
        }
        CurrentFuncCode = INVALID_FUNC_CODE;
    }
    else
    {
        pProgram = pInternalCode;
        Program_size = pInternalCodeSize;
        Segment.pData = pProgram;
    }


    // register functions and variables ---------
    strcpy_s(func_name, "null");
    inout = 0;
    bracket_inout = 0;
    bFunctionBlock = false;
    Token.SetProgram(pProgram, pProgram);
    if (bDebugInfo)
    {
        strcpy_s(DebugSourceFileName, Segment.name.c_str());
    }
    DebugSourceLine = DSL_INI_VALUE;
    bool bExtern;
    bool bImport;
    bExtern = false;
    bImport = false;
    bool bDotFlag;
    bDotFlag = false;
    do
    {
        Token_type = Token.Get();
        bDotFlag = false;
        if (Token_type == DOT)
        {
            Token_type = Token.Get();
            bDotFlag = true;
        }
        switch (Token_type)
        {
        case COMMENT:
            DebugSourceLine += Token.TokenLines();
            break;
        case LABEL:
            Token.StepBack();
            Token.StepBack();
            Token.Get(); // label name

            if (!LabelTable.AddUnicalString(Token.GetData()))
                SetError("duplicate label: %s", Token.GetData());
            Token.Get();
            break;
        case CASE_COMMAND:
            Token.Get(); // condition value
            if (Token.Get() != LABEL)
            {
                SetError("invalid case syntax");
                return false;
            }
            break;
        case SEPARATOR:
            if (bFunctionBlock && bExtern)
            {
                bFunctionBlock = false;
                bExtern = false;
            }
            if (bFunctionBlock && bImport)
            {
                bFunctionBlock = false;
                bImport = false;
            }
            break;
        case DEBUG_LINEFEED:
            DebugSourceLine++;
            break;
        case DEBUG_FILE_NAME:
            DebugSourceLine = DSL_INI_VALUE;
            strcpy_s(DebugSourceFileName, Token.GetData());
            break;
        case INCLUDE_LIBRIARY: {
            SCRIPT_LIBRIARY *pLib;
            VMA *pClass;

            auto name = std::string_view(Token.GetData());
            auto comparator = [&name](const auto &library) { return storm::iEquals(library.name, name); };

            if (std::ranges::find_if(LibriaryFuncs, comparator) != LibriaryFuncs.end())
            {
                break;
            }

            pClass = core_internal.FindVMA(Token.GetData());
            if (!pClass)
            {
                SetWarning("cant load libriary '%s'", Token.GetData());
                break;
            }

            pLib = static_cast<SCRIPT_LIBRIARY *>(pClass->CreateClass());
            if (pLib)
                pLib->Init();

            LibriaryFuncs.emplace_back(pLib, Token.GetData());
            if (script_cache_mode_ != kCacheDisabled)
            {
                script_cache_.script_libs.emplace_back(Token.GetData());
            }

            break;
        }
        case INCLIDE_FILE:
            if (Segment.Files_list->AddUnicalString(Token.GetData()))
            {
                file_code = Segment.Files_list->GetStringCode(Token.GetData());
                Control_offset = Token.GetProgramControl() - Token.GetProgramBase(); // store program scan point
                pApend_file = LoadFile(Token.GetData(), Append_file_size);
                if (pApend_file == nullptr)
                {
                    SetError("can't load file: %s", Token.GetData());
                    return false;
                }
                if (script_cache_mode_ != kCacheDisabled)
                {
                    script_cache_.files.emplace_back(Token.GetData());
                }
                if (bDebugInfo)
                {
                    uint32_t aps;
                    strcpy_s(dbg_file_name, "#file \"");
                    strcat_s(dbg_file_name, Token.GetData());
                    strcat_s(dbg_file_name, "\";");
                    aps = strlen(dbg_file_name);
                    char *pTempS;
                    pTempS = new char[aps + 1];
                    strcpy_s(pTempS, aps + 1, dbg_file_name);
                    AppendProgram(pProgram, Program_size, pTempS, aps, false);
                }
                AppendProgram(pProgram, Program_size, pApend_file, Append_file_size, true);
                Segment.pData = pProgram;
                Token.SetProgram(pProgram, pProgram + Control_offset);
            }
            break;
        case DEFINE_COMMAND: // create define
            // DefineTable
            if (inout == 0)
            {
                strcpy_s(var_name, Token.GetData());
                di.name = var_name;
                di.segment_id = Segment.id;
                Token.Get();
                di.deftype = Token.GetType();
                bool bMinus = false;
                if (Token.GetType() == OP_MINUS)
                {
                    bMinus = true;
                    Token.Get();
                    di.deftype = Token.GetType();
                }
                di.data4b = {};
                switch (Token.GetType())
                {
                case NUMBER:
                    lvalue = static_cast<int32_t>(atoll(Token.GetData()));
                    if (bMinus)
                        lvalue = -lvalue;
                    memcpy(&di.data4b, &lvalue, sizeof(int32_t));
                    break;
                case FLOAT_NUMBER:
                    fvalue = static_cast<float>(atof(Token.GetData()));
                    if (bMinus)
                        fvalue = -fvalue;
                    memcpy(&di.data4b, &fvalue, sizeof(float));
                    break;
                case STRING: {
                    const auto len = strlen(Token.GetData()) + 1;
                    di.data4b = (uintptr_t) new char[len];
                    memcpy((void *)di.data4b, Token.GetData(), len);
                    break;
                }
                case OP_MINUS:

                    break;
                }
                def_code = DefTab.AddDef(di);
                if (def_code == INVALID_DEF_CODE)
                {
                    SetError("define redefinition: %s", di.name);
                    return false;
                }
                if (script_cache_mode_ != kCacheDisabled)
                {
                    script_cache_.defines.emplace_back(storm::script_cache::Define{di.name, di.deftype, di.data4b});
                }
            }
            break;

        case EXTERN:
            Token_type = Token.Get();
            bExtern = true;
        case IMPORT:
            if (!bExtern)
            {
                Token_type = Token.Get();
                bImport = true;
            }
        case VAR_AREFERENCE:
        case VAR_REFERENCE:
        case VAR_INTEGER:
        case VAR_PTR:
        case VAR_FLOAT:
        case VAR_STRING:
        case VAR_OBJECT:
        case TVOID:
            if (inout == 0)
            {
                if (!bFunctionBlock)
                {
                    Token.Get();
                    if (Token.GetType() == OPEN_BRACKET)
                    {
                        // this is function header
                        Token.StepBack();
                        Token.StepBack();
                        Token.Get();           // function name
                        bFunctionBlock = true; // we are stepped into func area

                        strcpy_s(func_name, Token.GetData());

                        fi.segment_id = Segment.id;
                        if (bExtern)
                            fi.offset = INVALID_FUNC_OFFSET;
                        else if (bImport)
                            fi.offset = INVALID_FUNC_OFFSET;
                        else
                            fi.offset = Segment.BCode_Program_size;
                        fi.name = func_name;
                        fi.return_type = Token_type;
                        fi.arguments = 0;
                        fi.decl_file_name = DebugSourceFileName;
                        fi.decl_line = DebugSourceLine;
                        fi.imported_func = nullptr;
                        func_code = FuncTab.AddFunc(fi);

                        if (func_code == INVALID_FUNC_CODE)
                        {
                            func_code = FuncTab.FindFunc(fi.name);
                            FuncTab.GetFunc(fi, func_code);
                            SetError("Function [%s] already declared in: %s line %d", fi.name.c_str(),
                                     fi.decl_file_name.c_str(), fi.decl_line);

                            return false;
                        }
                        if (script_cache_mode_ != kCacheDisabled)
                        {
                            script_cache_.functions.emplace_back(storm::script_cache::Function{
                                fi, std::vector<storm::script_cache::FunctionLocalVariable>{}});
                        }
                        break;
                    }
                    Token.StepBack();

                    // global variable
                    Token.StepBack();
                    do
                    {
                        Token.Get();
                        if (Token.GetData() == nullptr)
                        {
                            SetError("Invalid variable name");
                            return false;
                        }
                        vi.name = Token.GetData();
                        vi.segment_id = Segment.id;
                        vi.elements = 1;
                        vi.type = Token_type;
                        Token.Get();
                        if (Token.GetType() == SQUARE_OPEN_BRACKET)
                        {
                            Token.Get();
                            if (Token.GetType() != NUMBER)
                            {
                                def_code = DefTab.FindDef(Token.GetData());
                                if (def_code != INVALID_DEF_CODE)
                                {
                                    DefTab.GetDef(di, def_code);
                                    if (di.deftype == NUMBER)
                                    {
                                        lvalue = di.data4b;
                                    }
                                    else
                                    {
                                        SetError("Invalid array (%s) size", vi.name.c_str());
                                        return false;
                                    }
                                }
                                else
                                {
                                    SetError("Invalid array (%s) size", vi.name.c_str());
                                    return false;
                                }
                            }
                            else
                                lvalue = static_cast<int32_t>(atoll(Token.GetData()));
                            vi.elements = lvalue;
                            Token.Get(); // SQUARE_CLOSE_BRACKET
                            Token.Get();
                        }
                        var_code = VarTab.AddVar(vi);
                        if (var_code == INVALID_VAR_CODE)
                        {
                            SetError("Duplicate variable name: %s", vi.name.c_str());
                            return false;
                        }

                        real_var = VarTab.GetVarX(var_code);
                        if (real_var == nullptr)
                        {
                            SetError("Registered variable %s has invalid code", vi.name.c_str());
                            return false;
                        }

                        bool bNeg;
                        if (Token.GetType() == OP_EQUAL)
                        {
                            // global var initialization
                            if (vi.elements > 1)
                            {
                                // array
                                if (SkipAuxiliaryTokens() != BLOCK_IN)
                                {
                                    SetError("Invalid array '%s' initialization", vi.name.c_str());
                                    return false;
                                }
                                uint32_t aindex;
                                aindex = 0;

                                SkipAuxiliaryTokens();
                                while (Token.GetType() != END_OF_PROGRAMM)
                                {
                                    if (Token.GetType() == OP_MINUS)
                                    {
                                        bNeg = true;
                                        Token.Get();
                                    }
                                    else
                                        bNeg = false;
                                    if (Token.GetType() == BLOCK_OUT)
                                        break;
                                    if (Token.GetType() == END_OF_PROGRAMM)
                                        break;

                                    switch (Token.GetType())
                                    {
                                    case TRUE_CONST:
                                        if (vi.type != VAR_INTEGER)
                                        {
                                            SetError("Invalid array '%s' initialization parameter", vi.name.c_str());
                                            return false;
                                        }
                                        real_var->value->Set(1);
                                        break;
                                    case FALSE_CONST:
                                        if (vi.type != VAR_INTEGER)
                                        {
                                            SetError("Invalid array '%s' initialization parameter", vi.name.c_str());
                                            return false;
                                        }
                                        real_var->value->Set(0);
                                        break;

                                    case NUMBER:
                                        if (vi.type != VAR_INTEGER)
                                        {
                                            SetError("Invalid array '%s' initialization parameter", vi.name.c_str());
                                            return false;
                                        }
                                        if (bNeg)
                                            real_var->value->Set(-atoi(Token.GetData()), aindex);
                                        else
                                            real_var->value->Set(static_cast<int32_t>(atoll(Token.GetData())), aindex);
                                        aindex++;
                                        break;
                                    case FLOAT_NUMBER:
                                        if (vi.type != VAR_FLOAT)
                                        {
                                            SetError("Invalid array '%s' initialization parameter", vi.name.c_str());
                                            return false;
                                        }
                                        if (bNeg)
                                            real_var->value->Set(-static_cast<float>(atof(Token.GetData())), aindex);
                                        else
                                            real_var->value->Set(static_cast<float>(atof(Token.GetData())), aindex);
                                        aindex++;
                                        break;
                                    case STRING:
                                        if (vi.type != VAR_STRING)
                                        {
                                            SetError("Invalid array '%s' initialization parameter", vi.name.c_str());
                                            return false;
                                        }
                                        real_var->value->Set(Token.GetData(), aindex);
                                        aindex++;
                                        break;
                                    case UNKNOWN:

                                        break;
                                    default:
                                        break;
                                    }
                                    SkipAuxiliaryTokens();
                                    if (Token.GetType() == BLOCK_OUT)
                                        break;
                                    if (Token.GetType() != COMMA)
                                    {
                                        SetError("Invalid array '%s' initialization parameters list", vi.name.c_str());
                                        return false;
                                    }
                                    SkipAuxiliaryTokens();
                                }
                                SkipAuxiliaryTokens();
                            }
                            else
                            {
                                // single variable
                                if (Token.Get() == OP_MINUS)
                                {
                                    bNeg = true;
                                    Token.Get();
                                }
                                else
                                    bNeg = false;

                                switch (Token.GetType())
                                {
                                case TRUE_CONST:
                                    if (vi.type != VAR_INTEGER)
                                        break;
                                    real_var->value->Set(1);
                                    break;
                                case FALSE_CONST:
                                    if (vi.type != VAR_INTEGER)
                                        break;
                                    real_var->value->Set(0);
                                    break;
                                case NUMBER:
                                    if (vi.type != VAR_INTEGER)
                                        break;
                                    if (bNeg)
                                        real_var->value->Set(-atoi(Token.GetData()));
                                    else
                                        real_var->value->Set(static_cast<int32_t>(atoll(Token.GetData())));
                                    break;
                                case FLOAT_NUMBER:
                                    if (vi.type != VAR_FLOAT)
                                        break;
                                    if (bNeg)
                                        real_var->value->Set(-static_cast<float>(atof(Token.GetData())));
                                    else
                                        real_var->value->Set(static_cast<float>(atof(Token.GetData())));
                                    break;
                                case STRING:
                                    if (vi.type != VAR_STRING)
                                        break;
                                    real_var->value->Set(Token.GetData());
                                    break;
                                case UNKNOWN:

                                    break;
                                default:
                                    break;
                                }
                                Token.Get();
                            }
                        }

                        if (script_cache_mode_ != kCacheDisabled)
                        {
                            auto &cached_var = script_cache_.variables.emplace_back(vi);
                            cached_var.value = std::make_unique<DATA>();
                            *cached_var.value = *real_var->value;
                        }
                    } while (Token.GetType() == COMMA);
                    Token.StepBack();
                    break;
                }
                // function argument list
                // compute number of arguments
                {
                    lvi.type = Token_type;
                    lvi.name = Token.GetData();
                    lvi.elements = 1;
                    if (bExtern)
                        FuncTab.AddFuncArg(func_code, lvi, true);
                    else
                        FuncTab.AddFuncArg(func_code, lvi);

                    if (script_cache_mode_ != kCacheDisabled)
                    {
                        script_cache_.functions.back().arguments.emplace_back(
                            storm::script_cache::FunctionLocalVariable{lvi, bExtern});
                    }
                }
            }
            else
            {
                // local variables
                // compute number of local vars
                lvi.type = Token_type;
                Token.StepBack();
                do
                {
                    Token.Get();
                    if (Token.GetData() == nullptr)
                    {
                        SetError("Invalid variable name");
                        return false;
                    }
                    lvi.name = Token.GetData();
                    lvi.elements = 1;
                    Token.Get();
                    if (Token.GetType() == SQUARE_OPEN_BRACKET)
                    {
                        Token.Get();
                        if (Token.GetType() != NUMBER)
                        {
                            def_code = DefTab.FindDef(Token.GetData());
                            if (def_code != INVALID_DEF_CODE)
                            {
                                DefTab.GetDef(di, def_code);
                                if (di.deftype == NUMBER)
                                {
                                    lvalue = di.data4b;
                                }
                                else
                                {
                                    SetError("Invalid array (%s) size", lvi.name.c_str());
                                    return false;
                                }
                            }
                            else
                            {
                                SetError("Invalid array (%s) size", lvi.name.c_str());
                                return false;
                            }
                        }
                        else
                            lvalue = static_cast<int32_t>(atoll(Token.GetData()));
                        lvi.elements = lvalue;
                        Token.Get(); // SQUARE_CLOSE_BRACKET
                        Token.Get();
                    }

                    if (lvi.name.empty())
                    {
                        SetWarning("Empty variable name");
                    }
                    else if (!FuncTab.AddFuncVar(func_code, lvi))
                    {
                        SetError("Duplicate variable name: %s", lvi.name.c_str());
                        return false;
                    }

                    if (script_cache_mode_ != kCacheDisabled)
                    {
                        script_cache_.functions.back().local_variables.emplace_back(lvi);
                    }
                } while (Token.GetType() == COMMA);
                Token.StepBack();
            }
            break;

        case BLOCK_IN:
            inout++;
            break;
        case BLOCK_OUT:
            inout--;
            if (inout == 0)
                bFunctionBlock = false; // sign if we are out of function
            break;
        default:
            break;
        }
    } while (Token_type != END_OF_PROGRAMM);

    if (inout != 0)
    {
        SetError("Missing '}' in segment");
        return false;
    }


    Current_line = 1;
    DebugSourceLine = DSL_INI_VALUE;
    uint32_t fnsize;

    if (bDebugInfo)
    {
        strcpy_s(DebugSourceFileName, Segment.name.c_str());
        fnsize = Segment.name.length();
        CompileToken(Segment, DEBUG_FILE_NAME, 3, (char *)&DebugSourceLine, sizeof(uint32_t), (char *)&fnsize,
                     sizeof(uint32_t), Segment.name.c_str(), fnsize);
        CompileToken(Segment, DEBUG_LINE_CODE, 1, (char *)&DebugSourceLine, sizeof(uint32_t));
    }

    uint32_t block_io_code;
    block_io_code = 0;
    inout = 0;
    bracket_inout = 0;
    bFunctionBlock = false;
    pProgram = Segment.pData;
    Token.SetProgram(pProgram, pProgram);

    STRINGS_LIST BreakUpdateTable;
    if (!CompileBlock(Segment, bFunctionBlock, inout, TVOID, INVALID_OFFSET, INVALID_OFFSET, BreakUpdateTable))
        return false;
    DOUBLE_DWORD ddw;
    uint32_t label_offset;
    for (n = 0; n < LabelUpdateTable.GetStringsCount(); n++)
    {
        LabelUpdateTable.GetStringData(n, &ddw);
        LabelTable.GetStringData(ddw.dw1, &label_offset);
        memcpy(&Segment.pCode[ddw.dw2], &label_offset, sizeof(uint32_t));
    }

    if (pInternalCode == nullptr)
    {
        delete[] Segment.pData;
        Segment.pData = nullptr;
    }
    if (bWriteCodeFile)
    {
        // the compiled code of the script (ini script.codefiles), in the cache: never in the game folder
        const auto codeFolder = fs::GetCachePath() / "ScriptCode";
        std::error_code codeError;
        std::filesystem::create_directories(codeFolder, codeError);
        const auto codeFile = codeFolder / (std::filesystem::path(Segment.name.c_str()).filename().string() + ".b");
        std::ofstream code(codeFile, std::ios::binary | std::ios::trunc);
        code.write(reinterpret_cast<const char *>(Segment.pCode), Segment.BCode_Program_size);
        if (!code)
            spdlog::error("Cannot write the code file {}", codeFile.string());
    }
    if (script_cache_mode_ != kCacheDisabled)
    {
        SaveSegmentToCache(Segment);
    }
    return true;
}

S_TOKEN_TYPE COMPILER::SkipAuxiliaryTokens()
{
    S_TOKEN_TYPE Token_type;
    do
    {
        Token_type = Token.Get();
        switch (Token_type)
        {
        case DEBUG_LINEFEED:
        case DEBUG_FILE_NAME:
        case COMMENT:
        case INCLIDE_FILE:
        case WHITESPACE:
            break;
        default:
            return Token_type;
        }
    } while (Token_type != END_OF_PROGRAMM);
    return Token_type;
}

S_TOKEN_TYPE COMPILER::CompileAuxiliaryTokens(SEGMENT_DESC &Segment) //, bool & bFunctionBlock, DWORD & inout)
{
    uint32_t fnsize;
    S_TOKEN_TYPE Token_type;

    do
    {
        Token_type = Token.Get();
        switch (Token_type)
        {
        case DEBUG_LINEFEED:
            if (!bDebugInfo)
                break;
            DebugSourceLine++;
            CompileToken(Segment, DEBUG_LINE_CODE, 1, (char *)&DebugSourceLine, sizeof(uint32_t));
            break;
        case DEBUG_FILE_NAME:
            if (!bDebugInfo)
                break;
            DebugSourceLine = DSL_INI_VALUE;
            if (Token.GetData() == nullptr)
            {
                SetError("Invalid DBG_FILE_NAME");
                return DEBUG_FILE_NAME;
            }
            strcpy_s(DebugSourceFileName, Token.GetData());
            fnsize = strlen(Token.GetData());

            CompileToken(Segment, DEBUG_FILE_NAME, 3, (char *)&DebugSourceLine, sizeof(uint32_t), (char *)&fnsize,
                         sizeof(uint32_t), Token.GetData(), strlen(Token.GetData()));
            break;
        case COMMENT:
            DebugSourceLine += Token.TokenLines();
            break;
        case INCLIDE_FILE:
        case WHITESPACE:
            break;
        default:
            return Token_type;
        }
    } while (Token_type != END_OF_PROGRAMM);
    return Token_type;
}

void COMPILER::CopyOffsets(SEGMENT_DESC &Segment, STRINGS_LIST &srclist, STRINGS_LIST &dstlist, const char *sname)
{
    uint32_t n;
    uint32_t i;
    uint32_t jump_voffset;
    if (sname == nullptr)
    {
        for (n = 0; n < srclist.GetStringsCount(); n++)
        {
            srclist.GetStringData(n, (char *)&jump_voffset);

            i = dstlist.GetStringsCount();
            dstlist.AddString("c");
            dstlist.SetStringData(i, (char *)&jump_voffset);
        }
        return;
    }

    for (n = 0; n < srclist.GetStringsCount(); n++)
    {
        if (strcmp(sname, srclist.GetString(n)) != 0)
            continue;

        srclist.GetStringData(n, (char *)&jump_voffset);

        i = dstlist.GetStringsCount();
        dstlist.AddString("c");
        dstlist.SetStringData(i, (char *)&jump_voffset);
    }
}

void COMPILER::UpdateOffsets(SEGMENT_DESC &Segment, STRINGS_LIST &list, uint32_t offset, const char *sname)
{
    uint32_t n;
    uint32_t jump_voffset;
    if (sname == nullptr)
    {
        for (n = 0; n < list.GetStringsCount(); n++)
        {
            list.GetStringData(n, (char *)&jump_voffset);
            memcpy(&Segment.pCode[jump_voffset], &offset, sizeof(uint32_t));
        }
        return;
    }
    for (n = 0; n < list.GetStringsCount(); n++)
    {
        if (strcmp(sname, list.GetString(n)) != 0)
            continue;
        list.GetStringData(n, (char *)&jump_voffset);
        memcpy(&Segment.pCode[jump_voffset], &offset, sizeof(uint32_t));
    }
}

bool COMPILER::CompileBlock(SEGMENT_DESC &Segment, bool &bFunctionBlock, uint32_t &inout, S_TOKEN_TYPE bound_type,
                            uint32_t continue_jump, uint32_t break_offset, STRINGS_LIST &BreakTable)
{
    S_TOKEN_TYPE Token_type;
    uint32_t awcode;
    uint32_t var_code;
    uint32_t balance_bracket;
    uint32_t balance_block;
    uint32_t jump_offset;
    uint32_t update_offset;
    uint32_t vuint32_t;
    uint32_t n;
    uint32_t unk_code;
    uint32_t func_args;
    STRINGS_LIST BreakUpdateTable;
    BreakUpdateTable.SetStringDataSize(sizeof(uint32_t));
    bool bExtern;
    bool bImport;
    int32_t lvalue;
    uint32_t def_code;
    DEFINFO di;
    uint32_t dwRCode;

    const VarInfo *real_var;
    LocalVarInfo lvi;

    jump_offset = 0xffbadbad;

    balance_block = 0;
    balance_bracket = 0;

    switch (bound_type)
    {
    case CLOSE_BRACKET:
        balance_bracket = 1;
        break;
    case BLOCK_OUT:
        balance_block = 1;
        break;
    }

    bExtern = false;
    bImport = false;
    do
    {
        Token_type = CompileAuxiliaryTokens(Segment);

        switch (Token_type)
        {
        case EVENT_HANDLER:
            if (Token.Get() != OPEN_BRACKET)
            {
                SetError("Invalid event handler");
                return false;
            }

            if (Token.Get() == STRING)
            {
                if (Token.GetData() == nullptr)
                {
                    SetError("Invalid event handler");
                    return false;
                }
                strcpy_s(gs, Token.GetData());
            }
            else
            {
                if (Token.GetData() == nullptr)
                {
                    SetError("Invalid event handler");
                    return false;
                }
                def_code = DefTab.FindDef(Token.GetData());
                if (def_code != INVALID_DEF_CODE)
                {
                    DefTab.GetDef(di, def_code);
                    if (di.deftype != STRING)
                    {
                        SetError("Invalid event handler");
                        return false;
                    }
                    if (di.data4b == 0)
                    {
                        SetError("Invalid define");
                        return false;
                    }
                    strcpy_s(gs, (char *)di.data4b);
                }
                else
                {
                    SetError("Invalid event handler");
                    return false;
                }
            }
            if (Token.Get() != COMMA)
            {
                SetError("Invalid event handler");
                return false;
            }
            if (Token.Get() == STRING)
            {
                if (Token.GetData() == nullptr)
                {
                    SetError("Invalid event handler");
                    return false;
                }
                SetEventHandler(gs, Token.GetData(), 0, true);
                if (script_cache_mode_ != kCacheDisabled)
                {
                    script_cache_.event_handlers.emplace_back(storm::script_cache::EventHandler{gs, Token.GetData()});
                }
            }
            else
            {
                SetError("Invalid event handler");
                return false;
            }

            if (Token.Get() != CLOSE_BRACKET)
            {
                SetError("Invalid event handler");
                return false;
            }
            break;

        case GOTO_COMMAND:
            // LabelUpdateTable
            Token.Get(); // label name
            DOUBLE_DWORD ddw;
            ddw.dw1 = LabelTable.GetStringCode(Token.GetData());
            n = LabelUpdateTable.GetStringsCount();
            LabelUpdateTable.AddString("j");
            ddw.dw2 = Segment.BCode_Program_size + 2;
            LabelUpdateTable.SetStringData(n, (char *)&ddw);
            vuint32_t = 0xffffffff;
            CompileToken(Segment, JUMP, 1, (char *)&vuint32_t, sizeof(uint32_t));
            break;
        case BREAK_COMMAND:
            if (break_offset == INVALID_OFFSET)
            {
                SetError("invalid break");
                return false;
            }
            vuint32_t = Segment.BCode_Program_size + 2;
            n = BreakTable.GetStringsCount();
            BreakTable.AddString("b");
            BreakTable.SetStringData(n, (char *)&vuint32_t);
            vuint32_t = INVALID_OFFSET;
            CompileToken(Segment, JUMP, 1, (char *)&vuint32_t, sizeof(uint32_t));
            if (bound_type == BREAK_COMMAND)
            {
                return true;
            }
            break;
        case CONTINUE_COMMAND:
            if (continue_jump == INVALID_OFFSET)
            {
                SetError("invalid continue");
                return false;
            }
            vuint32_t = Segment.BCode_Program_size + 2;
            n = BreakTable.GetStringsCount();
            BreakTable.AddString("c");
            BreakTable.SetStringData(n, (char *)&vuint32_t);
            vuint32_t = INVALID_OFFSET;
            CompileToken(Segment, JUMP, 1, (char *)&vuint32_t, sizeof(uint32_t));
            break;
        case SEPARATOR:
            CompileToken(Segment, Token_type);
            if (bound_type == SEPARATOR)
            {
                return true;
            }
            break;
        case OPEN_BRACKET:
            balance_bracket++;
            CompileToken(Segment, Token_type);
            break;
        case CLOSE_BRACKET:
            balance_bracket--;
            CompileToken(Segment, Token_type);
            if (bound_type == CLOSE_BRACKET)
            {
                if (balance_bracket == 0)
                {
                    return true;
                }
            }
            break;
        case BLOCK_IN:
            inout++;
            balance_block++;
            break;
        case BLOCK_OUT:

            if (inout == 0)
            {
                SetError("syntax error");
                return false;
            }
            inout--;

            if (balance_block == 0)
            {
                switch (bound_type)
                {
                case CLOSE_BRACKET:
                    SetError("missed ')'");
                    break;
                case BLOCK_OUT:
                    SetError("missed '}'");
                    break;
                default:
                    SetError("syntax error");
                    break;
                }
                return false;
            }
            balance_block--;
            if (inout == 0)
            {
                CompileToken(Segment, FUNCTION_RETURN_VOID);
                bFunctionBlock = false; // sign if we are out of function
                break;
            }
            if (bound_type == BLOCK_OUT)
            {
                if (balance_block == 0)
                {
                    return true;
                }
            }
            break;
        case IF_BLOCK:

            Token_type = CompileAuxiliaryTokens(Segment);
            if (Token_type != OPEN_BRACKET)
            {
                SetError("missed '('");
                return false;
            }

            CompileExpression(Segment);

            if (Token.GetType() != CLOSE_BRACKET)
            {
                SetError("missed ')'");
                return false;
            }

            CompileToken(Segment, STACK_READ);
            CompileToken(Segment, EX);

            update_offset = Segment.BCode_Program_size + 2;
            CompileToken(Segment, JUMP_Z, 1, (char *)&jump_offset, sizeof(uint32_t));
            Token_type = CompileAuxiliaryTokens(Segment);
            if (Token_type == BLOCK_IN)
            {
                inout++;
                if (!CompileBlock(Segment, bFunctionBlock, inout, BLOCK_OUT, continue_jump, break_offset, BreakTable))
                    return false;
            }
            else
            {
                Token.StepBack();
                if (!CompileBlock(Segment, bFunctionBlock, inout, SEPARATOR, continue_jump, break_offset, BreakTable))
                    return false;
            }
            memcpy(&Segment.pCode[update_offset], &Segment.BCode_Program_size, sizeof(uint32_t));

            CompileToken(Segment, POP_EXPRESULT); // pop

            if (bound_type == IF_BLOCK)
            {
                return true;
            }

            break;
        case ELSE_BLOCK:
            update_offset = Segment.BCode_Program_size + 2;
            CompileToken(Segment, JUMP_NZ, 1, (char *)&jump_offset, sizeof(uint32_t));
            Token_type = CompileAuxiliaryTokens(Segment);
            if (Token_type == BLOCK_IN)
            {
                inout++;
                if (!CompileBlock(Segment, bFunctionBlock, inout, BLOCK_OUT, continue_jump, break_offset, BreakTable))
                    return false;
            }
            else
            {
                Token.StepBack();
                if (!CompileBlock(Segment, bFunctionBlock, inout, Token_type == IF_BLOCK ? IF_BLOCK : SEPARATOR,
                                  continue_jump, break_offset, BreakTable))
                    return false;
            }
            memcpy(&Segment.pCode[update_offset], &Segment.BCode_Program_size, sizeof(uint32_t));
            break;

        case WHILE_BLOCK:
            BreakUpdateTable.Release();
            jump_offset = Segment.BCode_Program_size;


            Token_type = CompileAuxiliaryTokens(Segment);
            if (Token_type != OPEN_BRACKET)
            {
                SetError("missed '('");
                return false;
            }
            CompileExpression(Segment);
            CompileToken(Segment, STACK_POP);
            CompileToken(Segment, EX);

            if (Token.GetType() != CLOSE_BRACKET)
            {
                SetError("missed ')'");
                return false;
            }


            update_offset = Segment.BCode_Program_size + 2;
            CompileToken(Segment, JUMP_Z, 1, (char *)&jump_offset, sizeof(uint32_t));
            Token_type = CompileAuxiliaryTokens(Segment);
            if (Token_type == BLOCK_IN)
            {
                inout++;
                if (!CompileBlock(Segment, bFunctionBlock, inout, BLOCK_OUT, jump_offset, 6, BreakUpdateTable))
                    return false;
            }
            else
            {
                Token.StepBack();
                if (!CompileBlock(Segment, bFunctionBlock, inout, SEPARATOR, jump_offset, 6, BreakUpdateTable))
                    return false;
                // **** ?
            }
            CompileToken(Segment, JUMP, 1, (char *)&jump_offset, sizeof(uint32_t));
            memcpy(&Segment.pCode[update_offset], &Segment.BCode_Program_size, sizeof(uint32_t));
            UpdateOffsets(Segment, BreakUpdateTable, Segment.BCode_Program_size, "b");
            UpdateOffsets(Segment, BreakUpdateTable, jump_offset, "c");
            BreakUpdateTable.Release();
            break;
        case FOR_BLOCK:
            BreakUpdateTable.Release();

            // '('
            Token_type = CompileAuxiliaryTokens(Segment);
            if (Token_type != OPEN_BRACKET)
            {
                SetError("missed '('");
                return false;
            }

            if (!CompileBlock(Segment, bFunctionBlock, inout, SEPARATOR, INVALID_OFFSET, INVALID_OFFSET, BreakTable))
                return false;

            jump_offset = Segment.BCode_Program_size;

            // condition
            CompileExpression(Segment);
            CompileToken(Segment, STACK_POP);
            CompileToken(Segment, EX);

            // jump if zero
            update_offset = Segment.BCode_Program_size + 2;
            CompileToken(Segment, JUMP_Z, 1, (char *)&jump_offset, sizeof(uint32_t));

            S_TOKEN_TYPE sttResult; // var for inc/dec op
            S_TOKEN_TYPE forOp;
            if (Token.Get() != UNKNOWN)
            {
                SetError("invalid 'for' syntax");
                return false;
            }

            sttResult = DetectUnknown(var_code);
            if (!(sttResult == VARIABLE || sttResult == LOCAL_VARIABLE))
            {
                SetError("invalid 'for' syntax");
                return false;
            }

            forOp = Token.Get();
            if (!(forOp == OP_INC || forOp == OP_DEC))
            {
                SetError("invalid 'for' syntax");
                return false;
            }
            if (CompileAuxiliaryTokens(Segment) != CLOSE_BRACKET)
            {
                SetError("invalid 'for' syntax");
                return false;
            }

            Token_type = CompileAuxiliaryTokens(Segment);
            if (Token_type == BLOCK_IN)
            {
                inout++;
                if (!CompileBlock(Segment, bFunctionBlock, inout, BLOCK_OUT, jump_offset, 6, BreakUpdateTable))
                    return false;
            }
            else
            {
                Token.StepBack();
                if (!CompileBlock(Segment, bFunctionBlock, inout, SEPARATOR, jump_offset, 6, BreakUpdateTable))
                    return false;
                // **** ?
            }

            uint32_t forcont_offset;
            forcont_offset = Segment.BCode_Program_size;
            CompileToken(Segment, sttResult, 1, (char *)&var_code, sizeof(uint32_t));
            CompileToken(Segment, forOp);
            CompileToken(Segment, JUMP, 1, (char *)&jump_offset, sizeof(uint32_t));
            memcpy(&Segment.pCode[update_offset], &Segment.BCode_Program_size, sizeof(uint32_t));
            UpdateOffsets(Segment, BreakUpdateTable, Segment.BCode_Program_size, "b");
            UpdateOffsets(Segment, BreakUpdateTable, forcont_offset, "c");
            BreakUpdateTable.Release();
            break;
        case SWITCH_COMMAND:
            BreakUpdateTable.Release();
            Token_type = CompileAuxiliaryTokens(Segment);
            if (Token_type != OPEN_BRACKET)
            {
                SetError("missed '('");
                return false;
            }


            CompileExpression(Segment);

            Token_type = CompileAuxiliaryTokens(Segment);
            if (Token_type == BLOCK_IN)
            {
                inout++;
                if (!CompileBlock(Segment, bFunctionBlock, inout, BLOCK_OUT, continue_jump, 6, BreakUpdateTable))
                    return false;
            }
            else
            {
                SetError("missed '{'");
                return false;
            }
            UpdateOffsets(Segment, BreakUpdateTable, Segment.BCode_Program_size, "b");
            CopyOffsets(Segment, BreakUpdateTable, BreakTable, "c");
            // UpdateOffsets(Segment,BreakUpdateTable,continue_jump,"c");    // -> ***
            CompileToken(Segment, POP_VOID);
            BreakUpdateTable.Release();
            break;
        case CASE_COMMAND:
            CompileExpression(Segment);
            CompileToken(Segment, STACK_POP);
            CompileToken(Segment, EX);


            if (Token.GetType() != LABEL)
            {
                SetError("missed ':'");
                return false;
            }

            CompileToken(Segment, STACK_COMPARE);

            update_offset = Segment.BCode_Program_size + 2;
            CompileToken(Segment, JUMP_Z, 1, (char *)&jump_offset, sizeof(uint32_t));

            // ??? why INVALID_OFFSET on continue_jump here? dont remember; anyway now change to normal
            if (!CompileBlock(Segment, bFunctionBlock, inout, BREAK_COMMAND, continue_jump, 0, BreakTable))
                return false;

            memcpy(&Segment.pCode[update_offset], &Segment.BCode_Program_size, sizeof(uint32_t));
            break;
        case INVALID_TOKEN:
            SetError("bad token");
            return false;
            break;


        case CALL:

            S_TOKEN_TYPE sttVar;
            uint32_t dwCallVarCode;

            Token_type = CompileAuxiliaryTokens(Segment);
            if (Token_type != UNKNOWN)
            {
                SetError("invalid dynamic call");
                return false;
            }
            sttVar = DetectUnknown(dwCallVarCode);
            if (!(sttVar == VARIABLE || sttVar == LOCAL_VARIABLE))
            {
                SetError("invalid dynamic call");
                return false;
            }

            if (CompileAuxiliaryTokens(Segment) != OPEN_BRACKET)
            {
                SetError("missing '('");
                return false;
            }
            if (Token.Get() == CLOSE_BRACKET)
            {
                // function w/o arguments
                func_args = 0;
            }
            else
            {
                // function with arguments
                bool bNext;
                bNext = true;
                func_args = 1;
                Token.StepBack();
                do
                {
                    CompileExpression(Segment);
                    switch (Token.GetType())
                    {
                    case CLOSE_BRACKET:
                        bNext = false;
                        break;
                    case COMMA:
                        func_args++;
                        break;
                    default:
                        SetError("invalid syntax");
                        return false;
                    }
                } while (bNext);
            }

            CompileToken(Segment, CALL);
            CompileToken(Segment, Token_type, 1, (char *)&dwCallVarCode, sizeof(uint32_t));
            CompileToken(Segment, ARGS_NUM, 1, (char *)&func_args, sizeof(uint32_t));
            CompileToken(Segment, POP_NZ); // pop return value if function returned value

            break;

        case UNKNOWN: {
            uint32_t func_code;
            uint32_t func_args;
            FuncInfo fi;

            if (DetectUnknown(func_code) == CALL_FUNCTION)
            {
                if (CompileAuxiliaryTokens(Segment) != OPEN_BRACKET)
                {
                    SetError("missing '('");
                    return false;
                }
                if (Token.Get() == CLOSE_BRACKET)
                {
                    // function w/o arguments
                    func_args = 0;
                }
                else
                {
                    // function with arguments
                    bool bNext;
                    bNext = true;
                    func_args = 1;
                    Token.StepBack();
                    do
                    {
                        CompileExpression(Segment);
                        switch (Token.GetType())
                        {
                        case CLOSE_BRACKET:
                            bNext = false;
                            break;
                        case COMMA:
                            func_args++;
                            break;
                        default:
                            SetError("invalid syntax");
                            return false;
                        }
                    } while (bNext);
                }

                if (!FuncTab.GetFuncX(fi, func_code))
                {
                    SetError("function not found");
                    return false;
                }

                if (fi.offset == INVALID_FUNC_OFFSET)
                {
                    // external function declared but not compiled yet
                    fi.arguments = fi.extern_arguments;
                }

                // off for debug needs

                if (fi.arguments != func_args)
                {
                    switch (fi.segment_id)
                    {
                    case INTERNAL_SEGMENT_ID:
                        if (!IsIntFuncVarArgsNum(func_code))
                        {
                            SetError("function '%s(args:%d)' doesnt accept %d arguments", fi.name.c_str(), fi.arguments,
                                     func_args);
                            return false;
                        }
                        break;
                    case IMPORTED_SEGMENT_ID:
                        // skip imported funcs checking for now
                        break;
                    default:
                        SetError("function %s(args:%d) doesnt accept %d arguments", fi.name.c_str(), fi.arguments,
                                 func_args);
                        return false;
                    }

                }

                CompileToken(Segment, CALL_FUNCTION, 1, (char *)&func_code, sizeof(uint32_t));
                CompileToken(Segment, ARGS_NUM, 1, (char *)&func_args, sizeof(uint32_t));
                if (fi.return_type != TVOID)
                    CompileToken(Segment, STACK_POP_VOID);
            }
            else if (!CompileUnknown(Segment))
                return false;
        }
        break;

        case EXTERN:
            Token_type = Token.Get();
            bExtern = true;
        case IMPORT:
            if (!bExtern)
            {
                Token_type = Token.Get();
                bImport = true;
            }
        case VAR_REFERENCE:
        case VAR_AREFERENCE:
        case VAR_INTEGER: // create global variable
        case VAR_PTR:
        case VAR_FLOAT:
        case VAR_STRING:
        case VAR_OBJECT:
        case TVOID:
            Token_type = DetectUnknown(unk_code);
            if (Token_type == CALL_FUNCTION)
            {
                // function declaration

                bFunctionBlock = true;

                CurrentFuncCode = FuncTab.FindFunc(Token.GetData());
                if (!bExtern)
                    if (!bImport)
                    {
                        if (!FuncTab.SetFuncOffset(Token.GetData(), Segment.BCode_Program_size))
                        {
                            SetError("Invalid function name: %s", Token.GetData());
                            return false;
                        }

                        auto func_name = Token.GetData();
                        auto cmp = [&func_name](const auto &func) { return storm::iEquals(func.info.name, func_name); };
                        auto it = std::ranges::find_if(script_cache_.functions, cmp);
                        if (it != script_cache_.functions.end())
                        {
                            it->info.offset = Segment.BCode_Program_size;
                        }
                    }
                bExtern = false;
                bImport = false;
                // skip function arguments list in function declaration
                if (Token.Get() != OPEN_BRACKET)
                {
                    SetError("Invalid syntax");
                    return false;
                }

                do
                {
                    Token_type = Token.Get();
                    switch (Token_type)
                    {
                    case VAR_INTEGER:
                    case VAR_PTR:
                    case VAR_STRING:
                    case VAR_FLOAT:
                    case VAR_OBJECT:
                    case VAR_REFERENCE:
                    case VAR_AREFERENCE:
                    case UNKNOWN:
                    case COMMA:
                    case CLOSE_BRACKET:
                    case DEBUG_LINEFEED:
                        break;
                    default:
                        SetError("Invalid function declaration");
                        return false;
                    }
                } while (Token_type != CLOSE_BRACKET);
            }
            else
            {
                if (Token_type == LOCAL_VARIABLE)
                {
                    if (Token.Get() == OP_EQUAL)
                    {
                        // initialization
                        CompileToken(Segment, Token_type, 1, (char *)&unk_code, sizeof(uint32_t));
                    }
                    Token.StepBack();
                }
            }
            // ... skip var or func declaration
            break;

        case FUNCTION_RETURN:
        case FUNCTION_RETURN_VOID:
            Token_type = Token.Get();
            if (Token_type == SEPARATOR)
                CompileToken(Segment, FUNCTION_RETURN_VOID);
            else
            {
                Token.StepBack();
                CompileExpression(Segment); // this will cause function result be pushed into stack
                CompileToken(Segment, STACK_POP);
                CompileToken(Segment, EX);
                CompileToken(Segment, FUNCTION_RETURN);
            }
            Token.StepBack(); // ???

            break;


        case OP_BOOL_NEG:
        case OP_BOOL_EQUAL:
        case OP_GREATER:
        case OP_GREATER_OR_EQUAL:
        case OP_LESSER:
        case OP_LESSER_OR_EQUAL:
        case OP_NOT_EQUAL:
        case OP_MINUS:
        case OP_PLUS:
        case OP_MULTIPLY:
        case OP_DIVIDE:
        case OP_POWER:
        case OP_MODUL:
        case OP_INC:
        case OP_DEC:
        case OP_BOOL_AND:
        case OP_BOOL_OR:
        case COMMA:
        case AND:
        case END_OF_PROGRAMM:
            CompileToken(Segment, Token_type);
            break;


        case OP_INCADD:
        case OP_DECADD:
        case OP_MULTIPLYEQ:
        case OP_DIVIDEEQ:
            CompileExpression(Segment);
            CompileToken(Segment, Token_type);
            CompileToken(Segment, STACK_POP_VOID);
            if (Token.GetType() == bound_type)
            {
                Token.StepBack();
            }

            break;

        case OP_EQUAL:
            if (inout == 0 && !bDebugExpressionRun)
                break; // skip global vars initialization
            CompileExpression(Segment);

            CompileToken(Segment, MOVE);
            CompileToken(Segment, LEFT_OPERAND);
            CompileToken(Segment, STACK_TOP);
            CompileToken(Segment, STACK_POP_VOID);
            Token.StepBack();

            break;


        case SQUARE_OPEN_BRACKET:
            if (!CompileExpression(Segment))
            {
                return false;
            }
            CompileToken(Segment, STACK_POP);
            CompileToken(Segment, LEFTOP_INDEX);

            break;
        case SQUARE_CLOSE_BRACKET:
            SetError("']' without matching '['");
            break;

        case MAKEREF_COMMAND:
            uint32_t dwRCode1, dwRCode2;
            S_TOKEN_TYPE sttVarType1, sttVarType2;

            if (Token.Get() != OPEN_BRACKET)
            {
                SetError("missed '('");
                return false;
            }

            if (Token.Get() != UNKNOWN)
            {
                SetError("invalid 1st operand");
                return false;
            }

            sttVarType1 = DetectUnknown(dwRCode1);

            if (!(sttVarType1 == VARIABLE || sttVarType1 == LOCAL_VARIABLE))
            {
                SetError("invalid 1st operand");
                return false;
            }


            if (Token.Get() != COMMA)
            {
                SetError("missed ','");
                return false;
            }

            if (Token.Get() != UNKNOWN)
            {
                SetError("invalid 1st operand");
                return false;
            }

            sttVarType2 = DetectUnknown(dwRCode2);

            if (!(sttVarType2 == VARIABLE || sttVarType2 == LOCAL_VARIABLE))
            {
                SetError("invalid 2st operand");
                return false;
            }

            if (Token.Get() == SQUARE_OPEN_BRACKET)
            {
                CompileExpression(Segment);

                CompileToken(Segment, STACK_POP);
                CompileToken(Segment, BX);

                CompileToken(Segment, SETREF_BXINDEX);
                CompileToken(Segment, sttVarType1, 1, (char *)&dwRCode1, sizeof(uint32_t));
                CompileToken(Segment, sttVarType2, 1, (char *)&dwRCode2, sizeof(uint32_t));

                if (Token.GetType() != SQUARE_CLOSE_BRACKET)
                {
                    SetError("missed ']'");
                    return false;
                }
                if (Token.Get() != CLOSE_BRACKET)
                {
                    SetError("missed ')'");
                    return false;
                }
            }
            else
            {
                if (Token.GetType() != CLOSE_BRACKET)
                {
                    SetError("missed ')'");
                    return false;
                }
                CompileToken(Segment, SETREF);
                CompileToken(Segment, sttVarType1, 1, (char *)&dwRCode1, sizeof(uint32_t));
                CompileToken(Segment, sttVarType2, 1, (char *)&dwRCode2, sizeof(uint32_t));
            }


            break;


        case MAKEAREF_COMMAND:

            // CompileToken(Segment,Token_type);    // debug
            S_TOKEN_TYPE sttVarType;

            if (Token.Get() != OPEN_BRACKET)
            {
                SetError("missed '('");
                return false;
            }
            Token_type = Token.Get();
            if (Token_type != UNKNOWN)
            {
                SetError("invalid 1st argument");
                return false;
            }
            Token_type = DetectUnknown(dwRCode);
            if (!(Token_type == VARIABLE || Token_type == LOCAL_VARIABLE))
            {
                SetError("invalid 1st argument");
                return false;
            }

            if (Token_type == VARIABLE)
            {
                real_var = VarTab.GetVarX(dwRCode);
                if (real_var == nullptr)
                {
                    SetError("Invalid var code");
                    return false;
                }

                if (!(real_var->type == VAR_AREFERENCE || real_var->type == VAR_REFERENCE))
                {
                    SetError("not aref or ref");
                    return false;
                }
            }
            else
            {
                FuncTab.GetVar(lvi, CurrentFuncCode, dwRCode);
                if (!(lvi.type == VAR_AREFERENCE || lvi.type == VAR_REFERENCE))
                {
                    SetError("not aref or ref");
                    return false;
                }
            }

            CompileToken(Segment, STACK_ALLOC);
            CompileToken(Segment, SETREF);
            CompileToken(Segment, STACK_TOP);
            CompileToken(Segment, Token_type, 1, (char *)&dwRCode, sizeof(uint32_t));

            if (Token.Get() != COMMA)
            {
                SetError("missed ','");
                return false;
            }

            sttVarType = Token.Get();
            if (sttVarType != UNKNOWN)
            {
                SetError("invalid 2d argument");
                return false;
            }
            sttVarType = DetectUnknown(dwRCode);
            if (!(sttVarType == VARIABLE || sttVarType == LOCAL_VARIABLE))
            {
                SetError("invalid 2d argument");
                return false;
            }

            Token_type = Token.Get();
            if (Token_type == SQUARE_OPEN_BRACKET)
            {
                CompileExpression(Segment);
                if (Token.GetType() != SQUARE_CLOSE_BRACKET)
                {
                    SetError("missing ']'");
                    return false;
                }
                CompileToken(Segment, STACK_POP);
                CompileToken(Segment, BX);

                // move ap to AP register
                CompileToken(Segment, MOVEAP_BXINDEX);
                CompileToken(Segment, AP);
                CompileToken(Segment, sttVarType, 1, (char *)&dwRCode, sizeof(uint32_t));
                Token_type = Token.Get();

                CompileToken(Segment, PUSH_OBJID_BXINDEX);
                CompileToken(Segment, sttVarType, 1, (char *)&dwRCode, sizeof(uint32_t));
            }
            else
            {
                // bove atributes root to AP
                CompileToken(Segment, MOVEAP);
                CompileToken(Segment, AP);
                CompileToken(Segment, sttVarType, 1, (char *)&dwRCode, sizeof(uint32_t));

                CompileToken(Segment, PUSH_OBJID);
                CompileToken(Segment, sttVarType, 1, (char *)&dwRCode, sizeof(uint32_t));
            }

            if (Token_type != DOT)
            {
                // set attribute pointer to referenced var
                CompileToken(Segment, SETAREF);
                CompileToken(Segment, STACK_TOP);
                CompileToken(Segment, AP);
                CompileToken(Segment, STACK_POP_VOID);
                break;
            }

            // advance ap

            sttResult = Token.Get();
            uint32_t dwAWCode;
            uint32_t dwVarCode;
            do
            {
                if (Token.GetType() == OPEN_BRACKET)
                {
                    // access var
                    sttResult = Token.Get();
                    if (sttResult != UNKNOWN)
                    {
                        SetError("Invalid access var syntax");
                        return false;
                    }
                    sttResult = DetectUnknown(dwVarCode);
                    if (!(sttResult == VARIABLE || sttResult == LOCAL_VARIABLE))
                    {
                        SetError("not variable: %s", Token.GetData());
                        return false;
                    }
                    CompileToken(Segment, VERIFY_AP);
                    CompileToken(Segment, sttResult, 1, &dwVarCode, sizeof(uint32_t));
                    if (Token.Get() != CLOSE_BRACKET)
                    {
                        SetError("missing ')'");
                        return false;
                    }
                }
                else
                {
                    // access word
                    if (Token.GetData() == nullptr)
                    {
                        SetError("Invalid access string");
                        return false;
                    }
                    Token.LowCase();
                    dwAWCode = SCodec.Convert(Token.GetData());
                    CompileToken(Segment, VERIFY_AP);
                    CompileToken(Segment, ACCESS_WORD_CODE, 1, &dwAWCode, sizeof(uint32_t));
                }
                sttResult = Token.Get();
                if (sttResult == DOT)
                    Token.Get(); // get next token (after dot)
            } while (sttResult == DOT);

            CompileToken(Segment, SETAREF);
            CompileToken(Segment, STACK_TOP);
            CompileToken(Segment, AP);
            CompileToken(Segment, STACK_POP_VOID);
            break;

        case DOT:
            Token_type = Token.Get();
            if (Token_type == OPEN_BRACKET)
            {
                // access word inside variable
                Token.Get(); // variable name
                var_code = FuncTab.FindVar(CurrentFuncCode, Token.GetData());
                if (var_code != INVALID_VAR_CODE)
                {
                    Token.Get();
                    if (Token.GetType() != CLOSE_BRACKET)
                    {
                        SetError("no mathcing ')'");
                        return false;
                    }
                    CompileToken(Segment, ACCESS_VAR);
                    CompileToken(Segment, LOCAL_VARIABLE, 1, (char *)&var_code, sizeof(uint32_t));
                    break;
                }

                var_code = VarTab.FindVar(Token.GetData());
                if (var_code != INVALID_VAR_CODE)
                {
                    Token.Get();
                    if (Token.GetType() != CLOSE_BRACKET)
                    {
                        SetError("no mathcing ')'");
                        return false;
                    }
                    CompileToken(Segment, ACCESS_VAR);
                    CompileToken(Segment, VARIABLE, 1, (char *)&var_code, sizeof(uint32_t));
                    break;
                }

                SetError("Invalid access string");
                return false;
            }
            // else its access word
            if (Token.GetData() == nullptr)
            {
                SetError("Invalid access string");
                return false;
            }

            Token.LowCase();
            awcode = SCodec.Convert(Token.GetData());
            CompileToken(Segment, ACCESS_WORD_CODE, 1, &awcode, sizeof(uint32_t));
            break;

        case TRUE_CONST:
            lvalue = 1;
            CompileToken(Segment, NUMBER, 1, (char *)&lvalue, sizeof(int32_t));
            break;
        case FALSE_CONST:
            lvalue = 0;
            CompileToken(Segment, NUMBER, 1, (char *)&lvalue, sizeof(int32_t));
            break;

        case NUMBER:
            CompileNumber(Segment);
            break;
        case FLOAT_NUMBER:
            CompileFloatNumber(Segment);
            break;
        case STRING:
            CompileString(Segment);
            break;

        case LABEL: // register label

            break;
        case DEFINE_COMMAND: // create define

            break;

        default:

            break;
        }
    } while (Token_type != END_OF_PROGRAMM);

    return true;
}

bool COMPILER::BC_Jump(SEGMENT_DESC &Segment, uint32_t offset)
{
    if (offset >= Segment.BCode_Program_size)
    {
        SetError("invalid jump");
        return false;
    }
    InstructionPointer = offset;
    return true;
}

// The expression register as the integer a conditional jump tests. A register that does not convert leaves `value`
// as it was (the jump then follows the previous condition, as it always did).
inline void COMPILER::ReadCondition(int32_t &value)
{
    if (ExpressionResult.Data_type == VAR_INTEGER && !ExpressionResult.bArray)
    {
        value = ExpressionResult.lValue;
        return;
    }
    ExpressionResult.Convert(VAR_INTEGER);
    ExpressionResult.Get(value);
}

// Reads the token at InstructionPointer: its type, where its data starts (ip, TLR_DataOffset) and the data size
// (one byte, or 0xff and four bytes); InstructionPointer moves to the next token.
S_TOKEN_TYPE COMPILER::BC_TokenGet(uint32_t &ip, uint32_t &token_data_size)
{
    const char *const code = pRunCodeBase;
    uint32_t at = InstructionPointer;
    const auto type = static_cast<S_TOKEN_TYPE>(code[at]);
    uint32_t size = static_cast<uint8_t>(code[at + 1]);
    at += 2;
    if (size == 0xff)
    {
        memcpy(&size, &code[at], sizeof(uint32_t));
        at += sizeof(uint32_t);
    }
    TokenLastReadResult = type;
    ip = at;
    TLR_DataOffset = at;
    token_data_size = size;
    InstructionPointer = at + size;
    return type;
}

// For the callers that read the token's data through TLR_DataOffset.
S_TOKEN_TYPE COMPILER::BC_TokenGet()
{
    uint32_t ip;
    uint32_t token_data_size;
    return BC_TokenGet(ip, token_data_size);
}

bool COMPILER::BC_CallFunction(uint32_t func_code, uint32_t &ip, DATA *&pVResult)
{
    CompilerStage = CS_RUNTIME;

    const FuncInfo *const call_pfi = FuncTab.GetFuncPtr(func_code);
    if (call_pfi == nullptr)
    {
        SetError("Invalid function call");
        return false;
    }
    // no deep copy: the table entry address is stable (see FuncTable::funcs_)
    const FuncInfo &call_fi = *call_pfi;
    const uint32_t call_segment_id = call_fi.segment_id;
    const S_TOKEN_TYPE call_return_type = call_fi.return_type;
    const SIMPORTFUNC call_imported_func = call_fi.imported_func;

    // TODO: only do if stack debug if enabled (should be runtime configurable)
    // push function details to call stack
    storm::ringbuffer_stack_push_guard push_guard(callStack_);

    // number f arguments pushed into stack for this function call
    if (BC_TokenGet() != ARGS_NUM)
    {
        push_guard.push(std::make_tuple(call_fi.decl_file_name.c_str(), call_fi.decl_line, call_fi.name.c_str(), kNoArguments, 0U));
        SetError("missing args_num token");
        return false;
    }
    const uint32_t arguments = *((int32_t *)&pRunCodeBase[TLR_DataOffset]);

    const uint32_t check_sp = SStack.GetDataNum() - arguments;
    push_guard.push(std::make_tuple(call_fi.decl_file_name.c_str(), call_fi.decl_line, call_fi.name.c_str(), check_sp, arguments));

    // the caller's position, restored after the callee has run
    const uint32_t mem_InstructionPointer = InstructionPointer;
    const uint32_t mem_ip = ip;
    FuncFrame *const mem_pfi = pRun_fi;
    const char *const mem_codebase = pRunCodeBase;
    const auto restore = [&] {
        pRun_fi = mem_pfi;
        InstructionPointer = mem_InstructionPointer;
        ip = mem_ip;
        if (pRun_fi)
            RunningSegmentID = pRun_fi->segment_id;
        pRunCodeBase = mem_codebase;
    };

    if (call_segment_id == INTERNAL_SEGMENT_ID)
    {
        BC_CallIntFunction(func_code, pVResult, arguments);
    }
    else if (call_segment_id == IMPORTED_SEGMENT_ID)
    {
        pVResult = nullptr;
        const uint32_t nResult = call_imported_func(&SStack);
        if (nResult == IFUNCRESULT_OK)
        {
            if (call_return_type != TVOID)
            {
                pVResult = SStack.Read();
            }
        }
    }
    else
    {
        BC_Execute(func_code, pVResult);
    }

    restore();

    if (abort_execution_)
    {
        // A native function aborted the running scripts (AbortExecution): no stack check, the caller's frame
        // returns too and the outermost one drops what the aborted frames left.
        return false;
    }

    if (check_sp != SStack.GetDataNum() - (pVResult ? 1 : 0))
    {
        if (call_segment_id != INTERNAL_SEGMENT_ID && call_segment_id != IMPORTED_SEGMENT_ID &&
            arguments != call_fi.arguments)
            SetError("function '%s' stack error: declared with %u arguments, called with %u", call_fi.name.c_str(),
                     call_fi.arguments, arguments);
        else
            SetError("function '%s' stack error", call_fi.name.c_str());
        DropStrayStackValues(check_sp, pVResult);
    }

    return true;
}

// After a call that left more values than its result above `base` (a script function called with more arguments
// than it declares: its frame starts at its own last arguments and the first ones stay behind), the values are
// dropped and the result moved down to `base`, so the caller's expression finds its own operands under it again
// (`"a" + RandPhraseSimple(x, y, z)` would otherwise add the result to x and leave "a" on the stack). A call that
// took fewer values than it should (a call with too few arguments uses the caller's values) cannot be undone here;
// the compiler reports both at compile time (CheckCallArguments).
void COMPILER::DropStrayStackValues(uint32_t base, DATA *&result)
{
    const uint32_t keep = base + (result ? 1 : 0);
    if (SStack.GetDataNum() <= keep)
        return;
    if (result)
    {
        DATA *const slot = SStack.Read(base, 0);
        slot->SetType(UNKNOWN); // as a push does
        slot->Copy(result);
        result = slot;
    }
    SStack.InvalidateFrom(keep);
}

// `call name()` leaves 1 (the callee returned a value) or 0 in the expression register. The
// callee may have left a reference there (`ref r = CharacterFromID(...)` does), and DATA::Set
// writes through a reference, which would overwrite the referenced object with the status; the
// register's previous type is therefore dropped first.
void COMPILER::StoreDynamicCallStatus(bool returned_value)
{
    ExpressionResult.ClearType();
    ExpressionResult.Set(static_cast<int32_t>(returned_value ? 1 : 0));
}

std::optional<int32_t> COMPILER::FindIntegerDefine(const char *name)
{
    DEFINFO di;
    const uint32_t def_code = DefTab.FindDef(name);
    if (def_code == INVALID_DEF_CODE || !DefTab.GetDef(di, def_code) || di.deftype != NUMBER)
        return std::nullopt;
    int32_t value;
    memcpy(&value, &di.data4b, sizeof(value));
    return value;
}

bool COMPILER::BC_Execute(uint32_t function_code, DATA *&pVReturnResult, const char *pDbgExpSource)
{
    uint32_t segment_index;
    uint32_t token_data_size;
    uint32_t ip;
    uint32_t n;
    uint32_t jump_offset;
    uint32_t func_code;
    uint32_t nLeftOperandCode;
    uint32_t bLeftOperandType;
    int32_t nLeftOperandIndex;
    S_TOKEN_TYPE Token_type;
    // fi: stable table entry (or an empty one for debug expressions), never copied.
    // frame: this invocation's scalar snapshot, including its own stack offset.
    static const FuncInfo empty_fi;
    const FuncInfo *pfi = &empty_fi;
    FuncFrame frame(empty_fi);
    const VarInfo *real_var;
    DATA *pV;
    DATA *pVResult;
    //    DATA   ExpressionResult;    // while compile expression not ready, each function have its own register
    const char *pCodeBase;
    int32_t lvalue;
    S_TOKEN_TYPE vtype;
    DATA *pVV;
    uint32_t var_code;
    const char *pAccess_string;
    DATA *pVar;
    // AbortExecution unwinds every frame (CALL_FUNCTION returns false); the outermost one drops the stack values
    // the aborted frames left and clears the request.
    struct ExecuteDepth
    {
        COMPILER &compiler;
        const FuncFrame &frame;
        ~ExecuteDepth()
        {
            if (--compiler.execute_depth_ != 0)
                return;
            storm::ScriptRunning(false);
            if (compiler.abort_execution_)
            {
                compiler.abort_execution_ = false;
                compiler.SStack.InvalidateFrom(frame.stack_offset);
            }
        }
    } execute_depth{*this, frame};
    if (execute_depth_++ == 0)
        storm::ScriptRunning(true);
    entid_t eid;
    ATTRIBUTES *pLeftOperandAClass;
    bool bDebugWaitForThisFunc;
    DATA *pVDst;
    DATA *pVSrc;
    bool bUseIndex;
    int32_t dwBXIndex;

    CompilerStage = CS_RUNTIME;

    bDebugWaitForThisFunc = false;

    pVReturnResult = nullptr;
    pVResult = nullptr;

    if (pDbgExpSource == nullptr)
    {
        if (const auto *table_fi = FuncTab.GetFuncPtr(function_code); table_fi != nullptr)
        {
            pfi = table_fi;
            frame = FuncFrame(*table_fi);
        }
        else
        {
            SetError("Invalid function: %s", pfi->name.c_str());
            return false;
        }

        if (frame.offset == INVALID_FUNC_OFFSET)
        {
            SetError("Function (%s) isnt loaded", pfi->name.c_str());
            return false;
        }

        if (frame.segment_id == INTERNAL_SEGMENT_ID)
        {
            SetError("Function (%s) is internal", pfi->name.c_str());
            return false;
        }

        if (frame.segment_id == IMPORTED_SEGMENT_ID)
        {
            SetError("Function (%s) is imported", pfi->name.c_str());
            return false;
        }

        segment_index = GetSegmentIndex(frame.segment_id);
        if (segment_index == INVALID_SEGMENT_INDEX)
        {
            SetError("Function (%s) segment not loaded", pfi->name.c_str());
            return false;
        }
        if (SegmentTable[segment_index].pCode == nullptr)
        {
            SetError("Segment (%s) not loaded", SegmentTable[segment_index].name.c_str());
            return false;
        }


        RunningSegmentID = frame.segment_id;
        frame.stack_offset = SStack.GetDataNum() - frame.arguments; // set stack offset

        for (n = 0; n < frame.arguments; n++)
        {
            if (pfi->local_vars[n].type == VAR_REFERENCE)
                continue;
            pV = SStack.Read(frame.stack_offset, n);
            if (pV->GetType() != pfi->local_vars[n].type)
            {
                pV = pV->GetVarPointer();
                if (!pV)
                {
                    SetError("uninitialized function argument");
                    return false;
                }

                if (pfi->local_vars[n].type == VAR_AREFERENCE && pV->GetType() == VAR_OBJECT)
                    continue;

                // TODO: remove and fix
                if (false && pV->GetType() != pfi->local_vars[n].type)
                {
                    SetWarning("wrong type of argument %d  %s(%s) <-- [%s]", n, pfi->name.c_str(),
                               Token.GetTypeName(pfi->local_vars[n].type), Token.GetTypeName(pV->GetType()));
                }
            }
        }

        for (n = frame.arguments; n < pfi->local_vars.size(); n++)
        {
            pV = SStack.Push();
            pV->SetType(pfi->local_vars[n].type, pfi->local_vars[n].elements);
        }

        pRun_fi = &frame; // set pointer to 'this' function info

        InstructionPointer = frame.offset;

        pCodeBase = SegmentTable[segment_index].pCode;
        pRunCodeBase = pCodeBase;
    }
    else
    {
        InstructionPointer = 0;
        pCodeBase = pDbgExpSource;
        pRunCodeBase = pCodeBase;
    }

    nLeftOperandIndex = INVALID_ARRAY_INDEX;
    nLeftOperandCode = INVALID_VAR_CODE;
    bLeftOperandType = 0xffffffff;
    pLeftOperandAClass = nullptr;

    do
    {
        // A sixth of the tokens only mark the source line and the end of a statement.
        do
        {
            Token_type = BC_TokenGet(ip, token_data_size);
            if (Token_type == DEBUG_LINE_CODE && core_internal.Exit_flag)
                return false;
        } while (Token_type == DEBUG_LINE_CODE || Token_type == SEPARATOR);

        switch (Token_type)
        {
        case ARGS_NUM:
            psnip_trap();
            break;
        case STACK_COMPARE:
            pV = SStack.Read();
            ExpressionResult.CompareAndSetResult(pV, OP_BOOL_EQUAL);
            break;
        case POP_EXPRESULT:
            pV = SStack.Pop();
            ExpressionResult.ClearType();
            ExpressionResult.Copy(pV);
            break;
        case PUSH_EXPRESULT:
            pV = SStack.Push();
            pV->Copy(&ExpressionResult);
            break;
        case POP_NZ:
            ReadCondition(lvalue);
            if (lvalue == 0)
                break;
            SStack.Pop();
            break;
        case POP_VOID:
            SStack.Pop();
            break;
        case JUMP:
            memcpy(&jump_offset, &pCodeBase[ip], sizeof(uint32_t));
            if (!BC_Jump(SegmentTable[segment_index], jump_offset))
                return false;
            break;
        case JUMP_Z:
            memcpy(&jump_offset, &pCodeBase[ip], sizeof(uint32_t));
            ReadCondition(lvalue);
            if (lvalue)
                break;
            if (!BC_Jump(SegmentTable[segment_index], jump_offset))
                return false;
            break;
        case JUMP_NZ:
            memcpy(&jump_offset, &pCodeBase[ip], sizeof(uint32_t));
            ReadCondition(lvalue);
            if (lvalue == 0)
                break;
            if (!BC_Jump(SegmentTable[segment_index], jump_offset))
                return false;
            break;
        case LOCAL_VARIABLE:
            pLeftOperandAClass = nullptr;            // reset attribute
            nLeftOperandIndex = INVALID_ARRAY_INDEX; // reset index
            nLeftOperandCode = *((int32_t *)&pRunCodeBase[TLR_DataOffset]);
            bLeftOperandType = LOCAL_VARIABLE;
            break;
        case VARIABLE:
            pLeftOperandAClass = nullptr;            // reset attribute
            nLeftOperandIndex = INVALID_ARRAY_INDEX; // reset index
            nLeftOperandCode = *((int32_t *)&pRunCodeBase[TLR_DataOffset]);
            bLeftOperandType = VARIABLE;
            break;
        case ACCESS_WORD_CODE:
            if (nLeftOperandCode == INVALID_VAR_CODE)
            {
                SetError("no Lvalue");
                return false;
            }

            if (nLeftOperandIndex == INVALID_ARRAY_INDEX)
            {
                if (pLeftOperandAClass == nullptr)
                {
                    if (bLeftOperandType == VARIABLE)
                    {
                        real_var = VarTab.GetVar(nLeftOperandCode);
                        if (real_var == nullptr)
                        {
                            SetError("Global variable not found");
                            return false;
                        }

                        pV = real_var->value.get();
                    }
                    else
                    {
                        pV = SStack.Read(pRun_fi->stack_offset, nLeftOperandCode);
                        if (pV == nullptr)
                        {
                            SetError("Local variable not found");
                            return false;
                        }
                    }
                    pLeftOperandAClass = pV->GetAClass();
                }
                if (pLeftOperandAClass == nullptr)
                {
                    SetError("AClass ERROR n1");
                    return false;
                }
                pLeftOperandAClass =
                    pLeftOperandAClass->VerifyAttributeClassByCode(*((int32_t *)&pRunCodeBase[TLR_DataOffset]));
                break;
            }
            if (pLeftOperandAClass == nullptr)
            {
                if (bLeftOperandType == VARIABLE)
                {
                    real_var = VarTab.GetVar(nLeftOperandCode);
                    if (real_var == nullptr)
                    {
                        SetError("Global variable not found");
                        return false;
                    }

                    pV = real_var->value.get();
                }
                else
                {
                    pV = SStack.Read(pRun_fi->stack_offset, nLeftOperandCode);
                    if (pV == nullptr)
                    {
                        SetError("Local variable not found");
                        return false;
                    }
                }
                pLeftOperandAClass = pV->GetAClass(nLeftOperandIndex);
            }
            if (pLeftOperandAClass == nullptr)
            {
                SetError("AClass ERROR n1");
                return false;
            }
            pLeftOperandAClass =
                pLeftOperandAClass->VerifyAttributeClassByCode(*((int32_t *)&pRunCodeBase[TLR_DataOffset]));
            break;
            break;
        case ACCESS_WORD:
            if (nLeftOperandCode == INVALID_VAR_CODE)
            {
                SetError("no Lvalue");
                return false;
            }
            if (nLeftOperandIndex == INVALID_ARRAY_INDEX)
            {
                if (pLeftOperandAClass == nullptr)
                {
                    if (bLeftOperandType == VARIABLE)
                    {
                        real_var = VarTab.GetVar(nLeftOperandCode);
                        if (real_var == nullptr)
                        {
                            SetError("Global variable not found");
                            return false;
                        }

                        pV = real_var->value.get();
                    }
                    else
                    {
                        pV = SStack.Read(pRun_fi->stack_offset, nLeftOperandCode);
                        if (pV == nullptr)
                        {
                            SetError("Local variable not found");
                            return false;
                        }
                    }
                    pLeftOperandAClass = pV->GetAClass();
                }
                if (pLeftOperandAClass == nullptr)
                {
                    SetError("AClass ERROR n1");
                    return false;
                }
                pLeftOperandAClass = pLeftOperandAClass->VerifyAttributeClass((char *)&pRunCodeBase[TLR_DataOffset]);
                break;
            }
            if (pLeftOperandAClass == nullptr)
            {
                if (bLeftOperandType == VARIABLE)
                {
                    real_var = VarTab.GetVar(nLeftOperandCode);
                    if (real_var == nullptr)
                    {
                        SetError("Global variable not found");
                        return false;
                    }

                    pV = real_var->value.get();
                }
                else
                {
                    pV = SStack.Read(pRun_fi->stack_offset, nLeftOperandCode);
                    if (pV == nullptr)
                    {
                        SetError("Local variable not found");
                        return false;
                    }
                }
                pLeftOperandAClass = pV->GetAClass(nLeftOperandIndex);
            }
            if (pLeftOperandAClass == nullptr)
            {
                SetError("AClass ERROR n1");
                return false;
            }
            pLeftOperandAClass = pLeftOperandAClass->VerifyAttributeClass((char *)&pRunCodeBase[TLR_DataOffset]);
            break;
            break;
        case ACCESS_VAR:
            if (nLeftOperandCode == INVALID_VAR_CODE)
            {
                SetError("no Lvalue");
                return false;
            }
            if (nLeftOperandIndex == INVALID_ARRAY_INDEX)
            {
                if (pLeftOperandAClass == nullptr)
                {
                    if (bLeftOperandType == VARIABLE)
                    {
                        real_var = VarTab.GetVar(nLeftOperandCode);
                        if (real_var == nullptr)
                        {
                            SetError("Global variable not found");
                            return false;
                        }

                        pV = real_var->value.get();
                    }
                    else
                    {
                        pV = SStack.Read(pRun_fi->stack_offset, nLeftOperandCode);
                        if (pV == nullptr)
                        {
                            SetError("Local variable not found");
                            return false;
                        }
                    }
                    pLeftOperandAClass = pV->GetAClass();
                }
                if (pLeftOperandAClass == nullptr)
                {
                    SetError("AClass ERROR n1");
                    return false;
                }

                vtype = BC_TokenGet();
                var_code = *((int32_t *)&pRunCodeBase[TLR_DataOffset]);
                if (!(vtype == VARIABLE || vtype == LOCAL_VARIABLE))
                {
                    SetError("invalid access var");
                    return false;
                }
                if (vtype == VARIABLE)
                {
                    real_var = VarTab.GetVar(var_code);
                    if (real_var == nullptr)
                    {
                        SetError("Global variable not found");
                        return false;
                    }

                    pVV = real_var->value.get();
                }
                else
                {
                    pVV = SStack.Read(pRun_fi->stack_offset, var_code);
                    if (pVV == nullptr)
                    {
                        SetError("Local variable not found");
                        return false;
                    }
                }
                pVV->Get(pAccess_string);

                pLeftOperandAClass = pLeftOperandAClass->CreateSubAClass(pLeftOperandAClass, pAccess_string);
                break;
            }
            if (pLeftOperandAClass == nullptr)
            {
                if (bLeftOperandType == VARIABLE)
                {
                    real_var = VarTab.GetVar(nLeftOperandCode);
                    if (real_var == nullptr)
                    {
                        SetError("Global variable not found");
                        return false;
                    }

                    pV = real_var->value.get();
                }
                else
                {
                    pV = SStack.Read(pRun_fi->stack_offset, nLeftOperandCode);
                    if (pV == nullptr)
                    {
                        SetError("Local variable not found");
                        return false;
                    }
                }
                pLeftOperandAClass = pV->GetAClass(nLeftOperandIndex);
            }
            if (pLeftOperandAClass == nullptr)
            {
                SetError("AClass ERROR n1");
                return false;
            }

            vtype = BC_TokenGet();
            var_code = *((int32_t *)&pRunCodeBase[TLR_DataOffset]);
            if (!(vtype == VARIABLE || vtype == LOCAL_VARIABLE))
            {
                SetError("invalid access var");
                return false;
            }
            if (vtype == VARIABLE)
            {
                real_var = VarTab.GetVar(var_code);
                if (real_var == nullptr)
                {
                    SetError("Global variable not found");
                    return false;
                }

                pVV = real_var->value.get();
            }
            else
            {
                pVV = SStack.Read(pRun_fi->stack_offset, var_code);
                if (pVV == nullptr)
                {
                    SetError("Local variable not found");
                    return false;
                }
            }
            pVV->Get(pAccess_string);

            pLeftOperandAClass = pLeftOperandAClass->CreateSubAClass(pLeftOperandAClass, pAccess_string);

            break;
            break;
        case DEBUG_FILE_NAME:

            break;
        case BLOCK_IN:
        case BLOCK_OUT:
            SetError("Unallowed instraction");
            return false;
            break;
        default:
            SetError("Incorrect instruction code");
            return false;

        case CALL:                                                  // undetermined function call
            vtype = BC_TokenGet();                                  // read variable
            var_code = *((int32_t *)&pRunCodeBase[TLR_DataOffset]); // var code
            if (vtype == VARIABLE)
            {
                real_var = VarTab.GetVar(var_code);
                if (real_var == nullptr)
                {
                    SetError("Global variable not found");
                    return false;
                }

                pVV = real_var->value.get();
            }
            else
            {
                pVV = SStack.Read(pRun_fi->stack_offset, var_code);
                if (pVV == nullptr)
                {
                    SetError("Local variable not found");
                    return false;
                }
            }
            pVV = pVV->GetVarPointer();
            if (pVV->GetType() != VAR_STRING)
            {
                SetError("string call argument isnt string var");
                return false;
            }
            pVV->Get(pAccess_string); // func name
            func_code = FuncTab.FindFunc(pAccess_string);
            if (func_code == INVALID_FUNC_CODE)
            {
                SetError("function '%s' not found", pAccess_string);
                return false;
            }
            pVResult = nullptr;
            if (!BC_CallFunction(func_code, ip, pVResult))
                return false;
            StoreDynamicCallStatus(pVResult != nullptr);
            break;
        case CALL_FUNCTION:
            memcpy(&func_code, &pCodeBase[ip], sizeof(uint32_t));
            pVResult = nullptr;
            if (!BC_CallFunction(func_code, ip, pVResult))
                return false;
            break;
        case FUNCTION_RETURN_VOID:
            if (frame.return_type != TVOID)
            {
                SetError("function must return value");
                return false;
            }
            SStack.InvalidateFrom(frame.stack_offset);

            return true;
        case FUNCTION_RETURN:
            if (frame.return_type == TVOID)
            {
                SetError("void function return value");

                return false;
            }
            // at this moment result expression placed in EX register

            if (pDbgExpSource == nullptr) // skip stack unwind for dbg expression process    // ????????
                SStack.InvalidateFrom(frame.stack_offset);

            pV = SStack.Push();
            pV->Copy(&ExpressionResult);
            pVReturnResult = pV;

            if (pDbgExpSource == nullptr) // skip test for dbg expression process
                if (frame.return_type != pV->GetType())
                {
                    if (frame.return_type == VAR_INTEGER && pV->GetType() == VAR_PTR)
                    {
                        pV->Convert(VAR_INTEGER);
                        return true;
                    }

                    SetError("%s function return %s value", Token.GetTypeName(frame.return_type),
                             Token.GetTypeName(pV->GetType()));
                    return false;
                }
            return true;


        case OP_MULTIPLYEQ:
        case OP_DIVIDEEQ:
        case OP_INCADD:
        case OP_DECADD:
            S_TOKEN_TYPE OpType;
            OpType = Token_type;
            if (nLeftOperandCode == INVALID_VAR_CODE)
            {
                SetError("no Lvalue");
                return false;
            }
            if (bLeftOperandType == VARIABLE)
            {
                real_var = VarTab.GetVar(nLeftOperandCode);
                if (real_var == nullptr)
                {
                    SetError("Global variable not found");
                    return false;
                }

                pV = real_var->value.get();
            }
            else
            {
                pV = SStack.Read(pRun_fi->stack_offset, nLeftOperandCode);
                if (pV == nullptr)
                {
                    SetError("Local variable not found");
                    return false;
                }
            }
            if (nLeftOperandIndex != INVALID_ARRAY_INDEX)
            {
                pV = static_cast<DATA *>(pV->GetArrayElement(nLeftOperandIndex));
                if (pV == nullptr)
                {
                    SetError("invalid array index");
                    return false;
                }
            }
            pVV = SStack.Read();
            switch (OpType)
            {
            case OP_INCADD:
                pV->Plus(pVV);
                break;
            case OP_DECADD:
                pV->Minus(pVV);
                break;
            case OP_MULTIPLYEQ:
                pV->Multiply(pVV);
                break;
            case OP_DIVIDEEQ:
                pV->Divide(pVV);
                break;
            }

            break;

        case OP_INC:
            if (nLeftOperandCode == INVALID_VAR_CODE)
            {
                SetError("no Lvalue");
                return false;
            }
            if (bLeftOperandType == VARIABLE)
            {
                real_var = VarTab.GetVar(nLeftOperandCode);
                if (real_var == nullptr)
                {
                    SetError("Global variable not found");
                    return false;
                }

                pV = real_var->value.get();
            }
            else
            {
                pV = SStack.Read(pRun_fi->stack_offset, nLeftOperandCode);
                if (pV == nullptr)
                {
                    SetError("Local variable not found");
                    return false;
                }
            }
            if (nLeftOperandIndex == INVALID_ARRAY_INDEX)
                pV->Inc();
            else
            {
                pV = static_cast<DATA *>(pV->GetArrayElement(nLeftOperandIndex));
                if (pV == nullptr)
                {
                    SetError("invalid array index");
                    return false;
                }
                pV->Inc();
            }
            break;
        case OP_DEC:
            if (nLeftOperandCode == INVALID_VAR_CODE)
            {
                SetError("no Lvalue");
                return false;
            }
            if (bLeftOperandType == VARIABLE)
            {
                real_var = VarTab.GetVar(nLeftOperandCode);
                if (real_var == nullptr)
                {
                    SetError("Global variable not found");
                    return false;
                }

                pV = real_var->value.get();
            }
            else
            {
                pV = SStack.Read(pRun_fi->stack_offset, nLeftOperandCode);
                if (pV == nullptr)
                {
                    SetError("Local variable not found");
                    return false;
                }
            }
            if (nLeftOperandIndex == INVALID_ARRAY_INDEX)
                pV->Dec();
            else
            {
                pV = static_cast<DATA *>(pV->GetArrayElement(nLeftOperandIndex));
                if (pV == nullptr)
                {
                    SetError("invalid array index");
                    return false;
                }
                pV->Dec();
            }
            break;



        case GOTO_COMMAND:
            break;
        case CONTINUE_COMMAND:
            break;
        case BREAK_COMMAND:
            break;

        case CASE_COMMAND:
            return false;

        case UNKNOWN:
            break;
        case VAR_OBJECT:
        case VAR_STRING:
        case VAR_FLOAT:
        case VAR_INTEGER:
        case VAR_PTR:
            break;
        case OPEN_BRACKET:
            break;
        case CLOSE_BRACKET:
            break;
        case COMMA:

            break;
        case DOT:

            break;

        case NUMBER:

            break;
        case FLOAT_NUMBER:

            break;
        case STRING:

            break;

        case PUSH_OBJID_BXINDEX:
        case PUSH_OBJID:

            if (Token_type == PUSH_OBJID_BXINDEX)
            {
                bUseIndex = true;
                rBX.Get(dwBXIndex);
            }
            else
                bUseIndex = false;

            pV = SStack.Push();
            pV->ClearType();

            pVV = GetOperand(pCodeBase, ip, &Token_type);

            if (!(Token_type == VARIABLE || Token_type == LOCAL_VARIABLE))
            {
                SetError("invalid arg for push_objid");
                return false;
            }
            if (bUseIndex)
            {
                pVV = pVV->GetArrayElement(dwBXIndex);
            }

            if (!(pVV->GetType() == VAR_AREFERENCE || pVV->GetType() == VAR_REFERENCE || pVV->GetType() == VAR_OBJECT))
            {
                SetError("not aref or ref or object");
            }

            pVV->Get(eid);
            pV->Set(eid);
            break;

        case SETAREF:

            pV = SStack.Pop();
            pV->Get(eid);

            // destination
            Token_type = BC_TokenGet(ip, token_data_size);
            switch (Token_type)
            {
            case STACK_TOP:
                pVDst = SStack.Read();
                break;
            default:
                SetError("invalid argument for setaref (dst)");
                return false;
            }
            if (!pVDst)
            {
                SetError("invalid argument for setaref (dst)");
                return false;
            }
            pVDst = pVDst->GetVarPointer();
            if (!pVDst)
            {
                SetError("invalid argument for setaref (dst)");
                return false;
            }
            if (pVDst->GetType() != VAR_AREFERENCE)
            {
                SetError("isnt aref");
                return false;
            }

            // source
            Token_type = BC_TokenGet(ip, token_data_size);
            switch (Token_type)
            {
            case AP:
                pVDst->Set(eid);
                pVDst->SetAReference(rAP);

                break;
            default:
                SetError("invalid argument for setref (src)");
                return false;
            }

            break;
        case SETREF_BXINDEX:
        case SETREF:

            if (Token_type == SETREF_BXINDEX)
            {
                bUseIndex = true;
                rBX.Get(dwBXIndex);
            }
            else
                bUseIndex = false;

            pVDst = nullptr;
            pVSrc = nullptr;

            // destination
            Token_type = BC_TokenGet(ip, token_data_size);
            switch (Token_type)
            {
            case STACK_TOP:
                pVDst = SStack.Read();
                pVDst->ClearType();
                pVDst->SetType(VAR_REFERENCE);
                break;
            case VARIABLE:
                real_var = VarTab.GetVar(*((uint32_t *)&pCodeBase[ip]));
                if (real_var == nullptr)
                {
                    SetError("Global variable not found");
                    return false;
                }

                pVDst = real_var->value.get();
                if (!pVDst)
                    return false;
                if (pVDst->GetType() != VAR_REFERENCE)
                {
                    SetError("'%s' isnt reference", real_var->name.c_str());
                    return false;
                }
                break;
            case LOCAL_VARIABLE:
                pVDst = SStack.Read(pRun_fi->stack_offset, *((uint32_t *)&pCodeBase[ip]));
                if (pVDst == nullptr)
                {
                    SetError("Local variable not found");
                    return false;
                }
                if (!pVDst)
                    return false;
                if (pVDst->GetType() != VAR_REFERENCE)
                {
                    SetError("Local variable is not reference");
                    return false;
                }
                break;
            default:
                SetError("invalid argument for setref (dst)");
                return false;
            }
            if (!pVDst)
            {
                SetError("invalid argument for setref (dst)");
                return false;
            }

            // source
            Token_type = BC_TokenGet(ip, token_data_size);
            switch (Token_type)
            {
            case VARIABLE:
                real_var = VarTab.GetVar(*((uint32_t *)&pCodeBase[ip]));
                if (real_var == nullptr)
                {
                    SetError("Global variable not found");
                    return false;
                }

                pVSrc = real_var->value.get();
                if (bUseIndex)
                    pVSrc = pVSrc->GetArrayElement(dwBXIndex);
                if (!pVSrc)
                    return false;
                break;
            case LOCAL_VARIABLE:
                pVSrc = SStack.Read(pRun_fi->stack_offset, *((uint32_t *)&pCodeBase[ip]));
                if (pVSrc == nullptr)
                {
                    SetError("Local variable not found");
                    return false;
                }
                if (bUseIndex)
                    pVSrc = pVSrc->GetArrayElement(dwBXIndex);
                if (!pVSrc)
                    return false;
                break;
            default:
                SetError("invalid argument for setref (src)");
                return false;
            }

            pVDst->SetReference(pVSrc->GetVarPointer());

            break;
        case MOVE:

            pVDst = nullptr;
            pVSrc = nullptr;
            Token_type = BC_TokenGet(ip, token_data_size);
            switch (Token_type)
            {
            case LEFT_OPERAND:
                if (nLeftOperandCode == INVALID_VAR_CODE)
                {
                    SetError("no Lvalue");
                    return false;
                }

                if (bLeftOperandType == VARIABLE)
                {
                    real_var = VarTab.GetVar(nLeftOperandCode);
                    if (real_var == nullptr)
                    {
                        SetError("Global variable not found");
                        return false;
                    }

                    pVDst = real_var->value.get();
                }
                else
                {
                    if (pRun_fi)
                    {
                        pVDst = SStack.Read(pRun_fi->stack_offset, nLeftOperandCode);
                        if (pVDst == nullptr)
                        {
                            SetError("Local variable not found");
                            return false;
                        }
                    }
                    else
                    {
                        pVDst = nullptr;
                        SetError("Internal error, line 4808! Current function not defined...");
                        return false;
                    }
                }
                break;
            default:
                SetError("invalid DST");
                return false;
            }

            if (pVDst == nullptr)
            {
                SetError("no LValue");
                return false;
            }

            Token_type = BC_TokenGet(ip, token_data_size);
            switch (Token_type)
            {
            case STACK_TOP:
                pVSrc = SStack.Read();
                if (!pVSrc)
                    return false;
                break;
            default:
                SetError("incorrect SRC");
                return false;
            }

            if (pLeftOperandAClass != nullptr)
            {
                const char *pChar;

                ExpressionResult.ClearType();
                ExpressionResult.Copy(pVSrc);

                // 2 signs precision --------------------------------
                if (ExpressionResult.GetType() == VAR_FLOAT)
                {
                    float fV1;
                    ExpressionResult.Get(fV1);
                    char text[storm::script::kFixed7Size];
                    storm::script::FormatFixed7(fV1, text);
                    ExpressionResult.Set(text);
                }
                // 2 signs precision --------------------------------
                ExpressionResult.Convert(VAR_STRING);
                if (!ExpressionResult.Get(pChar))
                    break;
                pLeftOperandAClass->SetValue(pChar);

                if (nLeftOperandIndex != INVALID_ARRAY_INDEX)
                    pVDst = pVDst->GetArrayElement(nLeftOperandIndex);
                if (!pVDst)
                    return false;

                pVDst->Get(eid);
                if (bEntityUpdate)
                {
                    core_internal.Entity_AttributeChanged(eid, pLeftOperandAClass);
                }
                break;
            }

            if (pVSrc->GetType() == VAR_REFERENCE && pVDst->GetType() == VAR_REFERENCE)
            {
                pVDst->SetReference(pVSrc->GetReference());
                break;
            }

            pVDst = pVDst->GetVarPointer();
            if (!pVDst)
            {
                SetError("no DST");
                return false;
            }

            if (nLeftOperandIndex != INVALID_ARRAY_INDEX)
                pVDst = pVDst->GetArrayElement(nLeftOperandIndex);
            if (!pVDst)
                return false;

            DATA *pVTTSrc;
            DATA *pVTTDst;
            pVTTSrc = pVSrc->GetVarPointer();
            pVTTDst = pVDst->GetVarPointer();

            if (pVTTSrc->GetType() != pVTTDst->GetType())
            {
                // ??? think about a little later
                ExpressionResult.ClearType();
                ExpressionResult.Copy(pVSrc);
                if (ExpressionResult.IsReference())
                    ExpressionResult.RefConvert();
                if (!ExpressionResult.Convert(pVTTDst->GetType())) /// ??????????
                {
                    break; // skip unallowed conversion, write to log(in Convert func)
                }
                pVSrc = ExpressionResult.GetVarPointer(); // converted source
            }

            // if not attribute operation proceed to variables copy

            pVDst->Copy(pVSrc);

            break;

        case STACK_ALLOC:
            SStack.Push();
            break;

        case STACK_WRITE_BXINDEX:
        case STACK_PUSH:
        case STACK_WRITE:

            if (Token_type == STACK_WRITE_BXINDEX)
            {
                bUseIndex = true;
                rBX.Get(dwBXIndex);
            }
            else
                bUseIndex = false;

            if (Token_type == STACK_PUSH)
                pV = SStack.Push();
            else
                pV = SStack.Read();

            Token_type = BC_TokenGet(ip, token_data_size);
            switch (Token_type)
            {
            case NUMBER:
                pV->Set(*((int32_t *)&pRunCodeBase[TLR_DataOffset]));
                break;
            case FLOAT_NUMBER:
                pV->Set(*((float *)&pRunCodeBase[TLR_DataOffset]));
                break;
            case STRING:
            {
                // The constant: its size with the terminator (4 bytes), then the text.
                uint32_t size;
                memcpy(&size, &pRunCodeBase[TLR_DataOffset], sizeof(size));
                pV->SetText(std::string_view(&pRunCodeBase[TLR_DataOffset + 4], size - 1));
            }
                break;
            case VARIABLE:
                real_var = VarTab.GetVar(*((uint32_t *)&pCodeBase[ip]));
                if (real_var == nullptr)
                {
                    SetError("Global variable not found");
                    break;
                }

                pVV = real_var->value.get();
                if (pVV->IsReference())
                {
                    if (bUseIndex)
                    {
                        pVV = pVV->GetVarPointer();
                        pVV = pVV->GetArrayElement(dwBXIndex);
                        if (!pVV)
                        {
                            SetError("invalid ref");
                            break;
                        }

                        pV->SetType(VAR_REFERENCE);
                        pV->SetReference(pVV);
                    }
                    else
                    {
                        pV->SetType(VAR_REFERENCE);
                        pV->SetReference(pVV->GetReference());
                    }
                    break;
                }

                if (bUseIndex)
                {
                    pVV = pVV->GetArrayElement(dwBXIndex);
                    if (!pVV)
                    {
                        SetError("invalid array index");
                        break;
                    }
                    pV->Copy(pVV);
                }
                else
                    pV->Copy(pVV);

                break;
            case LOCAL_VARIABLE:
                pVar = SStack.Read(pRun_fi->stack_offset, *((uint32_t *)&pCodeBase[ip]));
                if (pVar == nullptr)
                {
                    SetError("Local variable not found");
                    return false;
                }
                if (pVar->IsReference())
                {
                    if (bUseIndex)
                    {
                        pVar = pVar->GetVarPointer();
                        pVar = pVar->GetArrayElement(dwBXIndex);
                        if (!pVar)
                        {
                            SetError("invalid ref");
                            break;
                        }

                        pV->SetType(VAR_REFERENCE);
                        pV->SetReference(pVar);
                    }
                    else
                    {
                        pV->SetType(VAR_REFERENCE);
                        pV->SetReference(pVar->GetReference());
                    }
                    break;
                }


                if (bUseIndex)
                {
                    pVar = pVar->GetArrayElement(dwBXIndex);
                    if (!pVar)
                    {
                        SetError("invalid array index");
                        break;
                    }
                    pV->Copy(pVar);
                }
                else
                    pV->Copy(pVar);

                break;
            case AX:
                pV->Copy(&rAX);
                break;
            case BX:
                pV->Copy(&rBX);
                break;
            case EX:
                pV->Copy(&ExpressionResult);
                break;
            case AP_VALUE:
                if (!rAP)
                {
                    SetError("no rAP data");
                    pV->Set("error");
                    break;
                }
                pV->SetText(rAP->HasValue() ? std::string_view(rAP->GetValue()) : std::string_view());
                break;
            default:
                SetError("invalid argument for STACK_PUSH");
                return false;
            }
            break;

        case STACK_POP_VOID:
            SStack.Pop();
            break;
        case STACK_POP:
        case STACK_READ:
            if (SStack.GetDataNum() == 0)
                break;
            if (Token_type == STACK_POP)
                pV = SStack.Pop();
            else
                pV = SStack.Read();

            Token_type = BC_TokenGet(ip, token_data_size);
            switch (Token_type)
            {
            case VARIABLE:
                real_var = VarTab.GetVar(*((uint32_t *)&pCodeBase[ip]));
                if (real_var == nullptr)
                {
                    SetError("Global variable not found");
                    break;
                }

                real_var->value->Copy(pV);
                break;
            case LOCAL_VARIABLE:
                pVar = SStack.Read(pRun_fi->stack_offset, *((uint32_t *)&pCodeBase[ip]));
                if (pVar == nullptr)
                {
                    SetError("Local variable not found");
                    return false;
                }
                pVar->Copy(pV);
                break;
            case AX:
                rAX.ClearType();
                rAX.Copy(pV);
                break;
            case BX:
                rBX.ClearType();
                rBX.Copy(pV);
                break;
            case EX:
                ExpressionResult.ClearType();
                ExpressionResult.Copy(pV);
                break;
            case LEFTOP_INDEX:
                pV->Convert(VAR_INTEGER);
                pV->Get(nLeftOperandIndex);
                break;
            default:
                SetError("invalid argument for STACK_POP");
                return false;
            }
            break;

        case OP_BOOL_CONVERT:
            pV = GetOperand(pCodeBase, ip);
            if (!pV)
                return false;
            pV->BoolConvert();
            break;
        case OP_BOOL_AND:
        case OP_BOOL_OR:
            pV = GetOperand(pCodeBase, ip);
            if (!pV)
                return false;
            pVV = GetOperand(pCodeBase, ip);
            if (!pVV)
                return false;

            pV->BoolConvert();
            pV->RefConvert();
            pV->CompareAndSetResult(pVV, Token_type);
            break;
        case OP_NOT_EQUAL:
        case OP_LESSER_OR_EQUAL:
        case OP_LESSER:
        case OP_GREATER_OR_EQUAL:
        case OP_GREATER:
        case OP_BOOL_EQUAL:
            pV = GetOperand(pCodeBase, ip);
            if (!pV)
                return false;
            pVV = GetOperand(pCodeBase, ip);
            if (!pVV)
                return false;
            pV->RefConvert();
            pV->CompareAndSetResult(pVV, Token_type);
            break;
        case OP_PLUS:
            pV = GetOperand(pCodeBase, ip);
            if (!pV)
                return false;
            pVV = GetOperand(pCodeBase, ip);
            if (!pVV)
                return false;
            pV->Plus(pVV);
            break;
        case OP_MINUS:
            pV = GetOperand(pCodeBase, ip);
            if (!pV)
                return false;
            pVV = GetOperand(pCodeBase, ip);
            if (!pVV)
                return false;
            pV->Minus(pVV);
            break;
        case OP_MULTIPLY:
            pV = GetOperand(pCodeBase, ip);
            if (!pV)
                return false;
            pVV = GetOperand(pCodeBase, ip);
            if (!pVV)
                return false;
            pV->Multiply(pVV);
            break;
        case OP_DIVIDE:
            pV = GetOperand(pCodeBase, ip);
            if (!pV)
                return false;
            pVV = GetOperand(pCodeBase, ip);
            if (!pVV)
                return false;
            pV->Divide(pVV);
            break;
        case OP_MODUL:
            pV = GetOperand(pCodeBase, ip);
            if (!pV)
                return false;
            pVV = GetOperand(pCodeBase, ip);
            if (!pVV)
                return false;
            pV->Modul(pVV);
            break;
        case OP_POWER:
            pV = GetOperand(pCodeBase, ip);
            if (!pV)
                return false;
            pVV = GetOperand(pCodeBase, ip);
            if (!pVV)
                return false;
            pV->Power(pVV);
            break;
        case OP_SMINUS:
            pV = GetOperand(pCodeBase, ip);
            if (!pV)
                return false;
            pV->Inverse();
            break;
        case OP_BOOL_NEG:
            pV = GetOperand(pCodeBase, ip);
            if (!pV)
                return false;
            pV->Neg();
            break;

        case MOVEAP_BXINDEX:
        case MOVEAP:

            if (Token_type == MOVEAP_BXINDEX)
            {
                bUseIndex = true;
                rBX.Get(dwBXIndex);
            }
            else
                bUseIndex = false;

            pV = GetOperand(pCodeBase, ip, &Token_type);
            switch (Token_type)
            {
            case VARIABLE:
            case LOCAL_VARIABLE:
                SetError("not implemented");
                return false;
                break;
            case AP:
                pVV = GetOperand(pCodeBase, ip, &Token_type);
                if (Token_type == AP)
                    break;
                if (!pVV)
                    return false;
                if (bUseIndex)
                    pVV = pVV->GetArrayElement(dwBXIndex);
                if (!pVV)
                {
                    SetError("invalid array index");
                    return false;
                }
                rAP = pVV->GetAClass();
                break;
            }
            break;
        case VERIFY_AP:
        case ADVANCE_AP:

            S_TOKEN_TYPE sttV = Token_type;

            const char *pChar;
            if (!rAP)
            {
                SetError("null ap");
                BC_TokenGet();
                break;
            }

            Token_type = BC_TokenGet();
            switch (Token_type)
            {
            case ACCESS_WORD_CODE:
                if (sttV == VERIFY_AP)
                    rAP = rAP->VerifyAttributeClassByCode(*((int32_t *)&pRunCodeBase[TLR_DataOffset]));
                else
                    rAP = rAP->GetAttributeClassByCode(*((int32_t *)&pRunCodeBase[TLR_DataOffset]));
                if (!rAP)
                    SetError("missed attribute: %s", SCodec.Convert(*((int32_t *)&pRunCodeBase[TLR_DataOffset])));
                break;
            case VARIABLE:
                real_var = VarTab.GetVar(*((int32_t *)&pRunCodeBase[TLR_DataOffset]));
                if (real_var == nullptr)
                {
                    SetError("Global variable not found");
                    break;
                }

                ExpressionResult.ClearType();
                ExpressionResult.Copy(real_var->value.get());
                if (!ExpressionResult.Convert(VAR_STRING))
                {
                    SetError("invalid type for attribute var");
                    break;
                }
                ExpressionResult.Get(pChar);
                if (sttV == VERIFY_AP)
                    rAP = rAP->CreateSubAClass(rAP, pChar);
                else
                    rAP = rAP->FindAClass(rAP, pChar);

                if (!rAP)
                    SetError("missed attribute: %s", pChar);
                break;
            case LOCAL_VARIABLE:
                pV = SStack.Read(pRun_fi->stack_offset, *((int32_t *)&pRunCodeBase[TLR_DataOffset]));
                if (pV == nullptr)
                {
                    SetError("Local variable not found");
                    break;
                }

                ExpressionResult.ClearType();
                ExpressionResult.Copy(pV);
                if (!ExpressionResult.Convert(VAR_STRING))
                {
                    SetError("invalid type for attribute var");
                    break;
                }
                ExpressionResult.Get(pChar);

                if (sttV == VERIFY_AP)
                    rAP = rAP->CreateSubAClass(rAP, pChar);
                else
                    rAP = rAP->FindAClass(rAP, pChar);


                if (!rAP)
                    SetError("missed attribute: %s", pChar);

                break;
            default:
                SetError("invalid arg type for ADVANCE_AP");
                return false;
            }


            break;

        }
    } while (Token_type != END_OF_PROGRAMM);

    return false;
}

void COMPILER::CompileNumber(SEGMENT_DESC &Segment)
{
    int32_t lvalue;
    lvalue = static_cast<int32_t>(atoll(Token.GetData()));
    CompileToken(Segment, NUMBER, 1, (char *)&lvalue, sizeof(int32_t));
}

void COMPILER::CompileFloatNumber(SEGMENT_DESC &Segment)
{
    float fvalue;
    fvalue = static_cast<float>(atof(Token.GetData()));
    CompileToken(Segment, FLOAT_NUMBER, 1, (char *)&fvalue, sizeof(float));
}

void COMPILER::CompileString(SEGMENT_DESC &Segment)
{
    uint32_t string_size;
    const char *pData = Token.GetData();
    if (pData != nullptr)
    {
        string_size = strlen(Token.GetData()) + 1;
        CompileToken(Segment, STRING, 2, (char *)&string_size, sizeof(uint32_t), Token.GetData(), string_size);
    }
    else
    {
        string_size = strlen("") + 1;
        CompileToken(Segment, STRING, 2, (char *)&string_size, sizeof(uint32_t), "", string_size);
    }
}

bool COMPILER::CompileUnknown(SEGMENT_DESC &Segment)
{
    uint32_t func_code;
    uint32_t var_code;

    func_code = FuncTab.FindFunc(Token.GetData());

    if (func_code != INVALID_FUNC_CODE)
    {
        // this is function call
        CompileToken(Segment, CALL_FUNCTION, 1, (char *)&func_code, sizeof(uint32_t));
        return true;
    }

    var_code = FuncTab.FindVar(CurrentFuncCode, Token.GetData());
    if (var_code != INVALID_VAR_CODE)
    {
        CompileToken(Segment, LOCAL_VARIABLE, 1, (char *)&var_code, sizeof(uint32_t));
        return true;
    }

    var_code = VarTab.FindVar(Token.GetData());
    if (var_code != INVALID_VAR_CODE)
    {
        // this is gloabl variable
        CompileToken(Segment, VARIABLE, 1, (char *)&var_code, sizeof(uint32_t));
        return true;
    }

    const uint32_t def_code = DefTab.FindDef(Token.GetData());
    if (def_code != INVALID_DEF_CODE)
    {
        DEFINFO di;
        DefTab.GetDef(di, def_code);
        switch (di.deftype)
        {
        case NUMBER:
            CompileToken(Segment, NUMBER, 1, (char *)&di.data4b, sizeof(uint32_t));
            break;
        case FLOAT_NUMBER:
            CompileToken(Segment, FLOAT_NUMBER, 1, (char *)&di.data4b, sizeof(uint32_t));
            break;
        case STRING:
            uint32_t string_size;
            string_size = strlen((char *)di.data4b) + 1;
            CompileToken(Segment, STRING, 2, (char *)&string_size, sizeof(uint32_t), (char *)di.data4b, string_size);
            break;
        }
        return true;
    }

    const uint32_t lab_code = LabelTable.GetStringCode(Token.GetData());
    if (lab_code != INVALID_ORDINAL_NUMBER)
    {
        LabelTable.SetStringData(lab_code, &Segment.BCode_Program_size);
        return true;
    }

    SetError("Undeclared identifier: %s", Token.GetData());
    return false;
}

S_TOKEN_TYPE COMPILER::DetectUnknown(uint32_t &code)
{
    const uint32_t func_code = FuncTab.FindFunc(Token.GetData());

    if (func_code != INVALID_FUNC_CODE)
    {
        // this is function call
        code = func_code;
        return CALL_FUNCTION;
    }

    uint32_t var_code = FuncTab.FindVar(CurrentFuncCode, Token.GetData());
    if (var_code != INVALID_VAR_CODE)
    {
        code = var_code;
        return LOCAL_VARIABLE;
    }

    var_code = VarTab.FindVar(Token.GetData());
    if (var_code != INVALID_VAR_CODE)
    {
        code = var_code;
        return VARIABLE;
    }

    const uint32_t def_code = DefTab.FindDef(Token.GetData());
    if (def_code != INVALID_DEF_CODE)
    {
        code = def_code;
        return DEFINE_VAL;
    }

    return UNKNOWN;
}

void COMPILER::ExitProgram()
{
    const uint32_t function_code = FuncTab.FindFunc("ExitMain");
    if (function_code != INVALID_FUNC_CODE)
    {
        DATA *pResult;
        BC_Execute(function_code, pResult);
    }
    core_internal.Exit();
}

void COMPILER::ClearEvents()
{
    EventTab.Clear();
}

void COMPILER::SaveData(const void *data_PTR, uint32_t data_size)
{
    if (data_PTR == nullptr)
        return;

    if (dwCurPointer + data_size > dwMaxSize)
    {
        const uint32_t dwNewAllocate = (1 + (dwCurPointer + data_size) / (1024 * 1024)) * (1024 * 1024);
        auto *const newPtr = new char[dwNewAllocate];
        if (dwMaxSize != 0) // the first block: there is no buffer to copy from
            memcpy(newPtr, pBuffer, dwMaxSize);
        delete[] pBuffer;
        pBuffer = newPtr;

        dwMaxSize = dwNewAllocate;
    }

    memcpy(&pBuffer[dwCurPointer], data_PTR, data_size);

    dwCurPointer += data_size;
}

void COMPILER::WriteVDword(uint32_t v)
{
    uint8_t nbv;
    uint16_t nwv;
    if (v < 0xfe)
    {
        nbv = static_cast<uint8_t>(v);
        SaveData(&nbv, sizeof(nbv));
    }
    else if (v < 0xffff)
    {
        nbv = 0xfe;
        SaveData(&nbv, sizeof(nbv));
        nwv = static_cast<uint16_t>(v);
        SaveData(&nwv, sizeof(nwv));
    }
    else
    {
        nbv = 0xff;
        SaveData(&nbv, sizeof(nbv));
        SaveData(&v, sizeof(v));
    }
}

void COMPILER::SaveString(const char *pS)
{
    if (pS == nullptr)
    {
        WriteVDword(0);
        return;
    }
    const uint32_t n = strlen(pS) + 1;
    WriteVDword(n);
    SaveData(pS, n);
}

void COMPILER::SaveVariable(DATA *pV, bool bdim)
{
    int32_t nLongValue;
    uintptr_t ptrValue;
    float fFloatValue;
    const char *pString;
    uint32_t n;
    S_TOKEN_TYPE eType;
    entid_t eid;

    if (pV == nullptr)
    {
        SetError("Zero Variable Pointer");
        return;
    }
    if (!bdim) // skip this info for array elements
    {
        eType = pV->GetType();
        SaveData(&eType, sizeof(eType));

        n = pV->Number_of_elements;
        if (n == 0)
            n = 1; // for reference variables
        SaveData(&n, sizeof(n));
    }

    if (pV->IsArray())
    {
        // Elements carry no type of their own in the save: the load reads each with the array's
        // type. An element whose type changed at run time (`a[i] += 0.5` in an int array,
        // DATA::Plus) is written converted to it, or every value after it would be misread.
        const S_TOKEN_TYPE arrayType = pV->GetType();
        for (n = 0; n < pV->GetArraySize(); n++)
        {
            DATA *element = pV->GetArrayElement(n);
            const S_TOKEN_TYPE elementType = element->GetType();
            if (elementType == arrayType)
            {
                SaveVariable(element, true);
                continue;
            }
            DATA converted(arrayType);
            converted.SetVCompiler(this);
            const bool scalar = [&] {
                const auto isScalar = [](S_TOKEN_TYPE type) {
                    return type == VAR_INTEGER || type == VAR_FLOAT || type == VAR_STRING;
                };
                return isScalar(arrayType) && isScalar(elementType);
            }();
            if (scalar)
            {
                int32_t integer;
                float real;
                const char *text;
                if (elementType == VAR_INTEGER && element->Get(integer))
                    converted.Set(integer);
                else if (elementType == VAR_FLOAT && element->Get(real))
                    converted.Set(real);
                else if (elementType == VAR_STRING && element->Get(text))
                    converted.Set(text);
                converted.Convert(arrayType);
            }
            SetError("Save: element %u of '%s' is %s in an array of %s; saved %s", n, save_variable_name_,
                     storm::save::typeName(elementType), storm::save::typeName(arrayType),
                     scalar ? "converted" : "as an empty value");
            SaveVariable(&converted, true);
        }
        return;
    }

    switch (pV->GetType())
    {
    case VAR_INTEGER:
        pV->Get(nLongValue);
        SaveData(&nLongValue, sizeof(nLongValue));
        break;
    case VAR_PTR:
        pV->GetPtr(ptrValue);
        SaveData(&ptrValue, sizeof(ptrValue));
        break;
    case VAR_FLOAT:
        pV->Get(fFloatValue);
        SaveData(&fFloatValue, sizeof(fFloatValue));
        break;
    case VAR_STRING:
        pV->Get(pString);
        SaveString(pString);
        break;
    case VAR_OBJECT:
        pV->Get(eid);
        SaveData(&eid, sizeof(eid));
        SaveAttributesData(pV->AttributesClass);
        break;
    case VAR_REFERENCE: {
        if (pV->GetReference() == nullptr)
        {
            // uninitialized reference
            WriteVDword(0xffffffff);
            return;
        }
        // The chain is resolved by address, never followed through memory that may be freed
        // (save_references.hpp).
        const auto place = save_references_->Variable(pV);
        if (!place)
        {
            SetError("Ghost reference: '%s' names no global variable and is saved empty", save_variable_name_);
            // reference to none
            WriteVDword(0xffffffff);
            return;
        }

        WriteVDword(place->variable);
        WriteVDword(place->element);
        break;
    }
    case VAR_AREFERENCE: {
        if (pV->AReferenceDangling())
        {
            // The game leaves such arefs behind (reload.c reload_locator_ref names the quest reload's fake locator,
            // deleted once the location loaded); no script may read them. Cleared: the next save is silent, and the
            // variable holds what loading this save gives it.
            SetWarning("aref '%s' names a deleted attribute: saved empty and cleared", save_variable_name_);
            pV->AttributesClass = nullptr;
        }
        if (pV->AttributesClass == nullptr)
        {
            WriteVDword(0xffffffff);
            break;
        }
        const storm::save::AttributePlace *place = save_references_->Attribute(pV->AttributesClass);
        if (place == nullptr)
        {
            // An attribute of a local object or of an entity.
            SetError("Ghost A reference: '%s' names no attribute of a global object and is saved empty",
                     save_variable_name_);
            WriteVDword(0xffffffff);
            break;
        }
        if (place->nameless)
        {
            WriteVDword(0xffffffff);
            break;
        }
        WriteVDword(place->owner.variable);
        WriteVDword(place->owner.element);
        SaveString(place->path.empty() ? nullptr : place->path.c_str());
        break;
    }
    }
}

bool COMPILER::OnLoad()
{
    const uint32_t function_code = FuncTab.FindFunc("OnLoad");
    DATA *pResult;
    const bool executed = BC_Execute(function_code, pResult);
#ifdef __ANDROID__
    if (executed) LoadAndroidUiAdapter();
    else storm::android::traceLandAdapter(0, "onload-execution-failed");
#else
    (void)executed;
#endif
#ifdef STORM_SCENE_SWEEP
    LoadSceneSweep(); // a loaded save replaced the script state, the sweep's segment with it
#endif
    return true;
}

// The script state as a save holds it, appended to pBuffer: the program directory, the name codec, the segments
// and every global variable with its attributes.
void COMPILER::SerializeState()
{
    uint32_t n;
    // 1. Program Directory
    SaveString(ProgramDirectory);

    // 4. SCodec data
    WriteVDword(SCodec.GetNum());
    for (n = 0; n < SCodec.GetNum(); n++)
    {
        if (n == 0)
            SaveString(SCodec.Get());
        else
            SaveString(SCodec.GetNext());
    }

    const uint32_t nSegNum = 1; // SegmentsNum;
    // 2. Data Segments
    WriteVDword(nSegNum);

    // 3. Segments names
    for (n = 0; n < nSegNum; n++)
    {
        if (!SegmentTable[n].name.empty())
        {
            SaveString(SegmentTable[n].name.c_str());
        }
    }

    // 5. Variables table
    const uint32_t nVarNum = VarTab.GetVarNum();
    WriteVDword(nVarNum);

    // Built after OnSave has run: the script may still delete attributes there.
    const storm::save::ReferenceIndex references(VarTab);
    save_references_ = &references;
    const VarInfo *last_var{nullptr};
    for (n = 0; n < nVarNum; n++)
    {
        const VarInfo *real_var = VarTab.GetVar(n);
        if (real_var == nullptr)
        {
            real_var = last_var; // preserve old semanthics
        }
        else
        {
            last_var = real_var;
        }

        SaveString(real_var->name.c_str()); // ***
        save_variable_name_ = real_var->name.c_str();
        SaveVariable(real_var->value.get());
    }
    save_references_ = nullptr;
    save_variable_name_ = "";
}

bool COMPILER::SaveState(std::vector<char> &image)
{
    delete[] pBuffer;
    pBuffer = nullptr;

    dwCurPointer = 0;
    dwMaxSize = 0;

    DATA *pResult;
    const uint32_t function_code = FuncTab.FindFunc("OnSave");
    if (function_code != INVALID_FUNC_CODE)
        BC_Execute(function_code, pResult);

    // Serialize into memory only: the caller replaces the file atomically, so a
    // failure or crash at any point leaves the previous save intact.
    std::string save_info = "save";
    auto *pVDat = static_cast<VDATA *>(core_internal.GetScriptVariable("savefile_info"));
    if (pVDat && pVDat->GetString())
        save_info = pVDat->GetString();

    SerializeState();

    std::string error;
    const bool built = storm::save::buildState(save_info.c_str(), pBuffer, dwCurPointer, image, error);
    if (!built)
    {
        spdlog::error("Cannot build save image: {}", error);
    }

    delete[] pBuffer;
    pBuffer = nullptr;
    dwCurPointer = 0;
    dwMaxSize = 0;

    return built;
}

// `raw` is the uncompressed script state (save_file.hpp) and `state` its structure, both
// checked before the caller erased the running game (save_state.hpp): from here on nothing of
// the file can fail. What remains is the program itself (a segment that does not compile).
bool COMPILER::LoadState(const std::vector<char> &raw, const storm::save::ParsedState &state)
{
    Release();

    // 1. Program Directory
    if (state.programDirectory)
    {
        ProgramDirectory = new char[state.programDirectory->size() + 1];
        memcpy(ProgramDirectory, state.programDirectory->c_str(), state.programDirectory->size() + 1);
    }

    // 4. SCodec data: in saved order, which gives every name its saved code
    for (const auto &name : state.codec)
        SCodec.Convert(name.c_str());

    // 3.  Segments names
    // 3.a Initialize internal functions
    // 3.b Load preprocess
    InitInternalFunctions();
    LoadPreprocess();

    for (const auto &segment : state.segments)
    {
        if (!BC_LoadSegment(segment.c_str()))
            return false;
    }

    // 5. Variables table, all variables created during previous step, just read value
    storm::save::applyVariables(raw.data(), raw.size(), state.variables, VarTab, &SCodec,
                                [this](const std::string &line) { SetError("%s", line.c_str()); });
#ifdef STORM_SCENE_SWEEP
    WriteStateDump("load"); // the state as loaded, before the game's OnLoad runs
#endif

    // call to script function "OnLoad()"
    OnLoad();

    return true;
}

void COMPILER::SaveAttributesData(ATTRIBUTES *pRoot)
{
    if (pRoot == nullptr)
    {
        WriteVDword(0);      // number of subclasses
        SaveString(nullptr); // attribute name
        SaveString(nullptr); // attribute value
        return;
    }
    WriteVDword(pRoot->GetAttributesNum()); // number of subclasses

    WriteVDword(pRoot->GetThisNameCode());

    SaveString(pRoot->GetThisAttr());
    for (uint32_t n = 0; n < pRoot->GetAttributesNum(); n++)
    {
        SaveAttributesData(pRoot->GetAttributeClass(n));
    }
}

bool COMPILER::EventPosted(const char *name)
{
    for (uint32_t n = 0; n < EventMsg.GetClassesNum(); n++)
        if (const S_EVENTMSG *event = EventMsg.Read(n); event && !event->bInvalide && event->pEventName && storm::iEquals(event->pEventName, name))
            return true;
    return false;
}

void COMPILER::AddPostEvent(S_EVENTMSG *pEM)
{
    if (pEM->pMessageClass)
    {
        const std::string_view format = pEM->pMessageClass->GetFormat();
        for (size_t n = 0; n < format.size(); n++)
            if (ATTRIBUTES *attribute = format[n] == 'a' ? pEM->pMessageClass->AttributeAt(n) : nullptr)
                pEM->attributes.emplace_back(n, attribute->GetHandle());
    }
    EventMsg.Add(pEM);
}

// An attribute posted with the event and deleted before its time: the handler gets an empty aref.
void COMPILER::ForgetDeletedAttributes(S_EVENTMSG *pEM)
{
    for (const auto &[position, handle] : pEM->attributes)
    {
        if (ATTRIBUTES::IsAlive(handle))
            continue;
        SetError("PostEvent(\"%s\"): its argument %zu names a deleted attribute (deleted before the event's time): "
                 "the handler gets an empty aref", pEM->pEventName, position + 1);
        pEM->pMessageClass->SetAttributeAt(position, nullptr);
    }
}

bool COMPILER::SetSaveData(const char *file_name, void *save_data, int32_t data_size)
{
    if (file_name == nullptr || save_data == nullptr || data_size <= 0)
    {
        return false;
    }

    // Read-modify-write through a temporary file: the save written by
    // SaveState stays loadable if this step fails or the process dies.
    const auto path = std::filesystem::u8path(fio->ConvertPathResource(file_name));
    std::string error;
    storm::save::Bytes image;
    if (!storm::save::readFile(path, image, error))
    {
        spdlog::error("Cannot attach save data to '{}': {}", path.string(), error);
        return false;
    }

    std::string save_info = "save";
    auto *pVDat = static_cast<VDATA *>(core_internal.GetScriptVariable("savefile_info"));
    if (pVDat && pVDat->GetString())
        save_info = pVDat->GetString();

    const auto validate = [](const storm::save::Bytes &stored, std::string &why) {
        storm::save::Bytes scratch;
        return storm::save::readState(stored, scratch, why) && storm::save::readExtData(stored, scratch, why);
    };
    if (!storm::save::appendExtData(image, save_info.c_str(), save_data, static_cast<uint32_t>(data_size), error) ||
        !storm::save::writeAtomic(path, image, validate, error))
    {
        spdlog::error("Cannot attach save data to '{}': {}", path.string(), error);
        return false;
    }

    return true;
}

void *COMPILER::GetSaveData(const char *file_name, int32_t &data_size)
{
    data_size = 0;
    if (file_name == nullptr)
    {
        return nullptr;
    }

    const auto path = std::filesystem::u8path(fio->ConvertPathResource(file_name));
    std::string error;
    storm::save::Bytes data;
    if (!std::filesystem::exists(path))
    {
        SetError("cant open save file: %s", file_name);
        return nullptr;
    }
    // Corrupt or truncated extended data is reported as "no data" instead of
    // handing the caller a buffer of uninitialized bytes.
    if (!storm::save::readExtDataFromFile(path, data, error))
    {
        spdlog::warn("Ignoring damaged save description in '{}': {}", path.string(), error);
        return nullptr;
    }
    if (data.empty())
    {
        return nullptr;
    }

    char *pResult = new char[data.size()];
    memcpy(pResult, data.data(), data.size());
    data_size = static_cast<int32_t>(data.size());
    return pResult;
}

uint32_t COMPILER::SetScriptFunction(IFUNCINFO *pFuncInfo)
{
    FuncInfo fi;

    if (pFuncInfo->pFuncName == nullptr)
    {
        SetError("Invalid function name in SetScriptFunction(...)");
        return INVALID_FUNCHANDLE;
    }

    const uint32_t nFuncHandle = FuncTab.FindFunc(pFuncInfo->pFuncName);
    if (nFuncHandle != INVALID_FUNC_CODE)
    {
        FuncTab.GetFuncX(fi, nFuncHandle);
        if (fi.arguments != pFuncInfo->nArguments)
        {
            SetError("arguments num mismatch in function '%s'", pFuncInfo->pFuncName);
            return INVALID_FUNCHANDLE;
        }
    }
    else
    {
        SetError("native function '%s' not declared", pFuncInfo->pFuncName);
        return INVALID_FUNCHANDLE;
    }

    fi.name = pFuncInfo->pFuncName;
    fi.imported_func = pFuncInfo->pFuncAddress;
    fi.arguments = pFuncInfo->nArguments;
    fi.segment_id = IMPORTED_SEGMENT_ID;
    fi.offset = INVALID_FUNC_OFFSET;
    fi.stack_offset = 0xffffffff;

    if (pFuncInfo->pReturnValueName == nullptr)
        fi.return_type = TVOID;
    else
    {
        const S_TOKEN_TYPE TokenType = Token.Keyword2TokenType(pFuncInfo->pReturnValueName);
        switch (TokenType)
        {
        case TVOID:
        case VAR_INTEGER:
        case VAR_PTR:
        case VAR_FLOAT:
        case VAR_STRING:
        case VAR_OBJECT:
        case VAR_REFERENCE:
        case VAR_AREFERENCE:
            fi.return_type = TokenType;
            break;
        default:
            SetError("Invalid import function '%s' return type '%s'", pFuncInfo->pFuncName,
                     pFuncInfo->pReturnValueName);
            return INVALID_FUNCHANDLE;
        }
    }

    const uint32_t funch = FuncTab.AddFunc(fi);
    return funch;
}

DATA *COMPILER::GetOperand(const char *pCodeBase, uint32_t &ip, S_TOKEN_TYPE *pTokenType)
{
    uint32_t token_data_size;
    const VarInfo *real_var;
    DATA *pVar;

    const S_TOKEN_TYPE sttResult = BC_TokenGet(ip, token_data_size);
    if (pTokenType)
        *pTokenType = sttResult;
    switch (sttResult)
    {
    case STACK_TOP:
        pVar = SStack.Read();
        return pVar;
    case VARIABLE:
        real_var = VarTab.GetVar(*reinterpret_cast<const uint32_t *>(&pCodeBase[ip]));
        if (real_var == nullptr)
        {
            SetError("Global variable not found");
            break;
        }
        if (!real_var->value.get())
        {
            SetError("invalid global variable");
            break;
        }
        return real_var->value.get();
    case LOCAL_VARIABLE:
        pVar = SStack.Read(pRun_fi->stack_offset, *reinterpret_cast<const uint32_t *>(&pCodeBase[ip]));
        if (pVar == nullptr)
        {
            SetError("Local variable not found");
            return nullptr;
        }
        return pVar;
    case AX:
        return &rAX;
    case BX:
        return &rBX;
    case EX:
        return &ExpressionResult;
    case AP:
        return nullptr;
    default:
        SetError("invalid operand");
        return nullptr;
    }
    return nullptr;
}

int COMPILER::CallDepth(const char *func_name) const
{
    auto calls = callStack_;
    for (int depth = 0; !calls.empty(); ++depth, calls.pop())
        if (storm::iEquals(std::get<2>(calls.back()), func_name))
            return depth;
    return -1;
}

DATA *COMPILER::CallArgument(const char *func_name, uint32_t index)
{
    auto calls = callStack_;
    for (; !calls.empty(); calls.pop())
    {
        const auto &[file, line, name, offset, count] = calls.back();
        if (!storm::iEquals(name, func_name))
            continue;
        if (offset == kNoArguments || index >= count || offset + index >= SStack.GetDataNum())
            return nullptr;
        return SStack.Read(offset, index);
    }
    return nullptr;
}

void COMPILER::CollectCallStack() const
{
    logStack_->trace("Call stack:");
    auto callStackCopy = callStack_;
    while (!callStackCopy.empty())
    {
        const auto &record = callStackCopy.back();

        const auto &filename = std::get<0>(record);
        const auto &line = std::get<1>(record);
        const auto &name = std::get<2>(record);

        if (filename[0] == '\0')
        {
            logStack_->trace("{} (EVENT)", name);
        }
        else if (strcmp(filename, "engine") == 0)
        {
            logStack_->trace("{} (INTERNAL FUNCTION)", name);
        }
        else
        {
            logStack_->trace("{} (FUNCTION) at {}:{}", name, filename, line);
        }

        callStackCopy.pop();
    }
}

void COMPILER::LoadVariablesFromCache(storm::script_cache::BufferReader &reader, SEGMENT_DESC &segment)
{
    const auto size = reader.Read<size_t>();
    for (size_t i = 0; i < size; ++i)
    {
        auto vi = VarInfo();

        vi.segment_id = segment.id;
        vi.name = reader.ReadArray();
        vi.type = reader.Read<decltype(vi.type)>();
        vi.elements = reader.Read<decltype(vi.elements)>();

        const auto var_code = VarTab.AddVar(vi);
        if (var_code == INVALID_VAR_CODE)
        {
            // Name taken by a segment loaded since the entry was written; the
            // compiler reports it as a duplicate with file and line.
            throw storm::script_cache::ReaderException();
        }
        const auto real_var = VarTab.GetVarX(var_code);

        if (vi.elements == 1)
        {
            ReadScriptData(reader, vi.type, real_var->value.get());
        }
        else
        {
            for (size_t j = 0; j < vi.elements; ++j)
            {
                ReadScriptData(reader, vi.type, real_var->value->GetArrayElement(j));
            }
        }
    }
}

void COMPILER::LoadFunctionsFromCache(storm::script_cache::BufferReader &reader, SEGMENT_DESC &segment)
{
    const auto size = reader.Read<size_t>();
    for (size_t i = 0; i < size; ++i)
    {
        auto fi = FuncInfo();
        fi.segment_id = segment.id;

        fi.name = reader.ReadArray();
        fi.offset = reader.Read<decltype(fi.offset)>();
        fi.return_type = reader.Read<decltype(fi.return_type)>();
        fi.decl_file_name = reader.ReadArray();
        fi.decl_line = reader.Read<decltype(fi.decl_line)>();

        auto func_code = FuncTab.AddFunc(fi);

        // args
        auto count = reader.Read<size_t>();
        for (size_t j = 0; j < count; ++j)
        {
            auto lvi = LocalVarInfo();

            lvi.name = reader.ReadArray();
            lvi.type = reader.Read<decltype(lvi.type)>();
            lvi.elements = reader.Read<decltype(lvi.elements)>();

            auto is_extern = reader.Read<bool>();

            FuncTab.AddFuncArg(func_code, lvi, is_extern);
        }

        // local variables
        count = reader.Read<size_t>();
        for (size_t j = 0; j < count; ++j)
        {
            auto lvi = LocalVarInfo();

            lvi.name = reader.ReadArray();
            lvi.type = reader.Read<decltype(lvi.type)>();
            lvi.elements = reader.Read<decltype(lvi.elements)>();

            FuncTab.AddFuncVar(func_code, lvi);
        }
    }
}

void COMPILER::LoadScriptLibrariesFromCache(storm::script_cache::BufferReader &reader)
{
    const auto size = reader.Read<size_t>();
    for (size_t i = 0; i < size; ++i)
    {
        auto name = std::string(reader.ReadArray());

        auto cls = core_internal.FindVMA(name.c_str());
        if (!cls)
        {
            SetWarning("cant load library '%s'", name.c_str());
            continue;
        }

        auto lib = static_cast<SCRIPT_LIBRIARY *>(cls->CreateClass());
        if (lib)
        {
            lib->Init();
        }

        LibriaryFuncs.emplace_back(lib, name.c_str());
    }
}

void COMPILER::LoadEventHandlersFromCache(storm::script_cache::BufferReader &reader)
{
    const auto size = reader.Read<size_t>();
    for (size_t i = 0; i < size; ++i)
    {
        auto event = std::string(reader.ReadArray());
        auto name = std::string(reader.ReadArray());

        SetEventHandler(event.c_str(), name.c_str(), 0, true);
    }
}

void COMPILER::LoadByteCodeFromCache(storm::script_cache::BufferReader &reader, SEGMENT_DESC &segment)
{
    const auto code = reader.ReadArray();
    segment.BCode_Program_size = segment.BCode_Buffer_size = code.size();
    segment.pCode = new char[segment.BCode_Buffer_size];
    std::ranges::copy(code, segment.pCode);

    // relocations data
    auto strings = std::unordered_map<uint32_t, std::string>();
    auto variables = std::unordered_map<uint32_t, std::string>();
    auto functions = std::unordered_map<uint32_t, std::string>();

    auto size = reader.Read<size_t>();
    strings.reserve(size);
    for (size_t i = 0; i < size; ++i)
    {
        auto string_code = reader.Read<uint32_t>();
        auto string = reader.ReadArray();
        strings.emplace(string_code, string);
    }

    size = reader.Read<size_t>();
    variables.reserve(size);
    for (size_t i = 0; i < size; ++i)
    {
        auto variable_code = reader.Read<uint32_t>();
        auto name = reader.ReadArray();
        variables.emplace(variable_code, name);
    }

    size = reader.Read<size_t>();
    functions.reserve(size);
    for (size_t i = 0; i < size; ++i)
    {
        auto function_code = reader.Read<uint32_t>();
        auto name = reader.ReadArray();
        functions.emplace(function_code, name);
    }

    pRunCodeBase = segment.pCode;
    InstructionPointer = 0;
    S_TOKEN_TYPE token_type = END_OF_PROGRAMM;
    do
    {
        token_type = BC_TokenGet();
        auto &relocated_data =
            *const_cast<uint32_t *>(reinterpret_cast<const uint32_t *>(&pRunCodeBase[TLR_DataOffset]));
        if (token_type == ACCESS_WORD_CODE)
        {
            auto string_code = relocated_data;
            relocated_data = SCodec.Convert(strings[string_code].c_str());
        }
        else if (token_type == VARIABLE)
        {
            const auto real_var_code = VarTab.FindVar(variables[relocated_data]);
            relocated_data = real_var_code;
        }
        else if (token_type == CALL_FUNCTION)
        {
            const auto real_function_code = FuncTab.FindFunc(functions[relocated_data]);
            relocated_data = real_function_code;
        }
    } while (token_type != END_OF_PROGRAMM);
}

std::filesystem::path COMPILER::GetSegmentCachePath(const SEGMENT_DESC &segment) const
{
    auto path = GetCacheFolder() / segment.name;
    path.replace_extension(".b");
    return path;
}

void COMPILER::SaveVariablesToCache(storm::script_cache::BufferWriter &writer)
{
    writer.WriteData(script_cache_.variables.size());
    for (auto &vi : script_cache_.variables)
    {
        writer.WriteArray(vi.name);
        writer.WriteData(vi.type);
        writer.WriteData(vi.elements);

        if (vi.elements == 1)
        {
            WriteScriptData(writer, vi.type, vi.value.get());
        }
        else
        {
            for (size_t i = 0; i < vi.elements; ++i)
            {
                WriteScriptData(writer, vi.type, vi.value->GetArrayElement(i));
            }
        }
    }
}

void COMPILER::SaveFunctionsToCache(storm::script_cache::BufferWriter &writer)
{
    writer.WriteData(script_cache_.functions.size());
    for (auto &[fi, arguments, local_variables] : script_cache_.functions)
    {
        writer.WriteArray(fi.name);
        writer.WriteData(fi.offset);
        writer.WriteData(fi.return_type);
        writer.WriteArray(fi.decl_file_name);
        writer.WriteData(fi.decl_line);

        writer.WriteData(arguments.size());
        for (auto &[lvi, is_extern] : arguments)
        {
            writer.WriteArray(lvi.name);
            writer.WriteData(lvi.type);
            writer.WriteData(lvi.elements);
            writer.WriteData(is_extern);
        }

        writer.WriteData(local_variables.size());
        for (auto &lvi : local_variables)
        {
            writer.WriteArray(lvi.name);
            writer.WriteData(lvi.type);
            writer.WriteData(lvi.elements);
        }
    }
}

void COMPILER::SaveScriptLibrariesToCache(storm::script_cache::BufferWriter &writer)
{
    writer.WriteData(script_cache_.script_libs.size());
    for (auto &lib_name : script_cache_.script_libs)
    {
        writer.WriteArray(lib_name);
    }
}

void COMPILER::SaveEventHandlersToCache(storm::script_cache::BufferWriter &writer)
{
    writer.WriteData(script_cache_.event_handlers.size());
    for (auto &[event, handler] : script_cache_.event_handlers)
    {
        writer.WriteArray(event);
        writer.WriteArray(handler);
    }
}

void COMPILER::SaveByteCodeToCache(storm::script_cache::BufferWriter &writer, const SEGMENT_DESC &segment)
{
    auto code = std::string_view(segment.pCode, segment.pCode + segment.BCode_Program_size);
    writer.WriteArray(code);

    // relocations
    auto strings = std::unordered_map<uint32_t, std::string>();
    auto variables = std::unordered_map<uint32_t, std::string>();
    auto functions = std::unordered_map<uint32_t, std::string>();

    pRunCodeBase = segment.pCode;
    InstructionPointer = 0;
    auto token_type = S_TOKEN_TYPE();
    do
    {
        token_type = BC_TokenGet();
        auto &relocated_data = *reinterpret_cast<const uint32_t *>(&pRunCodeBase[TLR_DataOffset]);

        if (token_type == ACCESS_WORD_CODE)
        {
            strings.emplace(relocated_data, SCodec.Convert(relocated_data));
        }
        else if (token_type == VARIABLE)
        {
            auto vi = VarTab.GetVarX(relocated_data);
            variables.emplace(relocated_data, vi->name);
        }
        else if (token_type == CALL_FUNCTION)
        {
            auto fi = FuncInfo();
            FuncTab.GetFuncX(fi, relocated_data);
            functions.emplace(relocated_data, fi.name);
        }
    } while (token_type != END_OF_PROGRAMM);

    writer.WriteData(strings.size());
    for (auto &[string_code, string] : strings)
    {
        writer.WriteData(string_code);
        writer.WriteArray(string);
    }

    writer.WriteData(variables.size());
    for (auto &[variable_code, name] : variables)
    {
        writer.WriteData(variable_code);
        writer.WriteArray(name);
    }

    writer.WriteData(functions.size());
    for (auto &[function_code, name] : functions)
    {
        writer.WriteData(function_code);
        writer.WriteArray(name);
    }
}

void COMPILER::SaveSegmentToCache(const SEGMENT_DESC &segment)
{
    storm::script_cache::BufferWriter writer;

    // defines (in case of new compiled code depending on defines from cache)
    SaveDefinesToCache(writer);

    // variables with values
    SaveVariablesToCache(writer);

    // functions
    SaveFunctionsToCache(writer);

    // script libs
    SaveScriptLibrariesToCache(writer);

    // event handlers
    SaveEventHandlersToCache(writer);

    // bytecode
    SaveByteCodeToCache(writer, segment);

    const auto data = writer.GetDataBuffer();
    if (const auto error = storm::script_cache::writeEntry(GetSegmentCachePath(segment),
                                                           std::string_view(std::data(data), std::size(data)));
        !error.empty())
    {
        spdlog::warn("Script cache: {}; the segment is compiled again next start", error);
    }
}

void COMPILER::SaveDefinesToCache(storm::script_cache::BufferWriter &writer)
{
    writer.WriteData(script_cache_.defines.size());
    for (auto &[name, type, value] : script_cache_.defines)
    {
        writer.WriteArray(name);
        writer.WriteData(type);
        switch (type)
        {
        case NUMBER:
            writer.WriteData(static_cast<int32_t>(value));
            break;

        case FLOAT_NUMBER: {
            // data4b holds the float's bit pattern (Compile), not a number to convert.
            float bits;
            memcpy(&bits, &value, sizeof(bits));
            writer.WriteData(bits);
            break;
        }

        case STRING:
            writer.WriteArray(reinterpret_cast<const char *>(value));
            break;
        }
    }
}

bool COMPILER::LoadSegmentFromCache(SEGMENT_DESC &segment)
{
    const auto path = GetSegmentCachePath(segment);
    std::vector<char> data;
    switch (storm::script_cache::readEntry(path, data))
    {
    case storm::script_cache::Entry::Missing:
        return false;
    case storm::script_cache::Entry::Damaged: {
        spdlog::warn("Script cache entry '{}' is damaged; recompiling", path.string());
        std::error_code ec;
        std::filesystem::remove(path, ec);
        return false;
    }
    case storm::script_cache::Entry::Read:
        break;
    }

    storm::script_cache::BufferReader reader(std::move(data));

    try
    {
        // defines (in case of new compiled code depending on defines from cache)
        LoadDefinesFromCache(reader, segment);

        // variables with values
        LoadVariablesFromCache(reader, segment);

        // functions
        LoadFunctionsFromCache(reader, segment);

        // script libraries
        LoadScriptLibrariesFromCache(reader);

        // event handlers
        LoadEventHandlersFromCache(reader);

        // bytecode
        LoadByteCodeFromCache(reader, segment);
    }
    catch (const storm::script_cache::ReaderException &)
    {
        // Intact file that does not parse or apply: a layout bug or a name clash,
        // not disk damage. Undo what was registered for this segment and compile from source.
        spdlog::error("Script cache entry '{}' cannot be applied; recompiling", path.string());
        std::error_code ec;
        std::filesystem::remove(path, ec);
        delete[] segment.pCode;
        segment.pCode = nullptr;
        segment.BCode_Program_size = 0;
        segment.BCode_Buffer_size = 0;
        FuncTab.InvalidateBySegmentID(segment.id);
        VarTab.InvalidateBySegmentID(segment.id);
        EventTab.InvalidateBySegmentID(segment.id);
        DefTab.InvalidateBySegmentID(segment.id);
        return false;
    }

    return true;
}

void COMPILER::LoadDefinesFromCache(storm::script_cache::BufferReader &reader, SEGMENT_DESC &segment)
{
    const auto size = reader.Read<size_t>();
    for (size_t i = 0; i < size; ++i)
    {
        auto di = DEFINFO();

        di.segment_id = segment.id;
        auto name = std::string(reader.ReadArray());
        di.name = const_cast<char *>(name.c_str());
        di.deftype = reader.Read<uint32_t>();

        switch (di.deftype)
        {
        case NUMBER:
            di.data4b = static_cast<uintptr_t>(reader.Read<int32_t>());
            break;

        case FLOAT_NUMBER: {
            const float bits = reader.Read<float>();
            di.data4b = 0;
            memcpy(&di.data4b, &bits, sizeof(bits));
            break;
        }

        case STRING: {
            auto value = reader.ReadArray();
            const auto new_ptr = new char[value.size() + 1];
            std::ranges::copy(value, new_ptr);
            new_ptr[value.size()] = '\0';
            di.data4b = reinterpret_cast<uintptr_t>(new_ptr);
            break;
        }
        }

        DefTab.AddDef(di);
    }
}

void STRING_CODEC::VariableChanged()
{
}
