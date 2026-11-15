// Production Host::prepare_sea extracted unchanged by run.py; GPU submissions are recording stand-ins.
#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include "storm/fx/settings.hpp"
namespace fx=storm::fx;
namespace bgfx {
struct UniformHandle {int idx=-1;};struct TextureHandle{int idx=-1;};using ViewId=int;
namespace UniformType {enum Enum{Vec4,Sampler};}
template<class T>bool isValid(T h){return h.idx>=0;}
UniformHandle createUniform(const char*,UniformType::Enum,int=1){return {1};}
std::array<float,16> submitted{};int binds=0;
void setUniform(UniformHandle,const float* data,int count){std::copy(data,data+count*4,submitted.begin());}
void setTexture(int,UniformHandle,TextureHandle,int){++binds;}
}
constexpr int BGFX_SAMPLER_UVW_CLAMP=1,BGFX_SAMPLER_POINT=2;
namespace shim {
struct TexStore{int target_flags=0,width=1280,height=720;bgfx::TextureHandle handle{7};void sync(){}};
struct Surface{TexStore* value;struct Store{TexStore* value;TexStore* get(){return value;}}store;explicit Surface(TexStore* p):value(p),store{p}{}};
struct Device{struct State{float proj[16]{};float vs_const[96][4]{};}s;Surface* rt=nullptr;Surface* ds=nullptr;int views=0;int effects_view(bool){return ++views;}};
struct Context{uint32_t frame=1;static Context& instance(){static Context c;return c;}uint32_t submitted_frames(){return frame;}};
void logf(const char*){}
}
void clip_planes(const float*,float& near,float& far){near=.1f;far=4000;}
struct CaptureRecorder {
 bool fail=false;int captures=0;bgfx::TextureHandle texture{-1};
 bool Capture(int,bgfx::TextureHandle source,uint16_t,uint16_t,float,float){++captures;if(fail||!bgfx::isValid(source))return false;texture={20+captures};return true;}
 bgfx::TextureHandle Texture(){return texture;}
};
struct Host {
 bgfx::UniformHandle u_sea_fx_,s_sea_depth_;bool device_ready_=true,sea_depth_logged_=false;
 fx::Settings settings_;fx::Support support_;uint32_t sea_depth_frame_=UINT32_MAX;const void* sea_depth_target_=nullptr;
 CaptureRecorder sea_depth_;struct Weather{std::array<float,3> sun_dir{},sun_color{};}weather_;
 void advance_sea_waves(shim::Device&){}void set_sea_waves_uniforms(){}bool has_weather(){return false;}
 void prepare_sea(shim::Device&);
};
#include "actual_prepare_sea.hpp"
int main(){
 int fails=0;auto check=[&](bool pass,const char* why){if(!pass){std::cerr<<"FAIL "<<why<<'\n';++fails;}};
 Host host;host.settings_.sea_turbidity=true;host.support_.msaa_depth=true;
 shim::TexStore color,depth,color2;shim::Surface target(&color),ds(&depth),other(&color2);shim::Device d;d.rt=&target;d.ds=&ds;d.s.vs_const[29][0]=.2f;
 host.prepare_sea(d);
 check(host.sea_depth_.captures==1&&bgfx::binds==1&&bgfx::submitted[13]==0&&bgfx::submitted[12]==.2f&&bgfx::submitted[15]>0,"Medium captures with softedges disabled, actual watercolour and extinction bound");
 host.prepare_sea(d);check(host.sea_depth_.captures==1,"same target/frame capture reused");
 d.rt=&other;host.prepare_sea(d);check(host.sea_depth_.captures==2,"RT switch recaptures current target");
 d.rt=&target;host.prepare_sea(d);check(host.sea_depth_.captures==3,"return from reflection target recaptures main");
 ++shim::Context::instance().frame;host.sea_depth_.fail=true;const int binds=bgfx::binds;host.prepare_sea(d);
 check(bgfx::submitted[15]==0&&bgfx::binds==binds&&host.sea_depth_frame_==UINT32_MAX,"failed capture cannot sample older texture");
 host.sea_depth_.fail=false;host.prepare_sea(d);check(bgfx::submitted[15]>0,"failed capture retries same frame");
 ++shim::Context::instance().frame;d.ds=nullptr;host.prepare_sea(d);check(bgfx::submitted[15]==0,"missing attachment disables optical read");
 d.ds=&ds;color.target_flags=1;host.support_.msaa_depth=false;host.prepare_sea(d);check(bgfx::submitted[15]==0,"unreadable MSAA target disabled");
 color.target_flags=0;host.settings_.sea_turbidity=false;host.settings_.sea_soft_edges=true;host.prepare_sea(d);
 check(bgfx::submitted[1]==1&&bgfx::submitted[15]==0,"softedges independently work without turbidity");
 if(!fails)std::cout<<"PASS actual prepare_sea target/frame/failure/Medium controls\n";
 return fails?1:0;
}
