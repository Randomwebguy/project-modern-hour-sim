// SPDX-License-Identifier: GPL-3.0-or-later
Texture2D<float> SceneDepthInput:register(t37);
Texture2D<float2> PreviousDepthRange:register(t38);
RWTexture2D<float2> SceneDepthOutput:register(u0);
cbuffer SceneDepthDimensions:register(b13){uint2 SourceDepthSize;uint2 OutputDepthSize;};
// Conservative overlap coverage also retains odd last rows/columns. A parent
// covers every child texel whose normalized footprint intersects its footprint.
void DepthBounds(uint2 pixel,out uint2 begin,out uint2 end){
 begin=(pixel*SourceDepthSize)/OutputDepthSize;
 end=((pixel+1)*SourceDepthSize+OutputDepthSize-1)/OutputDepthSize;
 end=min(end,SourceDepthSize);
}
[numthreads(8,8,1)]void CSSceneDepthCopy(uint3 id:SV_DispatchThreadID){
 if(any(id.xy>=OutputDepthSize))return;
 uint2 begin,end;DepthBounds(id.xy,begin,end);float2 range=float2(1,0);
 [loop]for(uint y=begin.y;y<end.y;++y)[loop]for(uint x=begin.x;x<end.x;++x){
  float d=SceneDepthInput.Load(int3(x,y,0));range=float2(min(range.x,d),max(range.y,d));
 }
 SceneDepthOutput[id.xy]=range;
}
[numthreads(8,8,1)]void CSSceneDepthReduce(uint3 id:SV_DispatchThreadID){
 if(any(id.xy>=OutputDepthSize))return;
 uint2 begin,end;DepthBounds(id.xy,begin,end);float2 range=float2(1,0);
 [loop]for(uint y=begin.y;y<end.y;++y)[loop]for(uint x=begin.x;x<end.x;++x){
  float2 d=PreviousDepthRange.Load(int3(x,y,0));range=float2(min(range.x,d.x),max(range.y,d.y));
 }
 SceneDepthOutput[id.xy]=range;
}
