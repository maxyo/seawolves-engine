#include "storm/render/pipelines.hpp"
#include "effects/scene_readiness.hpp"
#include "device.hpp"
#include "effects/effects.hpp"
#include "effects/upscale.hpp"
#include "storm/fx/skin_tone.hpp"
#include "perf.hpp"
#include "ports.hpp"
#include "texture_fades.hpp"
#include "util.hpp"
#include "loading_transaction.hpp"
#include <cfloat>


namespace storm::shim
{

// ---------------------------------------------------------------------------
// vertex layouts
namespace
{
bgfx::Attrib::Enum texcoord_attrib(uint32_t index)
{
    return static_cast<bgfx::Attrib::Enum>(bgfx::Attrib::TexCoord0 + std::min(index, 7u));
}

void finish_layout(LayoutInfo &info, uint32_t stream, uint32_t stride)
{
    bgfx::VertexLayout &layout = info.layout[stream];
    layout.end();
    info.extent[stream] = layout.m_stride;
    if (stride > layout.m_stride)
        layout.m_stride = static_cast<uint16_t>(stride);
    // The hash covers attributes and offsets only; make it unique per stride.
    layout.m_hash ^= static_cast<uint32_t>(layout.m_stride) * 0x9E3779B1u;
    info.stride[stream] = layout.m_stride;
    info.handle[stream] = bgfx::createVertexLayout(layout);
}
} // namespace

const LayoutInfo *Device::fvf_layout(DWORD fvf, uint32_t stride)
{
    const uint64_t key = fvf | (static_cast<uint64_t>(stride) << 32);
    auto &slot = fvf_layouts[key];
    if (slot)
        return slot.get();
    slot = std::make_unique<LayoutInfo>();
    LayoutInfo &info = *slot;
    bgfx::VertexLayout &layout = info.layout[0];
    layout.begin();
    const DWORD position = fvf & D3DFVF_POSITION_MASK;
    if (position == D3DFVF_XYZRHW || position == D3DFVF_XYZW)
    {
        layout.add(bgfx::Attrib::Position, 4, bgfx::AttribType::Float);
        info.rhw = position == D3DFVF_XYZRHW;
    }
    else
    {
        layout.add(bgfx::Attrib::Position, 3, bgfx::AttribType::Float);
        if (position == D3DFVF_XYZB2 && (fvf & D3DFVF_LASTBETA_UBYTE4))
        {
            // GPU skinning: one blend weight, bone indices in the last beta (GEOS::AVERTEX0).
            layout.add(bgfx::Attrib::Weight, 1, bgfx::AttribType::Float);
            layout.add(bgfx::Attrib::Indices, 4, bgfx::AttribType::Uint8);
            info.skinned = true;
        }
        else if (position >= D3DFVF_XYZB1 && position <= D3DFVF_XYZB5)
        {
            const uint32_t weights = (position - D3DFVF_XYZB1) / 2 + 1;
            layout.skip(static_cast<uint8_t>(weights * 4));
            trap("FVF with blend weights other than XYZB2 | LASTBETA_UBYTE4");
        }
    }
    if (fvf & D3DFVF_NORMAL)
    {
        layout.add(bgfx::Attrib::Normal, 3, bgfx::AttribType::Float);
        info.has_normal = true;
    }
    if (fvf & D3DFVF_PSIZE)
        layout.skip(4);
    if (fvf & D3DFVF_DIFFUSE)
    {
        layout.add(bgfx::Attrib::Color0, 4, bgfx::AttribType::Uint8, true);
        info.has_color0 = true;
    }
    if (fvf & D3DFVF_SPECULAR)
    {
        layout.add(bgfx::Attrib::Color1, 4, bgfx::AttribType::Uint8, true);
        info.has_color1 = true;
    }
    info.texcoord_count = static_cast<uint8_t>((fvf & D3DFVF_TEXCOUNT_MASK) >> D3DFVF_TEXCOUNT_SHIFT);
    for (uint32_t i = 0; i < info.texcoord_count; ++i)
    {
        static const uint8_t sizes[4] = {2, 3, 4, 1};
        const uint8_t size = sizes[(fvf >> (16 + i * 2)) & 3];
        info.texcoord_size[i] = size;
        layout.add(texcoord_attrib(i), size, bgfx::AttribType::Float);
    }
    info.stream_count = 1;
    finish_layout(info, 0, stride);
    return &info;
}

const LayoutInfo *Device::build_decl_layout(VertexDeclaration &decl)
{
    // A stream's stride is the one bound with SetStreamSource, or the attributes' extent when that is smaller (0).
    for (const auto &cached : decl.layouts)
    {
        bool same = true;
        for (uint32_t i = 0; same && i < cached->stream_count; ++i)
            same = cached->stride[i] == std::max<uint32_t>(s.stream_stride[i], cached->extent[i]);
        if (same)
            return cached.get();
    }
    decl.layouts.push_back(std::make_unique<LayoutInfo>());
    LayoutInfo &info = *decl.layouts.back();
    uint32_t streams = 0;
    for (const auto &element : decl.elements)
        streams = std::max<uint32_t>(streams, element.Stream + 1u);
    if (streams > kMaxStreams)
    {
        decl.layouts.pop_back();
        trap("vertex declaration with more than 4 streams");
        return nullptr;
    }
    for (uint32_t stream = 0; stream < streams; ++stream)
    {
        std::vector<D3DVERTEXELEMENT9> elements;
        for (const auto &element : decl.elements)
            if (element.Stream == stream)
                elements.push_back(element);
        std::sort(elements.begin(), elements.end(),
                  [](const D3DVERTEXELEMENT9 &a, const D3DVERTEXELEMENT9 &b) { return a.Offset < b.Offset; });
        bgfx::VertexLayout &layout = info.layout[stream];
        layout.begin();
        uint32_t offset = 0;
        for (const auto &element : elements)
        {
            if (element.Offset > offset)
                layout.skip(static_cast<uint8_t>(element.Offset - offset));
            uint8_t count = 0;
            uint32_t bytes = 0;
            bgfx::AttribType::Enum type = bgfx::AttribType::Float;
            bool normalized = false;
            switch (element.Type)
            {
            case D3DDECLTYPE_FLOAT1:
            case D3DDECLTYPE_FLOAT2:
            case D3DDECLTYPE_FLOAT3:
            case D3DDECLTYPE_FLOAT4:
                count = static_cast<uint8_t>(element.Type - D3DDECLTYPE_FLOAT1 + 1);
                bytes = count * 4u;
                break;
            case D3DDECLTYPE_D3DCOLOR:
            case D3DDECLTYPE_UBYTE4N:
                count = 4, bytes = 4, type = bgfx::AttribType::Uint8, normalized = true;
                break;
            case D3DDECLTYPE_UBYTE4:
                count = 4, bytes = 4, type = bgfx::AttribType::Uint8;
                break;
            case D3DDECLTYPE_SHORT2:
            case D3DDECLTYPE_SHORT2N:
                count = 2, bytes = 4, type = bgfx::AttribType::Int16, normalized = element.Type == D3DDECLTYPE_SHORT2N;
                break;
            case D3DDECLTYPE_SHORT4:
            case D3DDECLTYPE_SHORT4N:
                count = 4, bytes = 8, type = bgfx::AttribType::Int16, normalized = element.Type == D3DDECLTYPE_SHORT4N;
                break;
            default:
                decl.layouts.pop_back();
                trap("vertex declaration element type");
                return nullptr;
            }
            bgfx::Attrib::Enum attrib = bgfx::Attrib::Count;
            switch (element.Usage)
            {
            case D3DDECLUSAGE_POSITION:
                attrib = bgfx::Attrib::Position;
                break;
            case D3DDECLUSAGE_POSITIONT:
                attrib = bgfx::Attrib::Position;
                info.rhw = true;
                break;
            case D3DDECLUSAGE_NORMAL:
                attrib = bgfx::Attrib::Normal;
                info.has_normal = true;
                break;
            case D3DDECLUSAGE_COLOR:
                attrib = element.UsageIndex == 0 ? bgfx::Attrib::Color0 : bgfx::Attrib::Color1;
                (element.UsageIndex == 0 ? info.has_color0 : info.has_color1) = true;
                break;
            case D3DDECLUSAGE_TEXCOORD:
                attrib = texcoord_attrib(element.UsageIndex);
                if (element.UsageIndex < 8)
                    info.texcoord_size[element.UsageIndex] = count;
                info.texcoord_count = std::max<uint8_t>(info.texcoord_count, element.UsageIndex + 1);
                break;
            case D3DDECLUSAGE_BLENDWEIGHT:
                attrib = bgfx::Attrib::Weight;
                break;
            case D3DDECLUSAGE_BLENDINDICES:
                attrib = bgfx::Attrib::Indices;
                break;
            case D3DDECLUSAGE_TANGENT:
                attrib = bgfx::Attrib::Tangent;
                break;
            case D3DDECLUSAGE_BINORMAL:
                attrib = bgfx::Attrib::Bitangent;
                break;
            default:
                break;
            }
            if (attrib == bgfx::Attrib::Count || layout.has(attrib))
                layout.skip(static_cast<uint8_t>(bytes));
            else
                layout.add(attrib, count, type, normalized);
            offset = element.Offset + bytes;
        }
        finish_layout(info, stream, s.stream_stride[stream]);
    }
    info.stream_count = static_cast<uint8_t>(streams);
    return &info;
}

const LayoutInfo *Device::current_layout()
{
    if (s.decl)
        return build_decl_layout(*s.decl);
    return fvf_layout(s.fvf, s.stream_stride[0]);
}

// ---------------------------------------------------------------------------
// uniforms
void Device::set_uniform(UniformId id, const float *data, uint16_t count)
{
    Uniform &uniform = uniforms[id];
    const size_t per = (id == U_PROJ || id == U_TEXMTX) ? 16 : 4;
    const size_t floats = per * count;
    if (uniform.sent && uniform.last.size() == floats && memcmp(uniform.last.data(), data, floats * sizeof(float)) == 0)
        return;
    uniform.last.assign(data, data + floats);
    uniform.sent = true;
    bgfx::setUniform(uniform.handle, data, count);
    ++perf().uniform_updates;
    perf().uniform_bytes += floats * sizeof(float);
    uniforms_changed = true;
}

// Buffers drawn in a later frame than they were written in: a range of bgfx's shared dynamic buffers, so creating one
// makes no GL buffer (one GL buffer each made opening a scene or a screen create hundreds in a frame; bgfx frees a
// shared buffer once its last range is gone). A vertex buffer's range is created in the layout it is drawn with, so it
// starts at a whole vertex of that stride; drawn with another stride, it is created again. Rewrites update the written
// range in place.
void Device::upload_buffer(VertexBuffer &vb, const bgfx::VertexLayout &layout)
{
    const uint32_t size = static_cast<uint32_t>(vb.shadow.size());
    const uint32_t stride = layout.getStride();
    const uint32_t vertices = size / stride; // a trailing partial vertex is never drawn
    if (bgfx::isValid(vb.handle) && vb.stride != stride)
    {
        bgfx::destroy(vb.handle);
        vb.handle = BGFX_INVALID_HANDLE;
    }
    if (!bgfx::isValid(vb.handle))
    {
        vb.handle = bgfx::createDynamicVertexBuffer(bgfx::copy(vb.shadow.data(), vertices * stride), layout);
        vb.stride = stride;
        ++perf().buffer_creates;
        perf().buffer_upload_bytes += vertices * stride;
    }
    else if (vb.dirty_begin < vb.dirty_end)
    {
        const uint32_t first = vb.dirty_begin / stride;
        const uint32_t end = std::min((std::min(vb.dirty_end, size) + stride - 1) / stride, vertices);
        if (end > first)
            bgfx::update(vb.handle, first, bgfx::copy(vb.shadow.data() + first * stride, (end - first) * stride));
        perf().buffer_upload_bytes += (end - first) * stride;
    }
    else
        return;
    ++perf().buffer_uploads;
    perf_resize(&render::PerfCounters::buffer_gpu_bytes, vb.gpu_bytes, size);
    vb.gpu_bytes = size;
    vb.dirty_begin = UINT32_MAX;
    vb.dirty_end = 0;
}

void Device::upload_buffer(IndexBuffer &ib, bool index32)
{
    const uint32_t size = static_cast<uint32_t>(ib.shadow.size());
    const uint32_t index_size = index32 ? 4 : 2;
    const uint32_t indices = size / index_size; // a trailing partial index is never drawn
    if (!bgfx::isValid(ib.handle))
    {
        ib.handle = bgfx::createDynamicIndexBuffer(bgfx::copy(ib.shadow.data(), indices * index_size),
                                                   index32 ? BGFX_BUFFER_INDEX32 : BGFX_BUFFER_NONE);
        ++perf().buffer_creates;
        perf().buffer_upload_bytes += indices * index_size;
    }
    else if (ib.dirty_begin < ib.dirty_end)
    {
        const uint32_t first = ib.dirty_begin / index_size;
        const uint32_t end = std::min((std::min(ib.dirty_end, size) + index_size - 1) / index_size, indices);
        if (end > first)
            bgfx::update(ib.handle, first, bgfx::copy(ib.shadow.data() + first * index_size, (end - first) * index_size));
        perf().buffer_upload_bytes += (end - first) * index_size;
    }
    else
        return;
    ++perf().buffer_uploads;
    perf_resize(&render::PerfCounters::buffer_gpu_bytes, ib.gpu_bytes, size);
    ib.gpu_bytes = size;
    ib.dirty_begin = UINT32_MAX;
    ib.dirty_end = 0;
}

void Device::forget_transient(const VertexBuffer *vb)
{
    std::erase_if(transient_ranges, [vb](const TransientRange &r) { return r.vb == vb; });
}

void Device::invalidate_uniforms()
{
    for (auto &uniform : uniforms)
        uniform.sent = false;
}

namespace
{
// Bit mask of the arguments (0, 1, 2) a texture operation reads.
uint32_t op_args(DWORD op)
{
    switch (op)
    {
    case D3DTOP_SELECTARG1:
    case D3DTOP_PREMODULATE:
        return 2;
    case D3DTOP_SELECTARG2:
        return 4;
    case D3DTOP_MULTIPLYADD:
    case D3DTOP_LERP:
        return 7;
    default:
        return 6;
    }
}

bool uses_texture(DWORD op, DWORD arg0, DWORD arg1, DWORD arg2)
{
    if (op == D3DTOP_DISABLE)
        return false;
    if (op == D3DTOP_BLENDTEXTUREALPHA || op == D3DTOP_BLENDTEXTUREALPHAPM)
        return true;
    const uint32_t mask = op_args(op);
    return ((mask & 1) && (arg0 & D3DTA_SELECTMASK) == D3DTA_TEXTURE) ||
           ((mask & 2) && (arg1 & D3DTA_SELECTMASK) == D3DTA_TEXTURE) ||
           ((mask & 4) && (arg2 & D3DTA_SELECTMASK) == D3DTA_TEXTURE);
}

float encode_arg(DWORD arg)
{
    return static_cast<float>((arg & D3DTA_SELECTMASK) + 16 * ((arg >> 4) & 3));
}

float material_source(DWORD source, bool color_vertex, const LayoutInfo &layout)
{
    if (!color_vertex)
        return 0.0f;
    if (source == D3DMCS_COLOR1)
        return layout.has_color0 ? 1.0f : 0.0f;
    if (source == D3DMCS_COLOR2)
        return layout.has_color1 ? 2.0f : 0.0f;
    return 0.0f;
}
} // namespace

void Device::apply_ff_uniforms(const LayoutInfo &layout, bool vs_port, bool ps_port, const effects::FfDraw &fx, bool point_draw)
{
    const bgfx::Caps *caps = Context::instance().caps();
    const bool flip = Context::instance().flip_y();
    const DWORD *rs = s.rs;
    const bool point_sprite = point_draw && rs[D3DRS_POINTSPRITEENABLE];
    const float vw = static_cast<float>(std::max<DWORD>(1, s.viewport.Width));
    const float vh = static_cast<float>(std::max<DWORD>(1, s.viewport.Height));
    // Pre-transformed vertices are in the pixels the game addresses; the half-pixel shift of D3D9 is a half pixel
    // of the store (fewer pixels on a scaled scene store), like point sizes.
    const std::array<float, 2> scale = target_scale();

    // D3D clip space -> backend clip space (row-vector matrix, applied last).
    D3DMATRIX fix = identity();
    const float fy = flip ? -1.0f : 1.0f;
    const float bias = as_float(rs[D3DRS_DEPTHBIAS]);
    fix.m[1][1] = fy;
    fix.m[3][0] = 1.0f / (vw * scale[0]);
    fix.m[3][1] = -fy / (vh * scale[1]);
    if (caps->homogeneousDepth)
    {
        // D3D keeps z == 0 and z == w; after the remap to [-1, 1] rounding would clip them,
        // so the range is shrunk by a few depth-buffer steps.
        const float keep = 1.0f - 2e-6f;
        fix.m[2][2] = 2.0f * keep;
        fix.m[3][2] = (-1.0f + 2.0f * bias) * keep;
    }
    else
        fix.m[3][2] = bias;

    D3DMATRIX world_view, proj;
    multiply(s.world, s.view, world_view);
    if (layout.rhw)
    {
        D3DMATRIX screen = identity();
        screen.m[0][0] = 2.0f / vw;
        screen.m[1][1] = -2.0f / vh;
        screen.m[3][0] = -1.0f - 2.0f * static_cast<float>(s.viewport.X) / vw;
        screen.m[3][1] = 1.0f + 2.0f * static_cast<float>(s.viewport.Y) / vh;
        multiply(screen, fix, proj);
    }
    else
        multiply(s.proj, fix, proj);
    if (!vs_port)
        bgfx::setTransform(&world_view.m[0][0]);
    set_uniform(U_PROJ, &proj.m[0][0], 1);

    if (vs_port)
    {
        // vs_1_1 programs output D3D clip space; they apply the fix-up matrix themselves.
        set_uniform(U_PROJ, &fix.m[0][0], 1);
        set_uniform(U_VSC, &s.vs_const[0][0], s.vs->port->vsc_count); // the registers the port declares
    }
    if (ps_port)
    {
        set_uniform(U_PSC, &s.ps_const[0][0], kPsConstants);
        // texbem/texbeml read the stage's bump environment matrix: (M00, M01, M10, M11) per stage.
        float bem[kFfStages][4];
        for (uint32_t stage = 0; stage < kFfStages; ++stage)
            for (uint32_t i = 0; i < 4; ++i)
                bem[stage][i] = as_float(s.tss[stage][D3DTSS_BUMPENVMAT00 + i]);
        set_uniform(U_BEM, &bem[0][0], kFfStages);
    }

    const bool lighting = rs[D3DRS_LIGHTING] != 0 && !layout.rhw;
    const bool color_vertex = rs[D3DRS_COLORVERTEX] != 0;
    const bool fog = rs[D3DRS_FOGENABLE] != 0 && !layout.rhw;
    // D3D9 fog: a table (pixel) mode wins and is evaluated from depth; otherwise the fixed-function
    // vertex mode is used, and with no mode at all the factor comes from the vertex (the vertex
    // shader's oFog, or the specular alpha of fixed-function vertices).
    enum : uint32_t { kFogOff = 0, kFogFactor = 4, kFogAerial = 5 };
    uint32_t fog_mode = kFogOff;
    bool fog_from_clip_depth = false, fog_from_specular = false;
    if (fog)
    {
        if (rs[D3DRS_FOGTABLEMODE] != D3DFOG_NONE)
        {
            fog_mode = rs[D3DRS_FOGTABLEMODE];
            fog_from_clip_depth = vs_port;
        }
        else if (vs_port)
            fog_mode = kFogFactor;
        else if (rs[D3DRS_FOGVERTEXMODE] != D3DFOG_NONE)
            fog_mode = rs[D3DRS_FOGVERTEXMODE];
        else
        {
            fog_mode = kFogFactor;
            fog_from_specular = true;
        }
    }
    // Effects: the game's exp fog with the aerial perspective (effects/lighting.hpp, ff_finish.sh mode 5).
    std::array<float, 2> aerial_haze{};
    if (fog_mode == D3DFOG_EXP && effects::Host::instance().lighting().aerial_fog(s.view, vs_port, aerial_haze))
        fog_mode = kFogAerial;

    if (!vs_port)
    {
        float light_pos[kMaxLights][4] = {}, light_dir[kMaxLights][4] = {}, light_diffuse[kMaxLights][4] = {};
        float light_ambient[kMaxLights][4] = {}, light_specular[kMaxLights][4] = {}, light_att[kMaxLights][4] = {};
        uint32_t light_count = 0;
        if (lighting)
        {
            // Original/cube/unsupported draw paths retain the raw authored light law. Never pass a summed
            // center into ff_vertex. Each expanded source remembers its original enabled slot for the masks.
            struct VertexLight { D3DLIGHT9 light; uint32_t slot; float score; };
            std::array<VertexLight,kMaxLights> vertex_lights{};
            size_t vertex_light_count=0;
            const bool expand_raw = std::any_of(lights.begin(),lights.end(),[&](const auto &entry){
                return entry.enabled && entry.aggregate.get() && entry.aggregate.get()->count>1;
            });
            const auto append_vertex_light = [&](VertexLight item) {
                const auto &l=item.light;
                if (expand_raw && l.Type != D3DLIGHT_DIRECTIONAL)
                {
                    const float dx=l.Position.x-s.world.m[3][0],dy=l.Position.y-s.world.m[3][1],dz=l.Position.z-s.world.m[3][2];
                    const float d=std::sqrt(dx*dx+dy*dy+dz*dz);
                    item.score = d<l.Range ? (item.score>0 ? item.score : std::max({l.Diffuse.r,l.Diffuse.g,l.Diffuse.b})) /
                        std::max(l.Attenuation0+l.Attenuation1*d+l.Attenuation2*d*d,1e-6f) : 0.0f;
                }
                if (vertex_light_count<kMaxLights) vertex_lights[vertex_light_count++]=item;
                else
                {
                    auto weakest=std::min_element(vertex_lights.begin(),vertex_lights.end(),[](const auto &a,const auto &b){return a.score<b.score;});
                    if (item.score>weakest->score) *weakest=item;
                }
            };
            uint32_t enabled_slot = 0;
            const auto origin = effects::Host::instance().lighting().world_origin();
            for (const Light &entry : lights)
            {
                if (!entry.enabled || enabled_slot >= kMaxLights) continue;
                const uint32_t source_slot = enabled_slot++;
                const auto *g = entry.aggregate.get();
                const bool evaluated = (fx.vertex_diffuse_off & (1u<<source_slot)) &&
                    (!rs[D3DRS_SPECULARENABLE] || (fx.vertex_specular_off & (1u<<source_slot)));
                if (!g || g->count == 1 || evaluated)
                {
                    append_vertex_light({entry.light,source_slot, entry.light.Type == D3DLIGHT_DIRECTIONAL ? 1e30f : 0.0f});
                    continue;
                }
                for (uint32_t i=0;i<g->count;++i)
                {
                    const auto &m=entry.aggregate.snapshot->members[g->first+i];
                    D3DLIGHT9 raw=entry.light;
                    raw.Position={m.position[0]-origin[0],m.position[1]-origin[1],m.position[2]-origin[2]};
                    raw.Diffuse={m.color[0],m.color[1],m.color[2],1.0f};
                    raw.Range=m.authored_range;
                    raw.Attenuation0=m.attenuation[0];raw.Attenuation1=m.attenuation[1];raw.Attenuation2=m.attenuation[2];
                    append_vertex_light({raw,source_slot,std::max({m.steady[0],m.steady[1],m.steady[2]})});
                }
            }
            for (size_t vertex_index=0;vertex_index<vertex_light_count;++vertex_index)
            {
                const auto &entry=vertex_lights[vertex_index];
                if (light_count >= kMaxLights) break;
                const D3DLIGHT9 &l = entry.light;
                const D3DMATRIX &v = s.view;
                float *p = light_pos[light_count], *d = light_dir[light_count];
                p[0] = l.Position.x * v.m[0][0] + l.Position.y * v.m[1][0] + l.Position.z * v.m[2][0] + v.m[3][0];
                p[1] = l.Position.x * v.m[0][1] + l.Position.y * v.m[1][1] + l.Position.z * v.m[2][1] + v.m[3][1];
                p[2] = l.Position.x * v.m[0][2] + l.Position.y * v.m[1][2] + l.Position.z * v.m[2][2] + v.m[3][2];
                p[3] = static_cast<float>(l.Type);
                d[0] = l.Direction.x * v.m[0][0] + l.Direction.y * v.m[1][0] + l.Direction.z * v.m[2][0];
                d[1] = l.Direction.x * v.m[0][1] + l.Direction.y * v.m[1][1] + l.Direction.z * v.m[2][1];
                d[2] = l.Direction.x * v.m[0][2] + l.Direction.y * v.m[1][2] + l.Direction.z * v.m[2][2];
                const float length = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
                if (length > 0.0f)
                    d[0] /= length, d[1] /= length, d[2] /= length;
                d[3] = l.Range;
                // Effects: terms the fragment shader evaluates per pixel leave the vertex sum.
                if (!(fx.vertex_diffuse_off & (1u << entry.slot)))
                    memcpy(light_diffuse[light_count], &l.Diffuse, sizeof(float) * 4);
                memcpy(light_ambient[light_count], &l.Ambient, sizeof(float) * 4);
                if (!(fx.vertex_specular_off & (1u << entry.slot)))
                    memcpy(light_specular[light_count], &l.Specular, sizeof(float) * 4);
                light_att[light_count][0] = l.Attenuation0;
                light_att[light_count][1] = l.Attenuation1;
                light_att[light_count][2] = l.Attenuation2;
                ++light_count;
            }
            set_uniform(U_LIGHTPOS, &light_pos[0][0], kMaxLights);
            set_uniform(U_LIGHTDIR, &light_dir[0][0], kMaxLights);
            set_uniform(U_LIGHTDIFFUSE, &light_diffuse[0][0], kMaxLights);
            set_uniform(U_LIGHTAMBIENT, &light_ambient[0][0], kMaxLights);
            set_uniform(U_LIGHTATT, &light_att[0][0], kMaxLights);
            if (rs[D3DRS_SPECULARENABLE])
                set_uniform(U_LIGHTSPECULAR, &light_specular[0][0], kMaxLights);
            float material[4][4];
            memcpy(material[0], &s.material.Diffuse, sizeof(float) * 4);
            memcpy(material[1], &s.material.Ambient, sizeof(float) * 4);
            memcpy(material[2], &s.material.Specular, sizeof(float) * 4);
            memcpy(material[3], &s.material.Emissive, sizeof(float) * 4);
            set_uniform(U_MATERIAL, &material[0][0], 4);
            float ambient[4] = {};
            if (!fx.vertex_ambient_off)
                color_to_float(rs[D3DRS_AMBIENT], ambient);
            set_uniform(U_AMBIENT, ambient, 1);
        }
        const float vs[7][4] = {
            {layout.rhw ? 1.0f : 0.0f, lighting ? 1.0f : 0.0f, layout.has_color0 ? 1.0f : 0.0f,
             layout.has_color1 ? 1.0f : 0.0f},
            {rs[D3DRS_NORMALIZENORMALS] ? 1.0f : 0.0f, static_cast<float>(light_count),
             rs[D3DRS_RANGEFOGENABLE] ? 1.0f : 0.0f, layout.has_normal ? 1.0f : 0.0f},
            {material_source(rs[D3DRS_DIFFUSEMATERIALSOURCE], color_vertex, layout),
             material_source(rs[D3DRS_AMBIENTMATERIALSOURCE], color_vertex, layout),
             material_source(rs[D3DRS_EMISSIVEMATERIALSOURCE], color_vertex, layout),
             material_source(rs[D3DRS_SPECULARMATERIALSOURCE], color_vertex, layout)},
            {rs[D3DRS_SPECULARENABLE] ? 1.0f : 0.0f, s.material.Power, fog_from_specular ? 1.0f : 0.0f,
             as_float(rs[D3DRS_POINTSIZE]) * scale[1]},
            {point_sprite ? 1.0f : 0.0f, caps->originBottomLeft ? 1.0f : -1.0f,
             static_cast<float>(std::lround(s.viewport.X * scale[0])),
             static_cast<float>(std::lround(s.viewport.Y * scale[1]))},
            {static_cast<float>(std::lround((s.viewport.X + s.viewport.Width) * scale[0]) - std::lround(s.viewport.X * scale[0])),
             static_cast<float>(std::lround((s.viewport.Y + s.viewport.Height) * scale[1]) - std::lround(s.viewport.Y * scale[1])),
             std::max(0.0f, as_float(rs[D3DRS_POINTSIZE_MIN])) * scale[1],
             std::min(64.0f, as_float(rs[D3DRS_POINTSIZE_MAX])) * scale[1]},
            {as_float(rs[D3DRS_POINTSCALE_A]), as_float(rs[D3DRS_POINTSCALE_B]), as_float(rs[D3DRS_POINTSCALE_C]),
             point_draw && rs[D3DRS_POINTSCALEENABLE] && !layout.rhw ? vh : 0.0f},
        };
        set_uniform(U_VS, &vs[0][0], 7);

        float clip[4] = {};
        if (rs[D3DRS_CLIPPLANEENABLE] & 1)
        {
            // World-space plane -> view space: p_view = p_world * inverse-transpose(view).
            // The view matrix is rigid, so rotate the normal and re-derive the distance.
            const float *p = s.clip_plane[0];
            const D3DMATRIX &v = s.view;
            clip[0] = p[0] * v.m[0][0] + p[1] * v.m[1][0] + p[2] * v.m[2][0];
            clip[1] = p[0] * v.m[0][1] + p[1] * v.m[1][1] + p[2] * v.m[2][1];
            clip[2] = p[0] * v.m[0][2] + p[1] * v.m[1][2] + p[2] * v.m[2][2];
            clip[3] = p[3] - (clip[0] * v.m[3][0] + clip[1] * v.m[3][1] + clip[2] * v.m[3][2]);
        }
        set_uniform(U_CLIP, clip, 1);

        float tex_cfg[kFfStages][4] = {};
        float tex_mtx[kFfStages][16];
        bool any_matrix = false;
        for (uint32_t stage = 0; stage < kFfStages; ++stage)
        {
            const DWORD index = s.tss[stage][D3DTSS_TEXCOORDINDEX];
            const DWORD flags = s.tss[stage][D3DTSS_TEXTURETRANSFORMFLAGS];
            const uint32_t coord = std::min<uint32_t>(index & 0xFFFF, 3);
            const uint32_t texgen = (index >> 16) & 7;
            const uint32_t count = flags & 0xFF;
            tex_cfg[stage][0] = static_cast<float>(coord);
            tex_cfg[stage][1] = static_cast<float>(std::min(texgen, 3u));
            tex_cfg[stage][2] = layout.rhw ? 0.0f : static_cast<float>(count);
            tex_cfg[stage][3] = (flags & D3DTTFF_PROJECTED) && !layout.rhw ? 1.0f : 0.0f;
            D3DMATRIX m = s.texture[stage];
            if (count != 0 && texgen == 0)
            {
                // D3D pads short coordinates with 1 in the next component; the shader sees (u, v, 0, 1).
                const uint32_t size = layout.texcoord_size[coord] ? layout.texcoord_size[coord] : 2;
                if (size < 3)
                    for (int c = 0; c < 4; ++c)
                        m.m[3][c] = m.m[size][c];
            }
            memcpy(tex_mtx[stage], &m.m[0][0], sizeof(float) * 16);
            any_matrix |= count != 0;
        }
        set_uniform(U_TEXCFG, &tex_cfg[0][0], kFfStages);
        if (any_matrix)
            set_uniform(U_TEXMTX, &tex_mtx[0][0], kFfStages);
    }

    // fragment side
    float stage_data[kFfStages * 3][4] = {};
    uint32_t stage_count = 0;
    if (!ps_port)
    {
        for (uint32_t stage = 0; stage < kFfStages; ++stage)
        {
            const DWORD *t = s.tss[stage];
            if (t[D3DTSS_COLOROP] == D3DTOP_DISABLE)
                break;
            const bool bound = s.textures[stage] != nullptr;
            if (!bound && (uses_texture(t[D3DTSS_COLOROP], t[D3DTSS_COLORARG0], t[D3DTSS_COLORARG1], t[D3DTSS_COLORARG2]) ||
                           uses_texture(t[D3DTSS_ALPHAOP], t[D3DTSS_ALPHAARG0], t[D3DTSS_ALPHAARG1], t[D3DTSS_ALPHAARG2])))
                break;
            float *c = stage_data[stage * 3], *a = stage_data[stage * 3 + 1], *m = stage_data[stage * 3 + 2];
            c[0] = static_cast<float>(t[D3DTSS_COLOROP]);
            c[1] = encode_arg(t[D3DTSS_COLORARG1]);
            c[2] = encode_arg(t[D3DTSS_COLORARG2]);
            c[3] = encode_arg(t[D3DTSS_COLORARG0]);
            a[0] = static_cast<float>(t[D3DTSS_ALPHAOP]);
            a[1] = encode_arg(t[D3DTSS_ALPHAARG1]);
            a[2] = encode_arg(t[D3DTSS_ALPHAARG2]);
            a[3] = encode_arg(t[D3DTSS_ALPHAARG0]);
            m[0] = (t[D3DTSS_RESULTARG] & D3DTA_SELECTMASK) == D3DTA_TEMP ? 1.0f : 0.0f;
            m[1] = (t[D3DTSS_TEXTURETRANSFORMFLAGS] & D3DTTFF_PROJECTED) && !layout.rhw ? 1.0f : 0.0f;
            // Above 1 while the stage fades from its previous texture (texture_fade.hpp): 1 + its share.
            m[2] = bound ? 1.0f + stage_fade_[stage] : 0.0f;
            m[3] = point_sprite ? 1.0f : 0.0f; // the stars (weather/stars.cpp) are point sprites
            stage_count = stage + 1;
        }
        set_uniform(U_STAGE, &stage_data[0][0], kFfStages * 3);
        set_uniform(U_TOWN_RATIO, town_ratio_, 1);
    }
    float fs[4][4] = {};
    color_to_float(rs[D3DRS_TEXTUREFACTOR], fs[0]);
    color_to_float(rs[D3DRS_FOGCOLOR], fs[1]);
    fs[1][3] = static_cast<float>(fog_mode);
    fs[3][3] = fog_from_clip_depth ? 1.0f : 0.0f;
    fs[2][0] = fog_mode == kFogAerial ? aerial_haze[0] : as_float(rs[D3DRS_FOGSTART]);
    fs[2][1] = fog_mode == kFogAerial ? aerial_haze[1] : as_float(rs[D3DRS_FOGEND]);
    fs[2][2] = as_float(rs[D3DRS_FOGDENSITY]);
    fs[2][3] = static_cast<float>(rs[D3DRS_ALPHAREF] & 255) / 255.0f;
    fs[3][0] = rs[D3DRS_ALPHATESTENABLE] && rs[D3DRS_ALPHAFUNC] != D3DCMP_ALWAYS ? static_cast<float>(rs[D3DRS_ALPHAFUNC])
                                                                               : 0.0f;
    fs[3][1] = rs[D3DRS_SPECULARENABLE] && !ps_port ? 1.0f : 0.0f;
    fs[3][2] = static_cast<float>(stage_count);
    set_uniform(U_FS, &fs[0][0], 4);
}

// ---------------------------------------------------------------------------
// render state
uint64_t Device::render_state_bits(D3DPRIMITIVETYPE type, bool *stencil, uint32_t *fstencil)
{
    const DWORD *rs = s.rs;
    uint64_t bits = 0;
    const DWORD mask = rs[D3DRS_COLORWRITEENABLE];
    if (mask & D3DCOLORWRITEENABLE_RED)
        bits |= BGFX_STATE_WRITE_R;
    if (mask & D3DCOLORWRITEENABLE_GREEN)
        bits |= BGFX_STATE_WRITE_G;
    if (mask & D3DCOLORWRITEENABLE_BLUE)
        bits |= BGFX_STATE_WRITE_B;
    if (mask & D3DCOLORWRITEENABLE_ALPHA)
        bits |= BGFX_STATE_WRITE_A;
    if (rs[D3DRS_ZENABLE] != D3DZB_FALSE && ds)
    {
        bits |= depth_func(rs[D3DRS_ZFUNC]);
        if (rs[D3DRS_ZWRITEENABLE])
            bits |= BGFX_STATE_WRITE_Z;
    }
    if (rs[D3DRS_ALPHABLENDENABLE])
    {
        if (rs[D3DRS_SEPARATEALPHABLENDENABLE])
            bits |= BGFX_STATE_BLEND_FUNC_SEPARATE(blend_factor(rs[D3DRS_SRCBLEND]), blend_factor(rs[D3DRS_DESTBLEND]),
                                                   blend_factor(rs[D3DRS_SRCBLENDALPHA]),
                                                   blend_factor(rs[D3DRS_DESTBLENDALPHA]));
        else
            bits |= BGFX_STATE_BLEND_FUNC(blend_factor(rs[D3DRS_SRCBLEND]), blend_factor(rs[D3DRS_DESTBLEND]));
        switch (rs[D3DRS_BLENDOP])
        {
        case D3DBLENDOP_SUBTRACT:
            bits |= BGFX_STATE_BLEND_EQUATION_SUB;
            break;
        case D3DBLENDOP_REVSUBTRACT:
            bits |= BGFX_STATE_BLEND_EQUATION_REVSUB;
            break;
        case D3DBLENDOP_MIN:
            bits |= BGFX_STATE_BLEND_EQUATION_MIN;
            break;
        case D3DBLENDOP_MAX:
            bits |= BGFX_STATE_BLEND_EQUATION_MAX;
            break;
        default:
            break;
        }
    }
    // Rendering upside down into GL render targets mirrors the winding.
    const bool swap_cull = Context::instance().flip_y();
    if (type >= D3DPT_TRIANGLELIST)
    {
        const DWORD cull = rs[D3DRS_CULLMODE];
        if (cull == D3DCULL_CW)
            bits |= swap_cull ? BGFX_STATE_CULL_CCW : BGFX_STATE_CULL_CW;
        else if (cull == D3DCULL_CCW)
            bits |= swap_cull ? BGFX_STATE_CULL_CW : BGFX_STATE_CULL_CCW;
    }
    switch (type)
    {
    case D3DPT_POINTLIST:
        bits |= BGFX_STATE_PT_POINTS;
        break;
    case D3DPT_LINELIST:
        bits |= BGFX_STATE_PT_LINES;
        break;
    case D3DPT_LINESTRIP:
        bits |= BGFX_STATE_PT_LINESTRIP;
        break;
    case D3DPT_TRIANGLESTRIP:
        bits |= BGFX_STATE_PT_TRISTRIP;
        break;
    default:
        break;
    }
    *stencil = rs[D3DRS_STENCILENABLE] != 0 && ds;
    if (*stencil)
        *fstencil = stencil_func(rs[D3DRS_STENCILFUNC]) | BGFX_STENCIL_FUNC_REF(rs[D3DRS_STENCILREF] & 255) |
                    BGFX_STENCIL_FUNC_RMASK(rs[D3DRS_STENCILMASK] & 255) |
                    stencil_op(rs[D3DRS_STENCILFAIL], BGFX_STENCIL_OP_FAIL_S_SHIFT) |
                    stencil_op(rs[D3DRS_STENCILZFAIL], BGFX_STENCIL_OP_FAIL_Z_SHIFT) |
                    stencil_op(rs[D3DRS_STENCILPASS], BGFX_STENCIL_OP_PASS_Z_SHIFT);
    return bits;
}

uint32_t Device::sampler_flags(uint32_t stage, const TexStore &store) const
{
    const DWORD *sm = s.sampler[stage];
    uint32_t flags = 0;
    const auto address = [](DWORD mode, uint32_t mirror, uint32_t clamp, uint32_t border) -> uint32_t {
        switch (mode)
        {
        case D3DTADDRESS_MIRROR:
            return mirror;
        case D3DTADDRESS_CLAMP:
            return clamp;
        case D3DTADDRESS_BORDER:
            return border;
        default:
            return 0;
        }
    };
    flags |= address(sm[D3DSAMP_ADDRESSU], BGFX_SAMPLER_U_MIRROR, BGFX_SAMPLER_U_CLAMP, BGFX_SAMPLER_U_BORDER);
    flags |= address(sm[D3DSAMP_ADDRESSV], BGFX_SAMPLER_V_MIRROR, BGFX_SAMPLER_V_CLAMP, BGFX_SAMPLER_V_BORDER);
    flags |= address(sm[D3DSAMP_ADDRESSW], BGFX_SAMPLER_W_MIRROR, BGFX_SAMPLER_W_CLAMP, BGFX_SAMPLER_W_BORDER);
    if (sm[D3DSAMP_ADDRESSU] == D3DTADDRESS_BORDER || sm[D3DSAMP_ADDRESSV] == D3DTADDRESS_BORDER ||
        sm[D3DSAMP_ADDRESSW] == D3DTADDRESS_BORDER)
    {
        if (!Context::instance().border_clamp())
        {
            // Without the extension the GPU has no border colour: bgfx clamps to the edge, said once here.
            static bool logged = false;
            if (!logged)
                logf("ERROR: D3DTADDRESS_BORDER sampled on a GPU without texture border clamp: clamped to the edge "
                     "(texels beyond the edge repeat the last row instead of the border colour)");
            logged = true;
        }
        else if (!Context::instance().border_colors())
        {
            // Vulkan without VK_EXT_custom_border_color has three border colours (D3DCOLOR ARGB): the game's own
            // (D3D's default, transparent black) is one of them.
            const DWORD color = sm[D3DSAMP_BORDERCOLOR];
            static bool logged = false;
            if (color != 0x00000000 && color != 0xff000000 && color != 0xffffffff && !logged)
            {
                logf("ERROR: D3DSAMP_BORDERCOLOR 0x%08x sampled on a Vulkan device without custom border colours: "
                     "opaque black is sampled instead",
                     static_cast<unsigned>(color));
                logged = true;
            }
        }
        flags |= BGFX_SAMPLER_BORDER_COLOR(border_color_index(sm[D3DSAMP_BORDERCOLOR]));
    }
    if (sm[D3DSAMP_MINFILTER] == D3DTEXF_ANISOTROPIC)
        flags |= BGFX_SAMPLER_MIN_ANISOTROPIC;
    else if (sm[D3DSAMP_MINFILTER] <= D3DTEXF_POINT)
        flags |= BGFX_SAMPLER_MIN_POINT;
    if (sm[D3DSAMP_MAGFILTER] == D3DTEXF_ANISOTROPIC)
        flags |= BGFX_SAMPLER_MAG_ANISOTROPIC;
    else if (sm[D3DSAMP_MAGFILTER] <= D3DTEXF_POINT)
        flags |= BGFX_SAMPLER_MAG_POINT;
    if (sm[D3DSAMP_MIPFILTER] <= D3DTEXF_POINT || (store.levels <= 1 && !store.full_chain))
        flags |= BGFX_SAMPLER_MIP_POINT;
    // Effects: anisotropic filtering of linearly filtered, mip-mapped world textures (render targets and mip-less
    // interface pictures and fonts keep their filter).
    else if (world_anisotropy && store.kind == TexKind::Tex2D && !store.render_target && !(flags & BGFX_SAMPLER_MIN_POINT))
        flags = (flags & ~(BGFX_SAMPLER_MIN_MASK | BGFX_SAMPLER_MAG_MASK)) | BGFX_SAMPLER_MIN_ANISOTROPIC |
                BGFX_SAMPLER_MAG_ANISOTROPIC;
    return flags;
}

// bgfx samples a border through a palette entry: one entry per distinct D3DSAMP_BORDERCOLOR, kept for the device's
// life (the game uses the default transparent black). Palette entries are bgfx-global; the shim is their only user.
uint32_t Device::border_color_index(DWORD color) const
{
    size_t index = 0;
    while (index < border_colors.size() && border_colors[index] != color)
        ++index;
    constexpr size_t kPaletteEntries = 16; // BGFX_CONFIG_MAX_COLOR_PALETTE (bgfx/src/config.h)
    if (index == border_colors.size())
    {
        if (index >= kPaletteEntries)
        {
            trap("more distinct D3DSAMP_BORDERCOLOR values than bgfx palette entries (first entry used)");
            index = 0;
        }
        else
            border_colors.push_back(color);
    }
    // Set with every use (a few sail draws): the entry survives a bgfx re-initialisation. D3DCOLOR is ARGB, the
    // palette takes RGBA.
    const DWORD c = border_colors[index];
    bgfx::setPaletteColor(static_cast<uint8_t>(index), (c << 8) | (c >> 24));
    return static_cast<uint32_t>(index);
}

// ---------------------------------------------------------------------------
// programmable shaders
namespace
{
// CTechnique passes a reference blob (see ports.hpp) instead of D3D9 bytecode; returns its key
// (empty for anything else) and the blob length in DWORDs including the 0x0000FFFF end token.
std::string_view shader_key(const DWORD *function, size_t *words)
{
    size_t count = 0;
    while (function[count] != 0x0000FFFF && count < 65536)
        ++count;
    *words = count + 1;
    if (count == 0 || function[0] != kShaderRefMagic)
        return {};
    std::string_view key(reinterpret_cast<const char *>(function + 1), (count - 1) * sizeof(DWORD));
    return key.substr(0, key.find('\0'));
}
} // namespace

HRESULT Device::CreateVertexShader(const DWORD *function, VertexShader **shader)
{
    auto *result = new VertexShader();
    size_t words = 0;
    const std::string_view key = shader_key(function, &words);
    result->bytecode.assign(function, function + words);
    result->port = find_port(key, true);
    if (!result->port)
        logf("vertex shader '%.*s' has no bgfx port; its draws are skipped", static_cast<int>(key.size()), key.data());
    *shader = result;
    return D3D_OK;
}

HRESULT Device::CreatePixelShader(const DWORD *function, PixelShader **shader)
{
    auto *result = new PixelShader();
    size_t words = 0;
    const std::string_view key = shader_key(function, &words);
    result->bytecode.assign(function, function + words);
    result->port = find_port(key, false);
    if (!result->port)
        logf("pixel shader '%.*s' has no bgfx port; its draws are skipped", static_cast<int>(key.size()), key.data());
    *shader = result;
    return D3D_OK;
}

HRESULT Device::SetVertexShader(VertexShader *shader)
{
    assign_ref(s.vs, static_cast<VertexShader *>(shader));
    return D3D_OK;
}
HRESULT Device::GetVertexShader(VertexShader **shader)
{
    *shader = s.vs;
    if (s.vs)
        s.vs->AddRef();
    return D3D_OK;
}
HRESULT Device::SetPixelShader(PixelShader *shader)
{
    assign_ref(s.ps, static_cast<PixelShader *>(shader));
    return D3D_OK;
}
HRESULT Device::GetPixelShader(PixelShader **shader)
{
    *shader = s.ps;
    if (s.ps)
        s.ps->AddRef();
    return D3D_OK;
}

bool Device::prepare_program(uint32_t vs_id, uint32_t fs_id, int &budget)
{
    if (!programs.contains({vs_id, fs_id}))
    {
        if (budget <= 0)
            return false;
        program(vs_id, fs_id); // logs a failure
        --budget;
    }
    // The draws of the same programs without clip plane and alpha test use the opaque twin (draw()).
    if (const uint32_t twin = opaque_twin(fs_id); twin != UINT32_MAX && !programs.contains({vs_id, twin}))
        return prepare_program(vs_id, twin, budget);
    return true;
}

bgfx::ProgramHandle Device::program(uint32_t vs_id, uint32_t fs_id)
{
    const auto key = std::make_pair(vs_id, fs_id);
    const auto it = programs.find(key);
    if (it != programs.end())
        return it->second;
    bgfx::ProgramHandle handle = BGFX_INVALID_HANDLE;
    if (vs_id < shaders.size() && fs_id < shaders.size())
    {
        for (const uint32_t id : {vs_id, fs_id})
            if (!bgfx::isValid(shaders[id]))
                shaders[id] = create_embedded_shader(id);
        handle = bgfx::createProgram(shaders[vs_id], shaders[fs_id], false);
    }
    if (!bgfx::isValid(handle))
        logf("ERROR: program creation failed (vs=%u fs=%u)", vs_id, fs_id);
    programs[key] = handle;
    return handle;
}

// ---------------------------------------------------------------------------
// draws
bool Device::draw_vertices(const DrawCall &call, const LayoutInfo &layout, uint32_t element_count, const uint8_t *&out,
                           uint32_t &out_stride, uint32_t &out_count)
{
    const bgfx::VertexLayout &vl = layout.layout[0];
    uint32_t first = call.indexed ? static_cast<uint32_t>(call.base_vertex) + call.min_index : call.start_vertex;
    const uint32_t count = call.indexed ? call.index_vertex_count : element_count;
    const uint8_t *data;
    size_t available;
    if (call.up_vertices)
    {
        data = call.up_vertices;
        if (call.indexed)
            first = call.min_index;
        available = (static_cast<size_t>(first) + count) * call.up_stride;
    }
    else
    {
        const VertexBuffer *vb = s.stream[0];
        if (!vb || s.stream_offset[0] > vb->shadow.size())
            return false;
        data = vb->shadow.data() + s.stream_offset[0];
        available = vb->shadow.size() - s.stream_offset[0];
    }
    const uint32_t stride = vl.getStride();
    if (count == 0 || stride == 0 || (static_cast<size_t>(first) + count) * stride > available)
        return false;
    out = data + static_cast<size_t>(first) * stride;
    out_stride = stride;
    out_count = count;
    return true;
}

bool Device::draw_bounds(const DrawCall &call, const LayoutInfo &layout, uint32_t element_count, float center[3],
                         float &radius)
{
    const bgfx::VertexLayout &vl = layout.layout[0];
    if (layout.rhw || !vl.has(bgfx::Attrib::Position))
        return false;
    // The vertex range the draw reaches: D3D's MinVertexIndex / NumVertices for indexed draws.
    uint32_t first = call.indexed ? static_cast<uint32_t>(call.base_vertex) + call.min_index : call.start_vertex;
    uint32_t count = call.indexed ? call.index_vertex_count : element_count;
    const uint8_t *data;
    size_t available;
    VertexBuffer *vb = nullptr;
    if (call.up_vertices)
    {
        data = call.up_vertices;
        if (call.indexed)
            first = call.min_index;
        available = (static_cast<size_t>(first) + count) * call.up_stride;
    }
    else
    {
        vb = s.stream[0];
        if (!vb || s.stream_offset[0] > vb->shadow.size())
            return false;
        data = vb->shadow.data() + s.stream_offset[0];
        available = vb->shadow.size() - s.stream_offset[0];
    }
    const uint32_t stride = vl.getStride();
    if (count == 0 || stride == 0 || (static_cast<size_t>(first) + count) * stride > available)
        return false;
    if (vb)
    {
        if (vb->bounds_serial != vb->write_serial)
        {
            vb->bounds.clear();
            vb->bounds_serial = vb->write_serial;
        }
        for (const VertexBuffer::RangeBounds &b : vb->bounds)
            if (b.first == first && b.count == count && b.stride == stride && b.offset == s.stream_offset[0])
            {
                memcpy(center, b.center, sizeof(float) * 3);
                radius = b.radius;
                return true;
            }
    }
    // Box centre, then the farthest vertex from it. Float positions (all the game's layouts) are read in place;
    // bgfx::vertexUnpack (format conversion per vertex) only for the others.
    float lo[3] = {FLT_MAX, FLT_MAX, FLT_MAX}, hi[3] = {-FLT_MAX, -FLT_MAX, -FLT_MAX};
    uint8_t components = 0;
    bgfx::AttribType::Enum type = bgfx::AttribType::Float;
    bool normalized = false, as_int = false;
    vl.decode(bgfx::Attrib::Position, components, type, normalized, as_int);
    const bool floats = type == bgfx::AttribType::Float && components >= 3;
    const uint8_t *position = data + vl.getOffset(bgfx::Attrib::Position) + static_cast<size_t>(first) * stride;
    const auto read = [&](uint32_t i, float p[4]) {
        if (floats)
            memcpy(p, position + static_cast<size_t>(i) * stride, sizeof(float) * 3);
        else
            bgfx::vertexUnpack(p, bgfx::Attrib::Position, vl, data, first + i);
    };
    float p[4];
    for (uint32_t i = 0; i < count; ++i)
    {
        read(i, p);
        for (int k = 0; k < 3; ++k)
        {
            lo[k] = std::min(lo[k], p[k]);
            hi[k] = std::max(hi[k], p[k]);
        }
    }
    float r2 = 0.0f;
    for (int k = 0; k < 3; ++k)
        center[k] = 0.5f * (lo[k] + hi[k]);
    for (uint32_t i = 0; i < count; ++i)
    {
        read(i, p);
        const float dx = p[0] - center[0], dy = p[1] - center[1], dz = p[2] - center[2];
        r2 = std::max(r2, dx * dx + dy * dy + dz * dz);
    }
    radius = std::sqrt(r2);
    if (vb && vb->bounds.size() < 256) // static models draw a handful of subsets; dynamic buffers are rewritten
        vb->bounds.push_back({first, count, stride, s.stream_offset[0], {center[0], center[1], center[2]}, radius});
    return true;
}

HRESULT Device::DrawPrimitive(D3DPRIMITIVETYPE type, UINT start_vertex, UINT primitive_count)
{
    DrawCall call;
    call.type = type;
    call.primitive_count = primitive_count;
    call.start_vertex = start_vertex;
    return issue(call);
}

HRESULT Device::DrawIndexedPrimitive(D3DPRIMITIVETYPE type, INT base_vertex, UINT min_index, UINT vertex_count,
                                     UINT start_index, UINT primitive_count)
{
    DrawCall call;
    call.type = type;
    call.primitive_count = primitive_count;
    call.indexed = true;
    call.base_vertex = base_vertex;
    call.min_index = min_index;
    call.index_vertex_count = vertex_count;
    call.start_index = start_index;
    return issue(call);
}

HRESULT Device::DrawPrimitiveUP(D3DPRIMITIVETYPE type, UINT primitive_count, const void *data, UINT stride)
{
    DrawCall call;
    call.type = type;
    call.primitive_count = primitive_count;
    call.up_vertices = static_cast<const uint8_t *>(data);
    call.up_stride = stride;
    return issue(call);
}

HRESULT Device::DrawIndexedPrimitiveUP(D3DPRIMITIVETYPE type, UINT min_index, UINT vertex_count, UINT primitive_count,
                                       const void *index_data, D3DFORMAT index_format, const void *data, UINT stride)
{
    DrawCall call;
    call.type = type;
    call.primitive_count = primitive_count;
    call.indexed = true;
    call.up_vertices = static_cast<const uint8_t *>(data);
    call.up_stride = stride;
    call.up_indices = index_data;
    call.up_index32 = index_format == D3DFMT_INDEX32;
    call.up_vertex_count = min_index + vertex_count;
    call.min_index = min_index;
    call.index_vertex_count = vertex_count;
    return issue(call);
}

namespace
{
// The FF_SKIN variant of a fixed-function vertex shader (GEOS::AVERTEX0 vertices), UINT32_MAX if none.
uint32_t skinned_variant(uint32_t vs_id)
{
    static const std::pair<uint32_t, uint32_t> table[] = {
        {shader_id_by_name("vs_ff"), shader_id_by_name("vs_ff_skin")},
        {shader_id_by_name("vs_ff_unlit"), shader_id_by_name("vs_ff_unlit_skin")},
        {shader_id_by_name("vs_ff_lit"), shader_id_by_name("vs_ff_lit_skin")}, // effects: per-pixel lighting
    };
    for (const auto &[plain, skinned] : table)
        if (plain == vs_id)
            return skinned;
    return UINT32_MAX;
}
} // namespace

// A draw that cannot be issued is never dropped quietly: it is counted and its reason logged once.
void Device::skip_draw(const char *reason)
{
    ++perf().skipped_draws;
    bgfx::discard(); // buffers or textures may already be bound for this draw
    trap(reason);
}

bool Device::sync_sampled(TexStore &store, bool scene)
{
    if (!scene || !store.realize_pending())
    {
        store.sync();
        return true;
    }
    RealizeBudget &budget = realize_budget();
    if (!budget.admit(storm::loading::active || effects::ScenePreparing()))
    {
        if (bgfx::isValid(store.handle))
        {
            budget.postponed(); // a finished transcode: its interim copy draws until a later frame swaps it in
            return true;
        }
        budget.left_out();
        return false;
    }
    const auto start = std::chrono::steady_clock::now();
    store.sync();
    budget.made(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
    return true;
}

// Every D3D draw call: the colour draw, and the caster entry for what may cast into the frame's shadow volumes. Inside
// a render::ShadowCasterOnly (geometry the camera rejected) only the caster entry.
HRESULT Device::issue(const DrawCall &call)
{
    if (call.primitive_count == 0)
        return D3D_OK;
    PerfScope timing(perf().shim_ms);
    Context::instance().poll_surface();
    if (!Context::instance().ready())
        return D3D_OK;
    const uint32_t element_count = vertex_count_for(call.type, call.primitive_count);
    if (element_count == 0)
        return D3DERR_INVALIDCALL;

    const LayoutInfo *layout = nullptr;
    if (call.up_vertices)
    {
        if (s.decl)
        {
            // User-pointer draws supply stream 0 only.
            const UINT saved = s.stream_stride[0];
            s.stream_stride[0] = call.up_stride;
            layout = build_decl_layout(*s.decl);
            s.stream_stride[0] = saved;
        }
        else
            layout = fvf_layout(s.fvf, call.up_stride);
    }
    else
        layout = current_layout();
    if (!layout || layout->stream_count == 0)
    {
        skip_draw("no vertex layout");
        return D3D_OK;
    }
    const bool offscreen = effects::Host::instance().lighting().shadow_casters().only();
    if (offscreen || draw(call, *layout, element_count))
        draw_caster(call, *layout, element_count, offscreen);
    return D3D_OK;
}

// The colour draw. False: it was not submitted (skip_draw() said why).
bool Device::draw(const DrawCall &call, const LayoutInfo &draw_layout, uint32_t element_count)
{
    const LayoutInfo *layout = &draw_layout;
    // Before any state of this draw is set: clearing uses views of its own.
    for (uint32_t stage = 0; stage < kFfStages; ++stage)
        if (TexStore *store = store_of(s.textures[stage]); store && store->needs_clear)
        {
            store->sync();
            clear_if_new(store);
            view_dirty = true; // this draw goes into a view after the clear
        }
    // Effects: a perspective draw into the game picture makes this frame a 3D scene (Device::scene_complete).
    if (rt == back_color && !layout->rhw && s.proj.m[2][3] != 0.0f)
    {
        scene_drawn = true;
        scene_view = s.view;
        scene_proj = s.proj;
    }

    // ---- program ----
    const bool vs_port = s.vs != nullptr, ps_port = s.ps != nullptr;
    if ((vs_port && !s.vs->port) || (ps_port && !s.ps->port))
    {
        skip_draw("shader without bgfx port");
        return false;
    }
    // [0] vertex lighting (D3DRS_LIGHTING), [1] unlit: the lights stay out of the program (ff_vertex.sh FF_UNLIT).
    static const uint32_t id_vs_ff[2] = {shader_id_by_name("vs_ff"), shader_id_by_name("vs_ff_unlit")};
    static const uint32_t id_fs_ff[5] = {shader_id_by_name("fs_ff"), shader_id_by_name("fs_ff_cube0"),
                                         shader_id_by_name("fs_ff_cube1"), shader_id_by_name("fs_ff_cube2"),
                                         shader_id_by_name("fs_ff_slopebias")};
    // A texture left bound on a stage after the first disabled one is not sampled (as in D3D9 and the
    // texture binding below), so it does not select a cube variant.
    uint32_t fs_variant = 0;
    for (uint32_t stage = 0; stage < 3 && !ps_port && s.tss[stage][D3DTSS_COLOROP] != D3DTOP_DISABLE; ++stage)
        if (const TexStore *store = store_of(s.textures[stage]); store && store->kind == TexKind::Cube)
        {
            fs_variant = stage + 1;
            break;
        }
    const float slope_bias = as_float(s.rs[D3DRS_SLOPESCALEDEPTHBIAS]);
    const bool port_bias = ps_port && s.ps->port->slope_bias;
    if (slope_bias != 0.0f || port_bias)
    {
        if (ps_port ? !port_bias : fs_variant != 0)
            trap("slope-scale depth bias with this pixel shader or a cube stage is not implemented (no bias applied)");
        else
        {
            if (!ps_port)
                fs_variant = 4;
            const float bias[4] = {slope_bias, 0.0f, 0.0f, 0.0f};
            set_uniform(U_DEPTHBIAS, bias, 1);
        }
    }
    // Effects: per-pixel lighting / alpha to coverage variant of a fixed-function draw (effects/lighting.hpp).
    // Slope-biased draws (projected shadows: unlit decals) keep the depth-writing fixed-function variant.
    effects::Lighting &fx_lighting = effects::Host::instance().lighting();
    effects::FfDraw fx_draw;
    // The lights are chosen for the part of the world the draw covers (its vertices' sphere in world space), computed
    // only when prepare() or the parallax probe needs it (effects::DrawSphereSource).
    struct SphereContext
    {
        Device *device;
        const DrawCall *call;
        const LayoutInfo *layout;
        uint32_t element_count;
    } sphere_context{this, &call, layout, element_count};
    const effects::DrawSphereSource sphere(
        [](const void *context) {
            const auto &c = *static_cast<const SphereContext *>(context);
            effects::DrawSphere result;
            float center[3], radius = 0.0f;
            if (c.device->draw_bounds(*c.call, *c.layout, c.element_count, center, radius))
            {
                const D3DMATRIX &w = c.device->s.world;
                for (int k = 0; k < 3; ++k)
                    result.center[k] = center[0] * w.m[0][k] + center[1] * w.m[1][k] + center[2] * w.m[2][k] + w.m[3][k];
                float scale = 0.0f;
                for (int r = 0; r < 3; ++r)
                    scale = std::max(scale, std::sqrt(w.m[r][0] * w.m[r][0] + w.m[r][1] * w.m[r][1] + w.m[r][2] * w.m[r][2]));
                result.radius = radius * scale;
                result.valid = true;
            }
            return result;
        },
        &sphere_context,
        [](const void *context) {
            const auto &c = *static_cast<const SphereContext *>(context);
            effects::DrawVertices result;
            if (!c.device->draw_vertices(*c.call, *c.layout, c.element_count, result.data, result.stride, result.count))
                result = {};
            return result;
        });
    if (!vs_port && !ps_port && fs_variant != 4 && !fx_lighting.idle())
        fx_lighting.prepare(*this, *layout, fs_variant, sphere, fx_draw);
    fx_lighting.census(*this, *layout, vs_port || ps_port, fs_variant == 4, fs_variant, fx_draw);
    if (layout->skinned && (vs_port || !store_of(s.vertex_texture)))
    {
        skip_draw(vs_port ? "skinned vertices with a vertex shader port"
                          : "skinned draw without a bone palette in D3DVERTEXTEXTURESAMPLER0");
        return false;
    }
    if (vs_port && s.vs->port->vertex_texture && !store_of(s.vertex_texture))
    {
        skip_draw("a vertex port that reads D3DVERTEXTEXTURESAMPLER0 without a texture there");
        return false;
    }
    // Without lighting the unlit variant leaves the lights out of the program (see ff_vertex.sh).
    const bool lit = s.rs[D3DRS_LIGHTING] != 0 && !layout->rhw;
    // Effects: a plain draw under a moving caster's sun shadow takes the pair with the dynamic layer's tap (FfDraw::ground).
    uint32_t vs_id = vs_port ? s.vs->port->shader_id()
                             : fx_draw.active ? fx_draw.vs_id : fx_draw.ground ? fx_lighting.ground_vs(lit) : id_vs_ff[lit ? 0 : 1];
    if (layout->skinned)
    {
        vs_id = skinned_variant(vs_id);
        if (vs_id == UINT32_MAX)
        {
            skip_draw("skinned draw with a vertex shader that has no skinned variant");
            return false;
        }
    }
    uint32_t fs_id = ps_port ? s.ps->port->shader_id()
                             : fx_draw.active ? fx_draw.fs_id : fx_draw.ground ? fx_lighting.ground_fs() : id_fs_ff[fs_variant];
    // Nothing of this draw can be discarded (no user clip plane, no alpha test): its shader's opaque twin, which
    // keeps early depth and the tile GPUs' hidden-surface removal (ff_finish.sh FF_OPAQUE; the ports' twins:
    // CMakeLists.txt).
    if (!(s.rs[D3DRS_CLIPPLANEENABLE] & 1) &&
        !(s.rs[D3DRS_ALPHATESTENABLE] && s.rs[D3DRS_ALPHAFUNC] != D3DCMP_ALWAYS))
        if (const uint32_t twin = opaque_twin(fs_id); twin != UINT32_MAX)
            fs_id = twin;
    const bgfx::ProgramHandle handle = program(vs_id, fs_id);
    if (!bgfx::isValid(handle))
    {
        skip_draw("program creation failed");
        return false;
    }

    // Effects: the Sea2 ports' improvements; the soft edges and the waves may capture the scene depth in a view of their own
    // first, so this comes before the draw's view.
    if ((ps_port && s.ps->port->sea_fx) || (vs_port && s.vs->port->sea_fx))
    {
        effects::Host::instance().prepare_sea(*this);
        effects::Host::instance().bind_sea_sun_road(*this);
    }
    const uint16_t view_id = ensure_view();
    if (!bind_geometry(call, *layout, element_count))
        return false;
    const bool fan = call.type == D3DPT_TRIANGLEFAN;

    // ---- textures ----
    const uint32_t stage_limit = ps_port ? s.ps->port->sampler_count : kFfStages;
    // The draw's distance from the camera (its vertices' bounding sphere), for the textures with a neural ×2
    // (effects/upscale.hpp): computed at the first such texture.
    effects::Upscales &upscales = effects::upscales();
    float upscale_distance = -2.0f, player_distance = -1.0f;
    float generation_radius = 0.0f, generation_camera_distance = -1.0f;
    const auto draw_distance = [&] {
        if (upscale_distance > -2.0f)
            return upscale_distance;
        upscale_distance = -1.0f;
        float center[3], radius = 0.0f;
        if (draw_bounds(call, *layout, element_count, center, radius))
        {
            const D3DMATRIX &w = s.world, &v = s.view;
            float scale = 0.0f, d2 = 0.0f;
            std::array<float, 3> device_center{};
            for (int r = 0; r < 3; ++r)
                scale = std::max(scale, std::sqrt(w.m[r][0] * w.m[r][0] + w.m[r][1] * w.m[r][1] + w.m[r][2] * w.m[r][2]));
            for (int k = 0; k < 3; ++k)
            {
                const float world = center[0] * w.m[0][k] + center[1] * w.m[1][k] + center[2] * w.m[2][k] + w.m[3][k];
                // The camera: -t R^T of the view's rotation R and translation t.
                device_center[k] = world;
                const float eye = -(v.m[3][0] * v.m[k][0] + v.m[3][1] * v.m[k][1] + v.m[3][2] * v.m[k][2]);
                d2 += (world - eye) * (world - eye);
            }
            generation_radius = radius * scale;
            generation_camera_distance = std::sqrt(d2);
            upscale_distance = std::max(0.0f, generation_camera_distance - generation_radius);
            player_distance = effects::generation_focus().distance(device_center, radius * scale);
        }
        return upscale_distance;
    };
    // A stage whose texture was swapped a moment ago fades from the previous one (texture_fade.hpp): bound at the first
    // fixed-function unit this draw leaves free, stage 0's at unit 4, stage 1's at unit 3.
    uint32_t ff_stages = 0;
    while (!ps_port && ff_stages < kFfStages && s.tss[ff_stages][D3DTSS_COLOROP] != D3DTOP_DISABLE)
        ++ff_stages;
    std::fill(std::begin(stage_fade_), std::end(stage_fade_), 0.0f);
    TextureFades &fades = texture_fades();
    // A town's lightmap keeps the game's texture; the hour's sun on it is a light ratio the stage multiplies it by
    // (effects/town_light.hpp), bound at the first unit from 2 on whose stage samples no texture (past the last enabled
    // stage, or one without a texture: the game's lightmapped geometry leaves stage 2 so). It is chosen before the fades,
    // which leave it to the ratio: a fade is a moment's blend, the ratio is the hour's light.
    effects::TownLight::Ratio town_ratio;
    const TexStore *town_store = nullptr;
    uint32_t town_stage = 0, town_flags = 0;
    uint32_t ratio_unit = kFfStages;
    for (uint32_t stage = 0; stage < ff_stages; ++stage)
        if (const TexStore *store = store_of(s.textures[stage]); store && store->lightmap && store->kind == TexKind::Tex2D)
        {
            ratio_unit = 2;
            while (ratio_unit < ff_stages && s.textures[ratio_unit])
                ++ratio_unit;
            break;
        }
    for (uint32_t stage = 0; stage < stage_limit; ++stage)
    {
        TexStore *store = store_of(s.textures[stage]);
        if (!store)
            continue;
        if (!ps_port && s.tss[stage][D3DTSS_COLOROP] == D3DTOP_DISABLE)
            break;
        const bool scene_draw = rt == back_color && !layout->rhw && s.proj.m[2][3] != 0.0f;
        if (scene_draw)
            effects::ObserveSceneTexture(shared_store_of(s.textures[stage]));
        if (!sync_sampled(*store, scene_draw))
        {
            // Made in a later frame within the budget (realize_budget.hpp): this frame leaves the draw out, counted.
            bgfx::discard();
            return false;
        }
        if (!bgfx::isValid(store->handle))
        {
            // No GPU copy will come (bgfx refused it, logged): the stand-in, not the unbound-texture fallback's black
            // (storm/render/missing_texture.hpp). One still on its way draws the fallback until it arrives.
            if (store->gpu_failed && store->kind == TexKind::Tex2D)
            {
                bgfx::setTexture(static_cast<uint8_t>(stage), Context::instance().sampler(stage),
                                 Context::instance().missing_texture(), sampler_flags(stage, *store));
                ++perf().texture_binds;
            }
            continue;
        }
        if (store->render_target && store->levels > 1 && (std::max(store->width, store->height) >> store->levels) != 0)
            // bgfx allocates the full chain for render targets; the levels below the D3D ones are never written.
            trap("sampling a mipped render target with a partial mip chain (lower levels undefined)");
        uint32_t unit = stage;
        if (store->kind != TexKind::Tex2D)
        {
            // cube: stages 0..2 -> units 5..7, stage 3 -> 11; volume: stages 0..2 -> 8..10, stage 4 -> 12
            const bool cube = store->kind == TexKind::Cube;
            if (stage < 3)
                unit = (cube ? 5 : 8) + stage;
            else if (cube && stage == 3)
                unit = 11;
            else if (!cube && stage == 4)
                unit = 12;
            else
                continue;
        }
        const auto normal_pending = [](const std::shared_ptr<effects::NormalMapEntry> &entry) {
            if (!entry) return false;
            const int state = entry->state.load();
            return state == effects::NormalMapEntry::Waiting || state == effects::NormalMapEntry::Pending;
        };
        const bool pending_normal = normal_pending(store->normal_map), pending_gloss = normal_pending(store->gloss_map);
        const bool pending_upscale = upscales.active() && store->upscale &&
            store->upscale->state != effects::UpscaleEntry::Resident && store->upscale->state != effects::UpscaleEntry::Failed;
        if (pending_normal || pending_gloss || pending_upscale)
        {
            draw_distance(); // reuse cached vertex-buffer bounds, only while generation is pending
            const auto generation_at = effects::generation_focus().snapshot();
            const auto observe_normal = [&](const std::shared_ptr<effects::NormalMapEntry> &entry) {
                entry->priority.observe(player_distance, generation_at);
                if (player_distance >= 0.0f) // no camera substitute when the controlled-player anchor is absent
                    entry->screen.observe(generation_radius, generation_camera_distance,
                        entry->relief == fx::ReliefClass::Character || entry->gloss == fx::GlossClass::Character,
                        generation_at);
            };
            if (pending_normal) observe_normal(store->normal_map);
            if (pending_gloss) observe_normal(store->gloss_map);
            if (pending_upscale) store->upscale->priority.observe(player_distance, generation_at);
        }
        const uint32_t flags = sampler_flags(stage, *store);
        if (store->lightmap && store->kind == TexKind::Tex2D)
            if (const effects::TownLight::Ratio r = fx_lighting.town_light().ratio(store); bgfx::isValid(r.texture))
            {
                town_ratio = r;
                town_store = store;
                town_stage = stage;
                town_flags = flags;
            }
        bgfx::TextureHandle handle = store->handle;
        uint32_t skin_width = store->width, skin_height = store->height;
        uint32_t skin_levels = store->full_chain ? 0 : store->levels;
        TextureSource source = TextureSource::Own;
        // A resident ×2 is the store's texture for every draw (one without bounds reports no distance but binds it
        // too): draws that disagreed about it would make it flicker and keep restarting its fade.
        if (store->upscale && upscales.active())
            if (const float distance = draw_distance();
                distance >= 0.0f || store->upscale->state == effects::UpscaleEntry::Resident)
                if (const bgfx::TextureHandle ours = distance >= 0.0f ? upscales.replacement(*store, distance) : store->upscale->handle;
                    bgfx::isValid(ours))
                {
                    handle = ours;
                    source = TextureSource::Upscale;
                    skin_width = store->upscale->width * 2;
                    skin_height = store->upscale->height * 2;
                    skin_levels = 0; // every resident upscale has its complete rebuilt chain
                }
        // The character reference was prepared before replacement selection. Match its coarse colour test to the
        // actual stage-0 albedo, including source -> x2 -> source changes and partial native mip chains.
        if (stage == 0)
            fx::SetSkinToneSampling(fx_draw.skin_tone, skin_width, skin_height, skin_levels);
        bgfx::setTexture(static_cast<uint8_t>(unit), Context::instance().sampler(unit), handle, flags);
        ++perf().texture_binds;
        if (store->render_target)
            schedule.read(view_id, store->handle.idx); // the view runs after the one that drew it (ViewSchedule)
        if (const TextureFade::Sample fade = store->fade.observe(handle.idx, source, fades.now(), fades.epoch());
            fade.from != kNoTexture)
        {
            const uint32_t from_unit = stage == 0 ? 4 : stage == 1 ? 3 : 0;
            const bool fades_here = !ps_port && store->kind == TexKind::Tex2D && from_unit && ff_stages <= from_unit &&
                                    from_unit != ratio_unit;
            if (fades_here)
            {
                bgfx::setTexture(static_cast<uint8_t>(from_unit), Context::instance().sampler(from_unit),
                                 bgfx::TextureHandle{fade.from}, flags);
                ++perf().texture_binds;
                last_draw.texture[from_unit] = fade.from;
                stage_fade_[stage] = fade.share;
            }
            if (fade.started)
            {
                TextureFadeCounts &counts = fades.counts();
                counts.reversed += fade.reversed ? 1 : 0;
                if (fades_here)
                    ++counts.started[size_t(fade.kind)];
                else
                    ++(ps_port ? counts.port : store->kind != TexKind::Tex2D ? counts.not_2d : counts.no_unit);
            }
        }
        if (last_draw.texture[unit] != handle.idx || last_draw.sampler[unit] != flags)
        {
            ++perf().texture_changes;
            last_draw.texture[unit] = handle.idx;
            last_draw.sampler[unit] = flags;
        }
    }

    town_ratio_[0] = -1.0f;
    if (town_store)
    {
        const uint32_t unit = ratio_unit;
        if (ps_port)
            fx_lighting.town_light().ratio_missed(town_store, "a game shader port");
        else if (unit >= kFfStages)
            fx_lighting.town_light().ratio_missed(town_store, "stages 2 to 4 all sample textures");
        else
        {
            bgfx::setTexture(static_cast<uint8_t>(unit), Context::instance().sampler(unit), town_ratio.texture, town_flags);
            ++perf().texture_binds;
            if (last_draw.texture[unit] != town_ratio.texture.idx || last_draw.sampler[unit] != town_flags)
            {
                ++perf().texture_changes;
                last_draw.texture[unit] = town_ratio.texture.idx;
                last_draw.sampler[unit] = town_flags;
            }
            town_ratio_[0] = static_cast<float>(town_stage);
            town_ratio_[1] = static_cast<float>(unit);
            town_ratio_[2] = town_ratio.weight;
            town_ratio_[3] = town_ratio.debug ? 1.0f : 0.0f;
        }
    }

    if (layout->skinned && !bind_palette())
        return false;
    if (vs_port && s.vs->port->vertex_texture && !bind_vertex_texture(vs_id))
        return false;

    apply_ff_uniforms(*layout, vs_port, ps_port, fx_draw, call.type == D3DPT_POINTLIST);
    if (fx_draw.active)
        fx_lighting.submit(fx_draw);
    else if (fx_draw.ground)
        fx_lighting.submit_ground(fx_draw);

    bool stencil = false;
    uint32_t fstencil = 0;
    D3DPRIMITIVETYPE effective = fan ? D3DPT_TRIANGLELIST : call.type;
    const uint64_t bits = render_state_bits(effective, &stencil, &fstencil) | fx_draw.state;
    uint32_t factor = 0;
    if (s.rs[D3DRS_ALPHABLENDENABLE])
    {
        const DWORD c = s.rs[D3DRS_BLENDFACTOR];
        factor = ((c & 0x00FFFFFF) << 8) | (c >> 24);
    }
    // Vulkan: a 3D scene draw whose pipeline does not exist yet is left out of the frame while the pipeline is
    // created off the render thread (patches/bgfx-vulkan-pipeline-cache.patch), instead of the frame waiting for the
    // driver's shader compile; the scene's recorded pipelines were created while it loaded (pipeline_sets.hpp), so
    // this is the last resort, logged. Only for what is drawn again anyway: the scene itself, next frame, and a
    // target the game has been rendering frame after frame (the sea's environment map). Not for a target's first
    // frames, so not for one the game draws once and keeps, and not while the next scene frame is to be kept as the
    // pause picture. Never for the interface: nothing drawn after the frame's first interface marked the end of the
    // scene (the HUD, menus, text, the fader) and no pre-transformed draw (sprites, text, the loading picture) waits;
    // their pipelines are small and are created at the draw. bgfx itself makes every read-back frame wait.
    constexpr uint64_t kPipelineMayWait = UINT64_C(0x0800000000000000); // STORM_STATE_PIPELINE_MAY_WAIT
    const bool may_wait = Context::instance().device_info().vulkan && Context::instance().async_pipelines() &&
                          !interface_drawn && !layout->rhw && view_color &&
                          (view_color == back_color->store.get() || view_color->target_recurring) &&
                          !Context::instance().hold_requested();
    bgfx::setState(bits | (may_wait ? kPipelineMayWait : 0), factor);
    if (stencil)
        bgfx::setStencil(fstencil);
    // Armed only by stock Fader/progress artwork; validate the actual destination, viewport, blend and scissor at submit.
    bool opaqueCoverage = (bits & BGFX_STATE_WRITE_RGB) == BGFX_STATE_WRITE_RGB && view_color == back_color->store.get() && layout->rhw &&
        view_rect[0] == 0 && view_rect[1] == 0 && view_rect[2] == back_color->store->width &&
        view_rect[3] == back_color->store->height &&
        (!s.rs[D3DRS_ALPHABLENDENABLE] || (s.tss[0][D3DTSS_ALPHAOP] == D3DTOP_SELECTARG1 && s.tss[0][D3DTSS_ALPHAARG1] == D3DTA_DIFFUSE &&
        s.tss[1][D3DTSS_ALPHAOP] == D3DTOP_DISABLE &&
        s.rs[D3DRS_SRCBLEND] == D3DBLEND_SRCALPHA &&
        s.rs[D3DRS_DESTBLEND] == D3DBLEND_INVSRCALPHA)) &&
        !s.rs[D3DRS_ALPHATESTENABLE] && !s.rs[D3DRS_ZENABLE] && !s.rs[D3DRS_STENCILENABLE];
    if (s.rs[D3DRS_SCISSORTESTENABLE])
    {
        // In the pixels the game addresses, like the viewport (new_view).
        const uint32_t target_height = rt->store->level_height(rt->level);
        const std::array<float, 2> scale = target_scale();
        const auto px = [](LONG v, float k) { return static_cast<int32_t>(std::lround(static_cast<float>(v) * k)); };
        const int32_t left = px(s.scissor.left, scale[0]), right = px(s.scissor.right, scale[0]);
        const int32_t top = px(s.scissor.top, scale[1]), bottom = px(s.scissor.bottom, scale[1]);
        const int32_t w = right - left, h = bottom - top;
        const int32_t y = Context::instance().flip_y() ? static_cast<int32_t>(target_height) - bottom : top;
        opaqueCoverage = opaqueCoverage && w <= UINT16_MAX && h <= UINT16_MAX && left <= 0 && y <= 0 &&
            right >= static_cast<int32_t>(back_color->store->width) &&
            y + h >= static_cast<int32_t>(back_color->store->height);
        bgfx::setScissor(static_cast<uint16_t>(std::max<int32_t>(0, left)), static_cast<uint16_t>(std::max(0, y)),
                         static_cast<uint16_t>(std::max(0, w)), static_cast<uint16_t>(std::max(0, h)));
    }
    // Keep discovery, material sync, lighting preparation and caster uniforms above intact.
    // Only the game picture is hidden; producer/offscreen targets remain ordinary.
    const bool hiddenBeauty = rt == back_color && !layout->rhw &&
        render::HideSceneBeauty(Context::instance().Surface().generation,
            held_color ? held_color->serial : back_color->store->serial);
    if (hiddenBeauty) {
        bgfx::discard(BGFX_DISCARD_ALL);
        // The discard consumes encoder state. No beauty counter or opaque-submit acknowledgement; the caster entry
        // (Device::issue) still offers the draw to the shadow volumes.
        invalidate_uniforms();
        return true;
    }
    // Effects: the parallax probe (effects/parallax_probe.hpp) tests a relief draw of the game picture that writes depth
    // and lies on the centre ray again after the scene, at the centre pixel, against the scene's final depth.
    bgfx::OcclusionQueryHandle probe_query = BGFX_INVALID_HANDLE;
    effects::ParallaxProbe &probe = fx_lighting.probe();
    if (fx_draw.relief_entry && probe.enabled() && rt == back_color && view_depth == back_depth->store.get() &&
        (bits & BGFX_STATE_WRITE_Z) && !layout->rhw)
        if (const effects::DrawSphere &bounds = sphere.get(); bounds.valid)
        {
            const float center[3] = {bounds.center[0], bounds.center[1], bounds.center[2]};
            probe_query = probe.candidate(fx_draw.relief_entry, &s.view.m[0][0], &s.proj.m[0][0], center, bounds.radius,
                                          {view_framebuffer, view_rect, view_min_z, view_max_z});
        }
    // Effects: a probe draw keeps its buffers for its copy, a draw before the sea for its copy into the sea's backdrop
    // (Host::sea_backdrop).
    auto &effects_host = effects::Host::instance();
    const bool backdrop = effects_host.sea_backdrop(*this, *layout, vs_port);
    bgfx::submit(view_id, handle, 0, bgfx::isValid(probe_query) || backdrop ? BGFX_DISCARD_NONE : BGFX_DISCARD_ALL);
    if (rt == back_color && !layout->rhw) render::SceneBeautySubmitted();
    render::SceneOpaqueDrawSubmitted(opaqueCoverage, !s.rs[D3DRS_ALPHABLENDENABLE] || s.rs[D3DRS_BLENDOP] == D3DBLENDOP_ADD);
    if (bgfx::isValid(probe_query))
    {
        // The probe view runs elsewhere in the frame: every uniform of the program is set again for the copy (the
        // shim sends only the changed ones, which the draws between would have changed).
        invalidate_uniforms();
        apply_ff_uniforms(*layout, vs_port, ps_port, fx_draw, call.type == D3DPT_POINTLIST);
        if (fx_draw.active)
            fx_lighting.submit(fx_draw);
        else if (fx_draw.ground)
            fx_lighting.submit_ground(fx_draw);
        constexpr uint64_t kKept = BGFX_STATE_PT_MASK | BGFX_STATE_CULL_MASK | BGFX_STATE_MSAA | BGFX_STATE_BLEND_ALPHA_TO_COVERAGE;
        bgfx::setState((bits & kKept) | BGFX_STATE_DEPTH_TEST_EQUAL);
        bgfx::setStencil(BGFX_STENCIL_NONE); // the scene's stencil is not the one this draw saw
        const uint32_t width = rt->store->level_width(rt->level), height = rt->store->level_height(rt->level);
        bgfx::setScissor(static_cast<uint16_t>(width / 2), static_cast<uint16_t>(height / 2), 1, 1);
        bgfx::submit(effects::ParallaxProbe::kView, handle, probe_query, 0, backdrop ? BGFX_DISCARD_NONE : BGFX_DISCARD_ALL);
    }
    if (backdrop)
        effects_host.submit_sea_backdrop(*this, layout->skinned, bits & BGFX_STATE_CULL_MASK, false);
    view_has_content = true;
    render::PerfCounters &counters = perf();
    ++counters.draws;
    const bool program_changed = last_draw.program != handle.idx;
    if (program_changed)
        ++counters.program_changes, last_draw.program = handle.idx;
    const uint32_t stencil_bits = stencil ? fstencil : 0;
    if (last_draw.state != bits || last_draw.stencil != stencil_bits || last_draw.factor != factor)
    {
        ++counters.state_changes;
        last_draw.state = bits, last_draw.stencil = stencil_bits, last_draw.factor = factor;
    }
    counters.uniform_draws += uniforms_changed ? 1 : 0;
    uniforms_changed = false;
    return true;
}

// Sets the draw's vertex streams and indices on the encoder: buffers written this frame and user-pointer data as
// transient copies, triangle fans as lists. False: the draw cannot be issued (skip_draw() said why).
bool Device::bind_geometry(const DrawCall &call, const LayoutInfo &layout, uint32_t element_count)
{
    const uint64_t frame = Context::instance().frame_index();
    // ---- index source ----
    const uint8_t *index_source = nullptr;
    bool index32 = false;
    IndexBuffer *ib = nullptr;
    if (call.indexed)
    {
        if (call.up_indices)
        {
            index_source = static_cast<const uint8_t *>(call.up_indices);
            index32 = call.up_index32;
        }
        else
        {
            ib = s.indices;
            if (!ib)
            {
                skip_draw("no index buffer bound");
                return false;
            }
            index32 = ib->index32;
            const size_t offset = static_cast<size_t>(call.start_index) * (index32 ? 4 : 2);
            if (offset + static_cast<size_t>(element_count) * (index32 ? 4 : 2) > ib->shadow.size())
            {
                skip_draw("index range outside the buffer");
                return false;
            }
            index_source = ib->shadow.data() + offset;
        }
    }
    const auto index_at = [&](uint32_t i) -> uint32_t {
        return index32 ? reinterpret_cast<const uint32_t *>(index_source)[i]
                       : reinterpret_cast<const uint16_t *>(index_source)[i];
    };

    // ---- vertex streams ----
    bool transient_vertices = call.up_vertices != nullptr;
    if (!call.up_vertices)
        for (uint32_t i = 0; i < layout.stream_count; ++i)
        {
            VertexBuffer *vb = s.stream[i];
            if (!vb)
            {
                skip_draw("no vertex buffer bound");
                return false;
            }
            if (vb->write_frame == frame || (s.stream_offset[i] % layout.stride[i]) != 0)
                transient_vertices = true;
        }

    uint32_t index_bias = 0;         // subtracted from every index
    uint32_t first_vertex = 0;       // first vertex of the range, relative to the stream start
    uint32_t vertex_count = element_count;
    if (call.indexed)
    {
        if (transient_vertices)
        {
            uint32_t lo = UINT32_MAX, hi = 0;
            for (uint32_t i = 0; i < element_count; ++i)
            {
                const uint32_t v = index_at(i);
                lo = std::min(lo, v);
                hi = std::max(hi, v);
            }
            index_bias = lo;
            first_vertex = static_cast<uint32_t>(call.base_vertex) + lo;
            vertex_count = hi - lo + 1;
        }
        else
            first_vertex = static_cast<uint32_t>(call.base_vertex);
    }
    else
        first_vertex = call.start_vertex;

    if (transient_vertices)
    {
        for (uint32_t i = 0; i < layout.stream_count; ++i)
        {
            const uint32_t stride = layout.stride[i];
            const uint8_t *source;
            size_t available;
            if (call.up_vertices)
            {
                source = call.up_vertices;
                available = SIZE_MAX;
            }
            else
            {
                VertexBuffer *vb = s.stream[i];
                source = vb->shadow.data() + s.stream_offset[i];
                available = vb->shadow.size() - std::min<size_t>(vb->shadow.size(), s.stream_offset[i]);
            }
            const size_t begin = static_cast<size_t>(first_vertex) * stride;
            const size_t bytes = static_cast<size_t>(vertex_count) * stride;
            if (begin + bytes > available)
            {
                // The caller's indices or vertex range reach past the end of its vertex buffer.
                skip_draw("vertex range outside the vertex buffer");
                return false;
            }
            const VertexBuffer *vb = call.up_vertices ? nullptr : s.stream[i];
            if (vb)
            {
                // The same range (passes over one dynamic buffer, several subsets) is copied once per frame.
                const size_t shadow_begin = s.stream_offset[i] + begin;
                const uint32_t bgfx_frame = Context::instance().submitted_frames();
                const auto reuse = std::find_if(transient_ranges.begin(), transient_ranges.end(), [&](const TransientRange &r) {
                    return r.bgfx_frame == bgfx_frame && r.vb == vb && r.serial == vb->write_serial && r.layout == layout.handle[i].idx &&
                           r.stride == stride && r.begin <= shadow_begin && shadow_begin + bytes <= r.begin + r.bytes &&
                           (shadow_begin - r.begin) % stride == 0;
                });
                if (reuse != transient_ranges.end())
                {
                    bgfx::setVertexBuffer(static_cast<uint8_t>(i), &reuse->tvb,
                                          static_cast<uint32_t>((shadow_begin - reuse->begin) / stride), vertex_count);
                    continue;
                }
            }
            if (bgfx::getAvailTransientVertexBuffer(vertex_count, layout.layout[i]) < vertex_count)
            {
                skip_draw("transient vertex budget exhausted");
                return false;
            }
            bgfx::TransientVertexBuffer tvb;
            bgfx::allocTransientVertexBuffer(&tvb, vertex_count, layout.layout[i]);
            memcpy(tvb.data, source + begin, bytes);
            perf().transient_vb_bytes += bytes;
            if (vb)
                transient_ranges.push_back({Context::instance().submitted_frames(), vb, vb->write_serial, layout.handle[i].idx, stride,
                                            s.stream_offset[i] + begin, bytes, tvb});
            bgfx::setVertexBuffer(static_cast<uint8_t>(i), &tvb, 0, vertex_count);
        }
    }
    else
    {
        for (uint32_t i = 0; i < layout.stream_count; ++i)
        {
            VertexBuffer *vb = s.stream[i];
            upload_buffer(*vb, layout.layout[i]);
            const uint32_t start = s.stream_offset[i] / layout.stride[i] + first_vertex;
            const uint32_t count = call.indexed ? UINT32_MAX : vertex_count;
            bgfx::setVertexBuffer(static_cast<uint8_t>(i), vb->handle, start, count, layout.handle[i]);
        }
    }

    // ---- indices ----
    const bool fan = call.type == D3DPT_TRIANGLEFAN;
    const bool transient_indices =
        fan || (call.indexed && (call.up_indices || index_bias != 0 || (ib && ib->write_frame == frame)));
    if (transient_indices)
    {
        const uint32_t out_count = fan ? call.primitive_count * 3 : element_count;
        const bool out32 = call.indexed ? index32 : vertex_count > 65535;
        if (bgfx::getAvailTransientIndexBuffer(out_count, out32) < out_count)
        {
            skip_draw("transient index budget exhausted");
            return false;
        }
        bgfx::TransientIndexBuffer tib;
        bgfx::allocTransientIndexBuffer(&tib, out_count, out32);
        const auto put = [&](uint32_t i, uint32_t value) {
            if (out32)
                reinterpret_cast<uint32_t *>(tib.data)[i] = value;
            else
                reinterpret_cast<uint16_t *>(tib.data)[i] = static_cast<uint16_t>(value);
        };
        const auto source = [&](uint32_t i) -> uint32_t { return call.indexed ? index_at(i) - index_bias : i; };
        if (fan)
        {
            for (uint32_t i = 0; i < call.primitive_count; ++i)
            {
                put(i * 3, source(0));
                put(i * 3 + 1, source(i + 1));
                put(i * 3 + 2, source(i + 2));
            }
        }
        else if (call.indexed && index_bias == 0 && out32 == index32)
            memcpy(tib.data, index_source, static_cast<size_t>(out_count) * (out32 ? 4 : 2)); // indices written this frame
        else
            for (uint32_t i = 0; i < out_count; ++i)
                put(i, source(i));
        bgfx::setIndexBuffer(&tib, 0, out_count);
        perf().transient_ib_bytes += static_cast<uint64_t>(out_count) * (out32 ? 4 : 2);
        perf().fan_draws += fan ? 1 : 0;
    }
    else if (call.indexed)
    {
        upload_buffer(*ib, index32);
        bgfx::setIndexBuffer(ib->handle, call.start_index, element_count);
    }
    return true;
}

// Sets the texture a vertex port reads at D3DVERTEXTEXTURESAMPLER0 (the sea's height field) on the encoder, as
// bind_palette sets a skinned draw's palette: last before the draw's submit, after everything that submits draws of
// its own on the way (effects::Host::prepare_sea's scene depth capture), which would take a texture set before them
// with them and leave this draw reading none. False: the draw cannot be issued (skip_draw() said why).
bool Device::bind_vertex_texture(uint32_t vs_id)
{
    TexStore *texture = store_of(s.vertex_texture);
    texture->sync();
    if (!bgfx::isValid(texture->handle))
    {
        skip_draw("a vertex port's D3DVERTEXTEXTURESAMPLER0 texture has no GPU texture");
        return false;
    }
    // A program whose vertex shader lost the sampler (a compiler that strips it) would read nothing there.
    auto declared = vertex_sampler_declared_.find(vs_id);
    if (declared == vertex_sampler_declared_.end())
    {
        bool found = false;
        bgfx::UniformHandle uniforms[64];
        const uint16_t count = vs_id < shaders.size() && bgfx::isValid(shaders[vs_id])
                                   ? bgfx::getShaderUniforms(shaders[vs_id], uniforms, 64)
                                   : 0;
        for (uint16_t i = 0; i < count && !found; ++i)
        {
            bgfx::UniformInfo info{};
            bgfx::getUniformInfo(uniforms[i], info);
            found = std::strcmp(info.name, "s_vtex") == 0;
        }
        if (!found)
            logf("ERROR: vertex shader %u reads D3DVERTEXTEXTURESAMPLER0 but its compiled program declares no s_vtex "
                 "sampler: its draws are skipped",
                 vs_id);
        declared = vertex_sampler_declared_.emplace(vs_id, found).first;
    }
    if (!declared->second)
    {
        skip_draw("a vertex port whose compiled program has no s_vtex sampler");
        return false;
    }
    bgfx::setTexture(static_cast<uint8_t>(kVertexTextureUnit), Context::instance().sampler(kVertexTextureUnit),
                     texture->handle, BGFX_SAMPLER_POINT | BGFX_SAMPLER_UVW_CLAMP);
    ++perf().texture_binds;
    return true;
}

// Sets the bone palette of a skinned draw on the encoder. False: the draw cannot be issued (skip_draw() said why).
bool Device::bind_palette()
{
    // The frame's palettes share one texture (bone_palettes.hpp): this palette's rows at an offset.
    TexStore *palette = store_of(s.vertex_texture);
    BonePalettes &palettes = Context::instance().bone_palettes();
    const BonePalettes::Slot slot = palettes.place(*palette);
    bgfx::TextureHandle texture = palettes.texture();
    if (!slot.valid())
    {
        // The frame's atlas is full (logged once): the palette's own texture, uploaded as before.
        palette->any_dirty = palette->sub(0, 0).dirty = true;
        palette->sync();
        texture = palette->handle;
    }
    // The first row and the column as instance data of one instance (ff_skin.sh i_data0): a uniform here
    // re-committed the program's whole uniform block for every skinned draw (bgfx GL commits all of a program's
    // uniforms on a change). The caster entry binds it the same way.
    if (bgfx::getAvailInstanceDataBuffer(1, 16) < 1)
    {
        skip_draw("transient budget exhausted for a skinned draw's palette row");
        return false;
    }
    bgfx::InstanceDataBuffer instance;
    bgfx::allocInstanceDataBuffer(&instance, 1, 16);
    const float base[4] = {slot.valid() ? static_cast<float>(slot.row) : 0.0f,
                           slot.valid() ? static_cast<float>(slot.column * BonePalettes::kTexelsPerBone) : 0.0f, 0.0f,
                           0.0f};
    memcpy(instance.data, base, sizeof(base));
    bgfx::setInstanceDataBuffer(&instance);
    bgfx::setTexture(static_cast<uint8_t>(kVertexTextureUnit), Context::instance().sampler(kVertexTextureUnit), texture,
                     BGFX_SAMPLER_POINT | BGFX_SAMPLER_UVW_CLAMP);
    ++perf().texture_binds;
    return true;
}

// The caster entry (effects/shadow_casters.hpp): offers a fixed-function draw into the game picture to the frame's
// shadow volumes, whether the camera drew it or not (`offscreen`: render::ShadowCasterOnly). What a volume takes is
// bound again with what a depth pass needs and nothing else: its buffers, the textures whose alpha cuts it, its bone
// palette. No colour program, no lighting variant, no relief or upscale demand.
void Device::draw_caster(const DrawCall &call, const LayoutInfo &layout, uint32_t element_count, bool offscreen)
{
    if (s.vs || s.ps || layout.rhw || rt != back_color)
        return;
    uint64_t primitive = 0;
    if (call.type == D3DPT_TRIANGLESTRIP)
        primitive = BGFX_STATE_PT_TRISTRIP;
    else if (call.type != D3DPT_TRIANGLELIST && call.type != D3DPT_TRIANGLEFAN)
        return;
    effects::Lighting &lighting = effects::Host::instance().lighting();
    effects::ShadowCasters &casters = lighting.shadow_casters();
    effects::ShadowCaster caster;
    caster.still = casters.still();
    caster.lit = s.rs[D3DRS_LIGHTING] != 0 && layout.has_normal;
    if ((!caster.lit && caster.still == 0) || lighting.idle())
        return;
    // What has no lighting variant casts nothing: slope-biased decals (projected shadows) and cube-mapped draws.
    if (as_float(s.rs[D3DRS_SLOPESCALEDEPTHBIAS]) != 0.0f)
        return;
    for (uint32_t stage = 0; stage < 3 && s.tss[stage][D3DTSS_COLOROP] != D3DTOP_DISABLE; ++stage)
        if (const TexStore *store = store_of(s.textures[stage]); store && store->kind == TexKind::Cube)
            return;
    if (!lighting.shadow_volumes(*this))
        return;
    caster.material = effects::caster_material(*this, layout);
    if (!caster.material.casts)
        return;
    caster.sun = caster.lit && lighting.sun_lit(*this);
    caster.skinned = layout.skinned;
    caster.offscreen = offscreen;
    caster.primitive = primitive;
    caster.triangles = call.primitive_count;
    caster.world = &s.world.m[0][0];
    caster.origin = lighting.world_origin();
    if (layout.skinned)
    {
        // Skinned vertices are in the bind pose: the model node being drawn bounds them, whatever the pose.
        if (const std::array<float, 4> &node = casters.bounds(); node[3] >= 0.0f)
        {
            caster.center = {node[0] - caster.origin[0], node[1] - caster.origin[1], node[2] - caster.origin[2]};
            caster.radius = node[3];
        }
    }
    else if (float center[3], radius = 0.0f; draw_bounds(call, layout, element_count, center, radius))
    {
        const D3DMATRIX &w = s.world;
        float scale = 0.0f;
        for (int k = 0; k < 3; ++k)
        {
            caster.center[k] = center[0] * w.m[0][k] + center[1] * w.m[1][k] + center[2] * w.m[2][k] + w.m[3][k];
            scale = std::max(scale, std::sqrt(w.m[k][0] * w.m[k][0] + w.m[k][1] * w.m[k][1] + w.m[k][2] * w.m[k][2]));
        }
        caster.radius = radius * scale;
    }
    casters.count_candidate();
    if (!casters.takes(caster))
        return;
    if (layout.skinned && !store_of(s.vertex_texture))
    {
        skip_draw("skinned caster without a bone palette in D3DVERTEXTEXTURESAMPLER0");
        return;
    }
    if (!bind_geometry(call, layout, element_count))
        return;
    for (uint32_t stage = 0; stage < 3; ++stage)
    {
        if (caster.material.stage[stage] <= 0.5f)
            continue;
        TexStore *store = store_of(s.textures[stage]); // a 2D texture with holes: caster_material()
        if (!sync_sampled(*store, true))
        {
            bgfx::discard(); // its texture is made in a later frame (realize_budget.hpp), counted
            return;
        }
        if (!bgfx::isValid(store->handle))
        {
            skip_draw("a caster's cut-out texture has no GPU texture");
            return;
        }
        bgfx::setTexture(static_cast<uint8_t>(stage), Context::instance().sampler(stage), store->handle, sampler_flags(stage, *store));
    }
    if (layout.skinned && !bind_palette())
        return;
    casters.submit(*this, lighting.casters(), caster);
}

} // namespace storm::shim
