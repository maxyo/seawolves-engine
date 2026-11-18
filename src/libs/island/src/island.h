#pragma once

#include "ai_flow_graph.h"
#include "island_base.h"
#include "collide.h"
#include "renderer.h"
#include "geometry.h"
#include "model.h"
#include "vma.hpp"
#include "map_zipper.h"

class ISLAND : public ISLAND_BASE
{
  private:
    // The zipped map of a game map file (<name>.tga): shipped next to it (<name>.tga.zap), else made from the tga into
    // the cache and read from there, logged. The game folder is never written.
    static std::filesystem::path ZapCache(const std::string &gameFile);
    static bool LoadZap(MapZipper &zipper, const std::string &gameFile);
    static void SaveZap(MapZipper &zipper, const std::string &gameFile);

    std::string sIslandName;
    std::vector<entid_t> aSpheres;
    std::vector<entid_t> aForts;
    AIFlowGraph AIPath;
    entid_t AIFortEID{};

    FRECT rIsland;
    bool bForeignModels;
    bool bDrawReflections;
    float fStepDX, fStepDZ, fStep1divDX, fStep1divDZ;
    float fShadowMapSize, fShadowMapStep;
    glm::vec3 vBoxSize, vBoxCenter, vRealBoxSize;
    uint32_t iDMapSize, iDMapSizeShift;
    entid_t model_id, seabed_id;

    bool bFirstRealize;
    bool dynamicLightsOn; 		//dynamic lighting

    std::string cModelsDir, cModelsID, cFoamDir;

    float fDepthHeight[256];

    MapZipper mzShadow, mzDepth;

    uint8_t *pDepthMap;
    uint8_t *pShadowMap;

    VRenderer *pRS;
    VGEOMETRY *pGS;
    COLLIDE *pCollide;

    glm::mat4 mIslandOld, mSeaBedOld;
    float fImmersionDepth, fImmersionDistance;
    float fCurrentImmersion;

    float GetShadowTemp(int32_t iX, int32_t iZ);

    // depth map section
    bool CreateHeightMap(const std::string_view &pDir, const std::string_view &pName);
    bool ActivateCamomileTrace(glm::vec3 &vSrc);
    inline float GetDepthCheck(uint32_t iX, uint32_t iZ);
    inline float GetDepthNoCheck(uint32_t iX, uint32_t iZ);

    bool Mount(const std::string_view &fname, const std::string_view &fdir, entid_t *eID);
    void Uninit();

    void CalcBoxParameters(glm::vec3 &vBoxCenter, glm::vec3 &vBoxSize);

    void SetName(const std::string_view &pIslandName)
    {
        sIslandName = pIslandName;
    };

    char *GetName()
    {
        return (char *)sIslandName.c_str();
    };

    void AddLocationModel(entid_t eid, const std::string_view &pIDStr, const std::string_view &pStr);

    // debug
    // dead code

  public:
    ISLAND();
    ~ISLAND();
    bool Init();
    void Realize(uint32_t Delta_Time);
    uint64_t ProcessMessage(MESSAGE &message);

    void ProcessStage(Stage stage, uint32_t delta) override
    {
        switch (stage)
        {
        case Stage::loading_realize:
            Realize(0);
            break;
        case Stage::realize:
            Realize(delta);
            break;
        }
    }

    void Move();
    void SetDevice();

    // inherit functions COLLISION_OBJECT
    float Trace(const glm::vec3 &src, const glm::vec3 &dst);

    bool Clip(const PLANE *planes, int32_t nplanes, const glm::vec3 &center, float radius, ADD_POLYGON_FUNC addpoly)
    {
        return false;
    };

    const char *GetCollideMaterialName()
    {
        return nullptr;
    };

    bool GetCollideTriangle(TRIANGLE &triangle)
    {
        return false;
    };

    // inherit functions CANNON_TRACE_BASE
    float Cannon_Trace(int32_t iBallOwner, const glm::vec3 &src, const glm::vec3 &dst);

    // inherit functions ISLAND_BASE
    bool GetMovePoint(glm::vec3 &vSrc, glm::vec3 &vDst, glm::vec3 &vRes);

    entid_t GetModelEID()
    {
        return model_id;
    };

    entid_t GetSeabedEID()
    {
        return seabed_id;
    };

    bool Check2DBoxDepth(glm::vec3 vPos, glm::vec3 vSize, float fAngY, float fMinDepth);

    bool OverheadContact(float x, float z, float fHeight);
    bool GetDepth(float x, float z, float *fRes = nullptr);
    bool GetLandMap(IslandLandMap &out) override;
    bool GetDepthFast(float x, float z, float *fRes = nullptr);
    bool GetDepth(FRECT *pRect, float *fMinH, float *fMaxH);

    bool GetShadow(float x, float z, float *fRes = nullptr);

    float GetCurrentImmersion()
    {
        return fCurrentImmersion;
    };
};
