// SPDX-License-Identifier: GPL-3.0-or-later
// post106: physically based bloom (after Jimenez, "Next Generation Post Processing in Call of
// Duty: Advanced Warfare", SIGGRAPH 2014). The world image (after TAA, before the HUD) is
// thresholded per pixel at full resolution (soft knee), downsampled through a mip chain with a
// 13-tap filter, then each level is upsampled with a 3x3 tent and added to the level above.
// The chain is linear, so an emitter's glow does not depend on where it falls on the grid;
// fireflies are already held by TAA (no Karis average: it made small emitters flicker). The sum of the levels, normalised by their count, is the glow:
// energy-conserving, round, smooth under sub-pixel motion and wide without blocks.
cbuffer BloomData:register(b0){
 float4 BloomSource; // xy source texel size, z unused, w exposure
 float4 BloomCurve;  // x threshold, y knee (exposed units)
};
Texture2D BloomInput:register(t0);
SamplerState BloomLinear:register(s0);
struct BloomVertex {float4 position:SV_POSITION;float2 uv:TEXCOORD0;};
BloomVertex VSBloom(uint vertex:SV_VertexID){
 BloomVertex output;output.uv=float2((vertex<<1)&2,vertex&2);
 output.position=float4(output.uv*float2(2,-2)+float2(-1,1),0,1);return output;
}
float3 BloomTap(float2 uv,float2 offset){return BloomInput.SampleLevel(BloomLinear,uv+offset*BloomSource.xy,0).rgb;}
// Soft threshold with a quadratic knee: below threshold-knee nothing glows, above the
// threshold the excess glows, in between it fades in smoothly.
float3 BloomPrefilter(float3 c){
 c=max(c,0)*BloomSource.w;float peak=max(c.r,max(c.g,c.b));
 float soft=clamp(peak-BloomCurve.x+BloomCurve.y,0,2*BloomCurve.y);soft=soft*soft/(4*BloomCurve.y+1e-5);
 return c*max(soft,peak-BloomCurve.x)/max(peak,1e-5);
}
float4 PSBloomPrefilter(BloomVertex input):SV_TARGET{return float4(BloomPrefilter(BloomInput.Load(int3(input.position.xy,0)).rgb),1);}
float4 PSBloomDown(BloomVertex input):SV_TARGET{
 float2 uv=input.uv;
 float3 a=BloomTap(uv,float2(-2,-2)),b=BloomTap(uv,float2(0,-2)),c=BloomTap(uv,float2(2,-2));
 float3 d=BloomTap(uv,float2(-1,-1)),e=BloomTap(uv,float2(1,-1));
 float3 f=BloomTap(uv,float2(-2,0)),g=BloomTap(uv,float2(0,0)),h=BloomTap(uv,float2(2,0));
 float3 i=BloomTap(uv,float2(-1,1)),j=BloomTap(uv,float2(1,1));
 float3 k=BloomTap(uv,float2(-2,2)),l=BloomTap(uv,float2(0,2)),m=BloomTap(uv,float2(2,2));
 // Five overlapping 2x2 boxes: the centre one weighs .5, the four corner ones .125 each.
 float3 box[5]={(d+e+i+j)*.25,(a+b+f+g)*.25,(b+c+g+h)*.25,(f+g+k+l)*.25,(g+h+l+m)*.25};
 float weight[5]={.5,.125,.125,.125,.125};
 float3 sum=0;float total=0;
 [unroll]for(int n=0;n<5;++n){
  sum+=box[n]*weight[n];total+=weight[n];
 }
 return float4(sum/max(total,1e-6),1);
}
float4 PSBloomUp(BloomVertex input):SV_TARGET{
 float2 uv=input.uv;
 float3 sum=BloomTap(uv,float2(0,0))*4;
 sum+=(BloomTap(uv,float2(-1,0))+BloomTap(uv,float2(1,0))+BloomTap(uv,float2(0,-1))+BloomTap(uv,float2(0,1)))*2;
 sum+=BloomTap(uv,float2(-1,-1))+BloomTap(uv,float2(1,-1))+BloomTap(uv,float2(-1,1))+BloomTap(uv,float2(1,1));
 return float4(sum/16,1);
}
