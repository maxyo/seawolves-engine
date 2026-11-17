#include "storm/fx/skin_tone.hpp"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
using namespace storm;
void expect(bool v,const char *s){if(!v){std::fprintf(stderr,"FAIL %s\n",s);std::exit(1);}std::puts(s);}
namespace bgfx {struct TextureHandle{int idx=-1;};bool isValid(TextureHandle h){return h.idx>=0;}void setTexture(uint8_t,int,TextureHandle,uint32_t){}}
enum class TextureSource {Own,Upscale}; // shim texture_fade.hpp
namespace effects {struct UpscaleEntry {enum State {Resident=6};};}
struct Upscale {uint32_t width=512,height=256;int state=0;bgfx::TextureHandle handle{};};
struct Store {uint32_t width=512,height=256,levels=10;bool full_chain=false;bgfx::TextureHandle handle{1};std::shared_ptr<Upscale> upscale=std::make_shared<Upscale>();};
struct Draw {std::array<float,8> skin_tone{};};
struct Upscales {bool enabled=false,resident=false;bool active(){return enabled;}bgfx::TextureHandle replacement(Store &,float){return {resident?2:-1};}};
struct Context {static Context &instance(){static Context c;return c;}int sampler(int){return 0;}};
void bind(Store *store,Draw &fx_draw,Upscales &upscales,uint32_t stage=0) {
 (void)fx_draw;uint32_t unit=stage,flags=0;auto draw_distance=[](){return 1.0f;};
PRODUCTION_BINDING
 (void)source; // the texture fade's input (Device::draw), not this check's
}
Draw reference(){Draw d;d.skin_tone={0.47f,0.32f,0.2f,0.7f,1,0,1,3};return d;}
int main(){
 Store store;Draw draw=reference();Upscales up;
 bind(&store,draw,up);expect(draw.skin_tone[7]==3.0f,"source512 binds64px skin test atLOD3");
 up.enabled=up.resident=true;draw=reference();bind(&store,draw,up);
 expect(draw.skin_tone[7]==4.0f&&1024.0f/std::exp2(draw.skin_tone[7])==64.0f,"residentx2 binds same64px mask level atLOD4");
 up.enabled=false;draw=reference();bind(&store,draw,up);expect(draw.skin_tone[7]==3.0f,"return fromx2 tosource restoresLOD3 without drift");
 up.enabled=true;up.resident=false;draw=reference();bind(&store,draw,up);expect(draw.skin_tone[7]==3.0f,"pending nonresident upscale keeps sourceLOD");
 up.resident=true;draw=reference();draw.skin_tone[7]=7;bind(&store,draw,up,1);expect(draw.skin_tone[7]==7,"other texture stages cannot alter character maskLOD");
 store.levels=1;up.enabled=false;draw=reference();bind(&store,draw,up);
 expect(draw.skin_tone[0]<0,"single-mip source missing64px level conservatively suppresses torso relief");
 store.full_chain=true;draw=reference();bind(&store,draw,up);expect(draw.skin_tone[0]>0&&draw.skin_tone[7]==3,"rebuilt source fullchain permits coarse skin test");
 store.full_chain=false;up.enabled=true;draw=reference();bind(&store,draw,up);expect(draw.skin_tone[0]>0&&draw.skin_tone[7]==4,"resident fullchain upscale recovers from single-mip source");
 draw.skin_tone=fx::PackSkinTone({});bind(&store,draw,up);expect(draw.skin_tone[0]<0,"unknown skin reference remains fail-closed under upscale");
 draw.skin_tone={};bind(&store,draw,up);expect(draw.skin_tone[0]==0&&draw.skin_tone[7]==0,"explicit disabled mask remains disabled");
 auto small=reference();fx::SetSkinToneSampling(small.skin_tone,64,32,1);expect(small.skin_tone[0]>0&&small.skin_tone[7]==0,"already-coarse singlelevel texture is usable");
 std::puts("PASS actual albedo binding skin sampling");
}
