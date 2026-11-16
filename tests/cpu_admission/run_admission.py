#!/usr/bin/env python3
"""Actual CPU Run body with an inert Ort API: the CPU permit is held across ORT Run; a cancelled admission
and tensor/Run failures report failure. No models, inference or device."""
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
struct OrtValue{};constexpr int ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT=1;
std::atomic<int> held{0};
struct Api {
 bool fail_tensor=false,fail_run=false;int calls=0,held_during_run=0;float pixels[3]={1,2,3};OrtValue value;
 int CreateTensorWithDataAsOrtValue(void*,float*,size_t,const int64_t*,int,int,OrtValue**v){*v=&value;return fail_tensor?1:0;}
 int Run(void*,void*,const char**,OrtValue**,int,const char**,int,OrtValue**v){++calls;held_during_run=held.load();*v=&value;return fail_run?1:0;}
 int GetTensorMutableData(OrtValue*,void**p){*p=pixels;return 0;}
 void ReleaseValue(OrtValue*){}
};
bool OrtOk(Api*,int status,std::string&){return status==0;}
struct Actual {
 Api *api_;void *memory_=nullptr,*session_=nullptr,*run_=nullptr;int scale_=1;
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
 // ORT Run waits for the permit, holds it while running and releases it after.
 assert(work.wait_for(std::chrono::milliseconds(50))==std::future_status::timeout&&api.calls==0);
 release.set_value();assert(work.get()&&api.calls==1&&api.held_during_run==1&&held==0&&out[2]==3);
 // A cancelled admission never reaches ORT.
 a.cpu_admission_=[]{return std::shared_ptr<void>{};};
 assert(!a.Run(in,1,1,out,error)&&api.calls==1&&error=="CPU admission cancelled");
 // Tensor and Run failures are reported and release the permit.
 a.cpu_admission_=permit;api.fail_tensor=true;
 assert(!a.Run(in,1,1,out,error)&&api.calls==1&&held==0);
 api.fail_tensor=false;api.fail_run=true;
 assert(!a.Run(in,1,1,out,error)&&api.calls==2&&held==0);
}
'''.replace('BODY',body)
with tempfile.TemporaryDirectory(prefix='cpu-run-admission-') as tmp:
 p=Path(tmp);(p/'check.cpp').write_text(source)
 subprocess.run(['c++','-std=c++20','-O0','-pthread','-fsanitize=address,undefined','-fno-sanitize-recover=all',str(p/'check.cpp'),'-o',str(p/'check')],check=True)
 subprocess.run([str(p/'check')],check=True,timeout=300) # deadlock guard only: every wait is on progress
print('actual CPU Run: permit held across ORT Run, cancelled admission, tensor/Run failures PASS')
