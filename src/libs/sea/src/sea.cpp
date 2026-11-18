#include "loading_transaction.hpp"
#include "sea.h"
#include "sea_surface_gpu.hpp"
#include "storm/render/sea_surface_check.hpp"

#include <algorithm>
#include <cmath>
#include <execution>
#include <thread>

#include "core.h"
#include "sse.h"
#include "math_inlines.h"
#include "shared/sea_ai/script_defines.h"
#include "tga.h"
#include "v_file_service.h"

#include <fmt/format.h>
#include <spdlog/spdlog.h>

CREATE_CLASS(SEA)


#define NUM_VERTEXS 65500
#define NUM_INDICES 165000
// The sea is drawn with 16-bit indices. FitBlocksToBuffer keeps every block's vertex grid ((size0 + 1)^2 points, the
// pIndices entries) within NUM_VERTEXS, hence its 2 * size0^2 triangles within the index buffer.
static_assert(NUM_VERTEXS <= 65536, "sea vertices are indexed with uint16_t");
static_assert(2 * NUM_VERTEXS <= NUM_INDICES, "the triangles of NUM_VERTEXS grid points must fit the index buffer");

#define FRAMES 64
#define XWIDTH 128
#define YWIDTH 128

#define GC_CONSTANT 0 // Global constants = {0.0, 1.0, 0.5, 0.0000152590218967 = (0.5 / 32767.5)}
#define GC_CONSTANT2 1 // Global constants 2 = {2.0, -1.0, 0.00036621652552071 = (12 / 32767.5), fog}
#define GC_ANIMATION 2 // Animation frames(0.0-1.0) for 4 stages = {stage0, stage1, stage2, stage3}
#define GC_SHADOW_CONST1 3
#define GC_SHADOW_CONST2 7
#define GC_LIGHT4 11 //
#define GC_LIGHT3 13
#define GC_LIGHT2 15
#define GC_LIGHT1 17
#define GC_LIGHT0 19
#define GC_MATERIAL 21 //
#define GC_CAMERA_POS 23 // Local Camera position = {x, y, z, 0.0}
#define GC_MTX_WVP 24 // c[0] = mWorld * mView * mProjection

#define GC_FREE 28

SEA *SEA::pSea = nullptr;

SEA::SEA()
{
    aBlocks.reserve(128);
    aSeaTrash.reserve(512);
    aTrashRects.reserve(512);
    aSeaLights.reserve(512);
    aLightsRects.reserve(512);
    aNormals.reserve(FRAMES);

    dwMaxDim = 65536 * 2;
    dwMinDim = 128;

    fMaxSeaHeight = 20.0f;
    fMaxSeaDistance = 1600.0f;
    fGridStep = 0.06f;
    fLodScale = 0.4f;

    fAmp1 = 1.0f;
    fFrame1 = 0.0f;
    fAnimSpeed1 = 1.0f;
    vMove1 = glm::vec3(0.0f);
    fScale1 = 0.4f;

    fAmp2 = 1.0f;
    fFrame2 = 10.0f;
    fAnimSpeed2 = 1.0f;
    vMove2 = glm::vec3(0.0f);
    fScale2 = 2.0f;

    fFoamV = 3.0f;
    fFoamK = 0.0f;
    fFoamUV = 3.0f;
    fFoamTextureDisturb = 0.7f;
    iFoamTexture = -1;
    bIniFoamEnable = false;
    bFoamEnable = false;

    iSeaLightTexture = -1;
    fLastLightTime = 0.0f;

    fLastTrashTime = 0.0f;
    iSeaTrashTexture = -1;
    bUnderSea = false;
    bUnderSeaStarted = false;
    bUnderSeaEnable = false;
    bStarted = false;
    bSimpleSea = false;

    fPosShift = 2.0f;

    fFogSeaDensity = 0.0f;
    fFogStartDistance = 0.0f;
    vFogColor = glm::vec3(0.0f);
    bFogEnable = false;

    iVSeaBuffer = -1;
    iISeaBuffer = -1;

    bTempFullMode = false;

    pSea = this;

    bStop = false;

    vWorldOffset = glm::vec3(0.f);

}

SEA::~SEA()
{
    if (dwFramesCut)
        spdlog::warn("sea: in {} frames the farthest blocks did not fit the vertex buffer, at most {} blocks at once",
                     dwFramesCut, dwMostBlocksCut);
    ReleaseTargets();
    rs->TextureRelease(volumeTexture);
    rs->TextureRelease(iFieldTexture);

    rs->TextureRelease(iSeaTrashTexture);
    rs->TextureRelease(iSeaLightTexture);

    if (iVSeaBuffer >= 0)
        rs->ReleaseVertexBuffer(iVSeaBuffer);
    iVSeaBuffer = -1;
    if (iISeaBuffer >= 0)
        rs->ReleaseIndexBuffer(iISeaBuffer);
    iISeaBuffer = -1;

    if (iFoamTexture >= 0)
        rs->TextureRelease(iFoamTexture);
    iFoamTexture = -1;

    delete[] pIndices;
    pIndices = nullptr;
    delete[] pVSea;
    pVSea = nullptr;

    for (int32_t i = 0; i < aBumps.size(); i++)
        delete[] aBumps[i];
    for (int32_t i = 0; i < aNormals.size(); i++)
        delete[] aNormals[i];
}

void SEA::SFLB_CreateBuffers()
{
    iVSeaBuffer = rs->CreateVertexBuffer(0, NUM_VERTEXS * sizeof(SeaVertex), D3DUSAGE_WRITEONLY | D3DUSAGE_DYNAMIC);
    iISeaBuffer = rs->CreateIndexBuffer(NUM_INDICES * 3 * sizeof(uint16_t), D3DUSAGE_WRITEONLY | D3DUSAGE_DYNAMIC);

    pIndices = new uint32_t[NUM_VERTEXS * 3];
    pVSea = new SeaVertex[NUM_VERTEXS];
}

void SEA::CreateVertexDeclaration()
{
    if (vertexDecl_)
        return;

    const D3DVERTEXELEMENT9 VertexElements[] = {
        {0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
        {0, 12, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_NORMAL, 0},
        {0, 24, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
        D3DDECL_END()};

    vertexDecl_ = rs->CreateVertexDeclaration(VertexElements);
}

bool SEA::Init()
{
    rs = static_cast<VRenderer *>(core.GetService("renderer"));
    CreateVertexDeclaration();
    {
        auto pEngineIni = fio->OpenIniFile(core.EngineIniFileName());
        bIniFoamEnable = (pEngineIni) ? pEngineIni->GetInt("Sea", "FoamEnable", 1) != 0 : false;
    }

    iFoamTexture = rs->TextureCreate("weather\\sea\\pena\\pena.tga");

    SFLB_CreateBuffers();

    // The animated bump normals: one volume texture, slices = frames, 4 mip levels.
    volumeTexture = rs->VolumeTextureCreate(XWIDTH, YWIDTH, FRAMES, 4, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED);
    if (volumeTexture < 0)
        throw std::runtime_error("SEA: cannot create the bump volume texture");

    CreateTargets();

    iSeaTrashTexture = rs->TextureCreate("seatrash.tga");
    iSeaLightTexture = rs->TextureCreate("sealight.tga");

    uint8_t bMin = 0xFF;
    uint8_t bMax = 0;

    std::vector<uint8_t *> aTmpBumps;

    uint32_t i;

    for (i = 0; i < FRAMES; i++)
    {
        char str[256];
        char *pFBuffer = nullptr;
        uint32_t dwSize;
        sprintf_s(str, "resource\\sea\\sea%.4d.tga", i);
        fio->LoadFile(str, &pFBuffer, &dwSize);
        if (!pFBuffer)
        {
            core.Trace("Sea: Can't load %s", str);
            return false;
        }

        auto *pFB = pFBuffer + sizeof(TGA_H);

        auto *pBuffer = new uint8_t[XWIDTH * YWIDTH];
        aTmpBumps.push_back(pBuffer);

        for (uint32_t y = 0; y < YWIDTH; y++)
            for (uint32_t x = 0; x < XWIDTH; x++)
            {
                const uint8_t bB = (*pFB);
                if (bB < bMin)
                    bMin = bB;
                if (bB > bMax)
                    bMax = bB;
                pBuffer[x + y * XWIDTH] = bB & 0xFF;
                pFB += sizeof(uint32_t);
            }

        delete[] pFBuffer;
        pFBuffer = nullptr;
    }

    for (i = 0; i < FRAMES; i++)
    {
        auto *pBuffer = new uint8_t[XWIDTH * YWIDTH];
        aBumps.push_back(pBuffer);

        for (uint32_t y = 0; y < YWIDTH; y++)
            for (uint32_t x = 0; x < XWIDTH; x++)
            {
                const auto dwAddress = x + y * YWIDTH;
                float b1, b2, b3, b4, b5; // -2 -1 0 1 2

                b1 = 0.08f * static_cast<float>(aTmpBumps[(i - 2) & (FRAMES - 1)][dwAddress]);
                b2 = 0.17f * static_cast<float>(aTmpBumps[(i - 1) & (FRAMES - 1)][dwAddress]);
                b3 = 0.50f * static_cast<float>(aTmpBumps[(i - 0) & (FRAMES - 1)][dwAddress]);
                b4 = 0.17f * static_cast<float>(aTmpBumps[(i + 1) & (FRAMES - 1)][dwAddress]);
                b5 = 0.08f * static_cast<float>(aTmpBumps[(i + 2) & (FRAMES - 1)][dwAddress]);

                pBuffer[dwAddress] = aTmpBumps[(i - 0) & (FRAMES - 1)][dwAddress];
            }
    }

    for (const auto *bump : aTmpBumps)
        delete[] bump;

    BuildVolumeTexture();
    BuildFieldTexture();
    UpdateWaves();

    EditMode_Update();

    return true;
}

void SEA::BuildVolumeTexture()
{
    std::vector<glm::vec3 *> aVectors;
    uint32_t i, j;

    // The per-texel normals are the heights' alone (bSimpleSea changes only the texture): made once, and their buffers
    // stay where they are for the sea's life (the waves' frame blends point into them).
    const bool bNormals = aNormals.empty();

    D3DLOCKED_BOX box[4];

    for (i = 0; i < 4; i++)
        if (!rs->TextureLockBox(volumeTexture, i, box[i], 0))
            throw std::runtime_error("SEA: cannot fill the bump volume texture");

    // build normals

    aVectors.reserve(FRAMES);
    for (i = 0; i < FRAMES; i++)
    {
        if (bNormals)
            aNormals.push_back(new uint32_t[XWIDTH * YWIDTH]);

        auto *pVectors = new glm::vec3[XWIDTH * YWIDTH];
        aVectors.push_back(pVectors);

        for (uint32_t y = 0; y < YWIDTH; y++)
            for (uint32_t x = 0; x < XWIDTH; x++)
            {
#define GET_MASSIVE(dx, dy)                                                                                            \
    (float(pMassive[((x + dx) & (XWIDTH - 1)) + ((y + dy) & (XWIDTH - 1)) * XWIDTH] & 0xFF) / 255.0f)
                uint8_t *pMassive = aBumps[i];

                float fCenter = GET_MASSIVE(0, 0);

                float fLeft = GET_MASSIVE(-1, 0) - fCenter;
                float fRight = GET_MASSIVE(1, 0) - fCenter;
                float fTop = GET_MASSIVE(0, -1) - fCenter;
                float fBottom = GET_MASSIVE(0, 1) - fCenter;

                glm::vec3 vRes, vRes1, d0, d1, d2, d3;

                uint32_t dwNums = 0;
                if (fLeft < 0.0f)
                    dwNums++;
                if (fRight < 0.0f)
                    dwNums++;
                if (fTop < 0.0f)
                    dwNums++;
                if (fBottom < 0.0f)
                    dwNums++;

                float d = 1.0f;
                d0 = glm::vec3(-1.f, d * fLeft, 0.f);
                d1 = glm::vec3(0.f, d * fTop, -1.f);
                d2 = glm::vec3(1.f, d * fRight, 0.f);
                d3 = glm::vec3(0.f, d * fBottom, 1.f);


                glm::vec3 v1 = glm::cross(d1, d0);
                glm::vec3 v2 = glm::cross(d3, d2);
                glm::vec3 v3 = glm::cross(d0, d3);
                glm::vec3 v4 = glm::cross(d2, d1);

                vRes = vRes1 = storm::math::normalizeOrZero(glm::cross(d1, d0) + glm::cross(d3, d2) + glm::cross(d0, d3) + glm::cross(d2, d1));
                if (bSimpleSea)
                {
                    vRes1 = storm::math::normalizeOrZero(vRes * glm::vec3(100.0f, 1.0f, 100.0f));
                }
                uint32_t dwRes = MAKELONG(static_cast<short>(vRes.x * 32767.5f), static_cast<short>(vRes.z * 32767.5f));

                aNormals[i][x + y * XWIDTH] = dwRes;
                aVectors[i][x + y * XWIDTH] = vRes1;

                int32_t red = fftol((vRes1.x * 0.5f + 0.5f) * 255.0f);   // FIX-ME no ftol
                int32_t green = fftol((vRes1.y * 0.5f + 0.5f) * 255.0f); // FIX-ME no ftol
                int32_t blue = fftol((vRes1.z * 0.5f + 0.5f) * 255.0f);  // FIX-ME no ftol

                if (bSimpleSea)
                    *(uint32_t *)&(
                        static_cast<char *>(box[0].pBits)[i * box[0].SlicePitch + y * box[0].RowPitch + x * 4]) =
                        ARGB(0x80, blue, blue, red);
                else
                    *(uint32_t *)&(
                        static_cast<char *>(box[0].pBits)[i * box[0].SlicePitch + y * box[0].RowPitch + x * 4]) =
                        ARGB(0x80, blue, green, red);
            }
    }

    for (j = 1; j < 4; j++)
    {
        auto *pVectors = new glm::vec3[(XWIDTH >> j) * (YWIDTH >> j)];

        for (i = 0; i < (FRAMES >> j); i++)
        {
            // calculate current pVectors
            for (uint32_t y = 0; y < (YWIDTH >> j); y++)
                for (uint32_t x = 0; x < (XWIDTH >> j); x++)
                {
                    int32_t iNumVectors = 0;
                    glm::vec3 vVec(0.0f);
                    uint32_t dwW = 1 << j;
                    for (uint32_t k = i * dwW; k < (i + 1) * dwW; k++)
                    {
                        for (uint32_t yy = y * dwW; yy < (y + 1) * dwW; yy++)
                            for (uint32_t xx = x * dwW; xx < (x + 1) * dwW; xx++)
                            {
                                iNumVectors++;
                                vVec += aVectors[k][xx + yy * XWIDTH];
                            }
                    }
                    pVectors[x + y * (XWIDTH >> j)] = vVec / static_cast<float>(iNumVectors);
                }

            for (uint32_t y = 0; y < (YWIDTH >> j); y++)
                    for (uint32_t x = 0; x < (XWIDTH >> j); x++)
                    {
                        int32_t red = fftol((pVectors[x + y * (XWIDTH >> j)].x * 0.5f + 0.5f) * 255.0f); // FIX-ME no ftol
                        int32_t green = fftol((pVectors[x + y * (XWIDTH >> j)].y * 0.5f + 0.5f) * 255.0f);
                        // FIX-ME no ftol
                        int32_t blue = fftol((pVectors[x + y * (XWIDTH >> j)].z * 0.5f + 0.5f) * 255.0f); // FIX-ME no ftol

                        if (bSimpleSea)
                            *(uint32_t *)&(static_cast<char *>(
                                box[j].pBits)[i * box[j].SlicePitch + y * box[j].RowPitch + x * 4]) =
                                ARGB(0x80, blue, blue, red);
                        else
                            *(uint32_t *)&(static_cast<char *>(
                                box[j].pBits)[i * box[j].SlicePitch + y * box[j].RowPitch + x * 4]) =
                                ARGB(0x80, blue, green, red);
                    }
        }
        delete[] pVectors;
    }

    for (i = 0; i < 4; i++)
        rs->TextureUnlockBox(volumeTexture, i);

    for (const auto &vector : aVectors)
        delete[] vector;
    aVectors.clear();
}

bool SEA::EditMode_Update()
{
    v4SeaColor = glm::vec4(10.0f / 255.0f, 55.0f / 255.0f, 100.0f / 255.0f, 1.0f);
    v4SkyColor = glm::vec4(1.0f, 1.0f, 1.0f, 1.0f);

    const float fReflection = 0.8f;
    const float fTransparency = 0.7f;
    fFrenel = 0.75f; // the member: a local here left it unset until the script sets Sea.Frenel
    const float fAttenuation = 0.9f;
    v4SeaParameters = glm::vec4(fAttenuation, fReflection, fTransparency, 0.0f);

    fAmp1 = _fAmp1 = 8.0f;
    fAnimSpeed1 = 6.0f;
    fScale1 = 0.5f;
    vMoveSpeed1 = glm::vec3(5.0f, 0.0f, 0.0f);

    fAmp2 = _fAmp2 = 1.0f;
    fAnimSpeed2 = 3.0f;
    fScale2 = 2.0f;
    vMoveSpeed2 = glm::vec3(0.0f, 0.0f, 2.0f);

    fBumpScale = 0.1f;
    fBumpSpeed = 1.0f;
    fGridStep = 0.07f;
    fLodScale = 0.5f;

    fPosShift = 1.2f;

    Realize(0);

    return true;
}

int32_t SEA::VisCode(const glm::vec3 &vP)
{
    int32_t vc = 0;


    // Max - so the accuracy is much higher, blocks should not disappear sometimes in the distance
    PLANE *p = &pFrustumPlanes[0];
    if (p->Nx * vP.x + p->Ny * vP.y + p->Nz * vP.z < p->D)
        vc |= 0x01;
    p = &pFrustumPlanes[1];
    if (p->Nx * vP.x + p->Ny * vP.y + p->Nz * vP.z < p->D)
        vc |= 0x02;
    p = &pFrustumPlanes[2];
    if (p->Nx * vP.x + p->Ny * vP.y + p->Nz * vP.z < p->D)
        vc |= 0x04;
    p = &pFrustumPlanes[3];
    if (p->Nx * vP.x + p->Ny * vP.y + p->Nz * vP.z < p->D)
        vc |= 0x08;

    return vc;
}

bool SEA::isVisibleBBox(const glm::vec3 &vCenter, const glm::vec3 &v1, const glm::vec3 &v2)
{

    // if sphere not visible - return

    // check box visible
    int32_t vc = 0xFF;
    vc &= VisCode(glm::vec3(v1.x, v1.y, v1.z));
    if (vc == 0)
        return true;
    vc &= VisCode(glm::vec3(v1.x, v2.y, v1.z));
    vc &= VisCode(glm::vec3(v2.x, v1.y, v1.z));
    vc &= VisCode(glm::vec3(v2.x, v2.y, v1.z));
    if (vc == 0)
        return true;
    vc &= VisCode(glm::vec3(v1.x, v1.y, v2.z));
    vc &= VisCode(glm::vec3(v1.x, v2.y, v2.z));
    vc &= VisCode(glm::vec3(v2.x, v1.y, v2.z));
    vc &= VisCode(glm::vec3(v2.x, v2.y, v2.z));

    return vc == 0;
}

inline float SEA::CalcLod(const float &x, const float &y, const float &z)
{
    return Sqr(x - vCamPos.x) + /*Sqr((y - vCamPos.y)) + */ Sqr(z - vCamPos.z);
}

void SEA::CalculateLOD(const glm::vec3 &v1, const glm::vec3 &v2, int32_t &iMaxLOD, int32_t &iMinLOD)
{
    float fCur, fMax, fMin;

    fCur = CalcLod(v1.x, vSeaCenterPos.y, v1.z);
    fMax = fCur;
    fCur = CalcLod(v2.x, vSeaCenterPos.y, v1.z);
    if (fCur > fMax)
        fMax = fCur;
    fCur = CalcLod(v1.x, vSeaCenterPos.y, v2.z);
    if (fCur > fMax)
        fMax = fCur;
    fCur = CalcLod(v2.x, vSeaCenterPos.y, v2.z);
    if (fCur > fMax)
        fMax = fCur;

    if (vCamPos.x < v1.x)
    {
        if (vCamPos.z < v1.z)
            fMin = CalcLod(v1.x, vSeaCenterPos.y, v1.z);
        else if (vCamPos.z > v2.z)
            fMin = CalcLod(v1.x, vSeaCenterPos.y, v2.z);
        else
            fMin = CalcLod(v1.x, vSeaCenterPos.y, vCamPos.z);
    }
    else if (vCamPos.x > v2.x)
    {
        if (vCamPos.z < v1.z)
            fMin = CalcLod(v2.x, vSeaCenterPos.y, v1.z);
        else if (vCamPos.z > v2.z)
            fMin = CalcLod(v2.x, vSeaCenterPos.y, v2.z);
        else
            fMin = CalcLod(v2.x, vSeaCenterPos.y, vCamPos.z);
    }
    else
    {
        if (vCamPos.z < v1.z)
            fMin = CalcLod(vCamPos.x, vSeaCenterPos.y, v1.z);
        else if (vCamPos.z > v2.z)
            fMin = CalcLod(vCamPos.x, vSeaCenterPos.y, v2.z);
        else
            fMin = CalcLod(vCamPos.x, vSeaCenterPos.y, vCamPos.z);
    }

    // At least 1 under the logarithm: a block around the camera has fMin = 0, and log(0) = -inf has no int (undefined
    // behaviour; arm64 and x86 convert it differently). Below LOD 4 the result is clamped anyway.
    iMaxLOD = static_cast<int>(0.5f * logf(std::max(fLodScale * fMax, 1.0f)) / logf(2));
    if (iMaxLOD < 4)
        iMaxLOD = 4;
    iMinLOD = static_cast<int>(0.5f * logf(std::max(fLodScale * fMin, 1.0f)) / logf(2));
    if (iMinLOD < 4)
        iMinLOD = 4;
}

void SEA::AddBlock(int32_t iTX, int32_t iTY, int32_t iSize, int32_t iLOD)
{
    aBlocks.emplace_back(SeaBlock{0, 0, 0, 0, iSize >> iLOD, iTX, iTY, iSize, iLOD, 0, 0, 0});
}

// The block tree is anchored to the world. It spans twice MaxDim grid cells and its origin follows the camera only
// in steps of a quarter of that, so every block of the levels below the top two sits on the same world grid before
// and after a step, with the same vertices and the same LOD (both depend on the block's world extent and the camera
// only); the top two levels are far beyond the sea's relief whenever they are leaves. The camera stays in the second
// quarter of each axis, at least MaxDim / 2 cells from the edges, as far as with the original placement. That one
// stepped by 256 grid cells: the partition of every larger block and the vertices of the coarse LODs (36 m and wider
// between vertices, beyond ~700 m) moved to other world points at each step, about every 18 m of camera travel, and
// the far band of the sea changed its shape and shading in one frame (tests/sea_tree_continuity).
uint32_t SEA::TreeDim() const
{
    return dwMaxDim * 2;
}

void SEA::PlaceTree()
{
    const float fStep = fGridStep * static_cast<float>(TreeDim() / 4);
    vSeaCenterPos = glm::vec3(fStep * (std::floor(vCamPos.x / fStep) - 2.0f), fMaxSeaHeight * 0.5f,
                              fStep * (std::floor(vCamPos.z / fStep) - 2.0f));
}

void SEA::BuildTree(int32_t iTX, int32_t iTY, int32_t iLev)
{
    int32_t iMaxLOD, iMinLOD;
    const int32_t iSize = static_cast<int32_t>(TreeDim() >> iLev);
    const float fGSize = fGridStep * iSize;

    const glm::vec3 v1 = vSeaCenterPos + glm::vec3(iTX * fGSize, -fMaxSeaHeight / 2.0f, iTY * fGSize);
    const glm::vec3 v2 = v1 + glm::vec3(fGSize, fMaxSeaHeight, fGSize);

    if (!isVisibleBBox(vSeaCenterPos, v1, v2))
        return;

    CalculateLOD(v1, v2, iMaxLOD, iMinLOD);

    if (iSize <= static_cast<int32_t>(dwMinDim) || iMaxLOD - iMinLOD <= 1)
    {
        AddBlock(iTX, iTY, iSize, iMinLOD);
        return;
    }

    iTX *= 2;
    iTY *= 2;
    iLev++;

    BuildTree(iTX, iTY, iLev);
    BuildTree(iTX + 1, iTY, iLev);
    BuildTree(iTX, iTY + 1, iLev);
    BuildTree(iTX + 1, iTY + 1, iLev);
}

// INTEL COMMENT:
// This version of the function takes in 4 at a time, to take advantage of SSE.
// In particular I have converted the normalise and square roots over to SSE.
void SEA::SSE_WaveXZ(SeaVertex **pArray)
{
    glm::vec3 vNormal[4];
    float nY1[4];
    float nY2[4];
    bool bDistancePass = false;

    for (int i = 0; i < 4; i++)
    {
        float fDistance = Sqr(pArray[i]->vPos.x - vCamPos.x) + Sqr(pArray[i]->vPos.z - vCamPos.z);
        if (fDistance > fMaxSeaDistance * fMaxSeaDistance)
        {
            vNormal[i].x = 0.0f;
            nY1[i] = 1.0f;
            nY2[i] = 1.0f;
            vNormal[i].y = 1.0f;
            vNormal[i].z = 0.0f;
            pArray[i]->vPos.y = 0.0f;
            continue;
        }

        bDistancePass = true;

        float nX1, nZ1, nX2, nZ2;
        pArray[i]->vPos.y = SampleWave(waves_[0], pArray[i]->vPos.x, pArray[i]->vPos.z, nX1, nZ1);
        pArray[i]->vPos.y += SampleWave(waves_[1], pArray[i]->vPos.x, pArray[i]->vPos.z, nX2, nZ2);

        nY1[i] = 1.0f - (Sqr(nX1) + Sqr(nZ1));
        nY2[i] = 1.0f - (Sqr(nX2) + Sqr(nZ2));

        vNormal[i].x = waves_[0].scale * waves_[0].amp * nX1 + waves_[1].scale * waves_[1].amp * nX2;
        vNormal[i].z = waves_[0].scale * waves_[0].amp * nZ1 + waves_[1].scale * waves_[1].amp * nZ2;
    }

    if (bDistancePass)
    {
        // INTEL COMMENT:
        // xmm variables.
        SeaFloat4 m128X, m128Y, m128Z;
        SeaFloat4 m128NX, m128NY, m128NZ;
        SeaFloat4 m128Y1, m128Y2;
        SeaFloat4 m128SY1, m128SY2;

        // INTEL COMMENT:
        // Gather the Y's.
        SSE_GatherFourFloats(&nY1[0], &nY1[1], &nY1[2], &nY1[3], &m128Y1);
        SSE_GatherFourFloats(&nY2[0], &nY2[1], &nY2[2], &nY2[3], &m128Y2);
        m128SY1 = SeaSqrt(m128Y1);
        m128SY2 = SeaSqrt(m128Y2);
        m128Y = SeaAdd(m128SY1, m128SY2);

        // INTEL COMMENT:
        // Gather the X's and Z's.
        SSE_GatherFourFloats(&vNormal[0].x, &vNormal[1].x, &vNormal[2].x, &vNormal[3].x, &m128X);
        SSE_GatherFourFloats(&vNormal[0].z, &vNormal[1].z, &vNormal[2].z, &vNormal[3].z, &m128Z);

        // INTEL COMMENT:
        // Normalise.
        SSE_Normalise(m128X, m128Y, m128Z, &m128NX, &m128NY, &m128NZ);

        // INTEL COMMENT:
        // Scatter back out.
        SSE_ScatterFourFloats(&pArray[0]->vNormal.x, &pArray[1]->vNormal.x, &pArray[2]->vNormal.x,
                              &pArray[3]->vNormal.x, m128NX);
        SSE_ScatterFourFloats(&pArray[0]->vNormal.y, &pArray[1]->vNormal.y, &pArray[2]->vNormal.y,
                              &pArray[3]->vNormal.y, m128NY);
        SSE_ScatterFourFloats(&pArray[0]->vNormal.z, &pArray[1]->vNormal.z, &pArray[2]->vNormal.z,
                              &pArray[3]->vNormal.z, m128NZ);
    }
    else
    {
        pArray[0]->vNormal = vNormal[0];
        pArray[1]->vNormal = vNormal[1];
        pArray[2]->vNormal = vNormal[2];
        pArray[3]->vNormal = vNormal[3];
    }
}

void SEA::AddWaveSource(const storm::render::EffectsWaveSource &source)
{
    waveSources_.push_back(source);
}

// The field WaveXZ below samples, for the coast foam's vertex shader (vs_port_coastfoam.sc): keep the two together.
bool SEA::HeightField(SeaHeightField &field) const
{
    if (aBumps.size() != FRAMES)
        return false;
    field.frames = aBumps.data();
    field.frameCount = FRAMES;
    field.size = XWIDTH;
    field.frame[0] = fFrame1;
    field.frame[1] = fFrame2;
    field.scale[0] = fScale1;
    field.scale[1] = fScale2;
    field.amplitude[0] = fAmp1;
    field.amplitude[1] = fAmp2;
    field.move[0] = vMove1;
    field.move[1] = vMove2;
    field.camera = vCamPos;
    field.reach = fMaxSeaDistance;
    return true;
}

// The frame `fFrame` blended with the next one (frames wrap), texel by texel as SampleWave reads them.
SEA::FrameBlend SEA::WaveBlend(float fFrame) const
{
    const int32_t iFrame1 = fftol(fFrame) % static_cast<int32_t>(aBumps.size());
    const int32_t iFrame2 = (iFrame1 + 1) % static_cast<int32_t>(aBumps.size());
    return {aBumps[iFrame1], aBumps[iFrame2], aNormals[iFrame1], aNormals[iFrame2], fFrame - iFrame1};
}

// One wave at the world position (x, z): its height in metres (the frames' blend sampled bilinearly, wrapping) and
// the bilinear blend of the per-texel normals' x and z.
float SEA::SampleWave(const Wave &wave, float x, float z, float &nX, float &nZ)
{
    const float x1 = (x + wave.move.x) * wave.scale;
    const float z1 = (z + wave.move.z) * wave.scale;
    int32_t iX1 = ffloor(x1 + 0.0f), iX2 = iX1 + 1;
    int32_t iY1 = ffloor(z1 + 0.0f), iY2 = iY1 + 1;
    const float fX = (x1 - iX1);
    const float fZ = (z1 - iY1);
    iX1 &= (XWIDTH - 1);
    iX2 &= (XWIDTH - 1);
    iY1 &= (XWIDTH - 1);
    iY2 &= (XWIDTH - 1);

    const FrameBlend &b = wave.blend;
    const float a1 = b.Height(iX1 + iY1 * XWIDTH);
    const float a2 = b.Height(iX2 + iY1 * XWIDTH);
    const float a3 = b.Height(iX1 + iY2 * XWIDTH);
    const float a4 = b.Height(iX2 + iY2 * XWIDTH);

    float nx1, nx2, nx3, nx4, nz1, nz2, nz3, nz4;
    b.Normal(iX1 + iY1 * XWIDTH, nx1, nz1);
    b.Normal(iX2 + iY1 * XWIDTH, nx2, nz2);
    b.Normal(iX1 + iY2 * XWIDTH, nx3, nz3);
    b.Normal(iX2 + iY2 * XWIDTH, nx4, nz4);
    nX = (nx1 + fX * (nx2 - nx1) + fZ * (nx3 - nx1) + fX * fZ * (nx4 + nx1 - nx2 - nx3));
    nZ = (nz1 + fX * (nz2 - nz1) + fZ * (nz3 - nz1) + fX * fZ * (nz4 + nz1 - nz2 - nz3));
    return wave.amp * (a1 + fX * (a2 - a1) + fZ * (a3 - a1) + fX * fZ * (a4 + a1 - a2 - a3));
}

// The surface of the two waves at (x, z), within the sea's reach: its height, and its unit normal in `pNormal`.
float SEA::SurfaceXZ(const Wave (&waves)[2], float x, float z, glm::vec3 *pNormal)
{
    float nX1, nZ1, nX2, nZ2;
    float fRes = SampleWave(waves[0], x, z, nX1, nZ1);
    fRes += SampleWave(waves[1], x, z, nX2, nZ2);
    if (pNormal)
    {
        const float nY1 = sqrtf(1.0f - (Sqr(nX1) + Sqr(nZ1)));
        const float nY2 = sqrtf(1.0f - (Sqr(nX2) + Sqr(nZ2)));
        glm::vec3 vNormal;
        vNormal.x = waves[0].scale * waves[0].amp * nX1 + waves[1].scale * waves[1].amp * nX2;
        vNormal.z = waves[0].scale * waves[0].amp * nZ1 + waves[1].scale * waves[1].amp * nZ2;
        vNormal.y = nY1 + nY2;
        *pNormal = storm::math::normalizeOrZero(vNormal);
    }
    return fRes;
}

float SEA::WaveXZ(float x, float z, glm::vec3 *pNormal)
{
    const float fDistance = Sqr(x - vCamPos.x) + Sqr(z - vCamPos.z);
    if (fDistance > fMaxSeaDistance * fMaxSeaDistance)
    {
        if (pNormal)
            *pNormal = glm::vec3(0.0f, 1.0f, 0.0f);
        return 0.0f;
    }
    return SurfaceXZ(waves_, x, z, pNormal);
}

// Orders the blocks far to near (coarse LOD first, PrepareIndicesForBlock stitches each block to the coarser ones
// before it) and drops the farthest ones whose vertices do not fit the buffer. A block of size0 cells a side takes
// (size0 + 1)^2 grid points (pIndices) and writes at most as many vertices (fewer when it shares an edge).
void SEA::FitBlocksToBuffer()
{
    const auto distance2 = [this](const SeaBlock &block) {
        const float size = fGridStep * static_cast<float>(block.iSize);
        const float x1 = vSeaCenterPos.x + static_cast<float>(block.iTX) * size;
        const float z1 = vSeaCenterPos.z + static_cast<float>(block.iTY) * size;
        const float dx = std::clamp(vCamPos.x, x1, x1 + size) - vCamPos.x;
        const float dz = std::clamp(vCamPos.z, z1, z1 + size) - vCamPos.z;
        return dx * dx + dz * dz;
    };
    std::sort(aBlocks.begin(), aBlocks.end(), [&distance2](const SeaBlock &a, const SeaBlock &b) {
        return a.iLOD != b.iLOD ? a.iLOD > b.iLOD : distance2(a) > distance2(b);
    });

    int32_t points = 0;
    auto first = aBlocks.size(); // the farthest block that is kept
    while (first > 0)
    {
        const int32_t side = aBlocks[first - 1].iSize0 + 1;
        if (points + side * side > NUM_VERTEXS)
            break;
        points += side * side;
        --first;
    }
    if (first == 0)
        return;

    if (dwFramesCut++ == 0)
        spdlog::warn("sea: the {} visible blocks need more than the {} vertices of the sea's buffer; the farthest {} "
                     "are not drawn (the frames are counted when the sea goes)",
                     aBlocks.size(), NUM_VERTEXS, first);
    dwMostBlocksCut = std::max(dwMostBlocksCut, static_cast<uint32_t>(first));
    aBlocks.erase(aBlocks.begin(), aBlocks.begin() + static_cast<std::ptrdiff_t>(first));
}

void SEA::PrepareIndicesForBlock(uint32_t dwBlockIndex)
{
    SeaBlock *pB = &aBlocks[dwBlockIndex];

    const float fStep = fGridStep * static_cast<float>(1 << pB->iLOD);
    int32_t x, y, size0 = pB->iSize >> pB->iLOD;
    // FitBlocksToBuffer keeps these within the buffers; past them the 16-bit indices and the buffers would overflow.
    if (iIStart + (size0 + 1) * (size0 + 1) > NUM_VERTEXS || iTStart + 2 * size0 * size0 > NUM_INDICES)
        throw std::runtime_error(fmt::format("sea: block {} ({} cells a side) does not fit the buffers: {} of {} grid "
                                             "points, {} of {} triangles taken",
                                             dwBlockIndex, size0, iIStart, NUM_VERTEXS, iTStart, NUM_INDICES));

    pB->iIStart = iIStart;

    const float x1 = static_cast<float>(pB->iTX * pB->iSize) * fGridStep;
    const float y1 = static_cast<float>(pB->iTY * pB->iSize) * fGridStep;
    const float x2 = x1 + static_cast<float>(size0) * fStep;
    const float y2 = y1 + static_cast<float>(size0) * fStep;

    pB->iX1 = fftoi(x1 / fGridStep);
    pB->iX2 = fftoi(x2 / fGridStep);
    pB->iY1 = fftoi(y1 / fGridStep);
    pB->iY2 = fftoi(y2 / fGridStep);

    // analysis
    int32_t i, j;
    for (i = 0; i < static_cast<int32_t>(dwBlockIndex); i++) // if (i == 6)
    {
        SeaBlock *pB2 = &aBlocks[i];

        // Test Up & Down
        const bool bTestedUp = pB->iY1 == pB2->iY2;
        const bool bTestedDown = pB->iY2 == pB2->iY1;

        if (bTestedUp || bTestedDown)
        {
            const int32_t iAddSrc = pB2->iIStart + ((bTestedUp) ? (pB2->iSize0 + 1) * pB2->iSize0 : 0);
            const int32_t iAddDst = pB->iIStart + ((bTestedUp) ? 0 : (pB->iSize0 + 1) * pB->iSize0);

            if ((pB->iX1 >= pB2->iX1 && pB->iX2 <= pB2->iX2) || (pB->iX1 <= pB2->iX1 && pB->iX2 >= pB2->iX2))
            {
                const int32_t iMinX = Max(pB->iX1, pB2->iX1);
                const int32_t iMaxX = Min(pB->iX2, pB2->iX2);

                const int32_t iStartDstX = pB->iSize0 * (iMinX - pB->iX1) / (pB->iX2 - pB->iX1);
                const int32_t iStartSrcX = pB2->iSize0 * (iMinX - pB2->iX1) / (pB2->iX2 - pB2->iX1);

                const int32_t iEndDstX = pB->iSize0 * (iMaxX - pB->iX1) / (pB->iX2 - pB->iX1);
                int32_t iEndSrcX = pB2->iSize0 * (iMaxX - pB2->iX1) / (pB2->iX2 - pB2->iX1);

                if (pB->iLOD == pB2->iLOD)
                    for (j = iStartDstX; j <= iEndDstX; j++)
                        pIndices[iAddDst + j] = pIndices[iAddSrc + iStartSrcX + (j - iStartDstX)];
                else
                    for (j = iStartDstX; j <= iEndDstX; j++)
                        pIndices[iAddDst + j] = pIndices[iAddSrc + iStartSrcX + (j - iStartDstX) / 2];

                continue;
            }
        }

        // Test Left & Right
        const bool bTestedLeft = pB->iX1 == pB2->iX2;
        const bool bTestedRight = pB->iX2 == pB2->iX1;
        if (bTestedLeft || bTestedRight)
        {
            const int32_t iAddSrc = pB2->iIStart + ((bTestedLeft) ? (pB2->iSize0) : 0);
            const int32_t iAddDst = pB->iIStart + ((bTestedLeft) ? 0 : (pB->iSize0));

            if ((pB->iY1 >= pB2->iY1 && pB->iY2 <= pB2->iY2) || (pB->iY1 <= pB2->iY1 && pB->iY2 >= pB2->iY2))
            {
                const int32_t iMinY = Max(pB->iY1, pB2->iY1);
                const int32_t iMaxY = Min(pB->iY2, pB2->iY2);

                const int32_t iStartDstY = pB->iSize0 * (iMinY - pB->iY1) / (pB->iY2 - pB->iY1);
                const int32_t iStartSrcY = pB2->iSize0 * (iMinY - pB2->iY1) / (pB2->iY2 - pB2->iY1);

                const int32_t iEndDstY = pB->iSize0 * (iMaxY - pB->iY1) / (pB->iY2 - pB->iY1);
                int32_t iEndSrcY = pB2->iSize0 * (iMaxY - pB2->iY1) / (pB2->iY2 - pB2->iY1);

                if (pB->iLOD == pB2->iLOD)
                    for (j = iStartDstY; j <= iEndDstY; j++)
                        pIndices[iAddDst + j * (pB->iSize0 + 1)] =
                            pIndices[iAddSrc + (iStartSrcY + j - iStartDstY) * (pB2->iSize0 + 1)];
                else
                    for (j = iStartDstY; j <= iEndDstY; j++)
                        pIndices[iAddDst + j * (pB->iSize0 + 1)] =
                            pIndices[iAddSrc + (iStartSrcY + (j - iStartDstY) / 2) * (pB2->iSize0 + 1)];
            }
        }
    }

    int32_t iIFirst = 1000000, iILast = -1000000;

    for (y = 0; y <= size0; y++)
        for (x = 0; x <= size0; x++)
        {
            if (pIndices[iIStart] != static_cast<uint32_t>(-1))
            {
                iIStart++;
                continue;
            }

            if (iVStart < iIFirst)
                iIFirst = iVStart;
            if (iVStart > iILast)
                iILast = iVStart;

            pIndices[iIStart++] = static_cast<uint32_t>(iVStart);
            iVStart++;
        }

    pB->iIFirst = iIFirst;
    pB->iILast = iILast;

    // setup triangles
    int32_t yy, dyy = size0 + 1;
    for (y = 0, yy = 0; y < size0; y++, yy += dyy)
        for (x = 0; x < size0; x++)
        {
            // first triangle
            *pTriangles++ = static_cast<uint16_t>(pIndices[pB->iIStart + static_cast<uint32_t>(x + yy + dyy + 1)]);
            *pTriangles++ = static_cast<uint16_t>(pIndices[pB->iIStart + static_cast<uint32_t>(x + yy + 1)]);
            *pTriangles++ = static_cast<uint16_t>(pIndices[pB->iIStart + static_cast<uint32_t>(x + yy)]);
            // second triangle
            *pTriangles++ = static_cast<uint16_t>(pIndices[pB->iIStart + static_cast<uint32_t>(x + yy + dyy)]);
            *pTriangles++ = static_cast<uint16_t>(pIndices[pB->iIStart + static_cast<uint32_t>(x + yy + dyy + 1)]);
            *pTriangles++ = static_cast<uint16_t>(pIndices[pB->iIStart + static_cast<uint32_t>(x + yy)]);

            iTStart += 2;
        }
}

void SEA::SSE_WaveXZBlock(SeaBlock &pB)
{
    SeaVertex *vTmp[4];
    SeaVertex vFake;

    vFake.vPos.x = vCamPos.x + 1e+5f;
    vFake.vPos.z = vCamPos.z + 1e+5f;

    float cx, cz, fStep = fGridStep * static_cast<float>(1 << pB.iLOD);
    float fSize = fGridStep * pB.iSize;
    int32_t x, y, size0 = pB.iSize >> pB.iLOD;

    float x1 = static_cast<float>(pB.iTX * pB.iSize) * fGridStep;
    float y1 = static_cast<float>(pB.iTY * pB.iSize) * fGridStep;
    float x2 = x1 + static_cast<float>(size0) * fStep;
    float y2 = y1 + static_cast<float>(size0) * fStep;

    pB.iX1 = fftoi(x1 / fGridStep);
    pB.iX2 = fftoi(x2 / fGridStep);
    pB.iY1 = fftoi(y1 / fGridStep);
    pB.iY2 = fftoi(y2 / fGridStep);

    x1 += vSeaCenterPos.x;
    x2 += vSeaCenterPos.x;
    y1 += vSeaCenterPos.z;
    y2 += vSeaCenterPos.z;

    int32_t iCurrentV = 0;

    int32_t iIStart1 = pB.iIStart;
    const int32_t iIFirst = pB.iIFirst;
    const int32_t iILast = pB.iILast;

    // calculate
    for (cz = y1, y = 0; y <= size0; y++, cz += fStep)
    {
        for (cx = x1, x = 0; x <= size0; x++, cx += fStep)
        {
            const uint32_t dwVIndex = pIndices[iIStart1];

            if (static_cast<int32_t>(dwVIndex) < iIFirst || static_cast<int32_t>(dwVIndex) > iILast)
            {
                iIStart1++;
                continue;
            }

            vTmp[iCurrentV] = &pVSea[dwVIndex];
            vTmp[iCurrentV]->vPos.x = cx;
            vTmp[iCurrentV]->vPos.z = cz;

            iCurrentV++;

            if (iCurrentV == 4)
            {
                SSE_WaveXZ(&vTmp[0]);
                iCurrentV = 0;

                for (int32_t i = 0; i < 4; i++)
                {
                    vTmp[i]->tu = (vTmp[i]->vPos.x - vTmp[i]->vNormal.x * 5.3f) * fBumpScale;
                    vTmp[i]->tv = (vTmp[i]->vPos.z - vTmp[i]->vNormal.z * 5.3f) * fBumpScale;
                    vTmp[i]->vPos.x -= vTmp[i]->vNormal.x * fPosShift * 3.0f + vWorldOffset.x;
                    vTmp[i]->vPos.z -= vTmp[i]->vNormal.z * fPosShift * 3.0f + vWorldOffset.z;
                }
            }


            iIStart1++;
        }
    }

    if (iCurrentV != 0)
    {
        for (int32_t i = iCurrentV; i < 4; i++)
            vTmp[i] = &vFake;

        SSE_WaveXZ(&vTmp[0]);
        for (int32_t i = 0; i < iCurrentV; i++)
        {
            vTmp[i]->tu = (vTmp[i]->vPos.x - vTmp[i]->vNormal.x * 5.3f) * fBumpScale;
            vTmp[i]->tv = (vTmp[i]->vPos.z - vTmp[i]->vNormal.z * 5.3f) * fBumpScale;

            vTmp[i]->vPos.x -= vTmp[i]->vNormal.x * fPosShift * 3.0f + vWorldOffset.x;
            vTmp[i]->vPos.z -= vTmp[i]->vNormal.z * fPosShift * 3.0f + vWorldOffset.z;
        }
    }

}

// This frame's waves (fFrame*, fScale*, fAmp*, vMove*) for WaveXZ and the CPU surface.
void SEA::UpdateWaves()
{
    waves_[0] = {WaveBlend(fFrame1), fScale1, fAmp1, vMove1};
    waves_[1] = {WaveBlend(fFrame2), fScale2, fAmp2, vMove2};
}

// The grid points of the block this block owns (PrepareIndicesForBlock: [iIFirst, iILast]; the points on an edge
// shared with a block before it are that block's), metres from MeshOrigin, flat: the vertex shader displaces them.
void SEA::WriteBlockVertices(const SeaBlock &block, SeaVertex *pVertices) const
{
    const auto half = static_cast<int32_t>(TreeDim() / 2);
    const int32_t step = 1 << block.iLOD, size0 = block.iSize >> block.iLOD;
    const int32_t x0 = block.iTX * block.iSize - half, z0 = block.iTY * block.iSize - half;
    int32_t point = block.iIStart;
    for (int32_t y = 0; y <= size0; y++)
        for (int32_t x = 0; x <= size0; x++)
        {
            const auto vertex = static_cast<int32_t>(pIndices[point++]);
            if (vertex < block.iIFirst || vertex > block.iILast)
                continue;
            pVertices[vertex] = {glm::vec3(static_cast<float>(x0 + x * step) * fGridStep, 0.0f,
                                           static_cast<float>(z0 + y * step) * fGridStep),
                                 glm::vec3(0.0f), 0.0f, 0.0f};
        }
}

// The mesh's origin in the world (x, z): the middle of the block tree, so the grid's numbers stay within a quarter of
// the tree around the camera (PlaceTree).
glm::dvec2 SEA::MeshOrigin() const
{
    const double half = static_cast<double>(TreeDim() / 2) * fGridStep;
    return {vSeaCenterPos.x + half, vSeaCenterPos.z + half};
}

// GPU surface: the flat grid of this frame's blocks in the buffers, written only when the blocks, the tree's place or
// the grid step changed since the grid there was built; the vertex shader displaces it every frame (sea_surface.sh).
// True when it was written.
bool SEA::BuildMesh()
{
    bool same = bMeshBuilt && vMeshTree == vSeaCenterPos && fMeshGridStep == fGridStep &&
                aMeshBlocks.size() == aBlocks.size() * 4;
    for (size_t i = 0; same && i < aBlocks.size(); i++)
    {
        const SeaBlock &b = aBlocks[i];
        const int32_t *key = &aMeshBlocks[i * 4];
        same = key[0] == b.iTX && key[1] == b.iTY && key[2] == b.iSize && key[3] == b.iLOD;
    }
    if (same)
        return false;
    aMeshBlocks.clear();
    for (const SeaBlock &b : aBlocks)
        aMeshBlocks.insert(aMeshBlocks.end(), {b.iTX, b.iTY, b.iSize, b.iLOD});
    vMeshTree = vSeaCenterPos;
    fMeshGridStep = fGridStep;
    bMeshBuilt = true;

    iVStart = 0;
    iTStart = 0;
    iIStart = 0;
    memset(pIndices, 0xFF, NUM_VERTEXS * sizeof(pIndices[0]) * 3);
    auto *pVertices = static_cast<SeaVertex *>(rs->LockVertexBuffer(iVSeaBuffer, D3DLOCK_DISCARD | D3DLOCK_NOSYSLOCK));
    pTriangles = static_cast<uint16_t *>(rs->LockIndexBuffer(iISeaBuffer, D3DLOCK_DISCARD | D3DLOCK_NOSYSLOCK));
    if (!pVertices || !pTriangles)
        throw std::runtime_error("SEA: cannot lock the sea's vertex or index buffer");
    for (uint32_t i = 0; i < aBlocks.size(); i++)
        PrepareIndicesForBlock(i);
    for (const SeaBlock &b : aBlocks)
        WriteBlockVertices(b, pVertices);
    rs->UnLockVertexBuffer(iVSeaBuffer);
    rs->UnLockIndexBuffer(iISeaBuffer);
    return true;
}

// CPU surface (until the GPU surface self-check passed, or for good when it failed): every vertex of this frame's
// blocks displaced here (SSE_WaveXZBlock) and the buffers written again.
void SEA::BuildCpuMesh()
{
    bMeshBuilt = false; // the GPU surface builds its grid again
    iVStart = 0;
    iTStart = 0;
    iIStart = 0;
    memset(pIndices, 0xFF, NUM_VERTEXS * sizeof(pIndices[0]) * 3);

    auto *pVSea2 = static_cast<SeaVertex *>(rs->LockVertexBuffer(iVSeaBuffer, D3DLOCK_DISCARD | D3DLOCK_NOSYSLOCK));
    pTriangles = static_cast<uint16_t *>(rs->LockIndexBuffer(iISeaBuffer, D3DLOCK_DISCARD | D3DLOCK_NOSYSLOCK));

    for (uint32_t i = 0; i < aBlocks.size(); i++)
        PrepareIndicesForBlock(i);

    // Blocks write disjoint vertex ranges ([iIFirst, iILast], assigned above by PrepareIndicesForBlock) and only read
    // per-frame constants and the waves' frames, so they can be evaluated concurrently. The vertex buffer upload
    // below stays on this thread.
#ifdef __ANDROID__
    // libc++ in the NDK has no parallel algorithms; use the engine's fork-join helper.
    waveWorkers_.run(aBlocks.size(), [this](size_t index) { SSE_WaveXZBlock(aBlocks[index]); });
#else
    std::for_each(std::execution::par_unseq, std::begin(aBlocks), std::end(aBlocks),
                  [this](auto &i) { SSE_WaveXZBlock(i); });
#endif

    if (iVStart && iTStart)
        memcpy(pVSea2, pVSea, iVStart * sizeof(SeaVertex));

    rs->UnLockVertexBuffer(iVSeaBuffer);
    rs->UnLockIndexBuffer(iISeaBuffer);
}

// The height field of the vertex shader (sea_gpu::pack_field): the frames of aBumps and aNormals, once per sea.
void SEA::BuildFieldTexture()
{
    iFieldTexture = rs->TextureCreate(sea_gpu::kAtlasSize, sea_gpu::kAtlasSize, 1, 0, D3DFMT_A32B32G32R32F,
                                      D3DPOOL_MANAGED);
    D3DLOCKED_RECT locked{};
    if (iFieldTexture < 0 || !rs->TextureLock(iFieldTexture, 0, locked, 0))
        throw std::runtime_error("SEA: cannot create the sea's height field texture");
    const std::vector<float> texels = sea_gpu::pack_field(aBumps.data(), aNormals.data());
    constexpr size_t kRowBytes = sea_gpu::kAtlasSize * 4 * sizeof(float);
    for (int32_t y = 0; y < sea_gpu::kAtlasSize; y++)
        memcpy(static_cast<uint8_t *>(locked.pBits) + static_cast<size_t>(y) * locked.Pitch,
               texels.data() + static_cast<size_t>(y) * sea_gpu::kAtlasSize * 4, kRowBytes);
    rs->TextureUnlock(iFieldTexture, 0);
}

void SEA::SurfaceCheckFailed(const std::string &why)
{
    surface_ = Surface::Cpu;
    spdlog::error("sea: the GPU surface self-check failed on {}: {}. The sea's vertices stay displaced on the CPU "
                  "(more game-thread time a frame) instead of a wrong surface",
                  storm::render::SeaSurfaceCheckDevice(), why);
}

// The GPU surface self-check (sea_surface_check.hpp): the vertex shader's surface for points of this sea's field,
// read back and compared with SurfaceXZ (WaveXZ's formula), before the sea's vertices are displaced on the GPU.
void SEA::UpdateSurfaceCheck()
{
    if (surface_ != Surface::Checking)
        return;
    // The check's own surface over this sea's field, whatever the weather: two waves of fixed amplitudes (a field read
    // as zeros or garbage cannot match), the second across the frames' wrap (63 -> 0); a far origin, moves.
    constexpr double kOriginX = 1234.5, kOriginZ = -678.25;
    const Wave waves[2] = {{WaveBlend(13.37f), 0.35f, 6.0f, glm::vec3(37.5f, 0.0f, -12.25f)},
                           {WaveBlend(63.6f), 1.3f, 1.5f, glm::vec3(-8.0f, 0.0f, 21.75f)}};
    constexpr uint32_t kPoints = storm::render::kSeaSurfaceCheckMaxPoints;
    float points[kPoints][2];
    for (uint32_t i = 0; i < kPoints; i++)
    {
        // A spiral out to 90 m: every texel phase, both waves
        const float angle = static_cast<float>(i) * 2.39996f, radius = 1.37f * static_cast<float>(i) + 0.21f;
        points[i][0] = radius * std::cos(angle);
        points[i][1] = radius * std::sin(angle);
    }
    if (surfaceCheck_ == 0)
    {
        float registers[sea_gpu::kRegisters][4]{};
        for (int w = 0; w < 2; w++)
            sea_gpu::wave_register(kOriginX, kOriginZ, waves[w].move.x, waves[w].move.z, waves[w].scale, waves[w].amp,
                                   registers[w]);
        registers[2][0] = 13.37f;
        registers[2][1] = 63.6f;
        registers[4][2] = 1.0e8f; // every point within reach (camera at the origin)
        std::string why;
        bool retry = false;
        surfaceCheck_ = storm::render::StartSeaSurfaceCheck(rs->GetTextureFromID(iFieldTexture), &registers[0][0],
                                                            &points[0][0], kPoints, why, retry);
        surfaceCheckFrames_ = 0;
        if (!surfaceCheck_ && !retry)
            SurfaceCheckFailed("it cannot run: " + why);
        return;
    }
    std::vector<storm::render::SeaSurfaceSample> samples;
    switch (storm::render::PollSeaSurfaceCheck(surfaceCheck_, samples))
    {
    case storm::render::SeaSurfaceCheckState::Pending:
        if (++surfaceCheckFrames_ > kSurfaceCheckFrames)
            SurfaceCheckFailed(fmt::format("no result after {} frames", kSurfaceCheckFrames));
        return;
    case storm::render::SeaSurfaceCheckState::Lost:
        surfaceCheck_ = 0;
        if (++surfaceCheckLosses_ > kSurfaceCheckLosses)
            SurfaceCheckFailed(fmt::format("its result was lost {} times", surfaceCheckLosses_));
        return;
    case storm::render::SeaSurfaceCheckState::Done:
        break;
    }
    surfaceCheck_ = 0;
    // Within float rounding of the two (world positions on the CPU, wrapped texel coordinates on the GPU) and a half
    // float target's precision: a millimetre in a surface metres high, normals to 1/500.
    float worstHeight = 0.0f, worstNormal = 0.0f;
    uint32_t wrong = 0;
    std::string example;
    for (uint32_t i = 0; i < kPoints; i++)
    {
        glm::vec3 normal;
        const auto x = static_cast<float>(kOriginX + points[i][0]), z = static_cast<float>(kOriginZ + points[i][1]);
        const float height = SurfaceXZ(waves, x, z, &normal);
        const storm::render::SeaSurfaceSample &gpu = samples[i];
        const float dh = std::fabs(gpu.height - height);
        const float dn = std::max({std::fabs(gpu.nx - normal.x), std::fabs(gpu.ny - normal.y), std::fabs(gpu.nz - normal.z)});
        const bool bad = !(dh <= 0.004f + 0.002f * std::fabs(height)) || !(dn <= 0.002f);
        worstHeight = std::max(worstHeight, std::isfinite(dh) ? dh : INFINITY);
        worstNormal = std::max(worstNormal, std::isfinite(dn) ? dn : INFINITY);
        if (bad && wrong++ == 0)
            example = fmt::format("point {} ({:.2f}, {:.2f}): GPU height {:.4f} normal ({:.4f}, {:.4f}, {:.4f}), CPU "
                                  "height {:.4f} normal ({:.4f}, {:.4f}, {:.4f})",
                                  i, points[i][0], points[i][1], gpu.height, gpu.nx, gpu.ny, gpu.nz, height, normal.x,
                                  normal.y, normal.z);
    }
    if (wrong)
    {
        SurfaceCheckFailed(fmt::format("{} of {} points differ from the CPU's surface (largest difference: height {:.4f} "
                                       "m, normal {:.4f}); first {}",
                                       wrong, kPoints, worstHeight, worstNormal, example));
        return;
    }
    surface_ = Surface::Gpu;
    spdlog::info("sea: the GPU surface self-check passed on {}: {} points match the CPU's surface (largest difference: "
                 "height {:.5f} m, normal {:.5f}); the sea's vertices are displaced on the GPU",
                 storm::render::SeaSurfaceCheckDevice(), kPoints, worstHeight, worstNormal);
}

void SEA::Realize(uint32_t dwDeltaTime)
{
    static float fTmp = 0.0f;

    if (!storm::loading::active && core.Controls->GetDebugAsyncKeyState(VK_SHIFT) < 0 && core.Controls->GetDebugAsyncKeyState('S') < 0)
    {
        if (bTempFullMode)
        {
            bTempFullMode = false;
            fGridStep = fTempGridStep;
            fLodScale = fTempLodScale;
        }
        else
        {
            bTempFullMode = true;
            fTempGridStep = fGridStep;
            fTempLodScale = fLodScale;

            fGridStep = 0.07f;
            fLodScale = 0.5f;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    if (bStop)
        return;

    float fDeltaTime = static_cast<float>(dwDeltaTime) * 0.001f;

    vMove1 += fDeltaTime * vMoveSpeed1;
    vMove2 += fDeltaTime * vMoveSpeed2;

    fFrame1 += fDeltaTime * fAnimSpeed1;
    fFrame2 += fDeltaTime * fAnimSpeed2;
    while (fFrame1 >= FRAMES)
        fFrame1 -= FRAMES;
    while (fFrame2 >= FRAMES)
        fFrame2 -= FRAMES;
    UpdateWaves();

    uint64_t dwTotalRDTSC;
    RDTSC_B(dwTotalRDTSC);

    glm::mat4 mView, mIView;
    rs->GetTransform(D3DTS_VIEW, storm::math::d3dMatrix(mView));
    mIView = mView;
    storm::math::invertRigid(mIView);

    vCamPos = storm::math::pos(mIView);
#ifdef OLD_WORLD_POS
    vWorldOffset = 0.0f;
#else
    vWorldOffset = vCamPos;
    vWorldOffset.y = 0.f;
#endif

    bUnderSea = false;

    if (vCamPos.y < 0.0f)
        bUnderSea = true;

    if (bStarted && vCamPos.y < WaveXZ(vCamPos.x, vCamPos.z))
        bUnderSea = true;

    if (!storm::loading::active && !bUnderSea)
        bUnderSeaStarted = false;

    cubeFaces_ = CubeFacesDue(); // EnvMap_Render / SunRoad_Render draw only these faces this frame
    if (bSimpleSea)
    {
        EnvMap_Render2();
        SunRoad_Render2();
    }
    else
    {
        EnvMap_Render();
        SunRoad_Render();
    }

    pFrustumPlanes = rs->GetPlanes();

    PlaceTree();

    aBlocks.clear();
    BuildTree(0, 0, 0);
    FitBlocksToBuffer();
    cubeSeaInView_ = !aBlocks.empty(); // the next frame's cube faces (CubeFacesDue)

    UpdateSurfaceCheck();
    const bool bGpuSurface = surface_ == Surface::Gpu;
    if (bGpuSurface)
        BuildMesh();
    else
        BuildCpuMesh();

    if (iVStart && iTStart)
    {
        // Waves from objects (renderer: fx::SeaWaves, fx::SeaWakes): this frame's sources and the sea's vertex origin.
        storm::render::EffectsSeaFrame waves;
        waves.vertex_origin = {vWorldOffset.x, vWorldOffset.y, vWorldOffset.z};
        waves.camera = {vCamPos.x, vCamPos.y, vCamPos.z};
        {
            // Where the view ray meets the water, at most 45 m out (row-vector view: its third column is the look).
            const glm::vec3 look = storm::math::normalizeOrZero(glm::vec3(mIView[2]));
            float reach = 45.0f;
            if (look.y < -0.01f)
                reach = std::min(reach, std::max(vCamPos.y, 0.0f) / -look.y);
            const glm::vec3 flat = storm::math::normalizeOrZero(glm::vec3(look.x, 0.0f, look.z)) *
                                   (reach * std::sqrt(std::max(0.0f, 1.0f - look.y * look.y)));
            waves.focus = waveFocus_.Update(waves.camera, {look.x, look.y, look.z},
                {vCamPos.x + flat.x, 0.0f, vCamPos.z + flat.z}, waveSources_, fDeltaTime);
        }
        waves.dt = fDeltaTime;
        waves.fresnel = fFrenel;
        waves.sources = waveSources_;
        storm::render::SetEffectsSeaFrame(waves);

        glm::mat4 mWorldView, mWorldViewProj;

        glm::mat4 mView = rs->GetView();
        glm::mat4 mWorld;
        mWorld = glm::mat4(1.0f); // = rs->GetWorld();
        glm::mat4 mProjection = rs->GetProjection();

#ifndef OLD_WORLD_POS
        storm::math::moveInversePosition(mView, -vWorldOffset.x, 0.f, -vWorldOffset.z);
#endif

        mWorldView = mView * mWorld;
        mWorldViewProj = mProjection * mWorldView;

        mWorldViewProj = glm::transpose(mWorldViewProj);

        fTmp += fDeltaTime * fBumpSpeed;
        while (fTmp >= 1.0f)
            fTmp -= 1.0f;

        rs->SetVertexDeclaration(vertexDecl_);

        const auto vec1 = glm::vec4(0.0f, 1.0f, 0.5f, -0.04f);
        const auto vec2 = glm::vec4(2.0f, -1.0f, 0.00036621652552071f, (bFogEnable) ? fFogSeaDensity : 0.0f);
        const auto vec3 = glm::vec4(fFoamV, fFoamK, fFoamUV, 6.0f);
        const auto vec4 = glm::vec4(fTmp, fTmp, fTmp, fTmp);
        const auto vec5 = glm::vec4(vCamPos.x - vWorldOffset.x, vCamPos.y, vCamPos.z - vWorldOffset.z, 1.0f);
        const auto vec6 = glm::vec4(1.0f, 0.0f, 0.0f, 1.0f);
        const auto vec7 = glm::vec4(fFrenel, 1.0f, 0.5f, 1.0f);
        const auto vec8 = glm::vec4(1.0f, 0.0f, 0.0f, 1.0f);
        glm::vec3 vTmp = storm::math::normalizeOrZero(glm::vec3(0.0f, 1.0f, 0.0f));
        const auto vec9 = glm::vec4(vTmp.x, vTmp.y, vTmp.z, 1.0f);
        rs->SetVertexShaderConstantF(GC_CONSTANT, (const float *)&vec1, 1);
        rs->SetVertexShaderConstantF(GC_CONSTANT2, (const float *)&vec2, 1);
        rs->SetVertexShaderConstantF(GC_SHADOW_CONST1, (const float *)&vec3, 1);
        rs->SetVertexShaderConstantF(GC_ANIMATION, (const float *)&vec4, 1);
        {
            // c8.w: whether it does (the CPU surface wrote the vertices displaced). The field is bound either way (the
            // ports declare it).
            const glm::dvec2 origin = MeshOrigin();
            float surface[sea_gpu::kRegisters][4]{};
            sea_gpu::wave_register(origin.x, origin.y, vMove1.x, vMove1.z, fScale1, fAmp1, surface[0]);
            sea_gpu::wave_register(origin.x, origin.y, vMove2.x, vMove2.z, fScale2, fAmp2, surface[1]);
            surface[2][0] = fFrame1;
            surface[2][1] = fFrame2;
            surface[2][2] = fPosShift * 3.0f;
            surface[2][3] = fBumpScale;
            surface[3][0] = static_cast<float>(origin.x - vWorldOffset.x);
            surface[3][1] = static_cast<float>(origin.y - vWorldOffset.z);
            surface[3][2] = static_cast<float>(origin.x * fBumpScale);
            surface[3][3] = static_cast<float>(origin.y * fBumpScale);
            surface[4][0] = static_cast<float>(vCamPos.x - origin.x);
            surface[4][1] = static_cast<float>(vCamPos.z - origin.y);
            surface[4][2] = fMaxSeaDistance * fMaxSeaDistance;
            surface[4][3] = bGpuSurface ? sea_gpu::kSurfaceGpu : sea_gpu::kSurfaceCpu;
            rs->SetVertexShaderConstantF(sea_gpu::kFirstRegister, &surface[0][0], sea_gpu::kRegisters);
            rs->TextureSet(D3DVERTEXTEXTURESAMPLER0, iFieldTexture);
        }
        rs->SetVertexShaderConstantF(GC_CAMERA_POS, (const float *)&vec5, 1);
        rs->SetVertexShaderConstantF(GC_MTX_WVP, (const float *)&mWorldViewProj, 4);

        rs->SetVertexShaderConstantF(GC_FREE, (const float *)&v4SeaParameters, 1);
        rs->SetVertexShaderConstantF(GC_FREE + 1, (const float *)&v4SeaColor, 1);
        rs->SetVertexShaderConstantF(GC_FREE + 2, (const float *)&v4SkyColor, 1);

        rs->SetVertexShaderConstantF(GC_FREE + 5, (const float *)&vec6, 1);
        rs->SetVertexShaderConstantF(GC_FREE + 6, (const float *)&vec7, 1);
        // Frenel K, Frenel Max
        rs->SetVertexShaderConstantF(GC_FREE + 7, (const float *)&vec8, 1);
        rs->SetVertexShaderConstantF(GC_FREE + 30, (const float *)&vec9, 1);

        if (bSimpleSea)
        {
            rs->SetVertexShaderConstantF(GC_FREE + 8, (const float *)&mTexProjection, 4); // Matrix!!

            rs->TextureSet(0, volumeTexture);
            rs->TextureSet(1, reflection);
            rs->TextureSet(2, volumeTexture);
            rs->TextureSet(3, reflectionSunroad);

            rs->SetTextureStageState(1, D3DTSS_BUMPENVMAT00, F2DW(0.08f));
            rs->SetTextureStageState(1, D3DTSS_BUMPENVMAT10, F2DW(0.0f));
            rs->SetTextureStageState(1, D3DTSS_BUMPENVMAT01, F2DW(0.0f));
            rs->SetTextureStageState(1, D3DTSS_BUMPENVMAT11, F2DW(0.08f));

            rs->SetTextureStageState(3, D3DTSS_BUMPENVMAT00, F2DW(0.05f));
            rs->SetTextureStageState(3, D3DTSS_BUMPENVMAT10, F2DW(0.0f));
            rs->SetTextureStageState(3, D3DTSS_BUMPENVMAT01, F2DW(0.0f));
            rs->SetTextureStageState(3, D3DTSS_BUMPENVMAT11, F2DW(0.05f));

            rs->DrawIndexedPrimitiveNoVShader(D3DPT_TRIANGLELIST, iVSeaBuffer, sizeof(SeaVertex), iISeaBuffer, 0,
                                              iVStart, 0, iTStart, "Sea3");
        }
        else
        {
            const auto vec1 = storm::math::eulerMatrix(0.0f, 0.0f, PId2);
            rs->SetVertexShaderConstantF(GC_FREE + 8, (const float *)&vec1, 4); // Matrix!!

            rs->TextureSet(0, volumeTexture);
            // The foam texture for the waves from objects (the renderer's Sea2 pixel shader reads it only on the
            // ripples' crests; the original shader leaves stage 1 unread).
            rs->TextureSet(1, iFoamTexture);
            rs->TextureSet(3, envMap);
            // The sun road's cube: the renderer's Sea2 pixel shader takes the colour of the sun's glint from the sun
            // as the game draws it there (the original shader leaves stage 2 unread).
            rs->TextureSet(2, sunRoadMap);
            // The technique's foam and sun road passes are drawn by this one (fs_port_sea2.sc folds them in); c0.x:
            // whether the foam's is.
            const bool foam = fFoamK > 0.0f && bFoamEnable && bIniFoamEnable;
            const auto layers = glm::vec4(foam ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f);
            rs->SetPixelShaderConstantF(0, (const float *)&layers, 1);
            rs->DrawIndexedPrimitiveNoVShader(D3DPT_TRIANGLELIST, iVSeaBuffer, sizeof(SeaVertex), iISeaBuffer, 0,
                                              iVStart, 0, iTStart, "Sea2");
            rs->TextureSet(1, -1);
            rs->TextureSet(2, -1);
        }
        rs->TextureSet(D3DVERTEXTEXTURESAMPLER0, -1); // a skinned draw's bone palette goes there
    }

    if (!storm::loading::active)
        waveSources_.clear(); // the Execute stage of the next frame adds them again
    RDTSC_E(dwTotalRDTSC);


    if (bUnderSea && bUnderSeaEnable)
    {
        glm::vec3 d(20.0f, 5.0f, 20.0f);
        glm::vec3 d2(54.0f, 10.0f, 54.0f);
        if (!storm::loading::active && !bUnderSeaStarted)
        {
            // doing a bunch of sea trash for the first time
            aSeaTrash.clear();
            for (int32_t i = 0; i < 1000; i++)
            {
                aSeaTrash.push_back(SeaTrash{});
                SeaTrash &st = aSeaTrash.back();
                st.vPos = vCamPos + glm::vec3(RRnd(-d.x, d.x), RRnd(-d.y, d.y), RRnd(-d.z, d.z));
                st.vSpeed = glm::vec3(0.1f, 0.0f, 0.0f);
                st.dwSubTexture = rand() % 4;
                st.fSize = RRnd(0.01f, 0.05f);
                st.fTime = static_cast<float>(i) * 0.02f;
            }

            // making a bunch of light poles for the first time
            aSeaLights.clear();
            for (int32_t i = 0; i < 50; i++)
            {
                aSeaLights.push_back(SeaLight{});
                SeaLight &sl = aSeaLights.back();
                sl.vPos = vCamPos + glm::vec3(RRnd(-d2.x, d2.x), RRnd(-d2.y, d2.y), RRnd(-d2.z, d2.z));
                sl.vSpeed = glm::vec3(0.1f, 0.0f, 0.0f);
                sl.dwSubTexture = rand() % 4;
                sl.fTime = static_cast<float>(i);
            }
        }

        // add 1 item of sea trash every second
        fLastTrashTime += fDeltaTime;
        while (!storm::loading::active && fLastTrashTime >= 0.03f)
        {
            fLastTrashTime -= 0.03f;

            aSeaTrash.push_back(SeaTrash{});
            SeaTrash &st = aSeaTrash.back();
            st.vPos = vCamPos + glm::vec3(RRnd(-d.x, d.x), RRnd(-d.y, d.y), RRnd(-d.z, d.z));
            st.vSpeed = glm::vec3(0.1f, 0.0f, 0.0f);
            st.dwSubTexture = rand() % 4;
            st.fTime = 20.0f;
            st.fSize = RRnd(0.01f, 0.05f);
        }

        // Remove the old sea trash
        for (int32_t i = 0; i < aSeaTrash.size(); i++)
        {
            aSeaTrash[i].fTime -= fDeltaTime;
            if (!storm::loading::active && aSeaTrash[i].fTime <= 0.0f)
            {
                aSeaTrash[i] = aSeaTrash.back();
                aSeaTrash.pop_back();
                i--;
                continue;
            }
            aSeaTrash[i].vPos += aSeaTrash[i].vSpeed * fDeltaTime;
        }
        // Render sea trash
        aTrashRects.clear();
        for (int32_t i = 0; i < aSeaTrash.size(); i++)
        {
            aTrashRects.push_back(RS_RECT{});
            RS_RECT &r = aTrashRects.back();

            float fAlpha = 1.0f;
            if (aSeaTrash[i].fTime >= 45.0f)
                fAlpha = 1.0f - (aSeaTrash[i].fTime - 45.0f) / 5.0f;
            if (aSeaTrash[i].fTime <= 5.0f)
                fAlpha = 1.0f - (5.0f - aSeaTrash[i].fTime) / 5.0f;

            r.vPos = aSeaTrash[i].vPos;
            r.fSize = aSeaTrash[i].fSize;
            r.fAngle = 0.0f;
            r.dwColor = ARGB(fAlpha * 255.0f, 255, 255, 255);
            r.dwSubTexture = aSeaTrash[i].dwSubTexture;
        }

        if (aTrashRects.size())
        {
            rs->TextureSet(0, iSeaTrashTexture);
            rs->DrawRects(&aTrashRects[0], aTrashRects.size(), "seatrash", 2, 2);
        }

        // Render schools of fish

        // draw light poles
        // add 1 light pole every second
        fLastLightTime += fDeltaTime;
        while (!storm::loading::active && fLastLightTime >= 1.0f)
        {
            fLastLightTime -= 1.0f;

            aSeaLights.push_back(SeaLight{});
            SeaLight &sl = aSeaLights.back();
            sl.vPos = vCamPos + glm::vec3(RRnd(-d2.x, d2.x), RRnd(-d2.y, d2.y), RRnd(-d2.z, d2.z));
            sl.vSpeed = glm::vec3(0.1f, 0.0f, 0.0f);
            sl.dwSubTexture = rand() % 4;
            sl.fTime = 50.0f;
        }

        // Remove the old light poles
        for (int32_t i = 0; i < aSeaLights.size(); i++)
        {
            aSeaLights[i].fTime -= fDeltaTime;
            if (!storm::loading::active && aSeaLights[i].fTime <= 0.0f)
            {
                aSeaLights[i] = aSeaLights.back();
                aSeaLights.pop_back();
                i--;
                continue;
            }
            aSeaLights[i].vPos += aSeaLights[i].vSpeed * fDeltaTime;
        }

        float fAlphaK = (vCamPos.y < 0.0f) ? Max(0.0f, 1.0f + vCamPos.y / 30.0f) : 1.0f;

        // draw light poles
        aLightsRects.clear();
        for (int32_t i = 0; i < aSeaLights.size(); i++)
        {
            aLightsRects.push_back(RS_RECT{});
            RS_RECT &r = aLightsRects.back();
            float fAlpha = 1.0f;
            if (aSeaLights[i].fTime >= 45.0f)
                fAlpha = 1.0f - (aSeaLights[i].fTime - 45.0f) / 5.0f;
            if (aSeaLights[i].fTime <= 5.0f)
                fAlpha = 1.0f - (5.0f - aSeaLights[i].fTime) / 5.0f;

            glm::vec3 v = aSeaLights[i].vPos;
            r.vPos = glm::vec3(v.x, WaveXZ(v.x, v.z) - 5.0f, v.z);
            r.fSize = 20.0f;
            r.fAngle = 0.0f;
            r.dwColor = ARGB(0.07f * fAlphaK * fAlpha * 255.0f, 227, 245, 153);
            r.dwSubTexture = aSeaLights[i].dwSubTexture;
        }

        if (aLightsRects.size())
        {
            rs->TextureSet(0, iSeaLightTexture);
            rs->DrawRects(&aLightsRects[0], aLightsRects.size(), "seatrash", 2, 2, 0.5f);
        }

        if (!storm::loading::active) bUnderSeaStarted = true;
    }

    bStarted = true;
}

float SEA::Trace(const glm::vec3 &vSrc, const glm::vec3 &vDst)
{
    const int32_t iNumTests = 5;
    float fRes = 2.0f;
    const float fDV = 1.0f / static_cast<float>(iNumTests - 1);

    if (vSrc.y > fMaxSeaHeight && vDst.y > fMaxSeaHeight)
        return 2.0f;

    for (int32_t i = 0; i < iNumTests; i++)
    {
        const glm::vec3 vTemp = vSrc + static_cast<float>(i) * fDV * (vDst - vSrc);
        const float fWaveY = WaveXZ(vTemp.x, vTemp.z, nullptr);

        if (fWaveY > vTemp.y)
            return static_cast<float>(i) * fDV;
    }

    return 2.0f;
}

float SEA::Cannon_Trace(int32_t iBallOwner, const glm::vec3 &vSrc, const glm::vec3 &vDst)
{
    const float fRes = Trace(vSrc, vDst);

    if (fRes <= 1.0f)
    {
        const glm::vec3 vTemp = vSrc + fRes * (vDst - vSrc);
        const float fTmpY = WaveXZ(vTemp.x, vTemp.z, nullptr);
        core.Event(BALL_WATER_HIT, "lfff", iBallOwner, vTemp.x, fTmpY, vTemp.z);
    }

    return fRes;
}

uint32_t SEA::AttributeChanged(ATTRIBUTES *pAttribute)
{
    ATTRIBUTES *pParent = pAttribute->GetParent();
    ATTRIBUTES *pParent2 = (pParent) ? pParent->GetParent() : nullptr;

    if (*pParent == "isDone")
    {
        Realize(0);
    }

    if (*pParent == "Sea2")
    {
        if (*pAttribute == "WaterColor")
        {
            v4SeaColor = storm::math::colorFromARGB(pAttribute->GetAttributeAsDword());
            return 0;
        }
        if (*pAttribute == "SkyColor")
        {
            v4SkyColor = storm::math::colorFromARGB(pAttribute->GetAttributeAsDword());
            return 0;
        }

        if (*pAttribute == "Amp1")
        {
            _fAmp1 = pAttribute->GetAttributeAsFloat();
            return 0;
        }
        if (*pAttribute == "AnimSpeed1")
        {
            fAnimSpeed1 = pAttribute->GetAttributeAsFloat();
            return 0;
        }
        if (*pAttribute == "Scale1")
        {
            fScale1 = pAttribute->GetAttributeAsFloat();
            return 0;
        }
        if (*pAttribute == "MoveSpeed1")
        {
            sscanf(pAttribute->GetThisAttr(), "%f, %f, %f", &vMoveSpeed1.x, &vMoveSpeed1.y, &vMoveSpeed1.z);
            return 0;
        }

        if (*pAttribute == "Amp2")
        {
            _fAmp2 = pAttribute->GetAttributeAsFloat();
            return 0;
        }
        if (*pAttribute == "AnimSpeed2")
        {
            fAnimSpeed2 = pAttribute->GetAttributeAsFloat();
            return 0;
        }
        if (*pAttribute == "Scale2")
        {
            fScale2 = pAttribute->GetAttributeAsFloat();
            return 0;
        }
        if (*pAttribute == "MoveSpeed2")
        {
            sscanf(pAttribute->GetThisAttr(), "%f, %f, %f", &vMoveSpeed2.x, &vMoveSpeed2.y, &vMoveSpeed2.z);
            return 0;
        }

        if (*pAttribute == "PosShift")
        {
            fPosShift = pAttribute->GetAttributeAsFloat();
            return 0;
        }

        if (*pAttribute == "BumpScale")
        {
            fBumpScale = pAttribute->GetAttributeAsFloat();
            return 0;
        }
        if (*pAttribute == "BumpSpeed")
        {
            fBumpSpeed = pAttribute->GetAttributeAsFloat();
            return 0;
        }
        if (*pAttribute == "LodScale")
        {
            fLodScale = pAttribute->GetAttributeAsFloat();
            return 0;
        }
        if (*pAttribute == "GridStep")
        {
            fGridStep = pAttribute->GetAttributeAsFloat();
            return 0;
        }

        if (*pAttribute == "Reflection")
        {
            v4SeaParameters.y = pAttribute->GetAttributeAsFloat();
            return 0;
        }
        if (*pAttribute == "Transparency")
        {
            v4SeaParameters.z = pAttribute->GetAttributeAsFloat();
            return 0;
        }
        if (*pAttribute == "Attenuation")
        {
            v4SeaParameters.x = pAttribute->GetAttributeAsFloat();
            return 0;
        }

        if (*pAttribute == "Frenel")
        {
            fFrenel = pAttribute->GetAttributeAsFloat();
            return 0;
        }

        if (*pAttribute == "FoamV")
        {
            fFoamV = pAttribute->GetAttributeAsFloat();
            return 0;
        }
        if (*pAttribute == "FoamK")
        {
            fFoamK = pAttribute->GetAttributeAsFloat();
            return 0;
        }
        if (*pAttribute == "FoamUV")
        {
            fFoamUV = pAttribute->GetAttributeAsFloat();
            return 0;
        }
        if (*pAttribute == "FoamTexDisturb")
        {
            fFoamTextureDisturb = pAttribute->GetAttributeAsFloat();
            return 0;
        }
        if (*pAttribute == "FoamEnable")
        {
            bFoamEnable = pAttribute->GetAttributeAsDword() != 0;
            return 0;
        }

        if (*pAttribute == "SimpleSea")
        {
            bSimpleSea = pAttribute->GetAttributeAsDword() != 0;
            BuildVolumeTexture();
            return 0;
        }
        return 0;
    }

    if (*pParent == "fog")
    {
        if (*pAttribute == "Enable")
        {
            bFogEnable = pAttribute->GetAttributeAsDword() != 0;
            return 0;
        }
        if (*pAttribute == "Start")
        {
            fFogStartDistance = pAttribute->GetAttributeAsFloat();
            return 0;
        }
        if (*pAttribute == "Color")
        {
            vFogColor = (1.0f / 255.0f) * COLOR2VECTOR(pAttribute->GetAttributeAsDword());
            return 0;
        }
        if (*pAttribute == "SeaDensity")
        {
            fFogSeaDensity = pAttribute->GetAttributeAsFloat();
            return 0;
        }
        return 0;
    }

    if (*pAttribute == "Stop")
    {
        bStop = AttributesPointer->GetAttributeAsDword("Stop") != 0;
    }

    if (*pAttribute == "UnderWater")
    {
        bUnderSeaEnable = AttributesPointer->GetAttributeAsDword("UnderWater") != 0;
    }

    if (*pAttribute == "MaxSeaHeight")
    {
        fMaxSeaHeight = AttributesPointer->GetAttributeAsFloat("MaxSeaHeight", 50.0f);

        const float fScale = (fMaxSeaHeight >= _fAmp1 + _fAmp2) ? 1.0f : fMaxSeaHeight / (_fAmp1 + _fAmp2);

        fAmp1 = _fAmp1 * fScale;
        fAmp2 = _fAmp2 * fScale;
    }

    if (*pAttribute == "MaxDim")        { dwMaxDim = pAttribute->GetAttributeAsDword(); return 0; }
    if (*pAttribute == "MinDim")        { dwMinDim = pAttribute->GetAttributeAsDword(); return 0; }

    if (*pAttribute == "MaxSeaDistance")
    {
        fMaxSeaDistance = AttributesPointer->GetAttributeAsFloat("MaxSeaDistance");
    }

    return 0;
}

void SEA::CreateTargets()
{
    envMap = rs->CubeTextureCreate(128, 1, D3DUSAGE_RENDERTARGET, D3DFMT_R5G6B5, D3DPOOL_DEFAULT);
    sunRoadMap = rs->CubeTextureCreate(128, 1, D3DUSAGE_RENDERTARGET, D3DFMT_R5G6B5, D3DPOOL_DEFAULT);
    zStencil = rs->DepthSurfaceCreate(128, 128, D3DFMT_D24S8);

    reflection = rs->TextureCreate(128, 128, 1, D3DUSAGE_RENDERTARGET, D3DFMT_R5G6B5, D3DPOOL_DEFAULT);
    reflectionSunroad = rs->TextureCreate(128, 128, 1, D3DUSAGE_RENDERTARGET, D3DFMT_R5G6B5, D3DPOOL_DEFAULT);
    reflectionDepth = rs->DepthSurfaceCreate(128, 128, D3DFMT_D24S8);
}

void SEA::ReleaseTargets()
{
    TEXTURE_RELEASE(rs, reflection);
    TEXTURE_RELEASE(rs, reflectionSunroad);
    TEXTURE_RELEASE(rs, envMap);
    TEXTURE_RELEASE(rs, sunRoadMap);
    rs->DepthSurfaceRelease(zStencil);
    zStencil = {};
    rs->DepthSurfaceRelease(reflectionDepth);
    reflectionDepth = {};
}

void SEA::LostRender()
{
    ReleaseTargets();
}

void SEA::RestoreRender()
{
    cubeRefreshAll_ = true; // new cube targets hold nothing yet
    CreateTargets();
}
