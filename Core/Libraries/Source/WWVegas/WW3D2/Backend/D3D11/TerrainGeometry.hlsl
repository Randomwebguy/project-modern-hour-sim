// SPDX-License-Identifier: GPL-3.0-or-later
Texture2D TerrainHeight:register(t11);
Texture2D TerrainIdentity:register(t12);
Texture2D TerrainExclusion:register(t14);
SamplerState TerrainSampler:register(s9);
struct WorldTerrainCell {float4 coordinates[4];float4 alpha;};
StructuredBuffer<WorldTerrainCell> TerrainCells:register(t13);
// terrain101: x = class A | class B<<8 | mean blend alpha (16-bit)<<16, y/z = region colour A/B (sRGB bytes),
// w = 1 when the cell's 3x3 neighbourhood (all the field reads there) is that one class and region.
Buffer<uint4> TerrainCellClasses:register(t56);
cbuffer TerrainMapInfo:register(b3){float4 TerrainDimensions;};
// terrain96/97: world-space photo-scanned ground library (TerrainMaterialLibrary.h).
// Table: [layer]=(tile,mean albedo), [16+class]=(primary,partner,tint,enabled),
// [48+class]=(relief amplitude,macro tile,tessellation weight,0).
Texture2DArray TerrainLibraryAlbedo:register(t48);
Texture2DArray TerrainLibrarySurface:register(t49);
StructuredBuffer<float4> TerrainLibraryTable:register(t50);
SamplerState TerrainLibrarySampler:register(s14);
float TerrainHash(float2 p){return frac(sin(dot(p,float2(127.1,311.7)))*43758.5453);}
float TerrainNoise(float2 p){
 float2 i=floor(p),f=frac(p);f=f*f*(3-2*f);
 return lerp(lerp(TerrainHash(i),TerrainHash(i+float2(1,0)),f.x),lerp(TerrainHash(i+float2(0,1)),TerrainHash(i+float2(1,1)),f.x),f.y);
}
void WorldSurface(float2 world,out float2 baseUV,out float2 blendUV,out float alpha){
 float2 grid=(world+TerrainDimensions.z)/max(TerrainDimensions.w,.001);
 uint2 cell=min((uint2)max(floor(grid),0),uint2(TerrainDimensions.xy)-1);
 float2 local=saturate(grid-float2(cell));
 WorldTerrainCell data=TerrainCells[cell.y*(uint)TerrainDimensions.x+cell.x];
 float4 uv=lerp(lerp(data.coordinates[0],data.coordinates[1],local.x),
                lerp(data.coordinates[3],data.coordinates[2],local.x),local.y);
 baseUV=uv.xy;blendUV=uv.zw;
 alpha=saturate(lerp(lerp(data.alpha.x,data.alpha.y,local.x),lerp(data.alpha.w,data.alpha.z,local.x),local.y));
}
uint GroundAt(float2 uv){
 uint w,h;TerrainIdentity.GetDimensions(w,h);
 return (uint)round(TerrainIdentity.Load(int3(min((uint2)(frac(uv)*float2(w,h)),uint2(w-1,h-1)),0)).r*255);
}
// terrain101: continuous ground-class field (flag 65536). Zero Hour paints the
// classes per 10-unit cell with a linear corner alpha, so every border is a
// staircase of cell edges. The field weighs the classes of the four cell centres
// around a domain-warped position (organic borders, never aligned with the grid);
// TerrainFieldTransition sharpens the soft field with the layers' scanned heights.
// The ground shading and the grass placement/shadows use the same functions.
// Two-component value noise from a sin-free hash (the sin hash cost .49 ms at 1080p here).
float2 TerrainHash22(float2 p){float3 p3=frac(float3(p.xyx)*float3(.1031,.1030,.0973));p3+=dot(p3,p3.yzx+33.33);return frac((p3.xx+p3.yz)*p3.zy);}
float2 TerrainNoise22(float2 p){
 float2 i=floor(p),f=frac(p);f=f*f*(3-2*f);
 return lerp(lerp(TerrainHash22(i),TerrainHash22(i+float2(1,0)),f.x),lerp(TerrainHash22(i+float2(0,1)),TerrainHash22(i+float2(1,1)),f.x),f.y);
}
float2 TerrainFieldWarp(float2 world){
 // At most 5 units (half a cell): the field at a point then reads only its own
 // cell's 3x3 neighbourhood, which the uniform flag (w) covers exactly.
 return (TerrainNoise22(world/23+float2(3.1,7.7))*2-1)*3.2+(TerrainNoise22(world/6.1+float2(5.5,1.3))*2-1)*1.2+(TerrainNoise22(world/2.3+float2(1.9,6.4))*2-1)*.6;
}
float3 TerrainRegionAt(float2 uv){
 uint w,h;TerrainIdentity.GetDimensions(w,h);
 return TerrainIdentity.Load(int3(min((uint2)(frac(uv)*float2(w,h)),uint2(w-1,h-1)),0)).gba;
}
struct TerrainFieldSample {uint first,second;float firstWeight,secondWeight;float3 firstRegion,secondRegion;};
// Up to three distinct classes are kept in fixed slots (a fourth, only at a rare
// four-class corner, is dropped); the region colours of the two strongest are
// summed in a second pass over the same four cells. Small and register-lean
// inside the large legacy pixel shader, where live registers cost occupancy.
#define TERRAIN_FIELD_ADD(G,W) if((W)>0){\
  if((G)==c0)w0+=(W);else if((G)==c1)w1+=(W);else if((G)==c2)w2+=(W);\
  else if(w0<=0){c0=(G);w0=(W);}else if(w1<=0){c1=(G);w1=(W);}else if(w2<=0){c2=(G);w2=(W);}}
TerrainFieldSample TerrainFieldAt(float2 world){
 int2 limit=max(int2(TerrainDimensions.xy)-1,0);
 TerrainFieldSample s;
 // Inside a uniform neighbourhood (most ground) the field is the cell itself.
 int2 own=clamp(int2(floor((world+TerrainDimensions.z)/max(TerrainDimensions.w,.001))),0,limit);
 uint4 home=TerrainCellClasses[own.y*(uint)TerrainDimensions.x+own.x];
 [branch]if(home.w!=0){
  s.first=s.second=home.x&255;s.firstWeight=1;s.secondWeight=0;
  s.firstRegion=s.secondRegion=float3(home.y&255,(home.y>>8)&255,(home.y>>16)&255)/255;
 }else{
 float2 grid=(world+TerrainFieldWarp(world)+TerrainDimensions.z)/max(TerrainDimensions.w,.001)-.5,base=floor(grid),f=grid-base;
 uint c0=16,c1=16,c2=16;float w0=0,w1=0,w2=0;
 [unroll]for(int k=0;k<4;++k){
  int2 corner=int2(k&1,k>>1),cell=clamp(int2(base)+corner,0,limit);
  float bilinear=(corner.x?f.x:1-f.x)*(corner.y?f.y:1-f.y);
  uint x=TerrainCellClasses[cell.y*(uint)TerrainDimensions.x+cell.x].x;float alpha=(x>>16)/65535.0;
  TERRAIN_FIELD_ADD(x&255,bilinear*(1-alpha));TERRAIN_FIELD_ADD((x>>8)&255,bilinear*alpha);
 }
 bool zeroFirst=w0>=w1&&w0>=w2,oneFirst=!zeroFirst&&w1>=w2;
 s.first=zeroFirst?c0:(oneFirst?c1:c2);s.firstWeight=zeroFirst?w0:(oneFirst?w1:w2);
 float wa=zeroFirst?w1:w0,wb=zeroFirst||oneFirst?w2:w1;uint ca=zeroFirst?c1:c0,cb=zeroFirst||oneFirst?c2:c1;
 bool aSecond=wa>=wb;s.second=aSecond?ca:cb;s.secondWeight=aSecond?wa:wb;if(s.secondWeight<=0)s.second=s.first;
 s.firstRegion=s.secondRegion=0;
 [unroll]for(int j=0;j<4;++j){
  int2 corner=int2(j&1,j>>1),cell=clamp(int2(base)+corner,0,limit);
  float bilinear=(corner.x?f.x:1-f.x)*(corner.y?f.y:1-f.y);
  uint4 data=TerrainCellClasses[cell.y*(uint)TerrainDimensions.x+cell.x];float alpha=(data.x>>16)/65535.0;
  float3 ra=float3(data.y&255,(data.y>>8)&255,(data.y>>16)&255),rb=float3(data.z&255,(data.z>>8)&255,(data.z>>16)&255);
  uint ga=data.x&255,gb=(data.x>>8)&255;float wa=bilinear*(1-alpha),wb=bilinear*alpha;
  s.firstRegion+=(ga==s.first?wa:0)*ra+(gb==s.first?wb:0)*rb;s.secondRegion+=(ga==s.second?wa:0)*ra+(gb==s.second?wb:0)*rb;
 }
 s.firstRegion/=255*max(s.firstWeight,1e-6);
 s.secondRegion=s.secondWeight>0?s.secondRegion/(255*s.secondWeight):s.firstRegion;
 }
 return s;
}
#undef TERRAIN_FIELD_ADD
// A class's primary layer height (TerrainLibraryColor(layer,...,0,1).a) less the
// layer's mean (its 1x1 mip): a layer that is higher on average must not push the
// border into its neighbour and shrink the painted area.
// The border shape needs no anisotropic detail: one isotropic fetch at the footprint's mip.
float TerrainFieldHeight(uint ground,float2 world,float2 dx,float2 dy){
 float layer=TerrainLibraryTable[16+ground].x,tile=TerrainLibraryTable[(uint)layer].x;
 uint w,h,elements,levels;TerrainLibraryAlbedo.GetDimensions(0,w,h,elements,levels);
 float mean=TerrainLibraryAlbedo.SampleLevel(TerrainLibrarySampler,float3(.5,.5,layer),levels-1).a;
 float lod=log2(max(max(length(dx),length(dy))*w/tile,1e-6));
 return TerrainLibraryAlbedo.SampleLevel(TerrainLibrarySampler,float3(world.x/tile,-world.y/tile,layer),lod).a-mean;
}
// Share of the second class: the soft field decides where the border runs, the
// scanned heights make it organic (clumps of one ground reaching into the other).
float TerrainFieldTransition(TerrainFieldSample s,float2 world,float2 dx,float2 dy){
 float sum=s.firstWeight+s.secondWeight,t=0;
 [branch]if(s.secondWeight>0&&sum>0){
  float share=s.secondWeight/sum;
  float2 h=float2(TerrainFieldHeight(s.first,world,dx,dy)+1.4*(1-share),TerrainFieldHeight(s.second,world,dx,dy)+1.4*share);
  float top=max(h.x,h.y)-.25;float2 k=max(h-top,0);t=k.y/max(k.x+k.y,1e-5);
 }
 return t;
}
// Region colours travel packed (8-bit sRGB, the class map's own precision) through
// the layer evaluation: six live floats there raised the pixel shader's registers.
uint TerrainPackRegion(float3 c){uint3 b=(uint3)round(saturate(c)*255);return b.x|(b.y<<8)|(b.z<<16);}
float4 TerrainUnpackRegion(uint c){return float4(0,float3(c&255,(c>>8)&255,(c>>16)&255)/255);}
bool TerrainFieldLibrary(TerrainFieldSample s){
 return TerrainLibraryTable[16+s.first].w>.5&&(s.secondWeight<=0||TerrainLibraryTable[16+s.second].w>.5);
}
// terrain97: macro relief, the class primary layer's scanned height resampled at
// its macro tile (two rotated octaves) so 5-25 unit bumps reach the RTS camera.
// Mean-free per layer (the 1x1 mip is the layer mean); mip from vertex spacing.
float TerrainClassRelief(uint ground,float2 world,float spacing){
 float4 relief=TerrainLibraryTable[48+ground];float result=0;
 [branch]if(relief.x>0&&TerrainLibraryTable[16+ground].w>.5){
  float layer=TerrainLibraryTable[16+ground].x;uint w,h,elements,levels;TerrainLibraryAlbedo.GetDimensions(0,w,h,elements,levels);
  float level=log2(max(spacing*w/relief.y,1)),s,c;sincos(.65,s,c);
  float2 a=float2(world.x,-world.y)/relief.y,b=float2(c*world.x-s*world.y,-(s*world.x+c*world.y))/(relief.y*.43);
  float mean=TerrainLibraryAlbedo.SampleLevel(TerrainLibrarySampler,float3(.5,.5,layer),levels-1).a;
  float height=.65*TerrainLibraryAlbedo.SampleLevel(TerrainLibrarySampler,float3(a,layer),level).a+
               .35*TerrainLibraryAlbedo.SampleLevel(TerrainLibrarySampler,float3(b,layer),level+1.22).a;
  result=(height-mean)*relief.x;
 }
 return result;
}
// Classes of the four native cells around world, weighted bilinearly between
// cell centres (and by each cell's blend alpha): a pure function of the world
// position, continuous across every cell and patch edge, so no cracks.
float TerrainLibraryRelief(float2 world,float spacing,bool evaluate,out float tessellation){
 float weights[16];[unroll]for(int i=0;i<16;++i)weights[i]=0;
 float2 grid=(world+TerrainDimensions.z)/max(TerrainDimensions.w,.001)-.5,base=floor(grid),f=grid-base;
 int2 limit=max(int2(TerrainDimensions.xy)-1,0);
 [unroll]for(int k=0;k<4;++k){
  int2 corner=int2(k&1,k>>1);int2 cell=clamp(int2(base)+corner,0,limit);
  float w=(corner.x?f.x:1-f.x)*(corner.y?f.y:1-f.y);
  WorldTerrainCell data=TerrainCells[cell.y*(uint)TerrainDimensions.x+cell.x];
  float4 uv=(data.coordinates[0]+data.coordinates[1]+data.coordinates[2]+data.coordinates[3])*.25;
  float alpha=saturate(dot(data.alpha,.25));
  weights[min(GroundAt(uv.xy),15)]+=w*(1-alpha);weights[min(GroundAt(uv.zw),15)]+=w*alpha;
 }
 float relief=0;tessellation=0;
 [loop]for(uint g=1;g<16;++g){
  [branch]if(weights[g]>0&&TerrainLibraryTable[16+g].w>.5){
   tessellation+=weights[g]*TerrainLibraryTable[48+g].z;
   if(evaluate)relief+=weights[g]*TerrainClassRelief(g,world,spacing);
  }
 }
 return relief;
}
// World size of an 8-pixel step at this position. Camera only, so every patch
// sharing a vertex selects the same height mip.
float TerrainVertexSpacing(float3 world){
 float scale=2/max(abs(Projection[1][1])*Viewport.w,1e-6);
 float pixel=Projection[3][3]>.5?scale:scale*length(world-CameraWorld.xyz);
 return max(pixel*8,10.0/12);
}
float ReliefAmplitude(uint ground){
 if(ground==1)return .08;if(ground==2)return .14;if(ground==3)return .18;
 if(ground==4)return .35;if(ground==5)return .12;if(ground==6)return .16;
 if(ground==7||ground==8)return .025;if(ground==9)return .08;return 0;
}
float CoveredGround(float2 world){
 uint w,h;TerrainExclusion.GetDimensions(w,h);if(w==0||h==0)return 0;
 float2 uv=(world+TerrainDimensions.z)/(TerrainDimensions.xy*TerrainDimensions.w);
 if(any(uv<0)||any(uv>=1))return 1;
 // The immutable occupancy upload already contains the exact clamped 3x3
 // maximum. One fetch is equivalent in both tessellation and grass stages.
 return TerrainExclusion.Load(int3(int2(uv*float2(w,h)),0)).r;
}
float GroundNoise(float2 p){return sin(p.x*1.73+sin(p.y*.83))*sin(p.y*1.37+sin(p.x*.61))*.5;}
float Relief(float2 uv,float2 world){
 float h=TerrainHeight.SampleLevel(TerrainSampler,uv,0).r;
 return ReliefAmplitude(GroundAt(uv))*((h-.5)*.85+GroundNoise(world)*.15);
}
// The displacement the terrain domain shader applies at a native surface point:
// the native material relief (cell-edge faded), terrain97 library macro relief,
// and the sink under roads/concrete. Shared with the GPU grass roots.
float TerrainSurfaceOffset(float3 world){
 float2 baseUV,blendUV;float blend;
 WorldSurface(world.xy,baseUV,blendUV,blend);
 // Use the map's canonical UVs in every pass. Extra blend passes may point
 // uv0 at a different material; their geometry must still match the base.
 float relief=lerp(Relief(baseUV,world.xy),Relief(blendUV,world.xy),blend);
 // Native cell boundaries remain undisplaced. Different material UVs and
 // secondary blend passes cannot open cracks along the 10-unit map grid.
 float2 cell=frac(world.xy/10);
 float edge=min(min(cell.x,1-cell.x),min(cell.y,1-cell.y));
 // Sink relief under actual roads and concrete to avoid surface intersections.
 float offset=lerp(relief*smoothstep(0,.15,edge),-.06,CoveredGround(world.xy));
 // terrain97: macro relief needs no cell-edge fade; it is continuous by construction.
 // Without it the expression above is the original one (bit-exact geometry).
 [branch]if(((uint)ModernMaterial.w&8192)!=0){float density;float library=TerrainLibraryRelief(world.xy,TerrainVertexSpacing(world),true,density);
  offset=lerp(relief*smoothstep(0,.15,edge)+library,-.06,CoveredGround(world.xy));}
 return offset;
}
float4 TerrainClip(float3 world){
 // Tessellation density and grass seeds still use the native camera. Only
 // output projection changes; depth and color therefore share actual geometry.
 if(ShadowRender.y>.5){row_major float4x4 light=ShadowRender.x<.5?SceneLightVP:SceneFarVP;return mul(float4(world,1),light);}
 return mul(mul(float4(world,1),View),Projection);
}
struct TerrainFactors {float edge[3]:SV_TessFactor;float inside:SV_InsideTessFactor;};
float TerrainEdge(LegacyOutput a,LegacyOutput b,float density){
 // Symmetric endpoints give equal factors on shared edges. terrain97: relief
 // classes refine toward 8 px per segment (grass keeps 24: its blade seeding
 // follows the tessellation); density is a function of the endpoints only.
 float2 pa=a.position.xy/max(abs(a.position.w),.001);
 float2 pb=b.position.xy/max(abs(b.position.w),.001);
 float length2D=length((pa-pb)*Viewport.zw*.5);
 // Without relief the factor is the original constant expression (bit-exact).
 return density>0?clamp(length2D/lerp(24,8,saturate(density)),1,12):clamp(length2D/24,1,12);
}
float TerrainLibraryDensity(float3 world){
 float density=0;
 [branch]if(((uint)ModernMaterial.w&8192)!=0)TerrainLibraryRelief(world.xy,1,false,density);
 return density;
}
// Reject only a patch whose entire possible displaced/grass output lies
// outside one homogeneous clip plane. Keep the original density for every
// potentially visible patch, including silhouettes and shadow-only casters.
bool TerrainPatchOutside(InputPatch<LegacyOutput,3> patch){
 float4 a=TerrainClip(patch[0].worldPosition),b=TerrainClip(patch[1].worldPosition),c=TerrainClip(patch[2].worldPosition);
 row_major float4x4 clipMatrix=mul(View,Projection);
 if(ShadowRender.y>.5)clipMatrix=ShadowRender.x<.5?SceneLightVP:SceneFarVP;
 // Relief <= .175 + library macro relief <= 1.2, sink <= .06, blade height <= 1.5, lateral <= .48.
 // World-axis support is deliberately larger, with a numerical clip margin.
 float4 support=abs(clipMatrix[0])+abs(clipMatrix[1])+2*abs(clipMatrix[2]);
 float4 numerical=1e-4*(1+max(abs(a),max(abs(b),abs(c))));support+=numerical;
 if(any(!isfinite(a))||any(!isfinite(b))||any(!isfinite(c))||any(!isfinite(support)))return false;
 float4 positive=max(a.w+a,max(b.w+b,c.w+c));
 float4 negative=max(a.w-a,max(b.w-b,c.w-c));
 return positive.x<-(support.w+support.x)||negative.x<-(support.w+support.x)||
        positive.y<-(support.w+support.y)||negative.y<-(support.w+support.y)||
        max(a.z,max(b.z,c.z))<-support.z||negative.z<-(support.w+support.z);
}
TerrainFactors TerrainPatch(InputPatch<LegacyOutput,3> patch){
 TerrainFactors f=(TerrainFactors)0;
 if(TerrainPatchOutside(patch))return f;
 float d0=TerrainLibraryDensity(patch[0].worldPosition),d1=TerrainLibraryDensity(patch[1].worldPosition),d2=TerrainLibraryDensity(patch[2].worldPosition);
 f.edge[0]=TerrainEdge(patch[1],patch[2],max(d1,d2));
 f.edge[1]=TerrainEdge(patch[2],patch[0],max(d2,d0));f.edge[2]=TerrainEdge(patch[0],patch[1],max(d0,d1));
 f.inside=max(f.edge[0],max(f.edge[1],f.edge[2]));return f;
}
[domain("tri")][partitioning("fractional_even")][outputtopology("triangle_cw")]
[outputcontrolpoints(3)][patchconstantfunc("TerrainPatch")][maxtessfactor(12.0)]
LegacyOutput TerrainHS(InputPatch<LegacyOutput,3> patch,uint i:SV_OutputControlPointID){return patch[i];}
[domain("tri")]
LegacyOutput TerrainDS(TerrainFactors factors,float3 bary:SV_DomainLocation,const OutputPatch<LegacyOutput,3> patch){
 LegacyOutput o=(LegacyOutput)0;
 #define TERRAIN_INTERPOLATE(field) o.field=patch[0].field*bary.x+patch[1].field*bary.y+patch[2].field*bary.z
 TERRAIN_INTERPOLATE(diffuse);TERRAIN_INTERPOLATE(specular);
 TERRAIN_INTERPOLATE(uv0);TERRAIN_INTERPOLATE(uv1);TERRAIN_INTERPOLATE(uv2);TERRAIN_INTERPOLATE(uv3);
 TERRAIN_INTERPOLATE(uv4);TERRAIN_INTERPOLATE(uv5);TERRAIN_INTERPOLATE(uv6);TERRAIN_INTERPOLATE(uv7);
 TERRAIN_INTERPOLATE(fog);TERRAIN_INTERPOLATE(pointSize);TERRAIN_INTERPOLATE(worldPosition);TERRAIN_INTERPOLATE(worldNormal);
 #undef TERRAIN_INTERPOLATE
 o.worldPosition.z+=TerrainSurfaceOffset(o.worldPosition);
 float4 camera=mul(float4(o.worldPosition,1),View);
 o.position=TerrainClip(o.worldPosition);
 o.fog=FogSettings.w>.5?length(camera.xyz):abs(camera.z);return o;
}

// Sparse, genuine blade triangles. Seeds are on a persistent world grid;
// no screen-facing sprite or camera-dependent random distribution is used.
// Keep the original full signature for arbitrary legacy shader programs.
#define TERRAIN_GRASS_ENTRY TerrainGrassGS
#define TERRAIN_GRASS_OUTPUT LegacyOutput
#define TERRAIN_GRASS_PACK(value) value
#include "TerrainGrassBody.hlsli"
#undef TERRAIN_GRASS_ENTRY
#undef TERRAIN_GRASS_OUTPUT
#undef TERRAIN_GRASS_PACK

// Only unused varyings are omitted. Reflection proves the selected pixel
// shader reads no omitted component before this stage can be used at runtime.
struct TerrainRasterOutput {
 float4 position:SV_POSITION;float4 diffuse:COLOR0;float4 specular:COLOR1;
 float4 uv0:TEXCOORD0;float4 uv1:TEXCOORD1;float4 uv2:TEXCOORD2;float4 uv3:TEXCOORD3;
 float fog:TEXCOORD8;float3 worldPosition:TEXCOORD10;float3 worldNormal:TEXCOORD11;float blade:TEXCOORD12;
};
TerrainRasterOutput PackTerrainRaster(LegacyOutput input){
 TerrainRasterOutput output;
 output.position=input.position;output.diffuse=input.diffuse;output.specular=input.specular;
 output.uv0=input.uv0;output.uv1=input.uv1;output.uv2=input.uv2;output.uv3=input.uv3;
 output.fog=input.fog;output.worldPosition=input.worldPosition;output.worldNormal=input.worldNormal;output.blade=input.blade;return output;
}
#define TERRAIN_GRASS_ENTRY TerrainGrassCompactGS
#define TERRAIN_GRASS_OUTPUT TerrainRasterOutput
#define TERRAIN_GRASS_PACK(value) PackTerrainRaster(value)
#include "TerrainGrassBody.hlsli"
#undef TERRAIN_GRASS_ENTRY
#undef TERRAIN_GRASS_OUTPUT
#undef TERRAIN_GRASS_PACK
