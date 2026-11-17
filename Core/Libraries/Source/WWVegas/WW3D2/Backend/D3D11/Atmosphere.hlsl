// SPDX-License-Identifier: GPL-3.0-or-later
#include "AtmosphereCommon.hlsl"
#include "WeatherSky.hlsl"
#include "LocalLightShadows.hlsl"
Texture2DArray<float> AtmosphereSceneShadow:register(t10);
SamplerComparisonState AtmosphereComparison:register(s9);
cbuffer AtmosphereShadowData:register(b2){
 row_major float4x4 AtmosphereNearVP;float4 AtmosphereNearSettings;
 row_major float4x4 AtmosphereFarVP;float4 AtmosphereFarSettings;
 float4 AtmosphereShadowDepth,AtmosphereSplit,AtmosphereRender,AtmosphereShadowSun;
};
float AtmosphereVisibility(float3 world){
 if(AtmosphereNearSettings.w<.5)return 1;
 float depth=dot(float4(world,1),AtmosphereShadowDepth);
 if(AtmosphereSplit.w>1.5&&depth>AtmosphereSplit.z)return 1;
 uint cascade=AtmosphereSplit.w>1.5&&depth>AtmosphereSplit.x?1:0;
 float4 clip=mul(float4(world,1),cascade==0?AtmosphereNearVP:AtmosphereFarVP);
 float3 p=clip.xyz/max(clip.w,1e-6);float2 uv=p.xy*float2(.5,-.5)+.5;
 if(any(uv<0)||any(uv>1)||p.z<=0||p.z>=1)return 1;
 return AtmosphereSceneShadow.SampleCmpLevelZero(AtmosphereComparison,float3(uv,cascade),p.z-.0001);
}
struct LocalVolumeLight {float4 positionRange,radianceType,direction,attenuation,spot;};
cbuffer LocalVolumeData:register(b10){uint4 LocalVolumeMetadata;LocalVolumeLight LocalVolumeLights[128];};
RWStructuredBuffer<uint4> LocalClusterOutput:register(u0);
StructuredBuffer<uint4> LocalClusterMasks:register(t35);
[numthreads(4,4,4)]void CSLocalClusters(uint3 id:SV_DispatchThreadID){
 if(any(id>=uint3(20,12,8)))return;
 float3 low=float3(1e30,1e30,1e30),high=-low;bool veryWide=false;
 [unroll]for(uint corner=0;corner<8;++corner){
  float2 cell=min(float2(id.xy*8+uint2((corner&1)?8:0,(corner&2)?8:0)),float2(160,90));
  float3 endpoint=AtmosphereUnproject(cell/float2(160,90),1);float3 delta=endpoint-AtmosphereCamera.xyz;
  float viewDistance=max(dot(float4(endpoint,1),AtmosphereViewport),.001);
  veryWide=veryWide||viewDistance/max(length(delta),.001)<.05;
  // The integrator samples z-.5, not a slice's upper edge. Enclose that exact
  // interval so the first sample in every cluster cannot lose a nearby light.
  float depth=clamp(float(id.z*8+((corner&4)?8:0))-.5,0,62.5)*AtmosphereRange.x/63;
  float3 p=AtmosphereCamera.xyz+delta*(depth/viewDistance);low=min(low,p);high=max(high,p);
 }
 // The froxel march clamps extreme view angles. Including the origin in this
 // rare case conservatively encloses its shortened rays without dropping light.
 if(veryWide){low=min(low,AtmosphereCamera.xyz);high=max(high,AtmosphereCamera.xyz);}
 low-=.05;high+=.05;
 uint4 mask=uint4(0,0,0,0);
 [loop]for(uint light=0;light<min(LocalVolumeMetadata.x,128u);++light){
  float4 sphere=LocalVolumeLights[light].positionRange;float3 nearest=clamp(sphere.xyz,low,high);float3 d=nearest-sphere.xyz;
  if(dot(d,d)<=sphere.w*sphere.w){uint word=light>>5,bit=1u<<(light&31);
   mask.x|=word==0?bit:0;mask.y|=word==1?bit:0;mask.z|=word==2?bit:0;mask.w|=word==3?bit:0;
  }
 }
 LocalClusterOutput[(id.z*12+id.y)*20+id.x]=mask;
}
float3 LocalVolumeRadiance(float3 world,float3 ray,uint3 froxel){
 float3 sum=float3(0,0,0);
 [branch]if(LocalVolumeMetadata.x>0){
 uint3 cluster=froxel/8;uint4 mask=LocalClusterMasks[(cluster.z*12+cluster.y)*20+cluster.x];
 [loop]for(uint word=0;word<4;++word){uint bits=mask[word];
  [loop]while(bits){uint bit=(uint)firstbitlow(bits)&31u;bits&=bits-1;uint index=word*32+bit;
   LocalVolumeLight light=LocalVolumeLights[index];float3 towardLight=light.positionRange.xyz-world;float distance=length(towardLight);
   if(distance>light.positionRange.w)continue;
   float3 direction=towardLight/max(distance,.001);
   float attenuation=1/max(dot(light.attenuation.xyz,float3(1,distance,distance*distance)),1e-6);
   if(light.radianceType.w>1.5&&distance>.001){float cosine=dot(-direction,light.direction.xyz);
    if(cosine<=light.spot.y)continue;
    attenuation*=light.spot.z==0?1:pow(saturate((cosine-light.spot.y)/max(light.spot.x-light.spot.y,1e-6)),light.spot.z);
   }
   float cosine=dot(ray,direction),g=.45;
   float phase=(1-g*g)/(12.5663706*pow(max(1+g*g-2*g*cosine,.01),1.5));
   sum+=light.radianceType.rgb*attenuation*phase*LocalLightVisibility(world,light.direction.w,light.radianceType.w);
  }
 }
 }
 return sum;
}
RWTexture2D<float> CloudShadowOutput:register(u0);
[numthreads(8,8,1)]void CSCloudShadow(uint3 id:SV_DispatchThreadID){
 if(any(id.xy>=256))return;
 float2 xy=((float2(id.xy)+.5)/256-.5)*AtmosphereShadowArea.z+AtmosphereShadowArea.xy;
 float height=AtmosphereRange.z-AtmosphereRange.y;
 float dz=height/8;
 float opticalDepth=0;
 [unroll]for(uint i=0;i<8;++i){
  float z=AtmosphereRange.y+(i+.5)*dz;
  float3 samplePosition=float3(xy+AtmosphereSun.xy/max(AtmosphereSun.z,.15)*z,z);
  opticalDepth+=CloudDensity(samplePosition)*AtmosphereRange.w*dz/max(AtmosphereSun.z,.15);
 }
 CloudShadowOutput[id.xy]=exp(-min(opticalDepth,12));
}
RWTexture3D<float4> AtmosphereOutput:register(u0);
[numthreads(8,8,1)]void CSAtmosphere(uint3 id:SV_DispatchThreadID){
 if(any(id.xy>=uint2(160,90)))return;
 float2 uv=(float2(id.xy)+.5)/float2(160,90);
 float3 endpoint=AtmosphereUnproject(uv,1);
 float3 delta=endpoint-AtmosphereCamera.xyz;
 float viewDistance=max(dot(float4(endpoint,1),AtmosphereViewport),.001);
 float3 ray=delta/max(length(delta),.001);
 float viewCosine=max(viewDistance/max(length(delta),.001),.05);
 float stepLength=AtmosphereRange.x/63/viewCosine;
 float cosTheta=dot(ray,AtmosphereSun.xyz);
 float g=.45;float phase=(1-g*g)/(12.5663706*pow(max(1+g*g-2*g*cosTheta,.01),1.5));
 float3 scatter=0;float transmission=1;
 AtmosphereOutput[uint3(id.xy,0)]=float4(0,0,0,1);
 [loop]for(uint z=1;z<64;++z){
  float3 p=AtmosphereCamera.xyz+ray*((z-.5)*stepLength);
  float extinction=AtmosphereWeather.w*exp(-max(p.z,0)/700);
  float segment=exp(-extinction*stepLength);
  float visibility=AtmosphereVisibility(p)*CloudTransmission(p);
  float3 light=AtmosphereSunColor.rgb*(AtmosphereSun.w*phase*visibility)+AtmosphereAmbient.rgb*lerp(1,.65,AtmosphereWeather.x)+LocalVolumeRadiance(p,ray,uint3(id.xy,z));
  if(AtmosphereFlashRadiance.w>0){float3 towardFlash=AtmosphereFlashPosition.xyz-p;float flashCosine=dot(ray,towardFlash/max(length(towardFlash),.001));light+=StormRadiance(p)*SkyHG(flashCosine,.45);}
  scatter+=transmission*(1-segment)*.92*light;
  transmission*=segment;
  AtmosphereOutput[uint3(id.xy,z)]=float4(min(scatter,65504),transmission);
 }
}
struct AtmosphereQuad {float4 position:SV_POSITION;float2 uv:TEXCOORD0;};
cbuffer CloudHistoryData:register(b9){
 row_major float4x4 CloudPreviousVP;
 float4 CloudPreviousCamera,CloudDisplacement,CloudHistorySettings,CloudPixels;
};
AtmosphereQuad VSAtmosphere(uint vertex:SV_VertexID){
 AtmosphereQuad o;o.uv=float2((vertex<<1)&2,vertex&2);o.position=float4(o.uv*float2(2,-2)+float2(-1,1),1,1);return o;
}
// Native RTS background retains weather color but no visible cloud geometry.
float4 PSRTSBackground(AtmosphereQuad input):SV_TARGET{
 float3 ray=normalize(AtmosphereUnproject(input.uv,1)-AtmosphereCamera.xyz);
 return float4(WeatherBackgroundRadiance(ray),1);
}
float4 PSCloudSky(AtmosphereQuad input):SV_TARGET{
 float3 ray=normalize(AtmosphereUnproject(input.uv,1)-AtmosphereCamera.xyz);
 float footprint=max(length(ddx(ray)),length(ddy(ray)));
 float jitter=frac(52.9829189*frac(dot(input.position.xy,float2(.06711056,.00583715)))+CloudHistorySettings.z*.61803399);
 float cloudDistance;float3 radiance=WeatherSkyRadiance(AtmosphereCamera.xyz,ray,48,footprint,.05+.9*jitter,cloudDistance,true);
 return float4(radiance,cloudDistance);
}
TextureCube<float4> RawWeatherSky:register(t33);
RWTexture2DArray<float4> WeatherProbeOutput:register(u0);
cbuffer WeatherProbeFilter:register(b8){float4 ProbeFilter;} // mip roughness, side, unused, unused
float ProbeRadicalInverse(uint bits){
 bits=(bits<<16)|(bits>>16);bits=((bits&0x55555555)<<1)|((bits&0xaaaaaaaa)>>1);
 bits=((bits&0x33333333)<<2)|((bits&0xcccccccc)>>2);
 bits=((bits&0x0f0f0f0f)<<4)|((bits&0xf0f0f0f0)>>4);
 bits=((bits&0x00ff00ff)<<8)|((bits&0xff00ff00)>>8);return float(bits)*2.3283064365386963e-10;
}
[numthreads(8,8,1)]void CSWeatherSky(uint3 id:SV_DispatchThreadID){
 if(any(id.xy>=64)||id.z>=6)return;
 float3 ray=WeatherCubeDirection(id.z,(float2(id.xy)+.5)/64*2-1);
 float depth;WeatherProbeOutput[id]=float4(WeatherSkyRadiance(AtmosphereCamera.xyz,ray,32,2.0/64,.5,depth,false),1);
}
void ProbeBasis(float3 n,out float3 tangent,out float3 bitangent){
 tangent=normalize(cross(abs(n.z)<.999?float3(0,0,1):float3(1,0,0),n));bitangent=cross(n,tangent);
}
[numthreads(8,8,1)]void CSWeatherSpecular(uint3 id:SV_DispatchThreadID){
 uint side=(uint)ProbeFilter.y;if(any(id.xy>=side)||id.z>=6)return;
 float3 n=WeatherCubeDirection(id.z,(float2(id.xy)+.5)/side*2-1);
 float3 sum=0;float weight=0;float3 tangent,bitangent;ProbeBasis(n,tangent,bitangent);
 if(ProbeFilter.x<=0){WeatherProbeOutput[id]=float4(RawWeatherSky.SampleLevel(AtmosphereClamp,n,0).rgb,1);return;}
 float alpha=ProbeFilter.x*ProbeFilter.x;
 [loop]for(uint i=0;i<64;++i){
  float phi=6.2831853*float(i)/64,xi=ProbeRadicalInverse(i);
  float z=sqrt((1-xi)/max(1+(alpha*alpha-1)*xi,.000001));float r=sqrt(max(0,1-z*z));
  float3 h=tangent*(r*cos(phi))+bitangent*(r*sin(phi))+n*z;
  float3 l=normalize(2*dot(n,h)*h-n);float nl=saturate(dot(n,l));
  if(nl>0){sum+=RawWeatherSky.SampleLevel(AtmosphereClamp,l,0).rgb*nl;weight+=nl;}
 }
 WeatherProbeOutput[id]=float4(sum/max(weight,.000001),1);
}
[numthreads(8,8,1)]void CSWeatherDiffuse(uint3 id:SV_DispatchThreadID){
 if(any(id.xy>=16)||id.z>=6)return;
 float3 n=WeatherCubeDirection(id.z,(float2(id.xy)+.5)/16*2-1);
 float3 tangent,bitangent;ProbeBasis(n,tangent,bitangent);float3 sum=0;
 // Cosine importance sampling estimates E/pi, matching the existing SH ABI.
 [loop]for(uint i=0;i<64;++i){
  float xi=(float(i)+.5)/64,phi=6.2831853*ProbeRadicalInverse(i),r=sqrt(xi),z=sqrt(1-xi);
  float3 direction=tangent*(r*cos(phi))+bitangent*(r*sin(phi))+n*z;
  sum+=RawWeatherSky.SampleLevel(AtmosphereClamp,direction,0).rgb;
 }
 WeatherProbeOutput[id]=float4(sum/64,1);
}
// Camera-centered deterministic GPU rain. No CPU particles, readback, or hidden
// object queries. Z-tested, no depth write; HUD is rendered after this pass.
struct RainVertex {float4 position:SV_POSITION;float2 uv:TEXCOORD0;};
Texture2D<float> RainSceneDepth:register(t37);
cbuffer RainDepthConstants:register(b13){float4 RainDepthViewport;float4 RainDepthSettings;};
float RainSurfaceFade(RainVertex input){
 float result=1;
 [branch]if(RainDepthSettings.x>.5){
  uint width=0,height=0;RainSceneDepth.GetDimensions(width,height);
  int2 pixel=int2(input.position.xy);float depthSpan=RainDepthSettings.w-RainDepthSettings.z;
  if(all(pixel>=0)&&all(pixel<int2(width,height))&&depthSpan>0&&RainDepthSettings.y>0){
   float sceneZ=(RainSceneDepth.Load(int3(pixel,0))-RainDepthSettings.z)/depthSpan;
   float particleZ=(input.position.z-RainDepthSettings.z)/depthSpan;
   if(sceneZ<.999999){
    if(particleZ>=sceneZ)result=0;
    else{
     float2 uv=(input.position.xy-RainDepthViewport.xy)/max(RainDepthViewport.zw,1);
     float3 sceneWorld=AtmosphereUnproject(uv,sceneZ),particleWorld=AtmosphereUnproject(uv,particleZ);
     if(all(isfinite(sceneWorld))&&all(isfinite(particleWorld)))result=saturate(distance(sceneWorld,particleWorld)/RainDepthSettings.y);
    }
   }
  }
 }
 return result;
}
uint RainHash(uint v){v^=v>>16;v*=0x7feb352d;v^=v>>15;v*=0x846ca68b;return v^(v>>16);}
RainVertex VSRain(uint vertex:SV_VertexID,uint instance:SV_InstanceID){
 float2 corner=float2(vertex==1||vertex==2||vertex==4?1:-1,vertex==2||vertex==4||vertex==5?1:-1);
 float3 random=float3(RainHash(instance*3),RainHash(instance*3+1),RainHash(instance*3+2))/4294967295.0;
 float2 anchor=floor(AtmosphereCamera.xy/128)*128;
 float fallPhase=frac(random.z-AtmosphereWind.z*.12);
 float3 p=float3(anchor+(random.xy-.5)*900,fallPhase*900);
 p.xy+=(1-fallPhase)*float2(AtmosphereWind.w,AtmosphereWind.w*.4)*8;
 float3 right=normalize(float3(AtmosphereViewProjection[0][0],AtmosphereViewProjection[1][0],AtmosphereViewProjection[2][0]));
 float4 centerClip=mul(float4(p,1),AtmosphereViewProjection);
 float width=max(.06,abs(centerClip.w)*.0007);
 p+=right*(corner.x*width)+float3(AtmosphereWind.w*.015,AtmosphereWind.w*.006,-1)*corner.y*4;
 RainVertex o;o.position=mul(float4(p,1),AtmosphereViewProjection);o.uv=corner;return o;
}
float4 PSRain(RainVertex input):SV_TARGET{
 float alpha=(1-abs(input.uv.x))*pow(saturate(1-abs(input.uv.y)),.5)*AtmosphereWeather.y*.22*RainSurfaceFade(input);
 return float4(.42,.50,.59,alpha);
}
// perf92: fog of war as a real, semi-transparent ground fog (war fog). The
// native shroud texture (projected in world XY exactly as the fixed-function
// texture matrix did) gives the cover: 0 visible, ~.5 explored, 1 unexplored.
// Ground out of sight lies under light drifting mist, unexplored ground under
// dense mist through which the terrain still shows faintly; the
// map border reads as unexplored. The layer follows the visible surface (scene
// depth), so terrain, structures and props are covered without per-object
// passes. Units and structures in unexplored cells stay hidden by game logic.
Texture2D<float> ShroudSceneDepth:register(t38);
Texture2D<float4> ShroudMap:register(t39);
// perf93: faded per-cell vision (0 visible, .5 out of sight, 1 unexplored) in the
// native texture layout; it replaces the levels because Zero Hour writes explored
// cells with ClearAlpha whether or not they are in sight.
Texture2D<float> ShroudVision:register(t40);
Texture2D<float> ShroudVisionTarget:register(t41);
cbuffer ShroudFogData:register(b12){float4 ShroudMapping,ShroudLevels,ShroudColor,ShroudViewport,ShroudSettings,ShroudVisionInfo;};
cbuffer ShroudFadeData:register(b6){float4 ShroudFadeRates;}; // clearing blend, fogging blend, snap
// Moves the vision toward the latest cell states: fog clears quickly where units
// now see and drifts back slowly where they left. Converged texels are exact.
float PSShroudFade(AtmosphereQuad input):SV_TARGET{
 int2 pixel=int2(input.position.xy);float target=ShroudVisionTarget.Load(int3(pixel,0));
 [branch]if(ShroudFadeRates.z>.5)return target;
 float previous=ShroudVision.Load(int3(pixel,0));
 float next=lerp(previous,target,target<previous?ShroudFadeRates.x:ShroudFadeRates.y);
 return abs(next-target)<.002?target:next;
}
// World units: a tank is ~18 long, large structures 40-80 high. The mist lies
// in banks ~300-700 across: thick banks stand ~60 high (scale height up to 58),
// thin ones hug the ground (~19); wisps reach ~150.
static const float ShroudFogHeight=34,ShroudFogTop=150,ShroudFogExtinction=.034,ShroudFogAlbedo=.95;
// Largest share of the scene a fogged surface keeps (perf94: thin gaps are
// allowed, the game hides units out of sight): out of sight 85%, unexplored 60%,
// and every bank stays semi-transparent (at least 10%).
static const float ShroudExploredFloor=.85,ShroudUnexploredFloor=.6,ShroudClearest=.1;
float3 ShroudNative(float2 world){return ShroudMap.SampleLevel(AtmosphereClamp,(world+ShroudMapping.zw)*ShroudMapping.xy,0).rgb;}
float ShroudLevelCover(float3 c){
 // Projection on the native shroud colour recovers the written level; a black
 // shroud colour leaves only clear cells non-zero.
 float level=ShroudColor.w>0?saturate(dot(c,ShroudColor.rgb)*ShroudColor.w):c.g;
 return saturate((ShroudLevels.x-level)/max(ShroudLevels.x-ShroudLevels.z,.001));
}
float ShroudCover(float2 world){
 float2 uv=(world+ShroudMapping.zw)*ShroudMapping.xy;float cover;
 [branch]if(ShroudVisionInfo.x>.5)cover=ShroudVision.SampleLevel(AtmosphereClamp,uv,0);
 else cover=ShroudLevelCover(ShroudMap.SampleLevel(AtmosphereClamp,uv,0).rgb);
 return cover;
}
// The mist shape reads the shroud one mip down (2x2 cells, the renderer's own
// mip chain): with the warp below the frontier loses the cell staircase.
// The gameplay floors always use the exact level-0 cover at the surface.
float ShroudShapeCover(float2 world){
 float2 uv=(world+ShroudMapping.zw)*ShroudMapping.xy;float cover;
 [branch]if(ShroudVisionInfo.x>.5)cover=ShroudVision.SampleLevel(AtmosphereClamp,uv,1);
 else cover=ShroudLevelCover(ShroudMap.SampleLevel(AtmosphereClamp,uv,1).rgb);
 return cover;
}
// Drift with the cloud wind and slow evolution; two periodic octaves at
// unrelated scales hide the 128^3 period. x base, y fine, z erosion.
float3 ShroudNoise(float2 xy,float height){
 float2 drift=AtmosphereWind.xy*.15;float time=AtmosphereWind.z;
 float base=AtmosphereNoise.SampleLevel(AtmosphereWrap,float3((xy-drift)/430,height/150+time*.004),0).r;
 float2 fine=AtmosphereNoise.SampleLevel(AtmosphereWrap,float3((xy-drift*1.7)/137+.37,height/55-time*.007),0);
 return float3(base,fine);
}
// Mist amount: 0 visible, .35 out of sight, .6 unexplored (before the bank field).
float ShroudAmount(float cover){
 float fogCover=ShroudLevels.w;
 return .35*saturate(cover/max(fogCover,.001))+.25*saturate((cover-fogCover)/max(1-fogCover,.001));
}
// Largest scene transmission the surface keeps (1 visible, then explored, unexplored).
float ShroudFloor(float cover){
 float fogCover=ShroudLevels.w;
 return cover<=fogCover?lerp(1,ShroudExploredFloor,saturate(cover/max(fogCover,.001))):lerp(ShroudExploredFloor,ShroudUnexploredFloor,saturate((cover-fogCover)/max(1-fogCover,.001)));
}
float4 PSShroudFog(AtmosphereQuad input):SV_TARGET{
 const bool flat=ShroudSettings.z>.5;const float4 untouched=flat?float4(1,1,1,1):float4(0,0,0,1);
 int2 pixel=int2(input.position.xy);float span=ShroudSettings.y-ShroudSettings.x;
 float z=(ShroudSceneDepth.Load(int3(pixel,0))-ShroudSettings.x)/max(span,1e-6);
 // Background beyond the map has no surface; the native passes never drew there.
 if(z>=.999999)return untouched;
 float2 uv=(input.position.xy-ShroudViewport.xy)/max(ShroudViewport.zw,1);
 float3 surface=AtmosphereUnproject(uv,z);
 if(any(!isfinite(surface)))return untouched;
 // Screen-space equivalent of the native multiplicative pass (no EQUAL depth test).
 if(flat)return float4(ShroudNative(surface.xy),1);
 float3 eye=AtmosphereCamera.xyz,delta=surface-eye;float distance=length(delta);float3 ray=delta/max(distance,.001);
 // Segment of the descending view ray inside [surface.z, surface.z+fog top].
 float top=surface.z+ShroudFogTop;
 float enter=ray.z<-1e-4?clamp((top-eye.z)/ray.z,0,distance):0;enter=max(enter,distance-900);
 float segment=distance-enter;float3 entry=eye+ray*enter;
 // Exact cover at the surface for the floors; the blurred shape cover (which
 // bounds the warped mist) decides whether the ray meets any fog.
 float coverSurface=ShroudCover(surface.xy),shapeSurface=ShroudShapeCover(surface.xy);
 float coverEntry=ShroudShapeCover(entry.xy),coverMiddle=ShroudShapeCover(lerp(entry.xy,surface.xy,.5));
 if(segment<=.01||max(max(coverSurface,shapeSurface),max(coverEntry,coverMiddle))<.003)return untouched;
 // perf95: lit exactly like the sky clouds (WeatherSkyRadiance): droplet albedo
 // .95, forward/back lobes and two broadening octaves for multiple scattering,
 // neutral-blue sky light; white sunlit cores, blue-grey shaded undersides.
 float cosine=dot(ray,AtmosphereSun.xyz);
 float phase0=.85*SkyHG(cosine,.65)+.15*SkyHG(cosine,-.2),phase1=SkyHG(cosine,.325),phase2=SkyHG(cosine,.1625);
 float daylight=max(AtmosphereSun.w/3.14159265,0);
 // Clouds are white: the map ambient enters by luminance, the sky adds blue.
 float ambientLuma=dot(AtmosphereAmbient.rgb,float3(.2126,.7152,.0722));
 float3 skyLight=lerp(float3(ambientLuma,ambientLuma,ambientLuma),AtmosphereAmbient.rgb,.35)*.8+float3(.12,.15,.19)*daylight;
 // RTS cloud shadows darken the fog exactly as they darken the ground below.
 float cloudShadow=CloudTransmission(float3(surface.xy,surface.z+30));
 float3 sunLight=AtmosphereSunColor.rgb*AtmosphereSun.w*cloudShadow;float sunZ=max(AtmosphereSun.z,.2);
 // Banks: a slow ~700-unit field decides where the mist is thick and tall and where
 // it thins to a ground veil. One lookup per pixel (rays cross ~150 units).
 float2 bankAt=lerp(entry.xy,surface.xy,.5)-AtmosphereWind.xy*.08;
 float bank=smoothstep(.32,.68,AtmosphereNoise.SampleLevel(AtmosphereWrap,float3(bankAt/700+.61,.23+AtmosphereWind.z*.0015),0).r);
 float bankDensity=lerp(.25,1.8,bank),bankHeight=ShroudFogHeight*lerp(.55,1.7,bank);
 // Samples crowd toward the ground where the mist is densest (quadratic spacing).
 const uint steps=8;float warpScale=1.4*ShroudSettings.w;
 float jitter=frac(52.9829189*frac(dot(input.position.xy,float2(.06711056,.00583715))));
 float3 scatter=0;float transmission=1;
 [loop]for(uint i=0;i<steps;++i){
  float s=(i+jitter)/steps;s*=s;float stepLength=segment*(2*i+1)/(steps*steps);
  float3 p=surface-ray*(segment*s);float height=max(p.z-surface.z,0);
  if(ShroudShapeCover(p.xy)<.003)continue;
  float3 n=ShroudNoise(p.xy,height);
  // Domain-warped cover gives an organic frontier inside one cell of the native edge.
  float amount=ShroudAmount(ShroudShapeCover(p.xy+(n.yz-.5)*warpScale));
  // Cloud shape (as CloudDensityFiltered): rounded billows with defined edges,
  // the Worley detail erodes the silhouette rather than painting noise inside.
  float billow=n.x*.75+n.y*.25,shape=saturate((billow-.36-.12*saturate(height/bankHeight))/.19);
  float wisp=saturate(shape-(1-n.z)*lerp(.23,.055,saturate(shape*3)));
  float column=amount*bankDensity*exp(-height/bankHeight);
  float density=column*wisp*1.3;
  float extinction=density*ShroudFogExtinction;if(extinction<1e-6)continue;
  // Light through the mist above the sample (self-shadowed bank interiors),
  // with a multiple-scattering floor; deep and dense mist sees less sky.
  float optical=ShroudFogExtinction*column*bankHeight*1.3/sunZ;
  float sunT=max(exp(-optical),.3*exp(-optical*.3));
  float access=lerp(.5,1,saturate(height/bankHeight))*lerp(1,.75,bank);
  float illumination=sunT*phase0+.5*sqrt(sunT)*phase1+.25*pow(sunT,.25)*phase2;
  float3 source=ShroudFogAlbedo*(sunLight*illumination+skyLight*access);
  float segmentT=exp(-extinction*stepLength);
  // Marching from the surface toward the eye: nearer mist attenuates what lies behind it.
  scatter=scatter*segmentT+(1-segmentT)*source;transmission*=segmentT;
 }
 // Semi-transparent war fog: even the densest bank keeps 12% of the ground.
 float clearT=max(transmission,ShroudClearest);scatter*=(1-clearT)/max(1-transmission,1e-4);transmission=clearT;
 // Gameplay floor, independent of view angle and of thin gaps in the mist:
 // ground out of sight keeps at most 85%, unexplored ground at most 60%; the rest
 // is mist of the local colour.
 float floorT=min(transmission,ShroudFloor(coverSurface));
 float3 fill=ShroudFogAlbedo*(sunLight*(phase0+.5*phase1+.25*phase2)*.6+skyLight*.8);
 scatter+=(transmission-floorT)*fill;
 return float4(min(scatter,65504),floorT);
}
Texture2D<float4> AtmosphereSkyImage:register(t25);
Texture2D<float4> CloudPreviousImage:register(t34);
float4 PSCloudTemporal(AtmosphereQuad input):SV_TARGET{
 int2 pixel=int2(input.position.xy);float4 current=AtmosphereSkyImage.Load(int3(pixel,0));
 [branch]if(CloudHistorySettings.x<.5)return current;
 float3 ray=normalize(AtmosphereUnproject(input.uv,1)-AtmosphereCamera.xyz);
 float3 world=AtmosphereCamera.xyz+ray*current.a;
 float layer=saturate((world.z-AtmosphereRange.y)/max(AtmosphereRange.z-AtmosphereRange.y,1));
 world.xy+=CloudDisplacement.xy+CloudDisplacement.z*float2(.7,.25)*layer*12;
 float4 previousClip=mul(float4(world,1),CloudPreviousVP);float2 uv=previousClip.xy/max(previousClip.w,.001)*float2(.5,-.5)+.5;
 [branch]if(previousClip.w<=0||any(uv<=0)||any(uv>=1))return current;
 float4 previous=CloudPreviousImage.SampleLevel(AtmosphereClamp,uv,0);
 float expected=length(world-CloudPreviousCamera.xyz);
 [branch]if(abs(previous.a-expected)>max(150,expected*.15))return current;
 float3 low=current.rgb,high=current.rgb;
 [unroll]for(int y=-1;y<=1;++y)[unroll]for(int x=-1;x<=1;++x){
  int2 neighbor=clamp(pixel+int2(x,y),int2(0,0),int2(CloudPixels.xy)-1);
  float3 value=AtmosphereSkyImage.Load(int3(neighbor,0)).rgb;low=min(low,value);high=max(high,value);
 }
 float3 filtered=clamp(previous.rgb,low,high);
 return float4(lerp(current.rgb,filtered,CloudHistorySettings.y),current.a);
}
float4 PSCloudResolve(AtmosphereQuad input):SV_TARGET{return float4(AtmosphereSkyImage.SampleLevel(AtmosphereClamp,input.uv,0).rgb,1);}
