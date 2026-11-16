#pragma once

// The perk tree of the hero screen (hero_character.cpp). Presentation only: the stock script owns
// eligibility and the final learn action. IDs and edges are runtime facts (app android_hero.c
// AndroidHeroPerkGraph), never reconstructed from translated captions.
//
// One perk group as a vertical tree of icons that scrolls vertically only, on a canvas of fixed
// width (Layout::width, which a 780 dp phone shows beside the panel):
// - a row per tier, top to bottom: a perk stands one row under its deepest prerequisite, and a perk
//   whose dependents all stand further down moves up next to the nearest of them;
// - linked perks form blocks stacked in the game's order; a perk without any link takes a free slot
//   beside a row of icons, the topmost row first (no captions or labels in the tree); only when no row
//   has room, new rows follow the blocks;
// - each perk is an icon in a touch box of one slot, on a grid of half slots, as near the mean of
//   its prerequisites (or dependents) as its row allows; a tier wider than the canvas wraps;
// - links are smooth curves (drawn by <ui-links>) from under the source icon to above the target
//   icon, each arriving at its own point; a link that would cross a row of icons runs down a free
//   lane between them, in its own offset. Lines of two sources never share a segment.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>
#include <queue>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace storm::ui::screens::perk_tree
{
inline std::string Id(std::string value)
{
    for (char &c : value)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return value;
}

struct Edge
{
    std::string from, to;
    bool operator==(const Edge &) const = default;
    bool operator<(const Edge &other) const
    {
        return std::tie(from, to) < std::tie(other.from, other.to);
    }
};

struct Point
{
    int x = 0, y = 0; // dp on the canvas
    bool operator==(const Point &) const = default;
};
// The way of one link (Layout::edges index): from under the source icon to above the target icon,
// through lane points; <ui-links> joins consecutive points with curves that leave and arrive vertically.
struct Link
{
    int edge = 0;
    std::vector<Point> points;
};
struct Place
{
    int left = 0, top = 0; // the touch box on the canvas, dp
};

struct Layout
{
    static constexpr int slots = 8, slot = 62, width = slots * slot;            // the canvas: 496 dp
    static constexpr int box = 56, icon = 44, icon_top = 6;                    // touch box height, icon in it
    static constexpr int pitch = 84, block_gap = 22, margin = 8;
    static constexpr int arrive_step = 7, lane_step = 5;
    std::map<std::string, Place> nodes;
    std::vector<Edge> edges;
    std::vector<Link> links;
    int height = 0;
    bool valid = false;
};

namespace detail
{
// Greedy interval colouring: the first lane whose last span ended clear of `from`.
inline int TakeLane(std::vector<int> &ends, int from, int to)
{
    constexpr int clearance = 6;
    for (size_t lane = 0; lane < ends.size(); ++lane)
        if (ends[lane] + clearance < from)
        {
            ends[lane] = to;
            return static_cast<int>(lane);
        }
    ends.push_back(to);
    return static_cast<int>(ends.size()) - 1;
}

inline Layout Build(const std::vector<std::string> &ids, const std::vector<Edge> &edges)
{
    Layout layout;
    const size_t n = ids.size();
    // Malformed or future data must not invent a dependency or loop forever.
    if (n == 0 || n > 128 || edges.size() > 512)
        return layout;
    std::map<std::string, int> index;
    for (size_t i = 0; i < n; ++i)
        if (ids[i].empty() || !index.emplace(ids[i], static_cast<int>(i)).second)
            return layout;
    std::vector<std::vector<int>> parents(n), children(n), incoming(n);
    for (size_t e = 0; e < edges.size(); ++e)
    {
        const auto from = index.find(edges[e].from), to = index.find(edges[e].to);
        if (from == index.end() || to == index.end() || from->second == to->second)
            return layout;
        parents[to->second].push_back(from->second);
        children[from->second].push_back(to->second);
        incoming[to->second].push_back(static_cast<int>(e));
    }
    // Topological order, ties in the game's order.
    std::vector<int> pending(n), order;
    std::priority_queue<int, std::vector<int>, std::greater<>> ready;
    for (size_t v = 0; v < n; ++v)
        if ((pending[v] = static_cast<int>(parents[v].size())) == 0)
            ready.push(static_cast<int>(v));
    while (!ready.empty())
    {
        const int v = ready.top();
        ready.pop();
        order.push_back(v);
        for (int c : children[v])
            if (--pending[c] == 0)
                ready.push(c);
    }
    if (order.size() != n)
        return layout; // a cycle: no fabricated partial tree

    std::vector<int> tier(n, 0);
    for (int v : order)
        for (int c : children[v])
            tier[c] = std::max(tier[c], tier[v] + 1);
    for (auto it = order.rbegin(); it != order.rend(); ++it)
        if (!children[*it].empty())
        {
            int nearest = tier[children[*it].front()];
            for (int c : children[*it])
                nearest = std::min(nearest, tier[c]);
            tier[*it] = std::max(tier[*it], nearest - 1);
        }
    int tiers = 0;
    for (size_t v = 0; v < n; ++v)
        tiers = std::max(tiers, tier[v] + 1);

    // Blocks of linked perks, in the order of their first perk.
    std::vector<int> group(n);
    for (size_t v = 0; v < n; ++v)
        group[v] = static_cast<int>(v);
    const auto root = [&group](int v) {
        while (group[v] != v)
            v = group[v] = group[group[v]];
        return v;
    };
    for (const auto &edge : edges)
    {
        const int a = root(index.at(edge.from)), b = root(index.at(edge.to));
        group[std::max(a, b)] = std::min(a, b);
    }
    std::map<int, std::vector<int>> blocks; // root (smallest member) -> members in order
    std::vector<int> unlinked;
    for (size_t v = 0; v < n; ++v)
        if (parents[v].empty() && children[v].empty())
            unlinked.push_back(static_cast<int>(v));
        else
            blocks[root(static_cast<int>(v))].push_back(static_cast<int>(v));

    constexpr int halves = 2 * Layout::slots; // half-slot grid lines 0..halves; a node covers [h, h + 2]
    std::vector<int> half(n, 0), top(n, 0), shift(n, 0); // shift: the block's offset in dp that centres it exactly
    int y = Layout::margin;
    for (const auto &[first, members] : blocks)
    {
        // Rows: each tier in the order of the mean centre of its prerequisites (a perk without one: of its
        // dependents, from the first pass), each perk as near that centre as its row allows.
        std::vector<int> row(n, -1);
        int rows = 0;
        for (int pass = 0; pass < 2; ++pass)
        {
            std::vector<int> placed(n, -1); // centre in half slots
            int row_at = 0;
            for (int t = 0; t < tiers; ++t)
            {
                std::vector<std::pair<double, int>> wanted;
                for (int v : members)
                {
                    if (tier[v] != t)
                        continue;
                    double sum = 0.0;
                    int count = 0;
                    for (int p : parents[v])
                        sum += placed[p], ++count;
                    if (count == 0 && pass == 1)
                        for (int c : children[v])
                            sum += half[c] + 1, ++count;
                    wanted.push_back({count ? sum / count : -1.0, v});
                }
                if (wanted.empty())
                    continue;
                std::stable_sort(wanted.begin(), wanted.end(), [](const auto &a, const auto &b) {
                    return std::tie(a.first, a.second) < std::tie(b.first, b.second);
                });
                for (size_t start = 0; start < wanted.size(); start += Layout::slots)
                {
                    const std::vector<std::pair<double, int>> line(wanted.begin() + start,
                                                                   wanted.begin() + std::min(wanted.size(), start + Layout::slots));
                    const int m = static_cast<int>(line.size());
                    std::vector<int> h(m);
                    for (int k = 0; k < m; ++k) // a perk without a wish: the row centred
                        h[k] = line[k].first < 0.0 ? Layout::slots - m + 2 * k : static_cast<int>(std::lround(line[k].first)) - 1;
                    for (int k = 0; k < m; ++k)
                        h[k] = std::max(k ? h[k - 1] + 2 : 0, h[k]);
                    for (int k = m - 1; k >= 0; --k)
                        h[k] = std::min(k + 1 < m ? h[k + 1] - 2 : halves - 2, h[k]);
                    for (int k = 0; k < m; ++k)
                    {
                        half[line[k].second] = h[k];
                        placed[line[k].second] = h[k] + 1;
                        row[line[k].second] = row_at;
                    }
                    ++row_at;
                }
            }
            rows = row_at;
        }
        // The block centred on the canvas, to the dp (its grid stays the half-slot grid).
        int low = halves, high = 0;
        for (int v : members)
            low = std::min(low, half[v]), high = std::max(high, half[v] + 2);
        const int offset = (Layout::width - (high - low) * Layout::slot / 2) / 2 - low * Layout::slot / 2;
        for (int v : members)
        {
            shift[v] = offset;
            top[v] = y + row[v] * Layout::pitch;
        }
        y += rows * Layout::pitch - (Layout::pitch - Layout::box) + Layout::block_gap;
    }
    // Perks without links: beside the rows of icons, topmost row first, alternately right and left of the row so
    // the block stays centred; a new centred row after the blocks only when no row has a free slot.
    std::vector<int> placed_nodes;
    for (size_t v = 0; v < n; ++v)
        if (!parents[v].empty() || !children[v].empty())
            placed_nodes.push_back(static_cast<int>(v));
    const auto left_of = [&](int v) { return half[v] * Layout::slot / 2 + shift[v]; };
    std::vector<int> rows_top;
    for (int v : placed_nodes)
        if (std::find(rows_top.begin(), rows_top.end(), top[v]) == rows_top.end())
            rows_top.push_back(top[v]);
    std::sort(rows_top.begin(), rows_top.end());
    if (blocks.empty())
        y += Layout::block_gap; // no blocks: the loop below starts the rows at the margin
    for (int v : unlinked)
    {
        bool done = false;
        for (int row_top : rows_top)
        {
            int low = Layout::width, high = 0;
            for (int u : placed_nodes)
                if (top[u] == row_top)
                    low = std::min(low, left_of(u)), high = std::max(high, left_of(u) + Layout::slot);
            // Alternately right and left of the row (the side with more room first), so it stays balanced.
            int added = 0;
            for (int u : unlinked)
                added += u != v && top[u] == row_top && std::find(placed_nodes.begin(), placed_nodes.end(), u) != placed_nodes.end();
            const bool right_first = ((Layout::width - high) >= low) == (added % 2 == 0);
            for (int side = 0; side < 2 && !done; ++side)
            {
                const bool right = (side == 0) == right_first;
                const int left = right ? high : low - Layout::slot;
                if (left < 0 || left + Layout::slot > Layout::width)
                    continue;
                half[v] = 0;
                shift[v] = left;
                top[v] = row_top;
                placed_nodes.push_back(v);
                done = true;
            }
            if (done)
                break;
        }
        if (!done)
        {
            const int row_top = y - Layout::block_gap + (rows_top.empty() ? 0 : Layout::pitch - Layout::box);
            rows_top.push_back(row_top);
            half[v] = 0;
            shift[v] = (Layout::width - Layout::slot) / 2;
            top[v] = row_top;
            placed_nodes.push_back(v);
            y = row_top + Layout::box + Layout::block_gap;
        }
    }
    y -= Layout::block_gap;
    layout.height = y + Layout::margin;
    layout.edges = edges;
    for (size_t v = 0; v < n; ++v)
        layout.nodes[ids[v]] = {half[v] * Layout::slot / 2 + shift[v], top[v]};

    // Links. Arrivals on a target's top, left to right by their source's centre.
    const auto centre = [&](int v) { return (half[v] + 1) * Layout::slot / 2 + shift[v]; };
    const auto exit_y = [&](int v) { return top[v] + Layout::icon_top + Layout::icon + 2; };
    const auto arrive_y = [&](int v) { return top[v] + Layout::icon_top - 1; };
    std::vector<int> arrive_x(edges.size());
    for (size_t v = 0; v < n; ++v)
    {
        auto in = incoming[v];
        std::stable_sort(in.begin(), in.end(), [&](int a, int b) { return centre(index.at(edges[a].from)) < centre(index.at(edges[b].from)); });
        const int k = static_cast<int>(in.size());
        for (int i = 0; i < k; ++i)
            arrive_x[in[i]] = centre(static_cast<int>(v)) + (2 * i - (k - 1)) * Layout::arrive_step / 2;
    }
    // A link whose span holds a row of icons between its ends goes down the free half-slot line nearest
    // to the middle of its ends, in its own offset among the links on that line.
    std::map<int, std::vector<int>> lane_ends; // lane x (dp) -> lane ends
    for (size_t e = 0; e < edges.size(); ++e)
    {
        const int s = index.at(edges[e].from), t = index.at(edges[e].to);
        Link link{static_cast<int>(e), {{centre(s), exit_y(s)}}};
        std::vector<int> between;
        for (size_t v = 0; v < n; ++v)
            if (top[v] > top[s] && top[v] < top[t])
                between.push_back(static_cast<int>(v));
        if (!between.empty())
        {
            const auto free = [&](int line) { // the lane's x passes no touch box of the rows between
                const int x = line * Layout::slot / 2 + shift[s];
                if (x <= 0 || x >= Layout::width)
                    return false;
                for (int v : between)
                    if (layout.nodes.at(ids[v]).left < x && x < layout.nodes.at(ids[v]).left + Layout::slot)
                        return false;
                return true;
            };
            const double middle = ((centre(s) + centre(t)) / 2.0 - shift[s]) / (Layout::slot / 2.0);
            int best = -halves - 1; // none yet
            // Lines across the whole canvas, in half slots from the source block's own grid.
            for (int line = -halves; line <= 2 * halves; ++line)
                if (free(line) && (best < -halves || std::fabs(line - middle) < std::fabs(best - middle)))
                    best = line;
            const int from = exit_y(s) + (Layout::pitch - Layout::box) / 2, to = arrive_y(t) - (Layout::pitch - Layout::box) / 2;
            const int base = best * Layout::slot / 2 + shift[s];
            const int lane = TakeLane(lane_ends[base], from, to);
            int offset = (lane + 1) / 2 * Layout::lane_step * (lane % 2 ? -1 : 1);
            if (base + offset < Layout::lane_step) // the canvas edges: lanes step inwards
                offset = std::abs(offset) + Layout::lane_step;
            if (base + offset > Layout::width - Layout::lane_step)
                offset = -std::abs(offset) - Layout::lane_step;
            const int x = base + offset;
            link.points.push_back({x, from});
            link.points.push_back({x, to});
        }
        link.points.push_back({arrive_x[e], arrive_y(t)});
        layout.links.push_back(std::move(link));
    }
    layout.valid = true;
    return layout;
}
} // namespace detail

// The layout of one topology. Selection, translations, point balances and learned bits are not part
// of the key: changing them restyles icons and links without laying the tree out again.
class Cache
{
  public:
    // `ids` in the game's order (the group's table, then the other group's prerequisites).
    const Layout &Get(std::vector<std::string> ids, std::vector<Edge> edges)
    {
        for (auto &id : ids)
            id = Id(std::move(id));
        for (auto &edge : edges)
        {
            edge.from = Id(std::move(edge.from));
            edge.to = Id(std::move(edge.to));
        }
        std::sort(edges.begin(), edges.end());
        edges.erase(std::unique(edges.begin(), edges.end()), edges.end());
        if (initialized_ && ids == ids_ && edges == edges_)
            return layout_;
        initialized_ = true;
        ids_ = std::move(ids);
        edges_ = std::move(edges);
        ++builds_;
        layout_ = detail::Build(ids_, edges_);
        return layout_;
    }
    size_t Builds() const
    {
        return builds_;
    }

  private:
    bool initialized_ = false;
    size_t builds_ = 0;
    std::vector<std::string> ids_;
    std::vector<Edge> edges_;
    Layout layout_;
};

inline bool CanLearn(bool local, bool full, bool learned, int points, const std::string &rule, bool stock_eligible,
                     const std::vector<bool> &requirements)
{
    // ChoosePerk evaluates ALL condition attributes. Unknown/OR rules stay locked until an
    // authoritative adapter and evaluator exist for them.
    return local && full && !learned && points > 0 && rule == "stock-all-v1" && stock_eligible &&
           std::all_of(requirements.begin(), requirements.end(), [](bool met) { return met; });
}
} // namespace storm::ui::screens::perk_tree
