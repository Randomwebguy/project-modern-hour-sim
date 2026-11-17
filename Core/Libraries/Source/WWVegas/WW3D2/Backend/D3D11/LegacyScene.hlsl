// SPDX-License-Identifier: GPL-3.0-or-later
// W3D fixed-function compatibility and native GGX material lighting.
#include "MaterialLighting.hlsl"
#include "NativeLightCalibration.hlsl"
#include "AtmosphereCommon.hlsl"
#include "LocalLightShadows.hlsl"
cbuffer LegacyData : register(b0) {
 row_major float4x4 World,View,Projection,NormalWorld;
 row_major float4x4 TextureMatrices[8];
 float4 MaterialDiffuse,MaterialAmbient,MaterialSpecular,MaterialEmissive;
 float4 GlobalAmbient,TextureFactor,FogColor,FogSettings;
 float4 Viewport,GeometryFlags,MaterialFlags,PointSettings;
 float4 LightDiffuse[4],LightAmbient[4],LightSpecular[4];
 float4 LightPosition[4],LightDirection[4],LightAttenuation[4],LightSpot[4];
 uint4 ColorStage[8],AlphaStage[8],CoordinateStage[8];
 float4 Bump[8],BumpLuma[8];
 float4 PSConstants[8];
 float4 BridgeFlags,PointScale,ModernMaterial,CameraWorld;
 float4 VehicleAge,VehicleProfile;
 // photo107: per-object motion of a moving object (MotionSettings.x = 1): MotionPreviousWorld is the previous
 // world relative to the current one (inverse(current) * previous), applied to the pixel's world position; the
 // camera's previous and current unjittered view-projections follow. Zero for every other draw.
 row_major float4x4 MotionPreviousWorld,MotionCurrentVP,MotionPreviousVP;float4 MotionSettings;
};
// Texture declarations and sampling helpers are specialized for actual 2D,
// cube and volume bindings by the bridge. No API texel type is silently cast.
cbuffer VertexProgramData : register(b1){float4 VSConstants[96];};
// taa101: the primary camera's TAA jitter for the game's own vertex programs (their
// projection lives in VSConstants): clip-space xy offset per unit w; zero elsewhere.
cbuffer TemporalVertexData : register(b8){float4 TemporalVertexJitter;};
/*TEXTURES*/
Texture2D AuthoredNormal:register(t8);
Texture2D AuthoredORM:register(t9);
Texture2D AuthoredHeight:register(t16);
Texture2D AuthoredEmissive:register(t17);
Texture2D WearAlbedo:register(t26);
Texture2D WearNormal:register(t27);
Texture2D WearORM:register(t28);
Texture2DArray<float> SceneShadow:register(t10);
SamplerComparisonState SceneShadowSampler:register(s8);
cbuffer SceneShadowData:register(b2){row_major float4x4 SceneLightVP;float4 SceneShadowSettings;
 row_major float4x4 SceneFarVP;float4 SceneFarSettings;
 float4 ShadowCameraDepth,ShadowSplit,ShadowRender,ShadowSun;};
#define GENZH_SUN_SHADOW_ARRAY 1
#define GENZH_SUN_SHADOW_TEXTURE SceneShadow
#define GENZH_SUN_SHADOW_SAMPLER SceneShadowSampler
#include "DirectionalPCSS.hlsl"
#undef GENZH_SUN_SHADOW_ARRAY
#undef GENZH_SUN_SHADOW_TEXTURE
#undef GENZH_SUN_SHADOW_SAMPLER

struct InstanceMatrices{row_major float4x4 world;row_major float4x4 normal;};
StructuredBuffer<InstanceMatrices> DrawInstances:register(t15);
cbuffer DrawInstanceData:register(b4){uint4 InstanceSettings;};
float NativeSpotAttenuation(float cosine,float3 cone){
 float result=0;
 if(cosine>cone.y)result=cone.z<=0?1:pow(saturate((cosine-cone.y)/max(cone.x-cone.y,1e-6)),cone.z);
 return result;
}
struct LegacyInput {
 uint instanceID:SV_InstanceID;
 float4 position:POSITION;float4 normal:NORMAL;
 float4 diffuse:COLOR0;float4 specular:COLOR1;
 float4 uv0:TEXCOORD0;float4 uv1:TEXCOORD1;float4 uv2:TEXCOORD2;float4 uv3:TEXCOORD3;
 float4 uv4:TEXCOORD4;float4 uv5:TEXCOORD5;float4 uv6:TEXCOORD6;float4 uv7:TEXCOORD7;
#ifdef LEGACY_PROGRAMMABLE_INPUT
 float4 reg0:BLENDWEIGHT0;
 float4 reg1:BLENDWEIGHT1;
 float4 reg2:BLENDWEIGHT2;
 float4 reg3:BLENDWEIGHT3;
 float4 reg4:BLENDWEIGHT4;
 float4 reg5:BLENDWEIGHT5;
 float4 reg6:BLENDWEIGHT6;
 float4 reg7:BLENDWEIGHT7;
 float4 reg8:BLENDWEIGHT8;
 float4 reg9:BLENDWEIGHT9;
 float4 reg10:BLENDWEIGHT10;
 float4 reg11:BLENDWEIGHT11;
 float4 reg12:BLENDWEIGHT12;
 float4 reg13:BLENDWEIGHT13;
 float4 reg14:BLENDWEIGHT14;
 float4 reg15:BLENDWEIGHT15;
#endif
};
struct LegacyOutput {
 float4 position:SV_POSITION;float4 diffuse:COLOR0;float4 specular:COLOR1;
 float4 uv0:TEXCOORD0;float4 uv1:TEXCOORD1;float4 uv2:TEXCOORD2;float4 uv3:TEXCOORD3;
 float4 uv4:TEXCOORD4;float4 uv5:TEXCOORD5;float4 uv6:TEXCOORD6;float4 uv7:TEXCOORD7;
 float fog:TEXCOORD8;float pointSize:TEXCOORD9;
 float3 worldPosition:TEXCOORD10;float3 worldNormal:TEXCOORD11;
 float blade:TEXCOORD12;
};
float3 Unit(float3 v){return v*rsqrt(max(dot(v,v),1e-12));}
float4 SelectMaterial(float source,float4 material,LegacyInput input){
 if(source==1)return input.diffuse;if(source==2)return input.specular;return material;
}
LegacyOutput FixedVS(LegacyInput input) {
 LegacyOutput o=(LegacyOutput)0;
 row_major float4x4 worldMatrix=World,normalMatrix=NormalWorld;
 if(InstanceSettings.x!=0){worldMatrix=DrawInstances[input.instanceID].world;normalMatrix=DrawInstances[input.instanceID].normal;}
 float4 world=mul(float4(input.position.xyz,1),worldMatrix);
 float4 camera=mul(world,View);
 float3 normal=Unit(mul(float4(input.normal.xyz,0),normalMatrix).xyz);
 float3 cameraNormal=Unit(mul(float4(normal,0),View).xyz);
 o.worldPosition=world.xyz;o.worldNormal=normal;
 if(GeometryFlags.x>.5){
  float w=1/max(abs(input.position.w),1e-12);
  o.position=float4(((input.position.x+.5-Viewport.x)/Viewport.z*2-1)*w,
    (1-(input.position.y+.5-Viewport.y)/Viewport.w*2)*w,input.position.z*w,w);
 }else o.position=mul(camera,Projection);
 o.diffuse=input.diffuse;o.specular=input.specular;
 if(ModernMaterial.x>.5){
  // Terrain RGB was already lit in native vertex colors. Preserve blend alpha
  // while using one modern lighting evaluation over its actual basemap.
  o.diffuse=ModernMaterial.x>1.5?float4(1,1,1,input.diffuse.a):SelectMaterial(MaterialFlags.x,MaterialDiffuse,input);
 }
 else if(GeometryFlags.y>.5 && GeometryFlags.x<.5){
  float4 diffuse=SelectMaterial(MaterialFlags.x,MaterialDiffuse,input);
  float4 ambient=SelectMaterial(MaterialFlags.y,MaterialAmbient,input);
  float4 specular=SelectMaterial(MaterialFlags.z,MaterialSpecular,input);
  o.diffuse=float4(GlobalAmbient.rgb*ambient.rgb+MaterialEmissive.rgb,diffuse.a);
  o.specular=0;
  [unroll]for(int i=0;i<4;++i){
   if(LightPosition[i].w<.5)continue;
   float3 towardLight;float attenuation=1;
   if(LightPosition[i].w>2.5)towardLight=-LightDirection[i].xyz;
   else{
    towardLight=LightPosition[i].xyz-world.xyz;float distance=length(towardLight);
    attenuation=distance<=LightAttenuation[i].w?1/max(dot(LightAttenuation[i].xyz,float3(1,distance,distance*distance)),1e-6):0;
    if(LightPosition[i].w>1.5){
     float spot=dot(-Unit(towardLight),Unit(LightDirection[i].xyz));
     attenuation*=NativeSpotAttenuation(spot,LightSpot[i].xyz);
    }
   }
   float nl=max(0,dot(normal,Unit(towardLight)));
   o.diffuse.rgb+=attenuation*(LightAmbient[i].rgb*ambient.rgb+LightDiffuse[i].rgb*diffuse.rgb*nl);
   float3 eye=GeometryFlags.z>.5?Unit(-camera.xyz):float3(0,0,-1);
   float3 h=Unit(mul(float4(Unit(towardLight),0),View).xyz+eye);
   o.specular.rgb+=attenuation*LightSpecular[i].rgb*specular.rgb*pow(max(0,dot(cameraNormal,h)),max(MaterialFlags.w,1));
  }
  o.diffuse=saturate(o.diffuse);o.specular=saturate(o.specular);
 }
 float4 coords[8]={input.uv0,input.uv1,input.uv2,input.uv3,input.uv4,input.uv5,input.uv6,input.uv7};
 float4 transformed[8];
 [unroll]for(int stage=0;stage<8;++stage){
  uint index=min(CoordinateStage[stage].x&65535,7);
  uint generation=CoordinateStage[stage].x>>16;
  float4 coord=coords[index];
  if(generation==1)coord=float4(cameraNormal,1);
  if(generation==2)coord=float4(camera.xyz,1);
  if(generation==3)coord=float4(reflect(Unit(camera.xyz),cameraNormal),1);
  uint flags=CoordinateStage[stage].y;
  if(flags!=0){coord=mul(float4(coord.xyz,1),TextureMatrices[stage]);
   if((flags&256)!=0){uint count=flags&255;float divisor=count==4?coord.w:count==3?coord.z:coord.y;coord.xy/=(abs(divisor)<1e-8?1e-8:divisor);}}
  transformed[stage]=coord;
 }
 o.uv0=transformed[0];o.uv1=transformed[1];o.uv2=transformed[2];o.uv3=transformed[3];
 o.uv4=transformed[4];o.uv5=transformed[5];o.uv6=transformed[6];o.uv7=transformed[7];
 o.fog=GeometryFlags.x>.5?input.specular.a:(FogSettings.w>.5?length(camera.xyz):abs(camera.z));
 o.pointSize=PointSettings.x;
 if(PointSettings.w>.5){float distance=length(camera.xyz);o.pointSize=Viewport.w*PointSettings.x*rsqrt(max(dot(PointScale.xyz,float3(1,distance,distance*distance)),1e-8));}
 o.pointSize=clamp(o.pointSize,PointSettings.y,PointSettings.z);
 return o;
}
/*VERTEX_PROGRAM*/
#include "TerrainGeometry.hlsl"
#include "TerrainGrass.hlsl"
[maxvertexcount(4)]void LegacyPointGS(point LegacyOutput input[1],inout TriangleStream<LegacyOutput> stream){
 float2 offset=input[0].pointSize/Viewport.zw*input[0].position.w;
 float2 corners[4]={float2(-1,1),float2(1,1),float2(-1,-1),float2(1,-1)};
 [unroll]for(int i=0;i<4;++i){LegacyOutput output=input[0];output.position.xy+=corners[i]*offset;
  if(PointScale.w>.5)output.uv0=float4((corners[i].x+1)*.5,(1-corners[i].y)*.5,0,1);stream.Append(output);}
 stream.RestartStrip();
}
float4 Argument(uint selector,float4 texel,float4 current,float4 diffuse,float4 specular,float4 temp){
 uint kind=selector&15;
 float4 value=kind==0?diffuse:kind==1?current:kind==2?texel:kind==3?TextureFactor:kind==4?specular:temp;
 if((selector&32)!=0)value=value.aaaa;if((selector&16)!=0)value=1-value;return value;
}
float4 Operation(uint op,float4 a0,float4 a1,float4 a2,float4 diffuse,float4 texel,float4 current){
 if(op==2)return a1;if(op==3)return a2;if(op==4)return a1*a2;
 if(op==5)return 2*a1*a2;if(op==6)return 4*a1*a2;
 if(op==7)return a1+a2;if(op==8)return a1+a2-.5;if(op==9)return 2*(a1+a2-.5);
 if(op==10)return a1-a2;if(op==11)return a1+a2-a1*a2;
 if(op==12)return lerp(a2,a1,diffuse.a);if(op==13)return lerp(a2,a1,texel.a);
 if(op==14)return lerp(a2,a1,TextureFactor.a);if(op==15)return a1+a2*(1-texel.a);
 if(op==16)return lerp(a2,a1,current.a);if(op==17)return a1*a2;
 if(op==18)return a1+a2*a1.aaaa;if(op==19)return a1*a2+a1.aaaa;
 if(op==20)return (1-a1.aaaa)*a2+a1;if(op==21)return (1-a1)*a2+a1.aaaa;
 if(op==24)return dot(a1.rgb*2-1,a2.rgb*2-1).xxxx;
 if(op==25)return a1*a2+a0;if(op==26)return lerp(a2,a1,a0);
 return current;
}
float4 FixedPixel(float4 diffuse,float4 specular,float4 coords[8]){
 float4 current=diffuse,temp=0;float2 bumpOffset=0;float bumpLight=1;
 [unroll]for(int stage=0;stage<8;++stage){
  if(ColorStage[stage].x==1)break;
  float4 coord=coords[stage];coord.xy+=bumpOffset;
  float4 texel=SampleStage(stage,coord)*bumpLight;
  bumpOffset=0;bumpLight=1;
  if(ColorStage[stage].x==22 || ColorStage[stage].x==23){
   bumpOffset=mul(float2x2(Bump[stage]),texel.xy);
   if(ColorStage[stage].x==23)bumpLight=texel.z*BumpLuma[stage].x+BumpLuma[stage].y;
   continue;
  }
  float4 a0=Argument(CoordinateStage[stage].z,texel,current,diffuse,specular,temp);
  float4 a1=Argument(ColorStage[stage].y,texel,current,diffuse,specular,temp);
  float4 a2=Argument(ColorStage[stage].z,texel,current,diffuse,specular,temp);
  float4 result=Operation(ColorStage[stage].x,a0,a1,a2,diffuse,texel,current);
  a0=Argument(CoordinateStage[stage].w,texel,current,diffuse,specular,temp);
  a1=Argument(AlphaStage[stage].y,texel,current,diffuse,specular,temp);
  a2=Argument(AlphaStage[stage].z,texel,current,diffuse,specular,temp);
  result.a=Operation(AlphaStage[stage].x,a0,a1,a2,diffuse,texel,current).a;
  if(ColorStage[stage].w==5)temp=saturate(result);else current=saturate(result);
 }
 return current;
}
float CascadeVisibility(float3 world,float3 normal,uint cascade,float3 worldDx,float3 worldDy){
 row_major float4x4 lightTransform=cascade==0?SceneLightVP:SceneFarVP;
 float4 settings=cascade==0?SceneShadowSettings:SceneFarSettings;
 float4 projected=mul(float4(world,1),lightTransform);
 float3 position=projected.xyz/projected.w;float2 uv=position.xy*float2(.5,-.5)+.5;
 if(any(uv<0)||any(uv>1)||position.z<=0||position.z>=1)return 1;
 float bias=settings.x*(1+3*(1-saturate(dot(Unit(normal),ShadowSun.xyz))));
 return DirectionalPCSS(uv,position.z,bias,cascade,settings.yz,DirectionalShadowScale(lightTransform),DirectionalReceiverGradient(lightTransform,worldDx,worldDy));
}
float SceneVisibility(float3 world,float3 normal){
 float3 worldDx=ddx(world),worldDy=ddy(world);
 if(SceneShadowSettings.w<.5)return 1;
 if(ShadowSplit.w<1.5)return CascadeVisibility(world,normal,0,worldDx,worldDy);
 float depth=dot(float4(world,1),ShadowCameraDepth);
 if(depth>ShadowSplit.z)return 1;
 if(depth<ShadowSplit.x-ShadowSplit.y)return CascadeVisibility(world,normal,0,worldDx,worldDy);
 if(depth>ShadowSplit.x+ShadowSplit.y)return CascadeVisibility(world,normal,1,worldDx,worldDy);
 float blend=smoothstep(ShadowSplit.x-ShadowSplit.y,ShadowSplit.x+ShadowSplit.y,depth);
 return lerp(CascadeVisibility(world,normal,0,worldDx,worldDy),CascadeVisibility(world,normal,1,worldDx,worldDy),blend);
}
cbuffer ScreenMaterialControl:register(b13){float4 ScreenCaptureControl;};
struct ScreenLightingComponents {float3 diffuse;float3 specular;float3 normal;float3 directional;float3 response;float roughness;float occlusion;float3 albedo;};
float3 TerrainBaseAlbedo(LegacyOutput input,uint maps){
 // Native terrain programs multiply vertex light, cloud and lightmap into
 // their output. Those are illumination, never a physical base color. Modern
 // direct/indirect/cloud/shadow paths already supply them independently.
 float3 base=DecodeSRGB(SampleStage(0,input.uv0).rgb);
 if((maps&16)!=0)base=lerp(base,DecodeSRGB(SampleStage(1,input.uv1).rgb),saturate(input.diffuse.a));
 return base;
}
float3 TerrainChartNormalGrad(float3 n,float3 dpdx,float3 dpdy,float2 duvdx,float2 duvdy,float3 map){
 float3 t=cross(dpdy,n)*duvdx.x+cross(n,dpdx)*duvdy.x;
 float3 b=cross(dpdy,n)*duvdx.y+cross(n,dpdx)*duvdy.y;
 float scale=rsqrt(max(max(dot(t,t),dot(b,b)),1e-12));
 // Screen derivatives can reverse the chart determinant. Restore physical
 // +U/+V orientation, including mirrored charts; keep the geometric normal.
 float determinant=duvdx.x*duvdy.y-duvdx.y*duvdy.x;
 if(abs(determinant)<1e-12)return n;
 float handedness=determinant<0?-1:1;
 return Unit((t*map.x+b*map.y)*(scale*handedness)+n*map.z);
}
float3 TerrainChartNormal(float3 n,float3 dpdx,float3 dpdy,float2 uv,float3 map){
 return TerrainChartNormalGrad(n,dpdx,dpdy,ddx(uv),ddy(uv),map);
}
// terrain96: photo-scanned ground layers (ambientCG CC0, terrain-library-v1)
// tiled in world space per native ground class. Table: [layer]=(tile,mean
// linear albedo), [16+class]=(primary,partner,tint,enabled). The class map
// stores the class in R and the region's mean native colour (sRGB) in GBA.
// Resources and TerrainNoise are declared in TerrainGeometry.hlsl.
struct TerrainSide {float3 albedo;float height;float3 normal;float occlusion;float roughness;};
struct TerrainLayers {float3 albedo;float3 mean;float height;float2 slope;float occlusion;float roughness;};
float4 TerrainIdentityAt(float2 uv){
 uint w,h;TerrainIdentity.GetDimensions(w,h);
 return TerrainIdentity.Load(int3(min((uint2)(frac(uv)*float2(w,h)),uint2(w-1,h-1)),0));
}
// uv=(x,-y)/tile after rotating the world by angle: +u is +x and image-up is
// +y, so an OpenGL normal map rotates straight back into world XY.
float2x2 TerrainLayerMatrix(uint layer,float angle,float stretch){
 float s,c;sincos(angle,s,c);
 return float2x2(c,-s,-s,-c)/(TerrainLibraryTable[layer].x*stretch);
}
float4 TerrainLibraryColor(uint layer,float2 world,float2 dx,float2 dy,float angle,float stretch){
 float2x2 m=TerrainLayerMatrix(layer,angle,stretch);
 return TerrainLibraryAlbedo.SampleGrad(TerrainLibrarySampler,float3(mul(m,world),layer),mul(m,dx),mul(m,dy));
}
float4 TerrainLibraryDetail(uint layer,float2 world,float2 dx,float2 dy,float angle,float stretch){
 float2x2 m=TerrainLayerMatrix(layer,angle,stretch);
 float4 detail=TerrainLibrarySurface.SampleGrad(TerrainLibrarySampler,float3(mul(m,world),layer),mul(m,dx),mul(m,dy));
 float s,c;sincos(angle,s,c);float2 g=detail.xy*2-1;
 return float4(c*g.x+s*g.y,c*g.y-s*g.x,detail.zw);
}
// terrain102: hex stochastic tiling (flag 131072). The layer plane is covered by a
// triangle lattice (hexes of ~0.8 tile); each pixel blends the three lattice
// vertices around it, each sampling the same layer with its own random rotation
// and offset, so the tile period disappears. The blend is height-based (the stone
// or clump standing higher wins), so the ground stays sharp instead of a ghosted
// cross-fade; vertices that cannot win are not sampled. A zero-mean +-4% brightness
// jitter per vertex varies the ground around the layer's own mean colour.
struct TerrainHexLayer {float4 color;float4 detail;};
// One lattice vertex: its rotation (cos, sin) and offset, and the sample position/gradients.
struct TerrainHexTap {float2 at,gx,gy,turn;};
TerrainHexTap TerrainHexVertex(float2 vertex,float2 uv,float2 ux,float2 uy){
 float2 h=TerrainHash22(vertex*float2(.7071,1.3)+float2(17.1,3.7));float sn,cs;sincos(h.x*6.2831853,sn,cs);
 TerrainHexTap t;t.turn=float2(cs,sn);
 t.at=float2(cs*uv.x-sn*uv.y,sn*uv.x+cs*uv.y)+h*7.31;t.gx=float2(cs*ux.x-sn*ux.y,sn*ux.x+cs*ux.y);t.gy=float2(cs*uy.x-sn*uy.y,sn*uy.x+cs*uy.y);return t;
}
float4 TerrainHexDetail(uint layer,TerrainHexTap t){
 float4 detail=TerrainLibrarySurface.SampleGrad(TerrainLibrarySampler,float3(t.at,layer),t.gx,t.gy);
 // The sample's slope rotates back into world XY (uv' = R F world: slope = R g).
 float2 g=detail.xy*2-1;return float4(t.turn.x*g.x-t.turn.y*g.y,t.turn.y*g.x+t.turn.x*g.y,detail.zw);
}
float TerrainHexJitter(float2 vertex){return 1+(TerrainHash22(vertex+float2(5.3,9.1)).x-.5)*.08;}
// photo107: the same lattice serves the primary layer, the macro variation copy (its
// rotation and stretch) and the partner layer; colour-only callers skip the detail taps.
TerrainHexLayer TerrainLibraryHex(uint layer,float2 world,float2 dx,float2 dy,float angle,float stretch,bool wantDetail){
 float2x2 m=TerrainLayerMatrix(layer,angle,stretch);float2 uv=mul(m,world),ux=mul(m,dx),uy=mul(m,dy);
 float2 st=uv*1.25*3.4641016,skewed=float2(st.x-.57735027*st.y,1.15470054*st.y),base=floor(skewed),f2=frac(skewed);
 float fz=1-f2.x-f2.y,s=fz<0?1:0,s2=2*s-1;
 float3 w=float3(-fz*s2,s-f2.y*s2,s-f2.x*s2);
 float2 v0=base+float2(s,s),v1=base+float2(s,1-s),v2=base+float2(1-s,s);
 float wmax=max(w.x,max(w.y,w.z));
 // A vertex more than .325 below the strongest cannot win the height blend (heights 0..1): not
 // sampled. Taps are rebuilt in the detail pass (cheap ALU) rather than kept live (registers).
 float4 c0=0,c1=0,c2=0;float3 score=-1e9;
 [branch]if(w.x>wmax-.325){TerrainHexTap t=TerrainHexVertex(v0,uv,ux,uy);c0=TerrainLibraryAlbedo.SampleGrad(TerrainLibrarySampler,float3(t.at,layer),t.gx,t.gy);score.x=c0.a+4*w.x;}
 [branch]if(w.y>wmax-.325){TerrainHexTap t=TerrainHexVertex(v1,uv,ux,uy);c1=TerrainLibraryAlbedo.SampleGrad(TerrainLibrarySampler,float3(t.at,layer),t.gx,t.gy);score.y=c1.a+4*w.y;}
 [branch]if(w.z>wmax-.325){TerrainHexTap t=TerrainHexVertex(v2,uv,ux,uy);c2=TerrainLibraryAlbedo.SampleGrad(TerrainLibrarySampler,float3(t.at,layer),t.gx,t.gy);score.z=c2.a+4*w.z;}
 float top=max(score.x,max(score.y,score.z))-.3;float3 k=max(score-top,0);k/=max(k.x+k.y+k.z,1e-5);
 TerrainHexLayer o;
 o.color=k.x*float4(c0.rgb*TerrainHexJitter(v0),c0.a)+k.y*float4(c1.rgb*TerrainHexJitter(v1),c1.a)+k.z*float4(c2.rgb*TerrainHexJitter(v2),c2.a);
 o.detail=0;
 [branch]if(wantDetail){
  [branch]if(k.x>0)o.detail+=k.x*TerrainHexDetail(layer,TerrainHexVertex(v0,uv,ux,uy));
  [branch]if(k.y>0)o.detail+=k.y*TerrainHexDetail(layer,TerrainHexVertex(v1,uv,ux,uy));
  [branch]if(k.z>0)o.detail+=k.z*TerrainHexDetail(layer,TerrainHexVertex(v2,uv,ux,uy));
 }
 return o;
}
TerrainLayers TerrainLibraryGround(uint ground,float2 world,float2 dx,float2 dy){
 float4 info=TerrainLibraryTable[16+ground];uint a=(uint)info.x,b=(uint)info.y;
 TerrainLayers o;o.mean=TerrainLibraryTable[a].yzw;
 float4 near,detail;
 [branch]if(((uint)ModernMaterial.w&131072)!=0){TerrainHexLayer hex=TerrainLibraryHex(a,world,dx,dy,0,1,true);near=hex.color;detail=hex.detail;}
 else{near=TerrainLibraryColor(a,world,dx,dy,0,1);detail=TerrainLibraryDetail(a,world,dx,dy,0,1);}
 // A rotated 2.7x copy modulates brightness and hue only, so the repeat
 // breaks while the sharp detail and its normals stay from one sample.
 float4 far;
 [branch]if(((uint)ModernMaterial.w&131072)!=0)far=TerrainLibraryHex(a,world,dx,dy,.65,2.7,false).color;
 else far=TerrainLibraryColor(a,world,dx,dy,.65,2.7);
 o.albedo=near.rgb*lerp(1,clamp(far.rgb/max(o.mean,1e-3),.5,1.8),.45);
 o.height=near.a;o.slope=detail.xy;o.occlusion=detail.z;o.roughness=detail.w;
 [branch]if(b!=a){
  // Partner patches (dry grass, gravel, darker soil) follow a slow world
  // noise and fill the primary layer's low ground first.
  float macro=TerrainNoise(world/47)*.65+TerrainNoise(world/15+float2(19.3,7.1))*.35;
  // terrain98: in game the patches read as camouflage; gentler and lower contrast.
  float weight=smoothstep(.2,.95,macro)*.75;
  [branch]if(weight>0){
   float4 color,partner;
   [branch]if(((uint)ModernMaterial.w&131072)!=0){TerrainHexLayer hexB=TerrainLibraryHex(b,world,dx,dy,1.9,1,true);color=hexB.color;partner=hexB.detail;}
   else{color=TerrainLibraryColor(b,world,dx,dy,1.9,1);partner=TerrainLibraryDetail(b,world,dx,dy,1.9,1);}
   float2 h=float2(near.a+1.2*(1-weight),color.a+1.2*weight);float top=max(h.x,h.y)-.6;float2 k=max(h-top,0);
   float t=k.y/max(k.x+k.y,1e-5);
   // Both layers move 65% toward their mixed mean first: patches differ in
   // structure and moderately in tone, never as foreign coloured blotches.
   float3 meanB=TerrainLibraryTable[b].yzw,mixed=lerp(o.mean,meanB,weight);
   o.albedo=lerp(o.albedo*lerp(1,mixed/max(o.mean,1e-3),.85),color.rgb*lerp(1,mixed/max(meanB,1e-3),.85),t);o.mean=mixed;
   o.height=lerp(o.height,color.a,t);o.slope=lerp(o.slope,partner.xy,t);
   o.occlusion=lerp(o.occlusion,partner.z,t);o.roughness=lerp(o.roughness,partner.w,t);
  }
 }
 return o;
}
TerrainSide TerrainLibrarySide(TerrainLayers l,float4 identity,float tint,float3 n,float3 tangent,float3 bitangent){
 // The map's own colour survives as a tint from each region's mean native
 // colour over the layer mean. Brightness follows the map (80%) for every class,
 // so the scene's calibrated light balance holds; the class strength sets how far
 // the hue follows (map palettes: dry, green, snowy; man-made ground keeps more).
 float3 region=DecodeSRGB(identity.gba),luma=float3(.2126,.7152,.0722);
 float regionLuma=dot(region,luma),layerLuma=max(dot(l.mean,luma),1e-3),brightness=.8;
 if(regionLuma<1e-4)tint=brightness=0;
 float3 hue=clamp((region/max(regionLuma,1e-4))/(max(l.mean,1e-3)/layerLuma),.5,2);
 TerrainSide s;s.albedo=l.albedo*lerp(1,clamp(regionLuma/layerLuma,.35,2.5),brightness)*lerp(1,hue,tint);s.height=l.height;
 s.normal=Unit(tangent*l.slope.x+bitangent*l.slope.y+n*sqrt(saturate(1-dot(l.slope,l.slope))));
 s.occlusion=lerp(1,l.occlusion,.8);s.roughness=clamp(l.roughness,.3,1);return s;
}
TerrainSide TerrainNativeSide(float3 color,float2 uv,float2 gx,float2 gy,float3 n,float3 dpdx,float3 dpdy){
 TerrainSide s;s.albedo=color;s.height=.5;
 s.normal=TerrainChartNormalGrad(n,dpdx,dpdy,gx,gy,AuthoredNormal.SampleGrad(Sampler0,uv,gx,gy).xyz*2-1);
 float3 orm=AuthoredORM.SampleGrad(Sampler0,uv,gx,gy).rgb;s.occlusion=orm.r;s.roughness=clamp(orm.g,.045,1);return s;
}
TerrainSide TerrainMix(TerrainSide a,TerrainSide b,float t){
 a.albedo=lerp(a.albedo,b.albedo,t);a.height=lerp(a.height,b.height,t);a.normal=Unit(lerp(a.normal,b.normal,t));
 a.occlusion=lerp(a.occlusion,b.occlusion,t);a.roughness=lerp(a.roughness,b.roughness,t);return a;
}
float TerrainLibraryWeight(uint ground,float slope){return TerrainLibraryTable[16+min(ground,15)].w>.5?slope:0;}
// terrain104: wet ground (flag 262144). The ground shading leaves the blended layer
// height and the ground's porosity for the wet response in MaterialLight.
static float TerrainPixelHeight=.5,TerrainPixelPorosity=.6,TerrainPixelPooling=.7;
float TerrainPorosity(uint ground){
 // How much a class darkens when soaked: sand and soil drink water, stone and snow much less.
 if(ground==5)return .9;if(ground==2||ground==3)return .8;if(ground==1)return .6;if(ground==4)return .35;
 if(ground==6)return .2;if(ground==7)return .5;if(ground==8)return .45;return .6;
}
// How readily standing water forms on a class (grass and sand drain, snow absorbs).
float TerrainPooling(uint ground){
 if(ground==1)return .35;if(ground==2||ground==3||ground==7||ground==8)return 1;if(ground==4)return .8;if(ground==5)return .5;if(ground==6)return 0;return .7;
}
// The class primary layer's mean height (its 1x1 mip): pools form below each layer's own mean.
float TerrainLayerMeanHeight(uint ground){
 uint w,h,elements,levels;TerrainLibraryAlbedo.GetDimensions(0,w,h,elements,levels);
 return TerrainLibraryAlbedo.SampleLevel(TerrainLibrarySampler,float3(.5,.5,TerrainLibraryTable[16+ground].x),levels-1).a;
}
// Rain rings on standing water: a 1.6-unit grid, each cell one drop with its own phase.
float2 TerrainRipple(float2 world,float time){
 float2 cell=floor(world/1.6),gradient=0;
 [unroll]for(int k=0;k<4;++k){
  float2 c=cell+float2(k&1,k>>1),h=TerrainHash22(c*1.7+3.1),centre=(c+h)*1.6;
  float phase=frac(time*.9+h.x*7.3),radius=phase*1.1,d=distance(world,centre);
  float ring=sin(saturate(1-abs(d-radius)/.18)*3.14159)*(1-phase)*step(d,radius+.18);
  gradient+=ring*(world-centre)/max(d,1e-3);
 }
 return gradient;
}
// photo107: scattered boulders on the library ground (flag
// 4194304). One candidate boulder per 9-unit cell and its 8 neighbours; each cell hashes to
// its presence, spot, size and texture offset. Boulders are shaded, not displaced: a dome
// normal (its own relief), the stone layer's albedo and detail, and a darker contact rim.
// They add no triangles and no shadow casters; the grass and relief passes are untouched.
struct TerrainRockHit {float mask;float3 albedo;float3 normal;float occlusion;};
TerrainRockHit TerrainRockAt(float2 world,uint ground,float2 dx,float2 dy){
 TerrainRockHit r;r.mask=0;r.albedo=0;r.normal=float3(0,0,1);r.occlusion=1;
 float allowed=ground==4?1:ground==2?.6:ground==3?.5:ground==5?.25:0;
 if(allowed<=0||TerrainLibraryTable[20].w<.5)return r;
 float2 cell=floor(world/9);float best=0;float2 bestCentre=0;float bestRadius=1;float3 bestHash=0;
 [unroll]for(int j=-1;j<=1;++j)[unroll]for(int i=-1;i<=1;++i){
  float2 c=cell+float2(i,j);float2 presence=TerrainHash22(c),spot=TerrainHash22(c+float2(17.3,5.9));
  if(presence.x>=.45*allowed)continue;
  float2 centre=(c+.1+.8*spot)*9;float radius=.55+1.05*presence.y;
  float m=saturate((radius-distance(world,centre))/(.2*radius));
  if(m>best){best=m;bestCentre=centre;bestRadius=radius;bestHash=float3(spot,presence.y);}
 }
 if(best<=0)return r;
 float2 o=world-bestCentre;float d2=dot(o,o),R2=bestRadius*bestRadius;
 float s=sqrt(max(1-d2/R2,.04));float H=bestRadius*.6;
 float2 grad=-H*o/(R2*s);float3 dome=normalize(float3(-grad,1));
 uint stone=(uint)TerrainLibraryTable[20].x;float2 texel=world+bestHash.xy*40;
 float4 color=TerrainLibraryColor(stone,texel,dx,dy,0,1);float4 detail=TerrainLibraryDetail(stone,texel,dx,dy,0,1);
 r.mask=best;r.albedo=color.rgb*lerp(.85,1.1,bestHash.z);
 r.normal=normalize(float3(dome.xy+.35*detail.xy,dome.z));
 r.occlusion=lerp(.6,1,smoothstep(0,.6,s));
 return r;
}
void TerrainLibraryShade(LegacyOutput input,float3 n,uint maps,out float3 albedo,out float3 normal,out float occlusion,out float roughness){
 float2 world=input.worldPosition.xy,dx=ddx(world),dy=ddy(world);
 float3 dpdx=ddx(input.worldPosition),dpdy=ddy(input.worldPosition);
 float2 uvA=input.uv0.xy,uvB=input.uv1.xy,gxA=ddx(uvA),gyA=ddy(uvA),gxB=ddx(uvB),gyB=ddy(uvB);
 bool blend=(maps&16)!=0;float alpha=blend?saturate(input.diffuse.a):0;
 float3 nativeA=DecodeSRGB(SampleStage(0,input.uv0).rgb),nativeB=nativeA;
 if(blend)nativeB=DecodeSRGB(SampleStage(1,input.uv1).rgb);
 float4 idA=TerrainIdentityAt(uvA),idB=blend?TerrainIdentityAt(uvB):idA;
 uint gA=min((uint)round(idA.r*255),15),gB=min((uint)round(idB.r*255),15);
 // Cliff faces keep the native chart mapping authored for steep ground;
 // planar world tiling would stretch the photo layers there.
 float slope=smoothstep(.5,.75,n.z);
 float wA=TerrainLibraryWeight(gA,slope),wB=TerrainLibraryWeight(gB,slope);
 float3 tangent=Unit(float3(1,0,0)-n*n.x),bitangent=Unit(float3(0,1,0)-n*n.y);
 // terrain101: the continuous ground field (65536) replaces the per-cell class
 // staircase wherever its classes are library ground: it selects the two classes,
 // their region tints and the transition, and the one layer evaluation below
 // serves both paths (a second copy of it raised the shader's register count).
 bool field=false;float fieldT=0;uint regionA=TerrainPackRegion(idA.gba),regionB=TerrainPackRegion(idB.gba);
 [branch]if((maps&65536)!=0&&slope>0){
  TerrainFieldSample f=TerrainFieldAt(world);
  [branch]if(TerrainFieldLibrary(f)){
   field=true;fieldT=TerrainFieldTransition(f,world,dx,dy);
   // Outside the visible border band one class is drawn alone (no second layer).
   if(fieldT>.98){f.first=f.second;f.firstRegion=f.secondRegion;fieldT=0;}else if(fieldT<.02)fieldT=0;
   gA=f.first;gB=f.second;regionA=TerrainPackRegion(f.firstRegion);regionB=TerrainPackRegion(f.secondRegion);alpha=fieldT;
   wA=TerrainLibraryWeight(gA,slope);wB=TerrainLibraryWeight(gB,slope);
   TerrainPixelCoverage=(gA==1?1-fieldT:0)+(gB==1?fieldT:0);
  }
 }
 TerrainSide sideA=(TerrainSide)0;TerrainLayers layersA=(TerrainLayers)0;
 [branch]if(wA<1)sideA=TerrainNativeSide(nativeA,uvA,gxA,gyA,n,dpdx,dpdy);
 [branch]if(wA>0){layersA=TerrainLibraryGround(gA,world,dx,dy);
  TerrainSide library=TerrainLibrarySide(layersA,TerrainUnpackRegion(regionA),TerrainLibraryTable[16+gA].z,n,tangent,bitangent);
  if(wA<1)sideA=TerrainMix(sideA,library,wA);else sideA=library;}
 TerrainSide result=sideA;float blendT=0;
 [branch]if(alpha>0){
  TerrainSide sideB=(TerrainSide)0;
  [branch]if(wB<1)sideB=TerrainNativeSide(nativeB,uvB,gxB,gyB,n,dpdx,dpdy);
  [branch]if(wB>0){
   // Layers are world-space: the same class on both sides samples identical
   // texels, only the region tint differs.
   TerrainLayers layersB=layersA;
   [branch]if(gB!=gA||wA<=0)layersB=TerrainLibraryGround(gB,world,dx,dy);
   TerrainSide library=TerrainLibrarySide(layersB,TerrainUnpackRegion(regionB),TerrainLibraryTable[16+gB].z,n,tangent,bitangent);
   if(wB<1)sideB=TerrainMix(sideB,library,wB);else sideB=library;}
  // Height-aware transition: stones and clumps of the upper layer poke
  // through the lower one instead of a linear cross-fade.
  float2 h=float2(sideA.height+1.2*(1-alpha),sideB.height+1.2*alpha);float top=max(h.x,h.y)-.2;float2 k=max(h-top,0);
  float t=field?fieldT:(wA>0||wB>0?k.y/max(k.x+k.y,1e-5):alpha);
  result=TerrainMix(sideA,sideB,t);blendT=t;
 }
 TerrainPixelHeight=result.height-lerp(TerrainLayerMeanHeight(gA),TerrainLayerMeanHeight(gB),blendT)+.5;
 TerrainPixelPorosity=lerp(TerrainPorosity(gA),TerrainPorosity(gB),blendT);TerrainPixelPooling=lerp(TerrainPooling(gA),TerrainPooling(gB),blendT);
 albedo=result.albedo;normal=result.normal;occlusion=result.occlusion;roughness=result.roughness;
 // photo107: scattered rocks on the dominant class (the upper layer where the ground blends).
 [branch]if((maps&4194304)!=0){
  TerrainRockHit rock=TerrainRockAt(world,blendT>.5?gB:gA,dx,dy);
  [branch]if(rock.mask>0){
   albedo=lerp(albedo,rock.albedo,rock.mask);normal=Unit(lerp(normal,rock.normal,rock.mask));
   occlusion*=lerp(1,rock.occlusion,rock.mask);roughness=lerp(roughness,.82,rock.mask);
  }
 }
}
// accum105: settled snow (flag 524288: alpine maps, mission snow) or dust (1048576: arid
// maps) on the upward faces of world objects (not the ground, not grass). Coverage follows
// the surface normal, so detail facing up holds it first, and world-space drifts break its
// edge; walls stay clean. Vehicles move and shed most of it.
// photo107: the library ground takes the same layer only where it is nearly flat (a settled
// layer on slopes sheds), with drifts broken by the same world noise.
float AccumulationCover(float3 world,float3 normal,uint maps,bool ground){
 bool snow=(maps&524288)!=0;
 float drift=TerrainNoise(world.xy/3.7+world.z*.11)*.65+TerrainNoise(world.xy/1.1-world.z*.23)*.35;
 float start=ground?.86:(snow?.5:.35);
 float cover=saturate((normal.z-start+(drift-.5)*(ground?.25:.45))/(ground?.08:.16));
 return cover*(snow?1:.4)*(VehicleAge.w>.5?.4:1);
}
float3 AccumulationAlbedo(uint maps){return (maps&524288)!=0?float3(.80,.82,.86):float3(.46,.36,.24);}
float3 MaterialLight(float3 encodedAlbedo,LegacyOutput input,out ScreenLightingComponents screen){
 screen.diffuse=screen.specular=screen.normal=screen.directional=screen.response=0;screen.roughness=screen.occlusion=0;screen.albedo=0;
 if(input.blade>.5){
  // A leaf has its own color. The dark soil and baked ground shadows beneath
  // it are not a leaf texture; inheriting them produced black needles.
  float variation=frac(sin(dot(floor(input.worldPosition.xy),float2(12.9898,78.233)))*43758.5453);
  encodedAlbedo=lerp(float3(.24,.32,.11),float3(.43,.49,.21),variation);
  encodedAlbedo*=lerp(.80,1.08,saturate(input.blade-1));
 }
 float3 albedo=DecodeSRGB(encodedAlbedo);
 // terrain97 GPU grass (TerrainGrass.hlsl): uv2 = linear ground colour at the
 // root, uv3 = (random, height fraction, clump, 1). Roots take the ground's
 // colour; tips are lighter and vary per blade and clump.
 bool gpuGrass=input.blade>.5&&input.uv3.w>.5;
 if(gpuGrass){
  float t=saturate(input.uv3.y),variation=input.uv3.x;
  // terrain99: in game the lighter, yellowed tips read as pale straw flecks
  // (17-22% of meadow pixels >1.4x the median); tips stay close to the ground hue.
  // perf120: taller tufts stand in full sun above the shaded ground, so leaves that copied the yellow-olive ground mean
  // read as straw. Leaves are greener than the soil-mixed ground (hue toward green, 92% of its luminance); only the
  // driest clumps keep a yellowed tip.
  float3 ground=input.uv2.rgb,luma=float3(.2126,.7152,.0722);
  float3 leaf=ground*float3(.82,1,.95);leaf*=.92*dot(ground,luma)/max(dot(leaf,luma),1e-4);
  float dry=smoothstep(.6,1,input.uv3.z)*variation;
  float3 tip=leaf*lerp(1,1.1,variation)*lerp(float3(1,1,1),float3(1.08,1.02,.86),dry);
  albedo=saturate(lerp(leaf*.85,tip,smoothstep(.15,1,t)));
  // Laid-down blades show their lighter, smoother flat faces: tracks read as a sheen.
  albedo=saturate(albedo*lerp(1,1.2,saturate(input.uv3.w-1)));
 }
 bool terrainSurface=ModernMaterial.x>1.5&&((uint)ModernMaterial.w&8)!=0;
 bool library=terrainSurface&&input.blade<.5&&((uint)ModernMaterial.w&4096)!=0;
 if(terrainSurface&&input.blade<.5&&!library)albedo=TerrainBaseAlbedo(input,(uint)ModernMaterial.w);
 uint wearMaps=(uint)ModernMaterial.w;
 bool vehicle=VehicleAge.w>.5;
 float surfaceWear=vehicle?saturate(VehicleAge.x):0;
 if(vehicle&&(wearMaps&256)!=0)albedo=lerp(albedo,DecodeSRGB(WearAlbedo.Sample(Sampler0,input.uv0.xy).rgb),surfaceWear);
 float3 n=Unit(input.worldNormal),v=Unit(CameraWorld.xyz-input.worldPosition);
 if(input.blade>.5&&!gpuGrass&&dot(n,v)<0)n=-n;
 if(ModernMaterial.x>1.5&&input.blade<.5){n=Unit(cross(ddy(input.worldPosition),ddx(input.worldPosition)));if(n.z<0)n=-n;}
 float3 geometricNormal=n;
 float roughness=ModernMaterial.y,metalness=ModernMaterial.z;
 uint maps=(uint)ModernMaterial.w;float occlusion=1;
 if(library){TerrainLibraryShade(input,geometricNormal,maps,albedo,n,occlusion,roughness);metalness=0;}
 if((maps&1)!=0&&input.blade<.5&&!library){
  float3 dpdx=ddx(input.worldPosition),dpdy=ddy(input.worldPosition);
  float2 duvdx=ddx(input.uv0.xy),duvdy=ddy(input.uv0.xy);
  float3 t=cross(dpdy,n)*duvdx.x+cross(n,dpdx)*duvdy.x;
  float3 b=cross(dpdy,n)*duvdx.y+cross(n,dpdx)*duvdy.y;
  float scale=rsqrt(max(max(dot(t,t),dot(b,b)),1e-12));
  float3 map=AuthoredNormal.Sample(Sampler0,input.uv0.xy).xyz*2-1;
  if(vehicle&&(maps&512)!=0)map=normalize(lerp(map,WearNormal.Sample(Sampler0,input.uv0.xy).xyz*2-1,surfaceWear));
  if(terrainSurface){
   float3 first=TerrainChartNormal(n,dpdx,dpdy,input.uv0.xy,map);
   n=first;
   if((maps&16)!=0){float3 second=TerrainChartNormal(geometricNormal,dpdx,dpdy,input.uv1.xy,AuthoredNormal.Sample(Sampler0,input.uv1.xy).xyz*2-1);
    n=Unit(lerp(first,second,saturate(input.diffuse.a)));}
  }else{
   if((maps&16)!=0)map=lerp(map,AuthoredNormal.Sample(Sampler0,input.uv1.xy).xyz*2-1,saturate(input.diffuse.a));
   n=Unit(t*scale*map.x+b*scale*map.y+n*map.z);
  }
 }
 if((maps&2)!=0&&input.blade<.5&&!library){float3 orm=AuthoredORM.Sample(Sampler0,input.uv0.xy).rgb;
  if((maps&16)!=0)orm=lerp(orm,AuthoredORM.Sample(Sampler0,input.uv1.xy).rgb,saturate(input.diffuse.a));
  if(vehicle&&(maps&1024)!=0)orm=lerp(orm,WearORM.Sample(Sampler0,input.uv0.xy).rgb,surfaceWear);
  occlusion=orm.r;roughness=clamp(orm.g,.045,1);metalness=saturate(orm.b);}
 if(vehicle&&VehicleProfile.w<.5){
  // Broad settled dust follows actual relative height, not world movement.
  float lower=1-smoothstep(.08,.65,(input.worldPosition.z-VehicleProfile.x)/max(VehicleProfile.y,.1));
  float dust=saturate(VehicleAge.y)*lower*.32;
  albedo=lerp(albedo,float3(.20,.155,.09),dust);roughness=lerp(roughness,.9,dust);metalness*=1-dust;
  // Stock surfaces without an authored pair receive conservative fading;
  // distinct chips are supplied by asset-specific maps, not invented UV marks.
  if((maps&256)==0)roughness=lerp(roughness,.8,surfaceWear*.12);
 }
 if(input.blade>.5){roughness=.88;metalness=0;}
 // Fake AO along the blade: dense bases are shadowed by their neighbours.
 if(gpuGrass){occlusion=lerp(lerp(.45,1,smoothstep(0,.75,input.uv3.y)),1,saturate(input.uv3.w-1));roughness=lerp(roughness,.55,saturate(input.uv3.w-1));}
 if((maps&64)!=0&&input.blade<.5&&!library){
  // Height affects fine cavity shading without moving atlas UVs across seams.
  float height=AuthoredHeight.Sample(Sampler0,input.uv0.xy).r;occlusion*=lerp(.9,1,smoothstep(.1,.7,height));
 }
 // terrain104: wet library ground. Porous ground darkens and turns glossy as it soaks;
 // standing water collects in the scanned layers' low spots and spreads as wetness
 // rises (dark, mirror-smooth, flat film over the ground's own normal) and rain rings
 // ripple it while it rains. Steep ground sheds water.
 if(library&&(maps&262144)!=0&&AtmosphereWeather.z>.01){
  float wet=saturate(AtmosphereWeather.z),porous=TerrainPixelPorosity;
  // Water rises from each layer's deepest spots (mean-free heights, mean at .5) as wetness grows.
  // Puddles gather in broad shallow basins (3-8 units) whose floor is the layer's own relief.
  float basin=TerrainNoise(input.worldPosition.xy/7.5)*.6+TerrainNoise(input.worldPosition.xy/3.1+float2(4.7,1.3))*.4;
  float level=wet*.45-.08,puddle=saturate((level-(TerrainPixelHeight*.45+basin*.55))/.06)*smoothstep(.93,.99,geometricNormal.z)*TerrainPixelPooling;
  albedo*=lerp(1,1-.5*porous,wet);roughness=lerp(roughness,roughness*lerp(.75,.35,porous),wet);
  albedo*=lerp(1,.55,puddle);roughness=lerp(roughness,.035,puddle);
  float2 ripple=TerrainRipple(input.worldPosition.xy,AtmosphereWind.z)*.35*saturate(AtmosphereWeather.y);
  n=Unit(lerp(n,Unit(geometricNormal+float3(ripple,0)),puddle));
 }
 // Wet film modifies surface reflectance, preserving authored maps/UVs/AO.
 else if(AtmosphereCamera.w>.5&&input.blade<.5){
  float wet=AtmosphereWeather.z*smoothstep(.15,.8,n.z);
  albedo*=lerp(1,.82,wet);roughness=lerp(roughness,min(roughness,.19),wet*.8);
 }
 // accum105: settled snow / dust on the upward faces of world objects.
 [branch]if((!terrainSurface||library)&&input.blade<.5&&(maps&(524288|1048576))!=0){
  float cover=AccumulationCover(input.worldPosition,library?geometricNormal:n,maps,library);
  if(cover>0){bool snow=(maps&524288)!=0;
   albedo=lerp(albedo,AccumulationAlbedo(maps),cover);roughness=lerp(roughness,snow?.62:.92,cover);metalness*=1-cover;
   n=Unit(lerp(n,geometricNormal,cover*.7));occlusion=lerp(occlusion,1,cover*.5);}
 }
 roughness=FilteredRoughness(roughness,n);
 MaterialBRDFResponse surfaceBRDF=LoadMaterialBRDF(roughness,saturate(dot(n,v)),lerp(.04.xxx,albedo,metalness));
 float3 ambient=NativeCalibratedGlobal(GlobalAmbient.rgb);
 float3 result=DecodeSRGB(MaterialEmissive.rgb);
 if((maps&128)!=0)result+=DecodeSRGB(AuthoredEmissive.Sample(Sampler0,input.uv0.xy).rgb);
 // All directional lights share the native solar depth atlas and cloud field.
 // The uniform guard keeps derivative evaluation coherent across the quad.
 bool hasDirectional=LightPosition[0].w>2.5||LightPosition[1].w>2.5||LightPosition[2].w>2.5||LightPosition[3].w>2.5;
 float solarVisibility=1;
 // terrain98: a blade's own shadow-map texel would self-shadow it (speckle);
 // its lookup is offset toward the sun by about a blade width.
 float3 shadowPosition=gpuGrass?input.worldPosition+ShadowSun.xyz*.35:input.worldPosition;
 [branch]if(hasDirectional)solarVisibility=SceneVisibility(shadowPosition,n)*CloudTransmission(input.worldPosition);
 // terrain98: analytic shadows of the GPU grass tufts on the ground (TerrainGrass.hlsl).
 [branch]if(terrainSurface&&input.blade<.5&&(maps&32768)!=0&&LightPosition[0].w>2.5){
  float footprint=max(length(ddx(input.worldPosition)),length(ddy(input.worldPosition)));
  float grassShade=GrassGroundShadow(input.worldPosition,Unit(-LightDirection[0].xyz),footprint);
  solarVisibility*=grassShade;occlusion*=lerp(1,grassShade,.5);
 }
 [unroll]for(int light=0;light<4;++light){
  if(LightPosition[light].w<.5)continue;
  float3 towardLight;float attenuation=1;
  if(LightPosition[light].w>2.5)towardLight=-LightDirection[light].xyz;
  else{towardLight=LightPosition[light].xyz-input.worldPosition;float distance=length(towardLight);
   attenuation=distance<=LightAttenuation[light].w?1/max(dot(LightAttenuation[light].xyz,float3(1,distance,distance*distance)),1e-6):0;
   if(LightPosition[light].w>1.5){float spot=dot(-Unit(towardLight),Unit(LightDirection[light].xyz));
    attenuation*=NativeSpotAttenuation(spot,LightSpot[light].xyz);}}
  // Calibrate the legacy unit diffuse light to Lambert radiance; GGX stays in linear light.
  float3 radiance=NativeCalibratedDirect(light,LightDiffuse[light].rgb)*attenuation;
  if(LightPosition[light].w>2.5)radiance*=solarVisibility;
  else radiance*=LocalSurfaceVisibility(input.worldPosition,LightDirection[light].w,LightPosition[light].w);
  float3 contribution=EvaluateGGX(albedo,roughness,metalness,n,v,Unit(towardLight),surfaceBRDF.energy)*radiance;
  // Thin blades transmit back-light toward the tip (reference translucency term).
  if(gpuGrass)contribution+=albedo*radiance*.15*saturate(input.uv3.y)*pow(saturate(dot(-v,Unit(towardLight))),4);
  result+=contribution;if(LightPosition[light].w>2.5&&(uint(ScreenCaptureControl.y)&(1u<<light)))screen.directional+=contribution;
  ambient+=NativeCalibratedAmbient(light,LightAmbient[light].rgb)*attenuation;
 }
 float weatherFill=lerp(1,.65,AtmosphereCamera.w*AtmosphereWeather.x);
 // Intracloud flash is analytic transient direct light. It must not persist
 // through the eight-Hz distant reflection history or replace authored maps.
 if(AtmosphereFlashRadiance.w>0&&AtmosphereCamera.w>.5)result+=EvaluateGGX(albedo,roughness,metalness,n,v,Unit(AtmosphereFlashPosition.xyz-input.worldPosition),surfaceBRDF.energy)*StormRadiance(input.worldPosition);
 IndirectLightingComponents components;
 float3 indirect;
 [branch]if(AtmosphereCamera.w>.5&&AtmosphereShadowArea.w>.5&&EnvironmentSettings.x>.5){
  float blend=saturate(AtmosphereSunColor.w);float3 reflection=reflect(-v,n);
  float3 reflected=lerp(PreviousWeatherReflection.SampleLevel(AtmosphereClamp,reflection,roughness*6).rgb,
                       WeatherReflection.SampleLevel(AtmosphereClamp,reflection,roughness*6).rgb,blend);
  float3 irradiance=lerp(PreviousWeatherDiffuse.SampleLevel(AtmosphereClamp,n,0).rgb,
                        WeatherDiffuse.SampleLevel(AtmosphereClamp,n,0).rgb,blend);
  float nv=saturate(dot(n,v));float3 f0=lerp(.04.xxx,albedo,metalness);
  float3 fresnel=f0+(max((1-roughness).xxx,f0)-f0)*pow(1-nv,5);
  float2 brdf=surfaceBRDF.integrated;
  // Weather cubes contain absolute native solar/ambient illumination. Do not
  // square ambient by multiplying it again. Additional native local ambient
  // remains per-object, and is not copied into the global distant environment.
  float specularAO=SpecularOcclusion(nv,roughness,occlusion)*ReflectionHorizon(n,v,geometricNormal);
  indirect=(1-fresnel)*(1-metalness)*albedo*irradiance*occlusion+reflected*(f0*brdf.x+brdf.y)*surfaceBRDF.energy*specularAO;
  screen.diffuse=(1-fresnel)*(1-metalness)*albedo*irradiance*occlusion;screen.specular=reflected*(f0*brdf.x+brdf.y)*surfaceBRDF.energy*specularAO;
  indirect+=EvaluateIndirect(albedo,roughness,metalness,n,v,max(ambient-AtmosphereAmbient.rgb,0),occlusion,geometricNormal,surfaceBRDF,components)*weatherFill;
  screen.diffuse+=components.diffuse*weatherFill;screen.specular+=components.specular*weatherFill;
 }else{
  // The weather branch replaces the distant IBL completely. Evaluate this
  // fallback only when it is used, retaining exactly the same final response.
  indirect=EvaluateIndirect(albedo,roughness,metalness,n,v,ambient,occlusion,geometricNormal,surfaceBRDF,components)*weatherFill;
  screen.diffuse=components.diffuse*weatherFill;screen.specular=components.specular*weatherFill;
 }
 screen.normal=n;screen.roughness=roughness;screen.occlusion=occlusion;screen.albedo=albedo;
 float3 surfaceF0=lerp(.04.xxx,albedo,metalness);
 screen.response=(surfaceF0*surfaceBRDF.integrated.x+surfaceBRDF.integrated.y)*surfaceBRDF.energy*SpecularOcclusion(saturate(dot(n,v)),roughness,occlusion)*ReflectionHorizon(n,v,geometricNormal);
 return result+indirect;
}
/*PIXEL_PROGRAM*/
// water107: the game's water (flag 2097152) with its soft shore computed per pixel from the
// scene depth behind it. Zero Hour writes the shore opacity into destination alpha with flat
// per-cell quads that the tessellated, relief-displaced ground occludes in triangles (shards
// along the shore). Opacity follows the native curve min(depth/(transparent*floor), floor)
// on the true depth below the water; a thin band of drifting foam marks the waterline.
cbuffer WaterData:register(b6){float4 WaterTransparency;}; // x transparent depth, y min opacity, z foam, w depth bound
Texture2D<float> WaterSceneDepth:register(t60);
// photo107: screen-space reflection of the scene on water (flag 8388608). The water is flat: the
// reflected view ray marches the pre-water scene copy (WaterSceneColour, t61) in screen space,
// with the depth of the same pass (WaterSceneDepth, t60); a hit contributes its radiance, a miss
// the atmosphere's reflection cube. Linear light, added after the sRGB decode of the legacy colour.
Texture2D<float4> WaterSceneColour:register(t61);
// Handedness-neutral: the left-handed form (P23 = 1, P33 = 0) is the same expression as before.
float WaterLinearDepth(float depth){return abs((Projection[3][2]-depth*Projection[3][3])/(depth*Projection[2][3]-Projection[2][2]));}
float3 WaterReflection(float3 world,float3 eye){
 float3 N=float3(0,0,1),toEye=eye-world;toEye*=rsqrt(max(dot(toEye,toEye),1e-12));
 float fresnel=.02+.98*pow(1-saturate(dot(N,toEye)),5);
 float3 R=reflect(-toEye,N),origin=world+N*.05;
 float3 found=0;bool hit=false;
 [loop]for(int i=1;i<=24;++i){
  float3 Q=origin+R*(.5*i+.04*i*i);
  float4 clip=mul(mul(float4(Q,1),View),Projection);
  if(clip.w<=1e-4)break;
  float2 uv=float2(clip.x/clip.w*.5+.5,.5-clip.y/clip.w*.5);
  if(any(uv<=0)||any(uv>=1))break;
  int2 pixel=int2(uv*Viewport.zw+Viewport.xy);
  float sceneDepth=WaterSceneDepth.Load(int3(pixel,0));
  if(sceneDepth>=1-1e-5)continue;
  // The visible surface lies in front of the ray point by a little, but not far in front.
  float sceneW=WaterLinearDepth(sceneDepth);
  if(clip.w>sceneW+.1&&clip.w<sceneW+2.5){found=WaterSceneColour.Load(int3(pixel,0)).rgb;hit=true;break;}
 }
 float3 sky=0;
 [branch]if(!hit&&AtmosphereCamera.w>.5&&AtmosphereShadowArea.w>.5&&EnvironmentSettings.x>.5){
  float blend=saturate(AtmosphereSunColor.w);
  sky=lerp(PreviousWeatherReflection.SampleLevel(AtmosphereClamp,R,0).rgb,WeatherReflection.SampleLevel(AtmosphereClamp,R,0).rgb,blend);
 }
 return (hit?found:sky)*fresnel*.9;
}
float4 ModernWater(float4 native,LegacyOutput input){
 float floorOpacity=saturate(WaterTransparency.y);
 // A single exit: without the scene depth the shore is the floor opacity only.
 float4 result=native;result.a=floorOpacity;
 [branch]if(WaterTransparency.w>=.5){
  float sampled=WaterSceneDepth.Load(int3(input.position.xy,0));
  float4 water=mul(float4(input.worldPosition,1),View);
  float behind=(Projection[3][2]-sampled*Projection[3][3])/(sampled*Projection[2][3]-Projection[2][2]);
  float3 viewGround=abs(Projection[2][3])>.5?water.xyz*(behind/water.z):float3(water.xy,behind);
  float3 ground=mul((float3x3)View,viewGround-View[3].xyz);
  float depth=max(input.worldPosition.z-ground.z,0);
  float opacity=min(depth/(max(WaterTransparency.x,1e-3)*max(floorOpacity,1e-3)),floorOpacity);
  float time=AtmosphereWind.z;
  float drift=TerrainNoise(input.worldPosition.xy*.9+float2(time*.35,time*.21))*.6+TerrainNoise(input.worldPosition.xy*2.3-float2(time*.5,0))*.4;
  float foam=(1-smoothstep(0,1.4,depth))*smoothstep(.02,.2,depth)*smoothstep(.35,.75,drift)*WaterTransparency.z;
  float light=saturate(max(input.diffuse.r,max(input.diffuse.g,input.diffuse.b))*1.6);
  // Depth colour: water absorbs red first, so deeper water reads darker and bluer.
  float absorb=saturate(depth/(max(WaterTransparency.x,1e-3)*3));
  result.rgb=lerp(result.rgb,float3(.10,.24,.30)*light,absorb*.45);
  result.rgb=lerp(result.rgb,float3(.86,.9,.93)*light,foam*.75);
  result.a=max(opacity,foam*.8);
 }
 return result;
}
struct LegacyLightingOutput {float4 color:SV_TARGET0;float4 diffuseIndirect:SV_TARGET1;float4 specularIndirect:SV_TARGET2;float4 normalOcclusion:SV_TARGET3;float4 directional:SV_TARGET4;float4 response:SV_TARGET5;float4 motion:SV_TARGET6;float4 albedo:SV_TARGET7;};
// Photoreal decal (track marks, building aprons): a multiplicative stain of the lit ground. The native coverage (texture alpha
// times the edge fade) feathers the stain and the native colour sets its tint, darkened to a stain. The relief (screen-space
// slope of the coverage, lit from the upper left) shades the imprint edges; flat ground gives 1.
float3 DecalStain(float3 nativeRgb,float cover,float relief){
 float3 tint=DecodeSRGB(saturate(nativeRgb))*.72;
 return lerp(float3(1,1,1),tint,cover)*lerp(1,relief,cover);
}
LegacyLightingOutput LegacyPS(LegacyOutput input){
 LegacyLightingOutput output;output.color=output.diffuseIndirect=output.specularIndirect=output.normalOcclusion=output.directional=output.response=0;output.motion=0;output.albedo=0;
 ScreenLightingComponents screen;screen.diffuse=screen.specular=screen.normal=screen.directional=screen.response=0;screen.roughness=screen.occlusion=0;screen.albedo=0;
 float4 coords[8]={input.uv0,input.uv1,input.uv2,input.uv3,input.uv4,input.uv5,input.uv6,input.uv7};
 float4 result=LegacyPixel(input.diffuse,input.specular,coords);
  // Photoreal decal: cameraWorld.w marks the draw (CompatibilityRenderer). The stain is computed from the pixel as LegacyPixel returns it,
  // written at the end; the derivatives stay in uniform flow, and a decal draw skips the alpha test.
  const bool decal=CameraWorld.w>.5;
  const float decalCover=smoothstep(.04,.9,saturate(result.a));
  const float2 decalSlope=float2(ddx(decalCover),ddy(decalCover))*.5;
  const float3 decalLightDir=normalize(float3(-.45,-.55,.7)),decalNormal=normalize(float3(-decalSlope,1));
  const float3 decalRgb=DecalStain(result.rgb,decalCover,clamp(dot(decalNormal,decalLightDir)/decalLightDir.z,.7,1.3));
 [branch]if(((uint)ModernMaterial.w&2097152)!=0)result=ModernWater(result,input);
 if(VehicleAge.w>.5&&VehicleProfile.w>.5){
  float factor=max(VehicleAge.x,VehicleAge.y);
  // Top-right of the atlas contains corrosion/oil runoff. Corrosion appears
  // later than dry dust/abrasion; source UV registration stays unchanged.
  if(input.uv0.x>.5&&input.uv0.y<.5)factor=VehicleAge.z;
  result.a*=saturate(factor);
 }
 if(AlphaStage[0].w!=0){float reference=BridgeFlags.y;uint compare=AlphaStage[0].w;
  bool accepted=compare==1?false:compare==2?result.a<reference:compare==3?result.a==reference:compare==4?result.a<=reference:
    compare==5?result.a>reference:compare==6?result.a!=reference:compare==7?result.a>=reference:true;
  if(!accepted&&!decal)discard;}
 bool modern=ModernMaterial.x>.5;
 if(modern)result.rgb=MaterialLight(result.rgb,input,screen);
 else if(GeometryFlags.w>.5)result.rgb+=input.specular.rgb;
 // accum105: the same layer on retail-lit objects, lit per pixel like their vertex lighting
 // (ambient + directional Lambert in native units); the sun shadow below applies to it too.
 [branch]if(!modern&&input.blade<.5&&((uint)ModernMaterial.w&(524288|1048576))!=0&&((uint)ModernMaterial.w&8)==0){
  uint maps=(uint)ModernMaterial.w;float3 nrm=Unit(input.worldNormal);
  float cover=AccumulationCover(input.worldPosition,nrm,maps,false);
  if(cover>0){
   float3 light=GlobalAmbient.rgb;
   [unroll]for(int i=0;i<4;++i)if(LightPosition[i].w>2.5)light+=LightAmbient[i].rgb+LightDiffuse[i].rgb*max(0,dot(nrm,Unit(-LightDirection[i].xyz)));
   float3 lit=GeometryFlags.y>.5?saturate(light):dot(input.diffuse.rgb,float3(.3,.59,.11)).xxx;
   result.rgb=lerp(result.rgb,EncodeSRGB(AccumulationAlbedo(maps))*lit,cover);
  }
 }
 // Generic vehicle dust preserves stock lighting and needs no new UV marks.
 if(!modern&&VehicleAge.w>.5&&VehicleProfile.w<.5){
  float low=1-smoothstep(.08,.65,(input.worldPosition.z-VehicleProfile.x)/max(VehicleProfile.y,.1));
  float dust=saturate(VehicleAge.y)*low*.18;
  result.rgb=lerp(result.rgb,float3(.48,.42,.32),dust);
 }
 if(!modern&&((uint)ModernMaterial.w&4)!=0)result.rgb*=pow(.45+.55*saturate(SceneVisibility(input.worldPosition,input.worldNormal)),1.0/2.2);
 if(FogSettings.x>.5){float fog=GeometryFlags.x>.5?input.fog:FogSettings.x==2?exp(-FogSettings.y*input.fog):
   FogSettings.x==3?exp(-pow(FogSettings.y*input.fog,2)):saturate((FogSettings.z-input.fog)/max(FogSettings.z-FogColor.a,1e-6));
   result.rgb=lerp(modern?DecodeSRGB(FogColor.rgb):FogColor.rgb,result.rgb,saturate(fog));
   screen.diffuse*=saturate(fog);screen.specular*=saturate(fog);screen.directional*=saturate(fog);screen.response*=saturate(fog);}
 // Retail inputs are gamma-encoded. Decode once before the linear scene target.
 if(!modern&&BridgeFlags.x<.5)result.rgb=DecodeSRGB(result.rgb);
 // photo107: the water's screen-space reflection, in linear light (only when the scene copy is bound).
 [branch]if(((uint)ModernMaterial.w&8388608)!=0&&WaterTransparency.w>.5)result.rgb+=WaterReflection(input.worldPosition,CameraWorld.xyz);
 // Screen/HUD and arbitrary projected passes bind no atmosphere constants.
 float4 volume=AtmosphereIntegral(input.worldPosition);result.rgb=result.rgb*volume.a+volume.rgb;
 screen.diffuse*=volume.a;screen.specular*=volume.a;screen.directional*=volume.a;screen.response*=volume.a;
 bool capture=ScreenCaptureControl.x>.5&&modern&&all(isfinite(result.rgb))&&all(result.rgb>=0)&&all(result.rgb<=65504)&&all(isfinite(screen.normal))&&dot(screen.normal,screen.normal)>.5;
 // terrain98: thin grass blades must not act as screen-space AO / contact-shadow
 // occluders or receivers (they turned every tuft into a dark speck); their
 // pixels are marked invalid for those passes and keep their own fake AO.
 if(capture&&input.blade>.5&&input.uv3.w>.5)capture=false;
 if(capture){
  output.diffuseIndirect=float4(HDRSurfaceRadiance(screen.diffuse),1);
  output.specularIndirect=float4(HDRSurfaceRadiance(screen.specular),screen.roughness);
  output.normalOcclusion=float4(screen.normal,1+saturate(screen.occlusion));
  output.directional=float4(HDRSurfaceRadiance(screen.directional),1);
  output.response=float4(HDRSurfaceRadiance(screen.response),screen.roughness);
  output.albedo=float4(screen.albedo,1);
 }
 if(modern)result.rgb=HDRSurfaceRadiance(result.rgb);output.color=result;
 // photo107: object motion of a moving unit (MotionSettings.x, not instanced): the surface's UV offset between
 // the previous and current unjittered frames. The previous position is the world point carried through
 // MotionPreviousWorld (inverse(current world) * previous world), so no interpolant is needed. Everything
 // else writes zero (alpha 0).
 if(MotionSettings.x>.5&&InstanceSettings.x==0){
  float4 worldPoint=float4(input.worldPosition,1);
  float4 current=mul(worldPoint,MotionCurrentVP),previous=mul(mul(worldPoint,MotionPreviousWorld),MotionPreviousVP);
  if(abs(current.w)>1e-6&&abs(previous.w)>1e-6){
   float2 currentNdc=current.xy/current.w,previousNdc=previous.xy/previous.w;
   output.motion=float4(clamp(float2(previousNdc.x-currentNdc.x,currentNdc.y-previousNdc.y)*.5,-1,1),0,1);
  }
 }
  // Decal draws: the stain alone; every other output stays zero (the early return did this).
  if(decal){output=(LegacyLightingOutput)0;output.color=float4(decalRgb,1);}
  return output;
}
LegacyOutput LegacyShadowVS(LegacyInput input){
 LegacyOutput output=FixedVS(input);
 row_major float4x4 lightMatrix=ShadowRender.x<.5?SceneLightVP:SceneFarVP;
 output.position=mul(float4(output.worldPosition,1),lightMatrix);return output;
}
void LegacyShadowPS(LegacyOutput input){
 // Opaque depth-only casters have no alpha rejection and no color output.
 // Keep every fixed stage and comparison live for actual alpha-tested casters.
 [branch]if(AlphaStage[0].w==0)return;
 float4 coords[8]={input.uv0,input.uv1,input.uv2,input.uv3,input.uv4,input.uv5,input.uv6,input.uv7};
 float alpha=FixedPixel(input.diffuse,input.specular,coords).a;
 if(AlphaStage[0].w!=0){float reference=BridgeFlags.y;uint compare=AlphaStage[0].w;
  bool accepted=compare==1?false:compare==2?alpha<reference:compare==3?alpha==reference:compare==4?alpha<=reference:
   compare==5?alpha>reference:compare==6?alpha!=reference:compare==7?alpha>=reference:true;
  if(!accepted)discard;}
}
