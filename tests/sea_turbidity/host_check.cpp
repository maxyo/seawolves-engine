// Production Host::prepare_sea extracted unchanged by run.py; GPU submissions are recording stand-ins.
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <iostream>
#include "storm/fx/settings.hpp"
namespace fx=storm::fx;
namespace bgfx {
struct UniformHandle {int idx=-1;};struct TextureHandle{int idx=-1;};using ViewId=int;
namespace UniformType {enum Enum{Vec4,Sampler};}
template<class T>bool isValid(T h){return h.idx>=0;}
UniformHandle createUniform(const char*,UniformType::Enum,int=1){return {1};}
std::array<float,20> submitted{};int binds=0,touches=0;
void setUniform(UniformHandle,const float* data,int count){std::copy(data,data+count*4,submitted.begin());}
void setTexture(int,UniformHandle,TextureHandle,int){++binds;}
void touch(ViewId){++touches;}
}
constexpr int BGFX_SAMPLER_UVW_CLAMP=1,BGFX_SAMPLER_POINT=2;
constexpr uint64_t BGFX_TEXTURE_RT_MSAA_MASK=0x70,BGFX_TEXTURE_RT_WRITE_ONLY=0x100;
namespace shim {
struct TexStore{uint64_t target_flags=0;int width=1280,height=720;bgfx::TextureHandle handle{7};void sync(){}};
struct Surface{TexStore* value;struct Store{TexStore* value;TexStore* get(){return value;}}store;explicit Surface(TexStore* p):value(p),store{p}{}};
struct Schedule{int writes=0;void write(int,int){++writes;}};
struct Device{struct State{struct Matrix{float m[4][4]{};}view;float proj[16]{};float vs_const[96][4]{};}s;Surface* rt=nullptr;Surface* ds=nullptr;Surface* back_color=nullptr;bool scene_done=false;
 TexStore* view_depth=nullptr;TexStore* substitute=nullptr;int views=0;Schedule schedule;int reads=0,target_views=0,last_read=-1;
 int ensure_view(){view_depth=ds?(substitute?substitute:ds->store.get()):nullptr;return views;}int effects_view(bool){ensure_view();return ++views;}
 int effects_target_view(const char*,std::initializer_list<int>,std::initializer_list<int> r){++target_views;for(int t:r)last_read=t;return ++views;}
 void note_read(bgfx::TextureHandle){++reads;}};
struct Context{uint32_t frame=1;uint64_t index=1;static Context& instance(){static Context c;return c;}uint32_t submitted_frames(){return frame;}uint64_t frame_index(){return index;}};
void logf(const char*){}
}
void clip_planes(const float*,float& near,float& far){near=.1f;far=4000;}
std::array<float,16> backend_projection(const shim::Device&,const float*){return {};}
struct CaptureRecorder {
 bool fail=false;int captures=0;int last_source=-1;bgfx::TextureHandle texture{-1};
 bool Capture(int,bgfx::TextureHandle source,uint16_t,uint16_t,float,float){++captures;last_source=source.idx;if(fail||!bgfx::isValid(source))return false;texture={20+captures};return true;}
 bgfx::TextureHandle Texture(){return texture;}
};
struct Waves{bgfx::TextureHandle Surface(){return {};}};struct Wakes{bgfx::TextureHandle Target(){return {};}};
struct Host {
 bgfx::UniformHandle u_sea_fx_,s_sea_depth_;bool device_ready_=true,sea_depth_logged_=false;
 fx::Settings settings_;fx::Support support_;uint32_t sea_depth_frame_=UINT32_MAX;const void* sea_depth_target_=nullptr;const void* sea_depth_source_=nullptr;
 CaptureRecorder sea_depth_;Waves sea_waves_;Wakes sea_wakes_;struct Weather{std::array<float,3> sun_dir{},sun_color{};}weather_;
 // The backdrop (sea_backdrop): drawn this frame when `armed` and a draw (or the sea's first draw) opened its view.
 bool armed=false,drawn=false;uint32_t sea_backdrop_frame_=UINT32_MAX;bgfx::TextureHandle sea_backdrop_depth_{40};
 uint16_t sea_backdrop_width_=1000,sea_backdrop_height_=500;int sea_backdrop_view_=90,opened=0;uint64_t sea_drawn_frame_=0;bool sea_drawn_=false;
 // As production: the picture before the boundary, after it while its depth is write-only (picture_write_only).
 bool picture_write_only=false;
 bool sea_backdrop_target(const shim::Device&d) const{return d.rt==d.back_color&&(!d.scene_done||picture_write_only);}
 bool sea_backdrop_armed(const shim::Device&) const{return armed;}
 bool open_sea_backdrop_view(shim::Device&,const std::array<float,32>&){++opened;sea_backdrop_frame_=shim::Context::instance().submitted_frames();return true;}
 void advance_sea_waves(shim::Device&){}void set_sea_waves_uniforms(){}bool has_weather(){return false;}
 void prepare_sea(shim::Device&);
};
#include "actual_prepare_sea.hpp"
int main(){
 int fails=0;auto check=[&](bool pass,const char* why){if(!pass){std::cerr<<"FAIL "<<why<<'\n';++fails;}};
 Host host;host.settings_.sea_turbidity=true;host.support_.msaa_depth=true;
 shim::TexStore color,depth,color2;shim::Surface target(&color),ds(&depth),other(&color2);shim::Device d;d.rt=&target;d.ds=&ds;d.s.vs_const[29][0]=.2f;d.s.view.m[0][2]=1;
 // Another target than the scene (back_color is none here): its own depth is captured in place.
 host.prepare_sea(d);
 check(host.sea_depth_.captures==1&&bgfx::binds==1&&bgfx::submitted[13]==0&&bgfx::submitted[12]==.2f&&bgfx::submitted[15]>0,"Medium captures with softedges disabled, actual watercolour and extinction bound");
 check(bgfx::submitted[16]==1&&bgfx::submitted[18]==0,"private actual shim forward uniform at yaw90");
 host.prepare_sea(d);check(host.sea_depth_.captures==1,"same target/frame capture reused");
 d.rt=&other;host.prepare_sea(d);check(host.sea_depth_.captures==2,"RT switch recaptures current target");
 d.rt=&target;host.prepare_sea(d);check(host.sea_depth_.captures==3,"return from reflection target recaptures main");
 shim::TexStore substituted;substituted.handle={99};d.substitute=&substituted;
 host.prepare_sea(d);check(host.sea_depth_.last_source==99,"actual substituted depth sampled instead of nominal ds");
 d.substitute=nullptr;
 ++shim::Context::instance().frame;host.sea_depth_.fail=true;const int binds=bgfx::binds;host.prepare_sea(d);
 check(bgfx::submitted[15]==0&&bgfx::binds==binds&&host.sea_depth_frame_==UINT32_MAX,"failed capture cannot sample older texture");
 host.sea_depth_.fail=false;host.prepare_sea(d);check(bgfx::submitted[15]>0,"failed capture retries same frame");
 ++shim::Context::instance().frame;d.ds=nullptr;host.prepare_sea(d);check(bgfx::submitted[15]==0,"missing attachment disables optical read");
 d.ds=&ds;depth.target_flags=0x10;host.support_.msaa_depth=false;host.prepare_sea(d);check(bgfx::submitted[15]==0,"unreadable MSAA target disabled");
 host.support_.msaa_depth=true;depth.target_flags=0x10|BGFX_TEXTURE_RT_WRITE_ONLY;host.prepare_sea(d);check(bgfx::submitted[15]==0,"write-only depth disabled");
 depth.target_flags=0;host.settings_.sea_turbidity=false;host.settings_.sea_soft_edges=true;host.prepare_sea(d);
 check(bgfx::submitted[1]==1&&bgfx::submitted[15]==0,"softedges independently work without turbidity");
 // The scene: never its own depth; the backdrop of this frame, captured in a target view that reads it.
 host.settings_.sea_turbidity=true;host.settings_.sea_soft_edges=false;d.back_color=&target;d.rt=&target;
 ++shim::Context::instance().frame;int captures=host.sea_depth_.captures;
 host.prepare_sea(d);
 check(host.sea_depth_.captures==captures&&bgfx::submitted[15]==0&&host.opened==0,"first frame of a sea: no backdrop, hard edges, no scene depth read");
 check(host.sea_drawn_,"the sea in the scene arms the next frames' backdrop");
 ++shim::Context::instance().frame;host.armed=true;host.sea_backdrop_frame_=shim::Context::instance().frame; // a draw before the sea opened it
 host.prepare_sea(d);
 check(host.sea_depth_.captures==captures+1&&host.sea_depth_.last_source==40&&d.last_read==40&&host.opened==0&&bgfx::submitted[15]>0,"armed frame: the backdrop is captured and read");
 check(d.schedule.writes>0&&d.reads>0,"the capture writes the sea depth, the sea's view reads it (view order)");
 host.prepare_sea(d);check(host.sea_depth_.captures==captures+1,"one backdrop capture per frame");
 ++shim::Context::instance().frame;host.prepare_sea(d);
 check(host.opened==1&&bgfx::touches==1&&host.sea_depth_.captures==captures+2&&bgfx::submitted[15]>0,"armed frame without draws before the sea: the cleared backdrop");
 d.scene_done=true;++shim::Context::instance().frame;captures=host.sea_depth_.captures;host.prepare_sea(d);
 check(host.sea_depth_.captures==captures+1&&host.sea_depth_.last_source==7,"after the scene boundary the picture is another target: captured in place");
 host.picture_write_only=true;++shim::Context::instance().frame;captures=host.sea_depth_.captures;host.prepare_sea(d);
 check(host.sea_depth_.captures==captures+1&&host.sea_depth_.last_source==40&&bgfx::submitted[15]>0,"after the boundary over the write-only scene depth (the title's sea): the backdrop");
 if(!fails)std::cout<<"PASS actual prepare_sea target/frame/failure/Medium/backdrop controls\n";
 return fails?1:0;
}
