#include "string.h"
#include "bi_defines.h"
#include "img_render.h"

BIString::BIString(BIImageRender *pImgRender, VRenderer *rs)
{
    m_pImgRender = pImgRender;
    Assert(m_pImgRender);
    m_pRS = rs;
    Assert(m_pRS);
    m_nPrioritet = ImagePrioritet_DefaultValue;
    m_bUpdateString = false;

    m_nFontID = -1;
    m_dwColor = 0xFFFFFFFF;
    m_fScale = 1.f;
}

BIString::~BIString()
{
}

void BIString::Render()
{
    if (m_bUpdateString)
    {
        UpdateString();
    }
    m_pRS->ExtPrint(m_nFontID, m_dwColor, 0, PR_ALIGN_LEFT, true, m_fScale, 0, 0, m_pntOutPos.x, m_pntOutPos.y, "%s",
                    m_sText.c_str());
}

void BIString::UpdateString()
{
    m_bUpdateString = false;
}
