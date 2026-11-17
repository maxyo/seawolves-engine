#pragma once

#include "token.h"
#include "v_data.h"

class VIRTUAL_COMPILER
{
  public:
    virtual void SetError(const char *data_PTR, ...){};

    virtual void SetWarning(const char *data_PTR, ...){};

    virtual VSTRING_CODEC *GetVSC()
    {
        return nullptr;
    }
};


class COMPILER;

class DATA final : public VDATA
{
    friend COMPILER;

    VIRTUAL_COMPILER *pVCompiler;
    S_TOKEN_TYPE Data_type;
    uint32_t Number_of_elements;
    bool bArray;
    int32_t lValue;
    uintptr_t pValue;
    float fValue;
    std::string sValue;

    bool bEntity;
    entid_t object_id;
    uint32_t Segment_id;

    DATA *pReference;

    std::vector<DATA> ArrayPTR;
    uint32_t nGlobalVarTableIndex;
    ATTRIBUTES::Handle arefHandle_{}; // an aref's AttributesClass, which it does not own

    void CopyAReference(const DATA &from)
    {
        AttributesClass = from.AttributesClass;
        arefHandle_ = from.arefHandle_;
    }

  public:
    ATTRIBUTES *AttributesClass;
    // An aref's attribute; nullptr, reported as a script error, when the aref is unset or names an attribute that
    // was deleted since (DeleteAttribute, a branch replaced by a copy, a destroyed object).
    ATTRIBUTES *AReferenceTarget();
    [[nodiscard]] bool AReferenceDangling() const
    {
        return Data_type == VAR_AREFERENCE && AttributesClass != nullptr && !ATTRIBUTES::IsAlive(arefHandle_);
    }
    DATA *GetReference() override
    {
        return pReference;
    }
    void SetReference(DATA *pRef) override;
    void SetAReference(ATTRIBUTES *pARef) override;
    void SetSegmentID(uint32_t id)
    {
        Segment_id = id;
    };

    uint32_t GetSegmentID()
    {
        return Segment_id;
    };
    void SetVCompiler(VIRTUAL_COMPILER *pVC);
    DATA();
    DATA(const DATA &);
    DATA(DATA &&) noexcept(false);
    DATA &operator=(const DATA &);
    DATA(S_TOKEN_TYPE _element_type);
    DATA(uint32_t num_of_elements, S_TOKEN_TYPE _element_type);
    ~DATA() override;
    void Set(int32_t value) override
    {
        if (Data_type == VAR_REFERENCE || bArray)
        {
            SetThroughReference(value);
            return;
        }
        Data_type = VAR_INTEGER;
        lValue = value;
    }
    void SetPtr(uintptr_t value) override;
    bool GetPtr(uintptr_t &value) override;

    void Set(float value) override;
    void Set(std::string value) override;
    // As Set(std::string), into the storage the value already has.
    void SetText(std::string_view value);
    void Set(const char* value) override;
    void Set(const char *attribute_name, const char *attribute_value) override;
    bool Get(int32_t &value) override;
    bool Get(float &value) override;
    bool Get(const char *&value) override;
    bool Get(const char *attribute_name, const char *&value) override;

    bool Set(int32_t value, uint32_t index) override;
    bool Set(float value, uint32_t index) override;
    bool Set(const char *value, uint32_t index) override;

    bool Get(int32_t &value, uint32_t index) override;
    bool Get(float &value, uint32_t index) override;
    bool Get(const char *&value, uint32_t index) override;

    bool Set(const char *attribute_name, const char *attribute_value, uint32_t index) override;
    bool Get(const char *attribute_name, const char *&value, uint32_t index) override;

    void Set(entid_t eid) override;
    void Get(entid_t &eid);

    bool Set(entid_t eid, uint32_t index) override;

    ATTRIBUTES *GetAClass() override;
    ATTRIBUTES *GetAClass(uint32_t index) override;

    // SetType and ClearType are part of every stack push: inlined there whatever the compiler's size limits say.
    [[gnu::always_inline]] void SetType(S_TOKEN_TYPE type, uint32_t array_size = 1)
    {
        ClearType();
        Data_type = type;
        if (array_size > 1)
        {
            MakeArray(array_size);
            return;
        }
        bArray = false;
        ArrayPTR.clear();
        Number_of_elements = 1;
    }
    bool Convert(S_TOKEN_TYPE type);
    void Error(const char *);
    DATA *GetArrayElement(uint32_t index) override;

    S_TOKEN_TYPE GetType()
    {
        return Data_type;
    };
    uint32_t GetElementsNum() override;
    void SetElementsNum(uint32_t asize) override;

    bool IsArray() override
    {
        return bArray;
    };
    // The elements of this variable itself; GetElementsNum of a `ref` follows the reference
    // (an array of refs answers 0 there).
    uint32_t GetArraySize() const
    {
        return bArray ? Number_of_elements : 1;
    }
    bool IsReference() override
    {
        return Data_type == VAR_REFERENCE;
    }
    bool IsAReference() override
    {
        return Data_type == VAR_AREFERENCE;
    }
    // The variable itself, or the one a chain of references ends at (nullptr: an unset reference, reported).
    DATA *GetVarPointer() override
    {
        return Data_type != VAR_REFERENCE ? this : GetReferencedVar();
    }
    [[gnu::always_inline]] void ClearType()
    {
        if (Data_type != VAR_AREFERENCE)
        {
            delete AttributesClass;
        }
        AttributesClass = nullptr;
        Data_type = UNKNOWN;
        pReference = nullptr;
        sValue.clear();
    }

    entid_t GetObjectIDPTR() override
    {
        return object_id;
    }

    // operations
    bool Inc() override;
    bool Dec() override;
    // Most copies move a number between the stack, a register and a local: those are done here.
    bool Copy(DATA *pV) override
    {
        if (pV && !bArray && !pV->bArray &&
            (Data_type == UNKNOWN || Data_type == VAR_INTEGER || Data_type == VAR_FLOAT || Data_type == VAR_STRING))
        {
            if (pV->Data_type == VAR_INTEGER)
            {
                Data_type = VAR_INTEGER;
                lValue = pV->lValue;
                return true;
            }
            if (pV->Data_type == VAR_FLOAT && pV->fValue == pV->fValue) // a NaN is reported by the full copy
            {
                Data_type = VAR_FLOAT;
                fValue = pV->fValue;
                return true;
            }
        }
        return CopyAny(pV);
    }
    bool Copy(DATA *pV, uint32_t index) override;
    bool CopyOnElement(DATA *pV, uint32_t index) override;
    bool Inverse() override;
    bool Power(int32_t Deg) override;
    bool Power(DATA *pV) override;
    bool Multiply(DATA *pV) override;
    bool Divide(DATA *pV) override;
    bool Modul(DATA *pV) override;
    bool Plus(DATA *pV) override;
    bool Minus(DATA *pV) override;
    bool Compare(DATA *pV, char opA, char opB) override;
    bool Neg() override;
    bool CompareAndSetResult(DATA *pV, S_TOKEN_TYPE op);

    bool BoolConvert();
    bool RefConvert();

    void BadIndex(uint32_t index, uint32_t array_size);

    int32_t GetInt() override;
    uintptr_t GetPtr() override;
    float GetFloat() override;
    const char *GetString() override;
    entid_t GetEntityID() override;

    void SetGlobalVarTableIndex(uint32_t index)
    {
        nGlobalVarTableIndex = index;
    };

    void Release();

  private:
    void SetThroughReference(int32_t value);
    bool CopyAny(DATA *pV);
    void MakeArray(uint32_t array_size);
    DATA *GetReferencedVar();
};
