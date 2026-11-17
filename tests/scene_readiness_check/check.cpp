#include "effects/scene_readiness.hpp"
#include "storm/render/loading_budget.hpp"
#include <cassert>
#include <cstdio>
using storm::effects::SceneReadiness;
using storm::effects::SceneResources;
int main() {
 storm::render::LoadingOpaque opaque;
 opaque.arm(1,true,true,255);opaque.frame(1,true,true);assert(!opaque.presented(1)); // skipped/failed draw
 opaque.arm(1,true,true,255);opaque.submitted(1);assert(!opaque.presented(1)); // submit not present yet
 opaque.arm(1,false,false,0);opaque.frame(1,true,true);assert(opaque.presented(1)); // disarm keeps successful receipt
 opaque.frame(1,true,true);assert(!opaque.presented(1)); // no stale receipt next frame
 opaque.arm(1,true,true,255);opaque.submitted(2);opaque.frame(2,true,true);assert(!opaque.presented(2)); // stale arm
 opaque.arm(1,true,true,255);opaque.submitted(1);opaque.frame(2,true,true);assert(!opaque.presented(2)); // stale submitted
 opaque.arm(2,false,true,255);opaque.submitted(2);opaque.frame(2,true,true);assert(!opaque.presented(2)); // partial rectangle
 opaque.arm(2,true,true,254);opaque.submitted(2);opaque.frame(2,true,true);assert(!opaque.presented(2)); // transparent
 opaque.arm(2,true,false,255);opaque.submitted(2);opaque.frame(2,true,true);assert(!opaque.presented(2)); // texture alpha unknown
 opaque.arm(2,true,true,255);opaque.submitted(2);opaque.invalidate();opaque.frame(2,true,true);assert(!opaque.presented(2));
 opaque.arm(2,true,true,255);opaque.submitted(2);opaque.frame(2,false,true);assert(!opaque.presented(2)); // failed/no-advance Present
 opaque.arm(2,true,true,255);opaque.submitted(2);opaque.frame(2,true,false);assert(!opaque.presented(2)); // replaced surface/backbuffer
 opaque.arm(2,true,true,255);opaque.submitted(2,false);opaque.frame(2,true,true);assert(!opaque.presented(2)); // actual scissor/viewport partial
 opaque.arm(2,true,true,255);opaque.submitted(2,true,false);opaque.frame(2,true,true);assert(!opaque.presented(2)); // MAX/MIN/SUBTRACT can retain destination despite alpha255
 // AccountSceneTexture counts relief/gloss in progress only for the report: they hold through SceneProductWait
 // (below); once the textures are uploaded the gate becomes ready after its stable frames.
 {
  SceneReadiness gate; const uint64_t g=gate.begin(0);
  for(uint64_t frame=1;frame<=4;++frame){
   SceneResources r{};
   for(int texture=0;texture<3;++texture)
    storm::effects::AccountSceneTexture(r,frame==1&&texture==0,true); // relief pending throughout
   assert(r.relief==3&&!r.failed);
   gate.frame(g,frame,true,r);
  }
  assert(gate.state()==SceneReadiness::State::Ready&&!gate.hold(0)&&gate.resources().relief==3);
  // An upload in flight holds the reveal.
  SceneResources r{};storm::effects::AccountSceneTexture(r,true,false);assert(r.pending()&&!r.failed);
 }
 // A texture that failed during the load (empty name, missing file, no GPU copy) never holds the reveal: it never
 // reaches the in-flight accounting, so the location reveals after its stable frames with the texture failed.
 {
  SceneReadiness gate; const uint64_t g=gate.begin(0);
  storm::effects::SceneTextureStall stall; stall.begin(0);
  for(uint64_t frame=1;frame<=4;++frame){
   SceneResources r{};
   storm::effects::AccountSceneTexture(r,false,false); // the failed texture: no work left, no GPU copy
   storm::effects::AccountSceneTexture(r,frame==1&&stall.in_flight(7,1),false); // a good one, uploaded in frame 2
   assert(stall.end(frame).empty());
   gate.frame(g,frame,true,r);
  }
  assert(gate.state()==SceneReadiness::State::Ready&&!gate.hold(4)&&!gate.resources().failed);
 }
 // A texture still in flight holds the reveal only within the budget: while any in-flight texture changes stage it
 // keeps holding; when none changed for the budget, they are released (named by the caller) and the location reveals.
 {
  using storm::effects::SceneTextureStall;
  SceneReadiness gate; const uint64_t g=gate.begin(0);
  SceneTextureStall stall; stall.begin(0);
  uint64_t frame=0;
  const auto sample=[&](uint64_t now,uint64_t stage){
   SceneResources r{};
   storm::effects::AccountSceneTexture(r,stall.in_flight(9,stage),false);
   const auto released=stall.end(now);
   r.textures-=static_cast<uint32_t>(released.size());
   gate.frame(g,++frame,true,r);
   return released;
  };
  assert(sample(1000,1).empty()&&gate.hold(1000));
  assert(sample(50000,2).empty()&&gate.hold(50000)); // progressed: the budget restarts
  assert(sample(50000+SceneTextureStall::kBudgetMs-1,2).empty()&&gate.hold(110000)); // within the budget
  const auto released=sample(50000+SceneTextureStall::kBudgetMs,2);
  assert(released.size()==1&&released[0]==9);
  assert(sample(200000,2).empty()); assert(sample(200001,2).empty()); // released: it no longer holds
  assert(gate.state()==SceneReadiness::State::Ready&&!gate.hold(200001));
 }
 // The quality products (relief, x2, bakes, ...) hold the reveal within one budget of kSceneProductBudgetMs counted
 // from mandatory-ready: a fast one until it is done, a slow one until the budget, an off one never.
 {
  using storm::effects::SceneProduct; using storm::effects::SceneProducts; using storm::effects::SceneProductWait;
  using storm::effects::kSceneProductBudgetMs;
  static_assert(kSceneProductBudgetMs==5000);
  SceneProducts relief; relief.add(SceneProduct::Relief,3);
  SceneProducts bake; bake.add(SceneProduct::Place);
  // Drives a gate and a wait like SampleSceneResources: mandatory textures until `mandatory_at`, then the products.
  struct Load { SceneReadiness gate; SceneProductWait wait; uint64_t g=0, frame=0;
   Load(){ g=gate.begin(0); wait.begin(); }
   bool sample(uint64_t now,bool mandatory,const SceneProducts &p){
    SceneResources r{}; r.textures=mandatory?1:0;
    r.products=wait.hold(!r.mandatory_pending(),p,now);
    gate.frame(g,++frame,true,r);
    return gate.hold(now);
   }
  };
  // A fast product: holds until it is done, then the location reveals after its stable frames.
  {
   Load l;
   assert(l.sample(0,true,relief)); assert(!l.wait.started()); // mandatory still pending: the budget has not begun
   assert(l.sample(500,false,relief)&&l.wait.started()&&l.wait.ready_at()==500);
   assert(l.sample(3000,false,relief)); // still being made: holds
   assert(l.sample(3100,false,{})); assert(l.sample(3200,false,{})); assert(!l.sample(3300,false,{})); // done: stable frames
   assert(!l.wait.expired()&&l.wait.waited_ms()==2500&&l.wait.waited()[SceneProduct::Relief]==3&&!l.wait.last().any());
  }
  // A slow product: holds until exactly kSceneProductBudgetMs after mandatory-ready, then the location reveals
  // with it still in progress (it continues after the reveal).
  {
   Load l;
   assert(l.sample(0,true,bake)); assert(l.sample(2000,true,bake)); // mandatory pending: the bake does not count yet
   assert(l.sample(4000,false,bake)&&l.wait.ready_at()==4000); // the budget starts at mandatory-ready
   assert(l.sample(4000+kSceneProductBudgetMs-1,false,bake)); // one millisecond short: still held
   l.sample(4000+kSceneProductBudgetMs,false,bake); // the budget: released
   assert(l.wait.expired()&&l.wait.waited_ms()==kSceneProductBudgetMs);
   l.sample(4001+kSceneProductBudgetMs,false,bake); assert(!l.sample(4002+kSceneProductBudgetMs,false,bake)); // revealed with the bake still running
   assert(l.gate.state()==SceneReadiness::State::Ready&&l.wait.last()[SceneProduct::Place]==1);
   l.wait.revealed(); SceneProducts more=bake; more.add(SceneProduct::Upscale);
   assert(!l.sample(10000+kSceneProductBudgetMs,false,more)); // after the reveal nothing holds it again
  }
  // One budget per load: mandatory work found after mandatory-ready (a texture first used late) does not restart it.
  {
   Load l;
   assert(l.sample(1000,false,relief)&&l.wait.ready_at()==1000);
   assert(l.sample(1000+kSceneProductBudgetMs/2,true,relief)); // a late texture holds as mandatory, the budget runs on
   assert(l.sample(999+kSceneProductBudgetMs,false,relief)); l.sample(1000+kSceneProductBudgetMs,false,relief); assert(l.wait.expired());
  }
  // An off product (the preset does not want it: its owner reports nothing in progress) never holds: the location
  // reveals after the mandatory resources' stable frames, with nothing waited.
  {
   Load l;
   assert(l.sample(0,true,{})); assert(l.sample(1,false,{})); assert(l.sample(2,false,{})); assert(!l.sample(3,false,{}));
   assert(l.wait.waited_ms()==0&&!l.wait.waited().any()&&l.wait.waited().describe()=="nothing");
  }
  SceneProducts two; two.add(SceneProduct::Relief,12); two.add(SceneProduct::TownLight);
  assert(two.describe()=="relief/gloss maps 12, town light 1");
 }
 storm::render::LoadingBudget pacing;
 assert(pacing.deadline(100,true,20,100)==100); // no prior real frame
 pacing.frame_started(100);
 assert(pacing.deadline(110,true,20,110)==140); // includes preceding frame CPU work
 assert(pacing.deadline(145,true,20,145)==145); // late frame never adds new delay
 assert(pacing.deadline(170,true,100,170)==180); // start-to-start <100, not +100 after work
 assert(pacing.deadline(180,true,100,180)==180);
 assert(pacing.deadline(110,false,20,110)==110); // Cancel/ready/error/no opaque
 assert(pacing.deadline(110,true,20,0)==110); // unavailable timer
 assert(pacing.deadline(500,true,20,100)==500); // stale measurement
 assert(pacing.deadline(110,true,0,110)==110); // invalid timer

 assert(storm::effects::SceneCooling(true,false,storm::effects::kThermalWaitReason));
 assert(!storm::effects::SceneCooling(true,true,storm::effects::kThermalWaitReason)); // slow active job
 assert(!storm::effects::SceneCooling(false,false,storm::effects::kThermalWaitReason)); // unrelated background pause
 assert(!storm::effects::SceneCooling(true,false,"renderer measurement"));
 assert(!storm::effects::SceneCooling(true,false,""));
 SceneReadiness r; const auto a=r.begin(0);
 r.frame(a,1,true,{0,1,0,0}); assert(r.hold(1)); // deque empty, inference in flight
 r.frame(a,2,true,{0,1,0,0}); assert(r.hold(2)); // Stored/read/upload still outstanding
 r.frame(a,3,true,{1,0,0,0}); assert(r.hold(3)); // upload not final
 r.frame(a,4,true,{0,0,1,0}); assert(r.hold(4)); // lazy initial shadow
 r.frame(a,5,true,{}); r.frame(a,6,true,{}); assert(r.hold(6));
 r.invalidate(); r.frame(a,7,true,{}); assert(r.hold(7)); // shared asset first used late
 r.frame(a,8,true,{}); r.frame(a,9,true,{}); assert(!r.hold(9));
 assert(r.state()==SceneReadiness::State::Ready);
 r.frame(a,10,true,{1,0,0,0}); assert(r.hold(10)); // an existing texture enters a new residency plan
 r.frame(a,11,true,{}); r.frame(a,12,true,{}); r.frame(a,13,true,{}); assert(!r.hold(13));
 r.refresh({0,0,0,1}); assert(r.hold(13)); // failure during fade, before the next completed-frame sample
 r.invalidate(); assert(r.hold(13));
 const auto b=r.begin(10); r.frame(a,100,true,{}); assert(r.hold(11)); // stale location completion
 r.frame(b,1,false,{}); r.frame(b,2,false,{}); r.frame(b,3,false,{}); assert(r.hold(12)); // menu frames
 r.frame(b,4,true,{}); r.frame(b,5,true,{}); r.invalidate(); // current method changed
 r.frame(b,6,true,{0,0,0,0,true}); assert(r.hold(13));
 r.frame(b,7,true,{}); r.frame(b,8,true,{}); r.frame(b,9,true,{}); assert(!r.hold(14)); // warm re-entry
 const auto c=r.begin(20); r.frame(c,1,true,{0,0,0,1}); r.frame(c,2,true,{0,0,0,1}); r.frame(c,3,true,{0,0,0,1});
 assert(r.hold(23)); assert(r.state()==SceneReadiness::State::Error); // explicit failure, never PASS
 const auto d=r.begin(30); r.frame(d,1,true,{0,1,0,0}); r.invalidate();
 r.observe_progress(1,30); r.observe_progress(2,90030); // a real producer stage changes, count stays one
 assert(r.hold(120030)); assert(r.state()==SceneReadiness::State::Warming); assert(!r.slow(120030));
 r.observe_progress(2,200030); // unchanged stages and more rendered frames are not progress
 assert(r.unchanged(200030)==110000); assert(r.hold(220030)); assert(r.slow(220030));
 assert(r.state()==SceneReadiness::State::Warming); // valid long inference is not proven stalled
 r.observe_progress(3,300030); assert(!r.slow(300030)); assert(r.hold(300030));
 r.frame(d,2,true,{0,1,0,1}); assert(r.state()==SceneReadiness::State::Error); // explicit producer failure
 assert(r.hold(300031)); // even a failed producer never grants incomplete reveal
 // An explicit producer retry creates a new token. Old failure cannot poison its success.
 const auto e=r.begin(130000); r.frame(d,100,true,{0,0,0,1});
 r.frame(e,1,true,{0,1,0,0}); assert(r.hold(130001));
 r.frame(e,2,true,{}); r.frame(e,3,true,{}); r.frame(e,4,true,{}); assert(!r.hold(130004));
 puts("scene readiness lifecycle: PASS");
}
