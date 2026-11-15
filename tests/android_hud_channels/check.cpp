// The HUD state channels build their JSON only when their typed state changed (HudChannel,
// platform/android_hud_channel.hpp) and publish exactly what comparing the JSON text every loop
// published. Each channel's state is driven through a deterministic random walk of its real
// mutations; after every step the old publisher (build the JSON, compare it with the last text)
// and the new one (compare the publication key, build only on a change) run side by side and must
// publish the same byte-identical sequence. Loops with an unchanged state must not build.
#include <platform/android_hud_channel.hpp>
#include <platform/android_sea_orders.hpp>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

using namespace storm::android;

namespace
{
// The old publisher and the new one, side by side, over one channel.
template <class KeyRefs> struct Twin
{
    const char *name;
    std::string lastText;
    std::vector<std::string> oldSent, newSent;
    HudChannel<KeyRefs> channel;
    size_t steps = 0, builds = 0, unchangedSteps = 0;
    // `json`: what the old publisher compared; `key`: the new publisher's typed state.
    template <class Json> void step(const Json &json, const KeyRefs &key)
    {
        ++steps;
        const bool unchanged = channel.same(key);
        const auto text = json();
        if (text != lastText) { lastText = text; oldSent.push_back(text); }
        if (const auto *sent = channel.publish(key, [&] { ++builds; return json(); })) newSent.push_back(*sent);
        if (unchanged) ++unchangedSteps;
    }
    // A new UI host: both forget what they sent.
    void reset() { lastText.clear(); channel.reset(); }
    void verify() const
    {
        assert(oldSent == newSent);
        assert(builds + unchangedSteps == steps); // an unchanged state never builds
        std::printf("  %-14s %5zu loops, %4zu publications identical, %5zu JSON builds (old: %zu)\n", name, steps,
                    newSent.size(), builds, steps);
    }
};

SeaOrdersFrame seaFrame(std::mt19937 &random)
{
    SeaOrdersFrame f;
    f.available = random() % 8 != 0;
    f.active = random() % 3 == 0;
    f.language = "russian";
    f.recipients.push_back({"r11_0", "Flagship", "11", 0, true});
    if (random() % 2) f.recipients.push_back({"r12_3", random() % 4 ? "Companion" : "Comp\"anion", "12", 3, false});
    f.recipientId = "r11_0";
    if (f.active)
    {
        f.mode = 32;
        f.items.push_back({"i0", "command", "Drift", "native-drift", true, 1, "BI_Drift"});
        // Cooldown animation, sometimes below the printed precision (no JSON change).
        f.items.push_back({"i1", "command", "Ability", "native-ability", random() % 2 == 0,
                           (random() % 4) * .25 + (random() % 2) * 1e-9, "BI_Ability"});
        f.selectedItemId = random() % 2 ? "i0" : "";
    }
    return f;
}

void seaOrdersChannel()
{
    std::mt19937 random(7);
    SeaOrdersState s;
    Twin<decltype(s.publicationKey())> twin{"seaOrders"};
    const auto json = [&] { return s.json(); };
    s.resetActivity();
    twin.step(json, s.publicationKey());
    auto frame = seaFrame(random);
    for (int i = 0; i < 4000; ++i)
    {
        switch (random() % 16)
        {
        case 0: frame = seaFrame(random); s.update("battle1:ship11", frame); break;
        case 1: s.update(random() % 2 ? "battle1:ship11" : "battle2:ship11", frame); break;
        case 2: s.invalidate(); break;
        case 3: s.resetActivity(); break;
        case 4: twin.reset(); s.resetActivity(); break; // publishSeaOrders on a new UI host
        case 5: s.update({}, {}); break;
        default: s.update("battle1:ship11", frame); break; // an unchanged refresh: most loops
        }
        twin.step(json, s.publicationKey());
    }
    twin.verify();
}

template <class Run> double nanosPerLoop(int loops, Run &&run)
{
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < loops; ++i) run();
    return std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count() / loops;
}
volatile size_t sink;

// One loop of an unchanged channel: the old publisher built and compared the JSON, the new one
// compares the key.
void bench()
{
    constexpr int loops = 200000;
    std::mt19937 random(3);
    {
        SeaOrdersState s;
        s.resetActivity();
        auto f = seaFrame(random);
        f.active = false;
        f.items.clear();
        s.update("battle1:ship11", f);
        std::string last = s.json();
        HudChannel<decltype(s.publicationKey())> channel;
        channel.publish(s.publicationKey(), [&] { return s.json(); });
        const double old = nanosPerLoop(loops, [&] { const auto j = s.json(); sink = sink + (j == last); });
        const double now = nanosPerLoop(loops, [&] { sink = sink + (channel.publish(s.publicationKey(), [&] { return s.json(); }) != nullptr); });
        std::printf("  bench %-14s unchanged loop: old %7.1f ns, new %6.1f ns\n", "seaOrders", old, now);
    }
}
} // namespace

int main(int argc, char **argv)
{
    if (argc > 1 && std::strcmp(argv[1], "--bench") == 0) { bench(); return 0; }
    seaOrdersChannel();
    std::puts("android_hud_channels: ok");
}
