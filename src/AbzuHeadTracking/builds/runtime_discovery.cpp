// SPDX-License-Identifier: MIT
// Copyright (c) 2026 itsloopyo

#include "runtime_discovery.hpp"


#include <algorithm>
#include <cstring>
#include <initializer_list>
#include <map>
#include <set>
#include <stdexcept>
#include <vector>

namespace ueht::builds {
namespace {

class Rejected : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

void Require(bool condition, const char* reason) {
    if (!condition) throw Rejected(reason);
}

struct Range {
    std::uint32_t begin;
    std::uint32_t size;

    bool Contains(std::uint64_t at, std::size_t length = 1) const {
        return at >= begin && length <= size && at - begin <= size - length;
    }
};

class Image {
public:
    explicit Image(ImageView view) : view_(view) {
        Require(Read<std::uint16_t>(0) == 0x5a4d, "missing DOS header");
        const auto nt = Read<std::uint32_t>(0x3c);
        Require(Read<std::uint32_t>(nt) == 0x4550 &&
                Read<std::uint16_t>(nt + 4) == 0x8664 &&
                Read<std::uint16_t>(nt + 24) == 0x20b, "expected an x64 PE image");
        Require(Read<std::uint32_t>(nt + 24 + 56) == view.size,
                "PE image size disagrees with the captured module");
        const auto count = Read<std::uint16_t>(nt + 6);
        const auto optionalSize = Read<std::uint16_t>(nt + 20);
        Require(optionalSize >= 160 && count > 0 && count <= 96, "invalid PE section table");
        const std::uint64_t sections = static_cast<std::uint64_t>(nt) + 24 + optionalSize;
        for (unsigned i = 0; i < count; ++i) {
            const auto header = sections + i * 40;
            Bounds(header, 40);
            char name[9]{};
            std::memcpy(name, view.data + header, 8);
            Range range{Read<std::uint32_t>(header + 12), Read<std::uint32_t>(header + 8)};
            Bounds(range.begin, range.size);
            Require(sections_.emplace(name, range).second, "duplicate PE section name");
            flags_.emplace(name, Read<std::uint32_t>(header + 36));
        }
        text = Section(".text", 0x20000000);
        rdata = Section(".rdata", 0x40000000);
        writable = Section(".data", 0x80000000);
        const auto pdata = Section(".pdata", 0x40000000);
        const auto exceptionRva = Read<std::uint32_t>(nt + 24 + 112 + 3 * 8);
        const auto exceptionSize = Read<std::uint32_t>(nt + 24 + 112 + 3 * 8 + 4);
        Require(exceptionSize && exceptionSize % 12 == 0 &&
                pdata.Contains(exceptionRva, exceptionSize), "invalid PE exception directory");
        for (std::uint32_t i = 0; i < exceptionSize; i += 12) {
            const auto begin = Read<std::uint32_t>(exceptionRva + i);
            const auto end = Read<std::uint32_t>(exceptionRva + i + 4);
            Require(end > begin && end <= view.size, "invalid function extent");
            Require(functions_.empty() || functions_.back().begin < begin,
                    "unsorted PE exception directory");
            functions_.push_back({begin, end - begin});
            unwind_[begin] = Read<std::uint32_t>(exceptionRva + i + 8);
        }
    }

    template<class T> T Read(std::uint64_t at) const {
        Bounds(at, sizeof(T));
        T value;
        std::memcpy(&value, view_.data + at, sizeof(T));
        return value;
    }

    std::uint32_t Relative(std::uint32_t displacement) const {
        const auto value = static_cast<std::int64_t>(displacement) + 4 + Read<std::int32_t>(displacement);
        Require(value >= 0 && static_cast<std::uint64_t>(value) < view_.size,
                "relative instruction target is outside the module");
        return static_cast<std::uint32_t>(value);
    }

    Range Function(std::uint32_t at) const {
        auto it = std::upper_bound(functions_.begin(), functions_.end(), at,
            [](std::uint32_t address, Range range) { return address < range.begin; });
        if (it == functions_.begin() || !(--it)->Contains(at)) return {};
        return *it;
    }

    std::uint32_t Root(std::uint32_t at) const {
        auto fn = Function(at);
        Require(fn.size != 0, "instruction has no unwind function");
        std::set<std::uint32_t> visited;
        for (;;) {
            Require(visited.insert(fn.begin).second, "cyclic chained unwind data");
            const auto u = unwind_.at(fn.begin);
            Require((Read<std::uint8_t>(u) & 7) == 1 || (Read<std::uint8_t>(u) & 7) == 2,
                    "unsupported unwind information version");
            const auto flags = Read<std::uint8_t>(u) >> 3;
            if (!(flags & 4)) return fn.begin;
            Require((flags & 3) == 0, "invalid chained unwind flags");
            const auto count = Read<std::uint8_t>(u + 2);
            const auto parent = Read<std::uint32_t>(u + 4 + ((count + 1u) & ~1u) * 2);
            fn = Function(parent);
            Require(fn.size && fn.begin == parent, "invalid chained unwind parent");
        }
    }

    std::vector<Range> Parts(std::uint32_t root) const {
        if (!partsIndexed_) {
            for (auto fn : functions_) parts_[Root(fn.begin)].push_back(fn);
            partsIndexed_ = true;
        }
        const auto found = parts_.find(root);
        return found == parts_.end() ? std::vector<Range>{} : found->second;
    }

    std::vector<std::uint32_t> Find(Range range, std::initializer_list<int> pattern) const {
        std::vector<std::uint32_t> hits;
        if (pattern.size() > range.size) return hits;
        const auto last = range.begin + range.size - pattern.size();
        for (std::uint32_t at = range.begin; at <= last; ++at) {
            std::size_t index = 0;
            for (const int byte : pattern) {
                if (byte >= 0 && view_.data[at + index] != byte) break;
                ++index;
            }
            if (index == pattern.size()) hits.push_back(at);
        }
        return hits;
    }

    std::vector<std::uint32_t> Strings(Range range, const char* name, bool wide = false) const {
        std::vector<std::uint8_t> bytes;
        do {
            bytes.push_back(static_cast<std::uint8_t>(*name));
            if (wide) bytes.push_back(0);
        } while (*name++);
        std::vector<std::uint32_t> hits;
        auto cursor = view_.data + range.begin;
        const auto end = cursor + range.size;
        while (cursor < end) {
            const auto found = std::search(cursor, end, bytes.begin(), bytes.end());
            if (found == end) break;
            const auto at = static_cast<std::uint32_t>(found - view_.data);
            if (at == range.begin || Read<std::uint8_t>(at - 1) == 0) hits.push_back(at);
            cursor = found + 1;
        }
        return hits;
    }

    Range text{}, rdata{}, writable{};

    std::vector<std::uint32_t> NamedEntries(const char* name, bool wide = false) const {
        const auto strings = Strings(rdata, name, wide);
        const std::set<std::uint32_t> names(strings.begin(), strings.end());
        std::vector<std::uint32_t> entries;
        for (auto at = rdata.begin; rdata.Contains(at, 16); at += 8) {
            const auto pointer = Read<std::uint64_t>(at);
            if (pointer >= view_.base && pointer - view_.base <= UINT32_MAX &&
                names.count(static_cast<std::uint32_t>(pointer - view_.base))) entries.push_back(at);
        }
        return entries;
    }

    bool Starts(std::uint32_t at, std::initializer_list<int> pattern) const {
        return text.Contains(at, pattern.size()) &&
               !Find({at, static_cast<std::uint32_t>(pattern.size())}, pattern).empty();
    }

private:
    void Bounds(std::uint64_t at, std::size_t length) const {
        Require(at <= view_.size && length <= view_.size - at, "read extends beyond the captured image");
    }

    Range Section(const char* name, std::uint32_t requiredFlag) const {
        const auto found = sections_.find(name);
        Require(found != sections_.end() && found->second.size != 0, "required PE section is missing");
        Require((flags_.at(name) & requiredFlag) != 0, "unexpected PE section protection");
        return found->second;
    }

    mutable bool partsIndexed_ = false;
    mutable std::map<std::uint32_t, std::vector<Range>> parts_;
    ImageView view_;
    std::map<std::string, Range> sections_;
    std::map<std::string, std::uint32_t> flags_;
    std::vector<Range> functions_;
    std::map<std::uint32_t, std::uint32_t> unwind_;
};

std::uint32_t Unique(const std::set<std::uint32_t>& values, const char* reason) {
    Require(values.size() == 1, reason);
    return *values.begin();
}

std::map<std::uint32_t,std::set<std::uint32_t>> References(const Image& image) {
    std::map<std::uint32_t,std::set<std::uint32_t>> refs;
    for(auto prefix:{0x48,0x4c}) for(auto reg:{0x05,0x0d,0x15,0x1d,0x25,0x2d,0x35,0x3d})
        for(auto at:image.Find(image.text,{prefix,0x8d,reg,-1,-1,-1,-1}))
        {
            const auto target=static_cast<std::int64_t>(at)+7+image.Read<std::int32_t>(at+3);
            if(target>=0 && target<=UINT32_MAX)refs[static_cast<std::uint32_t>(target)].insert(at);
        }
    return refs;
}
Bootstrap BootstrapFrom(const Image& image) {
    auto refs=References(image);
    std::map<std::uint32_t,unsigned> candidates;
    for(auto name:{"None","ByteProperty","IntProperty","BoolProperty","FloatProperty","ObjectProperty","NameProperty"}) {
        std::set<std::uint32_t> roots;
        for(auto str:image.Strings(image.rdata,name,true))
            for(auto at:refs[str]) if(image.Function(at).size) roots.insert(image.Root(at));
        for(auto root:roots) ++candidates[root];
    }
    std::set<std::uint32_t> constructors;
    for(auto item:candidates) if(item.second==7) constructors.insert(item.first);
    const auto constructor=Unique(constructors,"name constructor absent or ambiguous");
    std::set<std::uint32_t> names;
    for(auto part:image.Parts(constructor))
        for(auto at:image.Find(part,{0x48,0x8b,0x3d,-1,-1,-1,-1})) {
            auto target=image.Relative(at+3);
            if(image.writable.Contains(target,8)) names.insert(target);
        }
    std::set<std::uint32_t> classes;
    for(auto str:image.Strings(image.rdata,"GameViewport",true)) for(auto at:refs[str]) {
        auto fn=image.Function(at);
        if(!fn.Contains(at,24) || !image.Starts(at+17,{0x4c,0x8b,0x05,-1,-1,-1,-1}))continue;
        auto target=image.Relative(at+20);
        if(image.writable.Contains(target,8))classes.insert(target);
    }
    return {Unique(names,"name storage absent or ambiguous"),Unique(classes,"Engine class storage absent or ambiguous")};
}
std::uint32_t CameraTarget(const Image& image,const CameraLayout& layout) {
    std::set<std::uint32_t> roots;
    for(auto at:image.Find(image.text,{0x48,0x8b,0xf9})) {
        auto fn=image.Function(at);
        if(!fn.size)continue;
        auto root=image.Root(at);
        if(at-root>0x80)continue;
        bool delta=false,dispatch=false;
        std::set<std::uint32_t> reads,writes;
        for(auto part:image.Parts(root)) {
            delta |= !image.Find(part,{0x44,0x0f,0x28,0xd1}).empty();
            for(auto call:image.Find(part,{0x48,0x8b,0x07,0x48,0x8d,0x97,-1,-1,-1,-1,0x41,0x0f,0x28,0xd2,0x48,0x8b,0xcf,0xff,0x90,-1,-1,-1,-1})) {
                auto slot=image.Read<std::uint32_t>(call+19);
                dispatch |= image.Read<std::uint32_t>(call+6)==layout.viewTarget && slot>=0x100 && slot<0x1000 && slot%8==0;
            }
            for(auto x:image.Find(part,{0xf2,0x0f,0x10,0x87,-1,-1,-1,-1}))reads.insert(image.Read<std::uint32_t>(x+4));
            for(auto x:image.Find(part,{0x8b,0x87,-1,-1,-1,-1}))reads.insert(image.Read<std::uint32_t>(x+2)|0x80000000u);
            for(auto x:image.Find(part,{0xf2,0x0f,0x11,0x87,-1,-1,-1,-1}))writes.insert(image.Read<std::uint32_t>(x+4));
            for(auto x:image.Find(part,{0x89,0x87,-1,-1,-1,-1}))writes.insert(image.Read<std::uint32_t>(x+2)|0x80000000u);
        }
        if(delta && dispatch && reads.count(layout.viewLocation) && reads.count((layout.viewLocation+8)|0x80000000u) &&
           reads.count(layout.viewRotation) && reads.count((layout.viewRotation+8)|0x80000000u) &&
           writes.count(layout.location) && writes.count((layout.location+8)|0x80000000u) &&
           writes.count(layout.rotation) && writes.count((layout.rotation+8)|0x80000000u))roots.insert(root);
    }
    return Unique(roots,"camera copy widths or UpdateCamera dispatch absent or ambiguous");
}
}
bool DiscoverBootstrap(ImageView view,Bootstrap& out,std::string& reason) {
    out={};reason.clear();
    try{out=BootstrapFrom(Image(view));return true;}catch(const Rejected& e){reason=e.what();return false;}
}
bool DiscoverCameraTarget(ImageView view,CameraLayout& out,std::string& reason) {
    out.target=0;reason.clear();
    try{out.target=CameraTarget(Image(view),out);return true;}catch(const Rejected& e){reason=e.what();return false;}
}
}
