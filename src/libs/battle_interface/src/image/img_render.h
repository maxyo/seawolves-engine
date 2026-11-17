#pragma once

#include "material.h"

class BIImageRender
{
  public:
    BIImageRender(VRenderer *pRS);
    ~BIImageRender();

    void Render();
    IBIImage *CreateImage(BIImageType type, const char *pcTextureName, uint32_t color, const FRECT &uv, const RECT &pos,
                          int32_t nPrior = ImagePrioritet_DefaultValue, const char *pcTechniqueName = nullptr);

    BIImageMaterial *FindMaterial(const char *pcTextureName, const char *pcTechniqueName);
    BIImageMaterial *CreateMaterial(const char *pcTextureName, const char *pcTechniqueName = nullptr);
    void DeleteMaterial(BIImageMaterial *pMat);

    void ReleaseAllImages();
    void MaterialSorting();
    void TranslateBasePosToRealPos(float fXBase, float fYBase, float &fXReal, float &fYReal) const;


  protected:
    void Release();
    bool GetFirstPrioritetRange();
    bool GetNextPrioritetRange();

    int32_t m_nBeginOutputPrioritet;
    int32_t m_nEndOutputPrioritet;

    VRenderer *m_pRS;
    std::vector<BIImageMaterial *> m_apMaterial;

    float m_fHScale, m_fVScale;
    float m_fHOffset, m_fVOffset;
};

inline void BIImageRender::TranslateBasePosToRealPos(float fXBase, float fYBase, float &fXReal, float &fYReal) const
{
    fXReal = (m_fHOffset + fXBase) * m_fHScale;
    fYReal = (m_fVOffset + fYBase) * m_fVScale;
}
