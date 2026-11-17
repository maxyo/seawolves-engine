#pragma once

#include "renderer.h"

class SCRSHOTER : public Entity
{
    struct SAVETEXTURES
    {
        char *fileName;
        char *dataString;
        int32_t textureId = -1;
        SAVETEXTURES *next;
    };

  public:
    ~SCRSHOTER() override;
    void SetDevice();
    bool Init() override;
    void Execute(uint32_t Delta_Time);
    void Realize(uint32_t Delta_Time);
    uint64_t ProcessMessage(MESSAGE &message) override;

    void ProcessStage(Stage stage, uint32_t delta) override
    {
        switch (stage)
        {
        case Stage::execute:
            Execute(delta);
            break;
        case Stage::realize:
            Realize(delta);
            break;
        }
    }

  private:
    bool MakeScreenShot();
    bool MakeScreenShotFromFrameCopy();
    bool MakeReadable();
    int32_t FindSaveTexture(const char *fileName) const;
    char *FindSaveData(const char *fileName) const;
    int32_t AddSaveTexture(const char *dirName, const char *fileName);
    void DelSaveTexture(const char *fileName);
    int32_t GetTexFromSave(char *fileName, char **pDatStr) const;
    
    VRenderer *rs = nullptr;
    int32_t textureIndex_ = -1;
    bool readable_ = false; // textureIndex_ can be locked (else it is a render target on the GPU: MakeReadable)
    SAVETEXTURES *m_list = nullptr;
};
