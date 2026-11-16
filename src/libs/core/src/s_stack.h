#pragma once

#include "data.h"
#include "v_s_stack.h"

#define STACK_BUFFER_BLOCK_SIZE 512
#define STACK_BUFFER_LIMIT 65535

class S_STACK final : public VS_STACK
{
    std::vector<DATA *> pStackData;
    uint32_t Buffer_size;
    uint32_t Data_num;
    VIRTUAL_COMPILER *pVCompiler;

  public:
    S_STACK();
    ~S_STACK();
    void Release();

    // A cleared value on top (a copy of `pdataclass` when given).
    DATA *Push(DATA *pdataclass = nullptr) override
    {
        if (Data_num >= Buffer_size)
            Grow();
        DATA *const top = pStackData[Data_num];
        top->SetType(UNKNOWN);
        if (pdataclass)
        {
            top->Copy(pdataclass);
        }
        Data_num++;
        return top;
    }

    // The value stays as it is until the next Push reuses its place.
    DATA *Pop() override
    {
        if (Data_num == 0)
            EmptyStack("pop");
        Data_num--;
        return pStackData[Data_num];
    }

    DATA *Read(uint32_t offset, uint32_t index) override
    {
        if (offset + index >= Data_num)
            EmptyStack("read");
        return pStackData[offset + index];
    }

    DATA *Read() override
    {
        if (Data_num == 0)
            EmptyStack("read");
        return pStackData[Data_num - 1];
    }

    uint32_t GetDataNum() override
    {
        return Data_num;
    }

    void SetVCompiler(VIRTUAL_COMPILER *pvc)
    {
        pVCompiler = pvc;
    }

    void InvalidateFrom(uint32_t index) override
    {
        if (Data_num > index)
            Data_num = index;
    }

  private:
    void Grow();
    [[noreturn]] static void EmptyStack(const char *operation);
};
