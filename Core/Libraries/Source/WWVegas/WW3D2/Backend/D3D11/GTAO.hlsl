// SPDX-License-Identifier: GPL-3.0-or-later
// XeGTAO core is separately MIT licensed; see XeGTAO-LICENSE.txt.
#define VA_SATURATE saturate
#define XE_GTAO_USE_HALF_FLOAT_PRECISION 0
#define XE_GTAO_FP32_DEPTHS 1
#include "XeGTAO.h"
#include "XeGTAO.hlsli"
cbuffer GTAOFrame : register(b13) {
 GTAOConstants AO;
 row_major float4x4 InverseVP;
 row_major float4x4 CameraView;
 row_major float4x4 CameraProjection;
 float4 CameraViewport;
 float4 CameraDepthRange;
 float4 CameraEye;
 float4 CameraSunDirection;
 uint4 FilterDimensions; // previous width/height, output width/height
 float4 AOTuning; // strength, far depth, final denoise, reserved
 float4 ContactTuning; // strength, ray length, base thickness, normal bias
};
Texture2D<float> OriginalDepth : register(t37);
Texture2D<float4> IndirectDiffuse : register(t39);
Texture2D<float4> IndirectSpecular : register(t40);
Texture2D<float4> MappedNormalAO : register(t41);
Texture2D<float4> SunDirect : register(t42);
Texture2D<float4> OriginalHDR : register(t44);
Texture2D<lpfloat> WorkingDepth : register(t45);
Texture2D<uint> WorkingAO : register(t46);
Texture2D<lpfloat> WorkingEdges : register(t47);
RWTexture2D<float> DepthOutput : register(u0);
RWTexture2D<uint> AOOutput : register(u1);
RWTexture2D<unorm float> EdgeOutput : register(u2);
SamplerState PointClamp : register(s12);
bool ValidSurface(int2 pixel) {
 if(any(pixel<0)||any(pixel>=AO.ViewportSize)||any(float2(pixel)+.5<CameraViewport.xy)||any(float2(pixel)+.5>=CameraViewport.xy+CameraViewport.zw))return false;
 float4 n=MappedNormalAO.Load(int3(pixel,0));
 return n.w>=1&&n.w<=2.01&&all(isfinite(n))&&dot(n.xyz,n.xyz)>.5;
}
[numthreads(8,8,1)]
void CSGTAODepth(uint2 pixel:SV_DispatchThreadID) {
 if(any(pixel>=FilterDimensions.zw))return;
 float depth=OriginalDepth.Load(int3(pixel,0));
 float ndc=(depth-CameraDepthRange.x)/(CameraDepthRange.y-CameraDepthRange.x);
 float z=AO.DepthUnpackConsts.x/max(AO.DepthUnpackConsts.y-ndc,1e-8);
 DepthOutput[pixel]=ValidSurface(int2(pixel))&&depth<CameraDepthRange.y&&isfinite(z)&&z>0?min(z,AOTuning.y):AOTuning.y;
}
// Intel's depth-range weighting, with normalized overlapping footprints so
// odd last rows/columns are retained instead of dropping them at every mip.
[numthreads(8,8,1)]
void CSGTAOReduce(uint2 pixel:SV_DispatchThreadID) {
 if(any(pixel>=FilterDimensions.zw))return;
 uint2 first=pixel*FilterDimensions.xy/FilterDimensions.zw;
 uint2 end=min(FilterDimensions.xy,((pixel+1)*FilterDimensions.xy+FilterDimensions.zw-1)/FilterDimensions.zw);
 float maximum=0;
 [loop]for(uint y=first.y;y<end.y;++y)[loop]for(uint x=first.x;x<end.x;++x)maximum=max(maximum,WorkingDepth.Load(int3(uint2(x,y),0)));
 float radius=.75*AO.EffectRadius*AO.RadiusMultiplier;
 float falloff=max(AO.EffectFalloffRange*radius,1e-5);
 float start=radius*(1-AO.EffectFalloffRange),weighted=0,weightSum=0;
 [loop]for(uint y2=first.y;y2<end.y;++y2)[loop]for(uint x2=first.x;x2<end.x;++x2){
  float z=WorkingDepth.Load(int3(uint2(x2,y2),0));float weight=saturate((start-(maximum-z))/falloff+1);
  weighted+=z*weight;weightSum+=weight;
 }
 DepthOutput[pixel]=weighted/max(weightSum,1e-8);
}
[numthreads(8,8,1)]
void CSGTAO(uint2 pixel:SV_DispatchThreadID) {
 if(any(pixel>=uint2(AO.ViewportSize)))return;
 if(!ValidSurface(int2(pixel))){AOOutput[pixel]=170;EdgeOutput[pixel]=0;return;}
 float3 worldNormal=normalize(MappedNormalAO.Load(int3(pixel,0)).xyz);
 float3 viewNormal=normalize(mul(float4(worldNormal,0),CameraView).xyz);viewNormal.z*=CameraDepthRange.z;
 uint index=HilbertIndex(pixel.x,pixel.y);
 // No animated sampling before motion vectors and a real TAA history exist.
 float2 noise=frac(.5+index*float2(.75487766624669276005,.56984029099805326591));
 XeGTAO_MainPass(pixel,3,3,noise,viewNormal,AO,WorkingDepth,PointClamp,AOOutput,EdgeOutput);
}
[numthreads(8,8,1)]
void CSGTAODenoise(uint2 thread:SV_DispatchThreadID) {
 uint2 pixel=thread*uint2(2,1);if(any(pixel>=uint2(AO.ViewportSize)))return;
 XeGTAO_Denoise(pixel,AO,WorkingAO,WorkingEdges,PointClamp,AOOutput,AOTuning.z>.5);
 // Preserve neutral invalid pixels; depth edges must not synthesize hidden data.
 if(!ValidSurface(int2(pixel)))AOOutput[pixel]=AOTuning.z>.5?255:170;
 if(pixel.x+1<uint(AO.ViewportSize.x)&&!ValidSurface(int2(pixel+uint2(1,0))))AOOutput[pixel+uint2(1,0)]=AOTuning.z>.5?255:170;
}
struct ScreenVertex {float4 position:SV_POSITION;};
ScreenVertex VSGTAO(uint id:SV_VertexID){ScreenVertex v;v.position=float4(id==1?3:-1,id==2?-3:1,0,1);return v;}
float AdditionalSpecularAO(float nv,float roughness,float authoredAO,float visibility) {
 if(visibility>=1)return 1;
 float power=exp2(-16*roughness-1);
 float before=saturate(pow(max(nv+authoredAO,1e-6),power)-1+authoredAO);
 float afterAO=authoredAO*visibility;
 float after=saturate(pow(max(nv+afterAO,1e-6),power)-1+afterAO);
 return saturate(after/max(before,1e-6));
}
bool ReconstructVisible(int2 pixel,out float3 world) {
 world=0;if(!ValidSurface(pixel))return false;
 float depth=OriginalDepth.Load(int3(pixel,0));
 float2 uv=(float2(pixel)+.5-CameraViewport.xy)/CameraViewport.zw;
 float4 position=mul(float4(uv.x*2-1,1-uv.y*2,(depth-CameraDepthRange.x)/(CameraDepthRange.y-CameraDepthRange.x),1),InverseVP);
 if(depth>=CameraDepthRange.y||abs(position.w)<=1e-8||!all(isfinite(position)))return false;
 world=position.xyz/position.w;return all(isfinite(world));
}
float ContactVisibility(float3 world,float3 normal) {
 if(ContactTuning.x<=0||CameraSunDirection.w<.5)return 1;
 float3 light=CameraSunDirection.xyz;if(dot(normal,light)<=0)return 1;
 float viewZ=abs(mul(float4(world,1),CameraView).z);
 float pixelFootprint=max(abs(AO.NDCToViewMul_x_PixelSize.x),abs(AO.NDCToViewMul_x_PixelSize.y))*viewZ;
 float planeBias=max(ContactTuning.w,min(pixelFootprint*.1,.25));
 float3 origin=world+normal*planeBias;
 float thickness=max(ContactTuning.z,min(pixelFootprint*.5,2));
 // Static bounded samples until actual motion vectors/history exist. No frame
 // jitter or animated noise that could create unfiltered shadow shimmer.
 [loop]for(uint sampleIndex=0;sampleIndex<16;++sampleIndex){
  // photo107: log-spaced samples: fine steps near the surface (small details), the same reach.
  float distance=ContactTuning.y*pow((float(sampleIndex)+.5)/16,1.6);
  float3 sampleWorld=origin+light*distance;
  float4 sampleView=mul(float4(sampleWorld,1),CameraView);
  float4 clip=mul(sampleView,CameraProjection);
  if(clip.w<=1e-8||!all(isfinite(clip)))break;
  float3 ndc=clip.xyz/clip.w;float2 uv=float2(ndc.x*.5+.5,.5-ndc.y*.5);
  if(any(uv<=0)||any(uv>=1)||ndc.z<=0||ndc.z>=1)break;
  int2 samplePixel=int2(CameraViewport.xy+uv*CameraViewport.zw);float3 occluder;
  if(!ReconstructVisible(samplePixel,occluder))continue;
  // Pixel snapping on a sloping flat receiver must not cast self shadows.
  // Hidden/shroud and unsupported material footprints cannot be occluders.
  if(dot(occluder-world,normal)<=planeBias)continue;
  float occluderZ=abs(mul(float4(occluder,1),CameraView).z);
  float delta=abs(sampleView.z)-occluderZ;
  float depthBias=max(.015,viewZ*2e-5);
  if(delta>depthBias&&delta<thickness){
   float border=min(min(uv.x,1-uv.x)*CameraViewport.z,min(uv.y,1-uv.y)*CameraViewport.w);
   float confidence=saturate(border/4)*saturate(1-distance/ContactTuning.y);
   return 1-saturate(ContactTuning.x)*confidence;
  }
 }
 return 1;
}
float4 PSGTAO(ScreenVertex vertex):SV_TARGET {
 int2 pixel=int2(vertex.position.xy);float4 result=OriginalHDR.Load(int3(pixel,0));
 if(ValidSurface(pixel)&&(AOTuning.x>0||ContactTuning.x>0)){
  float visibility=1;if(AOTuning.x>0)visibility=lerp(1,saturate(float(WorkingAO.Load(int3(pixel,0)))/255),saturate(AOTuning.x));
  float4 normalAO=MappedNormalAO.Load(int3(pixel,0));float4 specular=IndirectSpecular.Load(int3(pixel,0));
  float3 world;
  if(ReconstructVisible(pixel,world)){
   float3 normal=normalize(normalAO.xyz),toEye=CameraEye.xyz-world;
   float nv=saturate(dot(normal,toEye*rsqrt(max(dot(toEye,toEye),1e-12))));
   float specularVisibility=AdditionalSpecularAO(nv,saturate(specular.w),saturate(normalAO.w-1),visibility);
   float3 removal=IndirectDiffuse.Load(int3(pixel,0)).rgb*(1-visibility)+specular.rgb*(1-specularVisibility);
   float3 sun=SunDirect.Load(int3(pixel,0)).rgb;
   if(any(sun>1e-6)&&all(isfinite(sun))&&all(sun>=0))removal+=sun*(1-ContactVisibility(world,normal));
   if(all(isfinite(removal)))result.rgb=max(result.rgb-removal,0);
  }
 }
 return result;
}
