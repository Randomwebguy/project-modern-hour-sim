// SPDX-License-Identifier: GPL-3.0-or-later
Texture2DArray<float> LocalShadowAtlas:register(t36);
SamplerComparisonState LocalShadowComparison:register(s13);
cbuffer LocalShadowData:register(b11){row_major float4x4 LocalShadowMatrices[24];float4 LocalShadowPositions[4];float4 LocalShadowSettings;};
uint LocalShadowCubeFace(float3 delta){float3 a=abs(delta);return a.x>=a.y&&a.x>=a.z?(delta.x>=0?0u:1u):(a.y>=a.z?(delta.y>=0?2u:3u):(delta.z>=0?4u:5u));}
float LocalLightVisibility(float3 world,float shadowIndex,float type){
 float visibility=1;
 [branch]if(shadowIndex>=.5&&shadowIndex<=LocalShadowSettings.x){
 uint slot=(uint)(shadowIndex-.5);float3 delta=world-LocalShadowPositions[slot].xyz;float distance=length(delta);
 uint face=type>1.5?0u:LocalShadowCubeFace(delta);
 // Bias along the light ray has consistent world units for perspective maps.
 float3 receiver=world-delta*(LocalShadowSettings.z/max(distance,.001));
 float4 clip=mul(float4(receiver,1),LocalShadowMatrices[slot*6+face]);
 float3 p=clip.xyz/max(clip.w,.000001);float2 uv=p.xy*float2(.5,-.5)+.5;
 [branch]if(distance>=.001&&clip.w>0&&all(uv>=0)&&all(uv<=1)&&p.z>0&&p.z<1){
  // Bilinear point-face filtering must not mix the comparison sampler's white
  // border into a closed cube edge. Angular taps still choose adjacent faces.
  float halfTexel=LocalShadowSettings.y*.5;if(type<1.5)uv=clamp(uv,halfTexel,1-halfTexel);
  visibility=LocalShadowAtlas.SampleCmpLevelZero(LocalShadowComparison,float3(uv,slot*6+face),p.z-LocalShadowSettings.w);
 }
 }
 return visibility;
}
float LocalSurfaceVisibility(float3 world,float shadowIndex,float type){
 float visibility=1;
 [branch]if(shadowIndex>=.5&&shadowIndex<=LocalShadowSettings.x){
  uint slot=(uint)(shadowIndex-.5);float3 delta=world-LocalShadowPositions[slot].xyz;float distance=max(length(delta),.001);float3 direction=delta/distance;
  float3 tangent=cross(abs(direction.z)<.99?float3(0,0,1):float3(0,1,0),direction);tangent*=rsqrt(max(dot(tangent,tangent),.000001));float3 bitangent=cross(direction,tangent);
  float footprint=distance*LocalShadowSettings.y*LocalShadowPositions[slot].w;
  // Each angular ray independently selects its cube face. A footprint crossing
  // a point-light seam therefore samples the adjacent depth face, not a white border.
  visibility=.25*(LocalLightVisibility(world+footprint*(tangent+bitangent),shadowIndex,type)+
                   LocalLightVisibility(world+footprint*(tangent-bitangent),shadowIndex,type)+
                   LocalLightVisibility(world+footprint*(-tangent+bitangent),shadowIndex,type)+
                   LocalLightVisibility(world+footprint*(-tangent-bitangent),shadowIndex,type));
 }
 return visibility;
}
