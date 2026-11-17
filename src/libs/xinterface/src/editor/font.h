#pragma once

#include "../inode.h"

class GIEditor;

class GIFont
{
  public:
    GIFont(GIFont &&) = delete;
    GIFont(const GIFont &) = delete;
    GIFont(GIEditor *pEditor, const char *pcFontName);
    ~GIFont();

    void Release() const;

    void Print(float x, float y, const char *pcFormat, ...) const;
    void SetHeight(float fHeight);

    float GetHeight() const
    {
        return m_fHeight;
    }

  protected:
    GIEditor *m_pEditor;
    int32_t m_nFontID;

    uint32_t m_dwColor;
    uint32_t m_dwBackColor;

    float m_fScale;
    float m_fHeight;
};
