// The characters' skin on their torso bones (fx/skin_tone.hpp, animation/skin_bones.hpp), over every character model
// of a game: per model and atlas, the skin reference read at its face and hands (as the renderer reads it: the texels
// under the vertices of the head, neck and hand bones), and how many texels of its torso and arms the colour test
// counts as skin. A bare chest is mostly skin; a shirt, a coat or leather should be next to none. Models far from the
// rest are flagged for a look.
//
//   check <game or RESOURCE directory> [--list N] [--model NAME]...
#include "skin_bones.hpp"
#include "storm/fx/skin_tone.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace
{
std::string Lower(std::string s)
{
    for (char &c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::vector<uint8_t> ReadAll(const fs::path &p)
{
    std::ifstream in(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
}

template <class T> T At(const std::vector<uint8_t> &d, size_t off)
{
    T v{};
    if (off + sizeof(T) <= d.size())
        std::memcpy(&v, d.data() + off, sizeof(T));
    return v;
}

// ---- skeletons ----
struct Skeleton
{
    std::string name;
    std::vector<glm::vec3> rest; // absolute
    std::vector<uint8_t> kind;   // 1 skin, 2 torso
};

std::optional<Skeleton> LoadSkeleton(const fs::path &file)
{
    const std::vector<uint8_t> d = ReadAll(file);
    if (d.size() < 12)
        return std::nullopt;
    const int n = At<int32_t>(d, 4);
    if (n <= 0 || d.size() < 12 + size_t(n) * 16)
        return std::nullopt;
    std::vector<int32_t> parents(n);
    std::vector<glm::vec3> offsets(n);
    std::memcpy(parents.data(), d.data() + 12, size_t(n) * 4);
    std::memcpy(offsets.data(), d.data() + 12 + size_t(n) * 4, size_t(n) * 12);
    parents[0] = -1;
    const storm::animation::SkinBones bones = storm::animation::FindSkinBones(parents, offsets);
    if (bones.skin.empty())
        return std::nullopt;
    Skeleton s;
    s.name = file.filename().string();
    s.rest.resize(n);
    s.kind.resize(n);
    for (int i = 0; i < n; ++i)
    {
        s.rest[i] = parents[i] < 0 ? offsets[i] : s.rest[parents[i]] + offsets[i];
        s.kind[i] = bones.skin[i] ? 1 : bones.torso[i] ? 2 : 0;
    }
    return s;
}

// ---- .tx textures: the level of at most 128 texels across, RGB ----
struct Image
{
    int w = 0, h = 0;
    std::vector<uint8_t> rgb;
    const uint8_t *At(float u, float v) const
    {
        u -= std::floor(u);
        v -= std::floor(v);
        const int x = std::clamp(static_cast<int>(u * w), 0, w - 1), y = std::clamp(static_cast<int>(v * h), 0, h - 1);
        return &rgb[(size_t(y) * w + x) * 3];
    }
};

void Rgb565(uint16_t c, uint8_t *o)
{
    o[0] = static_cast<uint8_t>(((c >> 11) & 31) * 255 / 31);
    o[1] = static_cast<uint8_t>(((c >> 5) & 63) * 255 / 63);
    o[2] = static_cast<uint8_t>((c & 31) * 255 / 31);
}

std::optional<Image> LoadTexture(const fs::path &file)
{
    const std::vector<uint8_t> d = ReadAll(file);
    if (d.size() < 24)
        return std::nullopt;
    const uint32_t flags = At<uint32_t>(d, 0), w = At<uint32_t>(d, 4), h = At<uint32_t>(d, 8), levels = At<uint32_t>(d, 12),
                   format = At<uint32_t>(d, 16), top = At<uint32_t>(d, 20);
    if (flags & 3 || !w || !h)
        return std::nullopt;
    const uint32_t dxt1 = 0x31545844, dxt3 = 0x33545844, dxt5 = 0x35545844;
    const bool block = format == dxt1 || format == dxt3 || format == dxt5;
    const auto level_bytes = [&](uint32_t lw, uint32_t lh) -> size_t {
        if (block)
            return size_t(std::max(1u, lw / 4)) * std::max(1u, lh / 4) * (format == dxt1 ? 8 : 16);
        return size_t(lw) * lh * 4;
    };
    if (!block && format != 21 && format != 22)
        return std::nullopt; // the few 16-bit atlases: not read here (listed as unreadable)
    if (level_bytes(w, h) != top)
        return std::nullopt;
    size_t off = 24;
    uint32_t lw = w, lh = h, level = 0;
    while (level + 1 < levels && std::max(lw, lh) > 128)
    {
        off += level_bytes(lw, lh);
        lw = std::max(1u, lw / 2);
        lh = std::max(1u, lh / 2);
        ++level;
    }
    if (off + level_bytes(lw, lh) > d.size() || lw < 4 || lh < 4)
        return std::nullopt;
    Image img;
    img.w = static_cast<int>(lw);
    img.h = static_cast<int>(lh);
    img.rgb.resize(size_t(lw) * lh * 3);
    if (!block)
    {
        for (size_t i = 0; i < size_t(lw) * lh; ++i)
        {
            const uint8_t *p = &d[off + i * 4]; // B, G, R, A
            img.rgb[i * 3] = p[2];
            img.rgb[i * 3 + 1] = p[1];
            img.rgb[i * 3 + 2] = p[0];
        }
        return img;
    }
    const size_t bsize = format == dxt1 ? 8 : 16;
    for (uint32_t by = 0; by < lh / 4; ++by)
        for (uint32_t bx = 0; bx < lw / 4; ++bx)
        {
            const uint8_t *b = &d[off + (size_t(by) * (lw / 4) + bx) * bsize + (bsize - 8)];
            const uint16_t c0 = static_cast<uint16_t>(b[0] | b[1] << 8), c1 = static_cast<uint16_t>(b[2] | b[3] << 8);
            uint8_t pal[4][3];
            Rgb565(c0, pal[0]);
            Rgb565(c1, pal[1]);
            for (int k = 0; k < 3; ++k)
            {
                if (c0 > c1 || format != dxt1)
                {
                    pal[2][k] = static_cast<uint8_t>((2 * pal[0][k] + pal[1][k]) / 3);
                    pal[3][k] = static_cast<uint8_t>((pal[0][k] + 2 * pal[1][k]) / 3);
                }
                else
                {
                    pal[2][k] = static_cast<uint8_t>((pal[0][k] + pal[1][k]) / 2);
                    pal[3][k] = 0;
                }
            }
            const uint32_t bits = static_cast<uint32_t>(b[4] | b[5] << 8 | b[6] << 16 | b[7] << 24);
            for (int py = 0; py < 4; ++py)
                for (int px = 0; px < 4; ++px)
                {
                    const uint32_t idx = (bits >> (2 * (py * 4 + px))) & 3;
                    std::memcpy(&img.rgb[((size_t(by) * 4 + py) * lw + bx * 4 + px) * 3], pal[idx], 3);
                }
        }
    return img;
}

// The image box-filtered over `n` x `n` texels (wrapping), at the same size.
Image Coarse(const Image &img, int n)
{
    Image out = img;
    for (int y = 0; y < img.h; ++y)
        for (int x = 0; x < img.w; ++x)
            for (int c = 0; c < 3; ++c)
            {
                int sum = 0;
                for (int dy = 0; dy < n; ++dy)
                    for (int dx = 0; dx < n; ++dx)
                    {
                        const int sx = ((x / n) * n + dx) % img.w, sy = ((y / n) * n + dy) % img.h;
                        sum += img.rgb[(size_t(sy) * img.w + sx) * 3 + c];
                    }
                out.rgb[(size_t(y) * img.w + x) * 3 + c] = static_cast<uint8_t>(sum / (n * n));
            }
    return out;
}

// ---- .gm character models ----
struct Vertex
{
    glm::vec3 pos, nrm;
    float weight;
    uint8_t b0, b1;
    float u, v;
};
struct Part
{
    std::string texture;
    std::vector<Vertex> verts;
    std::vector<uint32_t> tris;
};

std::vector<Part> LoadModel(const fs::path &file, int &max_bone)
{
    std::vector<Part> parts;
    const std::vector<uint8_t> d = ReadAll(file);
    if (d.size() < 80 || At<int32_t>(d, 0) != 0x312e3035) // RDF_VERSION '1.05'
        return parts;
    const int name_size = At<int32_t>(d, 8), names = At<int32_t>(d, 12), ntex = At<int32_t>(d, 16), nmat = At<int32_t>(d, 20),
              nlight = At<int32_t>(d, 24), nlabel = At<int32_t>(d, 28), nobj = At<int32_t>(d, 32), ntri = At<int32_t>(d, 36),
              nvb = At<int32_t>(d, 40);
    size_t off = 44 + 24 + 4; // header with the two vec3 and the radius
    const char *glob = reinterpret_cast<const char *>(d.data() + off);
    const size_t glob_off = off;
    off += size_t(name_size) + size_t(names) * 4;
    std::vector<int32_t> tex(ntex);
    for (int i = 0; i < ntex; ++i)
        tex[i] = At<int32_t>(d, off + size_t(i) * 4);
    off += size_t(ntex) * 4;
    const size_t mat_size = 4 * 2 + 4 * 4 + 16 + 16; // RDF_MATERIAL
    std::vector<int32_t> mat_tex(nmat);
    for (int i = 0; i < nmat; ++i)
    {
        const size_t m = off + size_t(i) * mat_size;
        mat_tex[i] = At<int32_t>(d, m + 24 + 16) >= 0 && At<int32_t>(d, m + 24) == 1 ? At<int32_t>(d, m + 24 + 16) : -1;
    }
    off += size_t(nmat) * mat_size;
    off += size_t(nlight) * (4 * 4 + 12 + 4 + 12 + 12 + 12 + 12);
    off += size_t(nlabel) * (12 + 64 + 16 + 16);
    struct Obj
    {
        int vb, ntri, stri, nv, sv, mat;
    };
    std::vector<Obj> objs(nobj);
    const size_t obj_size = 12 + 12 + 4 + 20 + 4 + 32 + 16 + 4; // RDF_OBJECT: 104 bytes
    for (int i = 0; i < nobj; ++i)
    {
        const size_t o = off + size_t(i) * obj_size;
        objs[i] = {At<int32_t>(d, o + 28), At<int32_t>(d, o + 32), At<int32_t>(d, o + 36), At<int32_t>(d, o + 40),
                   At<int32_t>(d, o + 44), At<int32_t>(d, o + 48)};
    }
    off += size_t(nobj) * obj_size;
    const size_t tri_off = off;
    off += size_t(ntri) * 6;
    std::vector<std::pair<int, int>> vbs(nvb);
    for (int i = 0; i < nvb; ++i)
        vbs[i] = {At<int32_t>(d, off + size_t(i) * 8), At<int32_t>(d, off + size_t(i) * 8 + 4)};
    off += size_t(nvb) * 8;
    std::vector<size_t> vb_off(nvb);
    for (int i = 0; i < nvb; ++i)
    {
        vb_off[i] = off;
        off += static_cast<size_t>(vbs[i].second);
    }
    max_bone = 0;
    for (const Obj &o : objs)
    {
        if (o.vb < 0 || o.vb >= nvb || !(vbs[o.vb].first & 4) || o.mat < 0 || o.mat >= nmat || mat_tex[o.mat] < 0 ||
            mat_tex[o.mat] >= ntex)
            continue;
        const int stride = 36 + (vbs[o.vb].first & 3) * 8 + (vbs[o.vb].first >> 2) * 8;
        if (stride != 44)
            continue;
        Part p;
        const int name = tex[mat_tex[o.mat]];
        if (name < 0 || static_cast<size_t>(name) >= static_cast<size_t>(name_size))
            continue;
        p.texture = std::string(glob + name);
        (void)glob_off;
        for (int v = 0; v < o.nv; ++v)
        {
            const size_t a = vb_off[o.vb] + size_t(o.sv + v) * 44;
            Vertex x;
            x.pos = At<glm::vec3>(d, a);
            x.weight = At<float>(d, a + 12);
            const uint32_t bones = At<uint32_t>(d, a + 16);
            x.b0 = static_cast<uint8_t>(bones & 255);
            x.b1 = static_cast<uint8_t>((bones >> 8) & 255);
            x.nrm = At<glm::vec3>(d, a + 20);
            x.u = At<float>(d, a + 36);
            x.v = At<float>(d, a + 40);
            max_bone = std::max({max_bone, int(x.b0), int(x.b1)});
            p.verts.push_back(x);
        }
        for (int t = 0; t < o.ntri; ++t)
            for (int k = 0; k < 3; ++k)
                p.tris.push_back(At<uint16_t>(d, tri_off + size_t(o.stri + t) * 6 + size_t(k) * 2));
        parts.push_back(std::move(p));
    }
    return parts;
}

float Weight(const Skeleton &s, const Vertex &v, uint8_t kind)
{
    const auto f = [&](uint8_t b) { return b < s.kind.size() && s.kind[b] == kind ? 1.0f : 0.0f; };
    const float w = std::clamp(v.weight, 0.0f, 1.0f);
    return f(v.b0) * w + f(v.b1) * (1.0f - w);
}

// A vertex the skin reference is read under (as effects/skin_tones.cpp): carried by the head, neck and hand bones, and
// facing forward in the bind pose (+z; the face, not the hair, the hat or the back of the head).
bool SkinReference(const Skeleton &s, const Vertex &v)
{
    return Weight(s, v, 1) >= 0.9f && v.nrm.z > 0.5f * glm::length(v.nrm);
}

struct Row
{
    std::string model, texture, skeleton;
    int samples = 0, torso = 0, masked = 0;
    float r = 0, g = 0, lo = 0, hi = 0;
    double share() const { return torso ? double(masked) / torso : 0.0; }
};
} // namespace

int main(int argc, char **argv)
{
    if (argc < 2)
    {
        std::fprintf(stderr, "usage: %s <game or RESOURCE directory> [--list N] [--model NAME]...\n", argv[0]);
        return 2;
    }
    fs::path root = argv[1];
    if (fs::exists(root / "RESOURCE"))
        root /= "RESOURCE";
    int list = 25;
    std::vector<std::string> only;
    fs::path dump; // --dump DIR: per listed model atlas a PPM: the atlas, its torso texels tinted blue, masked red, skin samples green
    for (int i = 2; i + 1 < argc; i += 2)
    {
        if (std::string(argv[i]) == "--list")
            list = std::atoi(argv[i + 1]);
        else if (std::string(argv[i]) == "--model")
            only.push_back(Lower(argv[i + 1]));
        else if (std::string(argv[i]) == "--dump")
            dump = argv[i + 1];
    }
    std::vector<Skeleton> skeletons;
    std::map<std::string, fs::path> textures;
    fs::path models_dir;
    for (const auto &e : fs::recursive_directory_iterator(root))
    {
        if (!e.is_regular_file())
            continue;
        const std::string ext = Lower(e.path().extension().string());
        const std::string parent = Lower(e.path().parent_path().filename().string());
        if (ext == ".an")
            if (auto s = LoadSkeleton(e.path()))
                skeletons.push_back(std::move(*s));
        if (ext == ".tx" && parent == "characters")
            textures[Lower(e.path().stem().stem().string())] = e.path(); // "x.tga.tx" -> "x"
        if (e.is_regular_file() && parent == "characters" && ext == ".gm" &&
            Lower(e.path().parent_path().parent_path().filename().string()) == "models")
            models_dir = e.path().parent_path();
    }
    std::vector<Row> rows;
    int unreadable = 0, no_reference = 0, no_skeleton = 0;
    for (const auto &e : fs::directory_iterator(models_dir))
    {
        if (Lower(e.path().extension().string()) != ".gm")
            continue;
        const std::string model = e.path().stem().string();
        if (!only.empty() && std::find(only.begin(), only.end(), Lower(model)) == only.end())
            continue;
        int max_bone = 0;
        const std::vector<Part> parts = LoadModel(e.path(), max_bone);
        if (parts.empty())
            continue;
        // The skeleton the model was made for: the one whose bones' rest positions lie nearest its vertices.
        const Skeleton *best = nullptr;
        double best_cost = 1e30;
        for (const Skeleton &s : skeletons)
        {
            if (static_cast<int>(s.rest.size()) <= max_bone)
                continue;
            double cost = 0;
            size_t n = 0;
            for (const Part &p : parts)
                for (size_t i = 0; i < p.verts.size(); i += 7, ++n)
                    cost += glm::length(p.verts[i].pos - s.rest[p.verts[i].b0]);
            if (n && cost / n < best_cost)
            {
                best_cost = cost / n;
                best = &s;
            }
        }
        if (!best)
        {
            ++no_skeleton;
            continue;
        }
        // Per atlas: the reference from the whole model's skin vertices on it, then its torso texels.
        std::map<std::string, std::vector<const Part *>> by_texture;
        for (const Part &p : parts)
            by_texture[Lower(fs::path(p.texture).stem().string())].push_back(&p);
        for (const auto &[texture, list_parts] : by_texture)
        {
            const auto file = textures.find(texture);
            const std::optional<Image> img = file == textures.end() ? std::nullopt : LoadTexture(file->second);
            if (!img)
            {
                ++unreadable;
                continue;
            }
            std::vector<uint8_t> samples;
            for (const Part *p : list_parts)
                for (const Vertex &v : p->verts)
                    if (SkinReference(*best, v))
                        for (int dy = -1; dy <= 1; ++dy)
                            for (int dx = -1; dx <= 1; ++dx)
                            {
                                const uint8_t *t = img->At(v.u + dx / float(img->w), v.v + dy / float(img->h));
                                samples.insert(samples.end(), t, t + 3);
                            }
            const storm::fx::SkinTone tone = storm::fx::MakeSkinTone(samples);
            Row row{model, texture, best->name, tone.samples};
            row.r = tone.r;
            row.g = tone.g;
            row.lo = tone.lum_lo;
            row.hi = tone.lum_hi;
            // Torso texels: the triangles with torso weight, rasterised in uv at the level's size (each texel once).
            std::vector<float> torso(size_t(img->w) * img->h, 0.0f);
            for (const Part *p : list_parts)
                for (size_t t = 0; t + 2 < p->tris.size(); t += 3)
                {
                    const Vertex *v[3];
                    bool ok = true;
                    for (int k = 0; k < 3; ++k)
                    {
                        if (p->tris[t + k] >= p->verts.size())
                            ok = false;
                        else
                            v[k] = &p->verts[p->tris[t + k]];
                    }
                    if (!ok)
                        continue;
                    float w[3];
                    for (int k = 0; k < 3; ++k)
                        w[k] = Weight(*best, *v[k], 2);
                    if (w[0] + w[1] + w[2] <= 0.0f)
                        continue;
                    float x[3], y[3];
                    for (int k = 0; k < 3; ++k)
                    {
                        x[k] = v[k]->u * img->w;
                        y[k] = v[k]->v * img->h;
                    }
                    const float area = (x[1] - x[0]) * (y[2] - y[0]) - (x[2] - x[0]) * (y[1] - y[0]);
                    if (std::abs(area) < 1e-6f)
                        continue;
                    const int x0 = static_cast<int>(std::floor(std::min({x[0], x[1], x[2]}))),
                              x1 = static_cast<int>(std::ceil(std::max({x[0], x[1], x[2]}))),
                              y0 = static_cast<int>(std::floor(std::min({y[0], y[1], y[2]}))),
                              y1 = static_cast<int>(std::ceil(std::max({y[0], y[1], y[2]})));
                    for (int py = y0; py <= y1; ++py)
                        for (int px = x0; px <= x1; ++px)
                        {
                            const float cx = px + 0.5f, cy = py + 0.5f;
                            const float a = ((x[1] - cx) * (y[2] - cy) - (x[2] - cx) * (y[1] - cy)) / area;
                            const float b = ((x[2] - cx) * (y[0] - cy) - (x[0] - cx) * (y[2] - cy)) / area;
                            const float c = 1.0f - a - b;
                            if (a < 0 || b < 0 || c < 0)
                                continue;
                            const int wx = ((px % img->w) + img->w) % img->w, wy = ((py % img->h) + img->h) % img->h;
                            float &cell = torso[size_t(wy) * img->w + wx];
                            cell = std::max(cell, a * w[0] + b * w[1] + c * w[2]);
                        }
                }
            // The torso test reads the atlas one mip level coarser (fx::kSkinTestTexels, ff_fragment.sh): a bare chest
            // stays skin, the grain of leather and dark cloth averages into its own colour.
            const Image coarse = Coarse(*img, 2);
            for (size_t i = 0; i < torso.size(); ++i)
            {
                if (torso[i] < 0.5f)
                    continue;
                ++row.torso;
                const uint8_t *t = &coarse.rgb[i * 3];
                // Same conservative invalid-reference fallback and partial torso weights as the runtime shader.
                if (storm::fx::TorsoSkinMask(tone, torso[i], t[0] / 255.0f, t[1] / 255.0f, t[2] / 255.0f) > 0.5f)
                    ++row.masked;
            }
            if (!dump.empty())
            {
                // Left the atlas, right the mask: white = torso texel counted as skin, dark blue = torso texel kept,
                // green = where the skin reference was read, black = neither.
                const int w2 = img->w * 2;
                std::vector<uint8_t> out(size_t(w2) * img->h * 3, 0);
                for (int y = 0; y < img->h; ++y)
                    for (int x = 0; x < img->w; ++x)
                    {
                        const size_t i = size_t(y) * img->w + x;
                        std::memcpy(&out[(size_t(y) * w2 + x) * 3], &img->rgb[i * 3], 3);
                        uint8_t *o = &out[(size_t(y) * w2 + img->w + x) * 3];
                        if (torso[i] >= 0.5f)
                        {
                            const uint8_t *t = &coarse.rgb[i * 3];
                            const bool m = storm::fx::TorsoSkinMask(tone, torso[i], t[0] / 255.0f, t[1] / 255.0f, t[2] / 255.0f) > 0.5f;
                            o[0] = m ? 255 : 30;
                            o[1] = m ? 255 : 40;
                            o[2] = m ? 255 : 110;
                        }
                    }
                for (const Part *p : list_parts)
                    for (const Vertex &v : p->verts)
                        if (SkinReference(*best, v))
                        {
                            const size_t i = size_t(img->At(v.u, v.v) - img->rgb.data()) / 3;
                            uint8_t *o = &out[((i / img->w) * w2 + img->w + i % img->w) * 3];
                            o[0] = 0;
                            o[1] = 200;
                            o[2] = 0;
                        }
                fs::create_directories(dump);
                std::ofstream ppm(dump / (model + "_" + texture + ".ppm"), std::ios::binary);
                ppm << "P6\n" << w2 << " " << img->h << "\n255\n";
                ppm.write(reinterpret_cast<const char *>(out.data()), static_cast<std::streamsize>(out.size()));
            }
            if (std::getenv("SKIN_DEBUG") && tone.valid)
            {
                const double det = double(tone.inv_rr) * tone.inv_gg - double(tone.inv_rg) * tone.inv_rg;
                std::printf("  %s/%s: tone r %.3f g %.3f sd_r %.4f sd_g %.4f lum %.2f..%.2f\n", model.c_str(), texture.c_str(), tone.r,
                            tone.g, std::sqrt(tone.inv_gg / det), std::sqrt(tone.inv_rr / det), tone.lum_lo, tone.lum_hi);
                int shown = 0;
                for (size_t i = 0; i < torso.size() && shown < 12; i += 7)
                    if (torso[i] >= 0.5f &&
                        storm::fx::SkinLikeness(tone, img->rgb[i * 3] / 255.0f, img->rgb[i * 3 + 1] / 255.0f, img->rgb[i * 3 + 2] / 255.0f) > 0.5f)
                    {
                        const uint8_t *t = &img->rgb[i * 3];
                        const float sum = std::max(1, t[0] + t[1] + t[2]);
                        std::printf("    texel %3d %3d %3d chroma %.3f %.3f like %.2f\n", t[0], t[1], t[2], t[0] / sum, t[1] / sum,
                                    storm::fx::SkinLikeness(tone, t[0] / 255.0f, t[1] / 255.0f, t[2] / 255.0f));
                        ++shown;
                    }
            }
            if (!tone.valid)
                ++no_reference;
            if (row.torso > 0)
                rows.push_back(row);
        }
    }
    std::sort(rows.begin(), rows.end(), [](const Row &a, const Row &b) { return a.share() > b.share(); });
    std::vector<double> shares;
    for (const Row &r : rows)
        shares.push_back(r.share());
    std::vector<double> sorted = shares;
    std::sort(sorted.begin(), sorted.end());
    const double median = sorted.empty() ? 0 : sorted[sorted.size() / 2];
    std::vector<double> dev;
    for (double s : shares)
        dev.push_back(std::abs(s - median));
    std::sort(dev.begin(), dev.end());
    const double mad = dev.empty() ? 0 : dev[dev.size() / 2];
    const double flag = std::max(median + 6 * mad, 0.25);
    int flagged = 0;
    std::printf("%-26s %-22s %-14s %7s %13s %11s %8s\n", "model", "atlas", "skeleton", "skin px", "chroma r,g", "torso px",
                "masked");
    for (size_t i = 0; i < rows.size(); ++i)
    {
        const Row &r = rows[i];
        const bool out = r.share() > flag;
        flagged += out ? 1 : 0;
        if (static_cast<int>(i) < list || !only.empty())
            std::printf("%-26s %-22s %-14s %7d  %.3f,%.3f %11d %7.1f%%%s\n", r.model.c_str(), r.texture.c_str(), r.skeleton.c_str(),
                        r.samples / 9, r.r, r.g, r.torso, 100.0 * r.share(), out ? "  FLAG" : "");
    }
    std::printf("%zu model atlases with torso texels: median %.1f%% masked, MAD %.1f%%; %d flagged above %.1f%%; %d without a "
                "skin reference (conservative torso mask), %d atlases unreadable, %d models without a skeleton\n",
                rows.size(), 100 * median, 100 * mad, flagged, 100 * flag, no_reference, unreadable, no_skeleton);
    return 0;
}
