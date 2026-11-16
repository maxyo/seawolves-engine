#!/usr/bin/env python3
"""Actual CPU Infer/RunTiles control, inert model callback; no ncnn/models/device.
Optional --baseline <backend.cpp> must compile and fail the retry sentinel at runtime.
"""
from pathlib import Path
import argparse, os, subprocess, tempfile
p=argparse.ArgumentParser(); p.add_argument('--baseline',type=Path); a=p.parse_args()
root=Path(__file__).resolve().parents[2]
source=(a.baseline or root/'src/libs/relief/backend.cpp').read_text()
start=source.index('bool RunTiles(')
end=source.index('\n// ',source.index('\n    return true;\n}',start))
tiler=source[start:end]
start=source.index('    bool Infer(',source.index('class CpuNetwork'))
end=source.index('\n    void SetCpuAdmission(',start)
infer=source[start:end].replace(' override','')
code=r'''
#include "backend.hpp"
#include "cpu_tiles.hpp"
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <new>
#include <stdexcept>
#include <vector>
using namespace storm::relief;
void Check(bool v,const char*s){if(!v){std::cerr<<s<<'\n';std::exit(23);}}
template<class...T>void Log(const char*,T...){}
TILER
struct Actual {
 int halo_,scale_,tile_;std::atomic<bool>cancelled_{false};
 int calls=0,fail_at=0,failures=0,fail_larger=0;bool unknown=false,cancel_on_failure=false,cancel_on_success=false,scratch=false;
 std::vector<std::pair<int,int>> shapes;
 bool Run(const float*in,int w,int h,float*out,std::string&error){
   ++calls;shapes.emplace_back(w,h);
   if((fail_at&&calls==fail_at)||(fail_larger&&std::max(w-2*halo_,h-2*halo_)>fail_larger)){
     ++failures;if(cancel_on_failure)cancelled_=true;
     if(scratch)throw std::bad_alloc();
     error=unknown?"ncnn: CPU inference failed (status -1): shape mismatch":kCpuOutOfMemory;
     return false;
   }
   // A local stencil uses the halo, detects layout/copy errors and runs at either output scale.
   const int ow=w*scale_,oh=h*scale_;
   for(int c=0;c<3;++c)for(int y=0;y<oh;++y)for(int x=0;x<ow;++x){
     int ix=x/scale_,iy=y/scale_;
     auto at=[&](int xx,int yy){return in[(size_t(c)*h+std::clamp(yy,0,h-1))*w+std::clamp(xx,0,w-1)];};
     out[(size_t(c)*oh+y)*ow+x]=at(ix,iy)*.5f+(at(ix-1,iy)+at(ix+1,iy)+at(ix,iy-1)+at(ix,iy+1))*.125f;
   }
   if(cancel_on_success)cancelled_=true;
   return true;
 }
 INFER
};
std::vector<float>Image(int w,int h){std::vector<float>v(size_t(w)*h*3);for(size_t i=0;i<v.size();++i)v[i]=float((i*29)%251)/251;return v;}
void EqualInner(const std::vector<float>&a,const std::vector<float>&b,int w,int h,int halo,int scale){
 for(int c=0;c<3;++c)for(int y=halo*scale;y<(h-halo)*scale;++y)for(int x=halo*scale;x<(w-halo)*scale;++x){size_t i=(size_t(c)*h*scale+y)*w*scale+x;Check(a[i]==b[i],"TILED_OUTPUT_MISMATCH");}
}
int main(){
 Check(DefaultCpuTile(10.9)==256&&DefaultCpuTile(4)==128&&DefaultCpuTile(0)==128,"HIGH_RAM_TILE_UNBOUNDED");
 Check(CpuAllocationFailure(kCpuOutOfMemory)&&!CpuAllocationFailure("ncnn: CPU inference failed (status -1)")&&!CpuAllocationFailure("cancelled"),"UNKNOWN_IS_OOM");
 Check(SmallerCpuTile(0,1072,560,24)==256&&SmallerCpuTile(256,112,96,24)==32&&SmallerCpuTile(32,80,80,24)==0,"RETRY_NOT_STRICTLY_SMALLER");
 for(int scale:{1,4}){
   const int w=349,h=237,halo=24;auto input=Image(w,h);std::vector<float>gold(size_t(w)*h*3*scale*scale,-7),output(gold.size(),-9);std::string error;
   Actual whole{halo,scale,0};Check(whole.Infer(input.data(),w,h,gold.data(),error),"REFERENCE_FAILED");
   Actual retry{halo,scale,0};retry.fail_larger=64;
   Check(retry.Infer(input.data(),w,h,output.data(),error),"ALLOCATION_RETRY_DID_NOT_RECOVER");
   Check(error.empty()&&retry.tile_==64&&retry.failures>0&&retry.failures<=4,"RETRY_STATE_INVALID");
   EqualInner(gold,output,w,h,halo,scale);
   int calls=retry.calls;Check(retry.Infer(input.data(),w,h,output.data(),error)&&retry.failures<=4&&retry.calls>calls,"LEARNED_BOUND_NOT_REUSED");
   // A failed late tile may have left earlier inner pixels; restart must overwrite them all.
   Actual partial{halo,scale,128};partial.fail_at=2;
   Check(partial.Infer(input.data(),w,h,output.data(),error),"PARTIAL_RETRY_FAILED");EqualInner(gold,output,w,h,halo,scale);
 }
 const int w=160,h=144,halo=24;auto in=Image(w,h);std::vector<float>out(in.size());std::string error;
 Actual unknown{halo,1,0};unknown.fail_at=1;unknown.unknown=true;
 Check(!unknown.Infer(in.data(),w,h,out.data(),error)&&unknown.calls==1&&error.find("shape mismatch")!=error.npos,"UNKNOWN_RETRIED");
 Actual cancelled{halo,1,0};cancelled.fail_at=1;cancelled.cancel_on_failure=true;
 Check(!cancelled.Infer(in.data(),w,h,out.data(),error)&&cancelled.calls==1,"CANCELLED_RETRIED");
 Actual racing{halo,1,0};racing.cancel_on_success=true;
 Check(!racing.Infer(in.data(),w,h,out.data(),error)&&racing.calls==1&&error=="cancelled","SUCCESS_AFTER_CANCEL");
 Actual stopped{halo,1,0};stopped.cancelled_=true;
 Check(!stopped.Infer(in.data(),w,h,out.data(),error)&&stopped.calls==0,"CANCELLED_SUBMITTED");
 Actual exhausted{halo,1,256};exhausted.fail_larger=1;
 Check(!exhausted.Infer(in.data(),w,h,out.data(),error)&&exhausted.calls<=5&&exhausted.tile_==32&&!error.empty(),"RETRY_UNBOUNDED");
 Actual scratch{halo,1,0};scratch.fail_at=1;scratch.scratch=true;
 Check(scratch.Infer(in.data(),w,h,out.data(),error)&&scratch.failures==1,"SCRATCH_RETRY_FAILED");
 std::cout<<"actual CPU Infer/RunTiles allocation recovery, x1/x4 stencil equality, partial/cancel/unknown/exhaustion PASS\n";
}
'''.replace('TILER',tiler).replace('INFER',infer)
with tempfile.TemporaryDirectory(prefix='cpu-tiles-',dir=os.environ.get('TMPDIR')) as tmp:
    tmp=Path(tmp);(tmp/'check.cpp').write_text(code)
    subprocess.run(['c++','-std=c++20','-O1','-g','-pthread','-fsanitize=address,undefined','-fno-omit-frame-pointer','-I'+str(root/'src/libs/relief'),str(tmp/'check.cpp'),'-o',str(tmp/'check')],check=True)
    result=subprocess.run([str(tmp/'check')],text=True,capture_output=True,timeout=300) # deadlock guard, not a time budget: the host suite runs many checks at once
    if a.baseline:
        assert result.returncode==23 and 'ALLOCATION_RETRY_DID_NOT_RECOVER' in result.stderr,(result.returncode,result.stdout,result.stderr)
        print('old actual Infer compiler0, exact ALLOCATION_RETRY_DID_NOT_RECOVER negative PASS')
    else:
        assert result.returncode==0,(result.returncode,result.stdout,result.stderr)
        print(result.stdout,end='')
