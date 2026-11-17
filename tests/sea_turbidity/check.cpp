#include <algorithm>
#include <cmath>
#include <iostream>
#include "storm/fx/settings.hpp"
using std::abs;using std::exp;
float max(float a,double b){return std::max(a,float(b));}
float min(float a,double b){return std::min(a,float(b));}
struct vec3 {float x,y,z;};
float dot(vec3 a,vec3 b){return a.x*b.x+a.y*b.y+a.z*b.z;}
vec3 normalize(vec3 a){float n=std::sqrt(dot(a,a));return {a.x/n,a.y/n,a.z/n};}
#include "../../src/libs/renderer_bgfx/shaders/include/sea_optics.sh"
int main(){
 int failed=0;auto check=[&](bool v,const char* why){if(!v){std::cerr<<"FAIL "<<why<<'\n';++failed;}};
 const float k=1.f/12;
 auto transmission=[](float gap,float ray,float extinction){
#ifdef LEGACY_TRANSMISSION
  return 1.f;
#else
  return seaOpticalTransmission(gap,ray,extinction);
#endif
 };
 check(transmission(0,1,k)==1,"waterline exact unchanged");
 check(transmission(-.3f,.3f,k)==1,"foreground/MSAA negative gap unchanged");
 check(transmission(100,1,0)==1,"disabled/capability fallback unchanged");
 check(transmission(.1f,1,k)>.99f,"shallow water retains bottom");
 check(transmission(30,1,k)<.09f,"deep water hides bottom/hull");
 check(transmission(12,1,k)<transmission(2,1,k),"progressive optical depth");
 check(transmission(2,.5f,k)<transmission(2,1,k),"longer oblique ray attenuates more");
 check(std::isfinite(transmission(1,0,k)),"grazing finite");
 auto viewCos=[](vec3 ray,vec3 forward){
#ifdef LEGACY_CAMERA
  // The previous shader used built-in u_view, which a port draw leaves identity/stale.
  return ray.z;
#else
  return seaViewRayCos(ray,forward);
#endif
 };
 float forward=viewCos({0,0,-1},{0,0,1});
 float yaw90=viewCos({-1,0,0},{1,0,0});
 check(std::abs(transmission(12,forward,k)-transmission(12,yaw90,k))<1e-6,
       "center ray depth invariant under camera yaw90");
 float pitch=viewCos({0,.6f,-.8f},{0,-.6f,.8f});
 check(std::abs(transmission(12,forward,k)-transmission(12,pitch,k))<1e-6,
       "center ray depth invariant under camera pitch");
 const float baseT=.7f,tint=.2f,reflection=.1f;
 for(float gap:{0.f,.1f,1.f,12.f,100.f}){
  float t=transmission(gap,1,k),alpha=baseT*t,source=reflection+tint*baseT*(1-t);
  check(std::abs(source+alpha*tint-(reflection+tint*baseT))<1e-6,"same-colour background conserves radiance");
  check(source>=reflection&&alpha<=baseT,"reflection untouched, transmission only decreases");
 }
 auto medium=storm::fx::Settings::FromPreset(storm::fx::QualityPreset::Medium);
 auto high=storm::fx::Settings::FromPreset(storm::fx::QualityPreset::High);
 check(medium.sea_turbidity&&high.sea_turbidity&&!medium.sea_soft_edges,"Medium independently enables optical depth");
 check(!storm::fx::Settings::FromPreset(storm::fx::QualityPreset::Low).sea_turbidity,"Low remains original");
 storm::fx::Support support{};support.scene_depth=true;support.msaa_samples=4;support.msaa_depth=true;
 for(auto setting:{medium,high}){
  std::vector<std::string> changes;
  check(storm::fx::ConstrainSettings(setting,support,changes).sea_turbidity,"supported Medium/High retains turbidity");
  auto noDepth=support;noDepth.scene_depth=false;
  check(!storm::fx::ConstrainSettings(setting,noDepth,changes).sea_turbidity,"missing depth explicit fallback");
  auto noResolve=support;noResolve.msaa_depth=false;setting.msaa=4;
  check(storm::fx::ConstrainSettings(setting,noResolve,changes).sea_turbidity,"MSAA without depth resolve keeps turbidity: the sea reads its single-sample backdrop");
  setting.msaa=1;check(storm::fx::ConstrainSettings(setting,noResolve,changes).sea_turbidity,"single sample supported without MSAA resolve");
 }
 if(!failed)std::cout<<"PASS optical transmission/composition and actual Medium/High capability policy\n";
 return failed?1:0;
}
