// The normals of a sail's vertices from where its vertices are this frame.
//
// The sails move every frame (wind, sway, rolling), the vertex positions with them. A normal that stays where the
// sail was first built no longer lies across the canvas: on a billowing sail parts of it turn into the canvas's plane
// or through it, and the per-pixel two-sided lighting (ff_fragment.sh: the viewer's side of a triangle, the light
// through the canvas from the other) then lit whole triangles from the wrong side, triangle by triangle and changing
// with every gust: dark (by day) or bright (lanterns) jagged patches that crawled over the sail.
// So the normal of every vertex is the cross product of the canvas's two grid directions at it (central differences,
// one-sided at the edges), turned to the side the sail's own normal (its plane's, SAILGEOMETRY::cv.normL) faces,
// one side for the whole sail. A vertex whose neighbours give no direction (a sail rolled flat onto its yard) keeps
// the normal it had.
//
// Grids (the vertex buffer of one sail, SAILONE::ss.sVert onwards):
//   * square and trapezoid sails: column-major, vertex (column c, row r) at c * rows + r; only the columns the
//     current level of detail computes (`columns`, ascending) are drawn and taken;
//   * triangle sails: row ix holds ix + 1 vertices, vertex (ix, iy) at ix * (ix + 1) / 2 + iy; only the rows the
//     current level of detail computes (`rows`, ascending) are taken.
#pragma once

#include "storm_glm.h"

#include <cstddef>
#include <cstdint>
#include <span>

namespace storm::rigging
{

// The sign that turns the grid's cross products to the side of `side` (summed over the sail); `previous` when the
// sail gives no side (its plane normal is zero while it is rolled up).
inline float SailNormalSide(const glm::vec3 &sum, const glm::vec3 &side, float previous)
{
    const float d = glm::dot(sum, side);
    if (d > 0.0f)
        return 1.0f;
    if (d < 0.0f)
        return -1.0f;
    return previous;
}

// Square and trapezoid sails. Returns the side used (pass it back as `side_sign` next frame).
template <class Vertex>
float SquareSailNormals(Vertex *pv, std::span<const uint16_t> columns, uint32_t rows, const glm::vec3 &side, float side_sign)
{
    if (columns.size() < 2 || rows < 2)
        return side_sign;
    const auto at = [&](size_t k, uint32_t r) -> const glm::vec3 & { return pv[columns[k] * rows + r].pos; };
    glm::vec3 sum{0.0f};
    const auto normal = [&](size_t k, uint32_t r) {
        const glm::vec3 across = at(k + 1 < columns.size() ? k + 1 : k, r) - at(k > 0 ? k - 1 : k, r);
        const glm::vec3 down = at(k, r + 1 < rows ? r + 1 : r) - at(k, r > 0 ? r - 1 : r);
        return glm::cross(down, across);
    };
    for (size_t k = 0; k < columns.size(); ++k)
        for (uint32_t r = 0; r < rows; ++r)
            sum += normal(k, r);
    const float sign = SailNormalSide(sum, side, side_sign);
    for (size_t k = 0; k < columns.size(); ++k)
        for (uint32_t r = 0; r < rows; ++r)
        {
            const glm::vec3 n = storm::math::normalizeOrZero(normal(k, r) * sign);
            if (glm::dot(n, n) > 0.0f)
                pv[columns[k] * rows + r].norm = n;
        }
    return sign;
}

// Triangle sails. Returns the side used.
template <class Vertex>
float TriangleSailNormals(Vertex *pv, std::span<const uint16_t> rows, const glm::vec3 &side, float side_sign)
{
    if (rows.size() < 2)
        return side_sign;
    const auto index = [](uint32_t ix, uint32_t iy) { return ix * (ix + 1) / 2 + iy; };
    const auto at = [&](size_t k, uint32_t iy) -> const glm::vec3 & {
        const uint32_t ix = rows[k];
        return pv[index(ix, iy < ix ? iy : ix)].pos;
    };
    // Along a row (iy) and across the rows (the neighbouring rows at the same iy, the last vertex of a shorter row).
    const auto normal = [&](size_t k, uint32_t iy) {
        const uint32_t ix = rows[k];
        const glm::vec3 along = ix == 0 ? glm::vec3{0.0f} : at(k, iy + 1 <= ix ? iy + 1 : iy) - at(k, iy > 0 ? iy - 1 : iy);
        const glm::vec3 across = at(k + 1 < rows.size() ? k + 1 : k, iy) - at(k > 0 ? k - 1 : k, iy);
        return glm::cross(along, across);
    };
    glm::vec3 sum{0.0f};
    for (size_t k = 0; k < rows.size(); ++k)
        for (uint32_t iy = 0; iy <= rows[k]; ++iy)
            sum += normal(k, iy);
    const float sign = SailNormalSide(sum, side, side_sign);
    for (size_t k = 0; k < rows.size(); ++k)
        for (uint32_t iy = 0; iy <= rows[k]; ++iy)
        {
            glm::vec3 n = storm::math::normalizeOrZero(normal(k, iy) * sign);
            if (rows[k] == 0)
            {
                // The tip has no row direction: the normal of the row after it at its first vertex.
                n = storm::math::normalizeOrZero(normal(k + 1 < rows.size() ? k + 1 : k, 0) * sign);
            }
            if (glm::dot(n, n) > 0.0f)
                pv[index(rows[k], iy)].norm = n;
        }
    return sign;
}

} // namespace storm::rigging
