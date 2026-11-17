// SPDX-License-Identifier: GPL-3.0-or-later
// post106: offline GPU checks of the mip-chain bloom (BloomGPU): nothing below the knee,
// energy of the glow against the thresholded energy, a smooth and round falloff, stability of
// a sub-pixel moving emitter, a lone bright pixel's glow and the cost at 1920x1080.
#include "BloomGPU.h"
#include <DirectXPackedVector.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;using namespace GenZH::Modern;
namespace {
unsigned checks=0;
void Check(bool condition,const std::string &label){++checks;if(!condition)throw std::runtime_error(label);}
void HR(HRESULT hr,const std::string &label){if(FAILED(hr)){char code[16];std::snprintf(code,sizeof(code),"%08lx",static_cast<unsigned long>(hr));throw std::runtime_error(label+" 0x"+code);}}
struct Gpu {
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;
    void Create(){D3D_FEATURE_LEVEL level=D3D_FEATURE_LEVEL_11_0;HR(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,&level,1,D3D11_SDK_VERSION,device.GetAddressOf(),nullptr,context.GetAddressOf()),"device");}
    ComPtr<ID3D11ShaderResourceView> Upload(const std::vector<float> &rgba,UINT w,UINT h){
        D3D11_TEXTURE2D_DESC d{};d.Width=w;d.Height=h;d.MipLevels=d.ArraySize=d.SampleDesc.Count=1;d.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;d.BindFlags=D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA init{rgba.data(),w*16,0};ComPtr<ID3D11Texture2D> t;HR(device->CreateTexture2D(&d,&init,t.GetAddressOf()),"hdr texture");
        ComPtr<ID3D11ShaderResourceView> v;HR(device->CreateShaderResourceView(t.Get(),nullptr,v.GetAddressOf()),"hdr view");return v;}
    // Level 0 of the bloom chain (half resolution), unpacked to RGB floats.
    std::vector<float> Read(const BloomGPU &bloom,UINT &w,UINT &h){
        ComPtr<ID3D11Resource> resource;bloom.Result()->GetResource(resource.GetAddressOf());ComPtr<ID3D11Texture2D> texture;HR(resource.As(&texture),"bloom texture");
        D3D11_TEXTURE2D_DESC d{};texture->GetDesc(&d);w=d.Width;h=d.Height;d.BindFlags=0;d.Usage=D3D11_USAGE_STAGING;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> staging;HR(device->CreateTexture2D(&d,nullptr,staging.GetAddressOf()),"staging");context->CopyResource(staging.Get(),texture.Get());
        D3D11_MAPPED_SUBRESOURCE map{};HR(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&map),"map");std::vector<float> out(std::size_t(w)*h*3);
        for(UINT y=0;y<h;++y){const auto *row=reinterpret_cast<const DirectX::PackedVector::XMFLOAT3PK*>(static_cast<const unsigned char*>(map.pData)+y*map.RowPitch);
            for(UINT x=0;x<w;++x){DirectX::XMFLOAT3 v;DirectX::XMStoreFloat3(&v,DirectX::PackedVector::XMLoadFloat3PK(&row[x]));const std::size_t i=(std::size_t(y)*w+x)*3;out[i]=v.x;out[i+1]=v.y;out[i+2]=v.z;}}
        context->Unmap(staging.Get(),0);return out;}
};
struct Image {std::vector<float> rgba;UINT w=0,h=0;
    Image(UINT width,UINT height,float fill){w=width;h=height;rgba.assign(std::size_t(w)*h*4,fill);for(std::size_t i=3;i<rgba.size();i+=4)rgba[i]=1;}
    void Add(float x,float y,float value){if(x<0||y<0||x>=w||y>=h)return;const std::size_t i=(std::size_t(y)*w+std::size_t(x))*4;for(int k=0;k<3;++k)rgba[i+k]+=value;}
    // A square emitter of side `size` at a sub-pixel position (area-weighted coverage).
    void Square(float cx,float cy,float size,float value){const float x0=cx-size/2,y0=cy-size/2,x1=x0+size,y1=y0+size;
        for(int y=int(std::floor(y0));y<int(std::ceil(y1));++y)for(int x=int(std::floor(x0));x<int(std::ceil(x1));++x){
            const float cover=std::max(0.f,std::min(x1,x+1.f)-std::max(x0,float(x)))*std::max(0.f,std::min(y1,y+1.f)-std::max(y0,float(y)));if(cover>0)Add(float(x),float(y),value*cover);}}
};
double Energy(const std::vector<float> &half){double s=0;for(std::size_t i=0;i<half.size();i+=3)s+=(half[i]+half[i+1]+half[i+2])/3;return s;}
}

int main(){try{
    Gpu gpu;gpu.Create();const UINT W=512,H=512;BloomGPU bloom;HR(bloom.Create(gpu.device.Get(),W,H),"bloom create: "+bloom.Stats().lastError);
    Check(bloom.Stats().levels==6,"six levels from half resolution down");
    auto run=[&](const Image &image,float exposure,UINT &w,UINT &h){auto view=gpu.Upload(image.rgba,image.w,image.h);HR(bloom.Apply(gpu.context.Get(),view.Get(),exposure),"apply");return gpu.Read(bloom,w,h);};
    // Composite energy at full resolution: each half-resolution texel covers four pixels.
    auto glow=[&](const std::vector<float> &half){return Energy(half)*4*bloom.CompositeScale()/bloom.settings.intensity;};
    UINT w=0,h=0;
    // 1. Nothing below the knee (threshold 1, knee .5: exposed .4 never glows).
    {Image dim(W,H,.4f);const auto out=run(dim,1,w,h);float peak=0;for(float v:out)peak=std::max(peak,v);std::cerr<<"below knee: peak "<<peak<<"\n";Check(peak==0,"no glow below the knee");
     const auto exposed=run(dim,4,w,h);float bright=0;for(float v:exposed)bright=std::max(bright,v);Check(bright>.5f,"exposure is applied before the threshold");}
    // 2. Energy: a 32x32 emitter of 4 (excess 3 per channel) spreads ~its thresholded energy.
    double energyRatio=0;{Image block(W,H,0);block.Square(256,256,32,4);const auto out=run(block,1,w,h);energyRatio=glow(out)/(32.0*32*3);
     std::cerr<<"energy of the glow / thresholded energy "<<energyRatio<<"\n";Check(energyRatio>.95&&energyRatio<1.05,"glow energy matches the thresholded energy");}
    // 3. Falloff of an 8x8 emitter: rings of the half-resolution glow (centre 128,128).
    std::vector<double> ring(64,0);std::vector<unsigned> ringN(64,0);double axis[2]{},diagonal[2]{};
    {Image block(W,H,0);block.Square(256,256,8,50);const auto out=run(block,1,w,h);
     for(UINT y=0;y<h;++y)for(UINT x=0;x<w;++x){const double dx=x+.5-128,dy=y+.5-128,r=std::sqrt(dx*dx+dy*dy);const auto bin=static_cast<std::size_t>(r);if(bin<ring.size()){ring[bin]+=out[(std::size_t(y)*w+x)*3+1];++ringN[bin];}}
     for(std::size_t i=0;i<ring.size();++i)ring[i]/=std::max(ringN[i],1u);
     auto at=[&](double x,double y){return double(out[(std::size_t(y)*w+std::size_t(x))*3+1]);};
     for(int k=0;k<2;++k){const double r=k?32:16,d=r/std::sqrt(2.0);
        axis[k]=(at(128+r,128)+at(128-r,128)+at(128,128+r)+at(128,128-r))/4;diagonal[k]=(at(128+d,128+d)+at(128-d,128+d)+at(128+d,128-d)+at(128-d,128-d))/4;}}
    bool monotonic=true;for(std::size_t i=5;i+1<60;++i)monotonic&=ring[i+1]<=ring[i]*1.02+1e-7;
    const double reach=ring[48]/std::max(ring[4],1e-12);
    std::cerr<<"falloff r4 "<<ring[4]<<" r16 "<<ring[16]<<" r32 "<<ring[32]<<" r48 "<<ring[48]<<" monotonic "<<monotonic<<"; axis/diagonal r16 "<<axis[0]/diagonal[0]<<" r32 "<<axis[1]/diagonal[1]<<"\n";
    Check(monotonic,"the glow falls off smoothly (no rings or blocks)");
    Check(reach>1e-3,"the glow reaches far (1/64 resolution level)");
    Check(axis[0]/diagonal[0]>.8&&axis[0]/diagonal[0]<1.25&&axis[1]/diagonal[1]>.8&&axis[1]/diagonal[1]<1.25,"the glow is round");
    // 4. A 2x2 emitter moving in quarter-pixel steps: energy and peak of the glow stay steady.
    double minEnergy=1e30,maxEnergy=0,minPeak=1e30,maxPeak=0;
    for(int step=0;step<8;++step){Image moving(W,H,0);moving.Square(256+.25f*step,256.f,2,20);const auto out=run(moving,1,w,h);const double e=Energy(out);float peak=0;for(float v:out)peak=std::max(peak,v);
        minEnergy=std::min(minEnergy,e);maxEnergy=std::max(maxEnergy,e);minPeak=std::min(minPeak,double(peak));maxPeak=std::max(maxPeak,double(peak));}
    const double energySwing=maxEnergy/minEnergy-1,peakSwing=maxPeak/minPeak-1;
    std::cerr<<"moving emitter: energy swing "<<energySwing<<" peak swing "<<peakSwing<<"\n";
    Check(energySwing<.05&&peakSwing<.15,"a sub-pixel moving emitter keeps a steady glow");
    // 5. A lone 1024 pixel spreads exactly its own thresholded energy (the chain is linear and
    // never amplifies; temporal fireflies are already held by the TAA resolve before the bloom).
    double firefly=0;{Image single(W,H,0);single.Add(256,256,1024);firefly=glow(run(single,1,w,h))/1023.0;}
    std::cerr<<"lone pixel glow / its thresholded energy "<<firefly<<"\n";Check(firefly>.9&&firefly<1.05,"a lone bright pixel spreads its own energy, no more");
    // 6. Cost at 1920x1080.
    double ms=1e9;{BloomGPU timed;HR(timed.Create(gpu.device.Get(),1920,1080),"timed create");Image frame(1920,1080,.5f);frame.Square(900,500,40,8);auto view=gpu.Upload(frame.rgba,1920,1080);
     D3D11_QUERY_DESC q{D3D11_QUERY_TIMESTAMP_DISJOINT,0};ComPtr<ID3D11Query> disjoint,start,end;HR(gpu.device->CreateQuery(&q,disjoint.GetAddressOf()),"query");q.Query=D3D11_QUERY_TIMESTAMP;
     HR(gpu.device->CreateQuery(&q,start.GetAddressOf()),"query");HR(gpu.device->CreateQuery(&q,end.GetAddressOf()),"query");HR(timed.Apply(gpu.context.Get(),view.Get(),1),"warm");
     for(int round=0;round<30;++round){gpu.context->Begin(disjoint.Get());gpu.context->End(start.Get());for(int r=0;r<10;++r)timed.Apply(gpu.context.Get(),view.Get(),1);
        gpu.context->End(end.Get());gpu.context->End(disjoint.Get());D3D11_QUERY_DATA_TIMESTAMP_DISJOINT d{};while(gpu.context->GetData(disjoint.Get(),&d,sizeof(d),0)!=S_OK){}
        UINT64 a=0,b=0;while(gpu.context->GetData(start.Get(),&a,8,0)!=S_OK){}while(gpu.context->GetData(end.Get(),&b,8,0)!=S_OK){}if(!d.Disjoint)ms=std::min(ms,double(b-a)/double(d.Frequency)*1000/10);}
     Check(ms<1e8,"timed bloom");}
    std::cerr<<"bloom 1080p "<<ms<<" ms\n";
    std::cout<<"{\"passed\":true,\"checks\":"<<checks<<",\"levels\":"<<bloom.Stats().levels<<",\"energy_ratio\":"<<energyRatio<<",\"falloff\":{\"r4\":"<<ring[4]<<",\"r16\":"<<ring[16]<<",\"r32\":"<<ring[32]<<",\"r48\":"<<ring[48]<<"}"
             <<",\"round_r16\":"<<axis[0]/diagonal[0]<<",\"round_r32\":"<<axis[1]/diagonal[1]<<",\"moving_energy_swing\":"<<energySwing<<",\"moving_peak_swing\":"<<peakSwing<<",\"firefly_ratio\":"<<firefly
             <<",\"intensity\":"<<bloom.settings.intensity<<",\"threshold\":"<<bloom.settings.threshold<<",\"knee\":"<<bloom.settings.knee<<",\"gpu_ms_1080p\":"<<ms<<"}"<<std::endl;
    return 0;
}catch(const std::exception &e){std::cout<<"{\"passed\":false,\"checks\":"<<checks<<",\"error\":\""<<e.what()<<"\"}"<<std::endl;return 1;}}
