// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <vector>
#include <memory>
#include <iterator>
#include <utility>
namespace GenZH::Modern {
// Keep deferred packets address-stable. Growing a vector<DrawPacket> copied the
// entire queue and required an 839 MiB contiguous allocation in the user trace.
// Every page has a fixed capacity; growth moves only small page pointers.
template<class T,std::size_t PageCapacity=64> class PagedSceneQueue {
 static_assert(PageCapacity>0);
 std::vector<std::unique_ptr<std::vector<T>>> pages_;
 std::size_t count_=0;
public:
 std::size_t size()const{return count_;}
 bool empty()const{return !count_;}
 T &operator[](std::size_t i){return (*pages_[i/PageCapacity])[i%PageCapacity];}
 const T &operator[](std::size_t i)const{return (*pages_[i/PageCapacity])[i%PageCapacity];}
 T &back(){return (*this)[count_-1];}
 const T &back()const{return (*this)[count_-1];}
 void push_back(T &&value){
  const auto page=count_/PageCapacity;
  if(page==pages_.size()){
   auto data=std::make_unique<std::vector<T>>();data->reserve(PageCapacity);
   pages_.push_back(std::move(data));
  }
  discard(); // a staged slot is never left behind an appended element
  pages_[page]->push_back(std::move(value));++count_;
 }
 // perf88: an element can be built in place in the next slot (stage), then
 // become the last element (commit) or be destroyed (discard), instead of
 // being built elsewhere and moved in. The staged slot is not part of size(),
 // indexing or iteration; nothing else is appended while a slot is staged.
 T &stage(){return stageWith();}
 // perf111: the slot is built in place from the arguments, so a tagged constructor can leave storage unzeroed.
 template<class... Args> T &stageWith(Args&&... args){
  const auto page=count_/PageCapacity;
  if(page==pages_.size()){
   auto data=std::make_unique<std::vector<T>>();data->reserve(PageCapacity);
   pages_.push_back(std::move(data));
  }
  discard();
  pages_[page]->emplace_back(std::forward<Args>(args)...);return pages_[page]->back();
 }
 bool staged(const T &value)const{
  const auto page=count_/PageCapacity;if(page>=pages_.size())return false;
  const auto &data=*pages_[page];return data.size()>count_%PageCapacity&&&data[count_%PageCapacity]==&value;
 }
 void commit(){++count_;}
 void discard(){
  const auto page=count_/PageCapacity;if(page>=pages_.size())return;
  auto &data=*pages_[page];while(data.size()>count_%PageCapacity)data.pop_back();
 }
 void clear(){
  for(auto &page:pages_)page->clear();count_=0;
  // Retain a normal scene's pages, release exceptional high-water frames.
  // Resource references always expire at the original frame boundary.
  // Normal profiled scenes exceed 2,048 packets. Retain up to 4,096 to
  // avoid allocating/deallocating large pages on every frame. clear above
  // still destroys all packets and releases texture/model leases immediately.
  if(pages_.size()>64)pages_.resize(64);
 }
 class const_iterator {
  const PagedSceneQueue *owner_;std::size_t at_;
 public:
  using iterator_category=std::forward_iterator_tag;
  using value_type=T;using difference_type=std::ptrdiff_t;using pointer=const T*;using reference=const T&;
  const_iterator(const PagedSceneQueue *owner,std::size_t at):owner_(owner),at_(at){}
  reference operator*()const{return (*owner_)[at_];}
  pointer operator->()const{return &**this;}
  const_iterator &operator++(){++at_;return *this;}
  bool operator!=(const const_iterator &other)const{return at_!=other.at_||owner_!=other.owner_;}
 };
 const_iterator begin()const{return {this,0};}
 const_iterator end()const{return {this,count_};}
 static constexpr std::size_t MaxPacketAllocation=PageCapacity*sizeof(T);
};
}
