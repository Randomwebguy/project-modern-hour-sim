// SPDX-License-Identifier: GPL-3.0-or-later
// One canonical generator, used by full and compact raster output variants.
[maxvertexcount(19)]
void TERRAIN_GRASS_ENTRY(triangle LegacyOutput input[3],inout TriangleStream<TERRAIN_GRASS_OUTPUT> stream){
 [unroll]for(int k=0;k<3;++k)stream.Append(TERRAIN_GRASS_PACK(input[k]));stream.RestartStrip();
 if(ModernMaterial.x<1.5||((uint)ModernMaterial.w&32)==0)return;
 float3 center=(input[0].worldPosition+input[1].worldPosition+input[2].worldPosition)/3;
 // A normal RTS camera is already more than 180 units above the ground.
 // Cull by horizontal range; elevation must not remove the whole lawn.
 float4 p0=mul(mul(float4(center,1),View),Projection);
 float4 p1=mul(mul(float4(center+float3(0,0,1.5),1),View),Projection);
 float pixels=length((p1.xy/max(abs(p1.w),.001)-p0.xy/max(abs(p0.w),.001))*Viewport.zw*.5);
 if(pixels<.35)return;
 float density=smoothstep(.35,2.0,pixels);
 float2 a=input[0].worldPosition.xy,b=input[1].worldPosition.xy,c=input[2].worldPosition.xy;
 float2 ab=b-a,ac=c-a;float denominator=ab.x*ac.y-ab.y*ac.x;
 if(abs(denominator)<.00001)return;
 float2 grid=floor(center.xy/2.0)*2.0;
 [unroll]for(int candidate=0;candidate<2;++candidate){
  float2 site=grid+float2(candidate&1,candidate>>1)*2.0;
  float random=frac(sin(dot(site,float2(12.9898,78.233)))*43758.5453);
  if(random>density)continue;
  // Jitter within the persistent world-grid cell, away from cell/triangle
  // boundaries that formerly discarded many of the seeds.
  float2 seedXY=site+float2(.35+random*.8,.35+frac(random*7.31)*.8);
  float2 ap=seedXY-a;
  float y=(ap.x*ac.y-ap.y*ac.x)/denominator;
  float z=(ab.x*ap.y-ab.y*ap.x)/denominator;
  float3 bary=float3(1-y-z,y,z);
  if(min(bary.x,min(bary.y,bary.z))<=.0001)continue;
  // A rejected seed never emits geometry; test containment before texture reads.
  if(CoveredGround(seedXY)>.1)continue;
  LegacyOutput seed=input[0];
  seed.uv0=input[0].uv0*bary.x+input[1].uv0*bary.y+input[2].uv0*bary.z;
  seed.uv1=input[0].uv1*bary.x+input[1].uv1*bary.y+input[2].uv1*bary.z;
  seed.uv2=input[0].uv2*bary.x+input[1].uv2*bary.y+input[2].uv2*bary.z;
  seed.diffuse=input[0].diffuse*bary.x+input[1].diffuse*bary.y+input[2].diffuse*bary.z;
  float2 baseUV,blendUV;float blend;
  WorldSurface(seedXY,baseUV,blendUV,blend);
  float grass=(GroundAt(baseUV)==1?1-blend:0)+(GroundAt(blendUV)==1?blend:0);
  if(grass<.65)continue;
  float3 base=input[0].worldPosition*bary.x+input[1].worldPosition*bary.y+input[2].worldPosition*bary.z;
  [unroll]for(int leaf=0;leaf<2;++leaf){
   float angle=random*6.283185+leaf*1.570796;
   float3 side=float3(cos(angle),sin(angle),0)*(.30+random*.18);
   // Lean along the leaf's length, perpendicular to its width. Leaning along
   // side made every blade a vertical plane with zero overhead light area.
   float height=.85+random*.65;float3 bend=float3(-side.y*.7,side.x*.7,height);
   seed.worldNormal=Unit(cross(side,bend));
   [unroll]for(int v=0;v<4;++v){
    LegacyOutput blade=seed;blade.blade=v<2?1:2;
    blade.worldPosition=base+(v==0?-side:v==1?side:v==2?bend-side*.25:bend+side*.25);
    float4 camera=mul(float4(blade.worldPosition,1),View);blade.position=TerrainClip(blade.worldPosition);
    blade.fog=FogSettings.w>.5?length(camera.xyz):abs(camera.z);stream.Append(TERRAIN_GRASS_PACK(blade));
   }
   stream.RestartStrip();
  }
 }
}
