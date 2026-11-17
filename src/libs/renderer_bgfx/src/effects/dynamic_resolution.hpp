// Dynamic internal resolution: the 3D scene's height follows what the device can draw in the frame.
//
// The scene draws at the preset's internal height and FSR 1 (EASU, RCAS) upscales it to the display
// (fx::Settings::render_height). A heavy scene or a throttled GPU that misses the frame is not paced any more: its
// frames alternate between two and three display refreshes, which is the stutter seen in lamp-lit
// interiors (GPU 22-42 ms for a 33.3 ms frame, thermal status SEVERE). This controller lowers the height in steps
// until the frame fits and raises it again when there is room:
//
//   * the ladder: the preset's height x 1.0, 0.9, 0.8, 0.7 (kSteps; never under kLowestHeight lines), so a preset
//     keeps its look and loses at most half of its scene pixels;
//   * the load of a window of scene frames (kWindowFrames, at least kWindowMs): with GPU timer queries the mean GPU
//     frame time x kGpuMargin over the frame's interval; without them (some Mali) the share of late frames;
//   * down: the GPU's frame time is over the interval, or more than kLateShare of the frames are late (a GPU that
//     fits lowers its clock until its time nearly fills the interval, so the time with its margin is no reason by
//     itself). With timers the step is the highest one whose pixels are predicted to fit (cost
//     taken as proportional to the pixels: an overestimate of the gain, so another window may step again); without
//     them one step. Not when the game thread itself fills the frame (kCpuBound of the interval): fewer pixels do not
//     help a CPU-bound frame, said once in the log;
//   * heat: at thermal status SEVERE one step down every kSevereStepMs while it lasts (the platform throttles and
//     vendors stop the app); no step up while the load is elevated or severe;
//   * up: with timers, after kUpWindows windows in a row whose load predicted for the next step stays under kUpLoad;
//     without them, after a probe interval of fitting windows. A step up that had to be taken back within
//     kRegretMs doubles the wait for the next one (up to kMaxUpWaitMs): a scene on the edge does not flicker
//     between two sizes;
//   * the frames are judged against the interval they are paced at. When even the lowest height does not hold the
//     frame rate setting, the pacing lowers the rate (60 to 30 fps), and the height that the slower rate has room for
//     comes back (a night town was drawn at the lowest height and 30 fps for a minute). At the lowered rate
//     the GPU's clock is lowered too and its frame time says nothing of the room, so the frames are judged as without
//     timers: down when frames come late, a step up tried after the wait and taken back if it does not hold. A GPU
//     time that fits the setting's interval as it is raises nothing (the pacing returns to it). The pacing lowers
//     the rate within a second of the first late frames, before a step down can land: so once per place or
//     kMaxUpWaitMs the setting's rate is tried again (take_rate_probe) at the height its last GPU time there
//     predicts to fit, and when it does not hold, the height from before the try comes back at the lowered rate;
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
    static constexpr double kProbeMs = 4000.0; // a try of the setting's frame rate is judged after this long
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
        double target_ms = 0.0;   // the interval the frames are paced at now and should fit; 0: none
        double preferred_ms = 0.0; // the interval of the frame rate setting; shorter than target_ms when the pacing
                                   // lowered the rate because frames did not fit it; 0: the same
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
        changed_ms_ = raised_ms_ = severe_ms_ = rate_probe_ms_ = -1e12;
        fast_known_ = rate_probe_ = probing_ = false;
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
    // True once when the setting's frame rate should be tried again: the caller asks the pacing for it.
    bool take_rate_probe() { return std::exchange(rate_probe_, false); }
    // A place was loaded: what was learnt of the frame rate in the place before does not hold here.
    void new_scene()
    {
        rate_probe_ms_ = -1e12;
        fast_known_ = false;
    }

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
        const bool measured = gpu_frames_ * 2 >= frames_;
        const double gpu = measured ? gpu_sum_ / gpu_frames_ : -1.0;
        // The pacing lowered the frame rate under the setting's: see below. A GPU whose frames have twice the time
        // they need lowers its clock, and its frame time grows to fill the interval (measured on a phone: the same
        // prison 15-16 ms at 60 fps and 25-28 ms at 30 fps; a night town 26-28 ms at 880 and at 770 lines alike).
        // So at a lowered rate the GPU time says nothing of the room, and the frames are judged as without timers:
        // by the late ones, with a step up tried and taken back when it does not hold.
        const bool slow = f.preferred_ms > 0.0 && f.target_ms > f.preferred_ms * 1.2;
        const bool timers = measured && !slow;
        const size_t last = kSteps.size() - 1;
        if (timers)
        {
            // The last reading at the setting's rate: there the GPU is pressed and its time is its cost.
            fast_gpu_ms_ = gpu;
            fast_gpu_step_ = step_;
            fast_known_ = true;
        }
        if (slow != was_slow_)
        {
            // Another frame rate: what a step up that did not hold taught belongs to the rate before.
            was_slow_ = slow;
            up_wait_ms_ = kUpWaitMs;
            fits_ = 0;
            if (slow && !probing_ && f.heat == Heat::Normal && f.now_ms - rate_probe_ms_ >= kMaxUpWaitMs)
            {
                // The pacing lowered the rate, usually within a second of the first late frames and before a step
                // down could land. Once per place (new_scene) or kMaxUpWaitMs the setting's rate is tried again at
                // the height its last GPU time there predicts to fit (unknown: the lowest); not when even the
                // lowest height is predicted over the interval. The caller asks the pacing for the setting's rate
                // (take_rate_probe).
                size_t fit = last;
                bool hopeless = false;
                if (fast_known_)
                {
                    const double fast_load = fast_gpu_ms_ * kGpuMargin / f.preferred_ms;
                    const double pixels = double(height(fast_gpu_step_)) * height(fast_gpu_step_) * kFitLoad / fast_load;
                    fit = fast_gpu_step_;
                    while (fit <= last && double(height(fit)) * height(fit) > pixels)
                        ++fit;
                    hopeless = fit > last;
                }
                if (!hopeless)
                {
                    probe_from_ = step_;
                    rate_probe_ms_ = f.now_ms;
                    rate_probe_ = probing_ = true;
                    restart(f.now_ms);
                    const size_t to = std::max(step_, fit);
                    if (height(to) == height(step_))
                        return false;
                    step_ = to;
                    changed_ms_ = f.now_ms;
                    settle_ = kSettleFrames;
                    reason_ = "the frame rate was lowered: the frame rate setting is tried at the height predicted to hold it";
                    return true;
                }
            }
        }
        if (probing_ && f.now_ms - rate_probe_ms_ >= kProbeMs)
        {
            // The try of the setting's rate is over: it holds, or the pacing is at the lowered rate again (or never
            // left it) and the height from before the try comes back.
            probing_ = false;
            if (slow && probe_from_ != step_)
            {
                restart(f.now_ms);
                step_ = probe_from_;
                changed_ms_ = f.now_ms;
                settle_ = kSettleFrames;
                reason_ = "the frame rate setting does not hold at the lower height either";
                return true;
            }
        }
        const double load = timers ? gpu * kGpuMargin / f.target_ms : 0.0;
        const double late = late_ / frames;
        const bool few_late = late_ <= 1;
        const double work = work_sum_ / frames;
        const bool cpu_bound = work >= f.target_ms * kCpuBound;
        // Down: the GPU time itself is over the interval, or frames come late. Not the GPU time with its margin
        // alone: a GPU that fits lowers its clock until its time is 85-90 % of the interval, which with the margin
        // read as "does not fit" and stepped down for nothing (14.7 ms of 16.7, 29.2 of 33.3).
        const bool misses = (timers && gpu > f.target_ms) || late > kLateShare;
        restart(f.now_ms);

        const bool may_change = f.now_ms - changed_ms_ >= kMinChangeMs;
        const bool fast_fits_now = slow && measured && gpu * kGpuMargin / f.preferred_ms <= 1.0;
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
                snprintf(why, sizeof(why), "GPU %.1f ms of the %.1f ms frame, %.0f%% of the frames late", gpu, f.target_ms,
                         late * 100.0);
            }
            else
                snprintf(why, sizeof(why), "%.0f%% of the frames miss the %.1f ms frame", late * 100.0, f.target_ms);
        }
        else if (fast_fits_now)
            fits_ = 0; // the GPU fits the setting's interval as it is: the pacing returns to it, nothing to raise
        else if (!misses && step_ > 0 && f.heat == Heat::Normal)
        {
            const double up = double(height(step_ - 1)) * height(step_ - 1) / (double(height(step_)) * height(step_));
            const bool room = timers ? load * up < kUpLoad : few_late;
            fits_ = room ? fits_ + 1 : 0;
            if (fits_ >= kUpWindows && f.now_ms - changed_ms_ >= up_wait_ms_)
            {
                next = step_ - 1;
                if (timers)
                    snprintf(why, sizeof(why), "GPU %.1f ms of the %.1f ms frame leaves room", gpu, f.target_ms);
                else
                    snprintf(why, sizeof(why), "the frames hold the %.1f ms frame: a step up is tried", f.target_ms);
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
    double changed_ms_ = -1e12, raised_ms_ = -1e12, severe_ms_ = -1e12, rate_probe_ms_ = -1e12, up_wait_ms_ = kUpWaitMs;
    double fast_gpu_ms_ = 0.0;
    size_t fast_gpu_step_ = 0, probe_from_ = 0;
    bool fast_known_ = false, rate_probe_ = false, probing_ = false;
    bool cpu_bound_said_ = false, cpu_bound_pending_ = false, was_slow_ = false;
    std::string reason_;
};

} // namespace storm::effects
