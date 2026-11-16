#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#ifdef __ANDROID__
#include <SDL2/SDL.h>
#include <SDL2/SDL_system.h>
#include <jni.h>
#endif

// «Вибрация» (settings screen «Экран и касания», app setting haptics): short effects for what the player feels
// in a fight. The engine names the moment, the application plays it (GameActivity.onHaptic: the view's haptic
// feedback, which follows the system's touch-feedback setting). A moment repeated sooner than its gap is dropped
// (a broadside's balls hitting one after another are one hit).
namespace storm::android
{
enum class Haptic : uint8_t
{
    Hit,       // the hero or his ship is hit
    Block,     // the hero blocks or parries a blow
    Broadside, // the player's ship fires a side
    Boarding,  // a boarding fight begins
    Count,
};

inline const char *HapticName(Haptic kind)
{
    switch (kind)
    {
    case Haptic::Hit: return "hit";
    case Haptic::Block: return "block";
    case Haptic::Broadside: return "broadside";
    case Haptic::Boarding: return "boarding";
    default: return "";
    }
}

// The shortest time between two effects of one kind, in ms.
inline constexpr std::array<uint32_t, static_cast<std::size_t>(Haptic::Count)> kHapticGapMs = {250, 150, 600, 3000};

class HapticThrottle
{
  public:
    // True when an effect of `kind` may play at `now_ms` (and it is then counted as played).
    bool Allow(Haptic kind, uint64_t now_ms)
    {
        const auto index = static_cast<std::size_t>(kind);
        if (index >= last_.size())
            return false;
        if (played_[index] && now_ms - last_[index] < kHapticGapMs[index])
            return false;
        played_[index] = true;
        last_[index] = now_ms;
        return true;
    }

  private:
    std::array<uint64_t, static_cast<std::size_t>(Haptic::Count)> last_{};
    std::array<bool, static_cast<std::size_t>(Haptic::Count)> played_{};
};

// Game thread. Off when the player switched «Вибрация» off (the app setting haptics; read from the app each time).
inline void haptic(Haptic kind)
{
#ifdef __ANDROID__
    static HapticThrottle throttle;
    if (!throttle.Allow(kind, SDL_GetTicks64()))
        return;
    auto *env = static_cast<JNIEnv *>(SDL_AndroidGetJNIEnv());
    auto activity = static_cast<jobject>(SDL_AndroidGetActivity());
    if (!env || !activity)
        return;
    auto type = env->GetObjectClass(activity);
    jmethodID setting = type ? env->GetMethodID(type, "getGraphicsSetting", "(Ljava/lang/String;I)I") : nullptr;
    jmethodID play = type ? env->GetMethodID(type, "onHaptic", "(Ljava/lang/String;)V") : nullptr;
    jstring key = env->NewStringUTF("haptics");
    const bool on = setting && key && env->CallIntMethod(activity, setting, key, static_cast<jint>(1)) != 0;
    if (on && play)
    {
        jstring name = env->NewStringUTF(HapticName(kind));
        env->CallVoidMethod(activity, play, name);
        if (name)
            env->DeleteLocalRef(name);
    }
    if (env->ExceptionCheck())
    {
        env->ExceptionDescribe();
        env->ExceptionClear();
    }
    if (key)
        env->DeleteLocalRef(key);
    if (type)
        env->DeleteLocalRef(type);
    env->DeleteLocalRef(activity);
#else
    (void)kind;
#endif
}
} // namespace storm::android
