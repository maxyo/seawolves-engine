// The bulge a sail model's limit point gives its sail.
//
// A sail can carry a limit-point label (sail?0) besides its corner labels; SAILONE::SetSail turns it into the
// sail's bulge (ss.fDeepZ / ss.fDeepH), which the wind code multiplies several times over (the bulge of a square
// sail's middle is about four times the value). The game's sail models were made and tuned against the original
// engine, which wrote this value as `v | vN + fD` with its CVECTOR: `+` binds tighter than `|`, and CVECTOR's
// implicit float constructor spreads fD over all three components, so the value is dot(v, vN + (fD, fD, fD)), not
// the point's signed distance to the sail plane, dot(v, vN) + fD.
//
// The signed distance is not what the models' limit points encode: it has either sign and up to several metres,
// and with it the sails of many ships (corvette, luggers, schooners, barque, ...) ballooned into huge shells or
// bulged against the wind. The original value is kept on purpose: it is the stock look of these models.
#pragma once

#include "storm_glm.h"

namespace storm::rigging
{

// vB1..vB3: three corners of the sail (hardPoints[0..2]); v: the limit point, in the same (node) space.
inline float SailLimitPointBulge(const glm::vec3 &v, const glm::vec3 &vB1, const glm::vec3 &vB2, const glm::vec3 &vB3)
{
    const glm::vec3 vN = storm::math::normalizeOrZero(glm::cross(vB1 - vB2, vB3 - vB2));
    const float fD = -glm::dot(vN, vB2);
    return glm::dot(v, vN + glm::vec3(fD));
}

} // namespace storm::rigging
