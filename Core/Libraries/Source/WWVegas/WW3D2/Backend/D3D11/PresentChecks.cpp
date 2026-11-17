// SPDX-License-Identifier: GPL-3.0-or-later
// present101: offline checks of the production RenderDevice swap chain on a hidden
// window (no input, nothing shown): tearing support matches DXGI, the frame-latency
// waitable object exists, presents and a resize succeed with the swap chain flags,
// and GENZH_TEARING=off restores the previous swap chain (no flags).
#include "RenderDevice.h"
#include <dxgi1_6.h>
#include <chrono>
#include <cstdio>
#include <iostream>
#include <stdexcept>
#include <string>
using namespace GenZH::Modern;using Microsoft::WRL::ComPtr;
static unsigned checks=0;
static void Check(bool ok,const char *what){++checks;if(!ok)throw std::runtime_error(what);}
int main(){try{
 WNDCLASSW wc{};wc.lpfnWndProc=DefWindowProcW;wc.hInstance=GetModuleHandleW(nullptr);wc.lpszClassName=L"GenZHPresentChecks";RegisterClassW(&wc);
 HWND window=CreateWindowExW(0,wc.lpszClassName,L"present checks",WS_OVERLAPPEDWINDOW,0,0,640,360,nullptr,nullptr,wc.hInstance,nullptr);Check(window!=nullptr,"hidden window");
 BOOL supported=FALSE;{ComPtr<IDXGIFactory5> f;if(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(f.GetAddressOf()))))f->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING,&supported,sizeof(supported));}
 auto run=[&](bool modern){
  SetEnvironmentVariableA("GENZH_TEARING",modern?nullptr:"off");
  RenderDevice device;Check(SUCCEEDED(device.Initialize(window,640,360,false)),"device and swap chain");
  if(modern){Check(device.Tearing()==(supported!=FALSE),"tearing follows DXGI support");Check(device.FrameWaitable(),"frame-latency waitable object");
   Check((device.SwapFlags()&DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT)!=0,"waitable swap chain flag");}
  else Check(device.SwapFlags()==0&&!device.Tearing()&&!device.FrameWaitable(),"GENZH_TEARING=off keeps the previous swap chain");
  const auto start=std::chrono::steady_clock::now();
  for(unsigned i=0;i<120;++i){const HRESULT hr=device.PresentFrame(false);Check(SUCCEEDED(hr),"present");}
  const double ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count()/120;
  Check(SUCCEEDED(device.Resize(800,450)),"resize with the swap chain flags");
  for(unsigned i=0;i<30;++i)Check(SUCCEEDED(device.PresentFrame(false)),"present after resize");
  device.Shutdown();return ms;};
 const double modernMs=run(true),legacyMs=run(false);
 SetEnvironmentVariableA("GENZH_TEARING",nullptr);DestroyWindow(window);
 std::cout<<"{\"passed\":true,\"checks\":"<<checks<<",\"tearing_supported\":"<<(supported?"true":"false")<<",\"present_ms_modern\":"<<modernMs<<",\"present_ms_legacy\":"<<legacyMs<<"}"<<std::endl;return 0;
}catch(const std::exception &e){std::cout<<"{\"passed\":false,\"checks\":"<<checks<<",\"error\":\""<<e.what()<<"\"}"<<std::endl;return 1;}}
