// ************************************************************************
// *  Author : Vano, 1861; Description : Sea/Ocean/Water/Lake/River/pool  *
// ************************************************************************
#pragma once

#include "sea_base.h"
#include <chrono>
#include "renderer.h"
#include "vma.hpp"
#include "wave_focus.hpp"


class SEA : public SEA_BASE
{
  private:
    // uint32_t dwSkyCode = MakeHashValue("sky");
    uint32_t dwIslandCode = MakeHashValue("island");
    uint32_t dwShipCode = MakeHashValue("ship");
    uint32_t dwSailCode = MakeHashValue("sail");

    storm::render::VertexDeclarationId vertexDecl_;

    // A point of the flat grid, metres from the mesh's origin (MeshOrigin); the vertex shader displaces it
    // (sea_surface_gpu.hpp).
    struct SeaVertex
    {
        float x, y, z;
    };

    struct SeaBlock
    {
        int32_t iX1, iX2, iY1, iY2; // result rectangle(in units)
        int32_t iSize0;             //

        int32_t iTX, iTY;
        int32_t iSize;
        int32_t iLOD;
        int32_t iIStart;
        int32_t iIFirst, iILast;

    };

    struct SeaTrash
    {
        glm::vec3 vPos;
        glm::vec3 vSpeed;
        uint32_t dwSubTexture;
        float fTime;
        float fSize;
    };

    struct SeaLight
    {
        glm::vec3 vPos;
        glm::vec3 vSpeed;
        uint32_t dwSubTexture;
        float fTime;
    };

    VRenderer *rs;
    static SEA *pSea;

    uint32_t dwMaxDim, dwMinDim;
    float fMaxSeaHeight;
    float fMaxSeaDistance;
    float fGridStep;
    float fLodScale;
    float fBumpScale, fBumpSpeed;
    glm::vec3 vMoveSpeed1, vMoveSpeed2;

    glm::vec3 vCamPos, vSeaCenterPos, vWorldOffset;

    bool bStarted;
    bool bUnderSea;
    bool bUnderSeaEnable;
    float bUnderSeaStarted;

    int32_t iVSeaBuffer, iISeaBuffer;

    // Renderer texture ids: the bump normal frames, the environment / sun road cubes and the reflections.
    int32_t volumeTexture = -1;
    int32_t envMap = -1, sunRoadMap = -1;
    storm::render::DepthSurfaceId zStencil;

    int32_t reflection = -1, reflectionSunroad = -1;
    storm::render::DepthSurfaceId reflectionDepth;

    void CreateTargets();
    void ReleaseTargets();

    bool bSimpleSea;

    uint16_t *pTriangles;
    uint32_t *pIndices;
    int32_t iVStart, iTStart, iIStart;
    // frames whose farthest blocks did not fit the vertex buffer (FitBlocksToBuffer), reported at the end
    uint32_t dwFramesCut = 0, dwMostBlocksCut = 0;

    PLANE *pFrustumPlanes;
    uint32_t dwNumFrustumPlanes;

    int32_t iSeaTrashTexture;
    float fLastTrashTime;
    std::vector<SeaTrash> aSeaTrash;
    std::vector<RS_RECT> aTrashRects;

    int32_t iSeaLightTexture;
    std::vector<SeaLight> aSeaLights;
    float fLastLightTime;
    std::vector<RS_RECT> aLightsRects;

    std::vector<uint32_t *> aNormals;
    std::vector<uint8_t *> aBumps;
    std::vector<SeaBlock> aBlocks;
    // The grid in the buffers: the blocks it was built from (iTX, iTY, iSize, iLOD each), its tree's place and step.
    // Rebuilt only when one of them changes (BuildMesh).
    std::vector<int32_t> aMeshBlocks;
    glm::vec3 vMeshTree{};
    float fMeshGridStep = 0.0f;
    bool bMeshBuilt = false;
    // The height field the vertex shader reads (sea_gpu::pack_field), D3DVERTEXTEXTURESAMPLER0
    int32_t iFieldTexture = -1;

    glm::vec4 v4SeaColor, v4SkyColor, v4SeaParameters;


    // A wave's two frames and the blend between them this frame: a texel's height (0..1, the bytes / 255) and normal
    // (x, z of aNormals), blended as WaveXZ samples them.
    struct FrameBlend
    {
        const uint8_t *h1 = nullptr, *h2 = nullptr;
        const uint32_t *n1 = nullptr, *n2 = nullptr;
        float delta = 0.0f;

        float Height(uint32_t texel) const
        {
            const float f1 = h1[texel], f2 = h2[texel];
            return (1.0f / 255.0f) * (f1 + (f2 - f1) * delta);
        }
        void Normal(uint32_t texel, float &nx, float &nz) const
        {
            const float nx1 = static_cast<float>(static_cast<short>(n1[texel])) / 32767.5f;
            const float nx2 = static_cast<float>(static_cast<short>(n2[texel])) / 32767.5f;
            const float nz1 = static_cast<float>(static_cast<short>(n1[texel] >> 0x10)) / 32767.5f;
            const float nz2 = static_cast<float>(static_cast<short>(n2[texel] >> 0x10)) / 32767.5f;
            nx = nx1 + (nx2 - nx1) * delta;
            nz = nz1 + (nz2 - nz1) * delta;
        }
    };
    FrameBlend waveBlend_[2];
    FrameBlend WaveBlend(float fFrame) const;

    glm::vec3 vMove1, vMove2;
    float fFrame1, fFrame2;
    float fScale1, fScale2;
    float fAmp1, fAmp2;
    float _fAmp1, _fAmp2;
    float fAnimSpeed1, fAnimSpeed2;
    float fPosShift;
    float fFrenel;

    bool bStop;
    // Waves from objects: what the Execute stage put on the water, handed to the renderer when the sea draws.
    std::vector<storm::render::EffectsWaveSource> waveSources_;
    storm::sea::WaveFocus waveFocus_;

    float fFogSeaDensity, fFogStartDistance;
    glm::vec3 vFogColor;
    bool bFogEnable;

    bool bTempFullMode;
    float fTempGridStep, fTempLodScale;

    bool bIniFoamEnable, bFoamEnable;
    float fFoamV, fFoamK, fFoamUV, fFoamTextureDisturb;
    int32_t iFoamTexture;

    glm::mat4 mTexProjection;

    bool HeightField(SeaHeightField &field) const override;
    float WaveXZ(float x, float z, glm::vec3 *pNormal = nullptr) override;
    void AddWaveSource(const storm::render::EffectsWaveSource &source) override;

    void AddBlock(int32_t iTX, int32_t iTY, int32_t iSize, int32_t iLOD);
    uint32_t TreeDim() const;
    void PlaceTree();
    void BuildTree(int32_t iTX, int32_t iTY, int32_t iLev);

    bool isVisibleBBox(const glm::vec3 &vCenter, const glm::vec3 &v1, const glm::vec3 &v2);
    void CalculateLOD(const glm::vec3 &v1, const glm::vec3 &v2, int32_t &iMaxLOD, int32_t &iMinLOD);
    inline float CalcLod(const float &x, const float &y, const float &z);
    void FitBlocksToBuffer();
    void PrepareIndicesForBlock(uint32_t dwBlockIndex);
    void WriteBlockVertices(const SeaBlock &block, SeaVertex *pVertices) const;
    bool BuildMesh();
    glm::dvec2 MeshOrigin() const;
    void BuildFieldTexture();

    int32_t VisCode(const glm::vec3 &vP);


    bool SunRoad_Render2();
    bool EnvMap_Render2();
    bool SunRoad_Render();
    bool EnvMap_Render();

    // Environment and sun-road cube faces rendered this frame (bit per D3DCUBEMAP_FACES): all of them after the
    // entity starts, a camera jump, a switch above/below the surface or the sea coming back into view, otherwise
    // round-robin at kCubeFacesPerSecond by the clock (5 faces above the surface: a full refresh every 83 ms, at
    // 30 fps two faces a frame as before, at 60 fps one: the faces' cost no longer doubles with the frame rate).
    // None while no sea block is in view (the frame before drew none: nothing reads the cubes; in a town the two
    // cubes' faces drew more triangles than the main view). While a place loads: every face in its first
    // kCubeLoadingFullFrames frames, then the same round-robin (every face of both cubes in every frame of a
    // ten-second wait for the place's products was 5.5 ms of GPU a frame on a phone, under the loading picture).
    // The fx water reads the same cube (env_cube).
    static constexpr float kCubeFacesPerSecond = 60.0f;
    static constexpr uint32_t kCubeLoadingFullFrames = 2;
    static constexpr float kCubeJump = 10.0f; // camera movement in one frame that refreshes every face
    uint32_t CubeFacesDue();
    uint32_t cubeFaces_ = 0;
    uint32_t cubeCursor_ = 0;
    uint32_t cubeLoadingFrames_ = 0;
    float cubeDue_ = 0.0f; // faces owed by the clock
    std::chrono::steady_clock::time_point cubeTime_{};
    bool cubeSeaInView_ = true; // the last frame drew sea blocks
    bool cubeRefreshAll_ = true;
    bool cubeUnderSea_ = false;
    glm::vec3 cubeCamPos_{};
    void EnvMap_GetSideMatrix(D3DCUBEMAP_FACES Face, glm::mat4 &mView);

    void SFLB_CreateBuffers();
    void CreateVertexDeclaration();

  public:
    SEA();
    ~SEA() override;

    bool Init() override;
    void BuildVolumeTexture();
    void Realize(uint32_t dwDeltaTime);

    float Trace(const glm::vec3 &vSrc, const glm::vec3 &vDst) override;
    float Cannon_Trace(int32_t iBallOwner, const glm::vec3 &src, const glm::vec3 &dst) override;

    bool Clip(const PLANE *planes, int32_t nplanes, const glm::vec3 &center, float radius, ADD_POLYGON_FUNC addpoly) override
    {
        return false;
    }

    const char *GetCollideMaterialName() override
    {
        return nullptr;
    };

    bool GetCollideTriangle(TRIANGLE &triangle) override
    {
        return false;
    }

    bool EditMode_Update();

    uint32_t AttributeChanged(ATTRIBUTES *pAttribute) override;

    void ProcessStage(Stage stage, uint32_t delta) override
    {
        switch (stage)
        {
            // case Stage::execute:
            //    Execute(delta); break;
        case Stage::loading_realize:
            Realize(0);
            break;
        case Stage::realize:
            Realize(delta);
            break;
        case Stage::lost_render:
            LostRender();
            break;
        case Stage::restore_render:
            RestoreRender();
            break;
        }
    }

    void LostRender();
    void RestoreRender();
};
