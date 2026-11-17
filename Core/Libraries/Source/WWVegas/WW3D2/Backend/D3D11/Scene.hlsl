// SPDX-License-Identifier: GPL-3.0-or-later
// Linear-light opaque/cutout PBR pass, directional shadow and SDR presentation.
#include "MaterialLighting.hlsl"
cbuffer MeshData : register(b0) {
    row_major float4x4 World;
    row_major float4x4 ViewProjection;
    row_major float4x4 NormalWorld;
    float4 BaseColor;
    float4 Surface;
    float4 Emissive;
};
cbuffer FrameData : register(b1) {
    float4 CameraExposure;
    float4 SunDirectionIntensity;
    float4 SunColor;
    float4 Ambient;
    float4 ShadowSettings;
    row_major float4x4 LightViewProjection;
};
Texture2D Albedo : register(t0);
Texture2D NormalMap : register(t1);
Texture2D ORM : register(t2);
Texture2D<float> ShadowMap : register(t3);
SamplerState MaterialSampler : register(s0);
SamplerComparisonState ShadowSampler : register(s1);
#define GENZH_SUN_SHADOW_ARRAY 0
#define GENZH_SUN_SHADOW_TEXTURE ShadowMap
#define GENZH_SUN_SHADOW_SAMPLER ShadowSampler
#include "DirectionalPCSS.hlsl"
#undef GENZH_SUN_SHADOW_ARRAY
#undef GENZH_SUN_SHADOW_TEXTURE
#undef GENZH_SUN_SHADOW_SAMPLER

struct VSInput { float3 position:POSITION; float3 normal:NORMAL; float2 uv:TEXCOORD0; };
struct VSOutput {
    float4 position:SV_POSITION;
    float3 worldPosition:TEXCOORD0;
    float3 worldNormal:TEXCOORD1;
    float2 uv:TEXCOORD2;
    float4 shadowPosition:TEXCOORD3;
};
VSOutput VSMain(VSInput input) {
    VSOutput output;
    float4 world=mul(float4(input.position,1),World);
    output.position=mul(world,ViewProjection);
    output.worldPosition=world.xyz;
    output.worldNormal=mul(float4(input.normal,0),NormalWorld).xyz;
    output.uv=input.uv;
    output.shadowPosition=mul(world,LightViewProjection);
    return output;
}

float3 SafeNormalize(float3 value) { return value*rsqrt(max(dot(value,value),1e-12)); }
float3 SurfaceNormal(VSOutput input) {
    float3 n=SafeNormalize(input.worldNormal);
    float3 dpdx=ddx(input.worldPosition), dpdy=ddy(input.worldPosition);
    float2 duvdx=ddx(input.uv), duvdy=ddy(input.uv);
    // Derivative cotangent frame permits legacy meshes without stored tangents.
    float3 dpdyPerp=cross(dpdy,n), dpdxPerp=cross(n,dpdx);
    float3 t=dpdyPerp*duvdx.x+dpdxPerp*duvdy.x;
    float3 b=dpdyPerp*duvdx.y+dpdxPerp*duvdy.y;
    float scale=rsqrt(max(max(dot(t,t),dot(b,b)),1e-12));
    float3 map=NormalMap.Sample(MaterialSampler,input.uv).xyz*2-1;
    map.xy*=Surface.w;
    return SafeNormalize(t*scale*map.x+b*scale*map.y+n*map.z);
}
float Visibility(float4 shadowPosition,float3 worldDx,float3 worldDy) {
    if(ShadowSettings.w<0.5 || shadowPosition.w<=0) return 1;
    float3 position=shadowPosition.xyz/shadowPosition.w;
    float2 uv=position.xy*float2(.5,-.5)+.5;
    if(any(uv<0) || any(uv>1) || position.z<=0 || position.z>=1) return 1;
    return DirectionalPCSS(uv,position.z,ShadowSettings.x,0,ShadowSettings.yz,DirectionalShadowScale(LightViewProjection),DirectionalReceiverGradient(LightViewProjection,worldDx,worldDy));
}
float4 PSMain(VSOutput input):SV_TARGET {
    float4 albedo=Albedo.Sample(MaterialSampler,input.uv)*BaseColor;
    clip(albedo.a-Surface.z);
    float3 orm=ORM.Sample(MaterialSampler,input.uv).rgb;
    float roughness=clamp(orm.g*Surface.x,.045,1);
    float metalness=saturate(orm.b*Surface.y);
    float3 n=SurfaceNormal(input),v=SafeNormalize(CameraExposure.xyz-input.worldPosition);
    roughness=FilteredRoughness(roughness,n);
    MaterialBRDFResponse surfaceBRDF=LoadMaterialBRDF(roughness,saturate(dot(n,v)),lerp(.04.xxx,albedo.rgb,metalness));
    float3 l=SafeNormalize(SunDirectionIntensity.xyz);
    float3 radiance=SunColor.rgb*SunDirectionIntensity.w;
    float3 direct=EvaluateGGX(albedo.rgb,roughness,metalness,n,v,l,surfaceBRDF.energy)*radiance*Visibility(input.shadowPosition,ddx(input.worldPosition),ddy(input.worldPosition));
    float3 indirect=EvaluateIndirect(albedo.rgb,roughness,metalness,n,v,Ambient.rgb,orm.r,SafeNormalize(input.worldNormal),surfaceBRDF);
    return float4(HDRSurfaceRadiance(direct+indirect+Emissive.rgb),albedo.a);
}
void PSShadow(VSOutput input) {
    clip(Albedo.Sample(MaterialSampler,input.uv).a*BaseColor.a-Surface.z);
}
struct PresentOutput { float4 position:SV_POSITION; float2 uv:TEXCOORD0; };
PresentOutput VSPresent(uint vertex:SV_VertexID) {
    PresentOutput output;
    output.uv=float2((vertex<<1)&2,vertex&2);
    output.position=float4(output.uv*float2(2,-2)+float2(-1,1),0,1);
    return output;
}
Texture2D HDRScene : register(t0);
Texture1D GammaCurve : register(t1);
SamplerState PresentSampler : register(s0);
cbuffer PostData:register(b2){float4 PostSettings;float4 PostBloom;float4 PostGrade;}; // post106: x glow composite scale, y exposure trim; photo107: grade settings (LUT built from them; w = LUT on)
Texture3D<float4> ColourLut:register(t3);
Texture2D BloomTexture:register(t2);
float3 Bloom(float2 uv){
 float3 sum=0;
 [unroll]for(int y=-1;y<=1;++y)[unroll]for(int x=-1;x<=1;++x){
  float3 sampleColor=HDRScene.Sample(PresentSampler,uv+float2(x,y)*PostSettings.xy*3).rgb;
  // Soft highlight rolloff; emissive/light contributions remain linear here.
  float luminance=dot(sampleColor,float3(.2126,.7152,.0722));
  sum+=sampleColor*max(0,luminance-1)/max(luminance,1e-4);
 }
 return sum/9*PostSettings.w;
}
float4 PSTonemap(PresentOutput input):SV_TARGET {
    float3 hdr=max(0,HDRScene.Sample(PresentSampler,input.uv).rgb*CameraExposure.w*PostBloom.y);
    if(PostSettings.w>0)hdr+=Bloom(input.uv)*CameraExposure.w*PostBloom.y;
    // post106: mip-chain glow of the world image (exposed units, built before the HUD).
    if(PostBloom.x>0)hdr+=BloomTexture.SampleLevel(PresentSampler,input.uv,0).rgb*PostBloom.x*PostBloom.y;
    // Fitted filmic curve; the SRGB RTV performs the final transfer conversion.
    float3 mapped=saturate((hdr*(2.51*hdr+.03))/(hdr*(2.43*hdr+.59)+.14));
    // photo107: colour LUT after the tone curve, linear in and out (PostGrade.w = LUT on). The LUT
    // holds the grade (saturation, highlight warmth, contrast about 18% grey); see BuildColourLut.
    if(PostGrade.w>.5)mapped=ColourLut.SampleLevel(PresentSampler,mapped*(31.0/32.0)+.5/32.0,0).rgb;
    float3 encoded=EncodeSRGB(mapped);
    float3 coordinate=(encoded*255+.5)/256;
    encoded=float3(GammaCurve.Sample(PresentSampler,coordinate.r).r,GammaCurve.Sample(PresentSampler,coordinate.g).g,GammaCurve.Sample(PresentSampler,coordinate.b).b);
    return float4(DecodeSRGB(encoded),1);
}
// Edge-directed spatial filtering of the tone-mapped scene. HUD/text are drawn
// afterwards, so they retain their pixel resolution and do not enter this filter.
Texture2D TonemappedScene:register(t0);
float Luma(float3 color){return dot(EncodeSRGB(color),float3(.299,.587,.114));}
float4 PSSceneAA(PresentOutput input):SV_TARGET{
 float2 uv=input.uv,step=PostSettings.xy;
 float3 center=TonemappedScene.Sample(PresentSampler,uv).rgb;
 float nw=Luma(TonemappedScene.Sample(PresentSampler,uv+step*float2(-1,-1)).rgb);
 float ne=Luma(TonemappedScene.Sample(PresentSampler,uv+step*float2(1,-1)).rgb);
 float sw=Luma(TonemappedScene.Sample(PresentSampler,uv+step*float2(-1,1)).rgb);
 float se=Luma(TonemappedScene.Sample(PresentSampler,uv+step*float2(1,1)).rgb);
 float middle=Luma(center),low=min(middle,min(min(nw,ne),min(sw,se))),high=max(middle,max(max(nw,ne),max(sw,se)));
 if(high-low<max(.0312,high*.125))return float4(center,1);
 float2 direction=float2(-(nw+ne-sw-se),nw+sw-ne-se);
 float reduce=max((nw+ne+sw+se)*(.25*.125),1.0/128);
 direction=clamp(direction/(min(abs(direction.x),abs(direction.y))+reduce),-8,8)*step;
 float3 inner=.5*(TonemappedScene.Sample(PresentSampler,uv-direction/6).rgb+TonemappedScene.Sample(PresentSampler,uv+direction/6).rgb);
 float3 outer=inner*.5+.25*(TonemappedScene.Sample(PresentSampler,uv-direction*.5).rgb+TonemappedScene.Sample(PresentSampler,uv+direction*.5).rgb);
 float candidate=Luma(outer);return float4(candidate<low||candidate>high?inner:outer,1);
}

// photo107: screen-space indirect light (GENZH_GI), half resolution. Each pixel casts four rays
// around its normal, rotated per pixel and per frame so the temporal resolve averages the noise.
// A ray marches four steps out to the radius in world space; the first visible surface it passes
// behind contributes its lit radiance times this pixel's albedo. Only the lit HDR is read here.
cbuffer GlobalIlluminationData:register(b13){
 row_major float4x4 GIViewProjection,GIInverseViewProjection;
 float4 GIEye;        // xyz eye (unused in the gather)
 float4 GIDepthRange; // x raw depth minimum, y raw depth maximum, z strength, w radius (world units)
 float4 GIFrame;      // x frame index, y full-resolution width, z full-resolution height
};
Texture2D<float> GIDepth:register(t24);
Texture2D<float4> GINormal:register(t25);
Texture2D<float4> GIAlbedo:register(t26);
Texture2D<float4> GIRadiance:register(t27);
Texture2D<float4> GIHdr:register(t28);
float GIDepthNormalized(float depth){return (depth-GIDepthRange.x)/max(GIDepthRange.y-GIDepthRange.x,1e-6);}
float3 GIWorld(float2 uv,float depth){
 float4 world=mul(float4(uv.x*2-1,1-uv.y*2,GIDepthNormalized(depth),1),GIInverseViewProjection);
 return world.xyz/world.w;
}
float4 PSGlobalIllumination(PresentOutput input):SV_TARGET{
 float2 uv=input.uv;
 float depth=GIDepth.SampleLevel(PresentSampler,uv,0);
 if(depth>=GIDepthRange.y-1e-5)return float4(0,0,0,1);
 float4 normalOcclusion=GINormal.SampleLevel(PresentSampler,uv,0);
 float3 albedo=GIAlbedo.SampleLevel(PresentSampler,uv,0).rgb;
 float normalLength=dot(normalOcclusion.xyz,normalOcclusion.xyz);
 if(normalLength<.25||max(albedo.r,max(albedo.g,albedo.b))<=0)return float4(0,0,0,1);
 float3 N=normalOcclusion.xyz*rsqrt(normalLength);
 float3 P=GIWorld(uv,depth);
 float3 seed=abs(N.z)<.9?float3(0,0,1):float3(1,0,0);
 float3 T=normalize(cross(seed,N)),B=cross(N,T);
 float radius=GIDepthRange.w;
 float noise=frac(52.9829189*frac(dot(input.position.xy+GIFrame.x*5.588238,float2(.06711056,.00583715))));
 float3 gathered=0;
 [unroll]for(int k=0;k<4;++k){
  float u1=frac((k+noise)*.25),u2=frac(k*.618034+noise);
  float r=sqrt(u1),phi=6.2831853*u2;
  // Cosine-weighted direction in the hemisphere of N.
  float3 direction=T*(r*cos(phi))+B*(r*sin(phi))+N*sqrt(max(1-u1,0));
  [loop]for(int s=1;s<=4;++s){
   float3 Q=P+direction*(radius*s*.25);
   float4 clip=mul(float4(Q,1),GIViewProjection);
   if(clip.w<=1e-4)break;
   float2 q=float2(clip.x/clip.w*.5+.5,.5-clip.y/clip.w*.5);
   if(any(q<0)||any(q>1))break;
   float sceneDepth=GIDepth.SampleLevel(PresentSampler,q,0);
   if(sceneDepth>=GIDepthRange.y-1e-5)continue;
   // View depth of the visible surface at q: the ray point is behind it by 'thickness'.
   float sceneW=mul(float4(GIWorld(q,sceneDepth),1),GIViewProjection).w;
   float thickness=clip.w-sceneW;
   if(thickness>.05&&thickness<radius*.5){
    gathered+=GIHdr.SampleLevel(PresentSampler,q,0).rgb*saturate(dot(N,direction))*(1-(s-1)*.25);
    break;
   }
  }
 }
 return float4(albedo*gathered*(.25*GIDepthRange.z),1);
}
float4 PSGlobalCompose(PresentOutput input):SV_TARGET{
 return float4(GIRadiance.SampleLevel(PresentSampler,input.uv,0).rgb,1);
}
