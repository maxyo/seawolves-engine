// A model loaded while the game is played (VGEOMETRY::CreateGeometryDeferred): its files are read on the background
// reader thread, in two steps, and the model is built from them on the game thread (GEOMETRY::RunStart):
//
//   1. the model file and its vertex colours;
//   2. the textures its file names that the renderer has not loaded yet (the names are in the model file);
//   3. built: GEOMETRY::CreateGeometry inside a storm::io::PrefetchScope, which hands those bytes to the readers.
//
// Until it is built the geometry is empty (Pending): no names, labels, objects, textures; it draws nothing, a ray
// misses it. Afterwards every call goes to the built model. A model that cannot be built stays empty (logged).
#pragma once

#include "background_read.hpp"
#include "geos.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

class GEOMETRY;

class DeferredGeometry final : public GEOS
{
  public:
    DeferredGeometry(GEOMETRY &service, std::string file, std::string light, int32_t flags, std::string lmPath,
                     std::string texturePath);
    ~DeferredGeometry() override;

    // Moves the reads on (step 1 to 2); true when the files are all read and the model can be built.
    bool Advance();
    // Builds the model from the files read (Advance returned true).
    void Build();
    // True in the frame that asked for it (its first GEOMETRY::RunStart).
    bool AskedThisFrame()
    {
        return std::exchange(asked_, false);
    }

    bool Pending() const override
    {
        return state_ != State::Built && state_ != State::Failed;
    }
    void LoadNow() override;

    int32_t FindName(const char *name) const override;
    void GetInfo(INFO &i) const override;
    int32_t FindLabelN(int32_t start_index, int32_t name_id) override;
    int32_t FindLabelG(int32_t start_index, int32_t group_name_id) override;
    void GetLabel(int32_t l, LABEL &lb) const override;
    void SetLabel(int32_t l, const LABEL &lb) override;
    int32_t FindMaterialN(int32_t start_index, int32_t name_id) override;
    int32_t FindMaterialG(int32_t start_index, int32_t group_name_id) override;
    void GetMaterial(int32_t m, MATERIAL &mt) const override;
    void SetMaterial(int32_t m, const MATERIAL &mt) override;
    int32_t FindObjN(int32_t start_index, int32_t name_id) override;
    int32_t FindObjG(int32_t start_index, int32_t group_name_id) override;
    void GetObj(int32_t o, OBJECT &ob) const override;
    void SetObj(int32_t o, const OBJECT &ob) override;
    int32_t FindLightN(int32_t start_index, int32_t name_id) override;
    int32_t FindLightG(int32_t start_index, int32_t group_name_id) override;
    void GetLight(int32_t l, LIGHT &lt) const override;
    void SetLight(int32_t l, const LIGHT &lt) override;
    void Draw(const PLANE *pl, int32_t np, MATERIAL_FUNC mtf) const override;
    bool HasFarLods() const override;
    float Trace(VERTEX &src, VERTEX &dst) override;
    bool Clip(const PLANE *planes, int32_t nplanes, const VERTEX &center, float radius,
              ADD_POLYGON_FUNC addpoly) override;
    bool GetCollisionDetails(TRACE_INFO &ti) const override;
    int32_t FindTexture(int32_t start_index, int32_t name_id) override;
    int32_t GetTexture(int32_t tx) const override;
    const char *GetTextureName(int32_t tx) const override;
    int32_t GetVertexBuffer(int32_t vb) const override;
    int32_t GetIndexBuffer() const override;
    bool AppendTriangles(std::vector<glm::vec3> &positions, std::vector<uint32_t> &indices,
                         SurfaceData *surface) const override;
    bool HasCollision() const override;
    bool LostBakedLight() const override;
    const std::string &LightmapFile() const override;
    int32_t LightmapTexture() const override;
    bool AppendLightmapTriangles(std::vector<glm::vec3> &positions, std::vector<glm::vec2> &uvs,
                                 std::vector<uint32_t> &indices) const override;

  private:
    enum class State
    {
        ReadingModel,
        ReadingTextures,
        Built,
        Failed,
    };
    void ReadTextures();

    GEOMETRY &service_;
    const std::string file_, light_, lmPath_, texturePath_;
    const int32_t flags_;
    State state_ = State::ReadingModel;
    bool asked_ = true;
    std::shared_ptr<storm::io::BackgroundRead> model_, colours_;
    std::vector<std::shared_ptr<storm::io::BackgroundRead>> textures_;
    std::unique_ptr<GEOS> built_;
};
