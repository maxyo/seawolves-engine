#include "geom_deferred.h"

#include "core.h"
#include "geometry_r.h"
#include "geom_texture_names.hpp"

#include <cstring>
#include <set>

#include <spdlog/spdlog.h>

extern GEOM_SERVICE_R GSR;
extern char texturePath[256];

namespace
{
std::shared_ptr<storm::io::BackgroundRead> ReadFile(const std::string &file)
{
    return storm::io::ReadInBackground({fio->ConvertPathResource(file.c_str())});
}
} // namespace

DeferredGeometry::DeferredGeometry(GEOMETRY &service, std::string file, std::string light, int32_t flags,
                                   std::string lmPath, std::string texturePath)
    : service_(service), file_(std::move(file)), light_(std::move(light)), lmPath_(std::move(lmPath)),
      texturePath_(std::move(texturePath)), flags_(flags)
{
    model_ = ReadFile(ModelFile(file_.c_str()));
    if (const std::string colours = ModelColoursFile(file_.c_str(), light_.c_str()); !colours.empty())
        colours_ = ReadFile(colours);
}

DeferredGeometry::~DeferredGeometry()
{
    service_.Forget(this);
}

bool DeferredGeometry::Advance()
{
    if (state_ == State::ReadingModel && model_->ready() && (!colours_ || colours_->ready()))
        ReadTextures();
    if (state_ != State::ReadingTextures)
        return false;
    for (const auto &texture : textures_)
        if (!texture->ready())
            return false;
    return true;
}

// The textures as GEOM's constructor will create them (GEOM_SERVICE_R::CreateTexture: TextureFile of the name, with
// this model's texture path and light path), each the file the renderer would read for it, if it has not got it.
void DeferredGeometry::ReadTextures()
{
    state_ = State::ReadingTextures;
    if (!model_->bytes())
        return; // the build reports the missing model
    char savedTexturePath[sizeof(texturePath)], savedLightPath[sizeof(lightPath)];
    std::memcpy(savedTexturePath, texturePath, sizeof(texturePath));
    std::memcpy(savedLightPath, lightPath, sizeof(lightPath));
    strcpy_s(texturePath, texturePath_.c_str());
    SetModelLightPath(file_.c_str(), lmPath_.c_str());
    std::set<std::string> asked;
    for (const std::string &name : storm::geometry::TextureNames(*model_->bytes()))
    {
        const std::string texture = GSR.TextureFile(name.c_str());
        if (!asked.insert(texture).second)
            continue;
        if (std::vector<std::string> candidates = service_.Renderer()->TextureFileCandidates(texture.c_str());
            !candidates.empty())
            textures_.push_back(storm::io::ReadInBackground(std::move(candidates)));
    }
    std::memcpy(texturePath, savedTexturePath, sizeof(texturePath));
    std::memcpy(lightPath, savedLightPath, sizeof(lightPath));
}

void DeferredGeometry::Build()
{
    {
        storm::io::PrefetchScope prefetched;
        prefetched.add(*model_);
        if (colours_)
            prefetched.add(*colours_);
        for (const auto &texture : textures_)
            prefetched.add(*texture);
        const std::string saved = service_.GetTexturePath();
        service_.SetTexturePath(texturePath_.c_str());
        built_.reset(service_.CreateGeometry(file_.c_str(), light_.c_str(), flags_, lmPath_.c_str()));
        service_.SetTexturePath(saved.c_str());
    }
    model_.reset();
    colours_.reset();
    textures_.clear();
    state_ = built_ ? State::Built : State::Failed;
    service_.Forget(this);
    if (!built_)
        spdlog::error("geometry: {} (loaded while the game is played) could not be built: it is not drawn", file_);
}

void DeferredGeometry::LoadNow()
{
    if (!Pending())
        return;
    model_->wait();
    if (colours_)
        colours_->wait();
    Advance();
    for (const auto &texture : textures_)
        texture->wait();
    Build();
}

// Every call goes to the built model; an empty answer while it is pending (or failed).
int32_t DeferredGeometry::FindName(const char *name) const
{
    return built_ ? built_->FindName(name) : -1;
}
void DeferredGeometry::GetInfo(INFO &i) const
{
    if (built_)
        built_->GetInfo(i);
    else
        i = {};
}
int32_t DeferredGeometry::FindLabelN(int32_t start_index, int32_t name_id)
{
    return built_ ? built_->FindLabelN(start_index, name_id) : -1;
}
int32_t DeferredGeometry::FindLabelG(int32_t start_index, int32_t group_name_id)
{
    return built_ ? built_->FindLabelG(start_index, group_name_id) : -1;
}
void DeferredGeometry::GetLabel(int32_t l, LABEL &lb) const
{
    if (built_)
        built_->GetLabel(l, lb);
}
void DeferredGeometry::SetLabel(int32_t l, const LABEL &lb)
{
    if (built_)
        built_->SetLabel(l, lb);
}
int32_t DeferredGeometry::FindMaterialN(int32_t start_index, int32_t name_id)
{
    return built_ ? built_->FindMaterialN(start_index, name_id) : -1;
}
int32_t DeferredGeometry::FindMaterialG(int32_t start_index, int32_t group_name_id)
{
    return built_ ? built_->FindMaterialG(start_index, group_name_id) : -1;
}
void DeferredGeometry::GetMaterial(int32_t m, MATERIAL &mt) const
{
    if (built_)
        built_->GetMaterial(m, mt);
}
void DeferredGeometry::SetMaterial(int32_t m, const MATERIAL &mt)
{
    if (built_)
        built_->SetMaterial(m, mt);
}
int32_t DeferredGeometry::FindObjN(int32_t start_index, int32_t name_id)
{
    return built_ ? built_->FindObjN(start_index, name_id) : -1;
}
int32_t DeferredGeometry::FindObjG(int32_t start_index, int32_t group_name_id)
{
    return built_ ? built_->FindObjG(start_index, group_name_id) : -1;
}
void DeferredGeometry::GetObj(int32_t o, OBJECT &ob) const
{
    if (built_)
        built_->GetObj(o, ob);
}
void DeferredGeometry::SetObj(int32_t o, const OBJECT &ob)
{
    if (built_)
        built_->SetObj(o, ob);
}
int32_t DeferredGeometry::FindLightN(int32_t start_index, int32_t name_id)
{
    return built_ ? built_->FindLightN(start_index, name_id) : -1;
}
int32_t DeferredGeometry::FindLightG(int32_t start_index, int32_t group_name_id)
{
    return built_ ? built_->FindLightG(start_index, group_name_id) : -1;
}
void DeferredGeometry::GetLight(int32_t l, LIGHT &lt) const
{
    if (built_)
        built_->GetLight(l, lt);
}
void DeferredGeometry::SetLight(int32_t l, const LIGHT &lt)
{
    if (built_)
        built_->SetLight(l, lt);
}
void DeferredGeometry::Draw(const PLANE *pl, int32_t np, MATERIAL_FUNC mtf) const
{
    if (built_)
        built_->Draw(pl, np, mtf);
}
bool DeferredGeometry::HasFarLods() const
{
    return built_ && built_->HasFarLods();
}
float DeferredGeometry::Trace(VERTEX &src, VERTEX &dst)
{
    return built_ ? built_->Trace(src, dst) : 2.0f;
}
bool DeferredGeometry::Clip(const PLANE *planes, int32_t nplanes, const VERTEX &center, float radius,
                            ADD_POLYGON_FUNC addpoly)
{
    return built_ && built_->Clip(planes, nplanes, center, radius, addpoly);
}
bool DeferredGeometry::GetCollisionDetails(TRACE_INFO &ti) const
{
    return built_ && built_->GetCollisionDetails(ti);
}
int32_t DeferredGeometry::FindTexture(int32_t start_index, int32_t name_id)
{
    return built_ ? built_->FindTexture(start_index, name_id) : -1;
}
int32_t DeferredGeometry::GetTexture(int32_t tx) const
{
    return built_ ? built_->GetTexture(tx) : -1;
}
const char *DeferredGeometry::GetTextureName(int32_t tx) const
{
    return built_ ? built_->GetTextureName(tx) : "";
}
int32_t DeferredGeometry::GetVertexBuffer(int32_t vb) const
{
    return built_ ? built_->GetVertexBuffer(vb) : -1;
}
int32_t DeferredGeometry::GetIndexBuffer() const
{
    return built_ ? built_->GetIndexBuffer() : -1;
}
bool DeferredGeometry::AppendTriangles(std::vector<glm::vec3> &positions, std::vector<uint32_t> &indices,
                                       SurfaceData *surface) const
{
    return built_ && built_->AppendTriangles(positions, indices, surface);
}
bool DeferredGeometry::HasCollision() const
{
    return built_ && built_->HasCollision();
}
bool DeferredGeometry::LostBakedLight() const
{
    return built_ && built_->LostBakedLight();
}
const std::string &DeferredGeometry::LightmapFile() const
{
    static const std::string none;
    return built_ ? built_->LightmapFile() : none;
}
int32_t DeferredGeometry::LightmapTexture() const
{
    return built_ ? built_->LightmapTexture() : -1;
}
bool DeferredGeometry::AppendLightmapTriangles(std::vector<glm::vec3> &positions, std::vector<glm::vec2> &uvs,
                                               std::vector<uint32_t> &indices) const
{
    return built_ && built_->AppendLightmapTriangles(positions, uvs, indices);
}
