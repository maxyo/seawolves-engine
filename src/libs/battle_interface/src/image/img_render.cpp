#include "img_render.h"
#include "storm_assert.h"

#include <algorithm>

BIImageRender::BIImageRender(VRenderer *pRS)
    : m_nBeginOutputPrioritet(0), m_nEndOutputPrioritet(0)
{
    m_pRS = pRS;
    m_fHScale = m_fVScale = 1.f;
    m_fHOffset = m_fVOffset = 0.f;
}

BIImageRender::~BIImageRender()
{
    Release();
}

void BIImageRender::Render()
{
    if (GetFirstPrioritetRange())
        do
        {
            for (int32_t n = 0; n < m_apMaterial.size(); n++)
                m_apMaterial[n]->Render(m_nBeginOutputPrioritet, m_nEndOutputPrioritet);
        } while (GetNextPrioritetRange());
}

IBIImage *BIImageRender::CreateImage(BIImageType type, const char *pcTextureName, uint32_t color, const FRECT &uv,
                                     const RECT &pos, int32_t nPrior, const char *pcTechniqueName)
{
    auto *pMaterial = CreateMaterial(pcTextureName, pcTechniqueName);
    if (pMaterial)
        return (IBIImage *)pMaterial->CreateImage(type, color, uv, pos.left, pos.top, pos.right, pos.bottom, nPrior);
    return nullptr;
}

BIImageMaterial *BIImageRender::FindMaterial(const char *pcTextureName, const char *pcTechniqueName)
{
    for (int32_t n = 0; n < m_apMaterial.size(); n++)
        if (m_apMaterial[n]->IsUseTexture(pcTextureName) && m_apMaterial[n]->IsUseTechnique(pcTechniqueName))
            return m_apMaterial[n];
    return nullptr;
}

BIImageMaterial *BIImageRender::CreateMaterial(const char *pcTextureName, const char *pcTechniqueName)
{
    auto *pMaterial = FindMaterial(pcTextureName, pcTechniqueName ? pcTechniqueName : "battle_tex_col_Rectangle");
    if (!pMaterial)
    {
        pMaterial = new BIImageMaterial(m_pRS, this);
        Assert(pMaterial);
        pMaterial->SetTexture(pcTextureName);
        pMaterial->SetTechnique(pcTechniqueName);
        pMaterial->UpdateFlagOn();
        int32_t n;
        for (n = 0; n < m_apMaterial.size(); n++)
            if (m_apMaterial[n]->GetMinPriority() > pMaterial->GetMinPriority())
                break;
        m_apMaterial.insert(m_apMaterial.begin() + n, pMaterial);
    }
    return pMaterial;
}

void BIImageRender::DeleteMaterial(BIImageMaterial *pMat)
{
    const auto it = std::find(m_apMaterial.begin(), m_apMaterial.end(), pMat);
    if (it != m_apMaterial.end())
        m_apMaterial.erase(it);
}

void BIImageRender::ReleaseAllImages()
{
    for (int32_t n = 0; n < m_apMaterial.size(); n++)
        m_apMaterial[n]->ReleaseAllImages();
}

void BIImageRender::MaterialSorting()
{
    for (auto bContinue = true; bContinue;)
    {
        bContinue = false;
        for (int32_t n = 1; n < m_apMaterial.size(); n++)
        {
            if (m_apMaterial[n]->GetMinPriority() < m_apMaterial[n - 1]->GetMinPriority())
            {
                std::swap(m_apMaterial[n - 1], m_apMaterial[n]);
                bContinue = true;
            }
        }
    }
}

void BIImageRender::Release()
{
    // destructors themselves erase pointers from vectors

    while (m_apMaterial.size() > 0)
        delete m_apMaterial.front();
}

bool BIImageRender::GetFirstPrioritetRange()
{
    if (m_apMaterial.size() == 0)
        return false;
    m_nBeginOutputPrioritet = m_apMaterial[0]->GetMinPriority();
    m_nEndOutputPrioritet = m_apMaterial[0]->GetMaxPriority();

    if (m_apMaterial.size() > 1 && m_nEndOutputPrioritet > m_apMaterial[1]->GetMinPriority())
        m_nEndOutputPrioritet = m_apMaterial[1]->GetMinPriority();
    return true;
}

bool BIImageRender::GetNextPrioritetRange()
{
    m_nBeginOutputPrioritet = ++m_nEndOutputPrioritet;
    int32_t n;
    for (n = 0; n < m_apMaterial.size(); n++)
    {
        if (m_apMaterial[n]->GetMaxPriority() >= m_nBeginOutputPrioritet)
        {
            m_nEndOutputPrioritet = m_apMaterial[n]->GetMaxPriority();
            for (int32_t i = n + 1; i < m_apMaterial.size(); i++)
                if (m_nBeginOutputPrioritet < m_apMaterial[i]->GetMinPriority())
                    m_nEndOutputPrioritet = m_apMaterial[i]->GetMinPriority();
            break;
        }
    }
    return (n < m_apMaterial.size());
}
