#include "loading_transaction.hpp"
#include "foam.h"
#include "core.h"
#include "math_inlines.h"
#include "renderer.h"

#include <algorithm>

CoastFoam::CoastFoam()
{
    iCursorTex = -1;

    bEditMode = false;
    bCanEdit = false;
    bMoved = false;

    fMaxFoamDistance = 1000.0f;
    fFoamDeltaY = 0.2f;
    iFoamDivides = 4;

    fCursorPosX = 400.0f;
    fCursorPosY = 300.0f;
}

CoastFoam::~CoastFoam()
{
    clear();
    ReleaseBuffers();
    if (iHeightTexture >= 0)
        rs->TextureRelease(iHeightTexture);
}

void CoastFoam::ReleaseBuffers()
{
    if (iVBuffer >= 0)
        rs->ReleaseVertexBuffer(iVBuffer);
    iVBuffer = -1;
    if (iIBuffer >= 0)
        rs->ReleaseIndexBuffer(iIBuffer);
    iIBuffer = -1;
    dwBufferVertices = 0;
}

void CoastFoam::BuildBuffers()
{
    bBuffersDirty = false;
    ReleaseBuffers();
    aFoamCull.assign(aFoams.size(), FoamCull{});
    for (size_t f = 0; f < aFoams.size(); f++)
    {
        const auto *pF = aFoams[f];
        FoamCull &cull = aFoamCull[f];
        cull.bWorkParts = !pF->aWorkParts.empty() && !pF->aFoamParts.empty();
        if (!cull.bWorkParts)
            continue;
        cull.vFirst = pF->aWorkParts[0].v[0];
        for (const auto &part : pF->aFoamParts)
            cull.vCentre += part.v[0] + part.v[1];
        cull.vCentre /= static_cast<float>(2 * pF->aFoamParts.size());
        for (const auto &part : pF->aFoamParts)
            cull.fRadius = std::max(
                {cull.fRadius, glm::length(part.v[0] - cull.vCentre), glm::length(part.v[1] - cull.vCentre)});
        cull.fRadius = cull.fRadius * 1.0001f + 1e-3f; // rounding: near the surface the exact test (IsClipped) decides
    }
    if (aFoams.size() > 65536)
    {
        core.Trace("ERROR: CoastFoam: %zu foams, the vertices index at most 65536; no coast foam is drawn",
                   aFoams.size());
        return;
    }
    uint32_t vertices = 0, indices = 0;
    for (const auto *pF : aFoams)
    {
        const auto iLen = static_cast<uint32_t>(pF->aWorkParts.size());
        if (iLen < 2)
            continue;
        vertices += foam_gpu::kRows * iLen;
        indices += (foam_gpu::kRows - 1) * 6 * (iLen - 1);
    }
    if (indices == 0)
        return;
    iVBuffer = rs->CreateVertexBuffer(0, vertices * sizeof(foam_gpu::Vertex), D3DUSAGE_WRITEONLY);
    iIBuffer = rs->CreateIndexBuffer(indices * sizeof(uint32_t), D3DUSAGE_WRITEONLY, D3DFMT_INDEX32);
    auto *pV = iVBuffer >= 0 ? static_cast<foam_gpu::Vertex *>(rs->LockVertexBuffer(iVBuffer)) : nullptr;
    auto *pI = iIBuffer >= 0 ? static_cast<uint32_t *>(rs->LockIndexBuffer(iIBuffer)) : nullptr;
    if (!pV || !pI)
    {
        core.Trace("ERROR: CoastFoam: no buffers for %u vertices, %u indices; no coast foam is drawn", vertices,
                   indices);
        if (pV)
            rs->UnLockVertexBuffer(iVBuffer);
        if (pI)
            rs->UnLockIndexBuffer(iIBuffer);
        ReleaseBuffers();
        return;
    }
    // Every foam's strand: row y of column x is vertex base + y * iLen + x, the rows' quads as the CPU path drew them
    uint32_t base = 0, first = 0;
    for (size_t f = 0; f < aFoams.size(); f++)
    {
        auto *pF = aFoams[f];
        const auto iLen = static_cast<uint32_t>(pF->aWorkParts.size());
        pF->dwFirstIndex = first;
        pF->dwIndices = 0;
        if (iLen < 2)
            continue;
        for (uint32_t y = 0; y < foam_gpu::kRows; y++)
            for (uint32_t x = 0; x < iLen; x++)
            {
                const auto &wp = pF->aWorkParts[x];
                *pV++ = {wp.v[0].x, wp.v[0].z, wp.v[1].x, wp.v[1].z, wp.tu, static_cast<uint8_t>(y),
                         foam_gpu::edge_quarters(static_cast<int32_t>(x), static_cast<int32_t>(iLen)),
                         static_cast<uint8_t>(f & 255), static_cast<uint8_t>(f >> 8)};
            }
        for (uint32_t y = 0; y + 1 < foam_gpu::kRows; y++)
            for (uint32_t x = 0; x + 1 < iLen; x++)
            {
                *pI++ = base + (y + 0) * iLen + x;
                *pI++ = base + (y + 1) * iLen + x;
                *pI++ = base + (y + 0) * iLen + x + 1;

                *pI++ = base + (y + 1) * iLen + x;
                *pI++ = base + (y + 1) * iLen + x + 1;
                *pI++ = base + (y + 0) * iLen + x + 1;
            }
        pF->dwIndices = (foam_gpu::kRows - 1) * 6 * (iLen - 1);
        first += pF->dwIndices;
        base += foam_gpu::kRows * iLen;
    }
    rs->UnLockVertexBuffer(iVBuffer);
    rs->UnLockIndexBuffer(iIBuffer);
    dwBufferVertices = vertices;
}

bool CoastFoam::UpdateHeights(entid_t seaId, const SEA_BASE &Sea, SeaHeightField &field)
{
    if (!Sea.HeightField(field) || field.frameCount != foam_gpu::kFrames || field.size != foam_gpu::kSize)
    {
        if (!bHeightsLogged)
            core.Trace("ERROR: CoastFoam: the sea has no height field of %d frames of %d x %d (%d of %d); no "
                       "coast foam is drawn",
                       foam_gpu::kFrames, foam_gpu::kSize, foam_gpu::kSize, field.frameCount, field.size);
        bHeightsLogged = true;
        return false;
    }
    // The frames are the sea's for its life: uploaded once per sea (a new sea can come with the same foam entity)
    if (iHeightTexture >= 0 && heightSea == seaId && pHeightFrames == field.frames)
        return true;
    if (iHeightTexture < 0)
        iHeightTexture = rs->VolumeTextureCreate(foam_gpu::kSize, foam_gpu::kSize, foam_gpu::kSlices, 1, 0,
                                                 D3DFMT_A8R8G8B8, D3DPOOL_MANAGED);
    D3DLOCKED_BOX box{};
    if (iHeightTexture < 0 || !rs->TextureLockBox(iHeightTexture, 0, box, 0))
    {
        if (!bHeightsLogged)
            core.Trace("ERROR: CoastFoam: no volume texture for the sea's heights; no coast foam is drawn");
        bHeightsLogged = true;
        return false;
    }
    const std::vector<uint32_t> texels = foam_gpu::pack_frames(field.frames);
    for (int32_t s = 0; s < foam_gpu::kSlices; s++)
        for (int32_t y = 0; y < foam_gpu::kSize; y++)
            memcpy(static_cast<uint8_t *>(box.pBits) + s * box.SlicePitch + y * box.RowPitch,
                   texels.data() + (static_cast<size_t>(s) * foam_gpu::kSize + y) * foam_gpu::kSize,
                   foam_gpu::kSize * sizeof(uint32_t));
    rs->TextureUnlockBox(iHeightTexture, 0);
    heightSea = seaId;
    pHeightFrames = field.frames;
    return true;
}

void CoastFoam::DrawStrands(const SeaHeightField &field, const glm::mat4 &mWVP)
{
    if (iVBuffer < 0 || iIBuffer < 0)
        return;
    float c[foam_gpu::kRegisters][4]{};
    for (int32_t i = 0; i < 4; i++)
        for (int32_t j = 0; j < 4; j++)
            c[i][j] = mWVP[i][j];
    foam_gpu::wave_register(field.scale[0], field.move[0].x, field.move[0].z, field.amplitude[0], c[4]);
    foam_gpu::wave_register(field.scale[1], field.move[1].x, field.move[1].z, field.amplitude[1], c[5]);
    c[6][0] = field.frame[0];
    c[6][1] = field.frame[1];
    c[6][2] = fFoamDeltaY;
    c[7][0] = field.camera.x;
    c[7][1] = field.camera.z;
    c[7][2] = field.reach * field.reach;
    rs->SetVertexDeclaration(vertexDecl_);
    rs->SetStreamSource(0, iVBuffer, sizeof(foam_gpu::Vertex));
    rs->TextureSet(1, iHeightTexture);
    // Per strand: the runs of foams drawn this frame (a few foams not drawn between them ride along, their strands
    // collapse in the shader), at most kFoamsPerDraw foams and one texture a draw
    constexpr size_t kMaxGap = 8;
    const size_t count = aFoams.size();
    for (const auto &params : aStrandParams)
        for (size_t f = 0; f < count;)
        {
            if (params[f].w == 0.0f || aFoams[f]->dwIndices == 0)
            {
                f++;
                continue;
            }
            const size_t start = f;
            const int32_t texture = aFoams[start]->iTexture;
            size_t last = start;
            for (size_t g = start + 1; g < count && g - start < foam_gpu::kFoamsPerDraw; g++)
            {
                if (aFoams[g]->iTexture != texture)
                    break;
                if (params[g].w == 0.0f)
                    continue;
                if (g - last > kMaxGap)
                    break;
                last = g;
            }
            c[6][3] = static_cast<float>(start);
            for (size_t g = start; g <= last; g++)
                memcpy(c[8 + g - start], &params[g], sizeof(c[0]));
            rs->SetVertexShaderConstantF(0, &c[0][0], static_cast<uint32_t>(8 + last - start + 1));
            rs->TextureSet(0, texture);
            const uint32_t firstIndex = aFoams[start]->dwFirstIndex;
            const uint32_t indices = aFoams[last]->dwFirstIndex + aFoams[last]->dwIndices - firstIndex;
            rs->DrawBuffer(-1, sizeof(foam_gpu::Vertex), iIBuffer, 0, dwBufferVertices, firstIndex, indices / 3,
                           "CoastFoam");
            f = last + 1;
        }
    rs->TextureSet(1, -1);
    rs->SetStreamSource(0, -1, 0);
}

bool CoastFoam::Init()
{
    rs = static_cast<VRenderer *>(core.GetService("renderer"));

    constexpr D3DVERTEXELEMENT9 elements[] = {
        {0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
        {0, 16, D3DDECLTYPE_FLOAT1, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
        {0, 20, D3DDECLTYPE_UBYTE4N, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0},
        D3DDECL_END()};
    vertexDecl_ = rs->CreateVertexDeclaration(elements);

    iCursorTex = rs->TextureCreate("cursor.tga");

    Load();

    bCanEdit = AttributesPointer->GetAttributeAsDword("edit", 0) != 0;
    return true;
}

void CoastFoam::Execute(uint32_t Delta_Time)
{
    bEditMode = (bCanEdit) ? (core.Controls->GetDebugKeyState(VK_NUMLOCK) < 0) : false;
    auto fDeltaTime = static_cast<float>(Delta_Time) * 0.001f;
}

void CoastFoam::ExtractRay(const D3DVIEWPORT9 &viewport, float fCursorX, float fCursorY, glm::vec3 &raystart,
                           glm::vec3 &rayend)
{
    const glm::mat4 matProj = rs->GetProjection();
    glm::vec3 v;
    v.x = (((2.0f * fCursorX) / viewport.Width) - 1) / matProj[0][0];
    v.y = -(((2.0f * fCursorY) / viewport.Height) - 1) / matProj[1][1];
    v.z = 1.0f;

    glm::mat4 mView3x3;
    glm::mat4 mView = rs->GetView();
    storm::math::invertRigid(mView);
    storm::math::copy3x3(*&mView3x3, mView);

    glm::vec3 raydir;
    glm::vec3 rayorig;
    raydir = storm::math::transformPoint(mView3x3, v);
    rayorig = storm::math::pos(mView);

    raystart = rayorig;
    rayend = (rayorig + (raydir * 5000.f));
}

void CoastFoam::Realize(uint32_t Delta_Time)
{
    pFrustumPlanes = rs->GetPlanes();

    // The sea can be removed/replaced while this foam entity survives a scene transition.
    const entid_t seaId = core.GetEntityId("sea");
    auto *pSea = static_cast<SEA_BASE *>(core.GetEntityPointerSafe(seaId));
    if (pSea == nullptr)
        return;
    SEA_BASE &Sea = *pSea;

    auto fDeltaTime = static_cast<float>(Delta_Time) * 0.001f;
    D3DVIEWPORT9 vp;
    rs->GetViewport(&vp);

    glm::mat4 mWorld;
    mWorld = glm::mat4(1.0f); // rs->GetWorld();
    glm::mat4 mView = rs->GetView();
    glm::mat4 mProjection = rs->GetProjection();

    glm::mat4 mWV, mWVP;
    mWV = mView * mWorld;
    mWVP = mProjection * mWV;

    SeaHeightField field;
    const bool bHeights = UpdateHeights(seaId, Sea, field);
    if (bBuffersDirty)
        BuildBuffers();

    glm::vec3 vCamPos, vCamAng;
    float fPerspective;
    rs->GetCamera(vCamPos, vCamAng, fPerspective);

    uint64_t dw1;
    dwNumPenasExecuted = 0;
    RDTSC_B(dw1);
    for (auto &params : aStrandParams)
        params.assign(aFoams.size(), glm::vec4(0.0f));
    for (int32_t i = 0; i < aFoams.size(); i++)
        if (IsFoamVisible(i, vCamPos))
            ExecuteFoam(i, fDeltaTime);
    if (bHeights)
        DrawStrands(field, mWVP);
    RDTSC_E(dw1);


    if (storm::loading::active || !bCanEdit)
        return;

    std::vector<RS_LINE> aLines;
    std::vector<RS_RECT> aRects;

    if (bEditMode)
        for (int32_t i = 0; i < aFoams.size(); i++)
        {
            aLines.clear();
            aRects.clear();

            auto pF = aFoams[i];
            for (int32_t j = 0; j < pF->aFoamParts.size(); j++)
            {
                RS_LINE rl, r2;
                rl.dwColor = 0xFFFFFFFF;
                r2.dwColor = 0xFFFFFFFF;

                rl.vPos = pF->aFoamParts[j].v[0];
                r2.vPos = pF->aFoamParts[j].v[1];
                aLines.push_back(rl);
                aLines.push_back(r2);

                if (j != pF->aFoamParts.size() - 1)
                {
                    rl.vPos = pF->aFoamParts[j].v[0];
                    r2.vPos = pF->aFoamParts[j + 1].v[0];
                    aLines.push_back(rl);
                    aLines.push_back(r2);

                    rl.vPos = pF->aFoamParts[j].v[1];
                    r2.vPos = pF->aFoamParts[j + 1].v[1];
                    aLines.push_back(rl);
                    aLines.push_back(r2);
                }

                rl.vPos = pF->aFoamParts[j].v[0];
                r2.vPos = pF->aFoamParts[j].v[1];
                aLines.push_back(rl);
                aLines.push_back(r2);

                if (bEditMode)
                {
                    auto v1 = pF->aFoamParts[j].v[0];
                    auto v2 = pF->aFoamParts[j].v[1];
                    storm::math::ProjectedPoint vP1, vP2;
                    storm::math::projectPoints(mWVP, &v1, &vP1, 1, static_cast<float>(vp.Width) * 0.5f, static_cast<float>(vp.Height) * 0.5f, sizeof(glm::vec3), sizeof(storm::math::ProjectedPoint));
                    storm::math::projectPoints(mWVP, &v2, &vP2, 1, static_cast<float>(vp.Width) * 0.5f, static_cast<float>(vp.Height) * 0.5f, sizeof(glm::vec3), sizeof(storm::math::ProjectedPoint));
                    auto fR1 = 0.15f;
                    auto fR2 = 0.15f;
                    if (SQR(vP1.x - fCursorPosX) + SQR(vP1.y - fCursorPosY) < 100.0f)
                        fR1 = 0.3f;
                    if (SQR(vP2.x - fCursorPosX) + SQR(vP2.y - fCursorPosY) < 100.0f)
                        fR2 = 0.3f;

                    RS_RECT r1, r2;
                    r1.vPos = v1;
                    r1.dwColor = 0xFFFFFF00;
                    r1.fAngle = 0.0f;
                    r1.fSize = fR1;
                    r2.vPos = v2;
                    r2.dwColor = 0xFF00FF00;
                    r2.fAngle = 0.0f;
                    r2.fSize = fR2;
                    aRects.push_back(r1);
                    aRects.push_back(r2);
                }
            }

            glm::mat4 mI;
            mI = glm::mat4(1.0f);
            rs->SetWorld(mI);
            if (bEditMode)
                rs->DrawLines(aLines.data(), aLines.size() / 2, "Line");

            rs->DrawRects(aRects.data(), aRects.size(), "FoamPoints");
        }

    auto bShift = core.Controls->GetDebugAsyncKeyState(VK_SHIFT) < 0;
    auto bMenu = core.Controls->GetDebugAsyncKeyState(VK_MENU) < 0;
    if (bShift && core.Controls->GetDebugAsyncKeyState('L') < 0)
        Load();

    if (bEditMode)
    {
        RS_SPRITE spr[4];

        auto fMinDistance = 1e10f;
        if (!bMoved)
        {
            iEditFoam = -1;
            iEditFoamPart = -1;
            iEditFoamVertex = -1;
            for (int32_t i = 0; i < aFoams.size(); i++)
            {
                auto *pF = aFoams[i];
                for (int32_t j = 0; j < pF->aFoamParts.size(); j++)
                {
                    for (int32_t k = 0; k < 2; k++)
                    {
                        storm::math::ProjectedPoint vP;
                        auto v = pF->aFoamParts[j].v[k];

                        storm::math::projectPoints(mWVP, &v, &vP, 1, static_cast<float>(vp.Width) * 0.5f, static_cast<float>(vp.Height) * 0.5f, sizeof(glm::vec3), sizeof(storm::math::ProjectedPoint));
                        auto fD = SQR(vP.x - fCursorPosX) + SQR(vP.y - fCursorPosY);
                        if (fD < 100.0f && fD < fMinDistance)
                        {
                            fMinDistance = fD;

                            iEditFoam = i;
                            iEditFoamPart = j;
                            iEditFoamVertex = k;
                        }
                    }
                }
            }
        }
        auto x = fCursorPosX, dx = 32.0f;
        auto y = fCursorPosY, dy = 32.0f;
        FillSpriteVertex(spr[0], x, y, 0.1f, 0xFFFFFFFF, 0.0f, 0.0f);
        FillSpriteVertex(spr[1], x, y + dy, 0.1f, 0xFFFFFFFF, 0.0f, 1.0f);
        FillSpriteVertex(spr[2], x + dx, y + dy, 0.1f, 0xFFFFFFFF, 1.0f, 1.0f);
        FillSpriteVertex(spr[3], x + dx, y, 0.1f, 0xFFFFFFFF, 1.0f, 0.0f);

        rs->TextureSet(0, iCursorTex);
        rs->DrawSprites(spr, 1, "Sprite");

        auto bSelected = iEditFoam >= 0 && iEditFoamPart >= 0 && iEditFoamVertex >= 0;

        CONTROL_STATE cs, csH, csV;

        core.Controls->GetControlState("Turn H", csH);
        fCursorPosX += csH.lValue;
        core.Controls->GetControlState("Turn V", csV);
        fCursorPosY += csV.lValue;

        fCursorPosX = Max(0.0f, Min(fCursorPosX, static_cast<float>(vp.Width)));
        fCursorPosY = Max(0.0f, Min(fCursorPosY, static_cast<float>(vp.Height)));

        core.Controls->GetControlState("CoastFoamLB", cs);

        if (cs.state == CST_ACTIVE && !bMoved)
        {
            bMoved = true;
        }

        if (cs.state == CST_INACTIVE && bMoved)
        {
            bMoved = false;
        }
        glm::vec3 vStart, vEnd;
        ExtractRay(vp, fCursorPosX, fCursorPosY, vStart, vEnd);

        if (bMoved && (csH.lValue || csV.lValue))
        {
            if (bSelected && (vStart.y > 0.0f && vStart.y > vEnd.y))
            {
                auto *pFP = &aFoams[iEditFoam]->aFoamParts[iEditFoamPart];
                const auto pln = storm::math::planeFromPointNormal(glm::vec3(0.0f, 1.0f, 0.0f), glm::vec3(0.0f));
                glm::vec3 vNewPos;
                if (storm::math::intersectSegmentPlane(pln, vStart, vEnd, vNewPos))
                {
                    auto vDiff = pFP->v[1] - pFP->v[0];
                    auto vDir = vNewPos - pFP->v[iEditFoamVertex];
                    if (bMenu)
                    {
                        for (int32_t i = 0; i < aFoams[iEditFoam]->aFoamParts.size(); i++)
                        {
                            aFoams[iEditFoam]->aFoamParts[i].v[0] += vDir;
                            aFoams[iEditFoam]->aFoamParts[i].v[1] += vDir;
                        }
                    }
                    else
                    {
                        pFP->v[iEditFoamVertex] = vNewPos;

                        if (iEditFoamVertex == 0 && bShift)
                            pFP->v[1] = pFP->v[0] + vDiff;
                    }

                    RecalculateFoam(iEditFoam);
                }
            }
        }

        CONTROL_STATE csIns, csDel, csCopy;
        core.Controls->GetControlState("CoastFoamINS", csIns);
        core.Controls->GetControlState("CoastFoamDEL", csDel);
        core.Controls->GetControlState("CoastFoamCopy", csCopy);

        if (bShift && csCopy.state == CST_ACTIVATED && bSelected)
        {
            auto *pF = new Foam();
            *pF = *aFoams[iEditFoam];

            for (int32_t i = 0; i < pF->aFoamParts.size(); i++)
            {
                pF->aFoamParts[i].v[0] += glm::vec3(10.0f, 0.0f, 10.0f);
                pF->aFoamParts[i].v[1] += glm::vec3(10.0f, 0.0f, 10.0f);
            }

            aFoams.push_back(pF);
            RecalculateFoam(aFoams.size() - 1);
        }

        if (csIns.state == CST_ACTIVATED)
        {
            if (bShift)
            {
                // insert new foam
                glm::mat4 mIView = rs->GetView();
                storm::math::invertRigid(mIView);
                auto *pF = new Foam();

                InitNewFoam(pF);

                auto vPos = storm::math::pos(mIView);
                vPos.y = 0.0f;
                auto vZ = storm::math::vz(mIView);
                vZ.y = 0.0;
                vZ = storm::math::normalizeOrZero(vZ);
                auto vX = storm::math::vx(mIView);
                vX.y = 0.0;
                vX = storm::math::normalizeOrZero(vX);

                FoamPart foam1, foam2;
                foam1.v[0] = vPos;
                foam1.v[1] = vPos + vZ * 6.0f;
                foam2.v[0] = vPos + vX * 3.0f;
                foam2.v[1] = vPos + vX * 3.0f + vZ * 6.0f;

                pF->aFoamParts.emplace_back(foam1);
                pF->aFoamParts.emplace_back(foam2);

                aFoams.push_back(pF);
                RecalculateFoam(aFoams.size() - 1);
            }
            else if (bSelected)
            {
                auto *pF = aFoams[iEditFoam];
                if (iEditFoamPart == pF->aFoamParts.size() - 1)
                {
                    const auto foam1 = pF->aFoamParts.end() - 2, foam2 = pF->aFoamParts.end() - 1;

                    FoamPart foam;
                    foam.v[0] = 2.0f * foam2->v[0] - foam1->v[0];
                    foam.v[1] = 2.0f * foam2->v[1] - foam1->v[1];
                    pF->aFoamParts.push_back(foam);

                }
                else
                {
                    pF->aFoamParts.insert(pF->aFoamParts.begin() + iEditFoamPart, FoamPart{});
                    pF->aFoamParts[iEditFoamPart] = pF->aFoamParts[iEditFoamPart + 1];
                    pF->aFoamParts[iEditFoamPart + 1].v[0] =
                        0.5f * (pF->aFoamParts[iEditFoamPart].v[0] + pF->aFoamParts[iEditFoamPart + 2].v[0]);
                    pF->aFoamParts[iEditFoamPart + 1].v[1] =
                        0.5f * (pF->aFoamParts[iEditFoamPart].v[1] + pF->aFoamParts[iEditFoamPart + 2].v[1]);
                }
                RecalculateFoam(iEditFoam);
            }
        }

        if (csDel.state == CST_ACTIVATED && bSelected)
        {
            bBuffersDirty = true; // the foams' indices move
            if (bShift)
                aFoams.erase(aFoams.begin() + iEditFoam);
            else
            {
                aFoams[iEditFoam]->aFoamParts.erase(aFoams[iEditFoam]->aFoamParts.begin() + iEditFoamPart);
                if (aFoams[iEditFoam]->aFoamParts.size() <= 1)
                    aFoams.erase(aFoams.begin() + iEditFoam);
                else
                    RecalculateFoam(iEditFoam);
            }
        }
    }
}

void CoastFoam::InitNewFoam(Foam *pF)
{
    pF->fAlphaMin = 148.0f;
    pF->fAlphaMax = 196.0f;
    pF->fAppearMax = 0.0f;
    pF->fAppearMin = 0.0f;
    pF->fSpeedMin = 0.2f;
    pF->fSpeedMax = 0.25f;
    pF->fTexScaleX = 0.1f;
    pF->Type = FOAM_TYPE_2;
    pF->sTexture = "foam.tga";
    pF->iNumFoams = 2;

    pF->iTexture = rs->TextureCreate(("weather\\coastfoam\\" + pF->sTexture).c_str());
}

// The foam's tests as the CPU path made them: its first work part within fMaxFoamDistance, not all of its parts'
// ends behind one frustum plane (IsClipped). Its bounding sphere answers for most foams (aFoamCull, contiguous).
bool CoastFoam::IsFoamVisible(int32_t iFoam, const glm::vec3 &vCamPos) const
{
    const FoamCull &cull = aFoamCull[iFoam];
    if (!cull.bWorkParts || glm::length(cull.vFirst - vCamPos) > fMaxFoamDistance)
        return false;
    bool bStraddles = false;
    for (uint32_t j = 0; j < 4; j++)
    {
        const PLANE &plane = pFrustumPlanes[j];
        const float fD = glm::dot(cull.vCentre, glm::vec3(plane.Nx, plane.Ny, plane.Nz)) - plane.D;
        if (fD + cull.fRadius < 0.0f)
            return false; // every end behind this plane
        bStraddles |= fD - cull.fRadius < 0.0f;
    }
    return !bStraddles || !IsClipped(aFoams[iFoam]);
}

// The strands' motion and alpha of a foam the camera sees; vs_port_coastfoam draws them from aStrandParams
void CoastFoam::ExecuteFoam(int32_t iFoam, float fDeltaTime)
{
    auto *pF = aFoams[iFoam];
    dwNumPenasExecuted++;

    for (int32_t k = 0; k < pF->iNumFoams; k++)
    {
        const int32_t kk = (k == 1) ? 0 : 1;

        if (pF->fMove[k] < 0.0f)
        {
            pF->fMove[k] += fDeltaTime;
            if (pF->fMove[k] < 0.0f)
                continue;
            pF->fMove[k] = 0.0f;
        }

        if (!storm::loading::active)
        {
            switch (pF->iMode[k])
            {
            case 0:
                pF->fAlpha[k] = Clampf(pF->fAlpha[k] + fDeltaTime * 0.15f);
                if (pF->fMove[k] > 0.6f)
                    pF->iMode[k] = 1;
                break;
            case 1:
                pF->fAlpha[k] = Clampf(pF->fAlpha[k] + fDeltaTime * 0.15f);
                pF->fSpeed[k] *= (1.0f - Clampf(fDeltaTime * 0.8f));
                if (pF->fSpeed[k] < 0.01f && pF->fMove[kk] < 0.0f)
                {
                    pF->fSX[kk] = Rnd(100.0f);
                    pF->fAlpha[kk] = 0.0f;
                    pF->iMode[kk] = 0;
                    pF->fSpeed[kk] = RRnd(pF->fSpeedMin, pF->fSpeedMax);
                    pF->fMove[kk] = 0.0f;
                    pF->fAlphaColor[k] = RRnd(pF->fAlphaMin, pF->fAlphaMax) / 255.0f;
                }
                if (pF->fSpeed[k] < 0.01f)
                {
                    pF->iMode[k] = 2;
                }
                break;
            case 2:
                pF->fSpeed[k] -= fDeltaTime * 0.035f;
                pF->fSpeed[k] = Max(-0.04f, pF->fSpeed[k]);
                pF->fAlpha[k] = Clampf(pF->fAlpha[k] - fDeltaTime * 0.2f);
                if (pF->fAlpha[k] <= 0.0f)
                {
                    pF->fSX[k] = Rnd(100.0f);
                    pF->fAlpha[k] = 0.0f;
                    pF->iMode[k] = 0;
                    pF->fSpeed[k] = RRnd(pF->fSpeedMin, pF->fSpeedMax);
                    pF->fMove[k] = -RRnd(pF->fAppearMin, pF->fAppearMax);
                    pF->fAlphaColor[k] = RRnd(pF->fAlphaMin, pF->fAlphaMax) / 255.0f;
                }
                break;
            }

        }

        pF->fMove[k] += pF->fSpeed[k] * fDeltaTime;
        aStrandParams[k][iFoam] = glm::vec4(pF->fMove[k], pF->fAlphaColor[k] * pF->fAlpha[k], pF->fSX[k], 1.0f);
    }
}

bool CoastFoam::IsClipped(const Foam *pF) const
{
    if (!pF)
        return true;

    glm::vec3 vP[4];
    uint32_t dwPlanesPoints[4];
    const uint32_t dwSize = pF->aFoamParts.size();

    for (uint32_t k = 0; k < 4; k++)
    {
        dwPlanesPoints[k] = 0;
        vP[k] = glm::vec3(pFrustumPlanes[k].Nx, pFrustumPlanes[k].Ny, pFrustumPlanes[k].Nz);
    }

    for (uint32_t i = 0; i < dwSize; i++)
    {
        auto v0 = pF->aFoamParts[i].v[0];
        auto v1 = pF->aFoamParts[i].v[1];

        for (uint32_t j = 0; j < 4; j++)
        {
            const auto fD1 = glm::dot(v0, vP[j]) - pFrustumPlanes[j].D;
            const auto fD2 = glm::dot(v1, vP[j]) - pFrustumPlanes[j].D;

            if (fD1 < 0.0f)
                dwPlanesPoints[j]++;
            if (fD2 < 0.0f)
                dwPlanesPoints[j]++;
        }
    }

    for (uint32_t z = 0; z < 4; z++)
        if (dwPlanesPoints[z] == dwSize * 2)
            return true;

    return false;
}

void CoastFoam::RecalculateFoam(int32_t iFoam)
{
    auto *pF = aFoams[iFoam];
    bBuffersDirty = true; // the static strands (BuildBuffers)

    pF->aWorkParts.clear();

    pF->fMove[0] = -RRnd(pF->fAppearMin, pF->fAppearMax);
    pF->fMove[1] = -100000.0f; // RRnd(pF->fAppearMin, pF->fAppearMax);
    pF->fSpeed[0] = RRnd(pF->fSpeedMin, pF->fSpeedMax);
    pF->fSpeed[1] = RRnd(pF->fSpeedMin, pF->fSpeedMax);

    pF->fAlphaColor[0] = RRnd(pF->fAlphaMin, pF->fAlphaMax) / 255.0f;
    pF->fAlphaColor[1] = RRnd(pF->fAlphaMin, pF->fAlphaMax) / 255.0f;

    pF->fSX[0] = Rnd(100.0f);
    pF->fSX[1] = Rnd(100.0f);
    pF->fAlpha[0] = 0.0f;
    pF->fAlpha[1] = 0.0f;
    pF->iMode[0] = 0;
    pF->iMode[1] = 0;

    auto sx = 0.0f;

    if (!pF->aFoamParts.empty())
    {
        for (size_t i = 0; i < pF->aFoamParts.size() - 1; i++)
        {
            auto *pF1 = &pF->aFoamParts[i];
            auto *pF2 = &pF->aFoamParts[i + 1];

            const auto dx = glm::length(pF1->v[0] - pF2->v[0]) / static_cast<float>(iFoamDivides);
            for (int32_t j = 0; j < iFoamDivides; j++)
            {
                if (j == 0 && i != 0)
                    continue;

                pF->aWorkParts.push_back(WorkPart{});
                auto *pWP = &pF->aWorkParts.back();
                pWP->tu = sx * pF->fTexScaleX;
                pWP->v[0] =
                    pF1->v[1] + static_cast<float>(j) / static_cast<float>(iFoamDivides - 1) * (pF2->v[1] - pF1->v[1]);
                pWP->v[1] =
                    pF1->v[0] + static_cast<float>(j) / static_cast<float>(iFoamDivides - 1) * (pF2->v[0] - pF1->v[0]);

                sx += dx;
            }
        }
    }
}

void CoastFoam::clear()
{
    for (auto &foam : aFoams)
    {
        if (foam->iTexture >= 0)
            rs->TextureRelease(foam->iTexture);
        delete foam;
    }
    aFoams.clear();
    bBuffersDirty = true;
}

void CoastFoam::Load()
{
    char cSection[256], cKey[256], cTemp[1024];

    const auto sID = std::string("resource\\foam\\locations\\") + to_string(AttributesPointer->GetAttribute("id")) + ".ini";
    auto pI = fio->OpenIniFile(sID.c_str());
    if (!pI)
        return;

    clear();
    const auto iNumFoams = pI->GetInt(nullptr, "NumFoams", 0);
    fMaxFoamDistance = pI->GetFloat(nullptr, "MaxFoamDistance", 1000.0f);
    fFoamDeltaY = pI->GetFloat(nullptr, "FoamDeltaY", 0.2f);
    iFoamDivides = pI->GetInt(nullptr, "FoamDivides", 4);
    for (int32_t i = 0; i < iNumFoams; i++)
    {
        aFoams.push_back(new Foam);
        auto *pF = aFoams.back();

        sprintf_s(cSection, "foam_%d", i);

        const int32_t iNumParts = pI->GetInt(cSection, "NumParts", 0);

        pI->ReadString(cSection, "Alpha", cTemp, sizeof(cTemp), "148, 196");
        sscanf(cTemp, "%f, %f", &pF->fAlphaMin, &pF->fAlphaMax);

        pI->ReadString(cSection, "Speed", cTemp, sizeof(cTemp), "0.200, 0.30");
        sscanf(cTemp, "%f, %f", &pF->fSpeedMin, &pF->fSpeedMax);

        pI->ReadString(cSection, "Braking", cTemp, sizeof(cTemp), "0.000, 0.000");
        sscanf(cTemp, "%f, %f", &pF->fBrakingMin, &pF->fBrakingMax);

        pI->ReadString(cSection, "Appear", cTemp, sizeof(cTemp), "0.000, 0.200");
        sscanf(cTemp, "%f, %f", &pF->fAppearMin, &pF->fAppearMax);

        pI->ReadString(cSection, "TexScaleX", cTemp, sizeof(cTemp), "0.050");
        sscanf(cTemp, "%f", &pF->fTexScaleX);

        pF->iNumFoams = (pI->GetInt(cSection, "NumFoams", 2) == 2) ? 2 : 1;

        pI->ReadString(cSection, "Texture", cTemp, sizeof(cTemp), "foam.tga");
        pF->sTexture = cTemp;
        pF->iTexture = rs->TextureCreate((std::string("weather\\coastfoam\\") + cTemp).c_str());
        pF->Type = static_cast<FOAMTYPE>(pI->GetInt(cSection, "Type", FOAM_TYPE_2));
        if (pF->Type != FOAM_TYPE_2)
        {
            // Type 1 (eight rows of their own speeds per column) was drawn by a CPU path no game file uses (every
            // foam of the game's resource/foam is type 2); its strands are drawn as type 2
            core.Trace("ERROR: CoastFoam: %s foam_%d has type %d; drawn as type 2", sID.c_str(), i,
                       static_cast<int>(pF->Type));
            pF->Type = FOAM_TYPE_2;
        }

        for (int32_t j = 0; j < ((iNumParts) ? iNumParts : 100000); j++)
        {
            sprintf_s(cKey, "key_%d", j);
            glm::vec3 v1, v2;
            v1.y = v2.y = 0.0f;
            pI->ReadString(cSection, cKey, cTemp, sizeof(cTemp), "");
            if (!cTemp[0])
                break;
            sscanf(cTemp, "%f, %f, %f, %f", &v1.x, &v1.z, &v2.x, &v2.z);

            FoamPart foam;
            foam.v[0] = v1;
            foam.v[1] = v2;
            pF->aFoamParts.push_back(foam);
        }
        pF->fMove[0] = -RRnd(pF->fAppearMin, pF->fAppearMax);
        pF->fMove[1] = -100000.0f; // RRnd(pF->fAppearMin, pF->fAppearMax);
        pF->fSpeed[0] = RRnd(pF->fSpeedMin, pF->fSpeedMax);
        pF->fSpeed[1] = RRnd(pF->fSpeedMin, pF->fSpeedMax);
        pF->fSX[0] = Rnd(100.0f);
        pF->fSX[1] = Rnd(100.0f);
        pF->fAlphaColor[0] = RRnd(pF->fAlphaMin, pF->fAlphaMax) / 255.0f;
        pF->fAlphaColor[1] = RRnd(pF->fAlphaMin, pF->fAlphaMax) / 255.0f;
        pF->fAlpha[0] = 0.0f;
        pF->fAlpha[1] = 0.0f;
        pF->iMode[0] = 0;
        pF->iMode[1] = 0;

        RecalculateFoam(i);
    }
}

uint32_t CoastFoam::AttributeChanged(ATTRIBUTES *pA)
{
    return 0;
}
