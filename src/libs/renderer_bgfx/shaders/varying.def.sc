vec4 v_color0    : COLOR0    = vec4(1.0, 1.0, 1.0, 1.0);
vec4 v_color1    : COLOR1    = vec4(0.0, 0.0, 0.0, 0.0);
vec4 v_texcoord0 : TEXCOORD0 = vec4(0.0, 0.0, 0.0, 1.0);
vec4 v_texcoord1 : TEXCOORD1 = vec4(0.0, 0.0, 0.0, 1.0);
vec4 v_texcoord2 : TEXCOORD2 = vec4(0.0, 0.0, 0.0, 1.0);
vec4 v_texcoord3 : TEXCOORD3 = vec4(0.0, 0.0, 0.0, 1.0);
vec4 v_texcoord4 : TEXCOORD4 = vec4(0.0, 0.0, 0.0, 1.0);
vec4 v_fog       : TEXCOORD7 = vec4(0.0, 0.0, 0.0, 0.0);
// The ground variant only (vs_ff[_unlit]_dyn / fs_ff_dyn): the sun map's uv and depth, w: facing the sun.
vec4 v_shadow    : TEXCOORD6 = vec4(0.0, 0.0, 0.0, 0.0);
// The Sea2 ports only (vs_port_sea2 / fs_port_sea2): the game's foam layer, folded into the sea's draw: the foam
// texture's uv and the layer's alpha (sea_sea2foam_vertex_shader's oT3).
vec4 v_foam      : TEXCOORD6 = vec4(0.0, 0.0, 0.0, 0.0);
// Per-pixel lighting variants only (vs_ff_lit / fs_ff_lit*, fs_ff_baked*): view-space normal and position,
// material diffuse and ambient colour of the vertex.
vec4 v_normal     : NORMAL    = vec4(0.0, 0.0, -1.0, 0.0);
vec4 v_position   : TEXCOORD5 = vec4(0.0, 0.0, 0.0, 1.0);
vec4 v_matDiffuse : COLOR2    = vec4(0.0, 0.0, 0.0, 0.0);
vec4 v_matAmbient : COLOR3    = vec4(0.0, 0.0, 0.0, 0.0);

vec4 a_position  : POSITION;
vec3 a_normal    : NORMAL;
vec4 a_color0    : COLOR0;
vec4 a_color1    : COLOR1;
vec4 a_texcoord0 : TEXCOORD0;
vec4 a_texcoord1 : TEXCOORD1;
vec4 a_texcoord2 : TEXCOORD2;
vec4 a_texcoord3 : TEXCOORD3;
vec4 a_texcoord4 : TEXCOORD4;
vec4 a_texcoord5 : TEXCOORD5;
vec4 a_texcoord6 : TEXCOORD6;
vec4 a_texcoord7 : TEXCOORD7;
vec4 a_weight    : BLENDWEIGHT;
uvec4 a_indices  : BLENDINDICES; // unsigned: bgfx binds non-normalised Uint8 as GL_UNSIGNED_BYTE (glVertexAttribIPointer)
                                 // and VK_FORMAT_R8G8B8A8_UINT; a signed input is undefined on both (PowerVR Vulkan)
vec3 a_tangent   : TANGENT;
vec3 a_bitangent : BITANGENT;
// Instance data of a GPU-skinned draw (one instance): x = its first row in the frame's bone palette texture
// (shim bone_palettes.hpp), outside the uniforms so that it does not re-commit the program's uniform block per draw.
vec4 i_data0     : TEXCOORD7;
