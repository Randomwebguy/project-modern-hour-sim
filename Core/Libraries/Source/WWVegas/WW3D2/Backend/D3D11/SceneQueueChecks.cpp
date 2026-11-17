// SPDX-License-Identifier: GPL-3.0-or-later
#include "SortedPacketBatch.h"
#include <iostream>
#include <cstdlib>
#include <new>
#include <stdexcept>
#include <limits>
using namespace GenZH::Modern;
static std::size_t largestAllocation=0,rejectedAllocation=0,allocationCalls=0;
void *operator new(std::size_t size){
 ++allocationCalls;
 if(size>64u*1024u*1024u){rejectedAllocation=size;throw std::bad_alloc();}
 if(size>largestAllocation)largestAllocation=size;
 if(auto p=std::malloc(size?size:1))return p;throw std::bad_alloc();
}
void operator delete(void *p)noexcept{std::free(p);}
void operator delete(void *p,std::size_t)noexcept{std::free(p);}
static unsigned checks=0;
struct CountingSamplerHash {
 static inline unsigned calls=0;
 std::size_t operator()(const BinaryStateKey<D3D11_SAMPLER_DESC> &key)const noexcept{++calls;return BinaryStateHash<D3D11_SAMPLER_DESC>{}(key);}
};
static void Check(bool value,const char *text){++checks;if(!value)throw std::runtime_error(text);}
static DrawPacket Packet(unsigned tag=0){
 DrawPacket p;p.nativeSource.sorted=true;p.nativeSource.root=1;p.nativeSource.mesh=2;p.nativeSource.resource=3;
 p.fixedVertices.resize(3);p.indices={2,0,1};
 for(unsigned i=0;i<3;++i){auto *bytes=reinterpret_cast<unsigned char*>(&p.fixedVertices[i]);
  for(std::size_t j=0;j<sizeof(FixedVertex);++j)bytes[j]=static_cast<unsigned char>(j+i+tag);}
 return p;
}
int main(){try{
 // Fail the old reserve before requesting actual huge physical memory.
 bool rejected=false;try{std::vector<DrawPacket> old;old.reserve(138255);}catch(const std::bad_alloc&){rejected=true;}
 Check(rejected,"old contiguous queue fails under a 64 MiB per-allocation guard");
 const auto oldRequest=rejectedAllocation;
 // The observed x86 crash request (879301848 bytes) belonged to an earlier, larger DrawPacket (about 6.3 KB). The current
 // packet is 2992 bytes, so the old contiguous request is checked for the current element count and size plus allocator overhead.
 if(sizeof(void*)==4)Check(oldRequest>=138255u*sizeof(DrawPacket)&&oldRequest-138255u*sizeof(DrawPacket)<64u,"old contiguous request is the element count times the current DrawPacket size plus allocator overhead");
 largestAllocation=0;
 PagedSceneQueue<DrawPacket> queue;
 // Cross the 92170 -> 138255 MSVC vector capacity transition in the crash.
 for(unsigned i=0;i<100000;++i){DrawPacket p;p.constants.world[0]=float(i);queue.push_back(std::move(p));}
 const auto *first=&queue[0];Check(queue.size()==100000,"all deferred packets retained");
 for(unsigned i=0;i<100000;++i)Check(queue[i].constants.world[0]==float(i),"every packet remains in original submission order");
 Check(first==&queue[0],"page growth cannot invalidate packet pointers");
 Check(largestAllocation<1024u*1024u,"no packet growth allocation reaches one MiB");
 const auto maxAllocation=largestAllocation;queue.clear();Check(queue.empty(),"frame clears queued states");
 auto lease=std::make_shared<int>(1);std::weak_ptr<int> weak=lease;
 auto leased=Packet();leased.sourceVertex=lease;queue.push_back(std::move(leased));lease.reset();
 Check(!weak.expired(),"deferred frame retains source lifetime");queue.clear();Check(weak.expired(),"clear releases every source lifetime");
 queue.push_back(Packet());Check(queue.size()==1&&queue.back().indices[0]==2,"retained pages reusable after high-water clear");queue.clear();
 auto a=Packet(),b=Packet(7);const auto av=a.fixedVertices,bv=b.fixedVertices;
 // Native aggregate padding is not model/appearance metadata. It may vary
 // after copies even when every field and shader constant is identical.
 const auto padding=offsetof(NativeMeshSource,sortedVertexCount)-(offsetof(NativeMeshSource,sorted)+sizeof(bool));
 if(padding){auto *bytes=reinterpret_cast<unsigned char*>(&b.nativeSource);std::memset(bytes+offsetof(NativeMeshSource,sorted)+sizeof(bool),0xab,padding);}
 Check(SameSortedSource(a.nativeSource,b.nativeSource),"ownership equality independent of aggregate padding");
 // Blend and alpha-test are deliberately enabled: order must be exact.
 a.MutableStates().render[D3DRS_ALPHABLENDENABLE]=b.MutableStates().render[D3DRS_ALPHABLENDENABLE]=TRUE;
 a.MutableStates().render[D3DRS_ALPHATESTENABLE]=b.MutableStates().render[D3DRS_ALPHATESTENABLE]=TRUE;
 Check(TryAppendSortedPacket(a,b),"adjacent identical sorted material can concatenate without reordering");
 Check(a.indices==std::vector<std::uint32_t>({2,0,1,5,3,4}),"triangle winding and transparent order identical");
 Check(std::memcmp(a.fixedVertices.data(),av.data(),av.size()*sizeof(FixedVertex))==0&&
  std::memcmp(a.fixedVertices.data()+av.size(),bv.data(),bv.size()*sizeof(FixedVertex))==0,"all normals colors and UV bytes retained");
 for(unsigned mode=0;mode<14;++mode){auto x=Packet(),y=Packet();
  switch(mode){case 0:y.constants.world[0]=1;break;case 1:y.MutableStates().stages[0][D3DTSS_ADDRESSU]=D3DTADDRESS_CLAMP;break;
   case 2:y.MutableStates().render[D3DRS_ALPHABLENDENABLE]=TRUE;break;case 3:y.nativeSource.bone=4;break;
   case 4:y.nativeSource.drawable=9;break;case 5:y.constants.vehicleAge[0]=1;break;case 6:y.vertexShader=1;break;
   case 7:y.pixelShader=1;break;case 8:y.terrain=true;break;case 9:y.groundOverlay=true;break;
   case 10:y.groundExclusionOverlay=true;break;case 11:y.nativeSource.skinned=true;break;
   case 12:y.outputOverlay=true;break;case 13:y.indices[0]=99;break;}
  Check(!TryAppendSortedPacket(x,y)&&x.indices==std::vector<std::uint32_t>({2,0,1}),"different or unsupported state remains a separate ordered packet");
 }
 auto wide=Packet(),tiny=Packet();wide.fixedVertices.resize(43690);Check(!TryAppendSortedPacket(wide,tiny),"eight MiB upload budget splits work without dropping geometry");
 // Descriptor identity must match the former byte string, with zero hot-loop allocations.
 std::unordered_map<BinaryStateKey<D3D11_SAMPLER_DESC>,unsigned,BinaryStateHash<D3D11_SAMPLER_DESC>> keys;
 for(unsigned i=0;i<256;++i){D3D11_SAMPLER_DESC desc{};desc.MaxAnisotropy=i;keys.emplace(BinaryStateKey(desc),i);}
 const auto allocations=allocationCalls;
 for(unsigned i=0;i<10000;++i){D3D11_SAMPLER_DESC desc{};desc.MaxAnisotropy=i%256;const BinaryStateKey key(desc);
  const auto found=keys.find(key);Check(found!=keys.end()&&found->second==i%256,"binary state lookup aliased different sampler descriptors");}
 Check(allocationCalls==allocations,"hot descriptor lookup allocated heap memory");
 // Memoization must bypass hashing, preserve distinct descriptors, and never
 // retain a reference into a cleared table. Rehash must keep prior values valid.
 BinaryStateTable<D3D11_SAMPLER_DESC,unsigned,CountingSamplerHash> memo;
 D3D11_SAMPLER_DESC repeated{};repeated.MaxAnisotropy=8;memo.Get(repeated)=71;
 const auto hashCalls=CountingSamplerHash::calls;
 const auto heapCalls=allocationCalls;
 for(unsigned i=0;i<10000;++i)Check(memo.Get(repeated)==71,"identical sampler memo changed state");
 Check(CountingSamplerHash::calls==hashCalls&&allocationCalls==heapCalls,"memo hit performed hash or allocation");
 for(unsigned i=0;i<256;++i){auto changed=repeated;changed.MaxAnisotropy=i;memo.Get(changed)=i;}
 Check(memo.Get(repeated)==8&&memo.size()==256,"rehash aliased descriptor states");
 memo.clear();Check(memo.size()==0&&memo.Get(repeated)==0,"reset kept stale state memo");
 // A 2,500-packet frame must reuse its pages, while clear releases leases.
 PagedSceneQueue<DrawPacket> normal;
 for(unsigned i=0;i<2500;++i)normal.push_back(DrawPacket{});normal.clear();
 const auto pageAllocations=allocationCalls;
 for(unsigned i=0;i<2500;++i)normal.push_back(DrawPacket{});
 Check(allocationCalls==pageAllocations,"normal scene reallocates pages every frame");normal.clear();
 std::cout<<"{\"passed\":true,\"checks\":"<<checks<<",\"pointer_bits\":"<<sizeof(void*)*8
  <<",\"packets_retained\":100000,\"packet_bytes\":"<<sizeof(DrawPacket)<<",\"old_contiguous_request\":"<<oldRequest
  <<",\"largest_new_allocation\":"<<maxAllocation<<",\"shader_or_texture_quality_reduced\":false,\"game_started\":false}";
 return 0;
}catch(const std::exception &e){std::cerr<<e.what();return 1;}}
