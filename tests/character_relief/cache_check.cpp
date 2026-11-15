#include "storm/fx/skin_tone.hpp"
#include "storm/fx/texture_pixels.hpp"
#include "storm/render/skinned_vertex.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>
using namespace storm;
void expect(bool value,const char *what) {if(!value){std::fprintf(stderr,"FAIL %s\n",what);std::exit(1);}std::puts(what);}
namespace storm::shim {
struct TexStore {
 uint32_t width=4,height=4;std::string name="characters/test";
 std::shared_ptr<const fx::TexturePixels> skin_pixels;
 std::shared_ptr<const fx::SkinTone> skin_tone;
 bool skin_tone_read=false;uint32_t skin_vertex_cursor=0;std::vector<uint8_t> skin_samples;
 std::shared_ptr<int> cutout,albedo;bool lightmap=false;
};
struct BaseTexture {TexStore *store;};TexStore *store_of(BaseTexture *t){return t->store;}
std::shared_ptr<TexStore> shared_store_of(BaseTexture *t){return {std::shared_ptr<TexStore>{},t->store};}
void logf(const char *,...){}
}
struct DrawVertices {const uint8_t *data=nullptr;uint32_t stride=0,count=0;};
struct DrawSphereSource {DrawVertices value;mutable int reads=0;DrawVertices vertices()const{++reads;return value;}};
struct Lighting {std::array<float,8> skin_tone(shim::TexStore &,const DrawSphereSource &);};
namespace storm::fx {std::string NormalizeTexturePath(const char *p){return p;}}
namespace storm::effects {
std::shared_ptr<const fx::TexturePixels> next_pixels;
std::shared_ptr<int> texture_cutout(shim::TexStore &){return {};}
std::shared_ptr<const fx::TexturePixels> texture_level(shim::TexStore &,uint32_t){return next_pixels;}
struct Maps {void on_texture_loaded(shim::TexStore &,const std::string_view,const std::filesystem::path &) {}};
struct EngineLighting {Maps maps;Maps &normal_maps(){return maps;}};
struct Host {EngineLighting value;static Host &instance(){static Host h;return h;}EngineLighting &lighting(){return value;}};
Maps &upscales(){static Maps m;return m;}
// The scene readiness observer is not part of this check: no texture is a scene texture here.
bool SceneTextureName(std::string_view){return false;}
void ObserveSceneTexture(const std::shared_ptr<shim::TexStore> &){}
}
// Production functions inserted by cache_run.py here.
PRODUCTION_FUNCTIONS

std::shared_ptr<const fx::TexturePixels> pixels(int r=180,int g=120,int b=80) {
 auto p=std::make_shared<fx::TexturePixels>();p->width=p->height=4;
 for(int i=0;i<16;++i){p->srgb.push_back(r);p->srgb.push_back(g);p->srgb.push_back(b);}return p;
}
std::vector<uint8_t> vertices(uint32_t n,bool face) {
 std::vector<uint8_t> v(n*render::kSkinnedSourceStride);
 for(uint32_t i=0;i<n;++i){auto *p=&v[i*render::kSkinnedSourceStride];float normal[3]={0,0,1};float uv[2]={0.5f,0.5f};
  std::memcpy(p+render::kSkinnedNormalOffset,normal,sizeof(normal));std::memcpy(p+render::kSkinnedUvOffset,uv,sizeof(uv));p[render::kSkinnedBonesOffset+render::kSkinWeightByte]=face?255:0;}
 return v;
}
DrawSphereSource source(const std::vector<uint8_t> &v){return {{v.data(),render::kSkinnedSourceStride,static_cast<uint32_t>(v.size()/render::kSkinnedSourceStride)},0};}
int main() {
 Lighting lighting;shim::TexStore atlas;atlas.skin_pixels=pixels();auto v=vertices(3,false);auto draw=source(v);
 expect(lighting.skin_tone(atlas,draw)[0]<0,"first torso-only range remains fail-closed");
 for(int i=0;i<3;++i)v[i*render::kSkinnedSourceStride+render::kSkinnedBonesOffset+render::kSkinWeightByte]=255;
 const auto recovered=lighting.skin_tone(atlas,draw);
 expect(recovered[0]>0&&atlas.skin_tone->valid,"later face range recovers same-atlas reference");
 const int reads=draw.reads;lighting.skin_tone(atlas,draw);
 expect(draw.reads==reads&&atlas.skin_samples.empty(),"valid reference cached without rescanning and partial samples released");
 shim::TexStore partial;partial.skin_pixels=pixels();auto one=vertices(1,true);auto small=source(one);
 expect(lighting.skin_tone(partial,small)[0]<0&&lighting.skin_tone(partial,small)[0]<0,"undersampled partial references remain fail-closed");
 expect(lighting.skin_tone(partial,small)[0]>0,"bounded partial face ranges accumulate enough actual samples");
 shim::TexStore large;large.skin_pixels=pixels();auto many=vertices(1100,false);
 for(int i=1097;i<1100;++i)many[i*render::kSkinnedSourceStride+render::kSkinnedBonesOffset+render::kSkinWeightByte]=255;
 auto sparse=source(many);expect(lighting.skin_tone(large,sparse)[0]<0,"large range first probe is bounded");
 expect(lighting.skin_tone(large,sparse)[0]>0,"rotating bounded probe reaches later face vertices");
 shim::TexStore invalid;invalid.skin_pixels=pixels(120,120,120);auto all=vertices(1024,true);auto all_draw=source(all);
 for(int i=0;i<4;++i){expect(lighting.skin_tone(invalid,all_draw)[0]<0,"unusable reference never unmasks torso");expect(invalid.skin_samples.size()<=4096*3,"partial reference memory bounded");}
 auto fresh=vertices(3,true);auto face=source(fresh);shim::BaseTexture texture{&atlas};effects::next_pixels=pixels(120,90,60);
 atlas.skin_vertex_cursor=13;atlas.skin_samples.assign(9,100);
 SetEffectsTextureSource(&texture,"characters/other",{});
 expect(!atlas.skin_tone&& !atlas.skin_tone_read&&atlas.skin_vertex_cursor==0&&atlas.skin_samples.empty(),"new material generation resets reference and partial probe state");
 const auto changed=lighting.skin_tone(atlas,face);expect(changed[0]>0&&std::abs(changed[0]-recovered[0])>0.01f,"new atlas generation uses its own face colour");
 SetEffectsTextureSource(&texture,"locations/not-character",{});
 expect(!atlas.skin_pixels&&!atlas.skin_tone&&lighting.skin_tone(atlas,face)[0]<0,"non-character source reuse clears stale skin pixels and reference");
 effects::next_pixels.reset();SetEffectsTextureSource(&texture,"characters/unreadable",{});
 expect(lighting.skin_tone(atlas,face)[0]<0,"unreadable replacement remains fail-closed");
 effects::next_pixels=pixels();SetEffectsTextureSource(&texture,"characters/readable",{});
 expect(lighting.skin_tone(atlas,face)[0]>0,"later readable generation recovers without texture object recreation");
 std::puts("PASS production partial-draw skin reference and source lifecycle");
}
