// Spirenkov Maxim aka Sp-Max Shaman, 2001
// Storm engine v2.00
// AnimationInfo
// This is where animation information is stored, loaded 1 time

#pragma once

#include "action_info.h"
#include "bone.h"
#include "storm_assert.h"
#include <limits>
#include <string>
#include <vector>

class AnimationInfo final
{
  public:
    AnimationInfo(const char *animationName);
    ~AnimationInfo();
    // Set the number of frames in the animation
    void SetNumFrames(int32_t _numFrames);
    // Create bones
    void CreateBones(int32_t numbones);
    // Create action
    ActionInfo *AddAction(const char *anctionName, int32_t startframe, int32_t endframe);
    // Set execution speed
    void SetFPS(float _fps);

    // Working with animation
  public:
    // Number of bones
    int32_t NumBones();
    // Access to the bone
    Bone &GetBone(int32_t iBone);
    // Compare with current name
    bool operator==(const char *animationName);
    // Increment reference count
    void AddRef();
    // Decrement reference count
    void RelRef();
    // Add downtime
    void AddDowntime(uint32_t dltTime);
    // Get downtime
    uint32_t GetDowntime();
    bool IsUnused() const;
    // Get animation name
    const char *GetName();
    // Find action by name
    ActionInfo *FindAction(const char *actionName);
    // Get animation time
    int32_t GetAniNumFrames();
    // Access to user data
    std::unordered_map<std::string, std::string> &GetUserData();
    // Get animation speed
    float GetFPS();
    // Per bone: 1 skin, 2 torso, 0 neither (skin_bones.hpp, Animation::GetSkinBones); empty: none found
    void SetSkinBones(std::vector<uint8_t> skin)
    {
        skinBones = std::move(skin);
    }
    const std::vector<uint8_t> &GetSkinBones() const
    {
        return skinBones;
    }

  private:
    char name[64]; // Animation name

    int32_t numFrames; // The number of frames in the entire animation
    float fps;      // Frames per second

    Bone *bone;    // Bones with animation keys
    int32_t numBones; // The number of bones in the skeleton

    std::vector<ActionInfo> actions; // Actions
    std::vector<uint8_t> skinBones;  // SetSkinBones

    int32_t refCounter; // Reference counter for this animation
    uint32_t downtime; // Unscaled frame milliseconds since the last reference was released

    std::unordered_map<std::string, std::string> userData; // User data
};

// inline

// Set the number of frames in the animation
inline void AnimationInfo::SetNumFrames(int32_t _numFrames)
{
    if (_numFrames < 0)
        _numFrames = 0;
    numFrames = _numFrames;
}

// Set execution speed
inline void AnimationInfo::SetFPS(float _fps)
{
    if (_fps < 0.0f)
        fps = 0.0f;
    if (_fps > 1000000.0f)
        fps = 1000000.0f;
    fps = _fps;
}

// Number of bones
inline int32_t AnimationInfo::NumBones()
{
    return numBones;
}

// Access to the bone
inline Bone &AnimationInfo::GetBone(int32_t iBone)
{
    Assert(iBone >= 0 && iBone < numBones);
    return bone[iBone];
}

// Increment reference count
inline void AnimationInfo::AddRef()
{
    refCounter++;
    downtime = 0;
}

// Decrement reference count
inline void AnimationInfo::RelRef()
{
    refCounter--;
    Assert(refCounter >= 0);
}

// Add downtime
inline void AnimationInfo::AddDowntime(uint32_t dltTime)
{
    if (refCounter == 0)
    {
        const auto remaining = std::numeric_limits<uint32_t>::max() - downtime;
        downtime += dltTime > remaining ? remaining : dltTime;
    }
    else
        downtime = 0;
}

// Get downtime
inline uint32_t AnimationInfo::GetDowntime()
{
    return downtime;
}

inline bool AnimationInfo::IsUnused() const
{
    return refCounter == 0;
}

// Get animation name
inline const char *AnimationInfo::GetName()
{
    return name;
}

// Get animation time
inline int32_t AnimationInfo::GetAniNumFrames()
{
    return numFrames;
}

// Access to user data
inline std::unordered_map<std::string, std::string> &AnimationInfo::GetUserData()
{
    return userData;
}

// Get animation speed
inline float AnimationInfo::GetFPS()
{
    return fps;
}
