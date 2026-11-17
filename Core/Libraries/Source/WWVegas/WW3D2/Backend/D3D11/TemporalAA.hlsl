// SPDX-License-Identifier: GPL-3.0-or-later
// taa100: temporal anti-aliasing of the world image, resolved after the world
// passes (depth, GTAO, fog) and before the HUD. Current samples are jittered by
// a Halton(2,3) sub-pixel offset; the history is reprojected through the
// primary camera (scene depth, unjittered matrices), rejected where the previous
// frame's depth shows another surface there (a unit moved away or in), and
// clipped to the current neighbourhood's YCoCg variance box.
// Accumulation is linear (an unbiased coverage average at edges); a gentle
// luminance weight only suppresses extreme HDR samples (single-frame fireflies).
cbuffer TemporalData:register(b0){
 row_major float4x4 CurrentInverseVP; // unjittered
 row_major float4x4 PreviousVP;       // unjittered, previous frame
 float4 TemporalViewport;             // camera viewport x,y,width,height (pixels)
 float4 TemporalJitter;               // xy jitter of this frame (pixels, +y down), z history valid, w base blend
 float4 TemporalSize;                 // target width,height, 1/width, 1/height
 float4 TemporalDepth;                // depth range min,max, variance gamma, motion blend boost
 float4 TemporalFilter;               // x reconstruction sharpness (Gaussian exp(-x*d*d), d in pixels), y tonemapped accumulation,
                                      // z motion (pixels) for the full current-frame boost, w luminance weight strength
 row_major float4x4 PreviousInverseVP;// unjittered, previous frame
 float4 TemporalDisocclusion;         // x tolerance in pixel footprints, y tolerance relative to view depth, z enabled
 row_major float4x4 CurrentVP;        // unjittered
 float4 TemporalSharpen;              // x sharpness 0..1 (0 = plain copy)
};
Texture2D<float4> CurrentColor:register(t0);
Texture2D<float> SceneDepth:register(t1);
Texture2D<float4> HistoryColor:register(t2);
Texture2D<float> PreviousDepth:register(t3);
// photo107: per-object motion (aux 5 of the screen materials): xy = UV offset from the previous
// frame, w = 1 where a moving object is visible. Null or w = 0: the camera-only reprojection.
Texture2D<float4> TemporalMotion:register(t4);
SamplerState LinearClamp:register(s0);
struct TemporalVertex {float4 position:SV_POSITION;};
TemporalVertex VSTemporal(uint id:SV_VertexID){TemporalVertex v;v.position=float4(id==1?3:-1,id==2?-3:1,0,1);return v;}
float3 TemporalToYCoCg(float3 c){return float3(dot(c,float3(.25,.5,.25)),dot(c,float3(.5,0,-.5)),dot(c,float3(-.25,.5,-.25)));}
float3 TemporalFromYCoCg(float3 c){return float3(c.x+c.y-c.z,c.x+c.z,c.x-c.y-c.z);}
// Reversible tonemap (Karis), for the tonemapped accumulation (A/B only: biased dark at edges).
float3 TemporalCompress(float3 c){return c/(1+max(c.r,max(c.g,c.b)));}
float3 TemporalExpand(float3 c){return c/max(1-max(c.r,max(c.g,c.b)),1e-4);}
bool TemporalTonemapped(){return TemporalFilter.y>.5;}
float3 TemporalSpace(float3 c){c=max(c,0);return TemporalToYCoCg(TemporalTonemapped()?TemporalCompress(c):c);}
int2 TemporalClamp(int2 pixel){int2 low=int2(TemporalViewport.xy),high=low+int2(TemporalViewport.zw)-1;return clamp(pixel,low,high);}
// Neighbours stay inside the camera viewport (outside it is HUD/letterbox).
float3 TemporalLoad(int2 pixel){return TemporalSpace(CurrentColor.Load(int3(TemporalClamp(pixel),0)).rgb);}
float TemporalNormalizedDepth(float depth){return (depth-TemporalDepth.x)/max(TemporalDepth.y-TemporalDepth.x,1e-6);}
// World position of a pixel centre of the current frame.
float3 TemporalWorld(int2 pixel){
 pixel=TemporalClamp(pixel);float2 local=float2(pixel)+.5-TemporalViewport.xy;
 float4 world=mul(float4((local/TemporalViewport.zw)*float2(2,-2)+float2(-1,1),TemporalNormalizedDepth(SceneDepth.Load(int3(pixel,0))),1),CurrentInverseVP);
 return world.xyz/world.w;
}
// Catmull-Rom history in 5 bilinear taps (corners dropped): less blur from the
// repeated resampling than a single bilinear fetch.
float3 TemporalHistory(float2 uv){
 float2 position=uv*TemporalSize.xy,center=floor(position-.5)+.5,f=position-center;
 float2 w0=f*(-.5+f*(1-.5*f)),w1=1+f*f*(-2.5+1.5*f),w2=f*(.5+f*(2-1.5*f)),w3=f*f*(-.5+.5*f);
 float2 w12=w1+w2,t12=(center+w2/w12)*TemporalSize.zw,t0=(center-1)*TemporalSize.zw,t3=(center+2)*TemporalSize.zw;
 float3 result=HistoryColor.SampleLevel(LinearClamp,float2(t12.x,t0.y),0).rgb*(w12.x*w0.y)+
               HistoryColor.SampleLevel(LinearClamp,float2(t0.x,t12.y),0).rgb*(w0.x*w12.y)+
               HistoryColor.SampleLevel(LinearClamp,t12,0).rgb*(w12.x*w12.y)+
               HistoryColor.SampleLevel(LinearClamp,float2(t3.x,t12.y),0).rgb*(w3.x*w12.y)+
               HistoryColor.SampleLevel(LinearClamp,float2(t12.x,t3.y),0).rgb*(w12.x*w3.y);
 float weight=w12.x*w0.y+w0.x*w12.y+w12.x*w12.y+w3.x*w12.y+w12.x*w3.y;
 return max(result/max(weight,1e-4),0);
}
// Clip toward the box centre (not a per-channel clamp): keeps hue.
float3 TemporalClip(float3 history,float3 low,float3 high){
 float3 center=.5*(high+low),extent=.5*(high-low)+1e-5,offset=history-center,ratio=abs(offset/extent);
 float largest=max(ratio.x,max(ratio.y,ratio.z));
 return largest>1?center+offset/largest:history;
}
float4 PSTemporalResolve(TemporalVertex input):SV_TARGET{
 int2 pixel=int2(input.position.xy);
 float2 local=input.position.xy-TemporalViewport.xy;
 // Outside the primary camera viewport the image is passed through.
 if(any(local<0)||any(local>=TemporalViewport.zw))return float4(CurrentColor.Load(int3(pixel,0)).rgb,1);
 // Neighbourhood statistics and a Gaussian reconstruction around the unjittered
 // pixel centre (a jittered sample at offset o lies at o-jitter from it).
 float3 sum=0,sumSquares=0,low=1e9,high=-1e9,filtered=0;float weightSum=0;
 [unroll]for(int y=-1;y<=1;++y)[unroll]for(int x=-1;x<=1;++x){
  float3 value=TemporalLoad(pixel+int2(x,y));
  sum+=value;sumSquares+=value*value;low=min(low,value);high=max(high,value);
  float2 d=float2(x,y)-TemporalJitter.xy;float weight=exp(-TemporalFilter.x*dot(d,d));
  filtered+=value*weight;weightSum+=weight;
 }
 filtered/=weightSum;
 float3 mean=sum/9,sigma=sqrt(max(sumSquares/9-mean*mean,0));
 // The box always holds this frame's own estimate: otherwise a pixel brighter or
 // darker than its neighbourhood is pulled toward it frame after frame.
 float3 boxLow=min(max(low,mean-TemporalDepth.z*sigma),filtered),boxHigh=max(min(high,mean+TemporalDepth.z*sigma),filtered);
 // Reprojection: the visible surface through the previous camera.
 float3 world=TemporalWorld(pixel);
 // The motion is the difference of the same point through both cameras, so the
 // round trip error of the reconstruction cancels (exactly zero for a still camera).
 float4 previous=mul(float4(world,1),PreviousVP),current=mul(float4(world,1),CurrentVP);
 float2 previousNdc=previous.xy/previous.w;
 float2 previousPixel=input.position.xy+(previousNdc-current.xy/current.w)*float2(.5,-.5)*TemporalViewport.zw;
 // photo107: a moving object carries its own offset; the camera-only one is wrong for it.
 float4 objectMotion=TemporalMotion.Load(int3(pixel,0));
 if(objectMotion.w>.5)previousPixel=input.position.xy+objectMotion.xy*TemporalViewport.zw;
 float2 previousUV=previousPixel*TemporalSize.zw;
 bool valid=TemporalJitter.z>.5&&previous.w>0&&all(previousPixel>=TemporalViewport.xy)&&all(previousPixel<TemporalViewport.xy+TemporalViewport.zw)&&all(isfinite(previousUV));
 float3 result=filtered;
 [branch]if(valid){
  // Disocclusion: none of the previous frame's 2x2 depths around the reprojected
  // point lies on this surface (within a few pixel footprints): the history shows
  // another object (a unit that moved away, or the ground a unit now covers).
  float trust=1;
  [branch]if(TemporalDisocclusion.z>.5){
   float footprint=max(min(distance(TemporalWorld(pixel+int2(1,0)),world),distance(TemporalWorld(pixel-int2(1,0)),world)),
                       min(distance(TemporalWorld(pixel+int2(0,1)),world),distance(TemporalWorld(pixel-int2(0,1)),world)));
   float tolerance=max(TemporalDisocclusion.x*footprint+TemporalDisocclusion.y*previous.w,1e-4);
   float4 depths=PreviousDepth.GatherRed(LinearClamp,previousUV);float nearest=1e30;
   [unroll]for(int i=0;i<4;++i){float4 p=mul(float4(previousNdc,TemporalNormalizedDepth(depths[i]),1),PreviousInverseVP);nearest=min(nearest,distance(p.xyz/p.w,world));}
   trust=saturate(2-nearest/tolerance);
  }
  float3 history=TemporalClip(TemporalSpace(TemporalHistory(previousUV)),boxLow,boxHigh);
  // Fast motion resamples the history often: weigh the current frame more.
  float motion=length(previousPixel-input.position.xy);
  float alpha=saturate(TemporalJitter.w+TemporalDepth.w*saturate(motion/max(TemporalFilter.z,1e-3)));
  float currentWeight=alpha,historyWeight=(1-alpha)*trust;
  if(!TemporalTonemapped()){currentWeight/=1+TemporalFilter.w*max(filtered.x,0);historyWeight/=1+TemporalFilter.w*max(history.x,0);}
  result=(filtered*currentWeight+history*historyWeight)/(currentWeight+historyWeight);
 }
 float3 color=TemporalFromYCoCg(result);
 // The compressed range is [0,1); keep the expansion finite.
 return float4(TemporalTonemapped()?TemporalExpand(clamp(color,0,.999)):max(color,0),1);
}

// post102: contrast-adaptive sharpening (after the AMD FidelityFX CAS idea, MIT): the
// resolved image's 4-neighbour cross is subtracted with a weight that shrinks where
// the local contrast is already high, so soft TAA detail sharpens without halos at
// strong edges. Runs on compressed (tonemap-like) values: a bright edge cannot ring.
// Reads the new history (t2), writes the scene colour; the history itself stays
// unsharpened, so sharpening never feeds back into the accumulation.
float4 PSTemporalSharpen(TemporalVertex input):SV_TARGET{
 int2 pixel=int2(input.position.xy);
 float2 local=input.position.xy-TemporalViewport.xy;
 float3 centre=HistoryColor.Load(int3(pixel,0)).rgb;
 [branch]if(any(local<0)||any(local>=TemporalViewport.zw)||TemporalSharpen.x<=0)return float4(centre,1);
 int2 low=int2(TemporalViewport.xy),high=low+int2(TemporalViewport.zw)-1;
 float3 c=TemporalCompress(max(centre,0));
 float3 n=TemporalCompress(max(HistoryColor.Load(int3(clamp(pixel+int2(0,-1),low,high),0)).rgb,0));
 float3 s=TemporalCompress(max(HistoryColor.Load(int3(clamp(pixel+int2(0,1),low,high),0)).rgb,0));
 float3 w=TemporalCompress(max(HistoryColor.Load(int3(clamp(pixel+int2(-1,0),low,high),0)).rgb,0));
 float3 e=TemporalCompress(max(HistoryColor.Load(int3(clamp(pixel+int2(1,0),low,high),0)).rgb,0));
 float3 mn=min(c,min(min(n,s),min(w,e))),mx=max(c,max(max(n,s),max(w,e)));
 // Headroom to the nearest limit, relative to the peak: 1 in soft areas, 0 at full contrast.
 float3 amp=sqrt(saturate(min(mn,1-mx)/max(mx,1e-4)));
 // Scaled below the CAS peak (1/8..1/5): the TAA output is already filtered, and a full-
 // strength cross brings the stair steps of near-Nyquist texture detail back.
 float3 weight=-amp*.2*TemporalSharpen.x;
 // Limited to a quarter of the cross's own range past it: edges steepen and fine detail
 // gains contrast, but nothing rings far past its neighbours.
 float3 slack=(mx-mn)*.25;
 float3 result=clamp((c+(n+s+w+e)*weight)/(1+4*weight),max(mn-slack,0),min(mx+slack,.999));
 return float4(TemporalExpand(result),1);
}
