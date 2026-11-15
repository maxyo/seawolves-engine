#include "storm/render/loading_scene_work.hpp"
#include "effects/scene_readiness.hpp"
#include <cassert>
using storm::render::LoadingSceneWork;
using storm::effects::SceneReadiness;
using storm::effects::SceneResources;
int main() {
    LoadingSceneWork work;
    SceneReadiness gate;
    const auto gen = gate.begin(0);
    SceneResources pending; pending.shadows = 1;
    for (unsigned frame = 1; frame <= 100; ++frame) {
        work.begin(frame, gen, 5, 8, true, true, true, true);
        assert(work.hidden(gen,5,8));
        work.presented(true,true);
        gate.frame(gen,frame,work.complete(gen),frame == 100 ? SceneResources{} : pending);
        assert(gate.state() == SceneReadiness::State::Warming);
        work.end();
    }
    for (unsigned frame = 101; frame <= 103; ++frame) {
        work.begin(frame,gen,5,8,true,true,false,true);
        assert(!work.hidden(gen,5,8)); work.beauty_submitted(); work.presented(true,true);
        gate.frame(gen,frame,work.complete(gen),{});
        assert((gate.state() == SceneReadiness::State::Ready) == (frame == 103)); work.end();
    }
    const auto next = gate.begin(200);
    for (unsigned frame=201; frame<=202; ++frame) {
        work.begin(frame,next,5,8,true,true,false,true); work.beauty_submitted(); work.presented(true,true);
        if (frame==202) {work.demand(); gate.invalidate();}
        gate.frame(next,frame,work.complete(next),{}); work.end();
    }
    assert(gate.state()==SceneReadiness::State::Warming);
    work.begin(203,next,5,8,true,true,false,true);
    assert(work.hidden(next,5,8));
    assert(work.hidden(next+1,5,8)); assert(work.hidden(next,6,8)); assert(work.hidden(next,5,9));
    work.beauty_submitted(); work.presented(true,true); assert(!work.complete(next)); work.end();
    for (unsigned frame=204; frame<=206; ++frame) {
        work.begin(frame,next,5,8,true,true,false,true); work.beauty_submitted(); work.presented(true,true);
        gate.frame(next,frame,work.complete(next),{}); work.end();
        assert((gate.state()==SceneReadiness::State::Ready)==(frame==206));
    }
    work.begin(207,next,5,8,true,false,true,true); assert(!work.hidden(next,5,8));
    work.beauty_submitted(); work.presented(false,true); assert(!work.complete(next)); work.end();
    // Failed Present does not advance frame id: a fresh attempt still revokes its old decision.
    work.begin(207,next,6,9,true,false,true,false); assert(!work.hidden(next,6,9));
    work.beauty_submitted(); work.presented(true,false); assert(!work.complete(next)); work.end();
    work.begin(208,next,6,9,true,true,true,true); assert(work.hidden(next,6,9));
    work.invalidate(); assert(work.hidden(next,6,9)); assert(!work.complete(next)); work.end();
    work.begin(209,next,6,9,true,true,false,true); assert(work.hidden(next,6,9)); work.end();
    work.begin(210,next,6,9,true,true,false,true); assert(!work.hidden(next,6,9));
    assert(!work.hidden(next,7,9)); work.beauty_submitted(); work.presented(true,true); assert(!work.complete(next)); work.end();
    work.begin(211,next,7,9,true,true,false,true); assert(!work.hidden(next,7,10));
    work.beauty_submitted(); work.presented(true,true); assert(!work.complete(next)); work.end();
    work.begin(212,next,7,10,true,true,false,true); work.presented(true,true);
    assert(!work.complete(next)); // Perspective discovery alone: every program/draw skipped.
    work.end();
    work.begin(213,next,7,10,true,false,false,true); work.beauty_submitted(); work.presented(true,true);
    assert(!work.complete(next)); assert(work.complete(next,true)); work.end();
}
