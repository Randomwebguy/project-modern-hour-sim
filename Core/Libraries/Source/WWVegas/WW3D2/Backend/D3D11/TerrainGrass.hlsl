// SPDX-License-Identifier: GPL-3.0-or-later
// terrain97: GPU-driven grass. A compute pass places blades on a fixed world
// grid over the visible ground (grass class weight, road/building exclusion,
// slope and distance density), culls them and appends the survivors; one
// indirect draw expands each into a curved blade. Technique adapted from
// EricHu33/UnityGrassIndirectRenderingExample (MIT, Copyright (c) 2024 EricHu):
// GPU detail fetching with an append buffer, Bezier blade with tip-weighted wind
// sway, distance width scale-up against sub-pixel needles, surface-normal
// ("fixed") shading, ground-tinted roots, fake AO and translucency.
cbuffer GrassData:register(b9){
 float4 GrassGrid;    // xy first grid cell, z cell size (world units), w maximum distance
 float4 GrassCounts;  // xy grid cells dispatched, z seconds, w instance capacity
 float4 GrassWind;    // xy wind direction, z sway (world units), w gust frequency
 float4 GrassShape;   // x blade height, y blade width, z full-density distance, w pixels per blade width
};
Texture2D<float> TerrainElevation:register(t51);  // native vertex heights (cellsX+1 x cellsY+1)
Texture2D<uint> TerrainTriangleFlip:register(t52); // per cell: 1 = diagonal 10-01
Texture2D GrassGroundAtlas:register(t54);           // the map's modern base atlas (root colour)
// perf116: tuft[t] holds GrassTuftData for tuft t (x gust, yz push, w bend), so GrassVS reads them instead of recomputing per vertex.
struct GrassBlade {float3 root;uint color;float facing;float height;float width;uint normal;float4 tuft[3];};
// terrain98: vehicle interaction (reference: interact motion buffer). A toroidal
// world map (InteractWindow.y texels of InteractWindow.x units) around the view:
// xy push direction, z flattening strength, w the texel's world cell key. Tracks
// recover over a few seconds; vehicles on the ground flatten, low rotors wash.
cbuffer GrassInteractionData:register(b10){
 float4 InteractWindow;   // x cell size, y texels per side, z decay factor this frame, w vehicles
 float4 InteractCenter;   // xy window centre (world)
 float4 Vehicles[64];     // xyz position, w radius
};
Texture2D<float4> GrassInteraction:register(t55);
RWTexture2D<float4> GrassInteractionOut:register(u1);
int2 InteractBase(){return int2(floor(InteractCenter.xy/InteractWindow.x))-int2(InteractWindow.y*.5,InteractWindow.y*.5);}
// Positive toroidal wrap (cells stay within +-8192 windows of the origin).
int2 InteractWrap(int2 cell){int size=int(InteractWindow.y);return int2(uint2(cell+size*8192)%uint(size));}
uint InteractKey(int2 cell){return (uint(cell.x)&65535)|(uint(cell.y)<<16);}
// Strength and push direction at a world position (zero outside the window).
float3 GrassInteractionAt(float2 world){
 int2 cell=int2(floor(world/InteractWindow.x)),base=InteractBase(),local=cell-base;int size=int(InteractWindow.y);
 float3 result=0;
 if(InteractWindow.y>0&&all(local>=0)&&all(local<size)){
  float4 texel=GrassInteraction.Load(int3(InteractWrap(cell),0));
  if(asuint(texel.w)==InteractKey(cell))result=float3(texel.xy,texel.z);
 }
 return result;
}
float GrassHash12(float2 p){float3 p3=frac(float3(p.xyx)*.1031);p3+=dot(p3,p3.yzx+33.33);return frac((p3.x+p3.y)*p3.z);}
float2 GrassHash22(float2 p){float3 p3=frac(float3(p.xyx)*float3(.1031,.1030,.0973));p3+=dot(p3,p3.yzx+33.33);return frac((p3.xx+p3.yz)*p3.zy);}
float GrassGradientNoise(float2 p){
 float2 i=floor(p),f=frac(p),u=f*f*f*(f*(f*6-15)+10);
 float2 g00=GrassHash22(i)*2-1,g10=GrassHash22(i+float2(1,0))*2-1,g01=GrassHash22(i+float2(0,1))*2-1,g11=GrassHash22(i+float2(1,1))*2-1;
 return lerp(lerp(dot(g00,f),dot(g10,f-float2(1,0)),u.x),lerp(dot(g01,f-float2(0,1)),dot(g11,f-float2(1,1)),u.x),u.y);
}
// perf116: per-tuft wind gust and vehicle interaction (x gust, yz push, w bend), evaluated once per placed blade in GrassCS
// with the arithmetic GrassVS used to repeat on each vertex of the strip. Tuft roots follow GrassVS (tufts 1 and 2 are offset).
float4 GrassTuftData(float3 root,float facing,float width,uint tuft){
 float tuftRandom=frac(sin(facing*12.9898+tuft*78.233)*43758.5453);
 float2 rootXY=root.xy;
 if(tuft>0){float a=facing*3.1+tuft*2.0944;rootXY+=float2(cos(a),sin(a))*width*lerp(.9,1.8,tuftRandom);}
 float gust=GrassGradientNoise(rootXY*.045+GrassWind.xy*GrassCounts.z*GrassWind.w)*1.6+GrassGradientNoise(rootXY*.21+GrassCounts.z*1.7)*.35;
 float3 interaction=GrassInteractionAt(rootXY);
 return float4(gust,interaction.xy,interaction.z);
}
// The native terrain surface: each 10-unit cell is two triangles; Zero Hour
// rotates the cell's corners when the blend needs it (HeightMap.cpp FLIP_TRIANGLES).
float3 NativeTerrainPoint(float2 world,out float3 normal){
 float2 grid=(world+TerrainDimensions.z)/max(TerrainDimensions.w,.001);
 int2 limit=max(int2(TerrainDimensions.xy)-1,0),cell=clamp(int2(floor(grid)),0,limit);float2 f=saturate(grid-cell);
 float h00=TerrainElevation.Load(int3(cell,0)),h10=TerrainElevation.Load(int3(cell+int2(1,0),0));
 float h01=TerrainElevation.Load(int3(cell+int2(0,1),0)),h11=TerrainElevation.Load(int3(cell+int2(1,1),0));
 float z;float2 slope;
 if(TerrainTriangleFlip.Load(int3(cell,0))==0){
  if(f.x>=f.y){slope=float2(h10-h00,h11-h10);}else{slope=float2(h11-h01,h01-h00);}
  z=h00+slope.x*f.x+slope.y*f.y;
 }else{
  if(f.x+f.y<=1){slope=float2(h10-h00,h01-h00);z=h00+slope.x*f.x+slope.y*f.y;}
  else{slope=float2(h11-h01,h11-h10);z=h11-slope.x*(1-f.x)-slope.y*(1-f.y);}
 }
 normal=Unit(float3(-slope/max(TerrainDimensions.w,.001),1));
 return float3(world,z);
}
// The region's mean native colour (class map GBA, written by the terrain worker):
// exact per texture region, so no atlas mip bleeds a neighbour's colour into
// roots near cell edges. A cache without it falls back to a fine atlas mip.
float3 GrassRegionColor(float2 uv){
 uint w,h;TerrainIdentity.GetDimensions(w,h);
 float3 region=TerrainIdentity.Load(int3(min((uint2)(frac(uv)*float2(w,h)),uint2(w-1,h-1)),0)).gba;
 return dot(region,1)>1e-4?DecodeSRGB(region):DecodeSRGB(GrassGroundAtlas.SampleLevel(TerrainSampler,uv,0).rgb);
}
// terrain99: tuft density from coverage and clumps; GrassCS and the analytic
// ground shadow use this one function so the shadows match the placed tufts.
float GrassClumpDensity(float coverage,float clump){return coverage*lerp(.6,1,smoothstep(.25,.75,clump));}
float GrassCoverage(float2 world,out float2 baseUV,out float2 blendUV,out float blend){
 WorldSurface(world,baseUV,blendUV,blend);
 return (GroundAt(baseUV)==1?1-blend:0)+(GroundAt(blendUV)==1?blend:0);
}
// terrain101: with the continuous ground field (65536) the meadow follows the very
// border the ground shading draws (same field, same height sharpening); footprint
// is the sampled world size for the layer height mip. Root colour: the field's
// region colours (the native cells' when a cache has none).
static float TerrainPixelCoverage=-1; // >=0: this pixel's field coverage (set by the ground shading)
float GrassFieldCoverage(float2 world,float footprint,out float3 rootColor){
 float2 baseUV,blendUV;float blend;bool field=false;float coverage=0;float3 region=0;
 [branch]if(((uint)ModernMaterial.w&65536)!=0){
  TerrainFieldSample s=TerrainFieldAt(world);
  [branch]if(TerrainFieldLibrary(s)){
   float t=TerrainFieldTransition(s,world,float2(footprint,0),float2(0,footprint));
   region=lerp(s.firstRegion,s.secondRegion,t);field=true;
   coverage=(s.first==1?1-t:0)+(s.second==1?t:0);
  }
 }
 float cells=GrassCoverage(world,baseUV,blendUV,blend);
 rootColor=field&&dot(region,1)>1e-4?DecodeSRGB(region):lerp(GrassRegionColor(baseUV),GrassRegionColor(blendUV),blend);
 return field?coverage:cells;
}
[numthreads(8,8,1)]
void GrassInteractCS(uint3 id:SV_DispatchThreadID){
 int size=int(InteractWindow.y);if(any(int2(id.xy)>=size))return;
 int2 base=InteractBase(),texel=int2(id.xy);
 int2 cell=base+InteractWrap(texel-base);
 float4 value=GrassInteractionOut[texel];
 if(asuint(value.w)!=InteractKey(cell))value=float4(0,0,0,asfloat(InteractKey(cell)));
 value.z*=InteractWindow.z;
 float2 world=(cell+.5)*InteractWindow.x;
 [loop]for(uint i=0;i<(uint)InteractWindow.w;++i){
  float4 vehicle=Vehicles[i];float2 offset=world-vehicle.xy;float distance=length(offset);
  float3 normal;float ground=NativeTerrainPoint(vehicle.xy,normal).z,altitude=vehicle.z-ground;
  float strength=0;
  if(altitude<5)strength=smoothstep(vehicle.w,vehicle.w*.55,distance);
  else if(altitude<45)strength=.65*(1-altitude/45)*smoothstep(16,4,distance);
  if(strength>value.z){value.z=strength;value.xy=distance>1e-3?offset/distance:float2(0,0);}
 }
 GrassInteractionOut[texel]=value;
}
// perf120: tall and short grass. Tallness is a clumped field (tall and short patches about 9 units across) times a per-site
// spread, so tufts range from about .3x to 2.3x the base height (mean about 1.1x). GrassCS places the blades and
// GrassGroundShadow shades them with this same function, so the shadows follow the tufts.
float GrassTallness(float2 rootXY,float2 site){
 return lerp(.45,1.75,smoothstep(.15,.85,TerrainNoise(rootXY/9+float2(11.3,4.7))))*lerp(.7,1.3,GrassHash12(site+5.1));
}
AppendStructuredBuffer<GrassBlade> GrassOutput:register(u0);
[numthreads(8,8,1)]
void GrassCS(uint3 id:SV_DispatchThreadID){
 if(any(id.xy>=(uint2)GrassCounts.xy))return;
 float2 site=GrassGrid.xy+float2(id.xy);
 float2 world=(site+GrassHash22(site))*GrassGrid.z;
 float2 grid=(world+TerrainDimensions.z)/max(TerrainDimensions.w,.001);
 if(any(grid<0)||any(grid>=TerrainDimensions.xy))return;
 float3 normal;float3 root=NativeTerrainPoint(world,normal);
 // Distance density keeps the projected blade count per pixel roughly constant;
 // a site's own threshold makes it grow in/out smoothly, never pop.
 float distance=length(root-CameraWorld.xyz);
 float keep=pow(saturate(GrassShape.z/max(distance,1)),1.6)*saturate((GrassGrid.w-distance)/(GrassGrid.w*.15));
 float threshold=GrassHash12(site+17.31);
 if(threshold>=keep)return;
 float3 ground;
 float coverage=GrassFieldCoverage(world,GrassGrid.z,ground);
 // Clumps: dense tufts and thin gaps instead of an even carpet.
 float clump=TerrainNoise(world/7.3)*.6+TerrainNoise(world/2.9+float2(5.2,1.7))*.4;
 float density=GrassClumpDensity(coverage,clump);
 float presence=GrassHash12(site*1.37+3.9);
 if(presence>=density||CoveredGround(world)>.1)return;
 if(normal.z<.6)return; // cliffs carry no blades
 root.z+=TerrainSurfaceOffset(root);
 float growth=saturate((keep-threshold)/max(keep*.25,1e-3))*saturate((density-presence)/.15);
 float pixel=2/max(abs(Projection[1][1])*Viewport.w,1e-6)*(Projection[3][3]>.5?1:length(root-CameraWorld.xyz));
 float tallness=GrassTallness(world,site);float height=GrassShape.x*tallness*lerp(.8,1.15,clump)*growth;
 // Fewer, broader blades with distance (coverage kept), never thinner than a pixel.
 float width=max(GrassShape.y*lerp(.8,1.2,GrassHash12(site+9.7)),pixel*GrassShape.w)*clamp(rsqrt(max(keep,1e-3)),1,3)*lerp(.9,1.25,saturate(tallness/1.2));
 // Frustum: the blade's bounding sphere against the clip volume.
 float4 clip=mul(mul(float4(root+float3(0,0,height*.5),1),View),Projection);
 float margin=(height+width)*.75*max(abs(Projection[0][0]),abs(Projection[1][1]));
 if(clip.w<=0||any(abs(clip.xy)>clip.w+margin))return;
 // Root colour: the map's ground colour of the root's regions, linear.
 uint3 color=(uint3)round(saturate(sqrt(ground))*255);
 GrassBlade blade;blade.root=root;blade.color=color.r|(color.g<<8)|(color.b<<16)|((uint)round(clump*255)<<24);
 blade.facing=GrassHash12(site+31.7)*6.2831853;blade.height=height;blade.width=width;
 int2 packed=(int2)round(clamp(normal.xy,-1,1)*32767);blade.normal=(uint(packed.x)&65535)|(uint(packed.y)<<16);
 blade.tuft[0]=GrassTuftData(blade.root,blade.facing,blade.width,0);blade.tuft[1]=GrassTuftData(blade.root,blade.facing,blade.width,1);blade.tuft[2]=GrassTuftData(blade.root,blade.facing,blade.width,2);
 GrassOutput.Append(blade);
}
StructuredBuffer<GrassBlade> GrassBlades:register(t53);
float3 GrassBezier(float3 p0,float3 p1,float3 p2,float3 p3,float t){float s=1-t;return s*s*s*p0+3*s*s*t*p1+3*s*t*t*p2+t*t*t*p3;}
// GrassGradientNoise and GrassTuftData are defined with the hashes above (GrassCS evaluates them once per blade).
// Each instance is a tuft of three blades (as the reference clump mesh), one
// triangle strip: per blade 7 vertices (left/right pairs, tip) plus two
// degenerate joins, 25 vertices in all.
LegacyOutput GrassVS(uint stripVertex:SV_VertexID,uint instance:SV_InstanceID){
 uint tuft=min(stripVertex/9,2),vertex=stripVertex%9;
 if(vertex==8){tuft=min(tuft+1,2);vertex=0;}else if(vertex==7)vertex=6;
 LegacyOutput o=(LegacyOutput)0;
 GrassBlade blade=GrassBlades[min(instance,(uint)GrassCounts.w-1)];
 if(instance>=(uint)GrassCounts.w)blade.height=blade.width=0;
 float2 packed=float2(int(blade.normal<<16)>>16,int(blade.normal)>>16)/32767;
 float3 up=Unit(float3(packed,sqrt(saturate(1-dot(packed,packed)))));
 float tuftRandom=frac(sin(blade.facing*12.9898+tuft*78.233)*43758.5453);
 if(tuft>0){float a=blade.facing*3.1+tuft*2.0944;blade.root.xy+=float2(cos(a),sin(a))*blade.width*lerp(.9,1.8,tuftRandom);}
 blade.facing+=tuft*2.0944+tuftRandom;blade.height*=tuft==0?1:lerp(.7,1,tuftRandom);
 float s,c;sincos(blade.facing,s,c);
 float3 side=Unit(float3(c,s,0)-up*dot(float3(c,s,0),up)),lean=cross(up,side);
 float random=frac(blade.facing*.1591549*7.31),clump=((blade.color>>24)&255)/255.0;
 // Wind: slow gusts over the field, tip-weighted (the root never moves).
 float4 tuftData=blade.tuft[tuft];float gust=tuftData.x;
 float3 sway=float3(GrassWind.xy,0)*GrassWind.z*(gust+.35)*blade.height;
 // terrain98: vehicles and rotor wash lay the blades down along the push.
 float3 interaction=tuftData.yzw;float bend=interaction.z;float3 push=float3(interaction.xy,0);
 sway*=1-bend;
 float3 p0=blade.root-up*.04,p1=p0+up*blade.height*.45*(1-.7*bend)+push*blade.height*.25*bend;
 float3 p3=p0+up*blade.height*lerp(.92-.12*random,.12,bend)+lean*blade.height*(.18+.25*random)*(1-bend)+push*blade.height*.9*bend+sway;
 float3 p2=lerp(p1,p3,.5)+up*blade.height*.12*(1-bend);
 uint level=min(vertex/2,3);float t=level/3.0;
 float3 position=GrassBezier(p0,p1,p2,p3,t);
 float across=vertex>=6?0:blade.width*.5*(1-t*.85)*((vertex&1)?1:-1);
 position+=side*across;
 o.worldPosition=position;
 // Fixed shading (reference technique): lit with the ground's normal, slightly
 // bent along the blade, so blades never read as dark spikes.
 o.worldNormal=Unit(up+lean*.25+side*sign(across)*.15);
 float4 camera=mul(float4(position,1),View);
 o.position=mul(camera,Projection);
 o.diffuse=float4(1,1,1,1);o.specular=0;
 float3 root=float3(blade.color&255,(blade.color>>8)&255,(blade.color>>16)&255)/255.0;
 // uv3.w = 1 + how far the blade is laid down (vehicle tracks shade differently).
 o.uv2=float4(root*root,1);o.uv3=float4(random,t,clump,1+bend);
 o.fog=FogSettings.w>.5?length(camera.xyz):abs(camera.z);
 o.blade=1+t;
 return o;
}
// terrain98: analytic grass shadows on the ground. A blade is thinner than a
// sun-cascade texel at RTS distance (it vanished under the cascade bias), so the
// ground evaluates the very tufts GrassCS places (same world grid and hashes)
// and their sun shadows exactly; when a pixel spans several tufts it converges
// to the meadow's mean shade. Flattened grass (vehicle tracks) casts less.
float GrassGroundShadow(float3 world,float3 towardSun,float footprint){
 float cell=GrassGrid.z,result=1;
 [branch]if(cell>0&&towardSun.z>.05){
  float3 rootColor;float coverage=TerrainPixelCoverage>=0?TerrainPixelCoverage:GrassFieldCoverage(world.xy,max(footprint,.05),rootColor);
  [branch]if(coverage>0){
   float clump=TerrainNoise(world.xy/7.3)*.6+TerrainNoise(world.xy/2.9+float2(5.2,1.7))*.4;
   float density=GrassClumpDensity(coverage,clump);
   float height=GrassShape.x*lerp(.8,1.15,clump)*(1-.85*GrassInteractionAt(world.xy).z);
   // A tuft at r shades r-drift*h for heights h up to its own: the roots that
   // can reach this point lie between it and drift*height toward the sun.
   float2 drift=towardSun.xy/max(towardSun.z,.25);
   // terrain99: the exact per-tuft loop only where pixels resolve tufts (the mean
   // takes over above ~.38 units per pixel); 3x3 sites unless the shadow is long.
   // perf120: tall tufts shade up to 2.3 x height, farther than the 4x4 window holds,
   // so pixels under such long shadows take the mean as well.
   float detail=saturate((footprint-.08)/.3),exact=1;
   float reach=height*2.3,reachCells=length(drift)*reach/cell;
   if(reachCells>4)detail=1;
   uint span=reachCells>1.5?4u:3u;int2 base=int2(floor((world.xy+drift*reach*.5)/cell))-1;
   [branch]if(detail<1)[loop]for(uint k=0;k<span*span;++k){
    float2 site=float2(base+int2(k%span,k/span));
    if(GrassHash12(site*1.37+3.9)>=density)continue;
    float2 root=(site+GrassHash22(site))*cell;float2 axis=-drift*height*GrassTallness(root,site);
    float along=saturate(dot(world.xy-root,axis)/max(dot(axis,axis),1e-6));
    float width=lerp(.24,.05,along),offset=length(world.xy-(root+axis*along));
    exact*=1-.45*(1-smoothstep(width*.5,width,offset));
   }
   // Mean shade of the canopy: shadow area of a tuft over a site cell.
   // perf120: the tufts average 1.1 x height (GrassTallness).
   float mean=1-.45*saturate(density*.14*height*1.1*length(drift)/(cell*cell));
   result=lerp(exact,mean,detail);
  }
 }
 return result;
}
