// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <cstdint>
#include <cstring>
#include <type_traits>
#include <optional>
#include <memory>
#include <unordered_map>
namespace GenZH::Modern {
// Descriptors are value-initialized by the caller, including padding. Store the
// same byte identity as the former string keys without a draw-time allocation.
template<class T> struct BinaryStateKey {
 static_assert(std::is_trivially_copyable_v<T>);
 std::array<unsigned char,sizeof(T)> bytes;
 explicit BinaryStateKey(const T &value){std::memcpy(bytes.data(),&value,sizeof(T));}
 bool operator==(const BinaryStateKey &other)const{return bytes==other.bytes;}
};
template<class T> struct BinaryStateHash {
 std::size_t operator()(const BinaryStateKey<T> &value)const noexcept{
  std::size_t hash=2166136261u;for(auto byte:value.bytes){hash^=byte;hash*=16777619u;}return hash;
 }
};
// This memo caches lookup results, never device binding state. Every draw still
// binds its required states, so helper passes cannot make it stale. References
// to unordered_map values survive rehash; clear invalidates the memo first.
template<class T,class Value,class Hash=BinaryStateHash<T>> class BinaryStateTable {
 std::unordered_map<BinaryStateKey<T>,Value,Hash> values_;
 std::optional<BinaryStateKey<T>> lastKey_;
 Value *last_=nullptr;
public:
 Value &Get(const T &descriptor){
  const BinaryStateKey<T> key(descriptor);
  if(last_&&*lastKey_==key)return *last_;
  auto &value=values_[key];lastKey_=key;last_=std::addressof(value);return value;
 }
 void clear(){last_=nullptr;lastKey_.reset();values_.clear();}
 std::size_t size()const{return values_.size();}
};
}
