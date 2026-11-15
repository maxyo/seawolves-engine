#include "storm/fx/relief_store.hpp"
#include "storm/fx/upscale.hpp"
#include <cassert>
#include <fstream>
#include <filesystem>
#include <vector>
#include <iostream>
using namespace storm::fx;
int main(int argc,char**argv){assert(argc==2);std::filesystem::path dir=argv[1];
 std::vector<uint8_t> bytes{1,2,3,4};assert(WriteUpscaled(dir,1,bytes));assert(ReadUpscaled(dir,1)==bytes);
 std::string error;assert(!ReadUpscaled(dir,1,&error,1)&&error=="cache read reservation changed"&&HasUpscaled(dir,1));
 auto badtarget=UpscaleFile(dir,2);std::filesystem::create_directory(badtarget);std::ofstream(badtarget/"keep")<<"original";
 assert(!WriteUpscaled(dir,2,bytes)&&"rename failure must not report success after cleanup");assert(std::filesystem::exists(badtarget/"keep"));
 assert(!std::filesystem::exists(badtarget.string()+".tmp"));
 std::ofstream(UpscaleFile(dir,1),std::ios::binary|std::ios::app).put('x');assert(!ReadUpscaled(dir,1));
 NormalMap map;map.width=map.height=4;map.mip_count=3;map.format=NormalMapFormat::Rg8;map.rg.resize(NormalMapBytes(4,4,map.format),7);
 assert(WriteReliefMap(dir,3,map));auto valid=ReadReliefMap(dir,3);assert(valid&&valid->rg==map.rg);
 assert(!ReadReliefMap(dir,3,&error,1)&&error=="cache read reservation changed"&&HasReliefMap(dir,3));
 std::ofstream(ReliefFile(dir,3),std::ios::binary|std::ios::app).put('x');assert(!ReadReliefMap(dir,3));
 assert(WriteReliefMap(dir,3,map));std::filesystem::resize_file(ReliefFile(dir,3),std::filesystem::file_size(ReliefFile(dir,3))-1);assert(!ReadReliefMap(dir,3));
 // Valid legacy bytes survive format-preserving rewrite/read.
 assert(WriteReliefMap(dir,3,map));assert(ReadReliefMap(dir,3));
 std::cout<<"PASS actual legacy codec roundtrip/EOF/truncation/reservation/rename failure\n";
}
