// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <memory>
#include <vector>
namespace GenZH::Modern {
// perf87: W3DShroud::render copies its system-memory shroud into the same
// video texture every frame, changed or not, and each copy forced a complete
// D3D11 texture rebuild (D3DX conversion, new resource, mips). A copy that
// writes exactly the bytes the previous copy wrote, to the same destination
// rectangles of the same surface, with no other write to that texture since
// (its revision is still the one recorded after that copy), leaves the texture
// byte-identical. Only then is the revision left unchanged. The native copy
// always runs; any other write (lock, UpdateTexture, other copies) bumps the
// revision and invalidates the record.
struct CopyRectsSignature {
    std::vector<std::int32_t> geometry; // format, bytes per pixel, then rect and destination point per copy
    std::vector<std::uint8_t> bytes;    // source rows of every rect, in order
    bool operator==(const CopyRectsSignature &other)const{return geometry==other.geometry&&bytes==other.bytes;}
};
template<class Texture> class CopyRectsRepeats {
    struct Entry {std::weak_ptr<Texture> texture;const void *surface=nullptr;std::uint64_t revision=0;CopyRectsSignature signature;std::uint64_t tick=0;};
    std::vector<Entry> entries_;std::uint64_t tick_=0;
public:
    static constexpr std::size_t Limit=16,MaxBytes=std::size_t(4)<<20;
    bool Repeat(const std::shared_ptr<Texture> &texture,std::uint64_t revision,const void *surface,const CopyRectsSignature &signature)const{
        if(!texture)return false;
        for(const auto &e:entries_)if(e.surface==surface){const auto owner=e.texture.lock();
            return owner&&owner==texture&&e.revision==revision&&e.signature==signature;}
        return false;
    }
    // Called after the copy's revision bump with the texture's new revision.
    void Record(const std::shared_ptr<Texture> &texture,std::uint64_t revision,const void *surface,CopyRectsSignature &&signature){
        if(!texture)return;
        Entry *slot=nullptr;
        for(auto &e:entries_)if(e.surface==surface){slot=&e;break;}
        if(!slot)for(auto &e:entries_)if(e.texture.expired()){slot=&e;break;}
        if(!slot&&entries_.size()<Limit){entries_.emplace_back();slot=&entries_.back();}
        if(!slot){slot=&entries_.front();for(auto &e:entries_)if(e.tick<slot->tick)slot=&e;}
        slot->texture=texture;slot->surface=surface;slot->revision=revision;slot->signature=std::move(signature);slot->tick=++tick_;
    }
    void Forget(const void *surface){for(auto it=entries_.begin();it!=entries_.end();++it)if(it->surface==surface){entries_.erase(it);return;}}
    void Clear(){entries_.clear();}
    std::size_t Size()const{return entries_.size();}
};
}
