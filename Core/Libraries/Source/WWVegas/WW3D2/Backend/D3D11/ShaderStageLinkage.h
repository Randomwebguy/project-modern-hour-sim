// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <d3dcompiler.h>
#include <d3d11shader.h>
#include <wrl/client.h>
#include <cstring>
namespace GenZH::Modern {
// A compact stage is eligible only when the actual compiled consumer reads
// no missing component. Unknown/reflection failures retain the full stage.
inline bool CanLinkRasterStage(ID3DBlob *producer,ID3DBlob *consumer,bool exactRegisterLayout=false){
 if(!producer||!consumer)return false;
 Microsoft::WRL::ComPtr<ID3D11ShaderReflection> output,input;
 if(FAILED(D3DReflect(producer->GetBufferPointer(),producer->GetBufferSize(),IID_PPV_ARGS(output.GetAddressOf())))||
    FAILED(D3DReflect(consumer->GetBufferPointer(),consumer->GetBufferSize(),IID_PPV_ARGS(input.GetAddressOf()))))return false;
 D3D11_SHADER_DESC outputs{},inputs{};if(FAILED(output->GetDesc(&outputs))||FAILED(input->GetDesc(&inputs)))return false;
 for(UINT i=0;i<inputs.InputParameters;++i){
  D3D11_SIGNATURE_PARAMETER_DESC required{};if(FAILED(input->GetInputParameterDesc(i,&required)))return false;
  if(!required.ReadWriteMask)continue;
  // Masks refer to packed register lanes. Removing an earlier varying may
  // move a scalar from z to y while its semantic value remains unchanged.
  UINT requiredMask=required.Mask,reads=required.ReadWriteMask&required.Mask;
  if(!requiredMask)return false;
  while(!(requiredMask&1u)){requiredMask>>=1;reads>>=1;}
  bool found=false;
  for(UINT j=0;j<outputs.OutputParameters;++j){
   D3D11_SIGNATURE_PARAMETER_DESC supplied{};if(FAILED(output->GetOutputParameterDesc(j,&supplied)))return false;
   UINT suppliedMask=supplied.Mask;
   if(!suppliedMask)continue;
   while(!(suppliedMask&1u))suppliedMask>>=1;
   if(required.SemanticIndex==supplied.SemanticIndex&&_stricmp(required.SemanticName,supplied.SemanticName)==0&&
      required.ComponentType==supplied.ComponentType&&required.SystemValueType==supplied.SystemValueType&&
      (reads&~suppliedMask)==0&&(!exactRegisterLayout||
       (required.Register==supplied.Register&&(required.ReadWriteMask&~supplied.Mask)==0))){found=true;break;}
  }
  if(!found)return false;
 }
 return true;
}
}
