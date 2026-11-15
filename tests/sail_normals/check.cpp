// The test runner inserts the actual production FillVertex body and its old-form mutation control.
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace glm {
struct vec3 {
    float x = 0, y = 0, z = 0;
    vec3() = default;
    vec3(float a, float b, float c) : x(a), y(b), z(c) {}
    vec3 &operator+=(vec3 b) { x += b.x; y += b.y; z += b.z; return *this; }
};
vec3 operator+(vec3 a, vec3 b) { return a += b; }
vec3 operator-(vec3 a, vec3 b) { return {a.x-b.x, a.y-b.y, a.z-b.z}; }
vec3 operator*(vec3 a, float b) { return {a.x*b, a.y*b, a.z*b}; }
}
namespace storm::math {
glm::vec3 normalizeOrZero(glm::vec3 v) {
    float d = std::sqrt(v.x*v.x+v.y*v.y+v.z*v.z);
    return d > 1e-12f ? v*(1.0f/d) : glm::vec3{};
}
}
constexpr int SAIL_COL_MAX=13, SAIL_ROW_MAX=17;
enum {SAIL_TREANGLE, SAIL_SQUARE, SAIL_TRAPECIDAL};
struct SAILVERTEX { glm::vec3 pos, norm; };
struct SAILONE {
    struct { int eSailType; glm::vec3 hardPoints[4]; struct {glm::vec3 c; float r;} boundSphere; } ss{};
    struct { struct {glm::vec3 normL,dnormL,dVv,dVh,ddVh,ddVv,dddVv;} cv; } sgeo{};
    void FillVertex(SAILVERTEX*);
    void OldFillVertex(SAILVERTEX*);
};
// INSERT_ACTUAL_FILL_VERTEX

static void require(bool v, const char *why) { if(!v) {std::fprintf(stderr,"FAIL %s\n",why);std::exit(1);} }
static bool same(glm::vec3 a, glm::vec3 b) {return a.x==b.x && a.y==b.y && a.z==b.z;}
int main() {
    for (int kind : {SAIL_TREANGLE, SAIL_SQUARE, SAIL_TRAPECIDAL}) {
        SAILONE s;
        s.ss.eSailType=kind;
        s.ss.hardPoints[0]={-1,-1,0}; s.ss.hardPoints[1]={1,-1,0};
        s.ss.hardPoints[2]={-1,1,0}; s.ss.hardPoints[3]={1,1,0};
        s.ss.boundSphere={{0,0,0},std::sqrt(2.0f)};
        s.sgeo.cv.normL={0,0,1}; s.sgeo.cv.dnormL={0,0,0};
        s.sgeo.cv.dVv=kind==SAIL_TREANGLE ? glm::vec3{-2.0f/16,2.0f/16,0} : glm::vec3{0,2.0f/16,0};
        std::array<SAILVERTEX,SAIL_COL_MAX*SAIL_ROW_MAX> now{},old{};
        s.FillVertex(now.data());s.OldFillVertex(old.data());
        const int n=kind==SAIL_TREANGLE ? SAIL_ROW_MAX*(SAIL_ROW_MAX+1)/2 : SAIL_COL_MAX*SAIL_ROW_MAX;
        int old_unlit=0;
        for(int i=0;i<n;++i) {
            require(same(now[i].pos,old[i].pos),"normal correction must not change geometry");
            const auto a=now[i].norm;
            require(std::isfinite(a.x)&&std::isfinite(a.y)&&std::isfinite(a.z),"finite normal");
            require(a.z>0.0f,"face-normal light reaches every planar sail vertex");
            const float length=std::sqrt(a.x*a.x+a.y*a.y+a.z*a.z);
            require(std::abs(length-1)<1e-5f,"unit smooth normal");
            if(kind!=SAIL_SQUARE) require(same(a,old[i].norm),"triangle/trapezoid unchanged");
            old_unlit += old[i].norm.z<=0;
        }
        if(kind==SAIL_SQUARE) require(old_unlit==n,"old square mutation must lose face-normal illumination");
    }
    std::puts("PASS actual FillVertex: planar finite positive smooth normals; old square fails; triangle/trapezoid and positions unchanged");
}
