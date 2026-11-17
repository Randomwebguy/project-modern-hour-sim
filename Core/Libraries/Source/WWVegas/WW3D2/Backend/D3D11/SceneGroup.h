// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <vector>
#include <cstddef>
namespace GenZH::Modern {
struct DrawPacket;
// Most legacy scene packets cannot instance. Keep their first pointer inline;
// allocate instance storage only when an equivalent second packet is found.
class DrawGroup {
    const DrawPacket *first_;
    std::vector<const DrawPacket*> extra_;
public:
    explicit DrawGroup(const DrawPacket *first):first_(first){}
    const DrawPacket *front()const{return first_;}
    std::size_t size()const{return 1+extra_.size();}
    const DrawPacket *operator[](std::size_t i)const{return i?extra_[i-1]:first_;}
    void push_back(const DrawPacket *packet){extra_.push_back(packet);}
};
using DrawGroups=std::vector<DrawGroup>;
}
