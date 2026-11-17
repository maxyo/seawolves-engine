#pragma once

#include "data.h"

#define BUFFER_BLOCK_SIZE 4
#define INVALID_EVENT_CODE 0xffffffff
#define INVALID_SEGMENT_ID 0xffffffff

#define FSTATUS_NEW 0
#define FSTATUS_NORMAL 1
#define FSTATUS_DELETED 2

struct EVENT_FUNC_INFO
{
    uint32_t segment_id;
    uint32_t func_code;
    uint32_t status;
    bool bStatic;
};

struct EVENTINFO
{
    uint32_t hash;
    std::vector<EVENT_FUNC_INFO> pFuncInfo;
    char *name;
    uint32_t elements;
};

#define HASHTABLE_SIZE 64

class S_EVENTTAB
{
    uint32_t Buffer_size[HASHTABLE_SIZE];
    uint32_t Event_num[HASHTABLE_SIZE];
    std::vector<EVENTINFO> pTable[HASHTABLE_SIZE];
    // A handler was added as new or marked deleted since the last ProcessFrame: only then has a frame anything to do.
    bool pendingStatuses_ = false;
  public:
    S_EVENTTAB();
    ~S_EVENTTAB();
    void SetStatus(const char *event_name, uint32_t func_code, uint32_t status);
    uint32_t AddEventHandler(const char *event_name, uint32_t func_code, uint32_t func_segment_id, int32_t flag,
                             bool bStatic = false);
    bool DelEventHandler(uint8_t ti, uint32_t event_code, uint32_t func_code, bool bDelStatic = false);
    // The event of a code FindEvent returned, or nullptr. The pointer is good until a handler is added to the table
    // (a running handler may do that): ask again after every script call.
    const EVENTINFO *GetEvent(uint32_t event_code) const;
    uint32_t MakeHashValue(const char *string);
    void Release();
    void Clear();
    void InvalidateBySegmentID(uint32_t segment_id);
    uint32_t FindEvent(const char *event_name);
    void ProcessFrame();
};
