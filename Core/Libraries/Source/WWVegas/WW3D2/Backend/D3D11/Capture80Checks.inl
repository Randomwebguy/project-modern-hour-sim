// SPDX-License-Identifier: GPL-3.0-or-later
// Production CPU stages, checked without starting a game or creating a device.
#include "StableDepthSort.h"
#include <chrono>
#include <limits>
#include <vector>
#include "LegacyDepthSort80.inl"
static double radix80Ms=0,legacy80Ms=0,snapshot80Ms=0,uncached80Ms=0;
static void CheckCapture80(){
    TestDevice native;for(unsigned i=0;i<11;++i)native.state.transforms[i]=Matrix(i);
    native.state.viewport={0,0,1920,1080,0,1};native.state.vertexShader=D3DFVF_XYZ|D3DFVF_NORMAL|D3DFVF_TEX2;
    for(unsigned i=0;i<4;++i){native.state.enabled[i]=TRUE;native.state.lightValid[i]=true;
        native.state.lights[i].Type=D3DLIGHT_SPOT;native.state.lights[i].Theta=.5f;native.state.lights[i].Phi=1;}
    CompatibilityRenderer baseline,current;baseline.SetDerivedStateReuse(false);
    std::uint32_t rng=139847;auto random=[&](){rng=rng*1664525u+1013904223u;return rng;};
    for(unsigned i=0;i<2048;++i){
        if(i%4==0){
            const unsigned which=random()%11;native.state.transforms[which]=Matrix(random()%130);
            native.state.transforms[which]._12=.25f;native.state.transforms[which]._21=-.13f;
            native.state.material.Power=static_cast<float>(random()%180);
            const unsigned light=random()%4;native.state.enabled[light]=random()%2;native.state.lights[light].Theta=static_cast<float>(random()%80)*.01f;
        }
        DrawPacket a,b;a.fvf=b.fvf=native.state.vertexShader;
        a.MutableStates().render[D3DRS_LIGHTING]=a.MutableStates().render[D3DRS_ZENABLE]=a.MutableStates().render[D3DRS_ZWRITEENABLE]=1;
        a.MutableStates().render[D3DRS_ALPHABLENDENABLE]=i%3==0;a.MutableStates().render[D3DRS_FOGENABLE]=i%2;
        a.MutableStates().render[D3DRS_FOGCOLOR]=random();a.MutableStates().render[D3DRS_TEXTUREFACTOR]=random();
        b.stateSnapshot=a.stateSnapshot;
        for(unsigned t=0;t<8;++t){a.MutableStates().stages[t][D3DTSS_COLOROP]=D3DTOP_MODULATE;
            a.MutableStates().stages[t][D3DTSS_TEXCOORDINDEX]=(i+t)%8;a.MutableStates().stages[t][D3DTSS_TEXTURETRANSFORMFLAGS]=D3DTTFF_COUNT2;}
        b.stateSnapshot=a.stateSnapshot;
        NativeMeshSource source{};source.vehicleWear.age={i*.0001f,.1f,.3f,.8f};ScopedNativeMeshSource scope(source);
        Check(SUCCEEDED(baseline.Snapshot(&native,a,true,&native.state))&&SUCCEEDED(current.Snapshot(&native,b,true,&native.state)),"cached/uncached snapshot success");
        Check(std::memcmp(&a.constants,&b.constants,sizeof(LegacyConstants))==0,"derived memo never changes any constant byte across pose/light/UV/fog/wear transitions");
    }
    DerivedFixedState a,b;a.enabled=false;float outA[16]{},outB[16]{};
    const float special[]={0.f,-0.f,-1.f,1.f,std::numeric_limits<float>::infinity(),std::numeric_limits<float>::quiet_NaN()};
    for(float x:special){for(unsigned warm=0;warm<3;++warm){
        float powerA=a.Roughness(x),powerB=b.Roughness(x);Check(!std::memcmp(&powerA,&powerB,4),"roughness preserves signed zero/nonfinite input semantics");
        a.Spot(0,x,.7f,outA);b.Spot(0,x,.7f,outB);Check(!std::memcmp(outA,outB,8),"spot preserves nonfinite input semantics");}}
    D3DMATRIX singular{};
    for(unsigned n=0;n<2;++n){a.NormalWorld(&singular._11,outA);b.NormalWorld(&singular._11,outB);Check(!std::memcmp(outA,outB,64),"singular matrix unchanged");}
    Check(b.reuses>0&&b.calculations>0,"both derived cache hit and miss exercised");
    constexpr unsigned count=200000;std::vector<DepthEntry80> input(count),scratch,reference;StableDepthSort<DepthEntry80> sorter;
    for(unsigned i=0;i<count;++i){input[i].z=static_cast<float>(i)-100000.f;input[i].idx=i;
        input[i].tri={70000u+i*3,70001u+i*3,70002u+i*3};}
    for(unsigned i=count-1;i;--i)std::swap(input[i],input[random()%(i+1)]);
    reference=input;LegacyDepthSort80(reference.data(),reference.data()+reference.size());scratch=input;
    auto *result=sorter.Sort(scratch.data(),scratch.size());Check(result!=nullptr,"large uint32 radix succeeds");
    for(unsigned i=0;i<count;++i)Check(!std::memcmp(&reference[i],&result[i],sizeof(DepthEntry80)),"unique-depth ordering and >65535 indices identical to original sorter");
    for(unsigned i=0;i<count;++i){input[i].z=static_cast<float>(i%1024)-512.f;input[i].idx=i;}
    reference=input;std::stable_sort(reference.begin(),reference.end(),[](const auto &x,const auto &y){return x.z<y.z;});scratch=input;result=sorter.Sort(scratch.data(),scratch.size());
    for(unsigned i=0;i<count;++i)Check(!std::memcmp(&reference[i],&result[i],sizeof(DepthEntry80)),"ties retain insertion order with no index loss");
    Check(DepthSortKey(-0.f)==DepthSortKey(0.f)&&DepthSortKey(-1.f)<DepthSortKey(0.f)&&DepthSortKey(0.f)<DepthSortKey(1.f),"physical signed depth/zero order");
    Check(DepthSortKey(std::numeric_limits<float>::infinity())<DepthSortKey(std::numeric_limits<float>::quiet_NaN()),"NaNs are deterministic last");
    Check(sorter.Sort(nullptr,0)==nullptr,"empty input needs no dereference");
    Check(sorter.Sort(nullptr,64u*1024u*1024u/sizeof(DepthEntry80)+1)==nullptr,"scratch budget bypass preserves original algorithm, never truncates indices");
    // Paired CPU-only microbenchmark: not a game FPS or GPU utilization result.
    for(unsigned mode=0;mode<2;++mode){double elapsed=0;
        for(unsigned n=0;n<12;++n){scratch=input;const auto begin=std::chrono::steady_clock::now();
            if(mode==0)LegacyDepthSort80(scratch.data(),scratch.data()+scratch.size());else result=sorter.Sort(scratch.data(),scratch.size());
            elapsed+=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begin).count();}
        (mode==0?legacy80Ms:radix80Ms)=elapsed/12;
    }
    for(unsigned mode=0;mode<2;++mode){auto &renderer=mode==0?baseline:current;DrawPacket packet;packet.fvf=native.state.vertexShader;
        const auto begin=std::chrono::steady_clock::now();for(unsigned n=0;n<50000;++n)renderer.Snapshot(&native,packet,true,&native.state);
        (mode==0?uncached80Ms:snapshot80Ms)=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begin).count();
    }
}
