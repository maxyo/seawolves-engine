#include "services_list.h"
#include <cstdint>
#include <cstring>
#include <stdexcept>

SERVICES_LIST::SERVICES_LIST()
{
    List = nullptr;
    Search_module_code = 0;
    Objects = 0;
}

SERVICES_LIST::~SERVICES_LIST()
{
    Release();
}

SERVICE *SERVICES_LIST::Add(uint32_t module_code, uint32_t class_code, SERVICE *pointer)
{
    auto *const Pointer = AddRef(module_code);

    if (Pointer)
        return Pointer;

    auto *node_PTR = new SERVICE_NODE{};
    node_PTR->module_code = module_code;
    node_PTR->class_code = class_code;
    node_PTR->pointer = pointer;
    node_PTR->reference = 1;

    Objects++;

    if (List == nullptr)
    {
        List = node_PTR;
        return node_PTR->pointer;
    }

    node_PTR->linkL = List;
    List->linkR = node_PTR;
    List = node_PTR;
    return node_PTR->pointer;
}

SERVICE *SERVICES_LIST::AddRef(uint32_t module_code)
{
    auto *const node_PTR = FindNode(module_code);
    if (node_PTR == nullptr)
        return nullptr;
    node_PTR->reference++;
    return node_PTR->pointer;
}

SERVICE_NODE *SERVICES_LIST::FindNode(uint32_t module_code)
{
    auto *node_PTR = List;
    while (node_PTR)
    {
        if (node_PTR->module_code == module_code)
            return node_PTR;
        node_PTR = static_cast<SERVICE_NODE *>(node_PTR->linkL);
    }
    return nullptr;
}

void SERVICES_LIST::Release()
{
    SERVICE_NODE *node_PTR = List;
    while (node_PTR)
    {
        List = static_cast<SERVICE_NODE *>(node_PTR->linkL);
        delete node_PTR;
        node_PTR = List;
    }

    List = nullptr;
}

SERVICE *SERVICES_LIST::GetService(uint32_t &class_code)
{
    if (List)
    {
        Search_module_code = List->module_code;
        class_code = List->class_code;
        return List->pointer;
    }
    return nullptr;
}

SERVICE *SERVICES_LIST::GetServiceNext(uint32_t &class_code)
{
    SERVICE_NODE *node_PTR = List;
    while (node_PTR)
    {
        if (node_PTR->module_code == Search_module_code)
        {
            auto *node_L = static_cast<SERVICE_NODE *>(node_PTR->linkL);
            if (node_L)
            {
                Search_module_code = node_L->module_code;
                class_code = node_L->class_code;
                return node_L->pointer;
            }
            return nullptr;
        }
        node_PTR = static_cast<SERVICE_NODE *>(node_PTR->linkL);
    }
    return nullptr;
}

