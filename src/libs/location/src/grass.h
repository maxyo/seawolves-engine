//    Spirenkov Maxim aka Sp-Max Shaman, 2001
//
//    Grass

#pragma once

#include "supervisor.h"
#include "renderer.h"
#include "grs.h"
#include "grass_pack.hpp"
#include "vma.hpp"

#define MSG_GRASS_LOAD_DATA 40666
#define MSG_GRASS_SET_TEXTURE 41666
#define MSG_GRASS_SET_PARAM 42666

class GEOS;

class Character;

class Grass : public Entity
{
    storm::render::VertexDeclarationId vertexDecl_;

#pragma pack(push, 1)

    using Vertex = grass_pack::Vertex; // a corner of a blade in the buffer (grass_pack.hpp, vs_port_grass.sc)

    struct VSConstant
    {
        VSConstant()
        {
            x = y = z = 0.0f;
            w = 1.0f;
        };

        VSConstant(float _x, float _y, float _z, float _w)
        {
            x = _x;
            y = _y;
            z = _z;
            w = _w;
        };
        float x, y, z, w;
    };

#pragma pack(pop)

    struct AngleInfo
    {
        float dx, dz; // Direction
        float cs;     // Cosine of the angle between direction and source
    };

    struct GRSMapElementEx
    {
        float x, y, z; // The position of a blade of grass in the world
        union {
            struct
            {
                uint8_t frame; // Frame
                uint8_t h;     // Height
                uint8_t w;     // Width
                uint8_t ang;   // Angle of rotation
            };

            uint32_t data;
        };
    };

    enum RenderQuality
    {
        rq_full = 0,
        rq_middle = 1,
        rq_low = 2,
        rq_off = 3,
    };

  public:
    struct CharacterPos
    {
        glm::vec3 pos;     // Current position
        glm::vec3 lastPos; // Inertial position
        Character *chr;  // Character pointer
        int32_t useCounter; // Grass Influence Counter
    };

  public:
    Grass();
    ~Grass() override;

    bool Init() override;
    void Execute(uint32_t delta_time);
    void Realize(uint32_t delta_time);
    void PrepareLoadingPose();
    uint64_t ProcessMessage(MESSAGE &message) override;

    void RestoreRender();

    void ProcessStage(Stage stage, uint32_t delta) override
    {
        switch (stage)
        {
        case Stage::execute:
            Execute(delta);
            break;
        case Stage::loading_prepare:
            PrepareLoadingPose();
            break;
        case Stage::loading_realize:
            Realize(0);
            break;
        case Stage::realize:
            Realize(delta);
            break;
        case Stage::restore_render:
            RestoreRender();
            break;
        }
    }

    // Load data for grass
    bool LoadData(const char *patchName);
    // Set texture
    void SetTexture(const char *texName);

    std::vector<CharacterPos> characters;

  private:
    // Box visibility check
    bool VisibleTest(const PLANE *plane, int32_t numPlanes, const glm::vec3 &min, const glm::vec3 &max);
    // The base heights of a new map (LoadData); the buffer is filled by UpdateWindow
    void PrepareHeights();
    // The blades of the blocks around the camera's block into the buffer, when the camera left the current window
    void UpdateWindow(int32_t camx, int32_t camz);
    // The block's LOD factor (0..1 between the LOD and visible distances) at a distance from the camera
    float BlockKLod(float distance) const;
    // The indices of the blades the camera can see from its cell in its direction bucket, when either changed
    void UpdateVisible(const glm::vec3 &camPos, const glm::mat4 &view, const glm::mat4 &prj);
    // The characters' contacts with the blades drawn this frame: the grass sounds and the characters bending it
    void CharacterContacts(const glm::vec3 &camPos, const PLANE *plane, int32_t numPlanes);
    // The per-frame constants c42.. and the draw of the visible set
    void DrawVisible(const glm::vec3 &camPos);
    // Vertex declaration
    void CreateVertexDeclaration();

  private:
    // Render service
    VRenderer *rs;
    // Buffers: the blades of a window of blocks around the camera, 4 vertices each (UpdateWindow); the indices of the
    // visible set's blades (UpdateVisible)
    int32_t vb, ib;
    int32_t vbBlades = 0;                                        // capacity of vb
    int32_t ibBlades = 0;                                        // capacity of ib
    grass_pack::Window window;                                   // the blocks in vb
    int32_t windowSerial = 0;                                    // counts the windows vb held
    std::vector<int32_t> windowStart;                            // first blade of each window block in vb
    bool reachLogged = false;

    // The visible set: built for the camera's cell and direction bucket, the projection, the window and the
    // distances; drawn from as long as they stay the same
    struct VisibleKey
    {
        int32_t cellX = 0, cellY = 0, cellZ = 0, yaw = 0, pitch = 0;
        int32_t tanX = 0, tanY = 0; // the projection's half-angle tangents in 1/64, rounded up
        int32_t windowSerial = -1, quality = -1;
        float minDist = 0.0f, maxDist = 0.0f, minLod = 0.0f, scale = 0.0f;
        bool operator==(const VisibleKey &) const = default;
    };
    VisibleKey visibleKey;
    bool visibleValid = false;
    int32_t visibleBlades = 0;          // blades in ib
    std::vector<uint32_t> visibleIndices; // their indices, as written to ib
    std::vector<int32_t> nearCharacters; // characters touching a drawn blade this frame (CharacterContacts)

    // The graphics preset's share of the game's distances (kPresetDistance in grass.cpp)
    int32_t preset = 2;
    float presetSeconds = 0.0f;
    float minDist = 0.0f, maxDist = 0.0f; // this frame's LOD and visible distance (Realize)
    // Texture
    int32_t texture;

    // Minimap
    GRSMiniMapElement *miniMap;
    // Minimap sizes
    int32_t miniX, miniZ;
    // Starting position of the map
    float startX, startZ;
    // Blocks with heights
    GRSMapElementEx *block;
    int32_t numElements;
    // Current parameters for grass turns
    AngleInfo angInfo[16];
    // Current swing phases
    float phase[7];
    // Current light source options
    glm::vec3 lDir;   // Direction of the source
    glm::vec3 lColor; // Source color
    glm::vec3 aColor; // Ambient light color

    float lodSelect; // Lod selection range factor (kLod = kLod^lodSelect)
    float winForce;  // Wind speed coefficient 0..1
    glm::vec3 winDir;  // Normalized wind direction

    float heightMin = 0.0f, heightStep = 1.0f; // the base heights of Vertex::height
    bool tooManyLogged = false;

    RenderQuality quality; // Rendering quality

    float cosPh1, sinPh2, sinPh5, sinPh6, winPow, winF10, kAmpWF, kDirWF, kLitWF;
    float windAng;
    int32_t initForce;

    VSConstant consts[42];

    char textureName[64];

    float m_fDataScale;
    float m_fMaxWidth;
    float m_fMaxHeight;
    float m_fMinVisibleDist;
    float m_fMaxVisibleDist;
    float m_fMinGrassLod;

    // boal grass parameters
    int32_t isGrassLightsOn;
};
