// SPDX-License-Identifier: GPL-3.0-or-later
// No window, game, sound, keyboard/mouse or GPU device initialization.
#include "CompatibilityRenderer.h"
#include "SortedColorPalette.h"
#include "NativeSubmitGate.h"
#include "NativeStateMirror.h"
#include "DeferredFixedWrites.h"
#include <iostream>
#include <cstring>
using namespace GenZH::Modern;
static unsigned checks=0,snapshots=0;
static void Check(bool ok,const char *label){++checks;if(!ok){std::cerr<<label<<'\n';std::exit(1);}}
struct TestDevice final:IDirect3DDevice8 {
 NativeFixedState state{};unsigned reads=0;bool fail=false;
 #include "NativeSubmitTestDevice.inl"
};
static D3DMATRIX Matrix(unsigned i){
 D3DMATRIX m{};m._11=1+.01f*i;m._22=1+.02f*i;m._33=1+.03f*i;m._44=1;
 m._41=static_cast<float>(i);m._42=static_cast<float>(i*2);m._43=static_cast<float>(i*3);return m;
}
static void SameSnapshot(TestDevice &native,NativeFixedStateMirror &mirror,bool sorted=false){
 Check(mirror.Capture(native),"capture ready");
 CompatibilityRenderer renderer;DrawPacket oldPacket,newPacket;
 oldPacket.fvf=newPacket.fvf=native.state.vertexShader;
 auto &rs=oldPacket.MutableStates().render;
 rs[D3DRS_ZENABLE]=rs[D3DRS_ZWRITEENABLE]=rs[D3DRS_LIGHTING]=rs[D3DRS_SPECULARENABLE]=rs[D3DRS_COLORVERTEX]=1;
 rs[D3DRS_ZFUNC]=D3DCMP_LESSEQUAL;rs[D3DRS_AMBIENT]=0xff446688;rs[D3DRS_TEXTUREFACTOR]=0xffaabbcc;
 rs[D3DRS_ALPHATESTENABLE]=1;rs[D3DRS_ALPHAFUNC]=D3DCMP_GREATER;rs[D3DRS_ALPHAREF]=117;
 rs[D3DRS_DIFFUSEMATERIALSOURCE]=D3DMCS_COLOR1;rs[D3DRS_FOGCOLOR]=0xff112233;
 if(sorted){rs[D3DRS_ZWRITEENABLE]=0;rs[D3DRS_ALPHABLENDENABLE]=1;rs[D3DRS_SRCBLEND]=D3DBLEND_SRCALPHA;rs[D3DRS_DESTBLEND]=D3DBLEND_INVSRCALPHA;
  rs[D3DRS_STENCILENABLE]=1;rs[D3DRS_STENCILFUNC]=D3DCMP_EQUAL;rs[D3DRS_STENCILPASS]=D3DSTENCILOP_INCR;rs[D3DRS_STENCILREF]=9;rs[D3DRS_STENCILWRITEMASK]=0x80;}
 for(unsigned i=0;i<8;++i){auto &s=oldPacket.MutableStates().stages[i];s[D3DTSS_COLOROP]=D3DTOP_MODULATE;s[D3DTSS_ALPHAOP]=D3DTOP_SELECTARG1;
  s[D3DTSS_TEXCOORDINDEX]=i;s[D3DTSS_TEXTURETRANSFORMFLAGS]=D3DTTFF_COUNT2;
  s[D3DTSS_COLORARG1]=D3DTA_TEXTURE;s[D3DTSS_COLORARG2]=D3DTA_DIFFUSE;s[D3DTSS_ALPHAARG1]=D3DTA_TEXTURE;
 }
 newPacket.MutableStates().render=rs;newPacket.MutableStates().stages=oldPacket.MutableStates().stages;
 NativeMeshSource source{11,12,13,14,77,0,1,2,false,false};source.vehicleWear.age={.25f,.5f,.75f,1.f};source.vehicleWear.profile={2.f,4.f,0.f,0.f};ScopedNativeMeshSource scoped(source);
 source.sorted=sorted;ScopedNativeMeshSource sortedScope(source);
 native.reads=0;Check(SUCCEEDED(renderer.Snapshot(&native,oldPacket,true)),"legacy production Snapshot");
 Check(native.reads>=18,"old path actually uses native getters");native.reads=0;
 renderer.SetDerivedStateReuse(true);Check(SUCCEEDED(renderer.Snapshot(&native,newPacket,true,&mirror.Value())),"mirrored production Snapshot");
 Check(native.reads==0,"hot fixed Snapshot has no native getters");
 Check(std::memcmp(&oldPacket.constants,&newPacket.constants,sizeof(LegacyConstants))==0,"all current constant bytes match");
 Check(oldPacket.MutableStates().render==newPacket.States().render&&oldPacket.MutableStates().stages==newPacket.States().stages,"render/UV stage values identical");
 Check(newPacket.nativeSource.root==11&&newPacket.nativeSource.bone==2&&newPacket.nativeSource.drawable==77,"native source attachment preserved");
 Check(newPacket.constants.vehicleAge[0]==.25f&&newPacket.constants.vehicleAge[3]==1.f&&newPacket.constants.vehicleProfile[0]==2.f,"per-vehicle appearance preserved by actual Snapshot");
 Check(newPacket.constants.modernMaterial[0]==(sorted?0.f:1.f)&&newPacket.constants.viewport[2]==1920,"original PBR eligibility and viewport preserved, including depth-write-disabled overlays");++snapshots;
}
struct SubmitOwner {unsigned calls=0;bool handle=true;long result=E_FAIL;NativeIndexedDraw captured{};};
static bool Dispatch(void *owner,const NativeIndexedDraw &draw,long &result){auto &x=*static_cast<SubmitOwner*>(owner);++x.calls;x.captured=draw;result=x.result;return x.handle;}

// Exercise the exact production deferrer with a failure-injecting native sink.
// It cannot learn invalid values, and synchronization failures cannot silently
// become successful modern draws or poison state block/getter snapshots.
static void CheckDeferredWrites(){
 struct Sink {
  NativeFixedState state{};unsigned writes=0;bool fail=false;
  HRESULT SetTransform(D3DTRANSFORMSTATETYPE s,const D3DMATRIX *m){++writes;if(fail)return E_FAIL;if(!m)return E_POINTER;
   const int i=s==D3DTS_WORLD?0:s==D3DTS_VIEW?1:s==D3DTS_PROJECTION?2:-1;if(i<0)return E_INVALIDARG;state.transforms[i]=*m;return S_OK;}
  HRESULT SetMaterial(const D3DMATERIAL8 *m){++writes;if(fail)return E_FAIL;if(!m)return E_POINTER;state.material=*m;return S_OK;}
 } sink;
 TestDevice device;for(unsigned i=0;i<11;++i)device.state.transforms[i]=Matrix(i);device.state.material.Power=1;
 NativeFixedStateMirror mirror;Check(mirror.Capture(device),"defer cold capture");sink.state=device.state;
 DeferredFixedWrites deferred;auto a=Matrix(120),b=Matrix(121),view=Matrix(122);auto ma=device.state.material,mb=ma;ma.Power=2;mb.Power=3;
 // No guesses about validation: every different value is first forwarded.
 Check(SUCCEEDED(deferred.Transform(sink,mirror,D3DTS_WORLD,&a))&&SUCCEEDED(deferred.Transform(sink,mirror,D3DTS_WORLD,&b)),"learn accepted worlds");
 Check(SUCCEEDED(deferred.Transform(sink,mirror,D3DTS_VIEW,&view)),"learn accepted view");
 Check(SUCCEEDED(deferred.Material(sink,mirror,&ma))&&SUCCEEDED(deferred.Material(sink,mirror,&mb)),"learn accepted materials");
 const auto before=sink.writes;
 Check(deferred.Begin(true,mirror)&&!deferred.Begin(true,mirror),"bounded scope rejects nesting");
 Check(SUCCEEDED(deferred.Transform(sink,mirror,D3DTS_WORLD,&a))&&SUCCEEDED(deferred.Material(sink,mirror,&ma)),"known values defer");
 Check(sink.writes==before&&deferred.Dirty()==5&&mirror.Value().material.Power==2,"no native write, complete modern mirror current");
 Check(!std::memcmp(&mirror.Value().transforms[0],&a,sizeof(a))&&!std::memcmp(&sink.state.transforms[0],&b,sizeof(b)),"native remains pending until barrier");
 Check(SUCCEEDED(deferred.Sync(sink))&&sink.writes==before+2&&!deferred.Dirty(),"getter/fallback/state-block barrier flushes exact dirty fields");
 Check(!std::memcmp(&sink.state.transforms[0],&a,sizeof(a))&&sink.state.material.Power==2,"barrier values exact");
 const auto repeat=sink.writes;Check(SUCCEEDED(deferred.Transform(sink,mirror,D3DTS_WORLD,&a))&&sink.writes==repeat,"unchanged live state remains deduplicated");
 for(unsigned i=0;i<100;++i){Check(SUCCEEDED(deferred.Transform(sink,mirror,D3DTS_WORLD,i%2?&a:&b)),"known world alternation");
  Check(SUCCEEDED(deferred.Material(sink,mirror,i%2?&ma:&mb)),"known material alternation");}
 Check(sink.writes==repeat&&deferred.counts.deferred>=202,"100 state alternations do not call legacy device");
 Check(SUCCEEDED(deferred.End(sink))&&!deferred.Active()&&!deferred.Dirty(),"end restores native state");
 Check(!deferred.Begin(false,mirror),"disabled feature has original path");
 {ScopedDeferredFixedReference ref(true);Check(!deferred.Begin(true,mirror),"explicit GPU reference has original path");}
 mirror.BeginBlock(S_OK);Check(!deferred.Begin(true,mirror),"state block recording cannot defer");
 const auto recorded=sink.writes;Check(SUCCEEDED(deferred.Material(sink,mirror,&ma))&&sink.writes==recorded+1,"recording forwards unchanged material");
 mirror.EndBlock(S_OK);Check(!deferred.Begin(true,mirror),"invalidated mirror cannot defer");device.state=sink.state;Check(mirror.Capture(device),"reseed after block");
 Check(deferred.Begin(true,mirror),"scope resumes after barrier");
 Check(SUCCEEDED(deferred.Transform(sink,mirror,D3DTS_WORLD,&b)),"known pending before invalid native input");
 const auto old=mirror.Value().material;Check(FAILED(deferred.Material(sink,mirror,nullptr))&&!std::memcmp(&old,&mirror.Value().material,sizeof(old)),"invalid input keeps native validation and old mirror");
 Check(!deferred.Dirty()&&!std::memcmp(&sink.state.transforms[0],&b,sizeof(b)),"pending synchronized before invalid native input");
 auto unknown=Matrix(555);sink.fail=true;Check(FAILED(deferred.Transform(sink,mirror,D3DTS_WORLD,&unknown)),"unknown failed setter never learned");sink.fail=false;
 const auto unlearned=sink.writes;Check(SUCCEEDED(deferred.Transform(sink,mirror,D3DTS_WORLD,&unknown))&&sink.writes==unlearned+1,"failed value must still take native validation on retry");
 Check(SUCCEEDED(deferred.Transform(sink,mirror,D3DTS_WORLD,&a)),"known value pending before device failure");
 sink.fail=true;Check(FAILED(deferred.End(sink))&&!deferred.Active()&&deferred.Dirty(),"sync failure retains pending state and closes scope");
 const auto failed=sink.writes;Check(FAILED(deferred.Material(sink,mirror,&mb))&&sink.writes==failed,"latched failure cannot authorize draw state");sink.fail=false;
 deferred.Reset();mirror.Reset();device.state=sink.state;Check(mirror.Capture(device),"new device epoch reseed");
 Check(deferred.Begin(true,mirror),"reset clears failure");const auto reset=sink.writes;
 Check(SUCCEEDED(deferred.Transform(sink,mirror,D3DTS_WORLD,&a))&&sink.writes==reset+1,"reset removes old validation permission");
 Check(SUCCEEDED(deferred.Transform(sink,mirror,D3DTS_WORLD,&unknown))&&sink.writes==reset+2,"failed unknown value did not enter accepted cache");
 auto projection=Matrix(777);const auto projected=sink.writes;Check(SUCCEEDED(deferred.Transform(sink,mirror,D3DTS_PROJECTION,&projection))&&sink.writes==projected+1,"projection retains native path");
 Check(SUCCEEDED(deferred.End(sink)),"final scope closed");
 // Cache collisions are exact-value misses, never false validation hits.
 AcceptedFixedValues<D3DMATRIX,1> collision;collision.Remember(D3DTS_WORLD,a);
 Check(collision.Has(D3DTS_WORLD,a)&&!collision.Has(D3DTS_VIEW,a)&&!collision.Has(D3DTS_WORLD,b),"validation cache compares tag and all bytes");collision.Remember(D3DTS_WORLD,b);
 Check(!collision.Has(D3DTS_WORLD,a)&&collision.Has(D3DTS_WORLD,b),"collision replacement stays conservative");collision.Clear();Check(!collision.Has(D3DTS_WORLD,b),"epoch clears validation");
}

static void CheckSceneBindings(){
 ScenePipelineBindings cache;unsigned calls=0;int a=0,b=0;auto bind=[&]{++calls;};
 cache.Single(ScenePipelineBindings::VS,&a,bind);cache.Single(ScenePipelineBindings::VS,&a,bind);
 Check(calls==2,"outside bounded loop every binding forwarded");
 Check(cache.Begin(true)&&!cache.Begin(true),"scene cache rejects nested ownership");
 for(unsigned i=0;i<10;++i){const auto slot=static_cast<ScenePipelineBindings::Slot>(i);const auto before=calls;
  cache.Single(slot,nullptr,bind);cache.Single(slot,nullptr,bind);Check(calls==before+1,"first null binds, repeated null skips");
  cache.Single(slot,&a,bind);cache.Single(slot,&a,bind);cache.Single(slot,&b,bind);cache.Single(slot,nullptr,bind);
  Check(calls==before+4,"exact shader/state/buffer pointer transitions forwarded");}
 auto before=calls;cache.Depth(&a,1,bind);cache.Depth(&a,1,bind);cache.Depth(&a,2,bind);cache.Depth(&b,2,bind);
 Check(calls==before+3,"stencil ref participates independently of state pointer");
 before=calls;cache.Vertex(&a,40,0,bind);cache.Vertex(&a,40,0,bind);cache.Vertex(&a,48,0,bind);cache.Vertex(&a,48,4,bind);cache.Vertex(&b,48,4,bind);
 Check(calls==before+4,"vertex buffer stride and offset fully represented");
 before=calls;std::array<float,6> vp{0,0,1920,1080,0,1};cache.Viewport(vp,bind);cache.Viewport(vp,bind);
 for(unsigned i=0;i<6;++i){auto changed=vp;changed[i]+=.1f;cache.Viewport(changed,bind);cache.Viewport(vp,bind);}
 Check(calls==before+13,"viewport dimensions and near/far cannot alias");
 std::array<int*,8> samplers{};before=calls;cache.Samplers(samplers,bind);cache.Samplers(samplers,bind);
 for(unsigned i=0;i<8;++i){samplers[i]=&a;cache.Samplers(samplers,bind);cache.Samplers(samplers,bind);}
 Check(calls==before+9,"all eight sampler slots compared");
 std::array<int*,10> textures{};before=calls;cache.Textures(textures,bind);cache.Textures(textures,bind);
 for(unsigned i=0;i<10;++i){textures[i]=&b;cache.Textures(textures,bind);cache.Textures(textures,bind);}
 Check(calls==before+11,"all ten base/normal/ORM resource slots compared");
 int *extra[2]{},*wear[3]{};before=calls;cache.Extra(extra,bind);cache.Extra(extra,bind);cache.Wear(wear,bind);cache.Wear(wear,bind);
 for(unsigned i=0;i<2;++i){extra[i]=&a;cache.Extra(extra,bind);}for(unsigned i=0;i<3;++i){wear[i]=&b;cache.Wear(wear,bind);}
 Check(calls==before+7,"height/emissive and all wear slots preserved");
 before=calls;cache.Topology(4,bind);cache.Topology(4,bind);cache.Topology(3,bind);Check(calls==before+2,"topology transition never skipped");
 Check(cache.skips>30&&cache.requests==cache.calls+cache.skips,"actual cache counts every binding and skip");
 cache.End();before=calls;cache.Single(ScenePipelineBindings::VS,&a,bind);cache.Single(ScenePipelineBindings::VS,&a,bind);Check(calls==before+2,"outside loop helper state never memoized");
 {ScopedScenePipelineBindings scope(cache,true);before=calls;cache.Single(ScenePipelineBindings::VS,&a,bind);Check(calls==before+1,"new frame starts cold even for same pointer");
  {ScopedScenePipelineBindings nested(cache,true);}Check(cache.Active(),"rejected nested scope cannot end its parent");}
 Check(!cache.Active(),"RAII exits scene cache before helper passes");
 {ScopedSceneBindingsReference reference(true);ScopedScenePipelineBindings scope(cache,true);before=calls;
  cache.Single(ScenePipelineBindings::VS,&a,bind);cache.Single(ScenePipelineBindings::VS,&a,bind);Check(calls==before+2&&!cache.Active(),"explicit reference rebinds every draw");}
 {ScopedScenePipelineBindings scope(cache,false);before=calls;cache.Single(ScenePipelineBindings::VS,&a,bind);cache.Single(ScenePipelineBindings::VS,&a,bind);Check(calls==before+2,"feature off retains old bindings");}
 try{ScopedScenePipelineBindings scope(cache,true);throw 1;}catch(int){}Check(!cache.Active(),"exception leaves no cached binding permission");
}

template<std::size_t N>static void CheckResolvedKeys(const std::array<unsigned,N> &inputs){
 std::array<DWORD,256> states{};
 auto key=ResolvedStateKey(states,inputs,false);
 ResolvedStateCache<N+1,unsigned,1> cache;unsigned owned=123,calls=0;unsigned *out=nullptr;
 auto resolve=[&](unsigned *&value){++calls;value=&owned;return S_OK;};
 Check(SUCCEEDED(cache.Get(key,true,out,resolve))&&out==&owned&&calls==1,"cold immutable state resolved");
 Check(SUCCEEDED(cache.Get(key,true,out,resolve))&&calls==1,"warm immutable state reused");
 for(unsigned i=0;i<N;++i){
  auto changed=states;changed[inputs[i]]=0x80000100u+i;
  const auto other=ResolvedStateKey(changed,inputs,false);const auto before=calls;
  Check(other!=key,"each original descriptor input is in the exact key");
  Check(SUCCEEDED(cache.Get(other,true,out,resolve))&&calls==before+1,"changed descriptor input reaches original resolver despite forced collision");
  Check(SUCCEEDED(cache.Get(other,true,out,resolve))&&calls==before+1,"changed state warm hit");
  Check(SUCCEEDED(cache.Get(key,true,out,resolve))&&calls==before+2,"forced collision cannot substitute another immutable state");
 }
 auto extra=ResolvedStateKey(states,inputs,true);auto before=calls;
 Check(SUCCEEDED(cache.Get(extra,true,out,resolve))&&calls==before+1,"terrain/MRT/PBR eligibility flag participates");
 Check(SUCCEEDED(cache.Get(extra,true,out,resolve))&&calls==before+1,"eligibility flag warm hit");
 {ScopedResolvedStatesReference ref(true);Check(SUCCEEDED(cache.Get(extra,true,out,resolve))&&calls==before+2,"reference invokes old resolver even with warm state");
  {ScopedResolvedStatesReference nested(false);}Check(resolvedStatesReference,"nested reference restores parent");}
 Check(!resolvedStatesReference,"reference restored at boundary");
 Check(SUCCEEDED(cache.Get(extra,false,out,resolve))&&calls==before+3,"off invokes old resolver with warm state");
 cache.Clear();Check(SUCCEEDED(cache.Get(extra,true,out,resolve))&&calls==before+4,"device reset invalidates all owned-value references");
 auto error=[&](unsigned *&value){++calls;value=nullptr;return E_OUTOFMEMORY;};cache.Clear();before=calls;
 Check(cache.Get(key,true,out,error)==E_OUTOFMEMORY&&!out,"native resolver failure preserved");
 Check(cache.Get(key,true,out,error)==E_OUTOFMEMORY&&calls==before+2,"failure never authorizes warm lookup");
 Check(SUCCEEDED(cache.Get(key,true,out,resolve))&&calls==before+3,"successful retry learns only valid state");
 auto nullSuccess=[&](unsigned *&value){++calls;value=nullptr;return S_OK;};cache.Clear();before=calls;
 Check(cache.Get(key,true,out,nullSuccess)==E_UNEXPECTED&&!out,"invalid successful null cannot be used");
 Check(cache.Get(key,true,out,nullSuccess)==E_UNEXPECTED&&calls==before+2,"null result never remembered");
 Check(cache.counts.requests==cache.counts.hits+cache.counts.misses+cache.counts.bypassed,"every request accounted exactly");
 Check(cache.counts.failures==4&&cache.counts.bypassed==2,"failure and reference/off counters exact");
}
static void CheckResolvedStates(){
 CheckResolvedKeys(RasterInputStates);CheckResolvedKeys(BlendInputStates);CheckResolvedKeys(DepthInputStates);CheckResolvedKeys(SamplerInputStates);
 // Resolved values are owned by the production unordered descriptor maps;
 // rehash cannot invalidate their addresses, while reset must clear the cache.
 std::unordered_map<unsigned,unsigned> values;values[1]=17;unsigned calls=0,*out=nullptr;
 ResolvedStateCache<2,unsigned,1> cache;
 auto resolve=[&](unsigned *&v){++calls;v=&values.at(1);return S_OK;};
 Check(SUCCEEDED(cache.Get({1,2},true,out,resolve))&&*out==17,"owned map value cached");
 for(unsigned i=2;i<10000;++i)values[i]=i;
 Check(SUCCEEDED(cache.Get({1,2},true,out,resolve))&&*out==17&&calls==1,"descriptor map rehash retains valid references");
 cache.Clear();values.clear();values[1]=39;
 Check(SUCCEEDED(cache.Get({1,2},true,out,resolve))&&*out==39&&calls==2,"new device epoch resolves new owned value");
 // The real 64-slot table reuses nonconsecutive keys, not just a last-key memo.
 ResolvedStateCache<2,unsigned> table;unsigned a=1,b=2,ra=0,rb=0;
 auto one=[&](unsigned *&v){++ra;v=&a;return S_OK;};auto two=[&](unsigned *&v){++rb;v=&b;return S_OK;};
 for(unsigned i=0;i<100;++i){Check(SUCCEEDED(table.Get({1,2},true,out,one))&&*out==1,"alternating exact state A");Check(SUCCEEDED(table.Get({2,3},true,out,two))&&*out==2,"alternating exact state B");}
 Check(ra==1&&rb==1&&table.counts.hits==198,"nonconsecutive exact states avoid repeated resolution");
}

static void CheckStateComOwnership(){
 struct State final:IUnknown {
  ULONG refs=1;unsigned &releases,&destroyed;
  State(unsigned &r,unsigned &d):releases(r),destroyed(d){}
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID,void **out)override{if(!out)return E_POINTER;*out=this;AddRef();return S_OK;}
  ULONG STDMETHODCALLTYPE AddRef()override{return ++refs;}
  ULONG STDMETHODCALLTYPE Release()override{++releases;const auto n=--refs;if(!n){++destroyed;delete this;}return n;}
 };
 unsigned releases=0,destroyed=0;
 // Negative control proves the test observes WRL's overloaded address release.
 {ComPtr<IUnknown> sample;sample.Attach(new State(releases,destroyed));
  ComPtr<IUnknown> *unsafe=&sample;
  Check(unsafe==std::addressof(sample)&&!sample&&releases==1&&destroyed==1,"WRL overloaded address releases owner: negative control");}
 releases=destroyed=0;
 BinaryStateTable<unsigned,ComPtr<IUnknown>> table;
 auto &one=table.Get(1);one.Attach(new State(releases,destroyed));auto *rawOne=one.Get();
 auto &two=table.Get(2);two.Attach(new State(releases,destroyed));auto *rawTwo=two.Get();
 Check(table.Get(1).Get()==rawOne&&table.Get(2).Get()==rawTwo&&!releases,"descriptor table address memo preserves COM ownership on alternation");
 ResolvedStateCache<2,ComPtr<IUnknown>> cache;ComPtr<IUnknown> *out=nullptr;unsigned calls=0;
 auto a=[&](ComPtr<IUnknown> *&owner){++calls;owner=std::addressof(table.Get(1));return S_OK;};
 auto b=[&](ComPtr<IUnknown> *&owner){++calls;owner=std::addressof(table.Get(2));return S_OK;};
 for(unsigned i=0;i<1000;++i){
  Check(SUCCEEDED(cache.Get({1,2},true,out,a))&&out->Get()==rawOne,"resolved actual WRL state A remains owned");
  Check(SUCCEEDED(cache.Get({2,3},true,out,b))&&out->Get()==rawTwo,"resolved actual WRL state B remains owned");
 }
 Check(calls==2&&cache.counts.hits==1998&&!releases&&!destroyed,"2000 exact state requests create two owners and never release/recreate");
 Check(table.size()==2,"state table owns complete immutable set");
 cache.Clear();Check(!releases,"clearing weak memo does not release map owners");
 table.clear();Check(releases==2&&destroyed==2,"device epoch clears each state owner exactly once");
}

static void CheckSortedPalette(){
 DrawPacket a;a.nativeSource={1,2,3,0,4,0,0,1,false,true};
 a.fvf=D3DFVF_XYZ|D3DFVF_NORMAL|D3DFVF_DIFFUSE|D3DFVF_TEX2;
 a.geometry=std::make_shared<DecodedGeometry>();a.geometry->sharedVertices=std::make_shared<SharedNativeVertices>();
 a.geometry->sharedVertices->twoUVVertices.resize(70018);a.geometry->sharedIndices=std::make_shared<SharedBufferedIndices>();
 a.geometry->sharedIndices->indices={UINT_MAX,UINT_MAX,70004,65536,3,70003,65535,4};
 a.geometry->sharedIndexFirst=2;a.geometry->sharedIndexBase=13;a.geometry->indices={70017,65549,16};
 auto m=Matrix(11);std::memcpy(a.constants.world,&m,64);m=Matrix(19);std::memcpy(a.constants.normalWorld,&m,64);
 auto c=a;c.geometry=std::make_shared<DecodedGeometry>(*a.geometry);c.geometry->sharedIndexFirst=5;c.geometry->indices={70016,65548,17};
 m=Matrix(29);std::memcpy(c.constants.world,&m,64);m=Matrix(37);std::memcpy(c.constants.normalWorld,&m,64);
 Check(SameSortedColorState(a,c)&&ConsecutiveSortedColor(a,a,c),"different exact transforms can share contiguous color range");
 SortedColorPalettes tables(true);tables.Add(a);tables.Add(c);auto *table=tables.Find(a);
 Check(table&&table->matrices.size()==2&&table->selectors[70017]==0&&table->selectors[70016]==1,"high R32 base vertex selectors preserve source ranges");
 Check(!std::memcmp(&table->matrices[0],a.constants.world,64)&&!std::memcmp(table->matrices[0].normal,a.constants.normalWorld,64),"first full world/normal matrices byte exact");
 Check(!std::memcmp(&table->matrices[1],c.constants.world,64)&&!std::memcmp(table->matrices[1].normal,c.constants.normalWorld,64),"second full world/normal matrices byte exact");
 Check(table->selectors[0]==UINT_MAX&&!table->Create(nullptr),"unused vertices not sampled; missing GPU falls back");
 auto gap=c;gap.geometry=std::make_shared<DecodedGeometry>(*c.geometry);gap.geometry->sharedIndexFirst=6;
 Check(!ConsecutiveSortedColor(a,a,gap),"index gap cannot merge");gap.geometry->sharedIndexFirst=4;Check(!ConsecutiveSortedColor(a,a,gap),"index overlap cannot merge");
 gap.geometry->sharedIndexFirst=5;gap.geometry->sharedIndexBase=14;Check(!SameSortedColorState(a,gap),"different base remains separate");
 auto changed=c;changed.geometry=std::make_shared<DecodedGeometry>(*c.geometry);changed.geometry->sharedVertices=std::make_shared<SharedNativeVertices>(*c.geometry->sharedVertices);
 Check(!SameSortedColorState(a,changed),"another immutable VB revision cannot merge");
 changed.geometry->sharedVertices=a.geometry->sharedVertices;changed.geometry->sharedIndices=std::make_shared<SharedBufferedIndices>(*c.geometry->sharedIndices);
 Check(!SameSortedColorState(a,changed),"another immutable IB revision cannot merge");
 for(unsigned i=0;i<256;++i){auto x=c;x.MutableStates().render[i]^=1;Check(!SameSortedColorState(a,x),"every render state including dynamic stencil preserved");}
 for(unsigned stage=0;stage<8;++stage)for(unsigned i=0;i<32;++i){auto x=c;x.MutableStates().stages[stage][i]^=1;Check(!SameSortedColorState(a,x),"every texture-stage transform/state preserved");}
 for(std::size_t at=0;at<sizeof(LegacyConstants);at+=4){
  if(at<64||(at>=offsetof(LegacyConstants,normalWorld)&&at<offsetof(LegacyConstants,normalWorld)+64))continue;
  if(at>=offsetof(LegacyConstants,materialDiffuse)&&at<offsetof(LegacyConstants,materialEmissive))continue;
  auto x=c;auto *bytes=reinterpret_cast<unsigned char*>(&x.constants);bytes[at]^=1;
  Check(!SameSortedColorState(a,x),"all non-transform appearance constant bytes preserved");
 }
 for(unsigned color=0;color<12;++color){
  auto varied=c;float *fields[]={varied.constants.materialDiffuse,varied.constants.materialAmbient,varied.constants.materialSpecular};fields[color/4][color%4]=.123f+color*.031f;
  Check(SameSortedColorState(a,varied),"VS-only colors may use independent GPU selectors");
  {ScopedSortedMaterialPaletteReference ref(true);Check(!SameSortedColorState(a,varied),"material palette off keeps original split");}
  SortedColorPalettes colors(true);colors.Add(a);colors.Add(varied);auto *t=colors.Find(a);
  Check(t&&t->matrices.size()==2&&!std::memcmp(t->matrices[1].diffuse,varied.constants.materialDiffuse,48),"all three material colors preserved byte exact");
  auto conflict=a;std::memcpy(conflict.constants.materialDiffuse,varied.constants.materialDiffuse,48);colors.Add(conflict);
  Check(!colors.Find(a),"shared vertex with inconsistent material rejects complete table");
 }
 for(unsigned i=0;i<8;++i){auto x=c;x.textures[i]=std::make_shared<TextureData>();Check(!SameSortedColorState(a,x),"every texture identity preserved");}
 auto conflict=c;conflict.geometry=std::make_shared<DecodedGeometry>(*c.geometry);conflict.geometry->indices={70017,65548,17};
 tables.Add(conflict);Check(!tables.Find(a)&&!tables.Find(c),"shared vertex with different world rejects whole source table");
 SortedColorPalettes same(true);same.Add(a);same.Add(a);Check(same.Find(a)&&same.Find(a)->matrices.size()==1,"repeat with identical transform valid");
 auto invalid=a;invalid.geometry=std::make_shared<DecodedGeometry>(*a.geometry);invalid.geometry->indices={70018,1,2};same.Add(invalid);Check(!same.Find(a),"invalid index cannot reach shader");
 invalid=a;invalid.constants.normalWorld[0]=std::numeric_limits<float>::quiet_NaN();SortedColorPalettes finite(true);finite.Add(invalid);Check(!finite.Find(a),"nonfinite normal matrix falls back");
 SortedColorPalettes disabled(false);disabled.Add(a);Check(!disabled.Find(a),"feature off has original path");
 {ScopedSortedColorPaletteReference ref(true);SortedColorPalettes original(true);original.Add(a);Check(!original.Find(a),"pixel reference forces original per-draw matrices");}
 auto tooLarge=std::make_shared<SharedNativeVertices>();tooLarge->twoUVVertices.resize(SortedColorPalette::ByteLimit/(2*sizeof(UINT))+1);
 SortedColorPalette limited(tooLarge);Check(limited.invalid&&limited.selectors.empty(),"table budget bypass before allocating selectors");
 const auto reject=[&](DrawPacket x){Check(!SortedColorPacket(x),"unsupported packet keeps original draw");};
 auto x=a;x.nativeSource.skinned=true;reject(x);x=a;x.nativeSource.sorted=false;reject(x);x=a;x.pixelShader=1;reject(x);
 x=a;x.vertexShader=1;reject(x);x=a;x.terrain=true;reject(x);x=a;x.outputOverlay=true;reject(x);x=a;x.groundOverlay=true;reject(x);
 x=a;x.groundExclusionOverlay=true;reject(x);x=a;x.constants.geometryFlags[0]=1;reject(x);x=a;x.MutableStates().render[D3DRS_CLIPPLANEENABLE]=1;reject(x);
}

#include "Capture80Checks.inl"
int main(){
 CheckCapture80();
 CheckSortedPalette();
 CheckDeferredWrites();CheckSceneBindings();CheckResolvedStates();CheckStateComOwnership();
 TestDevice native;
 for(unsigned i=0;i<11;++i)native.state.transforms[i]=Matrix(i);
 native.state.material.Diffuse={.7f,.4f,.2f,1};native.state.material.Ambient={.2f,.3f,.4f,1};
 native.state.material.Specular={.4f,.5f,.6f,1};native.state.material.Emissive={.01f,.02f,.03f,1};native.state.material.Power=19;
 native.state.viewport={0,0,1920,1080,.02f,.98f};native.state.vertexShader=D3DFVF_XYZ|D3DFVF_NORMAL|D3DFVF_TEX8;
 for(unsigned i=0;i<4;++i){auto &l=native.state.lights[i];l.Type=i%2?D3DLIGHT_SPOT:D3DLIGHT_DIRECTIONAL;
  l.Diffuse={.7f,.6f,.5f,1};l.Ambient={.1f,.2f,.3f,1};l.Specular={.8f,.9f,1,1};l.Position={1,2,3};l.Direction={0,0,-1};
  l.Attenuation0=1;l.Attenuation1=.1f;l.Attenuation2=.01f;l.Range=500;l.Theta=.3f;l.Phi=.8f;l.Falloff=2;
  native.state.lightValid[i]=true;native.state.enabled[i]=i%2?0:128;
 }
 NativeFixedStateMirror mirror;Check(mirror.Capture(native)&&mirror.Ready(),"initial state capture");
 Check(native.reads==21,"one cold capture reads 19 required values plus 2 enabled lights");
 native.reads=0;Check(mirror.Capture(native)&&native.reads==0,"warm capture performs zero getters");SameSnapshot(native,mirror);
 for(unsigned i=0;i<11;++i){auto matrix=Matrix(i+11);const auto state=i==0?D3DTS_WORLD:i==1?D3DTS_VIEW:i==2?D3DTS_PROJECTION:static_cast<D3DTRANSFORMSTATETYPE>(D3DTS_TEXTURE0+i-3);
  mirror.TransformChanged(state,&matrix,E_FAIL);Check(std::memcmp(&mirror.Value().transforms[i],&native.state.transforms[i],64)==0,"failed transform setter preserves old value");
  native.state.transforms[i]=matrix;mirror.TransformChanged(state,&matrix,S_OK);SameSnapshot(native,mirror);
 }
 auto material=native.state.material;material.Diffuse.r=.91f;material.Ambient.g=.53f;material.Specular.b=.18f;material.Emissive.r=.06f;material.Power=71;
 mirror.MaterialChanged(&material,E_FAIL);Check(mirror.Value().material.Power==19,"failed material preserves native value");
 native.state.material=material;mirror.MaterialChanged(&material,S_OK);SameSnapshot(native,mirror);
 auto viewport=native.state.viewport;viewport.X=20;viewport.Y=30;viewport.MinZ=.1f;viewport.MaxZ=.9f;
 native.state.viewport=viewport;mirror.ViewportChanged(&viewport,S_OK);SameSnapshot(native,mirror);
 // An initially disabled native light must be read before first enabling it.
 native.state.enabled[1]=TRUE;mirror.LightEnabled(1,TRUE,S_OK);Check(!mirror.Ready(),"enable of unread light invalidates");SameSnapshot(native,mirror);
 auto light=native.state.lights[1];light.Diffuse.r=.21f;light.Position.x=87;light.Range=800;
 native.state.lights[1]=light;mirror.LightChanged(1,&light,S_OK);SameSnapshot(native,mirror);
 mirror.BeginBlock(E_FAIL);Check(!mirror.Recording()&&mirror.Ready(),"failed block begin ignored");
 mirror.BeginBlock(S_OK);native.reads=0;Check(!mirror.Capture(native)&&native.reads==0,"recording cannot take direct capture");
 auto saved=mirror.Value().material;auto recorded=saved;recorded.Power=123;
 mirror.MaterialChanged(&recorded,S_OK);Check(mirror.Value().material.Power==saved.Power,"recorded setter does not alter live material");
 mirror.EndBlock(E_FAIL);Check(mirror.Recording(),"failed end retains recording");mirror.EndBlock(S_OK);Check(!mirror.Recording()&&!mirror.Ready(),"successful end requires native reseed");SameSnapshot(native,mirror);
 mirror.Invalidate();native.state.transforms[0]=Matrix(42);native.state.viewport.MinZ=.3f;SameSnapshot(native,mirror);
 mirror.Invalidate();native.fail=true;Check(!mirror.Capture(native)&&!mirror.Ready(),"native capture failure cannot commit partial state");
 native.fail=false;SameSnapshot(native,mirror);mirror.Reset();Check(!mirror.Ready()&&!mirror.Recording(),"device reset clears both flags");SameSnapshot(native,mirror);
 mirror.VertexShaderChanged(777,E_FAIL);Check(mirror.Value().vertexShader==native.state.vertexShader,"failed shader setter ignored");
 mirror.PixelShaderChanged(777,S_OK);Check(mirror.Value().pixelShader==777,"successful shader setter tracked");mirror.PixelShaderChanged(0,S_OK);
 // Dedup is valid only for known live successful state; block recording,
 // unknown/null inputs, failed writes and native reseeding retain validation.
 Check(mirror.SameMaterial(&native.state.material)&&mirror.SameViewport(&native.state.viewport),"known fixed values deduplicate");
 Check(mirror.SameTransform(D3DTS_WORLD,&native.state.transforms[0])&&mirror.SameLight(1,&native.state.lights[1]),"known transform/light deduplicate");
 Check(!mirror.SameTransform(static_cast<D3DTRANSFORMSTATETYPE>(999),&native.state.transforms[0])&&!mirror.SameMaterial(nullptr)&&!mirror.SameViewport(nullptr)&&!mirror.SameLight(4,&light),"invalid fixed inputs retain native validation");
 auto different=native.state.material;different.Power+=1;Check(!mirror.SameMaterial(&different),"changed material must be forwarded");
 mirror.MaterialChanged(&different,E_FAIL);Check(!mirror.SameMaterial(&different),"failed setter must not authorize dedup");
 mirror.BeginBlock(S_OK);Check(!mirror.SameMaterial(&native.state.material)&&!mirror.SameLightEnable(0,native.state.enabled[0]),"block recording never skips unchanged values");
 mirror.EndBlock(S_OK);Check(!mirror.SameMaterial(&native.state.material),"end block requires reseed");SameSnapshot(native,mirror);
 NativeStateMirror renderMirror;
 auto readRender=[](UINT state,DWORD *value)->HRESULT{if(state!=D3DRS_ZENABLE)return E_INVALIDARG;*value=1;return S_OK;};
 auto readStage=[](UINT stage,UINT state,DWORD *value)->HRESULT{if(stage!=0||state!=D3DTSS_COLOROP)return E_INVALIDARG;*value=D3DTOP_MODULATE;return S_OK;};
 Check(!renderMirror.SameRender(D3DRS_ZENABLE,1),"cold state not deduplicated");renderMirror.Capture(readRender,readStage);
 Check(renderMirror.SameRender(D3DRS_ZENABLE,1)&&renderMirror.SameStage(0,D3DTSS_COLOROP,D3DTOP_MODULATE),"known live render/stage values deduplicate");
 Check(!renderMirror.SameRender(0,0)&&!renderMirror.SameRender(256,0)&&!renderMirror.SameStage(0,0,0)&&!renderMirror.SameStage(8,0,0)&&!renderMirror.SameStage(0,32,0),"failed getters and out-of-range fields retain native validator");
 renderMirror.RenderChanged(D3DRS_ZENABLE,0,E_FAIL);Check(renderMirror.SameRender(D3DRS_ZENABLE,1),"failed native setter cannot update valid mirror");
 renderMirror.RenderChanged(D3DRS_ZENABLE,0,S_OK);Check(renderMirror.SameRender(D3DRS_ZENABLE,0),"successful native mutation updates live equality");
 renderMirror.BeginBlock(S_OK);Check(!renderMirror.SameRender(D3DRS_ZENABLE,0)&&!renderMirror.SameStage(0,D3DTSS_COLOROP,D3DTOP_MODULATE),"state blocks record all unchanged setters");
 renderMirror.EndBlock(S_OK);Check(!renderMirror.SameRender(D3DRS_ZENABLE,0),"state block completion invalidates dedup");
 renderMirror.Capture(readRender,readStage);Check(renderMirror.SameRender(D3DRS_ZENABLE,1),"native reseed replaces prior values");
 renderMirror.Reset();Check(!renderMirror.SameRender(D3DRS_ZENABLE,1),"device reset invalidates dedup");
 NativeMeshSource source{1,2,3,0,4,0,0,1,false,false};NativeIndexedDraw draw{D3DPT_TRIANGLELIST,19,70005,27,23000};
 Check(NativeSubmitContext(source,true,false,false),"rigid native context eligible");
 for(unsigned mode=0;mode<8;++mode){auto s=source;if(mode==0)s.root=0;if(mode==1)s.mesh=0;if(mode==2)s.resource=0;if(mode==3)s.skinned=true;if(mode==4)s.sorted=true;if(mode==5)s.pass=UINT32_MAX;
  Check(!NativeSubmitContext(s,mode!=6,mode==7,false),"unknown, skin, sorting, additional, off, offscreen contexts fall back");
 }
 Check(!NativeSubmitContext(source,true,false,true),"recording context falls back");Check(NativeSubmitLayout(mirror.Value(),false,draw),"normal XYZ triangle layout eligible");
 auto sorted=source;sorted.sorted=true;
 Check(NativeSortedSubmitContext(sorted,true,false,false),"sorted snapshots use mirrored state without reordering");
 Check(!NativeSortedSubmitContext(source,true,false,false)&&!NativeSortedSubmitContext(sorted,false,false,false)&&
  !NativeSortedSubmitContext(sorted,true,true,false)&&!NativeSortedSubmitContext(sorted,true,false,true),"ordinary/off/RTT/state-block draws retain full fallback");
 {ScopedSortedSubmitReference reference(true);Check(!NativeSortedSubmitContext(sorted,true,false,false),"actual original sorting snapshot reference selectable");}
 Check(NativeSortedSubmitContext(sorted,true,false,false),"reference scope restores production path");
 sorted.skinned=true;Check(!NativeSortedSubmitContext(sorted,true,false,false),"skinned ordering retains checked compatibility");
 SameSnapshot(native,mirror,true);
 for(unsigned mode=0;mode<6;++mode){auto s=mirror.Value();auto d=draw;if(mode==0)d.primitive=D3DPT_TRIANGLESTRIP;if(mode==1)s.pixelShader=1;if(mode==2)s.vertexShader=D3DFVF_XYZRHW|D3DFVF_NORMAL;if(mode==3)s.vertexShader=D3DFVF_XYZ;if(mode==4)s.vertexShader=D3DFVF_XYZB1|D3DFVF_NORMAL;
  Check(!NativeSubmitLayout(s,mode==5,d),"unsupported shaders, skin/RHW layout and topology rejected");
 }
 std::array<DWORD,256> states{};states[D3DRS_ZENABLE]=D3DZB_TRUE;states[D3DRS_ZWRITEENABLE]=1;states[D3DRS_ZFUNC]=D3DCMP_LESSEQUAL;
 Check(NativeSubmitState(states),"opaque state eligible");states[D3DRS_ALPHATESTENABLE]=1;Check(NativeSubmitState(states),"cutout state eligible");
 for(auto field:{D3DRS_ALPHABLENDENABLE,D3DRS_STENCILENABLE,D3DRS_CLIPPLANEENABLE}){states[field]=1;Check(!NativeSubmitState(states),"blend/stencil/clip retained on old path");states[field]=0;}

 states[D3DRS_STENCILENABLE]=1;states[D3DRS_STENCILFUNC]=D3DCMP_ALWAYS;states[D3DRS_STENCILFAIL]=D3DSTENCILOP_KEEP;states[D3DRS_STENCILZFAIL]=D3DSTENCILOP_KEEP;states[D3DRS_STENCILPASS]=D3DSTENCILOP_REPLACE;
 for(DWORD mask:{0u,0x80u,0xffu,0xffffffffu}){states[D3DRS_STENCILWRITEMASK]=mask;states[D3DRS_STENCILREF]=192;Check(NativeSubmitState(states),"player/occluder marking preserves all masks and refs");}
 for(DWORD func:{DWORD(D3DCMP_EQUAL),DWORD(D3DCMP_NEVER)}){states[D3DRS_STENCILFUNC]=func;Check(!NativeSubmitState(states),"readback/occlusion overlays retain compatibility");}
 states[D3DRS_STENCILFUNC]=D3DCMP_ALWAYS;
 for(auto field:{D3DRS_STENCILFAIL,D3DRS_STENCILZFAIL}){states[field]=D3DSTENCILOP_REPLACE;Check(!NativeSubmitState(states),"rejected-pixel mutations never bypass");states[field]=D3DSTENCILOP_KEEP;}
 states[D3DRS_STENCILPASS]=D3DSTENCILOP_INCR;Check(!NativeSubmitState(states),"counting stencil retains triangle ordering");states[D3DRS_STENCILENABLE]=0;
 states[D3DRS_ALPHABLENDENABLE]=1;states[D3DRS_SRCBLEND]=D3DBLEND_ONE;states[D3DRS_DESTBLEND]=D3DBLEND_ZERO;Check(NativeSubmitState(states),"identity blend is opaque");states[D3DRS_ALPHABLENDENABLE]=0;
 states[D3DRS_ZENABLE]=D3DZB_USEW;Check(!NativeSubmitState(states),"W buffer falls back");states[D3DRS_ZENABLE]=D3DZB_TRUE;
 states[D3DRS_ZWRITEENABLE]=0;Check(!NativeSubmitState(states),"no depth write falls back");states[D3DRS_ZWRITEENABLE]=1;
 states[D3DRS_ZFUNC]=D3DCMP_EQUAL;Check(!NativeSubmitState(states),"depth equal multipass falls back");states[D3DRS_ZFUNC]=D3DCMP_LESS;Check(NativeSubmitState(states),"strict depth accepted");
 Check(NativeSubmitBindings(true,true,32,0,0),"static binding eligible");
 Check(!NativeSubmitBindings(false,true,32,0,0)&&!NativeSubmitBindings(true,false,32,0,0)&&!NativeSubmitBindings(true,true,0,0,0),"missing resource or zero stride falls back");
 Check(!NativeSubmitBindings(true,true,32,D3DUSAGE_DYNAMIC,0)&&!NativeSubmitBindings(true,true,32,0,D3DUSAGE_DYNAMIC),"dynamic resources fall back");
 NativeMeshDispatch dispatch;SubmitOwner owner,other;int device=1,oldDevice=2;long result=S_OK;
 Check(!dispatch.Try(&device,draw,result)&&!dispatch.Try(nullptr,draw,result),"unregistered native device ignored");
 dispatch.Connect(&device,&owner,Dispatch);Check(!dispatch.Try(&oldDevice,draw,result)&&owner.calls==0,"foreign COM address never cast");
 Check(dispatch.Try(&device,draw,result)&&FAILED(result)&&owner.calls==1,"handled failure cannot trigger second draw");
 Check(owner.captured.vertices==70005&&owner.captured.firstIndex==27&&owner.captured.firstVertex==19,"native uint32 ranges retained");
 dispatch.Disconnect(&oldDevice,&owner);Check(dispatch.Try(&device,draw,result),"foreign disconnect ignored");
 dispatch.Connect(&oldDevice,&other,Dispatch);dispatch.Disconnect(&device,&owner);Check(dispatch.Try(&oldDevice,draw,result)&&other.calls==1,"old device destruction cannot disconnect replacement");
 other.handle=false;Check(!dispatch.Try(&oldDevice,draw,result),"not eligible chooses compatibility");dispatch.Disconnect(&oldDevice,&other);Check(!dispatch.Try(&oldDevice,draw,result),"exact lifetime disconnect clears dispatch");
 std::cout<<"{\"passed\":true,\"radix_200k_ms\":"<<radix80Ms<<",\"legacy_200k_ms\":"<<legacy80Ms<<",\"cached_50k_snapshot_ms\":"<<snapshot80Ms<<",\"uncached_50k_snapshot_ms\":"<<uncached80Ms<<",\"checks\":"<<checks<<",\"production_snapshot_pairs\":"<<snapshots<<",\"constant_bytes_per_pair\":"<<sizeof(LegacyConstants)<<",\"mirrored_snapshot_getters\":0,\"pointer_bits\":"<<sizeof(void*)*8<<",\"game_started\":false,\"GPU_created\":false,\"deferred_fixed_failure_checks\":true,\"scene_binding_boundary_checks\":true,\"resolved_state_failure_key_reset_checks\":true,\"state_COM_ownership_checks\":true}";
}
