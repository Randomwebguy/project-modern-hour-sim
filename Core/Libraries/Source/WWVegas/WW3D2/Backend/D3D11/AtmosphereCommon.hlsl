// SPDX-License-Identifier: GPL-3.0-or-later
// Z-up native world. All scattering/radiance and wet material response are linear.
cbuffer AtmosphereData:register(b7){
 row_major float4x4 AtmosphereInverseVP;
 row_major float4x4 AtmosphereViewProjection;
 float4 AtmosphereCamera; // xyz eye, w enabled
 float4 AtmosphereSun; // xyz toward native sun, w native directional luminance
 float4 AtmosphereSunColor;
 float4 AtmosphereWeather; // coverage, rain, wetness, extinction
 float4 AtmosphereWind; // continuous XY displacement, time, reserved
 float4 AtmosphereRange; // max ray distance, cloud base/top, cloud extinction
 float4 AtmosphereShadowArea; // center xy, world width, reserved
 float4 AtmosphereViewport; // native view depth axis xyz, translation
 float4 AtmosphereAmbient; // native map ambient, linear RGB radiance reference
 float4 AtmosphereFlashPosition; // anchored intracloud emitter xyz, radius
 float4 AtmosphereFlashRadiance; // transient linear RGB, envelope
};
float3 StormRadiance(float3 world){
 float3 delta=world-AtmosphereFlashPosition.xyz;float radiusSquared=AtmosphereFlashPosition.w*AtmosphereFlashPosition.w;
 return AtmosphereFlashRadiance.rgb*(radiusSquared/max(radiusSquared+dot(delta,delta),1))*step(.5,AtmosphereCamera.w)*step(1e-8,AtmosphereFlashRadiance.w);
}
Texture3D<float2> AtmosphereNoise:register(t22);
Texture3D<float4> AtmosphereVolume:register(t23);
Texture2D<float> AtmosphereCloudShadow:register(t24);
TextureCube<float4> WeatherReflection:register(t29);
TextureCube<float4> WeatherDiffuse:register(t30);
TextureCube<float4> PreviousWeatherReflection:register(t31);
TextureCube<float4> PreviousWeatherDiffuse:register(t32);
SamplerState AtmosphereWrap:register(s11);
SamplerState AtmosphereClamp:register(s12);
float3 AtmosphereUnproject(float2 uv,float z){
 float4 p=mul(float4(uv*float2(2,-2)+float2(-1,1),z,1),AtmosphereInverseVP);
 return p.xyz/max(abs(p.w),1e-6)*sign(p.w);
}
float CloudDensityFiltered(float3 world,float footprint){
 float layer=saturate((world.z-AtmosphereRange.y)/max(AtmosphereRange.z-AtmosphereRange.y,1));
 // Flat lower interface and rounded rising towers, becoming a broader storm
 // layer smoothly. Periodic gradient/Worley base supplies connected billows;
 // smaller Worley cells erode the silhouette rather than painting it with noise.
 float2 shear=AtmosphereWind.w*float2(.7,.25)*layer*12;
 float3 uv=(world-float3(AtmosphereWind.xy+shear,0))/1800;
 float lod=max(log2(max(footprint*128/1800,1)),0);
 float base=AtmosphereNoise.SampleLevel(AtmosphereWrap,uv,lod).r;
 // 1.9 preserves a full 19-period wrap for the controller's 18 km displacement.
 float2 smallBillows=AtmosphereNoise.SampleLevel(AtmosphereWrap,uv*3.7,max(log2(max(footprint*3.7*128/1800,1)),0));
 base=lerp(base,smallBillows.r,.18);float detail=smallBillows.g;
 float storm=smoothstep(.45,.85,AtmosphereWeather.x);
 // A slowly varying weather field controls formations rather than distributing
 // equally sized billows across an infinite sheet. Reuse the immutable volume;
 // 0.2 is exactly periodic over the existing 18 km displacement wrap.
 float macro=AtmosphereNoise.SampleLevel(AtmosphereWrap,float3(uv.xy*.2,.37),max(log2(max(footprint*128/9000,1)),0)).r;
 float weatherMask=smoothstep(.68-.2*AtmosphereWeather.x,.88-.2*AtmosphereWeather.x,macro);
 float formationTop=lerp(.4,1,saturate((macro-.52)/.2));
 // The already-sampled 3D billow field shapes rounded caps. High-density cores
 // can rise, sparse edges taper; no additional noise lookup or texture needed.
 // Column height is determined by weather, not by the same 3D billow value
 // that is later thresholded. This avoids correlated upright curtain walls.
 float cap=formationTop;
 float profile=smoothstep(0,lerp(.1,.06,storm),layer)*(1-smoothstep(cap*.65,cap,layer));
 float coverage=saturate(AtmosphereWeather.x);
 float shape=saturate((base-lerp(.73,.43,weatherMask)-(1-profile)*.12)/.19)*profile;
 // Erosion fades in the interior; retain solid billows and a broken thin edge.
 float erosion=(1-detail)*lerp(.23,.055,saturate(shape*3));
 float densityScale=lerp(2.2,1.8,smoothstep(.45,.85,coverage));
 return saturate((shape-erosion)*densityScale)*smoothstep(0,.08,coverage);
}
float CloudDensity(float3 world){return CloudDensityFiltered(world,0);}
float3 StormCloudEmission(float3 world,float footprint){
 // A light inside opaque clouds must illuminate the local volumes from within,
 // not paint a uniform bright sheet onto the visible lower interface. Three
 // bounded noise samples approximate emitter-to-sample optical thickness.
 float3 delta=AtmosphereFlashPosition.xyz-world;float distance=length(delta);float optical=0;
 [unroll]for(uint sample=0;sample<3;++sample)optical+=CloudDensityFiltered(world+delta*((sample+.5)/3),max(footprint,distance/12));
 float transmission=exp(-min(optical*AtmosphereRange.w*distance/3,16));
 return StormRadiance(world)*transmission*.07957747;
}
float CloudTransmission(float3 world){
 if(AtmosphereCamera.w<.5)return 1;
 float2 uv=(world.xy-AtmosphereShadowArea.xy)/AtmosphereShadowArea.z+.5;
 // Outside prepared coverage retain unshadowed light rather than clamping an edge.
 if(any(uv<0)||any(uv>1))return 1;
 return AtmosphereCloudShadow.SampleLevel(AtmosphereClamp,uv,0);
}
float4 AtmosphereIntegral(float3 world){
 float4 projected=mul(float4(world,1),AtmosphereViewProjection);
 float2 uv=projected.xy/max(abs(projected.w),.001)*float2(.5,-.5)+.5;
 float depth=dot(float4(world,1),AtmosphereViewport);
 float volumeZ=(saturate(depth/max(AtmosphereRange.x,1))*63+.5)/64;
 float4 value=AtmosphereVolume.SampleLevel(AtmosphereClamp,float3(uv,volumeZ),0);
 return lerp(float4(0,0,0,1),value,step(.5,AtmosphereCamera.w)*step(.001,projected.w));
}
float3 ApplyAtmosphere(float3 radiance,float3 world){
 float4 volume=AtmosphereIntegral(world);return radiance*volume.a+volume.rgb;
}
