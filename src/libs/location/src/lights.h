//    Spirenkov Maxim aka Sp-Max Shaman, 2001
//
//    Lights

#pragma once

#include <vector>

#include "collide.h"
#include "flame_flicker.hpp"
#include "light_components.hpp"
#include "renderer.h"
#include "storm/fx/aggregate_light.hpp"

class Lights : public Entity
{
    // Light source description
    struct LightType
    {
        char *name;
        D3DLIGHT9 dxLight;
        D3DCOLORVALUE color;
        float flicker;
        float flickerSlow;
        float freq;
        float freqSlow;
        float p;
        float pSlow;
        float coronaRange;
        float coronaRange2;
        float invCoronaRange;
        float coronaSize;
        int32_t corona;
    };

    // Source
    struct Light
    {
        D3DCOLORVALUE color; // Current source color
        D3DVECTOR pos;       // Source position
        storm::location::FlameFlicker flicker;
        float corona;        // Crown transparency
        // The last test of the line of sight from the camera to the lamp (Realize)
        struct
        {
            bool known = false;
            bool seen = false;
            uint32_t age = 0; // frames since the test
            D3DVECTOR lamp{};
        } ray;

        uint64_t identity = 0; // survives backing-array compaction
        int32_t type; // Source type index
    };

    // Controllable (moving) source
    struct MovingLight
    {
        int32_t id;
        int32_t light;
    };

    struct Vertex
    {
        glm::vec3 pos;
        uint32_t color;
        float u, v;
    };

    // To sort by distance
    struct lt_elem
    {
        int32_t idx;
        float dst;
    };

  public:
    Lights();
    ~Lights() override;

    bool Init() override;
    void Execute(uint32_t delta_time);
    // Drawing crowns
    void Realize(uint32_t delta_time);

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

    // Find source index
    int32_t FindLight(const char *name);
    // Add source to location
    void AddLight(int32_t index, const glm::vec3 &pos);
    // Add lantern model
    bool AddLampModel(entid_t lampModel);
    void DelAllLights();

    // Add portable source
    int32_t AddMovingLight(const char *type, const glm::vec3 &pos);
    // Put portable source in new position
    void UpdateMovingLight(int32_t id, const glm::vec3 &pos);
    // Remove portable source
    void DelMovingLight(int32_t id);

    // Set lights at pos
    void SetLightsAt(const glm::vec3 &pos);
    void UnsetLights();

    // Update source types
    void UpdateLightTypes(int32_t i);

  private:

    void PrintDebugInfo();
    void RebuildComponents();

    constexpr static auto max_d3d_lights = 8U;
    constexpr static auto max_d3d_custom_lights = max_d3d_lights - 1;

    VRenderer *rs;
    COLLIDE *collide;

    // Installed light sources
    struct
    {
        bool set;
        int32_t light;
    } lt[8];

    // Types of light sources
    std::vector<LightType> types;
    int32_t numTypes;
    int32_t maxTypes;
    // Existing lighting sources
    std::vector<Light> lights;
    uint32_t numLights;
    int32_t maxLights;
    int32_t lighter_code;

    // portable light sources
    std::vector<MovingLight> aMovingLight;
    inline static uint64_t nextLightIdentity = 0; // game thread; unique across location lifetimes/stateblock restores
    bool componentsDirty = true;
    std::vector<storm::location::LightComponent> components;
    std::shared_ptr<const storm::fx::AggregateSnapshot> aggregateSnapshot;

    // The camera of the last Realize: coronas are tested against a camera that stands still less often
    glm::vec3 lastCamera{0.0f};

    // Lantern models
    entid_t lampModels[16];
    int32_t numLampModels;

    Vertex buf[6 * 1];
};
