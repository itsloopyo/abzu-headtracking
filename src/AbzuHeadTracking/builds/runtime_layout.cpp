// SPDX-License-Identifier: MIT
// Copyright (c) 2026 itsloopyo
#include "runtime_discovery.hpp"
#include "build_profile.hpp"
#include "utility/SafeMemory.hpp"
#include "utility/Logging.hpp"
#include <vector>
#include <set>
#include <stdexcept>
#include <cstring>

namespace ueht::builds {
namespace {
std::vector<std::uint8_t> image;
std::uintptr_t base=0;
Bootstrap bootstrap{};
bool known=false;
class Unavailable: public std::runtime_error { public: using std::runtime_error::runtime_error; };
void Need(bool value,const char* why){if(!value)throw Unavailable(why);}
template<class T>T Read(std::uintptr_t at){T v{};Need(SafeRead(at,v),"live metadata is unreadable");return v;}
std::string Name(std::uint32_t id) {
    auto pool=Read<std::uintptr_t>(base+bootstrap.names);
    Need(pool!=0,"name table is not initialized");
    auto count=Read<std::uint32_t>(pool+0x400), chunks=Read<std::uint32_t>(pool+0x404);
    Need(count && count<=128*16384 && chunks && chunks<=128 && id<count && id/16384<chunks,"invalid name table bounds");
    auto chunk=Read<std::uintptr_t>(pool+(id/16384)*8);
    auto entry=Read<std::uintptr_t>(chunk+(id%16384)*8);
    auto header=Read<std::uint32_t>(entry);
    Need((header>>1)==id,"name entry index mismatch");
    std::string result;
    for(unsigned i=0;i<256;++i){
        auto ch=(header&1)?Read<std::uint16_t>(entry+16+i*2):Read<std::uint8_t>(entry+16+i);
        if(!ch)return result;
        Need(ch<128,"non-ASCII metadata name");result+=static_cast<char>(ch);
    }
    throw Unavailable("unterminated metadata name");
}
std::string ObjectName(std::uintptr_t p){Need(p!=0,"null metadata object");return Name(Read<std::uint32_t>(p+0x18));}
std::uintptr_t Class(std::uintptr_t p){return Read<std::uintptr_t>(p+0x10);}
bool NamedType(std::uintptr_t type,const char* name,const char* kind,const char* package) {
    return ObjectName(type)==name && ObjectName(Class(type))==kind &&
        ObjectName(Read<std::uintptr_t>(type+0x20))==package;
}
std::uintptr_t Owner(std::uintptr_t cls,const char* name) {
    std::set<std::uintptr_t> seen;
    for(unsigned i=0;cls && i<32;++i){
        Need(seen.insert(cls).second,"class inheritance cycle");
        if(NamedType(cls,name,"Class","/Script/Engine"))return cls;
        cls=Read<std::uintptr_t>(cls+0x30);
    }
    throw Unavailable("required Engine class missing");
}
struct Property { std::uint32_t offset,size; std::uintptr_t inner; };
Property Field(std::uintptr_t owner,const char* name,const char* kind,std::uint32_t width=0) {
    auto bound=Read<std::uint32_t>(owner+0x40);
    Need(bound && bound<0x100000,"invalid owner size");
    auto field=Read<std::uintptr_t>(owner+0x38);
    std::set<std::uintptr_t> seen;Property found{};unsigned count=0;
    for(unsigned i=0;field && i<4096;++i){
        Need(seen.insert(field).second,"property chain cycle");
        if(_stricmp(ObjectName(field).c_str(),name)==0){
            Need(ObjectName(Class(field))==kind,"property kind mismatch");
            Need(Read<std::uint32_t>(field+0x30)==1,"property array dimension mismatch");
            auto size=Read<std::uint32_t>(field+0x34), offset=Read<std::uint32_t>(field+0x50);
            Need(size && (!width || size==width) && offset<bound && size<=bound-offset,"property width or bounds mismatch");
            found={offset,size,0};++count;
            if(std::strcmp(kind,"StructProperty")==0){
                found.inner=Read<std::uintptr_t>(field+0x78);
                Need(found.inner && Read<std::uint32_t>(found.inner+0x40)==size,"inner struct width mismatch");
            }
        }
        field=Read<std::uintptr_t>(field+0x28);
    }
    Need(!field && count==1,"required property absent or ambiguous");return found;
}
std::uintptr_t Follow(std::uintptr_t instance,const char* owner,const char* member,std::uint32_t& offset) {
    auto field=Field(Owner(Class(instance),owner),member,"ObjectProperty",8);
    offset=field.offset;
    auto value=Read<std::uintptr_t>(instance+offset);
    Need(value!=0,"player camera chain is not initialized");return value;
}
void Scalar(std::uintptr_t structure,const char* name,std::uint32_t offset) {
    Need(Field(structure,name,"FloatProperty",4).offset==offset,"camera scalar layout mismatch");
}
void ViewFields(std::uintptr_t view) {
    Need(NamedType(view,"MinimalViewInfo","ScriptStruct","/Script/Engine"),"unexpected camera view struct");
    auto loc=Field(view,"Location","StructProperty",12), rot=Field(view,"Rotation","StructProperty",12);
    Need(loc.offset==0 && rot.offset==12 && Field(view,"FOV","FloatProperty",4).offset==24,"camera view member layout mismatch");
    Need(NamedType(loc.inner,"Vector","ScriptStruct","/Script/CoreUObject") &&
         NamedType(rot.inner,"Rotator","ScriptStruct","/Script/CoreUObject"),"camera vector/rotator identity mismatch");
    Scalar(loc.inner,"X",0);Scalar(loc.inner,"Y",4);Scalar(loc.inner,"Z",8);
    Scalar(rot.inner,"Pitch",0);Scalar(rot.inner,"Yaw",4);Scalar(rot.inner,"Roll",8);
}
std::uintptr_t Engine(std::uintptr_t cls,std::uint32_t viewportOffset) {
    auto nt=*reinterpret_cast<const std::uint32_t*>(image.data()+0x3c);
    auto sections=nt+24+*reinterpret_cast<const std::uint16_t*>(image.data()+nt+20);
    auto count=*reinterpret_cast<const std::uint16_t*>(image.data()+nt+6);
    std::set<std::uintptr_t> engines;
    for(unsigned i=0;i<count;++i){
        auto header=sections+i*40;
        if(!(*reinterpret_cast<const std::uint32_t*>(image.data()+header+36)&0x80000000))continue;
        auto rva=*reinterpret_cast<const std::uint32_t*>(image.data()+header+12);
        auto size=*reinterpret_cast<const std::uint32_t*>(image.data()+header+8);
        for(std::uint32_t at=0;at+8<=size;at+=8){
            std::uintptr_t obj=0,type=0,viewport=0;
            if(!SafeRead(base+rva+at,obj) || obj<0x10000 || obj%8 || !SafeRead(obj+0x10,type))continue;
            for(unsigned depth=0;type && depth<32;++depth){
                if(type==cls){
                    if(SafeRead(obj+viewportOffset,viewport) && viewport)engines.insert(obj);
                    break;
                }
                std::uintptr_t next=0;
                if(!SafeRead(type+0x30,next) || next==type)break;
                type=next;
            }
        }
    }
    Need(engines.size()==1,"live engine absent or ambiguous");return *engines.begin();
}
}
void StartRuntimeDiscovery(ImageView view,const Bootstrap& found,bool exact) {
    image.assign(view.data,view.data+view.size);base=view.base;bootstrap=found;known=exact;
}
void ResetRuntimeDiscovery(){image.clear();base=0;bootstrap={};known=false;}
bool RuntimeDiscoveryActive(){return !image.empty();}
std::uintptr_t ResolveRuntimeCamera(CameraLayout& out) {
    try {
        Need(Name(0)=="None","name table sentinel mismatch");
        auto engineClass=Read<std::uintptr_t>(base+bootstrap.engineClass);
        Need(NamedType(engineClass,"Engine","Class","/Script/Engine"),"engine class identity mismatch");
        auto viewportField=Field(engineClass,"GameViewport","ObjectProperty",8);
        auto engine=Engine(engineClass,viewportField.offset);
        std::uint32_t viewport=0,instance=0,players=0,controller=0,manager=0;
        auto vp=Follow(engine,"Engine","GameViewport",viewport);
        auto gi=Follow(vp,"GameViewportClient","GameInstance",instance);
        auto array=Field(Owner(Class(gi),"GameInstance"),"LocalPlayers","ArrayProperty",16);
        players=array.offset;
        auto data=Read<std::uintptr_t>(gi+players);
        auto count=Read<std::int32_t>(gi+players+8), capacity=Read<std::int32_t>(gi+players+12);
        Need(data && count>0 && count<=4 && capacity>=count && capacity<=16,"local player array not ready or invalid");
        auto player=Read<std::uintptr_t>(data);
        auto pc=Follow(player,"Player","PlayerController",controller);
        auto pcm=Follow(pc,"PlayerController","PlayerCameraManager",manager);
        auto pcmClass=Owner(Class(pcm),"PlayerCameraManager");
        auto cache=Field(pcmClass,"CameraCache","StructProperty"), target=Field(pcmClass,"ViewTarget","StructProperty");
        Need(NamedType(cache.inner,"CameraCacheEntry","ScriptStruct","/Script/Engine") &&
             NamedType(target.inner,"TViewTarget","ScriptStruct","/Script/Engine"),"camera cache struct identity mismatch");
        auto cachePov=Field(cache.inner,"POV","StructProperty"), targetPov=Field(target.inner,"POV","StructProperty");
        Need(cachePov.inner==targetPov.inner,"camera cache view types disagree");ViewFields(cachePov.inner);
        CameraLayout found{cache.offset+cachePov.offset,cache.offset+cachePov.offset+12,
            target.offset+targetPov.offset+12,target.offset,target.offset+targetPov.offset,target.offset+targetPov.offset+12,0,0};
        std::string reason;
        Need(DiscoverCameraTarget({image.data(),image.size(),base},found,reason),reason.c_str());
        auto table=Read<std::uintptr_t>(pcm);
        Need(table>=base && table-base<image.size()-256*8,"camera vtable outside image");
        unsigned hits=0;
        for(unsigned slot=0;slot<256;++slot)if(Read<std::uintptr_t>(table+slot*8)==base+found.target){found.slot=slot;++hits;}
        Need(hits==1,"UpdateCamera vtable entry absent or ambiguous");
        if(known){
            const auto& old=kSteamProfile_20201114.offsets;
            Need(bootstrap.engineClass==old.uengine_class_rva && viewport==old.engine_to_game_viewport &&
                 instance==old.game_viewport_to_game_instance && players==old.game_instance_to_local_players &&
                 controller==old.local_player_to_player_controller && manager==old.player_controller_to_camera_manager &&
                 found.slot==196 && found.location==0x3f8 && found.rotation==0x404 && found.secondary==0xbd4,
                 "discovery disagrees with exact historical layout");
        }
        out=found;
        UEHT_LOG(Info,"discovery: live camera validated; UpdateCamera RVA=0x%X slot=%u location=0x%X rotation=0x%X secondary=0x%X",
                 found.target,found.slot,found.location,found.rotation,found.secondary);
        return pcm;
    }catch(const Unavailable& e){
        static std::uint64_t next=0;auto now=GetTickCount64();
        if(now>=next){next=now+5000;UEHT_LOG(Info,"discovery: waiting for validated live camera: %s",e.what());}
        return 0;
    }
}
}
