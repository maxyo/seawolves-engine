#include "s_stack.h"
#include <stdexcept>
#include <string>

S_STACK::S_STACK()
{
    Buffer_size = 0;
    Data_num = 0;
    pVCompiler = nullptr;
}

S_STACK::~S_STACK()
{
    Release();
}

void S_STACK::Release()
{
    for (uint32_t n = 0; n < Buffer_size; n++)
    {
        delete pStackData[n];
    }
    pStackData.clear();
    Buffer_size = 0;
    Data_num = 0;
}

void S_STACK::Grow()
{
    if (Data_num > STACK_BUFFER_LIMIT)
        throw std::runtime_error("stack overflaw");
    const auto offset = Buffer_size;
    Buffer_size += STACK_BUFFER_BLOCK_SIZE;
    pStackData.resize(Buffer_size);
    for (auto n = offset; n < Buffer_size; n++)
    {
        pStackData[n] = new DATA;
        pStackData[n]->SetVCompiler(pVCompiler);
    }
}

void S_STACK::EmptyStack(const char *operation)
{
    throw std::runtime_error(std::string("stack '") + operation + "' error");
}
