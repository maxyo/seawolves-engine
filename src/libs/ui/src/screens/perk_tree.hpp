#pragma once

// The perk tree of the hero screen (hero_character.cpp). Presentation only: the stock script owns
// eligibility and the final learn action. IDs and edges are runtime facts (app android_hero.c
// AndroidHeroPerkGraph), never reconstructed from translated captions.
//
// One perk group is laid out to scroll vertically only, on a phone of 780 dp and more:
// - a column per tier: a perk stands one column right of its deepest prerequisite, and a perk whose
//   dependents all stand further right moves up to the nearest of them (fewer long links);
// - linked perks form blocks stacked top to bottom in the game's order; perks without any link are
//   packed row by row into every column under the blocks;
// - nodes have one height on a row grid. The page gives the columns their widths (flex), so the
//   layout holds vertical positions only (dp), plus lane positions inside the gutters between
//   columns where the links run:
//   * every source perk has its own lane in the gutter after its column;
//   * every link arrives at its own height on the target's left edge, under an arrowhead;
//   * a link that skips a column crosses it in the gap between two node rows, in its own channel,
//     and turns to the target in its own lane of the gutter before the target.
//   Lines of two sources never share a segment, so links never merge into one bus line.
#include <algorithm>
#include <cctype>
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

// A straight line piece, 2 dp thick. In a gutter x is measured from the gutter's left edge; a
// piece in a column is `full`: it spans the column's whole width at height y. `edges` are the
// indexes (Layout::edges) of the links that run along this piece.
struct Line
{
    int x = 0, y = 0, width = 0, height = 0;
    bool full = false;
    std::vector<int> edges;
};
struct Arrow
{
    int y = 0; // centre line of the arrival
    int edge = 0;
};
struct Place
{
    int column = 0, top = 0;
};
struct Column
{
    std::vector<std::string> nodes; // in the game's order
    std::vector<Line> lines;
};
struct Gutter
{
    int width = 0;
    std::vector<Line> lines;
    std::vector<Arrow> arrows;
};

struct Layout
{
    static constexpr int node_height = 80, pitch = 90, block_gap = 18, margin = 8, caption = 30;
    static constexpr int thickness = 2, exit_at = 30, arrive_at = 42, arrive_step = 9, arrive_last = 72;
    static constexpr int lane_first = 2, lane_step = 4, arrow = 7, gutter_min = 12;
    static constexpr int min_columns = 3;
    std::map<std::string, Place> nodes;
    std::vector<Edge> edges;
    std::vector<Column> columns;
    std::vector<Gutter> gutters;
    int height = 0;
    int caption_top = -1; // where the caption over the unlinked perks stands, -1: none
    bool valid = false;
};

namespace detail
{
// Greedy interval colouring: the first lane whose last span ended clear of `from`.
inline int TakeLane(std::vector<int> &ends, int from, int to)
{
    constexpr int clearance = 4;
    for (size_t lane = 0; lane < ends.size(); ++lane)
        if (ends[lane] + clearance < from)
        {
            ends[lane] = to;
            return static_cast<int>(lane);
        }
    ends.push_back(to);
    return static_cast<int>(ends.size()) - 1;
}

inline Line Horizontal(int x0, int x1, int y, std::vector<int> edges)
{
    return Line{std::min(x0, x1), y - Layout::thickness / 2, std::max(Layout::thickness, std::abs(x1 - x0)), Layout::thickness, false,
                std::move(edges)};
}
inline Line Vertical(int x, int y0, int y1, std::vector<int> edges)
{
    return Line{x, std::min(y0, y1) - Layout::thickness / 2, Layout::thickness, std::abs(y1 - y0) + Layout::thickness, false, std::move(edges)};
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
    std::vector<std::vector<int>> parents(n), children(n), incoming(n), outgoing(n);
    for (size_t e = 0; e < edges.size(); ++e)
    {
        const auto from = index.find(edges[e].from), to = index.find(edges[e].to);
        if (from == index.end() || to == index.end() || from->second == to->second)
            return layout;
        parents[to->second].push_back(from->second);
        children[from->second].push_back(to->second);
        incoming[to->second].push_back(static_cast<int>(e));
        outgoing[from->second].push_back(static_cast<int>(e));
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
        if (!parents[v].empty() || !children[v].empty())
            tiers = std::max(tiers, tier[v] + 1);
    const int columns = std::max(tiers, Layout::min_columns);

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

    std::vector<int> row(n, -1), top(n, 0), column(n, 0);
    int y = Layout::margin;
    for (const auto &[first, members] : blocks)
    {
        // Rows: each tier in the order of the mean row of its prerequisites (a perk without one: of
        // its dependents, from the first pass), each perk as near that row as the ones above allow.
        for (int pass = 0; pass < 2; ++pass)
        {
            std::vector<int> placed(n, -1);
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
                            sum += row[c], ++count;
                    wanted.push_back({count ? sum / count : -1.0, v});
                }
                // By the wanted row; at the same row a perk that leads further first, so that the longest
                // chain runs straight and the dead ends stack under it.
                const auto key = [&children](const std::pair<double, int> &w) {
                    return std::make_tuple(w.first < 0.0 ? -1 : static_cast<int>(w.first), children[w.second].empty(), w.second);
                };
                std::stable_sort(wanted.begin(), wanted.end(), [&key](const auto &a, const auto &b) { return key(a) < key(b); });
                int next = 0;
                for (const auto &[want, v] : wanted)
                {
                    placed[v] = std::max(next, want < 0.0 ? next : static_cast<int>(want + 0.5));
                    next = placed[v] + 1;
                }
            }
            for (int v : members)
                row[v] = placed[v];
        }
        int lowest = row[members.front()];
        for (int v : members)
            lowest = std::min(lowest, row[v]);
        int rows = 0;
        for (int v : members)
        {
            row[v] -= lowest;
            rows = std::max(rows, row[v] + 1);
            column[v] = tier[v];
            top[v] = y + row[v] * Layout::pitch;
        }
        y += rows * Layout::pitch - (Layout::pitch - Layout::node_height) + Layout::block_gap;
    }
    if (!unlinked.empty())
    {
        if (!blocks.empty())
        {
            layout.caption_top = y;
            y += Layout::caption;
        }
        for (size_t k = 0; k < unlinked.size(); ++k)
        {
            column[unlinked[k]] = static_cast<int>(k) % columns;
            top[unlinked[k]] = y + static_cast<int>(k) / columns * Layout::pitch;
        }
        y += static_cast<int>((unlinked.size() + columns - 1) / columns) * Layout::pitch - (Layout::pitch - Layout::node_height);
    }
    else
        y -= Layout::block_gap;
    layout.height = y + Layout::margin;

    layout.edges = edges;
    layout.columns.resize(columns);
    layout.gutters.resize(columns - 1);
    for (size_t v = 0; v < n; ++v)
    {
        layout.nodes[ids[v]] = {column[v], top[v]};
        layout.columns[column[v]].nodes.push_back(ids[v]);
    }

    // Where each link leaves and arrives: arrivals top to bottom by the source's height.
    std::vector<int> arrive(edges.size()), channel(edges.size(), 0);
    for (size_t v = 0; v < n; ++v)
    {
        auto in = incoming[v];
        std::stable_sort(in.begin(), in.end(), [&](int a, int b) {
            const int pa = index.at(edges[a].from), pb = index.at(edges[b].from);
            return std::tie(top[pa], column[pa]) < std::tie(top[pb], column[pb]);
        });
        const int count = static_cast<int>(in.size());
        const int step = count > 1 ? std::min(Layout::arrive_step, (Layout::arrive_last - Layout::arrive_at) / (count - 1)) : 0;
        for (int k = 0; k < count; ++k)
            arrive[in[k]] = top[v] + Layout::arrive_at + k * step;
    }
    // A link that skips columns runs straight at its arrival height when the skipped cells there are
    // empty (no channel boxing an empty cell), else it crosses them in the gap above its target's row,
    // in a channel of its own among the links whose column ranges overlap there.
    std::vector<std::vector<int>> in_column(columns);
    for (size_t v = 0; v < n; ++v)
        in_column[column[v]].push_back(static_cast<int>(v));
    const auto clear_at = [&](int first, int last, int y) {
        constexpr int clearance = 4;
        for (int c = first; c <= last; ++c)
            for (int v : in_column[c])
                if (top[v] - clearance <= y && y <= top[v] + Layout::node_height + clearance)
                    return false;
        return true;
    };
    std::map<int, std::vector<std::tuple<int, int, int>>> gaps; // gap top -> (first, last column, slot)
    for (size_t e = 0; e < edges.size(); ++e)
    {
        const int s = index.at(edges[e].from), t = index.at(edges[e].to);
        if (column[t] - column[s] < 2)
            continue;
        if (clear_at(column[s] + 1, column[t] - 1, arrive[e]))
        {
            channel[e] = arrive[e];
            continue;
        }
        auto &used = gaps[top[t]];
        int slot = 0;
        for (bool clash = true; clash; )
        {
            clash = false;
            for (const auto &[first, last, taken] : used)
                if (taken == slot && first <= column[t] && column[s] <= last)
                {
                    clash = true;
                    ++slot;
                    break;
                }
        }
        used.emplace_back(column[s], column[t], slot);
        constexpr int channel_step = 3;
        channel[e] = top[t] - channel_step - std::min(slot, (Layout::pitch - Layout::node_height) / channel_step - 2) * channel_step;
    }

    // Lanes: in gutter g, one per source in column g, one per skipping link that ends in column g + 1.
    struct Owner
    {
        int from = 0, to = 0;
        int source = -1;            // a source perk, or
        int edge = -1;              // the last leg of a skipping link
    };
    std::vector<std::vector<Owner>> owners(columns - 1);
    for (size_t v = 0; v < n; ++v)
    {
        if (outgoing[v].empty())
            continue;
        const int exit = top[v] + Layout::exit_at;
        Owner owner{exit, exit, static_cast<int>(v), -1};
        for (int e : outgoing[v])
        {
            const int t = index.at(edges[e].to);
            const int turn = column[t] - column[v] < 2 ? arrive[e] : channel[e];
            owner.from = std::min(owner.from, turn);
            owner.to = std::max(owner.to, turn);
        }
        owners[column[v]].push_back(owner);
    }
    for (size_t e = 0; e < edges.size(); ++e)
    {
        const int s = index.at(edges[e].from), t = index.at(edges[e].to);
        if (column[t] - column[s] >= 2 && channel[e] != arrive[e])
            owners[column[t] - 1].push_back(Owner{std::min(channel[e], arrive[e]), std::max(channel[e], arrive[e]), -1, static_cast<int>(e)});
    }
    for (int g = 0; g + 1 < columns; ++g)
    {
        auto &list = owners[g];
        std::stable_sort(list.begin(), list.end(), [](const Owner &a, const Owner &b) { return a.from < b.from; });
        std::vector<int> ends;
        std::vector<int> lanes;
        for (const Owner &owner : list)
            lanes.push_back(detail::TakeLane(ends, owner.from, owner.to));
        Gutter &gutter = layout.gutters[g];
        gutter.width = std::max<int>(Layout::gutter_min,
                                     Layout::lane_first + static_cast<int>(ends.size()) * Layout::lane_step + Layout::arrow);
        const int right = gutter.width, head = gutter.width - Layout::arrow;
        for (size_t k = 0; k < list.size(); ++k)
        {
            const Owner &owner = list[k];
            const int x = Layout::lane_first + lanes[k] * Layout::lane_step;
            if (owner.source >= 0)
            {
                const int v = owner.source, exit = top[v] + Layout::exit_at;
                gutter.lines.push_back(Horizontal(0, x + Layout::thickness, exit, outgoing[v]));
                std::vector<std::pair<int, int>> spans; // (turn, edge)
                for (int e : outgoing[v])
                {
                    const int t = index.at(edges[e].to);
                    const bool skip = column[t] - column[v] >= 2;
                    const int turn = skip ? channel[e] : arrive[e];
                    spans.push_back({turn, e});
                    if (skip)
                        gutter.lines.push_back(Horizontal(x, right, turn, {e}));
                    else
                    {
                        gutter.lines.push_back(Horizontal(x, head, turn, {e}));
                        gutter.arrows.push_back({turn, e});
                    }
                }
                // The trunk, cut at every turn so that each piece names the links along it.
                std::set<int> stops{exit};
                for (const auto &[turn, e] : spans)
                    stops.insert(turn);
                for (auto it = stops.begin(); std::next(it) != stops.end(); ++it)
                {
                    const int a = *it, b = *std::next(it);
                    std::vector<int> along;
                    for (const auto &[turn, e] : spans)
                        if (std::min(exit, turn) <= a && b <= std::max(exit, turn))
                            along.push_back(e);
                    gutter.lines.push_back(Vertical(x, a, b, along));
                }
            }
            else
            {
                const int e = owner.edge;
                gutter.lines.push_back(Horizontal(0, x + Layout::thickness, channel[e], {e}));
                gutter.lines.push_back(Vertical(x, channel[e], arrive[e], {e}));
                gutter.lines.push_back(Horizontal(x, head, arrive[e], {e}));
                gutter.arrows.push_back({arrive[e], e});
            }
        }
    }
    // The crossings of skipping links: whole columns and whole gutters between the two ends; a straight
    // one ends in the gutter before its target without a turn.
    for (size_t e = 0; e < edges.size(); ++e)
    {
        const int s = index.at(edges[e].from), t = index.at(edges[e].to);
        if (column[t] - column[s] >= 2 && channel[e] == arrive[e])
        {
            Gutter &last = layout.gutters[column[t] - 1];
            last.lines.push_back(Horizontal(0, last.width - Layout::arrow, arrive[e], {static_cast<int>(e)}));
            last.arrows.push_back({arrive[e], static_cast<int>(e)});
        }
        for (int c = column[s] + 1; c < column[t]; ++c)
        {
            layout.columns[c].lines.push_back(Line{0, channel[e] - Layout::thickness / 2, 0, Layout::thickness, true, {static_cast<int>(e)}});
            if (c + 1 < column[t])
                layout.gutters[c].lines.push_back(Horizontal(0, layout.gutters[c].width, channel[e], {static_cast<int>(e)}));
        }
    }
    layout.valid = true;
    return layout;
}
} // namespace detail

// The layout of one topology. Selection, translations, point balances and learned bits are not part
// of the key: changing them restyles nodes and lines without laying the tree out again.
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

// The longest words of a column's names (by letters; names with their short words bound by a no-break
// space, as ui-clamp wraps them): the page sizes each column so that its longest word fits on one
// line at the smallest type size.
inline std::vector<std::string> LongestWords(const std::vector<std::string> &names, size_t count = 3)
{
    const auto letters = [](const std::string &word) {
        size_t n = 0;
        for (unsigned char c : word)
            n += (c & 0xC0) != 0x80;
        return n;
    };
    std::set<std::string> distinct;
    for (const auto &name : names)
        for (size_t at = 0; at <= name.size();)
        {
            const size_t end = std::min(name.find(' ', at), name.size());
            if (end > at)
                distinct.insert(name.substr(at, end - at));
            at = end + 1;
        }
    std::vector<std::string> words(distinct.begin(), distinct.end());
    std::stable_sort(words.begin(), words.end(), [&](const auto &a, const auto &b) { return letters(a) > letters(b); });
    if (words.size() > count)
        words.resize(count);
    return words;
}
} // namespace storm::ui::screens::perk_tree
