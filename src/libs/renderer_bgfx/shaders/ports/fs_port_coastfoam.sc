$input v_color0, v_color1, v_texcoord0, v_texcoord1, v_texcoord2, v_texcoord3, v_texcoord4, v_fog
// The fixed-function stages of technique CoastFoam (src/techniques/weather/CoastFoam.fx) for vs_port_coastfoam:
// colour = texture, alpha = texture * diffuse * 2 (modulate2x). A pixel port because the vertex shader samples the
// sea's heights at stage 1 (s_vol1), which the fixed-function stages would not bind behind a disabled stage.
#include "port_ps.sh"
SAMPLER2D(s_tex0, 0);

void main()
{
    vec4 t0 = texture2D(s_tex0, v_texcoord0.xy);
    vec4 r0 = vec4(t0.rgb, t0.a * v_color0.a * 2.0);
    PORT_PS_OUTPUT(r0);
}
