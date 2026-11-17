#pragma once

#include "renderer.h"
#include "vma.hpp"
#include <geometry.h>

#include <string>
#include <vector>

// animated vertices
class DeferredGeometry;

// The files GEOMETRY::CreateGeometry reads for a model: the .gm, and its vertex colours of a light set (empty: none).
std::string ModelFile(const char *file_name);
std::string ModelColoursFile(const char *file_name, const char *light_file_name);
// The folder of the model's lightmap (GEOM_SERVICE_R::TextureFile of shadow.tga), set by CreateGeometry.
extern char lightPath[256];
void SetModelLightPath(const char *file_name, const char *lmPath);

class GEOMETRY final : public VGEOMETRY
{
    VRenderer *RenderService;
    std::vector<DeferredGeometry *> deferred_; // alive, in the order asked

  public:
    GEOMETRY();
    bool Init();
    bool LoadState(ENTITY_STATE *state);
    void RunStart() override;
    GEOS *CreateGeometry(const char *file_name, const char *light_file_name, int32_t flags, const char *lmPath);
    GEOS *CreateGeometryDeferred(const char *file_name, const char *light_file_name, int32_t flags,
                                 const char *lmPath) override;
    VRenderer *Renderer() const
    {
        return RenderService;
    }
    void Forget(DeferredGeometry *geometry);
    void DeleteGeometry(GEOS *);
    ANIMATION *LoadAnimation(const char *anim);
    void SetTechnique(const char *name);
    void SetVBConvertFunc(VERTEX_TRANSFORM _transform_func, void *context = nullptr);
    ANIMATION_VB GetAnimationVBDesc(int32_t avb);

    const char *GetTexturePath();
    void SetTexturePath(const char *);

    void SetCausticMode(bool bSet = false);
};

class GEOM_SERVICE_R final : public GEOM_SERVICE
{
    storm::render::VertexDeclarationId vertexDecl_;

    VRenderer *RenderService;
    GEOS::ID CurentIndexBuffer;
    GEOS::ID CurentVertexBuffer;
    uint32_t CurentVertexBufferSize;
    bool bCaustic;

  public:
    void SetRenderService(VRenderer *render_service);

    GEOM_FILE OpenFile(const char *fname) override;
    bool ReadFile(GEOM_FILE &file, void *data, int32_t bytes) override;
    void CloseFile(GEOM_FILE &file) override;
    void *malloc(int32_t bytes);
    void free(void *ptr);

    GEOS::ID CreateTexture(const char *fname, const char *requester) override;
    std::string TextureFile(const char *fname) const override;
    GEOS::ID MissingTexture() override;
    void SetMaterial(const GEOS::MATERIAL &mt);
    void ReleaseTexture(GEOS::ID tex);

    GEOS::ID CreateVertexBuffer(int32_t type, int32_t size);
    void *LockVertexBuffer(GEOS::ID vb);
    const void *ReadVertexBuffer(GEOS::ID vb) override;
    const void *ReadIndexBuffer(GEOS::ID ib) override;
    void UnlockVertexBuffer(GEOS::ID vb);
    void ReleaseVertexBuffer(GEOS::ID vb);

    GEOS::ID CreateIndexBuffer(int32_t size, bool progress = true);
    void *LockIndexBuffer(GEOS::ID ib);
    void UnlockIndexBuffer(GEOS::ID ib);
    void ReleaseIndexBuffer(GEOS::ID ib);

    void SetIndexBuffer(GEOS::ID ibuff);
    void SetVertexBuffer(int32_t vsize, GEOS::ID vbuff);
    void DrawIndexedPrimitive(int32_t minv, int32_t numv, int32_t vrtsize, int32_t startidx, int32_t numtrg);

    GEOS::ID CreateLight(const GEOS::LIGHT);
    void ActivateLight(GEOS::ID n);

    void SetCausticMode(bool bSet = false);
};
