// Real Session::Upscale and the real x4-to-x2 pipeline/cache writer. Only device/model inference is deterministic.
#include "relief.hpp"
#include "backend.hpp"
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <unistd.h>
namespace {
int cpu_runs=0,gpu_runs=0,cpu_released=0;
bool slow_gpu=false, fail_gpu_load=false, start_gpu=false;
void expect(bool v,const char*s) {if(!v){std::fprintf(stderr,"FAIL: %s\n",s);std::exit(1);}std::printf("ok: %s\n",s);}
class Model final: public storm::relief::Network {
 bool gpu_;int scale_;
 public:Model(bool gpu,int scale):gpu_(gpu),scale_(scale){}
 bool Infer(const float*,int w,int h,float*out,std::string&) override {
  if(gpu_){++gpu_runs;if(slow_gpu)std::this_thread::sleep_for(std::chrono::milliseconds(25));}
  else {++cpu_runs;std::this_thread::sleep_for(std::chrono::milliseconds(3));}
  std::fill(out,out+size_t(w)*h*3*scale_*scale_,.5f);return true;
 }
 void Cancel() override {}
 std::string Describe() const override {return gpu_?"GPU deterministic":"CPU deterministic";}
};
class Runtime final: public storm::relief::CpuRuntime {
 public:~Runtime() override {++cpu_released;}
 std::unique_ptr<storm::relief::Network> Load(std::span<const uint8_t>,int,int scale,std::string&) override {return std::make_unique<Model>(false,scale);}
};
void write_tx(const std::filesystem::path &p) {
 constexpr int size=64;const int32_t head[]={0,size,size,7,21,size*size*4};
 std::ofstream out(p,std::ios::binary);out.write(reinterpret_cast<const char*>(head),sizeof(head));
 for(int s=size;s;s/=2)for(int i=0;i<s*s;++i){const uint8_t rgba[]={70,100,180,255};out.write(reinterpret_cast<const char*>(rgba),4);}
}
}
namespace storm::relief {
std::unique_ptr<CpuRuntime> CpuRuntime::Open(int,std::string&,int) {return std::make_unique<Runtime>();}
const GpuDevice &Gpu(){static GpuDevice gpu{true,"deterministic","1",{}};return gpu;}
bool ReleaseGpu(){return false;}
std::unique_ptr<Network> LoadGpuNetwork(std::span<const uint8_t>,std::span<const uint8_t>,int,int scale,std::string &error) {if(fail_gpu_load){error="synthetic controlled GPU initialization failure";return nullptr;}return std::make_unique<Model>(true,scale);}
const char *DeviceName(Device d){return d==Device::Gpu?"GPU":"CPU";}
Device ChooseDevice(const std::filesystem::path&,std::span<const uint8_t>,std::span<const uint8_t>,std::span<const uint8_t>,int,int,double &cost){cost=1.;return start_gpu?Device::Gpu:Device::Cpu;}
void MarkGpuRunning(const std::filesystem::path&,const std::string&){}
void ClearGpuRunning(const std::filesystem::path&){}
std::string GpuCrash(const std::filesystem::path&){return {};}
std::string GpuLabel(const GpuDevice&){return "deterministic";}
// Kept so the same negative-control fixture links against the pre-situational production implementation.
void DemoteGpu(const std::filesystem::path&,const std::string&){}
}
int main(int argc,char**argv) {
 using namespace storm::relief;
 expect(argc==2,"isolated fixture root supplied");std::filesystem::path root=std::filesystem::path(argv[1])/("run-"+std::to_string(getpid())+"-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())), textures=root/"Textures";
 std::filesystem::create_directories(textures/"Characters");
 const uint8_t m[]={1};Models models{m,m,m,m,m,m,m,m,m,m,m,m};std::string error;
 auto run=[&](Session &session,int i,const char *group){auto p=textures/"Characters"/(std::string("Face-")+group+std::to_string(i)+".tga.tx");write_tx(p);return session.Upscale(textures,p,{root/group,true},64,error);};
 auto session=Session::Open(models,1,root,error);expect(bool(session),"real production Session opens CPU with GPU available");
 for(int i=0;i<8;++i)expect(run(*session,i,"fast")==Outcome::Prepared,"real CPU x2 prepared and stored");
 expect(cpu_runs==8 && gpu_runs==0,"eight x2-only requests ran on CPU");
 expect(!session->WaitsForGpu(),"eight CPU completions open background admission");
 expect(run(*session,8,"fast")==Outcome::Prepared,"ninth x2-only request succeeds on retry");
 expect(gpu_runs==1 && session->stats().gpu,"x2-only workload actually returns to GPU");
 expect(cpu_released==1,"returning GPU releases CPU runtime");
 session.reset();slow_gpu=true;
 session=Session::Open(models,1,root,error);
 for(int i=0;i<8;++i)expect(run(*session,i,"slow")==Outcome::Prepared,"slow-control CPU x2 prepared");
 expect(run(*session,8,"slow")==Outcome::Prepared,"slow GPU retry still publishes texture");
 expect(!session->stats().gpu,"slower actual x4 GPU retry keeps CPU");
 expect(session->WaitsForGpu(),"failed speed comparison restarts bounded CPU detour");
 expect(run(*session,9,"slow")==Outcome::Prepared,"CPU continues after rejected GPU retry");
 session->SetGpuFree(true);slow_gpu=false;
 expect(run(*session,10,"slow")==Outcome::Prepared && session->stats().gpu,"quiet renderer retries and returns to GPU immediately");
 session.reset();fail_gpu_load=true;start_gpu=true;
 session=Session::Open(models,1,root,error);
 expect(bool(session) && !session->stats().gpu && !session->stats().gpu_possible,"controlled first GPU model error opens CPU session");
 expect(run(*session,0,"load-failure")==Outcome::Prepared,"first texture survives controlled GPU initialization error");
 expect(!session->WaitsForGpu(),"known failed GPU does not starve CPU background work");
 std::puts("production Session x2 CPU/GPU return: PASS");
}
