//    Spirenkov Maxim aka Sp-Max Shaman, 2001
//
//    CameraFollow

#pragma once

#include "storm_glm.h"
#include "model.h"
#ifdef __ANDROID__
#include <platform/android_camera.hpp>
#endif

class LocationCamera;

class CameraFollow
{
    struct EdgeInfo
    {
        glm::vec3 n; // Normal to plane
        float d;   // Distance to the plane
        glm::vec3 s; // Start of the edge
        glm::vec3 e; // End of the edge
    };

  public:
    CameraFollow();
    virtual ~CameraFollow();
    void SetLocationCamera(LocationCamera *locCamera);

  public:
    // Make calculations
    void Update(float dltTime);

    // Find current camera position
    void MoveCamera(float dltTime);
#ifdef __ANDROID__
    // The mobile camera mode of MoveCamera (out of a fight).
    void MoveMobile(float dltTime);
#endif
    // Reinitialize camera position
    void BornCamera();
    // Calculate camera position for a given angle
    void CalcPosition(float ang, float radius, float dax, glm::vec3 &pos) const;
    // Estimate the radius in the desired direction for 1 sec
    float FindRadius(float curAng, float pitch) const;
    // Consider polygon in radius search
    static bool ApplyPoly(const glm::vec3 *v, int32_t n);
    static void ApplyPolyFindNearest(EdgeInfo *e, int32_t ne, const glm::vec3 &pos);
    // Change radius
    void ChangeRadius(float dltTime, float radius);

    void DrawDebug();

    // private:
  public:
    LocationCamera *lc;
    // FindRadius's clip of the location's still models in the last frames, one per direction asked (the camera's own
    // and a tight space's probes, tightView): a pure function of these inputs.
    struct StillRadius
    {
        bool valid = false;
        const void *location = nullptr;
        glm::vec3 pos{0.0f};
        float ang = 0.0f, ax = 0.0f, radius = 0.0f, found = 0.0f;
        uint64_t geometry = 0;
    };
    mutable StillRadius stillRadius_[48];
    mutable size_t stillNext_ = 0;

    glm::vec3 camPos, lookTo;
    // Camera following the character
    float camay;        // Current camera direction
    float camradius;    // Current radius
    bool isBrn;         // If set, then need to respawn the camera
    float rotSpd;       // Camera movement speed at the time of stop
    float rotAcc;       // Stop acceleration
    float kRadInert;    // Dependence of the inertia of the radius change on the turning speed
    float kRadInertCur; // Current dependence of inertia
#ifdef __ANDROID__
    storm::android::AutoFollow autoFollow;
    storm::android::Damped raise; // how much higher (lower: below 0) than the player's pitch the camera is now
    storm::android::Damped swing; // the yaw still to swing out of a tight space (value), easing to 0
    float tightRaise = 0.0f;      // the raise a tight space has chosen (0: the player's pitch)
    float tightAge = 0.0f;        // seconds until a tight view with too little room is looked for again
    bool settle_ = false;         // set on (re)birth: the next frame takes the view at once
    glm::vec3 heroPos{0.0f};  // where the hero was in the last frame (AutoFollow: does he move)
#endif

    static float fndRadius;     // Current radius when searching
    static float fndMaxRadius;  // Maximum search radius
    static glm::vec3 fndCamDir;   // Normalized camera heading
    static glm::vec3 fndCamDirXZ; // Normalized heading to camera in XZ
    static glm::vec3 fndCamDirY;  // Normalized camera direction in Y
    static glm::vec3 fndCamPos;   // Observation point
    static float fndcsAx;       // Cosine of the bounding angle in x
    static float fndkAx;        // 1/(1 - cos(ax))
    static float fndcsAy;       // Cosine of the y bounding angle
    static float fndkAy;        // 1/(1 - cos(ay))
    static PLANE fndPlanes[32]; // Temporarily used face planes
};
