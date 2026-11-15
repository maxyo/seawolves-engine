// Standalone CPU contract: original member sum vs shader payload; no GPU or game corpus required.
#include "storm/fx/aggregate_light.hpp"
#include "storm/fx/light_set.hpp"
#include "../../src/libs/location/src/light_components.hpp"
#include "../location_light_choice/fixture_rooms.hpp"
#include <algorithm>
#include <cstdlib>
#define CHECK(condition) do { if (!(condition)) { std::cerr << "FAIL line " << __LINE__ << ": " << #condition << "\n"; std::abort(); } } while (false)
#include <iostream>
using namespace storm;
using V = std::array<float,3>;
static std::shared_ptr<const fx::AggregateSnapshot> snapshot(std::vector<location::ComponentSource> sources)
{
    fx::AggregateSnapshot out;
    for (const auto &c:location::BuildLightComponents(sources))
    {
        fx::AggregateGroup g;
        g.id=c.id;g.center=c.position;g.first=out.members.size();g.count=c.members.size();
        g.authored_range=sources[c.members.front()].range;g.attenuation=c.attenuation;
        g.shape=fx::ShapeOfGameLight(g.authored_range,g.attenuation[0],g.attenuation[1],g.attenuation[2],{});
        for (auto index:c.members)
        {
            const auto &m=sources[index];out.members.push_back({m.id,m.position,m.color,m.steady,m.range,m.attenuation});
            g.support=std::max(g.support,fx::AggregateDistance(g.center,m.position)+g.shape.range);
        }
        out.groups.push_back(g);
    }
    return std::make_shared<const fx::AggregateSnapshot>(std::move(out));
}
static V term(V pos,V color,fx::LightShape shape,V p,V n,bool linear,float scale=1)
{
    const float d=fx::AggregateDistance(pos,p);
    float cosine=0;for(int k=0;k<3;++k)cosine+=n[k]*(pos[k]-p[k])/std::max(d,1e-4f);
    const float f=linear?fx::LightFalloffAt(shape,d):fx::LightFalloffGammaAt(shape,d);
    V out{};for(int k=0;k<3;++k){float c=color[k];out[k]=(linear?c*c:c)*std::max(cosine,0.0f)*f*(linear?scale:std::sqrt(scale));}
    return out;
}
static V original(const fx::AggregateRef &ref,V p,V n,bool linear,float scale=1)
{
    const auto &g=*ref.get();V sum{};
    for(uint32_t j=0;j<g.count;++j){const auto &m=ref.snapshot->members[g.first+j];const auto t=term(m.position,m.color,g.shape,p,n,linear,scale);for(int k=0;k<3;++k)sum[k]+=t[k];}
    return sum;
}
static V unpack(const fx::AggregatePayload &payload,const fx::AggregateRef &ref,V p,V n,bool linear,float scale=1)
{
    const size_t group=payload.index(ref)-1;const auto &h=payload.texels[group*2],&s=payload.texels[group*2+1];V sum{};
    for(int j=0;j<int(h[1]);++j){const auto &a=payload.texels[int(h[0])+2*j],&b=payload.texels[int(h[0])+2*j+1];
        const auto t=term({a[0],a[1],a[2]},{b[0],b[1],b[2]},{h[2],h[3],std::sqrt(s[0])},p,n,linear,scale);
        for(int k=0;k<3;++k)sum[k]+=t[k];}
    return sum;
}
static void equal(V a,V b){for(int k=0;k<3;++k)CHECK(std::abs(a[k]-b[k])<1e-5f*std::max(1.0f,std::abs(a[k])));}
int main()
{
    std::vector<location::ComponentSource> sources;
    uint64_t id=0;
    const auto room=fixture_rooms::Rooms()[2]; // a two-ring chandelier and two independent candles
    for(const auto &c:room.candles)sources.push_back({++id,c.position,{2.2f,1.5f,.5f},{2.2f,1.5f,.5f},15,{.5f,0,1.5f},false});
    // Unequal colors/flicker must survive packing without a group-color square.
    for(size_t i=0;i<sources.size();++i)sources[i].color={.4f+.07f*i,1.7f-.02f*i,.3f+.01f*i};
    auto s=snapshot(sources);CHECK(s->valid()&&s->groups.size()==room.count);
    std::vector<fx::AggregateRef> refs;for(uint32_t i=0;i<s->groups.size();++i)refs.push_back({s,i});
    fx::AggregatePayload payload;CHECK(payload.pack(refs));CHECK(payload.refs.size()==room.count);
    size_t controls=0;bool central=false;
    for(const auto &r:refs)
    {
        const auto &g=*r.get();
        if(g.count==16){central=true;CHECK(std::abs(g.shape.range-3.656349f)<1e-5f);CHECK(g.support>g.shape.range+0.4f&&g.support<g.shape.range+1.0f);}
        for(float distance:{0.f,.5f,1.f,1.5f,2.f,3.f,4.f,5.f})for(int axis=0;axis<3;++axis)for(float sign:{-1.f,1.f})
            for(V n:{V{1,0,0},V{-1,0,0},V{0,1,0},V{0,-1,0},V{0,0,1},V{0,0,-1}})for(bool linear:{false,true})
            {V p=g.center;p[axis]+=distance*sign;equal(original(r,p,n,linear),unpack(payload,r,p,n,linear));equal(original(r,p,n,linear,.37f),unpack(payload,r,p,n,linear,.37f));++controls;}
    }
    CHECK(central);
    const fx::AggregateRef chandelier=*std::find_if(refs.begin(),refs.end(),[](const auto &r){return r.get()->count==16;});
    // Energy fade .25 gives .25 of linear irradiance, .5 of gamma irradiance, independent of member colors.
    V receiver=chandelier.get()->center;receiver[1]-=1;
    for(bool linear:{false,true})
    {
        const auto full=original(chandelier,receiver,{0,1,0},linear);
        const auto faded=unpack(payload,chandelier,receiver,{0,1,0},linear,.25f);
        for(int k=0;k<3;++k)CHECK(std::abs(faded[k]-full[k]*(linear?.25f:.5f))<1e-5f);
    }
    // Snapshot restore is pointer/index based: publishing a new frame cannot mutate old RGB or payload rows.
    const auto old=chandelier;const auto oldColor=old.snapshot->members[old.get()->first].color;
    for(auto &m:sources)m.color={.1f,.8f,.2f};auto newer=snapshot(sources);
    CHECK(old.snapshot->members[old.get()->first].color==oldColor);
    CHECK(fx::SameAggregateGeometry(old,{newer,old.group}));
    auto moved=std::make_shared<fx::AggregateSnapshot>(*old.snapshot);
    const auto first=old.get()->first;
    std::swap(moved->members[first].position,moved->members[first+1].position);
    CHECK(!fx::SameAggregateGeometry(old,{moved,old.group})); // center/support unchanged; original exclusions differ
    auto changedWeight=std::make_shared<fx::AggregateSnapshot>(*old.snapshot);
    changedWeight->members[first].steady[0]*=.5f;
    CHECK(!fx::SameAggregateGeometry(old,{changedWeight,old.group}));
    fx::AggregatePayload mixed;std::array mix{old,fx::AggregateRef{newer,0},old};CHECK(mixed.pack(mix)&&mixed.refs.size()==2);
    // Capacity failure is atomic, including a malformed span: no silent member dropping.
    auto tooLarge=std::make_shared<fx::AggregateSnapshot>(*s);tooLarge->members.resize(257);
    auto before=payload.texels;std::array bad{fx::AggregateRef{tooLarge,0}};CHECK(!payload.pack(bad)&&payload.texels==before);
    tooLarge->members.resize(256);tooLarge->groups[0].first=256;tooLarge->groups[0].count=1;CHECK(!payload.pack(bad));
    // Remote chain end retains original radius; group center rejection must not discard it.
    sources.clear();for(int i=0;i<11;++i)sources.push_back({uint64_t(i),{.8f*i,0,0},{1,1,1},{1,1,1},15,{.5f,0,1.5f},false});
    auto chain=snapshot(sources);CHECK(chain->groups.size()==1);fx::AggregateRef r{chain,0};CHECK(r.get()->count==11);
    V p{10,0,0},n{-1,0,0};CHECK(original(r,p,n,true)[0]>0);CHECK(fx::AggregateScore(r,p,0)>0);
    fx::AggregatePayload chainPayload;std::array one{r};CHECK(chainPayload.pack(one));equal(original(r,p,n,true),unpack(chainPayload,r,p,n,true));
    // Exercise the renderer's actual failure packer, including sun + independent lit point + baked torch.
    fx::LightSelection failed;
    failed.count=3;
    failed.lights[0].aggregate=r;failed.lights[0].device_slot=3;
    failed.lights[1].color={.2f,.4f,.8f};failed.lights[1].device_slot=2;
    failed.lights[2].color={.9f,.3f,.1f}; // independent baked torch has no D3D vertex slot
    const std::array<float,16> identity{1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
    const auto fallback=fx::PackUnmergedPointLights(failed,identity,0b1101,0b1101,0b0001);
    CHECK(fallback.diffuse_off==0b0101 && fallback.specular_off==0b0101);
    CHECK(fallback.points.u_fxPointCount[0]==2);
    CHECK(fallback.points.u_fxPoint[1][0]==.2f && fallback.points.u_fxPoint[5][0]==.9f);
    // Source ownership domains: independent moving lights cannot merge merely because they pass nearby.
    sources[0].moving=sources[1].moving=true;CHECK(location::BuildLightComponents(sources).size()==3);
    // Explicit selection indices survive rank fade; comparing PointLight bytes would fail after the fade.
    std::vector<fx::PointLight> point(10);for(size_t i=0;i<point.size();++i){point[i].color={float(i+1),1,1};point[i].position={float(i),0,0};}
    const auto selected=fx::SelectLights(point,{0,0,0},0,4,true,true);CHECK(selected.count==4);
    for(int i=0;i<selected.count;++i)CHECK(selected.lights[i].position==point[selected.indices[i]].position);
    std::cout<<"aggregate CPU payload/reference PASS "<<controls<<" receiver/normal/mode controls; capacity/mixed lifetime/chain/indices PASS\n";
}
