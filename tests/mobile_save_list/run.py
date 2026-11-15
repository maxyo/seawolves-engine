#!/usr/bin/env python3
"""Exercise production full-list identity across profiles, the record facts parser and atomic original-script selection adapter."""
from pathlib import Path
import subprocess,tempfile,os
root=Path(__file__).resolve().parents[2]
source=(root/'src/libs/xinterface/src/mobile_save_list.hpp').read_text()
state=source[source.index('// One record:'):source.index('// The save list of the open save_load interface')]
facts=source[source.index('// What the save list shows of a record'):source.index('// Identity is bound to the complete current list')]
cap=source[source.index('class SaveListCapability'):source.index('inline SaveListCapability saveListCapability')]
select=source[source.index('inline bool SelectSaveTarget'):source.index('\n}\nnamespace storm::mobile')]
restore=source[source.index('inline bool RestoreSaveTarget'):source.rindex('\n}')]
choose=source[source.index('inline std::string ProfileToChoose('):source.index('// Makes `profile` the game')]
metadata_source=(root/'src/libs/xinterface/src/mobile_save_facts.cpp').read_text()
metadata=metadata_source[metadata_source.index('class SaveFactsCache'):metadata_source.index('SaveFactsCache saveFacts;')]
interface_source=(root/'src/libs/xinterface/src/xinterface.cpp').read_text()
getter=interface_source[interface_source.index('bool XINTERFACE::SFLB_GetSaveFileData('):interface_source.index('\nvoid XINTERFACE::ReleaseSaveFindList')]
getter=getter.replace('XINTERFACE::SFLB_GetSaveFileData','ReadSaveDescription')
code=r'''
#include <string>
#include <vector>
#include <optional>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <functional>
#include <map>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <algorithm>
#include "save_thumbnail.hpp"
#include "utf8.h"
#include <filesystem>
#include <memory>
#include <ctime>
namespace save=storm::save;
namespace spdlog {int errors=0;template<class... A> void error(A&&...){++errors;}}
struct MobileWidget { std::string node,type,label,value;int index,order;bool enabled;std::string rowId;std::vector<std::string> actions;std::string unavailableReason;std::map<std::string,std::string> context; };
''' + state + facts + cap + choose + r'''
SaveListSnapshot live;
std::map<std::string,std::vector<std::string>> profiles; // the profile folders on disk
std::optional<SaveListSnapshot> ReadSaveList(){return live;}
int switches=0;
bool ChooseSaveProfile(const std::string &profile){
 if(!profiles.count(profile))return false;
 ++switches;live.profile=profile;live.files=profiles[profile];live.selected=0;return true;
}
struct VDATA {int value;bool Get(int32_t &out){out=value;return true;}void Set(int32_t in){value=in;live.selected=in;}} page{0},selection{0};
constexpr int ACTION_MOUSECLICK=1;
struct Core {
 std::map<std::string,std::vector<uint8_t>> save_data;int reads=0;
 void *GetSaveData(const char *path,int32_t &size){++reads;auto it=save_data.find(path);size=it==save_data.end()?0:it->second.size();if(!size)return nullptr;auto *p=new char[size];std::memcpy(p,it->second.data(),size);return p;}
 void *GetScriptVariable(const char *name){return std::string(name)=="g_nFirstSaveIndex"?&page:&selection;}
 void Event(const char *name,const char *,float position,const char *){
  assert(std::string(name)=="ScrollPosChange");page.value=int((live.files.size()/5)*position)*5;selection.Set(page.value);
 }
 void Event(const char *name,const char *,int,const char *node){
  assert(std::string(name)=="eventSaveClick");selection.Set(page.value+std::stoi(std::string(node).substr(7))-1);
 }
} core;
struct Files {
 std::map<std::string,std::filesystem::file_time_type> stamps;
 bool _FileOrDirectoryExists(const char *path){return core.save_data.count(path);}
 auto _GetLastWriteTime(const char *path){return stamps[path];}
 std::time_t _ToTimeT(std::filesystem::file_time_type){return 1700000000;}
} files,*fio=&files;
''' + metadata + getter + select + restore + r'''
auto rowFacts=[](size_t index){return std::map<std::string,std::string>{{"row",std::to_string(index)}};};
int main(){
 // Original and newly described saves use the same extension. Never mutate the supplied bytes.
 namespace thumb=storm::save::thumbnail;
 auto extension=[](const std::string &description){
   std::vector<uint8_t> data(8+description.size()+1+thumb::kBytes);
   auto word=[&](size_t at,uint32_t n){for(int i=0;i<4;++i)data[at+i]=uint8_t(n>>(8*i));};
   word(0,description.size()+1);word(4,thumb::kBytes);
   std::copy(description.begin(),description.end(),data.begin()+8);
   const size_t pixels=8+description.size()+1;data[pixels]=17;data[pixels+1]=83;data[pixels+2]=201;data[pixels+3]=255;
   data.back()=129;return data;
 };
 for(const auto &description:{std::string("old@*@12:00  01.10.1654@1 hours 0 min.@russian"),std::string("new@*@12:01  01.10.1654@1 hours 1 min.@russian@hero@place")}){
   auto bytes=extension(description),original=bytes;const auto pixels=thumb::Pixels(bytes);
   assert(pixels.size()==thumb::kBytes&&pixels[0]==17&&pixels[1]==83&&pixels[2]==201&&pixels[3]==255&&pixels.back()==129&&bytes==original);
   // A picture of another size (a short or long record, another pitch) is dropped and reported; the description
   // stays readable, so the save stays listed and loadable.
   bytes.pop_back();assert(thumb::Pixels(bytes).empty()&&thumb::Decode(bytes).valid&&thumb::Decode(bytes).description==description&&thumb::Decode(bytes).picture_problem);
   bytes=original;bytes.push_back(0);assert(thumb::Pixels(bytes).empty()&&thumb::Decode(bytes).valid&&thumb::Decode(bytes).picture_problem);
   bytes=original;bytes[5]^=1;assert(thumb::Pixels(bytes).empty()&&thumb::Decode(bytes).valid&&thumb::Decode(bytes).description==description&&thumb::Decode(bytes).picture_problem);
   assert(!thumb::Decode(original).picture_problem);
   bytes=original;bytes[4]=0;bytes[5]=0;bytes[6]=0;bytes.resize(8+description.size()+1);assert(thumb::Pixels(bytes).empty()&&thumb::Decode(bytes).valid&&thumb::Decode(bytes).description==description&&!thumb::Decode(bytes).picture_problem); // valid description-only save
   bytes=original;bytes[0]=bytes[1]=bytes[2]=bytes[3]=255;assert(thumb::Pixels(bytes).empty());
   bytes=original;bytes[8+description.size()]=1;assert(thumb::Pixels(bytes).empty()&&!thumb::Decode(bytes).valid); // unterminated description
 }
 assert(thumb::Pixels({}).empty());
 const auto preview=thumb::Source("SAVE\\player\\Сен-Пьер улицы",9,123);
 assert(thumb::Path(preview)=="SAVE\\player\\Сен-Пьер улицы"&&thumb::Written(preview)==123);
 assert(preview!=thumb::Source("SAVE\\other\\Сен-Пьер улицы",9,123)); // same filename, different profile
 assert(preview!=thumb::Source("SAVE\\player\\Сен-Пьер улицы",10,123)&&preview!=thumb::Source("SAVE\\player\\Сен-Пьер улицы",9,124));
 assert(thumb::Source("SAVE\\..\\outside",9,123).empty()&&thumb::Source("SAVE\\player\\../outside",9,123).empty());
 assert(thumb::Path("save-preview/9/123/2e2e/61").empty()&&thumb::Path("save-preview/9/123/61/2f61").empty());
 assert(thumb::Path("save-preview/9/123/61/zz").empty()&&thumb::Path(preview+"/extra").empty());
 assert(!thumb::Written("save-preview/9/99999999999999999999/61/62"));
 // Actual legacy getter body: short/malformed extension must not reach a string scan.
 const std::string legacy="SAVE\\player\\legacy";
 char output[128];
 for(size_t bytes=0;bytes<9;++bytes){
   core.save_data[legacy]=std::vector<uint8_t>(bytes,255);output[0]='x';
   assert(!ReadSaveDescription(legacy.c_str(),sizeof(output),output)&&output[0]==0);
 }
 auto old_record=extension("Сен-Пьер@*@12:00  01.10.1654@1 hours 0 min.@russian");
 core.save_data[legacy]=old_record;
 assert(ReadSaveDescription(legacy.c_str(),sizeof(output),output)&&std::string(output).starts_with("Сен-Пьер"));
 assert(core.save_data[legacy]==old_record); // reader never mutates saved extension
 char small[4];assert(ReadSaveDescription(legacy.c_str(),sizeof(small),small)&&std::string(small)=="С"&&utf8::IsValidUtf8(small));
 char one='x';assert(!ReadSaveDescription(legacy.c_str(),1,&one)&&one==0);
 assert(!ReadSaveDescription(nullptr,sizeof(output),output)&&!ReadSaveDescription(legacy.c_str(),0,output)&&!ReadSaveDescription(legacy.c_str(),2,nullptr));
 old_record.resize(old_record.size()-thumb::kBytes);for(int i=4;i<8;++i)old_record[i]=0;
 core.save_data[legacy]=old_record;assert(ReadSaveDescription(legacy.c_str(),sizeof(output),output)&&std::string(output).starts_with("Сен-Пьер"));
 auto invalid_utf8=extension(std::string("a")+char(255)+"b");core.save_data[legacy]=invalid_utf8;
 assert(ReadSaveDescription(legacy.c_str(),sizeof(output),output)&&utf8::IsValidUtf8(output)&&core.save_data[legacy]==invalid_utf8);
 for(int kind=0;kind<4;++kind){
   auto bad=extension("description");
   if(kind==0)bad[0]=bad[1]=bad[2]=bad[3]=255;
   if(kind==1)bad[8+11]=1; // missing description terminator
   if(kind==2)bad.pop_back();
   if(kind==3)bad.push_back(0);
   core.save_data[legacy]=bad;output[0]='x';const int errors=spdlog::errors;
   if(kind<2)assert(!ReadSaveDescription(legacy.c_str(),sizeof(output),output)&&output[0]==0);
   // A picture of another size: loudly reported, the description (and so loading) kept.
   else assert(ReadSaveDescription(legacy.c_str(),sizeof(output),output)&&std::string(output)=="description");
   assert(spdlog::errors==errors+1);
 }
 core.save_data.erase(legacy);core.reads=0;
 // Actual production metadata cache: one validated extension read, reused on reopen.
 const std::string record="SAVE\\player\\old";
 core.save_data[record]=extension("old@*@12:00  01.10.1654@1 hours 0 min.@russian");
 files.stamps[record]=std::filesystem::file_time_type(std::filesystem::file_time_type::duration(123));
 SaveFactsCache metadata;
 const auto &first=metadata.get(record);assert(first.thumbnail&&first.facts.location=="old"&&core.reads==1);
 const auto source1=thumb::Source(record,9,first.written),source2=thumb::Source(record,10,metadata.get(record).written);
 assert(source1!=source2&&core.reads==1); // URI lifetime changes without rereading metadata
 files.stamps[record]+=std::filesystem::file_time_type::duration(1);
 core.save_data[record]=extension("new@*@12:01  01.10.1654@1 hours 1 min.@russian@hero@place");
 const auto &changed=metadata.get(record);assert(changed.thumbnail&&changed.facts.hero=="hero"&&core.reads==2);
 files.stamps[record]+=std::filesystem::file_time_type::duration(1);
 core.save_data[record]=std::vector<uint8_t>{255,255,255,255,0,0,1,0,'x'};
 const auto &bad=metadata.get(record);assert(!bad.thumbnail&&bad.facts.location.empty()&&core.reads==3); // no legacy unbounded string scan
 files.stamps[record]+=std::filesystem::file_time_type::duration(1);
 auto no_image=extension("text only@*@12:00  01.10.1654@1 hours 0 min.@russian");
 no_image.resize(no_image.size()-thumb::kBytes);for(int i=4;i<8;++i)no_image[i]=0;core.save_data[record]=no_image;
 const auto &description_only=metadata.get(record);assert(!description_only.thumbnail&&description_only.facts.location=="text only"&&core.reads==4);
 core.save_data.erase(record);assert(!metadata.get(record).present&&metadata.get(record).facts.location.empty()&&core.reads==4);
 // A description of the game ("label@faces@time  date@play time@language") and this port's "@hero@location".
 SaveFacts f=ParseSaveDescription("Сен-Пьер магазин 1@face_1,*@13:25  01.10.1654@0 hours 25 min.@russian@Шарль де Мор@Сен-Пьер магазин");
 assert(f.hero=="Шарль де Мор"&&f.location=="Сен-Пьер магазин"&&f.gameDate=="1654-10-01"&&f.gameTime=="13:25"&&f.playMinutes==25);
 f=ParseSaveDescription("Мартиника - море@*@09:05  07.10.1654@2days - 50 hours 3 min.@russian");
 assert(f.hero.empty()&&f.location=="Мартиника - море"&&f.gameDate=="1654-10-07"&&f.playMinutes==50*60+3);
 assert(SavePlayMinutes("1days - 2 hours 0 min.")==26*60&&SavePlayMinutes("garbage")==-1);
 f=ParseSaveDescription("broken");assert(f.location.empty()&&f.playMinutes==-1);
 f=ParseSaveDescription("x@*@99:99  45.13.1654@1 hours 1 min.@russian");assert(f.gameDate.empty()&&f.location=="x");
 // Saving: the current profile's records and the new slot.
 SaveListCapability cap;live.revision=4;live.profile="existing";live.saving=true;
 for(int i=0;i<37;++i)live.files.push_back("Save "+std::to_string(i));
 profiles["existing"]=live.files;
 auto own=[&]{live.records.clear();for(const auto &f:live.files)live.records.push_back({live.profile,f});};own();
 std::vector<MobileWidget> rows;cap.collect(live,true,rows,rowFacts);
 assert(rows.size()==39&&rows[0].node=="ANDROID_SAVES"&&rows[0].value=="37");rows.erase(rows.begin()); // the list, then the records
 assert(rows[37].unavailableReason=="new"&&rows[37].context.at("row")=="37"&&rows[3].context.at("row")=="3");
 auto token=[&](int index,const char *verb){return std::string(verb)+"@4:"+rows[index].rowId;};
 auto target=cap.accept(live,31,token(31,"delete"));assert(target);assert(SelectSaveTarget(*target));assert(page.value==30&&selection.value==31);
 selection.Set(34); // Delayed 350/500ms initial selection callback while confirmation is open.
 assert(RestoreSaveTarget(*target));assert(selection.value==31);
 live.records[31].file="replacement";assert(!RestoreSaveTarget(*target));assert(!cap.accept(live,31,token(31,"delete")));own();
 live.revision=5;assert(!RestoreSaveTarget(*target));live.revision=4;
 target=cap.accept(live,0,token(0,"activate"));assert(target&&SelectSaveTarget(*target)&&page.value==0&&selection.value==0);
 target=cap.accept(live,37,token(37,"activate"));assert(target&&SelectSaveTarget(*target)&&page.value==35&&selection.value==37);
 assert(!cap.accept(live,37,token(37,"delete")));assert(!cap.accept(live,2,token(1,"delete")));
 assert(switches==0);
 rows.clear();cap.collect(live,false,rows,rowFacts);for(const auto &row:rows)assert(!row.enabled&&row.actions.empty());
 // Loading: every profile's records in one list, newest first; no profile is shown or chosen.
 profiles["old build"]={"Old 0","Old 1","Old 2"};
 live.saving=false;live.revision=6;selection.Set(0);live.records={{"old build","Old 0"},{"existing","Save 0"},{"old build","Old 1"},{"existing","Save 1"},{"old build","Old 2"}};
 rows.clear();cap.collect(live,true,rows,rowFacts);rows.erase(rows.begin());
 assert(rows.size()==5&&rows[0].label=="Old 0"&&rows[1].label=="Save 0"&&rows[1].value=="1"&&rows[0].value=="0");
 auto token6=[&](int index,const char *verb){return std::string(verb)+"@6:"+rows[index].rowId;};
 // A record of another profile: that profile becomes current, then its slot is clicked.
 target=cap.accept(live,2,token6(2,"activate"));assert(target&&SelectSaveTarget(*target));
 assert(switches==1&&live.profile=="old build"&&selection.value==1);
 assert(RestoreSaveTarget(*target)&&selection.value==1);
 // Its own profile current again (the list is idle): the same rows, a record of it, no switch back needed.
 ChooseSaveProfile("existing");switches=0;
 target=cap.accept(live,3,token6(3,"delete"));assert(target&&SelectSaveTarget(*target)&&switches==0&&selection.value==1);
 assert(cap.accept(live,4,token6(4,"delete"))&&!cap.accept(live,5,token6(4,"activate")));
 // The game's profile name is spelled unlike its folder ("Player" for the folder "player",
 // the game's paths fold ASCII case): its records are its own, no switch, the same slot.
 assert(SameProfile("Player","player")&&!SameProfile("Player","player2")&&!SameProfile("Плеер","плеер"));
 live.profile="EXISTING";switches=0;
 target=cap.accept(live,3,token6(3,"delete"));assert(target&&SelectSaveTarget(*target)&&switches==0&&selection.value==1);
 assert(RestoreSaveTarget(*target));live.profile="existing";
 // One SetCurrentProfile per opening (each runs interface.c LoadGameOptions): frames of an
 // opening, counting the game's own (no chooser) and the list's (through the chooser).
 auto opening=[&](bool chooser,const std::string &game,bool listed,const std::string &newest){
  int sets=chooser?0:1; std::optional<std::string> current=chooser?std::nullopt:std::optional<std::string>(game);
  for(int frame=0;frame<5;++frame){
   const std::string target=ProfileToChoose(chooser,game,listed,newest,current);
   if(!target.empty()){++sets;current=target;chooser=false;}
  }
  return std::make_pair(sets,current);
 };
 auto run=opening(false,"player",false,"QA_Skip");assert(run.first==1&&run.second=="player"); // no folder: it stays current
 run=opening(false,"player",true,"QA_Skip");assert(run.first==1&&run.second=="player");
 run=opening(true,"player",true,"QA_Skip");assert(run.first==1&&run.second=="player");        // the chooser answered once
 run=opening(true,"player",false,"QA_Skip");assert(run.first==1&&run.second=="QA_Skip");      // with the newest record's
 run=opening(true,"player",false,"");assert(run.first==0);                                      // no record: nothing to choose
 // After a record of another profile was deleted the opening's profile is made current again, once.
 assert(ProfileToChoose(false,"player",true,"x",std::string("QA_Skip"))=="player"&&ProfileToChoose(false,"Player",true,"x",std::string("player")).empty());
 assert(ProfileToChoose(false,"player",false,"x",std::string("QA_Skip")).empty()&&ProfileToChoose(false,"player",true,"x",std::nullopt).empty());
 // A record whose profile folder is gone cannot be chosen.
 live.records.push_back({"vanished","X"});rows.clear();cap.collect(live,true,rows,rowFacts);rows.erase(rows.begin());
 auto token7=[&](int index,const char *verb){return std::string(verb)+"@6:"+rows[index].rowId;};
 target=cap.accept(live,5,token7(5,"activate"));assert(target&&!SelectSaveTarget(*target));
 // No records at all: the empty list.
 live.records.clear();live.files.clear();rows.clear();cap.collect(live,true,rows,rowFacts);assert(rows.size()==1&&rows[0].node=="ANDROID_SAVES"&&rows[0].value=="0");
 std::cout<<"PASS record facts, every profile in one list, records of other profiles chosen through the game, stable identity, distant/earlier/new slot selection, delayed callback restore, profile/list/revision guards, profile names compared the game's way (ASCII case), one SetCurrentProfile per opening\n";
}
'''
with tempfile.TemporaryDirectory(prefix='mobile-saves-') as directory:
    p=Path(directory);(p/'check.cpp').write_text(code)
    subprocess.run([os.environ.get('CXX','c++'),'-std=c++20','-g','-fsanitize=address,undefined','-fno-sanitize-recover=all','-I'+str(root/'src/libs/core/include'),'-I'+str(root/'src/libs/util/include'),str(p/'check.cpp'),'-o',str(p/'check')],check=True)
    subprocess.run([str(p/'check')],check=True)
