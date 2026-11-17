// SPDX-License-Identifier: GPL-3.0-or-later
// Shared energy-conserving GGX response for native material and scene paths.
// Same height-correlated Smith visibility as the offline split-sum LUT.
// roughness is perceptual; alpha is its square. No resource allocation.
float SmithGGXVisibility(float nv,float nl,float alpha){
 float a2=alpha*alpha;
 float gv=nl*sqrt(nv*nv*(1-a2)+a2);
 float gl=nv*sqrt(nl*nl*(1-a2)+a2);
 return .5/max(gv+gl,1e-12);
}
float3 EvaluateGGX(float3 albedo,float roughness,float metalness,float3 n,float3 v,float3 l,float3 energyCompensation){
 float3 h=(l+v)*rsqrt(max(dot(l+v,l+v),1e-12));
 float nl=saturate(dot(n,l)),nv=max(saturate(dot(n,v)),1e-4);
 float nh=saturate(dot(n,h)),vh=saturate(dot(v,h));
 float alpha=roughness*roughness,alpha2=alpha*alpha;
 // Cross-product form avoids cancellation in sharp near-normal highlights.
 float3 perpendicular=cross(n,h);
 float denominator=dot(perpendicular,perpendicular)+nh*nh*alpha2;
 float distribution=alpha2/max(3.14159265*denominator*denominator,1e-12);
 float visibility=SmithGGXVisibility(nv,nl,alpha);
 float3 f0=lerp(.04.xxx,albedo,metalness);
 float3 fresnel=f0+(1-f0)*pow(1-vh,5);
 float3 specular=distribution*visibility*fresnel*energyCompensation;
 float3 diffuse=(1-fresnel)*(1-metalness)*albedo/3.14159265;
 return (diffuse+specular)*nl;
}

// Reference/fallback single-scatter path without an environment table.
float3 EvaluateGGX(float3 albedo,float roughness,float metalness,float3 n,float3 v,float3 l){
 return EvaluateGGX(albedo,roughness,metalness,n,v,l,float3(1,1,1));
}

// IEC sRGB transfer, separate from linear normal/ORM/height data.
float3 DecodeSRGB(float3 x){x=max(x,0);return lerp(pow((x+.055)/1.055,2.4),x/12.92,step(x,.04045));}
float3 EncodeSRGB(float3 x){x=max(x,0);return lerp(1.055*pow(x,1.0/2.4)-.055,x*12.92,step(x,.0031308));}
// Keep extreme specular/emissive values representable by the native FP16 HDR
// scene target. Preserve all in-range radiance; overflow would poison tonemapping.
float3 HDRSurfaceRadiance(float3 x){return clamp(x,0,65504);}
// Precomputed distant HDR IBL; new slots do not overlap WW3D material maps,
// terrain, shadow, instance or HUD resources. Missing/disabled uses old fill.
TextureCube<float4> EnvironmentReflection:register(t18);
Texture2D<float2> EnvironmentBRDF:register(t19);
SamplerState EnvironmentSampler:register(s10);
cbuffer EnvironmentData:register(b5){float4 EnvironmentSettings;float4 EnvironmentSH[9];};
// Read the split-sum table once per surface, outside all native light loops.
// E=A+B is its unit-Fresnel directional energy. The scaled-lobe recovery
// approximates multiple scattering, preserving the existing authored maps.
struct MaterialBRDFResponse {float2 integrated;float3 energy;};
MaterialBRDFResponse LoadMaterialBRDF(float roughness,float nv,float3 f0){
 MaterialBRDFResponse p;p.integrated=float2(1,0);p.energy=float3(1,1,1);
 if(EnvironmentSettings.x>.5){
  float2 domain=float2(sqrt(saturate((nv-1e-4)/.9999)),roughness);
  p.integrated=EnvironmentBRDF.SampleLevel(EnvironmentSampler,domain*EnvironmentSettings.z+EnvironmentSettings.w,0);
  float directionalEnergy=clamp(p.integrated.x+p.integrated.y,1e-4,1);
  p.energy=1+f0*(rcp(directionalEnergy)-1);
 }
 // IBL-off/missing-table mode explicitly keeps the single-scatter fallback.
 return p;
}
float3 EnvironmentDiffuse(float3 n){
 float3 irradiance=EnvironmentSH[0].rgb*.2820947918
  +EnvironmentSH[1].rgb*(.4886025119*n.y)+EnvironmentSH[2].rgb*(.4886025119*n.z)+EnvironmentSH[3].rgb*(.4886025119*n.x)
  +EnvironmentSH[4].rgb*(1.092548431*n.x*n.y)+EnvironmentSH[5].rgb*(1.092548431*n.y*n.z)
  +EnvironmentSH[6].rgb*(.3153915653*(3*n.z*n.z-1))+EnvironmentSH[7].rgb*(1.092548431*n.x*n.z)
  +EnvironmentSH[8].rgb*(.5462742153*(n.x*n.x-n.y*n.y));return max(irradiance,0);
}
// Empirical specular AO differs from Lambert diffuse AO. Neither affects direct light.
float SpecularOcclusion(float nv,float roughness,float ao){
 ao=saturate(ao);return saturate(pow(max(nv+ao,1e-6),exp2(-16*roughness-1))-1+ao);
}
float ReflectionHorizon(float3 n,float3 v,float3 geometricNormal){
 float horizon=saturate(1+dot(reflect(-v,n),geometricNormal));return horizon*horizon;
}
struct IndirectLightingComponents {float3 diffuse;float3 specular;};
float3 EvaluateIndirect(float3 albedo,float roughness,float metalness,float3 n,float3 v,float3 ambient,float ao,float3 geometricNormal,MaterialBRDFResponse brdf,out IndirectLightingComponents components){
 float nv=saturate(dot(n,v));float3 f0=lerp(.04.xxx,albedo,metalness);
 float3 fresnel=f0+(max((1-roughness).xxx,f0)-f0)*pow(1-nv,5);
 float3 diffuse=(1-fresnel)*(1-metalness)*albedo*ambient*(.75+.25*saturate(n.z*.5+.5));
 float sky=saturate(reflect(-v,n).z*.5+.5);
 float3 environment=ambient*lerp(float3(.72,.65,.54),float3(.84,.94,1.08),sky);
 float specularAO=SpecularOcclusion(nv,roughness,ao)*ReflectionHorizon(n,v,geometricNormal);
 components.diffuse=diffuse*ao;components.specular=environment*fresnel*(1-.45*roughness)*specularAO;
 float3 result=diffuse*ao+environment*fresnel*(1-.45*roughness)*specularAO;
 if(EnvironmentSettings.x>.5){
  float3 reflection=EnvironmentReflection.SampleLevel(EnvironmentSampler,reflect(-v,n),roughness*EnvironmentSettings.y).rgb;
  float2 integrated=brdf.integrated;
  float3 diffuseIBL=(1-fresnel)*(1-metalness)*albedo*EnvironmentDiffuse(n);
  float3 specularIBL=reflection*(f0*integrated.x+integrated.y)*brdf.energy;
  result=(diffuseIBL*ao+specularIBL*specularAO)*ambient;
  components.diffuse=diffuseIBL*ao*ambient;components.specular=specularIBL*specularAO*ambient;
 }
 return result;
}
float3 EvaluateIndirect(float3 albedo,float roughness,float metalness,float3 n,float3 v,float3 ambient,float ao,float3 geometricNormal,MaterialBRDFResponse brdf){
 IndirectLightingComponents unused;return EvaluateIndirect(albedo,roughness,metalness,n,v,ambient,ao,geometricNormal,brdf,unused);
}
// Diagnostic/reference callers can request the same complete response directly.
float3 EvaluateIndirect(float3 albedo,float roughness,float metalness,float3 n,float3 v,float3 ambient,float ao,float3 geometricNormal){
 MaterialBRDFResponse brdf=LoadMaterialBRDF(roughness,saturate(dot(n,v)),lerp(.04.xxx,albedo,metalness));
 return EvaluateIndirect(albedo,roughness,metalness,n,v,ambient,ao,geometricNormal,brdf);
}
float FilteredRoughness(float roughness,float3 n){
 float3 dx=ddx(n),dy=ddy(n);float variance=min(.2,.15*(dot(dx,dx)+dot(dy,dy)));
 return sqrt(saturate(roughness*roughness+variance));
}
