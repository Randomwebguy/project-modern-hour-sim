// SPDX-License-Identifier: GPL-3.0-or-later
// Directional PCSS: orthographic depth separation, deterministic disk sampling.
// The RTS sun uses its angular radius. No texture/model simplification or jitter.
static const float2 DirectionalShadowDisk[16]={
 float2(.176777,0),float2(-.225772,.206831),float2(.034558,-.393771),float2(.284572,.371173),
 float2(-.522223,-.092374),float2(.494696,-.314684),float2(-.165466,.615526),float2(-.315562,-.607594),
 float2(.684642,.250030),float2(-.712256,.293970),float2(.343354,-.733728),float2(.253731,.808910),
 float2(-.764745,-.443181),float2(.897133,-.197230),float2(-.547504,.778771),float2(-.126487,-.976090)};
float3 DirectionalShadowScale(row_major float4x4 light){
 float w=abs(light[3][3]);float z=length(float3(light[0][2],light[1][2],light[2][2]));
 float3 scales=float3(0,0,0);
 if(w>=1e-6&&z>=1e-8&&abs(light[0][3])+abs(light[1][3])+abs(light[2][3])<=1e-6)scales=float3(.5*length(float3(light[0][0],light[1][0],light[2][0]))/w,
               .5*length(float3(light[0][1],light[1][1],light[2][1]))/w,w/z);
 return scales;
}
float2 DirectionalReceiverGradient(row_major float4x4 light,float3 worldDx,float3 worldDy){
 float3 dx=mul(float4(worldDx,0),light).xyz/max(abs(light[3][3]),1e-6);
 float3 dy=mul(float4(worldDy,0),light).xyz/max(abs(light[3][3]),1e-6);
 dx.xy*=float2(.5,-.5);dy.xy*=float2(.5,-.5);float determinant=dx.x*dy.y-dx.y*dy.x;
 float2 gradient=float2(0,0);
 if(abs(determinant)>=1e-12)gradient=float2(dx.z*dy.y-dy.z*dx.y,dy.z*dx.x-dx.z*dy.x)/determinant;
 return gradient;
}
float2 DirectionalPenumbra(float separation,float3 scales,float2 texel){
 // tan(solar angular radius), typical Earth sun; geometric source size, not a blur slider.
 return clamp(max(separation,0)*scales.z*.00465*scales.xy,texel*.5,texel*12);
}
float DirectionalCompare(float2 uv,uint layer,float receiver){
#if GENZH_SUN_SHADOW_ARRAY
 return GENZH_SUN_SHADOW_TEXTURE.SampleCmpLevelZero(GENZH_SUN_SHADOW_SAMPLER,float3(uv,layer),receiver);
#else
 return GENZH_SUN_SHADOW_TEXTURE.SampleCmpLevelZero(GENZH_SUN_SHADOW_SAMPLER,uv,receiver);
#endif
}
float DirectionalDepth(int2 coord,uint layer){
#if GENZH_SUN_SHADOW_ARRAY
 return GENZH_SUN_SHADOW_TEXTURE.Load(int4(coord,layer,0));
#else
 return GENZH_SUN_SHADOW_TEXTURE.Load(int3(coord,0));
#endif
}
float DirectionalPCSS(float2 uv,float receiver,float bias,uint layer,float2 texel,float3 scales,float2 gradient){
 float visibility=1;
 [branch]if(scales.z<=0||any(texel<=0)){
  float result=0;[unroll]for(int y=-1;y<=1;++y)[unroll]for(int x=-1;x<=1;++x)result+=DirectionalCompare(uv+float2(x,y)*texel,layer,receiver-bias);
  visibility=result/9;
 }else{
 uint width=0,height=0;
#if GENZH_SUN_SHADOW_ARRAY
 uint layers=0;GENZH_SUN_SHADOW_TEXTURE.GetDimensions(width,height,layers);
#else
 GENZH_SUN_SHADOW_TEXTURE.GetDimensions(width,height);
#endif
 [branch]if(width>0&&height>0){
 float2 dimensions=float2(width,height);float2 search=DirectionalPenumbra(receiver,scales,texel);
 float blockerSum=0,blockers=0;
 [loop]for(uint searchTap=0;searchTap<17;++searchTap){
  float2 searchOffset=searchTap==16?float2(0,0):DirectionalShadowDisk[min(searchTap,15u)]*search;
  float2 sampleUV=uv+searchOffset;
  [branch]if(all(sampleUV>=0)&&all(sampleUV<1)){
   int2 sampleCoord=min(int2(sampleUV*dimensions),int2(width-1,height-1));
   // Evaluate the receiver plane at the actual point-sampled texel center.
   float2 actualOffset=(float2(sampleCoord)+.5)/dimensions-uv;
   float depth=DirectionalDepth(sampleCoord,layer)-dot(gradient,actualOffset);
   if(depth<receiver-bias){blockerSum+=depth;blockers+=1;}
  }
 }
 [branch]if(blockers>=.5){
 float2 radius=DirectionalPenumbra(receiver-blockerSum/blockers,scales,texel);
 float filtered=0;
 [loop]for(uint filterTap=0;filterTap<17;++filterTap){
  float2 filterOffset=filterTap==16?float2(0,0):DirectionalShadowDisk[min(filterTap,15u)]*radius;
  // Receiver-plane correction avoids flat terrain self-shadow.
  filtered+=DirectionalCompare(uv+filterOffset,layer,receiver-bias+dot(gradient,filterOffset));
 }
 visibility=filtered/17;
 }
 }
 }
 return visibility;
}
