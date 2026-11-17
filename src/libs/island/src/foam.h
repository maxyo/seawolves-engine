#pragma once

#include "ai_flow_graph.h"
#include "island_base.h"
#include "collide.h"
#include "renderer.h"
#include "model.h"
#include "sea_base.h"
#include "vma.hpp"
#include "foam_gpu.hpp"

enum FOAMTYPE
{
    FOAM_TYPE_1 = 1,
    FOAM_TYPE_2 = 2,

    FOAM_MODE_FORCEDWORD = 0x7FFFFFFF
};

class CoastFoam : public Entity
{
  public:
    CoastFoam();
    ~CoastFoam();

    bool Init();
    void Realize(uint32_t Delta_Time);
    void Execute(uint32_t Delta_Time);
    uint32_t AttributeChanged(ATTRIBUTES *pA);

    void ProcessStage(Stage stage, uint32_t delta) override
    {
        switch (stage)
        {
        case Stage::execute:
            Execute(delta);
            break;
        case Stage::loading_realize:
            Realize(0);
            break;
        case Stage::realize:
            Realize(delta);
            break;
        }
    }

  private:
    struct FoamPart
    {
        glm::vec3 v[2];
    };

    struct WorkPart
    {
        glm::vec3 v[2];
        float tu;
        // FOAMMODE    Mode;
    };

    struct Foam
    {
        Foam()
        {
            iTexture = -1;
        }

        std::vector<FoamPart> aFoamParts;
        std::vector<WorkPart> aWorkParts;
        float fMove[2];
        float fSX[2];
        float fSpeed[2], fBraking[2];
        int32_t iMode[2];
        int32_t iTexture;
        std::string sTexture;
        int32_t iNumFoams;

        FOAMTYPE Type;

        float fAlphaColor[2], fAlpha[2], fAlphaMin, fAlphaMax;
        float fSpeedMin, fSpeedMax;
        float fBrakingMin, fBrakingMax;
        float fAppearMin, fAppearMax;
        float fTexScaleX;

        uint32_t dwFirstIndex = 0, dwIndices = 0; // its strand in the static index buffer (BuildBuffers)
    };

    int32_t iEditFoam, iEditFoamPart, iEditFoamVertex;
    bool bEditMode, bMoved, bCanEdit;
    float fCursorPosX, fCursorPosY;
    int32_t iFoamTexture, iCursorTex;
    std::vector<Foam *> aFoams;
    // Every foam's strand once (BuildBuffers): drawn twice when the foam has two, with the strand's motion
    // (aStrandParams) moving it in vs_port_coastfoam
    int32_t iVBuffer = -1, iIBuffer = -1;
    uint32_t dwBufferVertices = 0;
    bool bBuffersDirty = true;
    storm::render::VertexDeclarationId vertexDecl_{};
    // Per foam, for its visibility test (IsFoamVisible): the first work part's end, the bounding sphere of its parts
    struct FoamCull
    {
        glm::vec3 vFirst{}, vCentre{};
        float fRadius = 0.0f;
        bool bWorkParts = false;
    };
    std::vector<FoamCull> aFoamCull;
    // This frame's strands: per strand (0, 1) and foam: move, alpha, texture offset, 1 when drawn
    std::vector<glm::vec4> aStrandParams[2];
    // The sea's height frames on the GPU (foam_gpu::pack_frames), uploaded once per sea
    int32_t iHeightTexture = -1;
    entid_t heightSea{};
    const uint8_t *const *pHeightFrames = nullptr;
    bool bHeightsLogged = false;
    PLANE *pFrustumPlanes;

    float fMaxFoamDistance, fFoamDeltaY;
    int32_t iFoamDivides;

    uint32_t dwNumPenasExecuted;

    VRenderer *rs;

    void ExtractRay(const D3DVIEWPORT9 &viewport, float fCursorX, float fCursorY, glm::vec3 &raystart, glm::vec3 &rayend);

    bool IsClipped(const Foam *pF) const;

    void Load();
    void clear();

    void InitNewFoam(Foam *pF);
    void RecalculateFoam(int32_t iFoam);
    bool IsFoamVisible(int32_t iFoam, const glm::vec3 &vCamPos) const;
    void ExecuteFoam(int32_t iFoam, float fDeltaTime);
    void BuildBuffers();
    void ReleaseBuffers();
    // The sea's height field this frame, its frames on the GPU; false (logged) when the sea has none
    bool UpdateHeights(entid_t seaId, const SEA_BASE &Sea, SeaHeightField &field);
    void DrawStrands(const SeaHeightField &field, const glm::mat4 &mWVP);
};
