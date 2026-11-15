#include "effects/scene_readiness.hpp"
#include <cassert>
#include <cstdio>
using storm::effects::SceneReadiness;
using storm::effects::SceneResources;
int main() {
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
