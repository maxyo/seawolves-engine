// Dynamic internal resolution: the 3D scene's height follows what the device can draw in the frame.
//
// The scene draws at the preset's internal height and FSR 1 (EASU, RCAS) upscales it to the display
// (fx::Settings::render_height). A heavy scene or a throttled GPU that misses the frame is not paced any more: its
// frames alternate between two and three display refreshes, which is the stutter the owner's phone showed in lamp-lit
// interiors (GPU 22-42 ms for a 33.3 ms frame, thermal status SEVERE). This controller lowers the height in steps
// until the frame fits and raises it again when there is room:
//
//   * the ladder: the preset's height x 1.0, 0.9, 0.8, 0.7 (kSteps; never under kLowestHeight lines), so a preset
//     keeps its look and loses at most half of its scene pixels;
//   * the load of a window of scene frames (kWindowFrames, at least kWindowMs): with GPU timer queries the mean GPU
//     frame time x kGpuMargin over the frame's interval; without them (some Mali) the share of late frames;
//   * down: the load does not fit. With timers the step is the highest one whose pixels are predicted to fit (cost
//     taken as proportional to the pixels: an overestimate of the gain, so another window may step again); without
//     them one step. Not when the game thread itself fills the frame (kCpuBound of the interval): fewer pixels do not
//     help a CPU-bound frame, said once in the log;
//   * heat: at thermal status SEVERE one step down every kSevereStepMs while it lasts (the platform throttles and
//     vendors stop the app); no step up while the load is elevated or severe;
//   * up: with timers, after kUpWindows windows in a row whose load predicted for the next step stays under kUpLoad;
//     without them, after a probe interval of fitting windows. A step up that had to be taken back within
//     kRegretMs doubles the wait for the next one (up to kMaxUpWaitMs): a scene on the edge does not flicker
//     between two sizes;
//   * a change rebuilds the scene targets (one slow frame), so changes are at least kMinChangeMs apart and the frames
//     right after one are not judged (kSettleFrames).
// Loading and interface frames are not judged. Without a frame interval to fit (the host, no pacing) it stays at the
// preset's height. Every change is logged by the caller with reason().
//
// Header-only and free of the engine: tests/dynamic_resolution runs it as it is.
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>

namespace storm::effects
{

class DynamicResolution
{
  public:
    static constexpr std::array<float, 4> kSteps = {1.0f, 0.9f, 0.8f, 0.7f};
    static constexpr uint16_t kLowestHeight = 540;
    static constexpr uint32_t kWindowFrames = 45;
    static constexpr double kWindowMs = 1500.0;
    static constexpr double kGpuMargin = 1.15; // timer queries end before the tile resolve and the present
    static constexpr double kFitLoad = 0.92;   // what a step down aims at
    static constexpr double kUpLoad = 0.85;    // predicted for the next step up
    static constexpr uint32_t kUpWindows = 6;
    static constexpr double kLateFrame = 1.25; // of the interval: a frame that missed its slot
    static constexpr double kLateShare = 0.2;  // of a window's frames (no timer queries)
    static constexpr double kCpuBound = 0.85;  // game thread work of the interval
    static constexpr uint32_t kSettleFrames = 20;
    static constexpr double kMinChangeMs = 3000.0;
    static constexpr double kSevereStepMs = 10000.0;
    static constexpr double kUpWaitMs = 20000.0; // the first wait for a step up
    static constexpr double kRegretMs = 15000.0;
    static constexpr double kMaxUpWaitMs = 480000.0;

    enum class Heat : uint8_t
    {
        Normal,
        Elevated,
        Severe,
    };

    struct Frame
    {
        double now_ms = 0.0;      // a steady clock
        double interval_ms = 0.0; // since the previous presented frame
        double gpu_ms = -1.0;     // the GPU's frame time; < 0: no timer queries
        double work_ms = 0.0;     // the game thread's work of the frame (platform/frame_work.hpp)
        double target_ms = 0.0;   // the paced interval the frames should fit; 0: none
        bool scene = false;       // a 3D scene was drawn and no place is loading
        Heat heat = Heat::Normal;
    };

    // The preset's internal height (0: the display's, nothing to lower). Another height starts the ladder again.
    void reset(uint16_t base_height)
    {
        if (base_height == base_)
            return;
        base_ = base_height;
        step_ = 0;
        up_wait_ms_ = kUpWaitMs;
        restart(0.0);
        changed_ms_ = raised_ms_ = severe_ms_ = -1e12;
        reason_.clear();
    }

    // The height of a step: the base's share, in whole even lines, never under kLowestHeight or above the base.
    uint16_t height(size_t step) const
    {
        if (base_ == 0)
            return 0;
        const auto lines = static_cast<uint16_t>(std::lround(base_ * kSteps[std::min(step, kSteps.size() - 1)] / 2.0f) * 2);
        return std::min(base_, std::max(kLowestHeight, lines));
    }
    uint16_t height() const { return height(step_); }
    uint16_t lowest() const { return height(kSteps.size() - 1); }
    size_t step() const { return step_; }
    // Why the last change was made, for the log.
    const std::string &reason() const { return reason_; }
    // True once when late frames were left alone because the game thread fills the frame (no GPU timers), for the log.
    bool take_cpu_bound() { return std::exchange(cpu_bound_pending_, false); }

    // Every presented frame. True when height() changed.
    bool frame(const Frame &f)
    {
        if (base_ == 0 || f.target_ms <= 0.0 || !f.scene)
        {
            restart(f.now_ms); // a loading or interface screen, or nothing to fit: its frames say nothing of the scene
            return false;
        }
        if (settle_ > 0)
        {
            --settle_;
            window_start_ms_ = f.now_ms;
            return false;
        }
        if (frames_ == 0)
            window_start_ms_ = f.now_ms - f.interval_ms;
        ++frames_;
        interval_sum_ += f.interval_ms;
        work_sum_ += f.work_ms;
        late_ += f.interval_ms > f.target_ms * kLateFrame;
        if (f.gpu_ms >= 0.0)
        {
            gpu_sum_ += f.gpu_ms;
            ++gpu_frames_;
        }
        if (frames_ < kWindowFrames || f.now_ms - window_start_ms_ < kWindowMs)
            return false;

        const double frames = frames_;
        const bool timers = gpu_frames_ * 2 >= frames_;
        const double gpu = timers ? gpu_sum_ / gpu_frames_ : -1.0;
        const double load = timers ? gpu * kGpuMargin / f.target_ms : 0.0;
        const double late = late_ / frames;
        const double work = work_sum_ / frames;
        const bool cpu_bound = work >= f.target_ms * kCpuBound;
        const bool misses = timers ? load > 1.0 : late > kLateShare;
        restart(f.now_ms);

        const size_t last = kSteps.size() - 1;
        const bool may_change = f.now_ms - changed_ms_ >= kMinChangeMs;
        size_t next = step_;
        char why[200] = "";
        if (f.heat == Heat::Severe && step_ < last && f.now_ms - severe_ms_ >= kSevereStepMs && may_change)
        {
            next = step_ + 1;
            severe_ms_ = f.now_ms;
            snprintf(why, sizeof(why), "thermal status severe");
        }
        else if (misses && step_ < last && may_change)
        {
            if (cpu_bound && !timers)
            {
                // Late frames with the game thread filling the frame: the pixels are not what is slow.
                cpu_bound_pending_ = cpu_bound_pending_ || !cpu_bound_said_;
                cpu_bound_said_ = true;
                fits_ = 0;
                return false;
            }
            next = step_ + 1;
            if (timers)
            {
                // The highest step whose pixels are predicted to fit.
                const double pixels = kFitLoad / load, now = double(height(step_)) * height(step_);
                while (next < last && double(height(next)) * height(next) > now * pixels)
                    ++next;
                snprintf(why, sizeof(why), "GPU %.1f ms of the %.1f ms frame", gpu, f.target_ms);
            }
            else
                snprintf(why, sizeof(why), "%.0f%% of the frames miss the %.1f ms frame (no GPU timers)", late * 100.0,
                         f.target_ms);
        }
        else if (!misses && step_ > 0 && f.heat == Heat::Normal)
        {
            const double up = double(height(step_ - 1)) * height(step_ - 1) / (double(height(step_)) * height(step_));
            const bool room = timers ? load * up < kUpLoad : late == 0.0;
            fits_ = room ? fits_ + 1 : 0;
            if (fits_ >= kUpWindows && f.now_ms - changed_ms_ >= up_wait_ms_)
            {
                next = step_ - 1;
                if (timers)
                    snprintf(why, sizeof(why), "GPU %.1f ms of the %.1f ms frame leaves room", gpu, f.target_ms);
                else
                    snprintf(why, sizeof(why), "the frames fit the %.1f ms frame", f.target_ms);
            }
        }
        else
            fits_ = 0;
        if (next == step_ || height(next) == height(step_))
            return false;
        if (next > step_ && f.now_ms - raised_ms_ < kRegretMs)
            up_wait_ms_ = std::min(up_wait_ms_ * 2.0, kMaxUpWaitMs); // the last step up did not hold
        else if (next > step_)
            up_wait_ms_ = kUpWaitMs;
        if (next < step_)
            raised_ms_ = f.now_ms;
        step_ = next;
        changed_ms_ = f.now_ms;
        settle_ = kSettleFrames;
        fits_ = 0;
        cpu_bound_said_ = false;
        reason_ = why;
        return true;
    }

  private:
    void restart(double now_ms)
    {
        frames_ = gpu_frames_ = late_ = 0;
        interval_sum_ = work_sum_ = gpu_sum_ = 0.0;
        window_start_ms_ = now_ms;
    }

    uint16_t base_ = 0;
    size_t step_ = 0;
    uint32_t frames_ = 0, gpu_frames_ = 0, late_ = 0, settle_ = 0, fits_ = 0;
    double interval_sum_ = 0.0, work_sum_ = 0.0, gpu_sum_ = 0.0, window_start_ms_ = 0.0;
    double changed_ms_ = -1e12, raised_ms_ = -1e12, severe_ms_ = -1e12, up_wait_ms_ = kUpWaitMs;
    bool cpu_bound_said_ = false, cpu_bound_pending_ = false;
    std::string reason_;
};

} // namespace storm::effects
