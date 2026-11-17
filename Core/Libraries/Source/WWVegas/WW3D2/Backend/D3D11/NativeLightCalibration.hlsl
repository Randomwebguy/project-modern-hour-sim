// SPDX-License-Identifier: GPL-3.0-or-later
// Uniform native light state, decoded once on CPU instead of per fragment.
cbuffer NativeCalibratedLightData:register(b12){
 float4 CalibratedGlobalAmbient;
 float4 CalibratedDirectRadiance[4];
 float4 CalibratedLocalAmbient[4];
};
float3 NativeCalibratedDirect(uint light,float3 encoded){
 float3 value=float3(0,0,0);
 [branch]if(CalibratedGlobalAmbient.w>.5)value=CalibratedDirectRadiance[light].rgb;
 else value=DecodeSRGB(encoded)*3.14159265;
 return value;
}
float3 NativeCalibratedAmbient(uint light,float3 encoded){
 float3 value=float3(0,0,0);
 [branch]if(CalibratedGlobalAmbient.w>.5)value=CalibratedLocalAmbient[light].rgb;
 else value=DecodeSRGB(encoded);
 return value;
}
float3 NativeCalibratedGlobal(float3 encoded){
 float3 value=float3(0,0,0);
 [branch]if(CalibratedGlobalAmbient.w>.5)value=CalibratedGlobalAmbient.rgb;
 else value=DecodeSRGB(encoded);
 return value;
}
