#!/usr/bin/env python3
"""Actual CPU Run body, inert Ort API; no models/inference/device."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[2]
backend=(root/'src/libs/relief/backend.cpp').read_text()
start=backend.index('    bool Run(const float *image, int width, int height, float *planes, std::string &error)')
body=backend[start:backend.index('\n    void Cancel()',start)]
source=r'''
#include "backend.hpp"
#include <cassert>
#include <chrono>
#include <cstring>
#include <future>
#include <atomic>
#include <thread>
using Clock=std::chrono::steady_clock;
double Ms(Clock::time_point t){return std::chrono::duration<double,std::milli>(Clock::now()-t).count();}
using storm::relief::CpuInferenceTiming;
struct OrtValue{};constexpr int ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT=1;
struct Api {
 bool fail_tensor=false,fail_run=false;int calls=0;float pixels[3]={1,2,3};OrtValue value;
 int CreateTensorWithDataAsOrtValue(void*,float*,size_t,const int64_t*,int,int,OrtValue**v){*v=&value;return fail_tensor?1:0;}
 int Run(void*,void*,const char**,OrtValue**,int,const char**,int,OrtValue**v){++calls;std::this_thread::sleep_for(std::chrono::milliseconds(20));*v=&value;return fail_run?1:0;}
 int GetTensorMutableData(OrtValue*,void**p){*p=pixels;return 0;}
 void ReleaseValue(OrtValue*){}
};
bool OrtOk(Api*,int status,std::string&){return status==0;}
struct Actual {
 Api *api_;void *memory_=nullptr,*session_=nullptr,*run_=nullptr;int scale_=1;
 CpuInferenceTiming cpu_timing_{true};std::function<std::shared_ptr<void>()>cpu_admission_;
 BODY
};
int main(){
 Api api;Actual a{&api};std::atomic<bool> entered=false;std::promise<void> release;auto gate=release.get_future().share();
 a.cpu_admission_=[&]{entered=true;gate.wait();return std::shared_ptr<void>(new int,[](void*p){delete static_cast<int*>(p);});};
 float in[3]={},out[3]={};std::string error;
 auto work=std::async(std::launch::async,[&]{return a.Run(in,1,1,out,error);});
 while(!entered)std::this_thread::yield();
 assert(work.wait_for(std::chrono::milliseconds(80))==std::future_status::timeout&&api.calls==0);
 release.set_value();assert(work.get());assert(a.cpu_timing_.tiles==1&&a.cpu_timing_.admission_wall_ms>=70&&a.cpu_timing_.ort_wall_ms>=15&&a.cpu_timing_.ort_wall_ms<a.cpu_timing_.admission_wall_ms&&out[2]==3);
 a.cpu_timing_={true};a.cpu_admission_=[] {return std::shared_ptr<void>{};};
 assert(!a.Run(in,1,1,out,error)&&a.cpu_timing_.tiles==0&&api.calls==1);
 a.cpu_admission_={};api.fail_tensor=true;a.cpu_timing_={true};
 assert(!a.Run(in,1,1,out,error)&&a.cpu_timing_.tiles==0&&api.calls==1);
 api.fail_tensor=false;api.fail_run=true;a.cpu_timing_={true};
 assert(!a.Run(in,1,1,out,error)&&a.cpu_timing_.tiles==1&&api.calls==2&&a.cpu_timing_.ort_wall_ms>=15);
 CpuInferenceTiming sum;sum.Add({true,2,10,20});sum.Add({true,3,5,7});assert(sum.applicable&&sum.tiles==5&&sum.admission_wall_ms==15&&sum.ort_wall_ms==27);
}
'''.replace('BODY',body)
assert 'cpu_timing_ = {true};' in backend[backend.index('    bool Infer('):start], 'reset per actual Infer required'
upscale=(root/'src/libs/relief/upscale.cpp').read_text()
assert 'cpu_timing_ = {};' in upscale and 'cpu_timing_.Add(network_->CpuTiming());' in upscale
with tempfile.TemporaryDirectory(prefix='cpu-timing-') as tmp:
 p=Path(tmp);(p/'check.cpp').write_text(source)
 subprocess.run(['c++','-std=c++20','-O0','-pthread','-I'+str(root/'src/libs/relief'),str(p/'check.cpp'),'-o',str(p/'check')],check=True)
 subprocess.run([str(p/'check')],check=True,timeout=3)
print('actual CPU Run timing/cancel/tensor+ORT failure and Add PASS; reset/outer aggregation source-only')
