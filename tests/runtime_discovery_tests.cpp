// SPDX-License-Identifier: MIT
// Copyright (c) 2026 itsloopyo
#include "builds/runtime_discovery.hpp"
#include <algorithm>
#include <cstring>
#include <vector>
#include <initializer_list>
#include <iostream>
int checks=0,failures=0;
#define CHECK(x) do { ++checks; if(!(x)){++failures;std::cerr<<__LINE__<<": "<<#x<<"\n";} } while(false)
namespace {

struct Fixture {
    std::uint32_t shift;
    std::uintptr_t base;
    std::vector<std::uint8_t> bytes;

    template<class T> void Write(std::uint32_t at, T value) {
        std::memcpy(bytes.data() + at, &value, sizeof(value));
    }

    void Code(std::uint32_t at, std::initializer_list<std::uint8_t> code) {
        std::copy(code.begin(), code.end(), bytes.begin() + at + shift);
    }

    void String(std::uint32_t at, const char* value, bool wide = false) {
        do {
            bytes[at++ + shift] = static_cast<std::uint8_t>(*value);
            if (wide) bytes[at++ + shift] = 0;
        } while (*value++);
    }

    void Pointer(std::uint32_t at, std::uint32_t target) {
        Write<std::uint64_t>(at + shift, base + shift + target);
    }

    void Relative(std::uint32_t at, std::uint32_t target) {
        Write<std::int32_t>(at + shift, static_cast<std::int32_t>(target) - static_cast<std::int32_t>(at) - 4);
    }

    Fixture(std::uint32_t displacement = 0, std::uintptr_t imageBase = 0x140000000)
        : shift(displacement), base(imageBase), bytes(0x30000 + shift) {
        if (!base) base=reinterpret_cast<std::uintptr_t>(bytes.data());
        Write<std::uint16_t>(0, 0x5a4d);
        Write<std::uint32_t>(0x3c, 0x80);
        Write<std::uint32_t>(0x80, 0x4550);
        Write<std::uint16_t>(0x84, 0x8664);
        Write<std::uint16_t>(0x86, 4);
        Write<std::uint16_t>(0x94, 240);
        Write<std::uint16_t>(0x98, 0x20b);
        Write<std::uint32_t>(0x98 + 56, static_cast<std::uint32_t>(bytes.size()));
        struct Section { const char* name; std::uint32_t rva, size, flags; };
        const Section sections[] = {
            {".text", 0x1000, 0x2000, 0x60000000},
            {".rdata", 0x4000, 0x3000, 0x40000000},
            {".data", 0x8000, 0x20000, 0xc0000000},
            {".pdata", 0x29000, 0x1000, 0x40000000},
        };
        std::uint32_t header = 0x188;
        for (const auto& section : sections) {
            std::memcpy(bytes.data() + header, section.name, std::strlen(section.name));
            Write<std::uint32_t>(header + 8, section.size);
            Write<std::uint32_t>(header + 12, section.rva + shift);
            Write<std::uint32_t>(header + 36, section.flags);
            header += 40;
        }
        const std::uint32_t functions[][2] = {
            {0x1100,0x1180}, {0x1200,0x1280}, {0x1300,0x1400},
            {0x1400,0x1500}, {0x1600,0x1700}, {0x1900,0x1980},
        };
        std::uint32_t record = 0x29000 + shift;
        for (const auto& fn : functions) {
            Write<std::uint32_t>(record, fn[0] + shift);
            Write<std::uint32_t>(record + 4, fn[1] + shift);
            Write<std::uint32_t>(record + 8, 0x6800 + shift);
            record += 12;
        }
        Code(0x6800, {1,0,0,0});
        Write<std::uint32_t>(0x29000 + 12 + 8 + shift, 0x6810 + shift);
        Code(0x6810, {0x21,0,0,0});
        Write<std::uint32_t>(0x6814 + shift, 0x1100 + shift);
        Write<std::uint32_t>(0x98 + 112 + 24, 0x29000 + shift);
        Write<std::uint32_t>(0x98 + 112 + 28, record - 0x29000 - shift);
        unsigned n=0;
        for(auto name:{"None","ByteProperty","IntProperty","BoolProperty","FloatProperty","ObjectProperty","NameProperty"}){
            String(0x4000+n*40,name,true);
            Code(0x1100+n*16,{0x48,0x8d,0x15,0,0,0,0});Relative(0x1103+n*16,0x4000+n*40);++n;
        }
        Code(0x1200,{0x48,0x8b,0x3d,0,0,0,0});Relative(0x1203,0x8000);
        String(0x4400,"GameViewport",true);
        Code(0x1400,{0x48,0x8d,0x15,0,0,0,0});Relative(0x1403,0x4400);
        Code(0x1411,{0x4c,0x8b,0x05,0,0,0,0});Relative(0x1414,0x8010);
        Code(0x1300,{0x48,0x8b,0xf9,0x44,0x0f,0x28,0xd1});
        Code(0x1320,{0x48,0x8b,0x07,0x48,0x8d,0x97,0,0,0,0,0x41,0x0f,0x28,0xd2,0x48,0x8b,0xcf,0xff,0x90,0x38,6,0,0});
        Write<std::uint32_t>(0x1326+shift,0x900);
        for(unsigned i=0;i<2;++i){
            auto at=0x1340+i*32;
            Code(at,{0xf2,0x0f,0x10,0x87,0,0,0,0,0x8b,0x87,0,0,0,0,
                0xf2,0x0f,0x11,0x87,0,0,0,0,0x89,0x87,0,0,0,0});
            Write<std::uint32_t>(at+4+shift,0x908+i*12);
            Write<std::uint32_t>(at+10+shift,0x910+i*12);
            Write<std::uint32_t>(at+18+shift,0x208+i*12);
            Write<std::uint32_t>(at+24+shift,0x210+i*12);
        }
    }
    ueht::builds::ImageView View(){return {bytes.data(),bytes.size(),base};}
};
}
int main(){
    using namespace ueht::builds;
    for(auto shift:{0u,0x1000u}){
        Fixture f(shift);Bootstrap b{};std::string reason;
        CHECK(DiscoverBootstrap(f.View(),b,reason));CHECK(b.names==0x8000+shift);CHECK(b.engineClass==0x8010+shift);
        CameraLayout c{0x208,0x214,0x914,0x900,0x908,0x914};
        CHECK(DiscoverCameraTarget(f.View(),c,reason));CHECK(c.target==0x1300+shift);
        ++c.viewLocation;CHECK(!DiscoverCameraTarget(f.View(),c,reason));CHECK(c.target==0);
    }
    for(auto at:{0x0u,0x84u,0x98u,0x4000u,0x4400u,0x1203u,0x1411u,0x6814u}){
        Fixture f;f.bytes[at]^=1;Bootstrap b{1,2};std::string reason;
        CHECK(!DiscoverBootstrap(f.View(),b,reason));CHECK(b.names==0 && b.engineClass==0);
    }
    for(auto at:{0x1300u,0x1303u,0x1326u,0x132au,0x1340u,0x134au,0x1352u,0x1358u,0x1360u,0x136au,0x1372u,0x1378u}){
        Fixture f;f.bytes[at]^=1;std::string reason;CameraLayout c{0x208,0x214,0x914,0x900,0x908,0x914};
        CHECK(!DiscoverCameraTarget(f.View(),c,reason));CHECK(c.target==0);
    }
    {
        Fixture f;std::copy_n(f.bytes.begin()+0x1300,0x80,f.bytes.begin()+0x1900);
        std::string reason;CameraLayout c{0x208,0x214,0x914,0x900,0x908,0x914};CHECK(!DiscoverCameraTarget(f.View(),c,reason));
    }
    for(auto size:{0u,64u,0x100u,0x188u,0x29004u}){
        Fixture f;f.bytes.resize(size);Bootstrap b{};std::string reason;CHECK(!DiscoverBootstrap(f.View(),b,reason));
    }
    std::cout<<checks<<" checks, "<<failures<<" failures\n";return failures?1:0;
}
