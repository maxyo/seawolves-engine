// The sail normals from the vertices of this frame (src/libs/rigging/src/sail_normals.hpp): on a billowing, swaying
// canvas every triangle of every level of detail has its three vertex normals on one side of the triangle, and that
// side is the sail's own (its plane normal); the normals the sail was built with (radial from a point behind the
// sail, SAILONE::FillVertex, kept while the wind moves the canvas) fail that, which the two-sided lighting showed as jagged patches.
#include "../../src/libs/rigging/src/sail_normals.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace
{
struct Vertex
{
    glm::vec3 pos, norm;
};

constexpr uint32_t kCols = 13, kRows = 17;
const glm::vec3 kSide{0.0f, 0.0f, 1.0f};

void require(bool ok, const char *what)
{
    if (!ok)
    {
        std::fprintf(stderr, "FAIL: %s\n", what);
        std::exit(1);
    }
}

// A sail in the x/-y plane bellied towards +z by `depth`, with a gust that folds its lower lee corner.
glm::vec3 Canvas(float u, float v, float depth, float gust)
{
    const float belly = depth * std::sin(3.14159265f * u) * std::sin(3.14159265f * v);
    const float fold = gust * std::exp(-((u - 0.8f) * (u - 0.8f) + (v - 0.75f) * (v - 0.75f)) * 40.0f);
    return {u * 6.0f + fold * 1.5f, -v * 8.0f, belly - fold};
}

// The normals the sail was built with: radial from a point behind its centre (SAILONE::FillVertex).
glm::vec3 Radial(const glm::vec3 &p)
{
    const glm::vec3 centre{3.0f, -4.0f, 0.0f};
    return storm::math::normalizeOrZero(p - (centre - kSide * 5.0f));
}

// Every triangle between consecutive taken lines: all three normals on the sail's side of the triangle.
uint32_t WrongTriangles(const std::vector<Vertex> &v, const std::vector<std::array<uint32_t, 3>> &triangles)
{
    uint32_t wrong = 0;
    for (const auto &t : triangles)
    {
        glm::vec3 face = glm::cross(v[t[1]].pos - v[t[0]].pos, v[t[2]].pos - v[t[0]].pos);
        if (glm::dot(face, kSide) < 0.0f)
            face = -face;
        for (const uint32_t i : t)
            if (glm::dot(v[i].norm, face) <= 0.0f)
            {
                ++wrong;
                break;
            }
    }
    return wrong;
}

std::vector<std::array<uint32_t, 3>> SquareTriangles(const std::vector<uint16_t> &cols)
{
    std::vector<std::array<uint32_t, 3>> t;
    for (size_t k = 0; k + 1 < cols.size(); ++k)
        for (uint32_t r = 0; r + 1 < kRows; ++r)
        {
            const uint32_t a = cols[k] * kRows + r, b = cols[k + 1] * kRows + r;
            t.push_back({a, b, a + 1});
            t.push_back({b, b + 1, a + 1});
        }
    return t;
}

uint32_t TriIndex(uint32_t ix, uint32_t iy) { return ix * (ix + 1) / 2 + iy; }

void Square(float gust, const std::vector<uint16_t> &cols)
{
    std::vector<Vertex> v(kCols * kRows);
    for (uint32_t c = 0; c < kCols; ++c)
        for (uint32_t r = 0; r < kRows; ++r)
        {
            Vertex &x = v[c * kRows + r];
            // Built (FillVertex) with the belly to +z; this frame the wind fills it from the other side.
            x.norm = Radial(Canvas(c / float(kCols - 1), r / float(kRows - 1), 2.5f, 0.0f));
            x.pos = Canvas(c / float(kCols - 1), r / float(kRows - 1), -3.5f, gust);
        }
    const auto triangles = SquareTriangles(cols);
    require(WrongTriangles(v, triangles) > 0, "control: the built normals must fail on a folded canvas");
    const float side = storm::rigging::SquareSailNormals(v.data(), std::span(cols), kRows, kSide, 1.0f);
    require(side == 1.0f, "square: the sail's side");
    require(WrongTriangles(v, triangles) == 0, "square: a triangle with a normal on the wrong side");
    // A sail whose plane normal points the other way gets the other side, consistently.
    storm::rigging::SquareSailNormals(v.data(), std::span(cols), kRows, -kSide, 1.0f);
    for (const uint16_t c : cols)
        for (uint32_t r = 0; r < kRows; ++r)
            require(v[c * kRows + r].norm.z < 0.5f, "square: turned to the plane normal");
}

void Triangle(float gust, const std::vector<uint16_t> &rows)
{
    std::vector<Vertex> v(kRows * (kRows + 1) / 2);
    for (uint32_t ix = 0; ix < kRows; ++ix)
        for (uint32_t iy = 0; iy <= ix; ++iy)
        {
            Vertex &x = v[TriIndex(ix, iy)];
            const float u = ix / float(kRows - 1), w = ix == 0 ? 0.0f : iy / float(ix);
            x.norm = Radial(Canvas(u * w, u, 2.0f, 0.0f));
            x.pos = Canvas(u * w, u, -4.0f, gust);
        }
    std::vector<std::array<uint32_t, 3>> triangles;
    for (size_t k = 0; k + 1 < rows.size(); ++k)
    {
        const uint32_t a = rows[k], b = rows[k + 1];
        for (uint32_t iy = 0; iy < b; ++iy)
            triangles.push_back({TriIndex(b, iy), TriIndex(b, iy + 1), TriIndex(a, std::min(iy, a))});
    }
    require(WrongTriangles(v, triangles) > 0, "control: the built normals must fail on a folded triangle sail");
    storm::rigging::TriangleSailNormals(v.data(), std::span(rows), kSide, 1.0f);
    require(WrongTriangles(v, triangles) == 0, "triangle: a triangle with a normal on the wrong side");
}

std::vector<uint16_t> Steps(uint32_t count, uint32_t step)
{
    std::vector<uint16_t> lines;
    for (uint32_t i = 0; i < count; i += step)
        lines.push_back(static_cast<uint16_t>(i));
    return lines;
}
} // namespace

int main()
{
    for (const float gust : {0.0f, 0.3f, 0.6f})
    {
        Square(gust, Steps(kCols, 1));
        Square(gust, {0, 1, 3, 4, 5, 7, 8, 9, 11, 12}); // m_dwCol 10
        Square(gust, Steps(kCols, 2));
        Square(gust, Steps(kCols, 4));
        Triangle(gust, Steps(kRows, 1));
        Triangle(gust, Steps(kRows, 2));
        Triangle(gust, Steps(kRows, 4));
    }
    // A sail rolled flat onto its yard: no direction, the normals stay; no plane normal: last frame's side.
    std::vector<Vertex> flat(kCols * kRows, Vertex{{1.0f, 2.0f, 3.0f}, {0.0f, 1.0f, 0.0f}});
    const std::vector<uint16_t> all = Steps(kCols, 1);
    require(storm::rigging::SquareSailNormals(flat.data(), std::span(all), kRows, glm::vec3{0.0f}, -1.0f) == -1.0f,
            "a zero plane normal keeps the previous side");
    for (const Vertex &x : flat)
        require(x.norm == glm::vec3(0.0f, 1.0f, 0.0f), "a degenerate grid keeps its normals");
    std::puts("PASS: sail normals follow the moving canvas: every triangle of every level of detail (square, trapezoid, "
              "triangle sails) has its normals on the sail's side, where the built radial normals do not; a rolled sail "
              "keeps its normals and side");
    return 0;
}
