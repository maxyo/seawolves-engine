#include "storm/render/pipelines.hpp"
#include "background_read.hpp"
#include "core.h"
#include "platform/frame_work.hpp"
#include "fs.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include "geometry_r.h"
#include "geom_deferred.h"
#include "string_compare.hpp"

CREATE_SERVICE(GEOMETRY)

char technique[256] = "";
char RenderServiceName[] = "renderer";
GEOM_SERVICE_R GSR;
char texturePath[256];

const int32_t SHIFT_VALUE = 999999999;
#define AVB_MAX 1024
VGEOMETRY::ANIMATION_VB avb[AVB_MAX]; //!!! temporary

GEOMETRY::GEOMETRY()
{
    strcpy_s(texturePath, "");
}

const char *GEOMETRY::GetTexturePath()
{
    return &texturePath[0];
}

void GEOMETRY::SetTexturePath(const char *path)
{
    strcpy_s(texturePath, path);
}

// Block 1
void GEOMETRY::SetTechnique(const char *name)
{
    strcpy_s(technique, name);
}

GEOMETRY::ANIMATION_VB GEOMETRY::GetAnimationVBDesc(int32_t vb)
{
    return avb[vb - SHIFT_VALUE];
}

VERTEX_TRANSFORM transform_func = nullptr;
void *transform_context = nullptr;

void GEOMETRY::SetVBConvertFunc(VERTEX_TRANSFORM _transform_func, void *context)
{
    transform_func = _transform_func;
    transform_context = _transform_func ? context : nullptr;
}

static bool geoLog = false;

bool GEOMETRY::Init()
{
    RenderService = static_cast<VRenderer *>(core.GetService(RenderServiceName));
    if (!RenderService)
    {
        core.Trace("No service: %s", RenderServiceName);
        return false;
    }
    GSR.SetRenderService(RenderService);

    auto ini = fio->OpenIniFile(core.EngineIniFileName());
    if (ini)
    {
        geoLog = ini->GetInt(nullptr, "geometry_log", 0) == 1;
    }

    return true;
}

void GEOMETRY::SetCausticMode(bool bSet)
{
    GSR.SetCausticMode(bSet);
}

bool GEOMETRY::LoadState(ENTITY_STATE *state)
{
    return true;
}

char lightPath[256];
int vrtSize;

void SetModelLightPath(const char *file_name, const char *lmPath)
{
    sprintf_s(lightPath, "%s\\%s", lmPath, file_name);
    auto *const bs = strrchr(lightPath, '\\');
    if (bs != nullptr)
        *bs = 0;
}

std::string ModelFile(const char *file_name)
{
    return fmt::format("resource\\models\\{}.gm", file_name);
}

std::string ModelColoursFile(const char *file_name, const char *light_file_name)
{
    if (light_file_name == nullptr || light_file_name[0] == 0)
        return {};
    const auto *elf = light_file_name;
    if (elf[0] == '\\')
        elf++;
    if (elf[0] == '\\')
        elf++;
    return fmt::format("resource\\models\\{}_{}.col", file_name, elf);
}

GEOS *GEOMETRY::CreateGeometry(const char *file_name, const char *light_file_name, int32_t flags, const char *lmPath)
{
    const storm::platform::frame_work::SyncScope sync(storm::platform::frame_work::Sync::ModelLoad);
    if (light_file_name != nullptr)
        SetModelLightPath(file_name, lmPath);

    static auto first = 0;
    FILE *fl;
    if (geoLog)
    {
        vrtSize = 0;
        // the geometry log (ini geometry_log): in the engine's log folder, never in the game folder
        std::error_code error;
        std::filesystem::create_directories(fs::GetLogsPath(), error);
        const std::string log = (fs::GetLogsPath() / "geoLoad.txt").string();
        fl = fopen(log.c_str(), first == 0 ? "w" : "a");
    }

    GEOS *gp;
    const std::string fnt = ModelFile(file_name);
    try
    {
        const std::string lfn = ModelColoursFile(file_name, light_file_name);
        gp = ::CreateGeometry(fnt.c_str(), lfn.empty() ? nullptr : lfn.c_str(), GSR, flags);
    }
    catch (const std::exception &e)
    {
        core.Trace("%s: %s", fnt.c_str(), e.what());
        return nullptr;
    }
    catch (...)
    {
        core.Trace("Invalid model: %s", fnt.c_str());
        return nullptr;
    }

    if (geoLog)
    {
        // print statistics
        GEOS::INFO gi;
        gp->GetInfo(gi);
        first += gi.ntriangles * 2 * 3 + vrtSize;
        fprintf(fl, "%.2f, trgSize: %d, vrtSize: %d, tex: %d. obj: %d, %s\n", first / 1024.0f / 1024.0f,
                gi.ntriangles * 2 * 3, vrtSize, gi.ntextures, gi.nobjects, file_name);
        fclose(fl);
    }

    return gp;
}

GEOS *GEOMETRY::CreateGeometryDeferred(const char *file_name, const char *light_file_name, int32_t flags,
                                       const char *lmPath)
{
    // A model that does not exist is refused now, as CreateGeometry refuses it: the caller's script sees it fail.
    if (!fio->_FileOrDirectoryExists(ModelFile(file_name).c_str()))
        return CreateGeometry(file_name, light_file_name, flags, lmPath);
    auto *geometry = new DeferredGeometry(*this, file_name, light_file_name ? light_file_name : "", flags,
                                          lmPath ? lmPath : "", texturePath);
    deferred_.push_back(geometry);
    return geometry;
}

void GEOMETRY::Forget(DeferredGeometry *geometry)
{
    std::erase(deferred_, geometry);
}

// The models loaded while the game is played whose files have all been read are built here, in the order asked, from
// the frame after the one that asked for them (that frame already ran the script that spawned them). One is always
// built; more only while the frame has spent less than kBuildBudget on them, so that a crowd that walks in at once
// is spread over a few frames.
void GEOMETRY::RunStart()
{
    constexpr auto kBuildBudget = std::chrono::microseconds(2000);
    if (deferred_.empty())
        return;
    const auto start = std::chrono::steady_clock::now();
    bool built = false;
    // Building can load another model (a child node): iterate over a copy; one forgotten meanwhile is skipped.
    const std::vector<DeferredGeometry *> pending = deferred_;
    for (DeferredGeometry *geometry : pending)
    {
        if (std::find(deferred_.begin(), deferred_.end(), geometry) == deferred_.end() || !geometry->Pending())
            continue;
        const bool asked = geometry->AskedThisFrame();
        if (!geometry->Advance() || asked)
            continue;
        if (built && std::chrono::steady_clock::now() - start >= kBuildBudget)
            continue; // the next frame (its reads still move on)
        geometry->Build();
        built = true;
    }
}

ANIMATION *GEOMETRY::LoadAnimation(const char *anim)
{
    return nullptr;
}

void GEOMETRY::DeleteGeometry(GEOS *gid)
{
    delete gid;
}

// Block 2

void GEOM_SERVICE_R::SetRenderService(VRenderer *render_service)
{
    bCaustic = false;
    RenderService = render_service;
    CurentIndexBuffer = INVALID_BUFFER_ID;
    CurentVertexBuffer = INVALID_BUFFER_ID;
    CurentVertexBufferSize = 0;

    const D3DVERTEXELEMENT9 VertexElements[] = {
        {0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
        {0, 12, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
        {0, 24, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0},
        {0, 28, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 1},
        D3DDECL_END()};
    // GSR is a global that outlives each renderer: the declaration id belongs to this renderer.
    vertexDecl_ = RenderService->CreateVertexDeclaration(VertexElements);
}

GEOM_FILE GEOM_SERVICE_R::OpenFile(const char *fname)
{
    if (RenderService)
    {
        RenderService->ProgressView();
    }
    GEOM_FILE file;
    const std::string resolved = fio->ConvertPathResource(fname);
    if (const storm::io::Prefetched *prefetched = storm::io::FindPrefetched(resolved))
    {
        file.bytes = prefetched->bytes; // read by the background reader (none: it found no such file)
    }
    else if (std::ifstream stream(std::filesystem::u8path(resolved), std::ios::binary | std::ios::ate); stream.is_open())
    {
        const std::streamoff size = stream.tellg();
        auto bytes = std::make_shared<std::vector<char>>(size > 0 ? static_cast<size_t>(size) : 0);
        stream.seekg(0);
        if (size < 0 || !stream.read(bytes->data(), static_cast<std::streamsize>(bytes->size())))
            throw std::runtime_error("can't read geometry file");
        file.bytes = std::move(bytes);
    }
    // A missing vertex colour (.col) file is reported by the caller (geom_static.cpp getColData); a missing model
    // is an error.
    if (!file.is_open() && !storm::iEquals(&fname[strlen(fname) - 4], ".col"))
        throw std::runtime_error("can't open geometry file");

    return file;
}

bool GEOM_SERVICE_R::ReadFile(GEOM_FILE &file, void *data, int32_t bytes)
{
    const size_t wanted = bytes > 0 ? static_cast<size_t>(bytes) : 0;
    const size_t available = file.position < file.size() ? file.size() - file.position : 0;
    const size_t copied = std::min(wanted, available);
    if (copied)
        std::memcpy(data, file.bytes->data() + file.position, copied);
    file.position += copied;
    if (copied == wanted)
        return true;
    std::memset(static_cast<char *>(data) + copied, 0, wanted - copied);
    spdlog::error("geometry: a model file ends {} bytes early (zeroes read instead)", wanted - copied);
    return false;
}

void GEOM_SERVICE_R::CloseFile(GEOM_FILE &file)
{
    file = {};
}

void *GEOM_SERVICE_R::malloc(int32_t bytes)
{
    return new char[bytes];
}

void GEOM_SERVICE_R::free(void *ptr)
{
    delete[] static_cast<char*>(ptr);
}

std::string GEOM_SERVICE_R::TextureFile(const char *fname) const
{
    if (storm::iEquals(fname, "shadow.tga"))
        return std::string("lighting\\") + lightPath + "\\" + fname;
    return std::string(texturePath) + fname;
}

GEOS::ID GEOM_SERVICE_R::CreateTexture(const char *fname, const char *requester)
{
    const std::string tex = TextureFile(fname);
    if (RenderService)
    {
        RenderService->ProgressView();
        const auto texture = RenderService->TextureCreate(tex.c_str());
        // Mandatory model material, unlike optional interface probes through generic TextureCreate.
        if (texture < 0) storm::render::FailSceneTexture(tex.c_str(), requester);
        return texture;
    }
    return INVALID_TEXTURE_ID;
}

GEOS::ID GEOM_SERVICE_R::MissingTexture()
{
    return RenderService ? RenderService->TextureMissing() : INVALID_TEXTURE_ID;
}

void GEOM_SERVICE_R::SetCausticMode(bool bSet)
{
    bCaustic = bSet;
}

void GEOM_SERVICE_R::SetMaterial(const GEOS::MATERIAL &mt)
{
    RenderService->TextureSet(0, mt.texture[0]);

    if (bCaustic)
    {
        return;
    }

    if (mt.texture_type[1] != GEOS::TEXTURE_NONE)
    {
        // path BASE texture through
        RenderService->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG2);

        // stage #2
        RenderService->TextureSet(1, mt.texture[1]);
        // COLOR = BASE_C + DETAIL_C - 0.5
        RenderService->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_MODULATE2X);

        // COLOR = CURRENT_C * VERTEX_C * 2
        RenderService->TextureSet(2, 0);
        RenderService->SetTextureStageState(2, D3DTSS_COLOROP, D3DTOP_MODULATE2X);
    }
    else
    {
        RenderService->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE2X);

        RenderService->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
        RenderService->SetTextureStageState(2, D3DTSS_COLOROP, D3DTOP_DISABLE);
    }

    RenderService->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
    RenderService->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_TEXTURE);
    RenderService->SetTextureStageState(1, D3DTSS_COLORARG1, D3DTA_CURRENT);
    RenderService->SetTextureStageState(1, D3DTSS_COLORARG2, D3DTA_TEXTURE);
    RenderService->SetTextureStageState(2, D3DTSS_COLORARG1, D3DTA_CURRENT);
    RenderService->SetTextureStageState(2, D3DTSS_COLORARG2, D3DTA_DIFFUSE);

    RenderService->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);

    D3DMATERIAL9 m;
    m.Diffuse.r = m.Diffuse.g = m.Diffuse.b = mt.diffuse;
    m.Diffuse.a = 1.0f;
    m.Ambient.r = m.Ambient.g = m.Ambient.b = m.Ambient.a = 0.0f;
    m.Specular.r = m.Specular.g = m.Specular.b = mt.specular;
    m.Specular.a = 1.0f;
    m.Emissive.r = m.Emissive.g = m.Emissive.b = m.Emissive.a = 0.0f;
    m.Power = mt.gloss;

    RenderService->SetMaterial(m);
}

void GEOM_SERVICE_R::ReleaseTexture(GEOS::ID tex)
{
    if (RenderService)
        RenderService->TextureRelease(tex);
}

GEOS::ID GEOM_SERVICE_R::CreateVertexBuffer(int32_t type, int32_t size)
{
    if (size == 0)
        return INVALID_BUFFER_ID;
    if (!RenderService)
        return INVALID_BUFFER_ID;

    int32_t texset[4] = {D3DFVF_TEX1, D3DFVF_TEX2, D3DFVF_TEX3, D3DFVF_TEX4};
    const int32_t fvf = D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_DIFFUSE | D3DFVF_TEXTUREFORMAT2;
    const auto FVF = fvf | texset[type & 3];

    // animated vertices
    if (type & 4)
    {
        int32_t a;
        for (a = 0; a < AVB_MAX; a++)
            if (avb[a].nvertices == 0)
                break;
        if (a == AVB_MAX) return INVALID_BUFFER_ID;
        auto lifetime = std::make_shared<storm::render::VertexSourceLifetime>();
        avb[a].buff = new char[size];
        avb[a].lifetime = std::move(lifetime);
        avb[a].stride = sizeof(GEOS::VERTEX0);
        avb[a].nvertices = size / sizeof(GEOS::AVERTEX0);
        avb[a].fvf = FVF;
        return a + SHIFT_VALUE;
    }

    vrtSize += size;
    return RenderService->CreateVertexBuffer(FVF, size, D3DUSAGE_WRITEONLY);
}

void *GEOM_SERVICE_R::LockVertexBuffer(GEOS::ID vb)
{
    if (vb == INVALID_BUFFER_ID)
        return nullptr;
    if (RenderService == nullptr)
        return nullptr;
    if (vb >= SHIFT_VALUE)
        return avb[vb - SHIFT_VALUE].buff;
    return RenderService->LockVertexBuffer(vb);
}

const void *GEOM_SERVICE_R::ReadVertexBuffer(GEOS::ID vb)
{
    if (vb == INVALID_BUFFER_ID || RenderService == nullptr)
        return nullptr;
    if (vb >= SHIFT_VALUE)
        return avb[vb - SHIFT_VALUE].buff;
    return RenderService->LockVertexBuffer(vb, D3DLOCK_READONLY);
}

const void *GEOM_SERVICE_R::ReadIndexBuffer(GEOS::ID ib)
{
    if (ib == INVALID_BUFFER_ID || RenderService == nullptr)
        return nullptr;
    return RenderService->LockIndexBuffer(ib, D3DLOCK_READONLY);
}

void GEOM_SERVICE_R::UnlockVertexBuffer(GEOS::ID vb)
{
    if (vb == INVALID_BUFFER_ID)
        return;
    if (RenderService && vb < SHIFT_VALUE)
        RenderService->UnLockVertexBuffer(vb);
}

void GEOM_SERVICE_R::ReleaseVertexBuffer(GEOS::ID vb)
{
    if (vb == INVALID_BUFFER_ID)
        return;
    if (vb >= SHIFT_VALUE)
    {
        auto& source = avb[vb - SHIFT_VALUE];
        // Invalidate every model's dependent stream before source bytes can be
        // freed/reused, including direct NODE replacement and descriptor copies.
        if (source.lifetime) source.lifetime->invalidate();
        source.lifetime.reset();
        delete[] static_cast<char*>(source.buff);
        source.buff = nullptr;
        source.nvertices = 0;
    }
    else if (RenderService)
        RenderService->ReleaseVertexBuffer(vb);
}

GEOS::ID GEOM_SERVICE_R::CreateIndexBuffer(int32_t size, bool progress)
{
    if (RenderService && progress)
        RenderService->ProgressView();
    if (size == 0)
        return INVALID_BUFFER_ID;
    if (RenderService)
        return RenderService->CreateIndexBuffer(size);
    return INVALID_BUFFER_ID;
}

void *GEOM_SERVICE_R::LockIndexBuffer(GEOS::ID ib)
{
    if (ib == INVALID_BUFFER_ID)
        return nullptr;
    if (RenderService)
        return RenderService->LockIndexBuffer(ib);
    return nullptr;
}

void GEOM_SERVICE_R::UnlockIndexBuffer(GEOS::ID ib)
{
    if (ib == INVALID_BUFFER_ID)
        return;
    if (RenderService)
        RenderService->UnLockIndexBuffer(ib);
}

void GEOM_SERVICE_R::ReleaseIndexBuffer(GEOS::ID ib)
{
    if (ib == INVALID_BUFFER_ID)
        return;
    if (RenderService)
        RenderService->ReleaseIndexBuffer(ib);
}

void GEOM_SERVICE_R::SetIndexBuffer(GEOS::ID ibuff)
{
    CurentIndexBuffer = ibuff;
}

void GEOM_SERVICE_R::SetVertexBuffer(int32_t vsize, GEOS::ID vbuff)
{
    CurentVertexBuffer = vbuff;
    CurentVertexBufferSize = vsize;
}

void GEOM_SERVICE_R::DrawIndexedPrimitive(int32_t minv, int32_t numv, int32_t vrtsize, int32_t startidx, int32_t numtrg)
{
    if (!RenderService)
        return;

    if (bCaustic)
    {
        glm::mat4 mWorld, mView, mProjection;
        RenderService->GetTransform(D3DTS_WORLD, storm::math::d3dMatrix(mWorld));
        RenderService->GetTransform(D3DTS_PROJECTION, storm::math::d3dMatrix(mProjection));
        RenderService->GetTransform(D3DTS_VIEW, storm::math::d3dMatrix(mView));

        auto mWVP = mProjection * (mView * mWorld);

        mWVP = glm::transpose(mWVP);

        // constants
        // 0 - World * View * Projection
        // 4 - World

        RenderService->SetVertexShaderConstantF(0, storm::math::data(mWVP), 4);
        RenderService->SetVertexShaderConstantF(4, storm::math::data(mWorld), 4);

        RenderService->SetVertexDeclaration(vertexDecl_);

    }

    // draw animation
    if (transform_func != nullptr)
    {
        auto *cavb = &avb[CurentVertexBuffer - SHIFT_VALUE];

        const auto transformed = transform_func(transform_context,cavb->buff,cavb->lifetime,minv,numv,cavb->nvertices,!bCaustic);
        if (!transformed) return;
        if (!bCaustic)
            RenderService->DrawBuffer(transformed.value,sizeof(GEOS::AVERTEX0),CurentIndexBuffer,minv,numv,startidx,numtrg,technique);
        else
            RenderService->DrawIndexedPrimitiveNoVShader(D3DPT_TRIANGLELIST,transformed.value,cavb->stride,
                CurentIndexBuffer,minv,numv,startidx,numtrg,"caustic");
        return;
    }
    if (CurentIndexBuffer != INVALID_BUFFER_ID)
    {
        if (!bCaustic)
            RenderService->DrawBuffer(CurentVertexBuffer, vrtsize, CurentIndexBuffer, minv, numv, startidx, numtrg,
                                      technique);
        else
            RenderService->DrawIndexedPrimitiveNoVShader(D3DPT_TRIANGLELIST, CurentVertexBuffer, vrtsize,
                                                         CurentIndexBuffer, minv, numv, startidx, numtrg, "caustic");
    }

}

GEOS::ID GEOM_SERVICE_R::CreateLight(const GEOS::LIGHT)
{
    if (!RenderService)
        return INVALID_LIGHT_ID;
    return INVALID_LIGHT_ID;
}

void GEOM_SERVICE_R::ActivateLight(GEOS::ID n)
{
}
