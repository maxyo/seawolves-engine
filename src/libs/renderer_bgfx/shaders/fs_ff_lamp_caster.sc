// Lamp shadow faces (src/effects/lamp_shadows.hpp, vs_ff_caster*) for casters that may reach into the lamp's holder
// sphere (its own fixture, the hand that carries a lantern): nothing inside it casts. The face keeps the hardware
// depth of its own projection (fx::LampFaceDepth); this shader writes no depth. The distance to the lamp comes from
// the face's projection: w is the distance along the face axis, the position inside the tile gives the other two.
// Casters clear of the sphere take fs_ff_caster (no discard).
#include "storm_common.sh"

// x: face size in pixels (the atlas is a grid of faces), y unused, z: metres around the lamp in which nothing casts
// (its own holder), w: fx::kLampFaceGuard
uniform vec4 u_fxLampCaster;

void main()
{
    vec2 tile = fract(gl_FragCoord.xy / u_fxLampCaster.x) * 2.0 - vec2_splat(1.0);
    float reach = sqrt(1.0 + dot(tile, tile) * u_fxLampCaster.w * u_fxLampCaster.w) / gl_FragCoord.w;
    if (reach < u_fxLampCaster.z)
        discard;
    gl_FragColor = vec4_splat(0.0);
}
