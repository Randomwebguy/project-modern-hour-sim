// SPDX-License-Identifier: GPL-3.0-or-later
#include "NativeMeshGeometry.h"
#include "NativeLegacyPacking.h"
#include "NativeSharedGeometry.h"
#include "TerrainExclusionInputs.h"
#include "SortedBufferGeometry.h"
#include <iostream>
#include <limits>
using namespace GenZH::Modern;
static unsigned checks=0,pairs=0;
static void Check(bool value,const char *message){++checks;if(!value){std::cerr<<message<<'\n';std::exit(1);}}
static void Pair(unsigned count,unsigned channels,unsigned colorFlags){
 std::vector<std::array<float,3>> positions(count),normals(count);
 std::vector<std::array<float,2>> uv(count);std::vector<std::uint32_t> diffuse(count),specular(count);
 for(unsigned i=0;i<count;++i){const float f=static_cast<float>(i);positions[i]={f*.01f,-f*.02f,f*.003f};normals[i]={.1f,.2f,.97f};uv[i]={f*.001f,f*.002f};diffuse[i]=0xff112233u+i;specular[i]=0x88445566u+i;}
 const std::array<std::uint32_t,3> triangles[]={{0,count-1,count/2},{count/2,count-1,0}};
 NativeGeometryView view;view.recipe=std::make_shared<NativeGeometryRecipe>();view.recipe->polygons={1,0};
 view.positions=positions.data();view.normals=normals.data();view.diffuse=diffuse.data();view.specular=specular.data();view.triangles=triangles;
 view.vertices=view.range=count;view.triangleCount=2;view.model=7;view.legacyFirstVertex=90;view.legacyFirstIndex=100;
 view.fvf=D3DFVF_XYZ|D3DFVF_NORMAL|(channels<<D3DFVF_TEXCOUNT_SHIFT)|colorFlags;
 for(unsigned i=0;i<channels;++i)view.uv[i]=uv.data();
 const unsigned stride=24+(colorFlags&D3DFVF_DIFFUSE?4:0)+(colorFlags&D3DFVF_SPECULAR?4:0)+channels*8;
 std::vector<std::uint8_t> packed(std::size_t(count)*stride);
 for(unsigned i=0;i<count;++i){auto *vertex=packed.data()+std::size_t(i)*stride;std::memcpy(vertex,positions[i].data(),12);std::memcpy(vertex+12,normals[i].data(),12);unsigned offset=24;
  if(colorFlags&D3DFVF_DIFFUSE){std::memcpy(vertex+offset,&diffuse[i],4);offset+=4;}
  if(colorFlags&D3DFVF_SPECULAR){std::memcpy(vertex+offset,&specular[i],4);offset+=4;}
  for(unsigned j=0;j<channels;++j){std::memcpy(vertex+offset,uv[i].data(),8);offset+=8;}
 }
 std::vector<std::uint8_t> fallback(packed.size(),0xcd);
 Check(PackNativeLegacyVertices(view,fallback.data(),fallback.size()),"retained native arrays repack fallback vertices");
 Check(fallback==packed,"fallback vertex bytes equal original eager registration including all UVs/colors");
 std::array<std::uint32_t,6> fallbackIndices{};
 Check(PackNativeLegacyIndices(view,90,reinterpret_cast<std::uint8_t*>(fallbackIndices.data()),24),"fallback R32 index writer");
 Check(fallbackIndices==std::array<std::uint32_t,6>{count/2+90,count-1+90,90,90,count-1+90,count/2+90},"fallback base vertex and material order exact");
 std::vector<LegacyVertex> legacy;std::string error;Check(SUCCEEDED(DecodeVertices(packed.data(),packed.size(),stride,view.fvf,0,count,legacy,error)),"original FVF decoder");
 std::vector<std::uint32_t> directIndices{count/2,count-1,0,0,count-1,count/2};
 std::vector<FixedVertex> direct;
 Check(SUCCEEDED(DecodeReferencedFixedVertices(packed.data(),packed.size(),stride,view.fvf,0,count,directIndices,direct,error)),"direct compact dynamic FVF decode");
 const unsigned sources[]={0,count/2,count-1};
 Check(direct.size()==3&&directIndices==std::vector<std::uint32_t>({1,2,0,0,2,1}),"sparse compact gather preserves primitive order and repetitions");
 for(unsigned i=0;i<3;++i)Check(std::memcmp(&direct[i],&legacy[sources[i]],sizeof(FixedVertex))==0,"all compact UV/color/normal bytes equal original decoder");
 DecodedGeometry native;Check(SUCCEEDED(BuildNativeGeometry(view,native)),"direct native builder");Check(native.fixedVertices.size()==count&&native.vertices.empty(),"compact-only native output");
 for(unsigned i=0;i<count;++i)Check(std::memcmp(&native.fixedVertices[i],&legacy[i],192)==0,"every vertex attribute byte matches old decode");
 Check(native.indices==std::vector<std::uint32_t>({count/2,count-1,0,0,count-1,count/2}),"material polygon order and R32 values retained");
 Check(native.boundsReady&&native.low[1]==positions.back()[1]&&native.high[0]==positions.back()[0],"bounds derived from actual vertices");
 NativeIndexedDraw draw{D3DPT_TRIANGLELIST,90,count,100,2};Check(view.Matches(draw,view.fvf,7),"registered draw ranges match");
 auto bad=draw;++bad.firstIndex;Check(!view.Matches(bad,view.fvf,7),"other index range cannot consume scoped source");
 Check(!view.Matches(draw,view.fvf,8)&&!view.Matches(draw,D3DFVF_XYZ,7),"model/layout mismatch falls back");
 GeometryCache cache(128u*1024u*1024u);auto resource=std::make_shared<DecodedGeometry>(std::move(native));auto key=view.Key();
 cache.Insert(key,resource,view.recipe,view.recipe,&view.recipe->revision,&view.recipe->revision);Check(cache.Find(key)==resource,"immutable native resource cached by registration identity");
 ++view.recipe->revision;Check(!cache.Find(key),"registration revision invalidates");
 Check(key[0]==0x4e41544956453235ull,"native/legacy key namespace disjoint");
 auto second=std::make_shared<NativeGeometryRecipe>();Check(second->identity.Get()!=view.recipe->identity.Get(),"replacement registration gets a new identity");
 ++pairs;
}
struct TestArray {unsigned refs=1;void Add_Ref(){++refs;}void Release_Ref(){--refs;}};
static void DeferredChecks(){
 TestArray array;{auto owner=RetainNativeArray(&array);Check(array.refs==2,"native share array retained without model cycle");auto copy=owner;Check(array.refs==2,"ownership copies share intrusive reference");}Check(array.refs==1,"array owner releases exactly once");
 DeferredLegacyBuffer buffer;buffer.Reserve(64);unsigned writes=0,locks=0,unlocks=0;std::vector<std::uint8_t> bytes(64,0xcd);
 auto lifetime=std::make_shared<int>(3);std::weak_ptr<int> weak=lifetime;
 buffer.Add(4,8,[&,lifetime](std::uint8_t *out,std::size_t size){++writes;std::fill(out,out+size,static_cast<std::uint8_t>(*lifetime));return true;});lifetime.reset();
 Check(buffer.Pending()==1&&writes==0&&!weak.expired(),"registration keeps recipe only and allocates no packed buffer");
 buffer.Created();const auto created=deferredLegacyStats.materializedBuffers;buffer.Created();Check(deferredLegacyStats.materializedBuffers==created,"materialization counted once");
 Check(!buffer.Flush([&](unsigned,unsigned,std::uint8_t**){++locks;return false;},[&](){++unlocks;return true;}),"failed lock does not run source writer");
 Check(writes==0&&unlocks==0&&buffer.Pending()==1,"failed materialization retains recipes for retry");
 auto lock=[&](unsigned offset,unsigned count,std::uint8_t **out){++locks;Check(offset==4&&count==8,"fallback writes exact reserved subrange");*out=bytes.data()+offset;return true;};
 auto unlock=[&](){++unlocks;return true;};
 Check(buffer.Flush(lock,unlock),"fallback materializes queued writer");Check(buffer.Pending()==0&&weak.expired()&&writes==1&&unlocks==1,"flush releases array owners and recipes");
 Check(bytes[3]==0xcd&&bytes[4]==3&&bytes[11]==3&&bytes[12]==0xcd,"fallback cannot overwrite adjacent mesh range");
 const auto before=locks;Check(buffer.Flush(lock,unlock)&&locks==before,"warm buffer has no repeated writer or lock");
 bool rejected=false;try{buffer.Add(63,2,[](std::uint8_t*,std::size_t){return true;});}catch(const std::invalid_argument&){rejected=true;}Check(rejected,"out of capacity provider rejected before access");
 buffer.Add(4,8,[](std::uint8_t*,std::size_t){return false;});Check(!buffer.Flush(lock,unlock)&&buffer.Pending()==1,"failed writer unlocks and retains source for retry");
}

static void StridedUVChecks(){
 constexpr unsigned count=70005,stride=44;
 std::vector<std::array<float,3>> positions(count),normals(count);
 std::vector<std::array<float,2>> uv(count);
 auto packed=std::make_shared<std::vector<std::uint8_t>>(std::size_t(count)*stride);
 for(unsigned i=0;i<count;++i){positions[i]={float(i)*.01f,0,.5f};normals[i]={0,0,1};uv[i]={float(i)*.002f,.75f};
  auto *p=packed->data()+std::size_t(i)*stride;std::memcpy(p,positions[i].data(),12);std::memcpy(p+12,normals[i].data(),12);
  const std::uint32_t white=0xffffffff;std::memcpy(p+24,&white,4);std::memcpy(p+28,uv[i].data(),8);
  const float second[]={float(i)*-.003f,.125f};std::memcpy(p+36,second,8);}
 const std::array<std::uint32_t,3> tri[]={{{70002,70004,70003}}};
 NativeGeometryView v;v.recipe=std::make_shared<NativeGeometryRecipe>();v.recipe->polygons={0};v.model=17;
 v.vertices=count;v.first=70002;v.range=3;v.triangleCount=1;v.triangles=tri;v.positions=positions.data();v.normals=normals.data();
 v.fvf=D3DFVF_XYZ|D3DFVF_NORMAL|D3DFVF_DIFFUSE|D3DFVF_TEX2;v.uv[0]=uv.data();v.uv[1]=packed->data()+36;v.uvStrides[1]=stride;v.uvFallbackOwner=packed;
 DecodedGeometry native;Check(SUCCEEDED(BuildNativeGeometry(v,native)),"sorting spare UV slot retained with original packed stride");
 std::vector<LegacyVertex> original;std::string error;
 Check(SUCCEEDED(DecodeVertices(packed->data(),packed->size(),stride,v.fvf,70002,3,original,error)),"strided UV legacy baseline");
 for(unsigned i=0;i<3;++i)Check(std::memcmp(&native.fixedVertices[i],&original[i],192)==0,"all sorted vertex attributes byte exact above 65535");
 std::vector<std::uint8_t> repacked(packed->size());Check(PackNativeLegacyVertices(v,repacked.data(),repacked.size())&&repacked==*packed,"strided fallback roundtrip exact");
 NativeVertexPool pool;NativeVertexStats stats;DecodedGeometry a,b;
 Check(SUCCEEDED(BuildNativeSharedGeometry(v,pool,a,stats))&&SUCCEEDED(BuildNativeSharedGeometry(v,pool,b,stats))&&a.sharedVertices==b.sharedVertices&&stats.hits==1,"sorted submeshes share full native vertices");
 auto key=NativeVerticesKey(v);v.uvStrides[1]=8;Check(key!=NativeVerticesKey(v),"UV stride cannot alias shared vertex cache identity");
 v.uvStrides[1]=stride;v.uvFallbackOwner.reset();Check(!v.Valid(),"borrowed packed UV requires lifetime owner");
}
static void ReferencedChecks(){
 // Reproduce a wide compatibility buffer, with distant sparse indices above
 // 65535, eight UV channels/colors, and repeated strip/degenerate corners.
 constexpr unsigned count=200003,stride=96;
 const DWORD fvf=D3DFVF_XYZ|D3DFVF_NORMAL|D3DFVF_DIFFUSE|D3DFVF_SPECULAR|D3DFVF_TEX8;
 std::vector<std::uint8_t> bytes(std::size_t(count)*stride);
 for(unsigned i=0;i<count;++i){float p[6]={float(i)*.01f,float(i)*-.02f,.25f,0,0,1};std::memcpy(bytes.data()+std::size_t(i)*stride,p,24);
  std::uint32_t colors[]={0xff123456u+i,0x88765432u+i};std::memcpy(bytes.data()+std::size_t(i)*stride+24,colors,8);
  for(unsigned uv=0;uv<8;++uv){float pair[]={float(i)*.001f+uv,float(i)*-.003f+uv};std::memcpy(bytes.data()+std::size_t(i)*stride+32+uv*8,pair,8);}}
 const std::vector<std::uint32_t> original={200002,65536,3,3,200002,3,65536,3,200002};
 std::vector<LegacyVertex> old,gathered;std::string error;auto indices=original;
 Check(SUCCEEDED(DecodeVertices(bytes.data(),bytes.size(),stride,fvf,0,count,old,error)),"wide baseline decoded");
 Check(SUCCEEDED(DecodeReferencedVertices(bytes.data(),bytes.size(),stride,fvf,0,count,indices,gathered,error)),"sparse indexed compatibility gather");
 Check(gathered.size()==3,"three referenced vertices, no 200003-vertex transient allocation");
 for(unsigned i=0;i<original.size();++i)Check(std::memcmp(&old[original[i]],&gathered[indices[i]],sizeof(LegacyVertex))==0,"indexed winding, colors and all UVs identical to original");
 auto invalid=original;invalid[1]=2;const auto unchanged=invalid;
 Check(FAILED(DecodeReferencedVertices(bytes.data(),bytes.size(),stride,fvf,3,count-3,invalid,gathered,error))&&invalid==unchanged,"out-of-range rejection without index mutation");
 indices=original;
 Check(FAILED(DecodeReferencedVertices(bytes.data(),bytes.size()-1,stride,fvf,0,count,indices,gathered,error)),"declared full buffer extent still validated");
 // Actual multi-stream declarations exercise all register formats and skipped
 // fields; gather must preserve shader register values rather than infer FVF.
 std::vector<DWORD> declaration={D3DVSD_STREAM(0),D3DVSD_REG(0,D3DVSDT_FLOAT3),D3DVSD_REG(1,D3DVSDT_FLOAT3),D3DVSD_REG(2,D3DVSDT_D3DCOLOR),D3DVSD_REG(3,D3DVSDT_UBYTE4),D3DVSD_STREAM(1),D3DVSD_REG(4,D3DVSDT_FLOAT2),D3DVSD_SKIP(2),D3DVSD_REG(5,D3DVSDT_SHORT4),D3DVSD_END()};
 std::array<VertexStreamSpan,16> streams{};streams[0]={bytes.data(),bytes.size(),stride};streams[1]={bytes.data(),bytes.size(),stride};
 Check(SUCCEEDED(DecodeDeclaration(declaration,streams,0,count,old,error)),"wide multi-stream reference");indices=original;
 Check(SUCCEEDED(DecodeReferencedDeclaration(declaration,streams,0,count,indices,gathered,error)),"sparse multi-stream compatibility gather");
 Check(gathered.size()==3,"programmable path allocates only referenced vertices");
 for(unsigned i=0;i<original.size();++i)Check(std::memcmp(&old[original[i]],&gathered[indices[i]],sizeof(LegacyVertex))==0,"all programmable register values identical");
 indices=original;streams[1].bytes=1;
 Check(FAILED(DecodeReferencedDeclaration(declaration,streams,0,count,indices,gathered,error))&&indices==original,"missing secondary stream rejected transactionally");
 // Near-complete references also preserve the non-sparse case.
 indices.resize(count);for(unsigned i=0;i<count;++i)indices[i]=count-1-i;
 streams[1].bytes=bytes.size();
 Check(SUCCEEDED(DecodeReferencedVertices(bytes.data(),bytes.size(),stride,fvf,0,count,indices,gathered,error))&&gathered.size()==count,"full indexed input retains every vertex");
 for(unsigned i=0;i<count;++i)Check(indices[i]==count-1-i,"full stream order preserved");
}
#include "OpaqueSortPolicy.h"
static void CompactLayoutChecks(){
 constexpr unsigned stride=160,count=3;std::array<float,stride*count/4> raw{};
 for(unsigned i=0;i<raw.size();++i)raw[i]=.01f*static_cast<float>(i+1);
 for(unsigned rhw=0;rhw<2;++rhw)for(unsigned normal=0;normal<2;++normal)for(unsigned colors=0;colors<4;++colors)for(unsigned channels=0;channels<=8;++channels){
  DWORD fvf=(rhw?D3DFVF_XYZRHW:D3DFVF_XYZ)|(normal?D3DFVF_NORMAL:0)|(colors&1?D3DFVF_DIFFUSE:0)|(colors&2?D3DFVF_SPECULAR:0)|(channels<<D3DFVF_TEXCOUNT_SHIFT);
  for(unsigned i=0;i<channels;++i)fvf|=DWORD(i%4)<<(16+i*2);
  std::vector<std::uint32_t> a{2,0,2,1,0,1},b=a;std::vector<LegacyVertex> legacy;std::vector<FixedVertex> fixed;std::string error;
  Check(SUCCEEDED(DecodeReferencedVertices(raw.data(),sizeof(raw),stride,fvf,0,count,a,legacy,error))&&
   SUCCEEDED(DecodeReferencedFixedVertices(raw.data(),sizeof(raw),stride,fvf,0,count,b,fixed,error)),"compact and legacy XYZ/RHW, color and UV1..4 layouts decode");
  Check(a==b&&legacy.size()==fixed.size(),"all layouts preserve repeated transparent index order");
  for(unsigned i=0;i<fixed.size();++i)Check(std::memcmp(&fixed[i],&legacy[i],sizeof(FixedVertex))==0,"compact default and all eight UV attributes remain byte exact");
  const auto before=b;
  Check(FAILED(DecodeReferencedFixedVertices(raw.data(),sizeof(raw)-1,stride,fvf,0,count,b,fixed,error))&&b==before,"truncated compact source cannot publish remapped indices");
 }
}
static void CapturedSortedBufferChecks(){
 constexpr UINT count=70018,stride=44,base=13;
 auto vertexOwner=std::make_shared<int>(1),indexOwner=std::make_shared<int>(2);
 std::vector<std::uint8_t> raw(std::size_t(count)*stride);
 for(UINT i=0;i<count;++i){const float values[]={float(i)*.001f,float(i)*-.002f,.5f,0,0,1};
  auto *p=raw.data()+std::size_t(i)*stride;std::memcpy(p,values,24);const std::uint32_t color=0x80402010u+i;std::memcpy(p+24,&color,4);
  const float uv[]={float(i)*.002f,.125f,float(i)*-.003f,.75f};std::memcpy(p+28,uv,16);}
 std::vector<std::uint32_t> rawIndices={UINT_MAX,UINT_MAX,70004,65536,3,3,70004,65536};
 SortedBufferView view;view.vertexData=raw.data();view.vertexBytes=raw.size();view.indexData=rawIndices.data();view.indexBytes=rawIndices.size()*4;
 view.vertexOwner=vertexOwner;view.indexOwner=indexOwner;view.vertexRevision=view.indexRevision=1;
 view.fvf=D3DFVF_XYZ|D3DFVF_NORMAL|D3DFVF_DIFFUSE|D3DFVF_TEX2;view.stride=stride;
 view.firstVertex=base;view.vertexCount=count-base;view.firstIndex=2;view.indexCount=3;view.base=base;view.indexFormat=D3DFMT_INDEX32;
 std::vector<std::uint32_t> selected{70017,65549,16};
 SortedBufferGeometryPool pool(128u*1024u*1024u);DecodedGeometry first;std::string error;
 Check(BuildSortedBufferGeometry(view,selected,pool,first,error)==S_OK,"capture sorted R32 shared buffers with nonzero first index/base and unused UINT_MAX slots");
 Check(first.IndexFirst()==2&&first.IndexBase()==13&&first.indices==selected,"draw keeps original index offset/base and primitive order");
 std::vector<LegacyVertex> old;auto oldIndices=selected;
 Check(SUCCEEDED(DecodeReferencedVertices(raw.data(),raw.size(),stride,view.fvf,base,count-base,oldIndices,old,error)),"sparse legacy GPU attribute reference");
 for(unsigned i=0;i<3;++i)Check(std::memcmp(first.Position(selected[i]),&old[oldIndices[i]],sizeof(FixedVertex))==0,"shared indexed attributes byte exact above 65535 with base");
 // Compare every float32 attribute against the full original decode, including
 // the absent specular and six zero UV channels used by input-layout aliases.
 auto twoUVView=view;twoUVView.compactTwoUV=true;DecodedGeometry twoUV;
 Check(BuildSortedBufferGeometry(twoUVView,selected,pool,twoUV,error)==S_OK&&twoUV.TwoUV(),"native two-UV sorted snapshot selected");
 Check(twoUV.Stride()==96&&first.Stride()==192&&twoUV.sharedVertices!=first.sharedVertices,"input layout participates in the immutable snapshot cache key");
 for(UINT i=0;i<count;++i){FixedVertex expanded;std::memcpy(&expanded,&twoUV.sharedVertices->twoUVVertices[i],96);
  Check(!std::memcmp(&expanded,&first.sharedVertices->vertices[i],192),"all position normal color and eight UV vectors remain byte exact");}
 Check(twoUV.sharedVertices->Bytes()*2==first.sharedVertices->Bytes(),"two-UV representation halves CPU snapshot bytes without quantization");
 Check(twoUV.indices==first.indices&&!std::memcmp(twoUV.low,first.low,12)&&!std::memcmp(twoUV.high,first.high,12),"two-UV indices and conservative bounds byte exact");
 auto twoUVAgain=twoUV;Check(BuildSortedBufferGeometry(twoUVView,selected,pool,twoUVAgain,error)==S_OK&&twoUVAgain.sharedVertices==twoUV.sharedVertices,"exact layout snapshot reused");
 Check(pool.twoUVDraws==2&&pool.twoUVDecodedBytesSaved==std::uint64_t(count)*96,"two-UV decode and reuse counters exact");
 // Unsupported layouts retain all channels in the original full decoder.
 std::vector<TwoUVFixedVertex> rejected;Check(FAILED(DecodeTwoUVFixedVertices(raw.data(),raw.size(),stride,D3DFVF_XYZ|D3DFVF_TEX8,count,rejected,error))&&rejected.empty(),"eight-UV layouts cannot overrun the two-UV representation");
 Check(FAILED(DecodeTwoUVFixedVertices(raw.data(),raw.size()-1,stride,view.fvf,count,rejected,error)),"truncated packed input retains checked rejection");
 const auto oldTwoX=twoUV.Position(70017)[0];
 const auto bytes=pool.vertices.decodedBytes;
 for(unsigned draw=0;draw<10000;++draw){DecodedGeometry next;
  Check(BuildSortedBufferGeometry(view,selected,pool,next,error)==S_OK&&next.sharedVertices==first.sharedVertices&&next.sharedIndices==first.sharedIndices,"10000 ordered packets share one exact VB/IB revision");}
 Check(pool.vertices.misses==2&&pool.indices.misses==1&&pool.vertices.hits==10001&&pool.indices.hits==10002&&pool.vertices.decodedBytes==bytes,"warm captured buffer does no repeated vertex decode or resource creation");
 const auto oldX=first.Position(70017)[0];float changed=123.5f;std::memcpy(raw.data()+std::size_t(70017)*stride,&changed,4);++view.vertexRevision;
 DecodedGeometry rewritten;Check(BuildSortedBufferGeometry(view,selected,pool,rewritten,error)==S_OK,"successful VB unlock revision refreshes snapshot");
 twoUVView.vertexRevision=view.vertexRevision;DecodedGeometry changedTwoUV;
 Check(BuildSortedBufferGeometry(twoUVView,selected,pool,changedTwoUV,error)==S_OK&&changedTwoUV.Position(70017)[0]==changed&&twoUV.Position(70017)[0]==oldTwoX,"old two-UV packets survive source rewrite and new successful revision");
 Check(first.Position(70017)[0]==oldX&&rewritten.Position(70017)[0]==changed&&first.sharedIndices==rewritten.sharedIndices,"queued old positions survive DISCARD while unchanged index resource shares");
 rawIndices[2]=3;rawIndices[4]=70004;++view.indexRevision;selected={16,65549,70017};
 DecodedGeometry reindexed;Check(BuildSortedBufferGeometry(view,selected,pool,reindexed,error)==S_OK,"successful IB unlock revision refreshes draw order");
 Check(first.sharedIndices->indices[2]==70004&&reindexed.sharedIndices->indices[2]==3&&reindexed.indices==selected,"queued old index order survives NOOVERWRITE");
 auto bad=view;bad.firstVertex=17;DecodedGeometry untouched=first;
 Check(FAILED(BuildSortedBufferGeometry(bad,selected,pool,untouched,error))&&untouched.sharedVertices==first.sharedVertices,"out of declared vertex range rejected transactionally");
 bad=view;bad.indexBytes-=1;Check(FAILED(BuildSortedBufferGeometry(bad,selected,pool,untouched,error)),"truncated index allocation rejected");
 bad=view;bad.base=UINT_MAX;Check(BuildSortedBufferGeometry(bad,selected,pool,untouched,error)==S_FALSE,"unsigned base outside D3D11 signed range keeps compatibility fallback");
 SortedBufferGeometryPool tiny(1024);Check(BuildSortedBufferGeometry(view,selected,tiny,untouched,error)==S_FALSE&&untouched.sharedVertices==first.sharedVertices,"cache budget bypass leaves exact sparse quality path available");
 std::vector<std::uint16_t> index16={5,2,0};std::vector<std::uint32_t> selected16={18,15,13};
 auto sixteen=view;sixteen.indexOwner=std::make_shared<int>(3);sixteen.indexData=index16.data();sixteen.indexBytes=6;sixteen.indexFormat=D3DFMT_INDEX16;sixteen.firstIndex=0;
 DecodedGeometry shortIndicesGeometry;Check(BuildSortedBufferGeometry(sixteen,selected16,pool,shortIndicesGeometry,error)==S_OK&&shortIndicesGeometry.sharedIndices->indices==std::vector<std::uint32_t>({5,2,0}),"INDEX16 converted to R32 with original base and order");
 pool.Clear();Check(pool.Bytes()==0&&pool.Size()==0&&first.Position(70017)[0]==oldX,"device reset clears cache while outstanding immutable packet retains data");
 CapturedBufferPool<SharedBufferedIndices,3> lifetime(1024);auto owner=std::make_shared<int>(9);std::shared_ptr<SharedBufferedIndices> retained;
 Check(lifetime.Acquire({1,2,3},1,owner,12,[](SharedBufferedIndices &x){x.indices={1,2,3};return S_OK;},retained)==S_OK,"weak owner pool setup");
 owner.reset();lifetime.Prune();Check(lifetime.Size()==0&&lifetime.Bytes()==0&&retained->indices[2]==3,"expired owner pruned without dangling access or queued packet mutation");
}
int main(){
 CapturedSortedBufferChecks();
 CompactLayoutChecks();
 for(DWORD blend:{0u,1u})for(DWORD src:{DWORD(D3DBLEND_ONE),DWORD(D3DBLEND_SRCALPHA)})for(DWORD dst:{DWORD(D3DBLEND_ZERO),DWORD(D3DBLEND_INVSRCALPHA)})
 for(DWORD depth:{0u,1u,2u})for(DWORD write:{0u,1u})for(DWORD compare:{DWORD(D3DCMP_LESS),DWORD(D3DCMP_LESSEQUAL),DWORD(D3DCMP_ALWAYS)})for(DWORD stencil:{0u,1u})for(DWORD pixel:{0u,1u}){
  const bool expected=(!blend||(src==D3DBLEND_ONE&&dst==D3DBLEND_ZERO))&&depth==1&&write&&compare!=D3DCMP_ALWAYS&&!stencil&&!pixel;
  Check(OpaqueSortBypass(blend,src,dst,depth,write,compare,stencil,pixel)==expected,"opaque sorted category must retain depth and transparency contracts");
 }

 for(DWORD func:{DWORD(D3DCMP_ALWAYS),DWORD(D3DCMP_EQUAL),DWORD(D3DCMP_NEVER)})
 for(DWORD fail=1;fail<=8;++fail)for(DWORD zfail=1;zfail<=8;++zfail)for(DWORD pass=1;pass<=8;++pass){
  const bool safe=func==D3DCMP_ALWAYS&&fail==D3DSTENCILOP_KEEP&&zfail==D3DSTENCILOP_KEEP&&(pass==D3DSTENCILOP_KEEP||pass==D3DSTENCILOP_REPLACE);
  Check(OpaqueSortBypass(0,0,0,1,1,D3DCMP_LESSEQUAL,1,0,func,fail,zfail,pass)==safe,"player marking only; stencil-test/fail/counting operations retained");
 }
 StridedUVChecks();
 ReferencedChecks();
 DeferredChecks();
 for(unsigned channels:{0u,1u,2u,8u})for(unsigned flags:{0u,unsigned(D3DFVF_DIFFUSE),unsigned(D3DFVF_SPECULAR),unsigned(D3DFVF_DIFFUSE|D3DFVF_SPECULAR)})Pair(9,channels,flags);
 Pair(70005,8,D3DFVF_DIFFUSE|D3DFVF_SPECULAR);
 std::array<std::array<float,3>,5> p{},n{};std::array<std::array<float,2>,5> uv{};
 const std::array<std::uint32_t,3> triangles[]={{1,2,3},{3,2,1}},gap[]={{2,3,1}};
 NativeGeometryView view;view.recipe=std::make_shared<NativeGeometryRecipe>();view.recipe->polygons={2,0};view.model=10;view.fvf=D3DFVF_XYZ|D3DFVF_NORMAL|D3DFVF_DIFFUSE|D3DFVF_SPECULAR|D3DFVF_TEX1;
 view.vertices=5;view.first=1;view.range=3;view.positions=p.data();view.normals=n.data();view.uv[0]=uv.data();view.triangles=triangles;view.triangleCount=2;view.gapTriangles=gap;view.gapCount=1;
 DecodedGeometry geometry;Check(SUCCEEDED(BuildNativeGeometry(view,geometry)),"gap filler subset builds");Check(geometry.indices==std::vector<std::uint32_t>({1,2,0,0,1,2}),"local range normalization and gap winding preserved");
 Check(geometry.fixedVertices[0].diffuse[0]==1&&geometry.fixedVertices[0].specular[0]==1,"missing native color arrays retain original white fallback");
 for(unsigned mode=0;mode<8;++mode){auto broken=view;broken.recipe=std::make_shared<NativeGeometryRecipe>();broken.recipe->polygons=view.recipe->polygons;
  if(mode==0)broken.positions=nullptr;if(mode==1)broken.normals=nullptr;if(mode==2)broken.uv[0]=nullptr;if(mode==3)broken.fvf|=D3DFVF_TEXCOORDSIZE3(0);
  if(mode==4)broken.range=99;if(mode==5)broken.recipe->polygons={99};if(mode==6)broken.first=2;if(mode==7)broken.fvf=D3DFVF_XYZRHW|D3DFVF_NORMAL;
  const auto before=geometry.fixedVertices;const auto indices=geometry.indices;
  Check(FAILED(BuildNativeGeometry(broken,geometry)),"invalid/missing native input rejected");Check(std::memcmp(before.data(),geometry.fixedVertices.data(),before.size()*192)==0&&indices==geometry.indices,"failed construction does not publish partial geometry");
 }
 p[1][0]=std::numeric_limits<float>::quiet_NaN();Check(FAILED(BuildNativeGeometry(view,geometry)),"nonfinite position rejected");p[1][0]=0;
 auto stale=view;stale.current=[](const NativeGeometryView&){return false;};Check(FAILED(BuildNativeGeometry(stale,geometry)),"stale native array registration cannot be dereferenced");
 Check(currentNativeGeometry==nullptr,"no borrowed view outside native render");
 {ScopedNativeGeometry outer(&view);Check(currentNativeGeometry==&view,"native scoped view active");{ScopedNativeGeometry inner(nullptr);Check(currentNativeGeometry==nullptr,"fallback draw clears outer view");}Check(currentNativeGeometry==&view,"nested view restored");}
 Check(currentNativeGeometry==nullptr,"scope cannot leak native array pointers to deferred draws");
 auto source=std::make_shared<NativeGeometryRecipe>();GroundRoadInput road;road.vertex=road.index=source;road.indexed=true;
 Check(road.Live(),"terrain exclusions accept generic native recipe ownership");source.reset();Check(!road.Live(),"native recipe expiration cannot hit cached terrain mask");
 auto owner=std::make_shared<NativeGeometryRecipe>();auto cached=std::make_shared<DecodedGeometry>(geometry);GeometryCache cache;
 auto key=view.Key();cache.Insert(key,cached,owner,owner);owner.reset();Check(!cache.Find(key),"cache drops expired native registration");
 std::cout<<"{\"passed\":true,\"checks\":"<<checks<<",\"legacy_native_vertex_pairs\":"<<pairs<<",\"max_vertex_index\":70004,\"all_8_UV_channels\":true,\"pointer_bits\":"<<sizeof(void*)*8<<",\"game_started\":false}";
}
