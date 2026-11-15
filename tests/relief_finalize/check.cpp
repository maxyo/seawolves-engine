// Linked against actual candidate Finalizer and actual fx + EAC/bimg functions.
// The baseline function body is extracted verbatim from pre-split PrepareFile's tail.
#include "finalizer.hpp"
#include "backend.hpp"
#include "eac_encoder.hpp"
#include "cache.hpp"
#include "storm/fx/cpu_admission.hpp"
#include "storm/fx/relief_model.hpp"
#include "storm/fx/height_map.hpp"
#include "storm/fx/gloss_map.hpp"
#include "storm/fx/relief_store.hpp"
#include "storm/fx/upscale.hpp"
#include <future>
#include <map>
#include <stdexcept>
#include <cstdio>
#include <cmath>
using namespace storm;
using namespace storm::relief;
using Clock=std::chrono::steady_clock;
namespace {std::map<uint64_t,fx::NormalMap> writes;std::map<uint64_t,std::vector<uint8_t>> displays;bool fail_gloss=false;bool parallel_case=false;
void expect(bool ok,const char*why){if(!ok)throw std::runtime_error(why);}
bool same(const fx::NormalMap&a,const fx::NormalMap&b){return a.width==b.width&&a.height==b.height&&a.mip_count==b.mip_count&&a.format==b.format&&a.rg==b.rg&&a.material==b.material&&a.depth==b.depth&&a.trust==b.trust&&a.trust_coverage==b.trust_coverage;}
}
namespace storm::fx {
CpuAdmission&SharedCpuAdmission(){static CpuAdmission serial(1),parallel(4);return parallel_case?parallel:serial;}
bool WriteReliefMap(const std::filesystem::path&,uint64_t key,const NormalMap&map){writes[key]=map;return true;}
bool WriteGlossMap(const std::filesystem::path&,uint64_t key,const NormalMap&map){if(fail_gloss)return false;writes[key]=map;return true;}
std::filesystem::path ReliefFile(const std::filesystem::path&p,uint64_t){return p/"relief";}
std::filesystem::path GlossFile(const std::filesystem::path&p,uint64_t){return p/"gloss";}
bool WriteUpscaled(const std::filesystem::path&,uint64_t key,std::span<const uint8_t>b){displays[key]={b.begin(),b.end()};return true;}
std::filesystem::path UpscaleFile(const std::filesystem::path&p,uint64_t){return p/"x2";}
}
// EXTRACTED_HELPERS
Outcome old_tail(ComputedTexture &&p){
 struct OldImpl {std::atomic<bool>cancelled{false};int threads=1;std::unique_ptr<texture::EacEncoder>encoder;Session::Stats stats;};
 auto impl_=std::make_unique<OldImpl>();impl_->stats.provider="synthetic";impl_->threads=int(fx::SharedCpuAdmission().threads());
 struct Candidate{std::string name;fx::GlossClass gloss;};const auto candidate=std::make_optional(Candidate{p.name,p.gloss_class});
 auto rgba=std::move(p.rgba);auto normal_planes=std::move(p.normal_planes);auto height_planes=std::move(p.height_planes);auto gloss_height=std::move(p.gloss_height);auto roughness_planes=std::move(p.roughness_planes);
 int mw=p.mw,mh=p.mh;const int fw=p.fw,fh=p.fh,gw=p.gw,gh=p.gh,width=p.width,height=p.height,model_scale=p.model_scale;
 const auto key=p.key,gloss_key=p.gloss_key;const auto output=p.output;const bool height_class=p.height_class,gloss_wanted=p.gloss_wanted;
 const auto relief=p.relief;std::string failure;const std::string per_model;
 const double read_ms=0,decode_ms=0,upscale_ms=0,model_ms=0,infer_ms=0;const auto start=Clock::now();auto mark=start;
 const auto lap=[&]{auto now=Clock::now();double ms=std::chrono::duration<double,std::milli>(now-mark).count();mark=now;return ms;};
 // EXTRACTED_OLD_TAIL
}
std::unique_ptr<ComputedTexture> fixture(int kind){
 auto p=std::make_unique<ComputedTexture>();p->cancelled=std::make_shared<std::atomic<bool>>(false);p->name="synthetic stone";p->session=42;p->provider="synthetic";p->started=p->computed_at=Clock::now();
 p->width=p->height=32;p->mw=p->mh=kind==1?32:16;p->fw=p->fh=16;p->gw=p->gh=8;p->model_scale=kind==1?2:1;
 p->key=11;p->gloss_key=12;p->gloss_wanted=true;p->height_class=true;p->relief=fx::ReliefClass::Stone;p->gloss_class=fx::GlossClass::Stone;
 if(kind==4){p->width=p->height=128;p->mw=p->mh=p->fw=p->fh=64;p->gw=p->gh=32;}
 p->rgba.resize(size_t(p->width)*p->height*4);for(size_t i=0;i<p->rgba.size();++i)p->rgba[i]=uint8_t((i*37+11)%256);
 auto fill=[](std::vector<float>&v,size_t n,int seed){v.resize(n);for(size_t i=0;i<n;++i)v[i]=float((i*17+seed)%251)/250.f;};
 fill(p->normal_planes,size_t(p->mw)*p->mh*3,3);fill(p->height_planes,size_t(p->mw)*p->mh*3,9);fill(p->roughness_planes,size_t(p->gw)*p->gh*3,21);fill(p->gloss_height,size_t(p->gw)*p->gh,39);
 if(kind==2){p->key.reset();p->normal_planes.clear();p->height_planes.clear();p->height_class=false;}
 if(kind==3){p->gloss_key.reset();p->height_class=false;p->gloss_wanted=false;p->gloss_height.clear();p->roughness_planes.clear();}
 return p;
}
int main(){try{
 Finalizer actual;
 for(int kind=0;kind<5;++kind){parallel_case=kind==4;if(parallel_case){fx::SharedCpuAdmission().begin(1,true);fx::SharedCpuAdmission().opaque(1,true);}writes.clear();expect(old_tail(std::move(*fixture(kind)))==Outcome::Prepared,"OLD_TAIL_FAILED");const auto baseline=writes;writes.clear();
  auto r=actual.Run(fixture(kind));expect(r.outcome==Outcome::Prepared,"NEW_FINALIZER_FAILED");expect(writes.size()==baseline.size(),"ARTIFACT_COUNT_CHANGED");for(const auto&[key,map]:baseline)expect(writes.contains(key)&&same(map,writes[key]),"ACTUAL_EAC_MATERIAL_MIPS_CHANGED");if(parallel_case)expect(fx::SharedCpuAdmission().stats().reserved_peak==4&&fx::SharedCpuAdmission().stats().reserved==0,"ENCODER_TEAM_NOT_ADMITTED");}
 parallel_case=false;
 // Real single-core admission + gloss-only path: cancellation must release a queued finalizer, without releasing external permit.
 auto permit=fx::SharedCpuAdmission().acquire(1,[]{return false;});auto p=fixture(2);auto cancel=p->cancelled;
 auto waiting=std::async(std::launch::async,[&actual,p=std::move(p)]()mutable{return actual.Run(std::move(p));});
 std::this_thread::sleep_for(std::chrono::milliseconds(10));*cancel=true;
 expect(waiting.wait_for(std::chrono::seconds(1))==std::future_status::ready,"CAP1_CANCEL_DEADLOCK");expect(waiting.get().outcome==Outcome::Failed,"CANCELLED_WAS_PREPARED");permit.reset();
 writes.clear();fail_gloss=true;auto partial=actual.Run(fixture(0));expect(partial.outcome==Outcome::Failed&&partial.relief_written==11&&!partial.gloss_written&&writes.contains(11)&&!writes.contains(12),"PARTIAL_WRITE_LIED");fail_gloss=false;
 // The actual old WriteDisplay helper and current Upscale finalizer use identical real mip/format encoder.
 auto x=fixture(0);x->kind=ComputedTexture::Kind::Upscale;x->key=77;x->width=x->height=16;x->display={"unused",false};
 expect(WriteDisplay(x->display,*x->key,x->rgba,32,32,x->name),"OLD_DISPLAY_FAILED");const auto baseline=displays.at(77);displays.clear();expect(actual.Run(std::move(x)).outcome==Outcome::Prepared&&displays.at(77)==baseline,"DISPLAY_CODEC_CHANGED");
 expect(fx::SharedCpuAdmission().stats().reserved==0,"CPU_PERMIT_LEAK");std::puts("actual old-tail/new Finalizer fx+EAC bitexact, cap1 gloss/cancel, partial write, x2 codec PASS");return 0;
 }catch(const std::exception&e){std::fprintf(stderr,"%s\n",e.what());return 1;}}
