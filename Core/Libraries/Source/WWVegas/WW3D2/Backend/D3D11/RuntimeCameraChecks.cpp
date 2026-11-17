// SPDX-License-Identifier: GPL-3.0-or-later
#include "GTAOGPU.h"
#include "ShadowCascades.h"
#include "SceneVisibility.h"
#include "ShadowReuseState.h"
#include "GroundBlockRegistry.h"
#include "MaterialMemoryBudget.h"
#include <iostream>
#include <fstream>
#include <stdexcept>
using namespace GenZH::Modern;
using namespace DirectX;
static unsigned checks=0;
static void Check(bool value,const char *text){++checks;if(!value)throw std::runtime_error(text);}
static DrawPacket Opaque(){
 DrawPacket p;p.geometry=std::make_shared<DecodedGeometry>();p.geometry->boundsReady=true;
 const float low[]={-.1f,-.1f,.4f},high[]={.1f,.1f,.6f};std::copy_n(low,3,p.geometry->low);std::copy_n(high,3,p.geometry->high);p.geometry->indices={0,1,2};
 p.fvf=D3DFVF_XYZ|D3DFVF_NORMAL;p.primitive=D3DPT_TRIANGLELIST;
 for(float *m:{p.constants.world,p.constants.view,p.constants.projection,p.constants.normalWorld})for(unsigned i=0;i<16;++i)m[i]=i%5==0?1.f:0.f;
 p.constants.viewport[2]=1920;p.constants.viewport[3]=864;
 auto &s=p.States().render;s[D3DRS_ZENABLE]=D3DZB_TRUE;s[D3DRS_ZWRITEENABLE]=1;s[D3DRS_ZFUNC]=D3DCMP_LESS;
 s[D3DRS_STENCILENABLE]=1;s[D3DRS_STENCILFUNC]=D3DCMP_ALWAYS;s[D3DRS_STENCILFAIL]=s[D3DRS_STENCILZFAIL]=D3DSTENCILOP_KEEP;s[D3DRS_STENCILPASS]=D3DSTENCILOP_REPLACE;
 s[D3DRS_STENCILREF]=7;s[D3DRS_STENCILWRITEMASK]=127;return p;
}
int main(int argc,char **argv){try{
 Check(argc==2,"captured live camera fixture required");
 ScreenMaterialGPU::Camera liveCamera;UINT liveWidth=0,liveHeight=0;std::ifstream input(argv[1],std::ios::binary);
 input.read(reinterpret_cast<char*>(&liveCamera),sizeof(liveCamera));input.read(reinterpret_cast<char*>(&liveWidth),4);input.read(reinterpret_cast<char*>(&liveHeight),4);Check(bool(input),"original perf56 live camera loaded");
 GTAOGPU::Constants liveAO;Check(GTAOGPU::PrepareConstants(liveCamera,liveWidth,liveHeight,8,1,liveAO),"exact captured 1920x864 RH gameplay camera now accepted");
 auto livePlan=PlanShadowCascades(liveCamera.viewMatrix,liveCamera.projectionMatrix,liveCamera.sunDirection);Check(livePlan.valid&&livePlan.constants.split[3]==2,"exact live RH gameplay frustum now uses two cascades");
 for(float depth:{0.f,1.f}){
  XMFLOAT4X4 inverse,view;std::copy_n(liveCamera.inverseVP,16,&inverse._11);std::copy_n(liveCamera.viewMatrix,16,&view._11);
  const auto world=XMVector3TransformCoord(XMVectorSet(-1,-1,depth,1),XMLoadFloat4x4(&inverse));
  const float native=XMVectorGetZ(XMVector3TransformCoord(world,XMLoadFloat4x4(&view)));
  const float forward=XMVectorGetX(XMVector4Dot(world,XMLoadFloat4(reinterpret_cast<const XMFLOAT4*>(livePlan.constants.cameraDepth))));
  Check(forward>0&&std::abs(forward-std::abs(native))<.001f,"captured native near/far depths agree with GPU cascade selector");
 }
 for(bool right:{false,true}){
  XMFLOAT4X4 v,p;XMStoreFloat4x4(&v,right?XMMatrixLookAtRH(XMVectorSet(50,-80,100,1),XMVectorZero(),XMVectorSet(0,0,1,0)):XMMatrixLookAtLH(XMVectorSet(50,-80,100,1),XMVectorZero(),XMVectorSet(0,0,1,0)));
  XMStoreFloat4x4(&p,right?XMMatrixPerspectiveFovRH(.5f,1920.f/864,10,1110):XMMatrixPerspectiveFovLH(.5f,1920.f/864,10,1110));
  for(auto viewport:std::vector<std::array<float,4>>{{0,0,1920,1080},{0,0,1920,864},{13,19,1800,800}}){
   ScreenMaterialGPU::Camera camera;std::copy_n(&v._11,16,camera.viewMatrix);std::copy_n(&p._11,16,camera.projectionMatrix);
   XMFLOAT4X4 inverse;XMStoreFloat4x4(&inverse,XMMatrixInverse(nullptr,XMLoadFloat4x4(&v)*XMLoadFloat4x4(&p)));std::copy_n(&inverse._11,16,camera.inverseVP);
   std::copy(viewport.begin(),viewport.end(),camera.viewport);camera.depthRange[1]=1;camera.eye[0]=50;camera.eye[1]=-80;camera.eye[2]=100;camera.eye[3]=1;
   GTAOGPU::Constants constants;Check(GTAOGPU::PrepareConstants(camera,1920,1080,8,1,constants),"RTS and offset subviewports accepted for LH/RH cameras");
   for(float x:{.13f,.5f,.83f})for(float y:{.1f,.5f,.9f}){
    const float tx=(viewport[0]+viewport[2]*x)/1920,ty=(viewport[1]+viewport[3]*y)/1080;
    Check(std::abs(tx*constants.ao.NDCToViewMul.x+constants.ao.NDCToViewAdd.x-(2*x-1)/p._11)<.00001f,"target-NDC horizontal physical rays match original world camera");
    Check(std::abs(ty*constants.ao.NDCToViewMul.y+constants.ao.NDCToViewAdd.y-(1-2*y)/p._22)<.00001f,"target-NDC vertical physical rays preserve HUD viewport scale");
   }
   camera.viewport[0]=1920;Check(!GTAOGPU::PrepareConstants(camera,1920,1080,8,1,constants),"out-of-target camera rejected");
  }
  const float sun[]={.6f,.4f,.7f};auto plan=PlanShadowCascades(&v._11,&p._11,sun);Check(plan.valid&&plan.constants.split[0]>10&&plan.constants.split[2]>plan.constants.split[0],"RH/LH cascades use positive forward splits");
  for(float distance:{10.f,100.f,500.f}){
   const auto world=XMVector3TransformCoord(XMVectorSet(0,0,right?-distance:distance,1),XMMatrixInverse(nullptr,XMLoadFloat4x4(&v)));
   const float actual=XMVectorGetX(XMVector4Dot(world,XMLoadFloat4(reinterpret_cast<const XMFLOAT4*>(plan.constants.cameraDepth))));
   Check(std::abs(actual-distance)<.001f,"GPU cascade selection receives positive physical camera depth");
  }
 }
 auto a=Opaque(),outside=a;outside.constants.world[12]=3;
 auto unsafe=outside;unsafe.States().render[D3DRS_STENCILFUNC]=D3DCMP_EQUAL;
 auto normalFree=outside;normalFree.fvf=D3DFVF_XYZ;
 const auto before=outside.States().render;std::vector<DrawPacket> packets{a,outside,unsafe,normalFree};auto scene=PrepareCameraScene(packets);
 Check(scene.boundsTested==3&&scene.culled==2&&scene.fallback==1&&scene.packets.size()==2,"opaque safe stencil/no-normal geometry culled; unsafe stencil retained");
 Check(packets[1].States().render==before&&scene.packets[0]==&packets[0]&&scene.packets[1]==&packets[2],"visible packet order and player stencil state immutable");
 Check(PrepareCameraScene(packets,false).packets.size()==4,"diagnostic culling-off retains every packet");
 auto nearPacket=a;nearPacket.geometry=std::make_shared<DecodedGeometry>(*a.geometry);nearPacket.geometry->low[2]=-.1f;Check(!ProjectCameraBounds(nearPacket).valid,"near-plane crossings retain full rendering");
 auto skinned=outside;skinned.nativeSource.skinned=true;Check(!ProjectCameraBounds(skinned).valid,"skinned bound never guessed");
 Check(CanInstance(a),"idempotent player stencil marking eligible for instancing");
 const auto groups=GroupOpaque({&a,&outside});Check(groups.size()==1&&groups[0].size()==2,"adjacent identical stencil state can instance");
 auto differentRef=a;differentRef.States().render[D3DRS_STENCILREF]=8;Check(GroupOpaque({&a,&differentRef}).size()==2,"different player references never merge");
 ShadowDrawStamp sa,sb;Check(CaptureShadowStamp(a,sa),"opaque coverage reusable");auto changedColor=a;changedColor.constants.lightDiffuse[0][0]=.71f;changedColor.constants.view[12]=91;changedColor.constants.vehicleAge[0]=.9f;
 Check(CaptureShadowStamp(changedColor,sb)&&sa==sb,"lighting/camera/aging cannot invalidate unchanged opaque depth");
 Check(CaptureShadowStamp(outside,sb)&&!(sa==sb),"world pose invalidates opaque shadow reuse");
 auto changedMesh=a;changedMesh.sourceIdentity[0]=1;Check(CaptureShadowStamp(changedMesh,sb)&&!(sa==sb),"source geometry revision invalidates shadow reuse");
 auto cutout=a;cutout.constants.alphaStage[0][3]=D3DCMP_GREATER;Check(CaptureShadowStamp(cutout,sa),"cutout retains full alpha-aware stamp");cutout.constants.materialDiffuse[3]=.4f;Check(CaptureShadowStamp(cutout,sb)&&!(sa==sb),"alpha coverage changes cannot hit opaque fast stamp");
 D3D11GroundBlockRegistry registry;std::vector<D3D11GroundBlock> blocks{{10,20,15,7,.5f}};registry.Assign(blocks);const auto revision=registry.Revision();registry.Assign(blocks);Check(registry.Revision()==revision,"stationary building footprints retain exclusion revision");blocks[0].major+=1;registry.Assign(blocks);Check(registry.Revision()!=revision,"construction/footprint changes rebuild exclusion");
 TerrainExclusionInputs mask;auto atlas=std::make_shared<int>(1),otherAtlas=std::make_shared<int>(2);const std::array<float,4> dimensions{459,609,300,10};
 mask.CommitMap(atlas,7);mask.Commit(revision,dimensions,{});
 Check(mask.MapMatches(atlas,7)&&mask.Matches(revision,dimensions,{}),"stationary mask retained across unrelated draw barriers");
 Check(!mask.MapMatches(otherAtlas,7)&&!mask.MapMatches(atlas,8),"different atlas lifetime or revision never reuses a mask");
 Check(!mask.MapMatches({},7)&&!mask.Matches(revision+1,dimensions,{}),"unknown map and changed structure footprint cannot hit");
 atlas.reset();Check(!mask.MapMatches({},7),"weak atlas lifetime expires without retaining old maps");mask.Reset();
 Check(!mask.MapMatches(otherAtlas,7)&&!mask.Matches(revision,dimensions,{}),"device/map reset invalidates occupancy state");
 GeometryMemorySample memory;memory.videoKnown=memory.cpuKnown=true;memory.videoBudget=12ull*1024*1024*1024;memory.videoUsage=2ull*1024*1024*1024;memory.physicalAvailable=8ull*1024*1024*1024;memory.virtualAvailable=2ull*1024*1024*1024;
 Check(MaterialMemoryTarget(memory,512ull*1024*1024,32)==1024ull*1024*1024,"RTX class budget uses 1 GiB cache without map reduction in x86");
 Check(MaterialMemoryTarget(memory,512ull*1024*1024,64)==2048ull*1024*1024,"x64 material cache can use 2 GiB authored maps");
 memory.videoUsage=memory.videoBudget;Check(MaterialMemoryTarget(memory,512ull*1024*1024,32)<=512ull*1024*1024,"GPU budget pressure reclaims cache headroom");
 memory.videoUsage=0;memory.virtualAvailable=128ull*1024*1024;Check(MaterialMemoryTarget(memory,512ull*1024*1024,32)==128ull*1024*1024,"x86 virtual pressure bounded without downsampling");
 std::cout<<"{\"passed\":true,\"checks\":"<<checks<<",\"pointer_bits\":"<<sizeof(void*)*8<<",\"RTS_subviewport_LH_RH\":true,\"positive_cascade_depth\":true,\"safe_stencil_culling_instancing\":true,\"opaque_shadow_coverage_reuse\":true,\"adaptive_material_budget\":true,\"game_started\":false}";
 return 0;
}catch(const std::exception &e){std::cerr<<e.what();return 1;}}
