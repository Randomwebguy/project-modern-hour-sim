// SPDX-License-Identifier: GPL-3.0-or-later
// Shared linear HDR sky for the visible clouds and distant environment probes.
// No punctual solar disc: direct GGX already receives native sun radiance.
float SkyHG(float cosine,float g){return (1-g*g)/(12.5663706*pow(max(1+g*g-2*g*cosine,.01),1.5));}
float3 WeatherBackgroundRadiance(float3 ray){
 float daylight=max(AtmosphereSun.w/3.14159265,0);
 float horizon=saturate(ray.z*.5+.5);
 // Absolute native illumination, not a unit environment multiplied by ambient
 // again in the material. Retain moon/night/map ambient when direct sun is zero.
 float3 sky=AtmosphereAmbient.rgb*lerp(float3(.85,.85,.85),float3(.75,1,1.25),horizon);
 sky+=lerp(float3(.25,.30,.35),float3(.095,.21,.43),horizon)*daylight;
 sky*=lerp(1,.6,AtmosphereWeather.x);return sky;
}
float3 WeatherSkyRadiance(float3 origin,float3 ray,uint steps,float angularFootprint,float jitter,out float cloudDistance,bool transient){
 cloudDistance=18000;float3 sky=WeatherBackgroundRadiance(ray);
 float daylight=max(AtmosphereSun.w/3.14159265,0);
 float start=0,end=18000;
 if(abs(ray.z)<.001){if(origin.z<AtmosphereRange.y||origin.z>AtmosphereRange.z)return sky;}
 else{
  float t0=(AtmosphereRange.y-origin.z)/ray.z,t1=(AtmosphereRange.z-origin.z)/ray.z;
  start=max(0,min(t0,t1));end=min(18000,max(t0,t1));
 }
 if(end<=start)return sky;
 end=min(end,start+6000);
 float3 scatter=0;float transmission=1;float weightedDistance=0,totalOpacity=0;
 float cosine=dot(ray,AtmosphereSun.xyz);
 // Two broadening octaves approximate multiple scattering, without additional
 // density marches. This is an energy-bounded approximation, not full transport.
 float phase0=.85*SkyHG(cosine,.65)+.15*SkyHG(cosine,-.2);
 float phase1=SkyHG(cosine,.325),phase2=SkyHG(cosine,.1625);
 [loop]for(uint i=0;i<steps;++i){
  float u0=float(i)/steps,u1=float(i+1)/steps;
  float d0=start+(end-start)*(.25*u0+.75*u0*u0),d1=start+(end-start)*(.25*u1+.75*u1*u1);
  float stepLength=d1-d0;float distance=lerp(d0,d1,jitter);
  float3 position=origin+ray*distance;
  // A ray segment also integrates a finite interval. Filter its sub-voxel
  // erosion instead of letting evenly spaced samples form horizon bands.
  float footprint=max(distance*angularFootprint,1);
  float density=CloudDensityFiltered(position,footprint);
  if(density>.001){
   float optical=0;
   float lightRange=min(4000,(AtmosphereRange.z-AtmosphereRange.y)/max(AtmosphereSun.z,.2));
   [unroll]for(uint j=0;j<6;++j){
    float a=float(j)/6,b=float(j+1)/6;float l0=lightRange*a*a,l1=lightRange*b*b;
    float lightDistance=(l0+l1)*.5,lightStep=l1-l0;
    optical+=CloudDensityFiltered(position+AtmosphereSun.xyz*lightDistance,max(footprint,lightStep*.05))*AtmosphereRange.w*lightStep;
   }
   float illumination=exp(-optical)*phase0+.5*exp(-optical*.5)*phase1+.25*exp(-optical*.25)*phase2;
   float segment=exp(-density*AtmosphereRange.w*stepLength);
   float cloudHeight=saturate((position.z-AtmosphereRange.y)/max(AtmosphereRange.z-AtmosphereRange.y,1));
   float skyAccess=lerp(.15,1,cloudHeight);
   float3 source=AtmosphereSunColor.rgb*AtmosphereSun.w*illumination+(AtmosphereAmbient.rgb*.8+float3(.12,.15,.19)*daylight)*skyAccess;
   // Local cloud emission, never a global exposure or solar-color adjustment.
   // The slowly refreshed distant probe excludes this transient source.
   [branch]if(transient&&AtmosphereFlashRadiance.w>0)source+=StormCloudEmission(position,footprint);
   float opacity=transmission*(1-segment);weightedDistance+=opacity*distance;totalOpacity+=opacity;
   scatter+=opacity*.95*source;transmission*=segment;
   if(transmission<.015)break;
  }
 }
 // Distant slab noise must fade into atmospheric sky, never a hard repeating
 // band at the horizon. The same bounded aerial fade is used by every probe.
 if(totalOpacity>.001)cloudDistance=weightedDistance/totalOpacity;
 float fadeDistance=totalOpacity>.001?cloudDistance:start;
 float aerial=exp(-AtmosphereWeather.w*fadeDistance)*(1-smoothstep(8000,18000,fadeDistance));
 return lerp(sky,sky*transmission+scatter,aerial);
}
float3 WeatherCubeDirection(uint face,float2 uv){
 float u=uv.x,v=uv.y;
 if(face==0)return normalize(float3(1,-v,-u));
 if(face==1)return normalize(float3(-1,-v,u));
 if(face==2)return normalize(float3(u,1,v));
 if(face==3)return normalize(float3(u,-1,-v));
 if(face==4)return normalize(float3(u,-v,1));
 return normalize(float3(-u,-v,-1));
}
