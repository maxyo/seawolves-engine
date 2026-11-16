#!/usr/bin/env python3
"""Actual CPU Run body with an inert ncnn: the CPU permit is held across the ncnn extraction; a cancelled admission,
input/extraction failures and ncnn's out-of-memory status report failure. No models, inference or device."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[2]
backend=(root/'src/libs/relief/backend.cpp').read_text()
start=backend.index('    bool Run(const float *image, int width, int height, float *planes, std::string &error)')
body=backend[start:backend.index('\n    void Cancel()',start)]
source=r'''
#include <cassert>
#include <chrono>
#include <cstring>
#include <functional>
#include <future>
#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>
std::atomic<int> held{0};
struct Api {int fail_input=0,fail_run=0,calls=0,held_during_run=0;};
namespace ncnn {
struct Mat {
 int w=0,h=0,c=0;std::vector<float> d;
 Mat()=default;Mat(int w_,int h_,int c_):w(w_),h(h_),c(c_),d(size_t(w_)*h_*c_){}
 bool empty()const{return d.empty();}
 struct Channel{float*p;int w;float*row(int y){return p+size_t(y)*w;}};
 Channel channel(int k){return {d.data()+size_t(k)*w*h,w};}
};
struct Extractor {
 Api*api;
 int input(const char*,const Mat&){return api->fail_input;}
 int extract(const char*,Mat&m){++api->calls;api->held_during_run=held.load();if(api->fail_run)return api->fail_run;
  m=Mat(1,1,3);for(int k=0;k<3;++k)m.d[k]=float(k+1);return 0;}
};
struct Net {Api*api;Extractor create_extractor(){return {api};}};
}
constexpr const char *kCpuOutOfMemory="ncnn: CPU inference out of memory";
struct Actual {
 Api *api_;ncnn::Net net_{api_};int scale_=1;
 std::function<std::shared_ptr<void>()>cpu_admission_;
 BODY
};
std::shared_ptr<void> permit(){++held;return std::shared_ptr<void>(new int,[](void*p){delete static_cast<int*>(p);--held;});}
int main(){
 Api api;Actual a{&api};std::atomic<bool> entered=false;std::promise<void> release;auto gate=release.get_future().share();
 a.cpu_admission_=[&]{entered=true;gate.wait();return permit();};
 float in[3]={},out[3]={};std::string error;
 auto work=std::async(std::launch::async,[&]{return a.Run(in,1,1,out,error);});
 while(!entered)std::this_thread::yield();
 // The extraction waits for the permit, holds it while running and releases it after.
 assert(work.wait_for(std::chrono::milliseconds(50))==std::future_status::timeout&&api.calls==0);
 release.set_value();assert(work.get()&&api.calls==1&&api.held_during_run==1&&held==0&&out[2]==3);
 // A cancelled admission never reaches ncnn.
 a.cpu_admission_=[]{return std::shared_ptr<void>{};};
 assert(!a.Run(in,1,1,out,error)&&api.calls==1&&error=="CPU admission cancelled");
 // Input and extraction failures are reported and release the permit; ncnn's -100 is the out-of-memory error.
 a.cpu_admission_=permit;api.fail_input=1;
 assert(!a.Run(in,1,1,out,error)&&api.calls==1&&held==0&&error!=kCpuOutOfMemory);
 api.fail_input=0;api.fail_run=-1;
 assert(!a.Run(in,1,1,out,error)&&api.calls==2&&held==0&&error!=kCpuOutOfMemory);
 api.fail_run=-100;
 assert(!a.Run(in,1,1,out,error)&&api.calls==3&&held==0&&error==kCpuOutOfMemory);
}
'''.replace('BODY',body)
with tempfile.TemporaryDirectory(prefix='cpu-run-admission-') as tmp:
 p=Path(tmp);(p/'check.cpp').write_text(source)
 subprocess.run(['c++','-std=c++20','-O0','-pthread','-fsanitize=address,undefined','-fno-sanitize-recover=all',str(p/'check.cpp'),'-o',str(p/'check')],check=True)
 subprocess.run([str(p/'check')],check=True,timeout=300) # deadlock guard only: every wait is on progress
print('actual CPU Run: permit held across the ncnn extraction, cancelled admission, input/extraction/out-of-memory failures PASS')
