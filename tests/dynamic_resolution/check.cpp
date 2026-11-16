#include "effects/dynamic_resolution.hpp"

#include <cstdio>
#include <cstdlib>
#include <vector>

using storm::effects::DynamicResolution;

static void check(bool ok, const char *what)
{
    if (!ok)
    {
        std::fprintf(stderr, "FAILED: %s\n", what);
        std::exit(1);
    }
}

// A device whose GPU time is a fixed part plus a part proportional to the scene's pixels.
struct Device
{
    DynamicResolution control;
    double now = 1000.0, target = 16.67;
    double fixed_ms = 3.0, pixel_ms = 12.0; // at the preset's height
    double work_ms = 6.0;
    bool timers = true, scene = true;
    bool pacing_follows = false; // Swappy's auto rate: 30 fps after a second of frames that miss 60, 60 after one that fit
    double paced = 0.0, missed_ms = 0.0, fitted_ms = 0.0;
    DynamicResolution::Heat heat = DynamicResolution::Heat::Normal;
    std::vector<uint16_t> changes;

    double gpu() const
    {
        const double share = double(control.height()) / control.height(0);
        return fixed_ms + pixel_ms * share * share;
    }
    // Runs `seconds`; an unpaced frame takes the GPU's time in whole 60 Hz refreshes, at least the target.
    void run(double seconds)
    {
        const double end = now + seconds * 1000.0;
        while (now < end)
        {
            if (paced == 0.0)
                paced = target;
            const double interval = std::max(paced, std::ceil(gpu() * 1.15 / 16.67 - 1e-9) * 16.67);
            now += interval;
            if (pacing_follows)
            {
                const bool fits = gpu() * 1.15 <= target;
                missed_ms = fits ? 0.0 : missed_ms + interval;
                fitted_ms = fits ? fitted_ms + interval : 0.0;
                if (missed_ms > 1000.0)
                    paced = 2 * target;
                if (fitted_ms > 1000.0)
                    paced = target;
            }
            DynamicResolution::Frame f;
            f.now_ms = now;
            f.interval_ms = interval;
            f.gpu_ms = timers ? gpu() : -1.0;
            f.work_ms = work_ms;
            f.target_ms = paced;
            f.preferred_ms = target;
            f.scene = scene;
            f.heat = heat;
            if (control.frame(f))
                changes.push_back(control.height());
        }
    }
};

int main()
{
    {
        DynamicResolution ladder;
        ladder.reset(1100);
        check(ladder.height(0) == 1100 && ladder.height(1) == 990 && ladder.height(2) == 880 && ladder.height(3) == 770,
              "the ladder of «Высокая»: 1100, 990, 880, 770");
        ladder.reset(720);
        check(ladder.height(3) == 540 && ladder.lowest() == 540, "never under 540 lines");
        ladder.reset(0);
        check(ladder.height() == 0, "a preset at the display's height has nothing to lower");
    }
    {
        Device d; // 15 ms of GPU for a 16.67 ms frame: 1.03 with the margin, does not fit
        d.control.reset(1100);
        d.run(1.0);
        check(d.changes.empty(), "nothing is decided before a window of frames");
        d.run(4.0);
        check(d.changes.size() == 1 && d.changes[0] == 990, "a scene just over the frame goes one step down");
        d.run(120.0);
        check(d.changes.size() == 1, "and stays: the step up is predicted not to fit (no flicker between two sizes)");
        d.pixel_ms = 6.0; // a lighter scene
        d.run(40.0);
        check(d.changes.size() == 2 && d.changes[1] == 1100, "room for the next step: back up");
    }
    {
        Device d; // the owner's dungeon: 30 ms of GPU for a 16.67 ms frame
        d.control.reset(1100);
        d.fixed_ms = 4.0;
        d.pixel_ms = 26.0;
        d.run(20.0);
        check(!d.changes.empty() && d.changes[0] == 770, "a scene far over the frame goes straight to the step predicted to fit");
        check(d.control.height() == 770 && d.control.height() == d.control.lowest(), "bounded by the preset's lowest height");
        const size_t count = d.changes.size();
        d.run(60.0);
        check(d.changes.size() == count, "at the bound it stays");
    }
    {
        Device d; // fits with room, but the device is at SEVERE
        d.control.reset(1100);
        d.pixel_ms = 5.0;
        d.heat = DynamicResolution::Heat::Severe;
        d.run(8.0);
        check(d.changes.size() == 1 && d.changes[0] == 990, "severe: one step down");
        d.run(60.0);
        check(d.changes.size() == 3 && d.changes.back() == 770, "severe: a step every ten seconds down to the bound");
        d.heat = DynamicResolution::Heat::Elevated;
        d.run(120.0);
        check(d.changes.size() == 3, "no step up while the device is hot");
        d.heat = DynamicResolution::Heat::Normal;
        d.run(120.0);
        check(d.changes.back() == 1100, "cooled: back up step by step");
    }
    {
        Device d; // no GPU timers: late frames decide
        d.control.reset(1100);
        d.timers = false;
        d.run(6.0);
        check(d.changes.size() == 1 && d.changes[0] == 990, "no timers: late frames step down once");
        // 990 fits here; a probe up fails every time and is taken back: the waits double.
        d.run(600.0);
        size_t ups = 0;
        for (size_t i = 1; i < d.changes.size(); ++i)
            ups += d.changes[i] > d.changes[i - 1];
        check(ups >= 1 && ups <= 5, "no timers: probes up back off (20, 40, 80... s), no steady flicker");
    }
    {
        Device d; // late frames because the game thread fills the frame, no timers
        d.control.reset(1100);
        d.timers = false;
        d.work_ms = 16.0;
        d.run(20.0);
        check(d.changes.empty(), "a CPU-bound frame keeps its resolution");
        check(d.control.take_cpu_bound() && !d.control.take_cpu_bound(), "said once");
    }
    {
        Device d; // loading and interface frames are not judged; no interval to fit either
        d.control.reset(1100);
        d.pixel_ms = 40.0;
        d.scene = false;
        d.run(20.0);
        check(d.changes.empty(), "no scene: not judged");
        d.scene = true;
        d.target = 0.0;
        for (int i = 0; i < 600; ++i)
        {
            DynamicResolution::Frame f;
            f.now_ms = d.now += 40.0;
            f.interval_ms = 40.0;
            f.gpu_ms = 40.0;
            f.scene = true;
            check(!d.control.frame(f), "no interval to fit (the host): the preset's height");
        }
    }
    {
        Device d; // the owner's night town: 22 ms of GPU at the lowest height, 60 fps asked for
        d.control.reset(1100);
        d.pacing_follows = true;
        d.fixed_ms = 12.0;
        d.pixel_ms = 20.0; // 32 ms at 1100 lines, 21.8 at 770
        d.run(15.0);
        check(d.paced > 30.0, "nothing holds 60 fps: the pacing lowers the rate");
        d.run(240.0);
        check(d.control.height() >= 880, "at the lowered rate the height it has room for comes back (not 770 lines at 30 fps)");
        check(d.paced > 30.0, "and the rate stays lowered: nothing flickers between 60 and 30");
        const size_t count = d.changes.size();
        const uint16_t settled = d.control.height();
        d.run(240.0);
        check(d.changes.size() == count && d.control.height() == settled, "settled: no more changes in the same scene");
        // A lighter place: 60 fps fits again as it is; the pacing returns to it and the height follows as before.
        d.fixed_ms = 3.0;
        d.pixel_ms = 7.0;
        d.run(60.0);
        check(d.paced < 20.0, "a lighter scene: back at 60 fps");
        check(d.control.height() == 1100, "with the preset's height");
    }
    {
        Device d; // 60 fps holds at 880 lines but the rate was lowered at 1100 before the first step down landed
        d.control.reset(1100);
        d.pacing_follows = true;
        d.fixed_ms = 3.0;
        d.pixel_ms = 13.0; // 16 ms at 1100 (18.4 with the margin), 11.3 at 880
        d.paced = 33.34;
        d.missed_ms = 2000.0;
        d.run(30.0);
        check(d.paced < 20.0 && d.control.height() < 1100, "a lower step that fits the setting's rate is tried, and the rate comes back");
    }
    std::puts("dynamic_resolution: ok");
}
