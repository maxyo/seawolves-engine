// Executes actual SubmitScreenTriangle and SeaDepth::Capture with recording bgfx calls.
#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
struct ScreenVertex {float pos[3],data[3];};
namespace bgfx {
struct ProgramHandle{int idx=-1;};struct TextureHandle{int idx=-1;};struct FrameBufferHandle{int idx=-1;};struct UniformHandle{int idx=-1;};using ViewId=int;
namespace Attrib{enum Enum{Position,TexCoord0};}namespace AttribType{enum Enum{Float};}
namespace UniformType{enum Enum{Sampler,Vec4};}namespace TextureFormat{enum Enum{RGBA8};}
struct VertexLayout{VertexLayout& begin(){return *this;}VertexLayout& add(Attrib::Enum,int,AttribType::Enum){return *this;}void end(){}};
struct TransientVertexBuffer{void* data;};int available=3,submitted=0;
struct Caps{bool originBottomLeft=false;};const Caps* getCaps(){static Caps c;return &c;}
template<class T>bool isValid(T h){return h.idx>=0;}
uint32_t getAvailTransientVertexBuffer(int,const VertexLayout&){return available;}
void allocTransientVertexBuffer(TransientVertexBuffer* b,int,const VertexLayout&){static ScreenVertex vertices[3];b->data=vertices;}
void setVertexBuffer(int,TransientVertexBuffer*){}void setState(uint64_t){}void submit(int,ProgramHandle){++submitted;}
FrameBufferHandle createFrameBuffer(uint16_t,uint16_t,TextureFormat::Enum,int){return {8};}
UniformHandle createUniform(const char*,UniformType::Enum){return {9};}
void setUniform(UniformHandle,const float*){}void setTexture(int,UniformHandle,TextureHandle,int){}
}
constexpr int BGFX_TEXTURE_RT=1,BGFX_STATE_WRITE_RGB=2,BGFX_STATE_WRITE_A=4,kClampPoint=8;
template<class T>void Destroy(T& h){h.idx=-1;}
bgfx::ProgramHandle LoadProgram(const char*,const char*){return {10};}
void SetupView(int,const char*,bgfx::FrameBufferHandle,uint16_t,uint16_t){}
bool SubmitScreenTriangle(bgfx::ViewId,bgfx::ProgramHandle,uint64_t,bool=false,const std::array<std::array<float,3>,3>* = nullptr);
class SeaDepth {
 bgfx::FrameBufferHandle target_;bgfx::ProgramHandle program_;bgfx::UniformHandle s_depth_,u_capture_;uint16_t width_=0,height_=0;
 public:bool Capture(bgfx::ViewId,bgfx::TextureHandle,uint16_t,uint16_t,float,float);
};
#include "actual_capture.hpp"
int main(){
 SeaDepth depth;int fails=0;auto check=[&](bool pass,const char* why){if(!pass){std::cerr<<"FAIL "<<why<<'\n';++fails;}};
 check(depth.Capture(1,{1},1280,720,.1f,4000)&&bgfx::submitted==1,"warm depth capture actually submitted");
 bgfx::available=0;
 check(!depth.Capture(2,{1},1280,720,.1f,4000)&&bgfx::submitted==1,"exhausted transient cannot report fresh capture over old texture");
 bgfx::available=3;
 check(depth.Capture(3,{1},1280,720,.1f,4000)&&bgfx::submitted==2,"capacity recovery resubmits capture");
 check(!depth.Capture(4,{-1},1280,720,.1f,4000),"invalid depth source fails closed");
 if(!fails)std::cout<<"PASS actual transient submission and SeaDepth capture-success contract\n";
 return fails?1:0;
}
