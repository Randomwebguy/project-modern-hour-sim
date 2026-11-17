// SPDX-License-Identifier: GPL-3.0-or-later
// terrain96: offline real-GPU check of the world-space terrain material library.
// Loads the published library through the runtime loader, prepares a synthetic
// map atlas through the runtime terrain worker (class + region mean colour),
// renders the production LegacyPS with the library on and off, and writes raw
// float previews. terrain97: the production TerrainHS/TerrainDS with and without
// the library macro relief (cracks, bounds, flat classes, cost), and the GPU
// grass (roots on the rendered surface, grass ground only, stable placement,
// brightness against the ground, cost); terrain98: vehicle interaction (flatten,
// recovery, rotor wash, aircraft) and analytic grass shadows on the ground. Usage: genzh_terrain_librarychecks <library folder> <output folder> [environment.bin]
#include "CompatibilityRenderer.h"
#include "LegacyShaderSource.h"
#include "ProgramSource.h"
#include "ShaderBytecodeCache.h"
#include "EnvironmentGPU.h"
#include "NativeLightCalibrationGPU.h"
#include "TerrainMaterialLibrary.h"
#include "TerrainPreparation.h"
#include "TerrainGrass.h"
#include <DirectXMath.h>
#include <dxgi.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <set>
using namespace GenZH::Modern;
namespace {
unsigned checks=0;
void Check(bool ok,const std::string &label){++checks;if(!ok)throw std::runtime_error(label);}
void HR(HRESULT hr,const std::string &label){Check(SUCCEEDED(hr),label);}
constexpr unsigned CellsX=30,CellsY=16,AtlasW=1024,AtlasH=512,Region=128;
// Typical native Zero Hour ground palettes (sRGB): unknown, grass, dirt, soil,
// stone, sand, snow, asphalt, concrete, wood.
const std::array<std::array<int,3>,10> Palette{{{88,118,128},{92,112,56},{134,108,76},{104,80,56},{126,120,110},{198,172,126},{228,232,238},{68,68,70},{152,150,142},{120,96,66}}};
float Hash(unsigned x,unsigned y,unsigned s){unsigned h=x*374761393u+y*668265263u+s*2246822519u;h=(h^(h>>13))*1274126177u;return static_cast<float>((h^(h>>16))&0xffffu)/65535.0f;}
float Smooth(float x,float y,float scale,unsigned seed){
    const float fx=x/scale,fy=y/scale;const auto ix=static_cast<unsigned>(fx),iy=static_cast<unsigned>(fy);float tx=fx-static_cast<float>(ix),ty=fy-static_cast<float>(iy);
    tx=tx*tx*(3-2*tx);ty=ty*ty*(3-2*ty);
    const float a=Hash(ix,iy,seed),b=Hash(ix+1,iy,seed),c=Hash(ix,iy+1,seed),d=Hash(ix+1,iy+1,seed);
    return (a+(b-a)*tx)+((c+(d-c)*tx)-(a+(b-a)*tx))*ty;
}
double Linear(double c){c/=255.0;return c<=.04045?c/12.92:std::pow((c+.055)/1.055,2.4);}
std::array<unsigned,2> RegionOrigin(unsigned region){return {4+(region%5)*(Region+8),4+(region/5)*(Region+8)};}
bool FlippedCell(unsigned col,unsigned row){return (col*7+row*3)%4==0;}
float Bump(unsigned vx,unsigned vy){return vy<=11?1.5f*std::sin(static_cast<float>(vx)*.9f)*std::cos(static_cast<float>(vy)*1.3f):0.0f;}
float VertexHeight(unsigned row){
    // Flat cell rows 0-11, then cell rows 12/13/14 rise 6/12/25 units (n.z .86/.64/.37), row 15 is a plateau.
    const float rises[]={6,12,25};float z=0;for(unsigned r=13;r<=row&&r<=15;++r)z+=rises[r-13];return row<=12?0:z;
}
struct Target {
    ComPtr<ID3D11Texture2D> texture,staging;ComPtr<ID3D11RenderTargetView> view;ComPtr<ID3D11Texture2D> depth,depthStaging;ComPtr<ID3D11DepthStencilView> depthView;UINT width=0,height=0;
};
Target MakeTarget(ID3D11Device *device,UINT width,UINT height){
    Target t;t.width=width;t.height=height;D3D11_TEXTURE2D_DESC d{};d.Width=width;d.Height=height;d.MipLevels=d.ArraySize=d.SampleDesc.Count=1;
    d.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;d.BindFlags=D3D11_BIND_RENDER_TARGET;
    HR(device->CreateTexture2D(&d,nullptr,t.texture.GetAddressOf()),"float target");HR(device->CreateRenderTargetView(t.texture.Get(),nullptr,t.view.GetAddressOf()),"target view");
    d.BindFlags=0;d.Usage=D3D11_USAGE_STAGING;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;HR(device->CreateTexture2D(&d,nullptr,t.staging.GetAddressOf()),"readback");
    d.Format=DXGI_FORMAT_R32_FLOAT;HR(device->CreateTexture2D(&d,nullptr,t.depthStaging.GetAddressOf()),"depth readback");
    d.Usage=D3D11_USAGE_DEFAULT;d.CPUAccessFlags=0;d.Format=DXGI_FORMAT_R32_TYPELESS;d.BindFlags=D3D11_BIND_DEPTH_STENCIL;
    HR(device->CreateTexture2D(&d,nullptr,t.depth.GetAddressOf()),"depth");
    D3D11_DEPTH_STENCIL_VIEW_DESC dv{};dv.Format=DXGI_FORMAT_D32_FLOAT;dv.ViewDimension=D3D11_DSV_DIMENSION_TEXTURE2D;
    HR(device->CreateDepthStencilView(t.depth.Get(),&dv,t.depthView.GetAddressOf()),"depth view");
    return t;
}
}

int main(int argc,char **argv){try{
    Check(argc>=3,"usage: <library folder> <output folder> [environment.bin]");
    const std::filesystem::path library(argv[1]),output(argv[2]);std::filesystem::create_directories(output);
    SetEnvironmentVariableW(L"GENZH_USER_DATA_DIR",(output/L"profile").c_str());std::filesystem::create_directories(output/L"profile");
    std::ostringstream report;report<<"{";

    // 1. Manifest parser: the published file and malformed variants.
    TerrainLibraryManifest manifest;std::string error;
    {std::ifstream file(library/L"manifest.txt",std::ios::binary);Check(file.good(),"published manifest");Check(ParseTerrainLibraryManifest(file,manifest,error),"manifest parses: "+error);}
    Check(manifest.size==1024&&manifest.layers.size()==12,"12 layers at 1024");
    Check(!manifest.classes[0].enabled,"unknown ground keeps the native atlas");
    for(unsigned g=1;g<=8;++g)Check(manifest.classes[g].enabled,"every named ground class has layers");Check(!manifest.classes[9].enabled,"TERRAIN_WOOD keeps the native atlas");
    for(unsigned g=10;g<16;++g)Check(!manifest.classes[g].enabled,"no class beyond Wood");
    Check(manifest.classes[4].relief==2.4f&&manifest.classes[4].macroTile==84&&manifest.classes[4].tessellation==1,"stone macro relief");
    Check(manifest.classes[1].relief>0&&manifest.classes[1].tessellation==0,"grass relief keeps the native tessellation density");
    Check(manifest.classes[7].relief==0&&manifest.classes[8].relief==0,"asphalt and concrete stay flat");
    const auto table=TerrainLibraryTable(manifest);
    for(unsigned i=0;i<manifest.layers.size();++i){Check(table[i*4]==manifest.layers[i].tile,"table tile");for(unsigned c=0;c<3;++c)Check(table[i*4+1+c]==manifest.layers[i].mean[c],"table mean");}
    for(unsigned g=0;g<16;++g){Check(table[(48+g)*4]==manifest.classes[g].relief&&table[(48+g)*4+1]==manifest.classes[g].macroTile&&table[(48+g)*4+2]==manifest.classes[g].tessellation,"table relief");}
    for(unsigned g=0;g<16;++g){Check(table[(16+g)*4+3]==(manifest.classes[g].enabled?1.0f:0.0f),"table enabled");if(manifest.classes[g].enabled)Check(table[(16+g)*4]==static_cast<float>(manifest.classes[g].primary),"table primary");}
    const char *bad[]={"","terrain-library 2\nsize 1024\nlayer 0 A 10 .1 .1 .1\n","terrain-library 1\nsize 1000\nlayer 0 A 10 .1 .1 .1\n",
        "terrain-library 1\nsize 1024\nlayer 1 A 10 .1 .1 .1\n","terrain-library 1\nsize 1024\nlayer 0 ../x 10 .1 .1 .1\n",
        "terrain-library 1\nsize 1024\nlayer 0 A 10 .1 .1 .1\nclass 1 0 3 .5\n","terrain-library 1\nsize 1024\nlayer 0 A 10 .1 .1 .1\nclass 1 0 0 1.5\n",
        "terrain-library 1\nsize 1024\nlayer 0 A 10 .1 .1 .1\nclass 0 0 0 .5\n","terrain-library 1\nsize 1024\nlayer 0 A 10 .1 .1 .1 extra\n",
        "terrain-library 1\nsize 1024\nlayer 0 A 0 .1 .1 .1\n","terrain-library 1\nsize 1024\nlayer 0 A 10 0 .1 .1\n",
        "terrain-library 1\nsize 1024\nlayer 0 A 10 .1 .1 .1\nclass 1 0 0 .5\nclass 1 0 0 .5\n",
        "terrain-library 1\nsize 1024\nlayer 0 A 10 .1 .1 .1\nrelief 1 1 60 .5\n","terrain-library 1\nsize 1024\nlayer 0 A 10 .1 .1 .1\nclass 1 0 0 .5\nrelief 1 3 60 .5\n",
        "terrain-library 1\nsize 1024\nlayer 0 A 10 .1 .1 .1\nclass 1 0 0 .5\nrelief 1 1 5 .5\n","terrain-library 1\nsize 1024\nlayer 0 A 10 .1 .1 .1\nclass 1 0 0 .5\nrelief 1 1 60 2\n",
        "terrain-library 1\nsize 1024\nlayer 0 A 10 .1 .1 .1\nclass 1 0 0 .5\nrelief 1 1 60 .5\nrelief 1 1 60 .5\n"};
    for(const char *text:bad){std::istringstream in(text);TerrainLibraryManifest m;Check(!ParseTerrainLibraryManifest(in,m,error)&&m.layers.empty(),std::string("malformed manifest rejected: ")+text);}
    {std::istringstream in("terrain-library 1\r\nsize 64\r\nlayer 0 A 10 .1 .2 .3\r\nclass 2 0 0 .5\r\n");TerrainLibraryManifest m;Check(ParseTerrainLibraryManifest(in,m,error)&&m.classes[2].enabled&&m.layers[0].mean[2]==.3f,"CRLF manifest accepted");}

    // 2. Real hardware device; the runtime loader uploads the library.
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;D3D_FEATURE_LEVEL fl{};const D3D_FEATURE_LEVEL levels[]={D3D_FEATURE_LEVEL_11_0};
    HR(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,levels,1,D3D11_SDK_VERSION,device.GetAddressOf(),&fl,context.GetAddressOf()),"hardware device");
    ComPtr<IDXGIDevice> gd;ComPtr<IDXGIAdapter> ga;HR(device.As(&gd),"DXGI device");HR(gd->GetAdapter(ga.GetAddressOf()),"adapter");DXGI_ADAPTER_DESC ad{};ga->GetDesc(&ad);
    TerrainMaterialLibrary loader;
    {TerrainMaterialLibrary missing;missing.Start(output/L"no-such-library");for(int i=0;i<200&&!missing.Failed();++i){HR(missing.Pump(device.Get(),context.Get()),"missing library pump");std::this_thread::sleep_for(std::chrono::milliseconds(5));}
     Check(missing.Failed()&&!missing.Ready()&&!missing.Pending(),"missing library fails closed (native atlas kept)");}
    const auto loadStart=std::chrono::steady_clock::now();loader.Start(library);unsigned pumps=0;
    while(!loader.Ready()&&!loader.Failed()&&std::chrono::steady_clock::now()-loadStart<std::chrono::seconds(60)){HR(loader.Pump(device.Get(),context.Get()),"library pump");++pumps;std::this_thread::sleep_for(std::chrono::milliseconds(2));}
    const double loadMs=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-loadStart).count();
    Check(loader.Ready(),"library uploaded");Check(loader.Uploaded()==12ull*2*1024*1024*4,"every layer slice uploaded once");Check(pumps>=12,"bounded per-frame upload (several pumps)");
    loader.Start(library);Check(loader.Ready(),"same folder does not restart");

    // 3. The runtime terrain worker prepares a synthetic map: class in R,
    // region mean colour (sRGB) in GBA.
    TerrainRequest request;auto &atlas=request.atlas;atlas.width=AtlasW;atlas.height=AtlasH;atlas.count=10;atlas.cellsX=CellsX;atlas.cellsY=CellsY;atlas.borderWorld=0;
    request.base.width=AtlasW;request.base.height=AtlasH;request.base.pixels.assign(static_cast<std::size_t>(AtlasW)*AtlasH*4,255);
    std::array<std::array<double,3>,10> expectedMean{};
    for(unsigned r=0;r<10;++r){
        auto &region=atlas.regions[r];const auto origin=RegionOrigin(r);region.x=origin[0];region.y=origin[1];region.extent=Region;region.ground=static_cast<D3D11Ground>(r);
        std::snprintf(region.texture,sizeof(region.texture),"Synthetic%u",r);
        for(unsigned y=0;y<Region;++y)for(unsigned x=0;x<Region;++x){
            // Painted native look: broad blotches and fine speckle, tileable enough for the check.
            const float fx=static_cast<float>(x),fy=static_cast<float>(y);
            const float v=.82f+.22f*Smooth(fx,fy,32,r)+.1f*Smooth(fx,fy,6,r+20)-.05f;
            auto *p=request.base.pixels.data()+((static_cast<std::size_t>(region.y)+y)*AtlasW+region.x+x)*4;
            for(unsigned c=0;c<3;++c){p[c]=static_cast<std::uint8_t>(std::clamp(static_cast<int>(std::lround(Palette[r][c]*v)),0,255));expectedMean[r][c]+=Linear(p[c]);}
        }
        for(auto &m:expectedMean[r])m/=static_cast<double>(Region)*Region;
    }
    auto uvAt=[&](unsigned region,unsigned col,unsigned row,float lx,float ly){const auto o=RegionOrigin(region);
        return std::array<float,2>{(static_cast<float>(o[0])+((col%2)+lx)*64.0f)/AtlasW,(static_cast<float>(o[1])+((row%2)+1-ly)*64.0f)/AtlasH};};
    auto stripe=[](unsigned col){return col/3;};
    auto blendCell=[&](unsigned col){return col%3==2&&stripe(col)<9;};
    request.cells.resize(static_cast<std::size_t>(CellsX)*CellsY);
    const float corner[4][2]={{0,0},{1,0},{1,1},{0,1}};
    for(unsigned row=0;row<CellsY;++row)for(unsigned col=0;col<CellsX;++col){auto &cell=request.cells[static_cast<std::size_t>(row)*CellsX+col];
        for(unsigned k=0;k<4;++k){const auto a=uvAt(stripe(col),col,row,corner[k][0],corner[k][1]);const auto b=uvAt(blendCell(col)?stripe(col)+1:stripe(col),col,row,corner[k][0],corner[k][1]);
            cell.coordinates[k][0]=a[0];cell.coordinates[k][1]=a[1];cell.coordinates[k][2]=b[0];cell.coordinates[k][3]=b[1];cell.alpha[k]=blendCell(col)?corner[k][0]:0;}}
    PreparedTerrain prepared;std::atomic<bool> cancel{false};
    Check(PrepareTerrain(request,prepared,cancel),"runtime terrain worker: "+prepared.error);
    Check(prepared.width==AtlasW*2&&prepared.height==AtlasH*2,"2x atlas");
    unsigned meanChecked=0;int meanError=0;
    for(unsigned r=0;r<10;++r){const auto o=RegionOrigin(r);
        for(unsigned sample=0;sample<3;++sample){const unsigned x=(o[0]+sample*50)*2,y=(o[1]+sample*40)*2;const auto *p=prepared.pixels[4].data()+(static_cast<std::size_t>(y)*prepared.width+x)*4;
            Check(p[0]==r,"class map ground id");
            for(unsigned c=0;c<3;++c){const double m=expectedMean[r][c];const double e=m<=.0031308?m*12.92:1.055*std::pow(m,1/2.4)-.055;
                const int want=static_cast<int>(std::lround(e*255));meanError=std::max(meanError,std::abs(want-static_cast<int>(p[1+c])));++meanChecked;}}}
    Check(meanError<=1,"class map GBA is the region's linear-average native colour (sRGB)");

    auto texture=[&](const std::vector<std::uint8_t> &pixels,UINT width,UINT height,bool mips,DXGI_FORMAT format){
        D3D11_TEXTURE2D_DESC d{};d.Width=width;d.Height=height;d.MipLevels=mips?0:1;d.ArraySize=d.SampleDesc.Count=1;d.Format=format;
        d.BindFlags=D3D11_BIND_SHADER_RESOURCE|(mips?D3D11_BIND_RENDER_TARGET:0);d.MiscFlags=mips?D3D11_RESOURCE_MISC_GENERATE_MIPS:0;
        ComPtr<ID3D11Texture2D> t;ComPtr<ID3D11ShaderResourceView> v;HR(device->CreateTexture2D(&d,nullptr,t.GetAddressOf()),"atlas texture");HR(device->CreateShaderResourceView(t.Get(),nullptr,v.GetAddressOf()),"atlas view");
        context->UpdateSubresource(t.Get(),0,nullptr,pixels.data(),width*4,0);if(mips)context->GenerateMips(v.Get());return v;};
    // Same formats as TerrainUpload: RGBA8 UNORM, mips except the class map.
    auto baseView=texture(prepared.pixels[0],prepared.width,prepared.height,true,DXGI_FORMAT_R8G8B8A8_UNORM);
    auto normalView=texture(prepared.pixels[1],prepared.width,prepared.height,true,DXGI_FORMAT_R8G8B8A8_UNORM);
    auto ormView=texture(prepared.pixels[2],prepared.width,prepared.height,true,DXGI_FORMAT_R8G8B8A8_UNORM);
    auto classView=texture(prepared.pixels[4],prepared.width,prepared.height,false,DXGI_FORMAT_R8G8B8A8_UNORM);

    // 4. Production LegacyVS/LegacyPS with two 2D stages (base + blend).
    std::array<D3DRESOURCETYPE,8> types{};types[0]=types[1]=D3DRTYPE_TEXTURE;
    const auto source=BuildProgramSource(LegacyShaderSource,false,false,{},{},types,error);Check(!source.empty(),"program source");{std::ofstream dump(output/"program.hlsl",std::ios::binary);dump<<source;}
    auto compile=[&](const char *entry,const char *profile){ComPtr<ID3DBlob> code;bool hit=false;std::string message;HR(CompileCachedShader(source,entry,profile,code,message,hit),std::string(entry)+": "+message);return code;};
    auto vsCode=compile("LegacyVS","vs_5_0"),psCode=compile("LegacyPS","ps_5_0");
    ComPtr<ID3D11VertexShader> vs;ComPtr<ID3D11PixelShader> ps;
    HR(device->CreateVertexShader(vsCode->GetBufferPointer(),vsCode->GetBufferSize(),nullptr,vs.GetAddressOf()),"VS");HR(device->CreatePixelShader(psCode->GetBufferPointer(),psCode->GetBufferSize(),nullptr,ps.GetAddressOf()),"PS");
    std::vector<D3D11_INPUT_ELEMENT_DESC> elements{{"POSITION",0,DXGI_FORMAT_R32G32B32A32_FLOAT,0,0,D3D11_INPUT_PER_VERTEX_DATA,0},{"NORMAL",0,DXGI_FORMAT_R32G32B32A32_FLOAT,0,16,D3D11_INPUT_PER_VERTEX_DATA,0},{"COLOR",0,DXGI_FORMAT_R32G32B32A32_FLOAT,0,32,D3D11_INPUT_PER_VERTEX_DATA,0},{"COLOR",1,DXGI_FORMAT_R32G32B32A32_FLOAT,0,48,D3D11_INPUT_PER_VERTEX_DATA,0}};
    for(UINT i=0;i<8;++i)elements.push_back({"TEXCOORD",i,DXGI_FORMAT_R32G32B32A32_FLOAT,0,64+i*16,D3D11_INPUT_PER_VERTEX_DATA,0});
    ComPtr<ID3D11InputLayout> layout;HR(device->CreateInputLayout(elements.data(),static_cast<UINT>(elements.size()),vsCode->GetBufferPointer(),vsCode->GetBufferSize(),layout.GetAddressOf()),"input layout");
    std::vector<FixedVertex> vertices;
    for(unsigned row=0;row<CellsY;++row)for(unsigned col=0;col<CellsX;++col){
        const auto &cell=request.cells[static_cast<std::size_t>(row)*CellsX+col];FixedVertex v[4];
        for(unsigned k=0;k<4;++k){const unsigned vx=col+(corner[k][0]>0?1:0),vy=row+(corner[k][1]>0?1:0);
            v[k].position[0]=static_cast<float>(vx)*10;v[k].position[1]=static_cast<float>(vy)*10;v[k].position[2]=VertexHeight(vy)+Bump(vx,vy);
            v[k].diffuse[3]=cell.alpha[k];v[k].uv[0][0]=cell.coordinates[k][0];v[k].uv[0][1]=cell.coordinates[k][1];v[k].uv[1][0]=cell.coordinates[k][2];v[k].uv[1][1]=cell.coordinates[k][3];}
        // Corners 00,10,11,01; Zero Hour rotates a flipped cell (diagonal 10-01).
        if(FlippedCell(col,row)){for(unsigned k:{1u,2u,3u,1u,3u,0u})vertices.push_back(v[k]);}
        else for(unsigned k:{0u,1u,2u,0u,2u,3u})vertices.push_back(v[k]);
    }
    D3D11_BUFFER_DESC bd{};bd.ByteWidth=static_cast<UINT>(vertices.size()*sizeof(FixedVertex));bd.Usage=D3D11_USAGE_IMMUTABLE;bd.BindFlags=D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA init{vertices.data(),0,0};ComPtr<ID3D11Buffer> vb;HR(device->CreateBuffer(&bd,&init,vb.GetAddressOf()),"terrain vertices");
    auto constantBuffer=[&](UINT bytes){D3D11_BUFFER_DESC d{};d.ByteWidth=bytes;d.BindFlags=D3D11_BIND_CONSTANT_BUFFER;ComPtr<ID3D11Buffer> b;HR(device->CreateBuffer(&d,nullptr,b.GetAddressOf()),"constants");return b;};
    auto cb=constantBuffer(sizeof(LegacyConstants)),sb=constantBuffer(sizeof(ShadowConstants)),ib=constantBuffer(16),control=constantBuffer(16);
    {ShadowConstants shadow{};context->UpdateSubresource(sb.Get(),0,nullptr,&shadow,0,0);const UINT zero[4]{};context->UpdateSubresource(ib.Get(),0,nullptr,zero,0,0);context->UpdateSubresource(control.Get(),0,nullptr,zero,0,0);}
    D3D11_SAMPLER_DESC sd{};sd.Filter=D3D11_FILTER_ANISOTROPIC;sd.MaxAnisotropy=8;sd.AddressU=sd.AddressV=sd.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;sd.MaxLOD=D3D11_FLOAT32_MAX;
    ComPtr<ID3D11SamplerState> atlasSampler;HR(device->CreateSamplerState(&sd,atlasSampler.GetAddressOf()),"atlas sampler");
    D3D11_RASTERIZER_DESC rd{};rd.FillMode=D3D11_FILL_SOLID;rd.CullMode=D3D11_CULL_NONE;rd.DepthClipEnable=TRUE;ComPtr<ID3D11RasterizerState> raster;HR(device->CreateRasterizerState(&rd,raster.GetAddressOf()),"raster");
    D3D11_DEPTH_STENCIL_DESC dd{};dd.DepthEnable=TRUE;dd.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ALL;dd.DepthFunc=D3D11_COMPARISON_LESS;ComPtr<ID3D11DepthStencilState> depthState;HR(device->CreateDepthStencilState(&dd,depthState.GetAddressOf()),"depth state");
    NativeLightCalibrationGPU calibrated;HR(calibrated.Create(device.Get()),"native light calibration");
    EnvironmentGPU environment;bool ibl=false;
    if(argc>=4){IBL::Data data;if(IBL::Load(argv[3],data)){HR(environment.Create(device.Get(),data),"IBL");ibl=true;}}

    LegacyConstants c{};for(unsigned i=0;i<4;++i){c.world[i*5]=c.normalWorld[i*5]=1;c.materialDiffuse[i]=1;}
    c.colorStage[0][0]=2;c.colorStage[0][1]=2;c.alphaStage[0][0]=2;c.alphaStage[0][1]=2;c.colorStage[1][0]=1;c.coordinateStage[1][0]=1;
    c.modernMaterial[0]=2;c.modernMaterial[1]=.9f;
    // A Zero Hour-like afternoon sun from the south-east plus sky ambient.
    const float toward[3]={.42f,-.48f,.77f};for(unsigned i=0;i<3;++i){c.lightDirection[0][i]=-toward[i];c.lightDiffuse[0][i]=i==2?.86f:.95f;c.globalAmbient[i]=i==2?.36f:.32f;}
    c.lightPosition[0][3]=3;c.lightDiffuse[0][3]=1;
    auto render=[&](Target &t,bool on,const DirectX::XMMATRIX &view,const DirectX::XMMATRIX &projection,const DirectX::XMVECTOR &eye,unsigned repeat=1,UINT extra=0,const DirectX::XMMATRIX *previousWorldMatrix=nullptr,Target *motionTarget=nullptr,ID3D11Buffer *overrideVertices=nullptr,UINT overrideCount=0){
        DirectX::XMFLOAT4X4 m;DirectX::XMStoreFloat4x4(&m,view);std::memcpy(c.view,&m,64);DirectX::XMStoreFloat4x4(&m,projection);std::memcpy(c.projection,&m,64);
        DirectX::XMFLOAT3 e;DirectX::XMStoreFloat3(&e,eye);c.cameraWorld[0]=e.x;c.cameraWorld[1]=e.y;c.cameraWorld[2]=e.z;
        c.viewport[2]=static_cast<float>(t.width);c.viewport[3]=static_cast<float>(t.height);c.modernMaterial[3]=static_cast<float>(1|2|8|16|(on?4096:0)|extra);c.motionSettings[0]=previousWorldMatrix?1.0f:0.0f;if(previousWorldMatrix){DirectX::XMFLOAT4X4 pw,vp;DirectX::XMStoreFloat4x4(&pw,*previousWorldMatrix);std::memcpy(c.motionPreviousWorld,&pw,64);DirectX::XMStoreFloat4x4(&vp,DirectX::XMMatrixMultiply(view,projection));std::memcpy(c.motionCurrentVP,&vp,64);std::memcpy(c.motionPreviousVP,&vp,64);}
        HR(calibrated.Prepare(context.Get(),c,true),"light preparation");context->UpdateSubresource(cb.Get(),0,nullptr,&c,0,0);
        UINT stride=sizeof(FixedVertex),offset=0;auto *rawVB=overrideVertices?overrideVertices:vb.Get();context->IASetVertexBuffers(0,1,&rawVB,&stride,&offset);context->IASetInputLayout(layout.Get());context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->VSSetShader(vs.Get(),nullptr,0);context->PSSetShader(ps.Get(),nullptr,0);
        ID3D11Buffer *b0=cb.Get();context->VSSetConstantBuffers(0,1,&b0);context->PSSetConstantBuffers(0,1,&b0);
        ID3D11Buffer *b2=sb.Get();context->PSSetConstantBuffers(2,1,&b2);ID3D11Buffer *b4=ib.Get();context->VSSetConstantBuffers(4,1,&b4);context->PSSetConstantBuffers(4,1,&b4);ID3D11Buffer *b13=control.Get();context->PSSetConstantBuffers(13,1,&b13);
        ID3D11ShaderResourceView *views[10]{baseView.Get(),baseView.Get()};views[8]=normalView.Get();views[9]=ormView.Get();context->PSSetShaderResources(0,10,views);
        ID3D11ShaderResourceView *identity=classView.Get();context->PSSetShaderResources(12,1,&identity);
        ID3D11SamplerState *samplers[2]{atlasSampler.Get(),atlasSampler.Get()};context->PSSetSamplers(0,2,samplers);
        if(ibl)environment.Bind(context.Get());
        if(on)loader.Bind(context.Get());else{ID3D11ShaderResourceView *none[3]{};context->PSSetShaderResources(TerrainMaterialLibrary::AlbedoSlot,3,none);}
        auto *rtv=t.view.Get();context->OMSetRenderTargets(1,&rtv,t.depthView.Get());context->OMSetDepthStencilState(depthState.Get(),0);
        D3D11_VIEWPORT vp{0,0,static_cast<float>(t.width),static_cast<float>(t.height),0,1};context->RSSetViewports(1,&vp);context->RSSetState(raster.Get());
        if(motionTarget){ID3D11RenderTargetView *targets[7]{rtv,nullptr,nullptr,nullptr,nullptr,nullptr,motionTarget->view.Get()};context->OMSetRenderTargets(7,targets,t.depthView.Get());}for(unsigned i=0;i<repeat;++i){const float clear[4]{};context->ClearRenderTargetView(rtv,clear);if(motionTarget)context->ClearRenderTargetView(motionTarget->view.Get(),clear);context->ClearDepthStencilView(t.depthView.Get(),D3D11_CLEAR_DEPTH,1,0);
            context->Draw(overrideVertices?overrideCount:static_cast<UINT>(vertices.size()),0);}
    };
    auto read=[&](Target &t){context->CopyResource(t.staging.Get(),t.texture.Get());D3D11_MAPPED_SUBRESOURCE map{};HR(context->Map(t.staging.Get(),0,D3D11_MAP_READ,0,&map),"readback");
        std::vector<float> pixels(static_cast<std::size_t>(t.width)*t.height*4);
        for(UINT y=0;y<t.height;++y)std::memcpy(pixels.data()+static_cast<std::size_t>(y)*t.width*4,static_cast<const unsigned char*>(map.pData)+y*map.RowPitch,t.width*16);
        context->Unmap(t.staging.Get(),0);return pixels;};
    auto save=[&](const std::vector<float> &pixels,const Target &t,const char *name){std::ofstream f(output/name,std::ios::binary);const std::uint32_t header[2]{t.width,t.height};
        f.write(reinterpret_cast<const char*>(header),8);f.write(reinterpret_cast<const char*>(pixels.data()),static_cast<std::streamsize>(pixels.size()*4));Check(f.good(),"preview written");};

    // 5. Top-down orthographic render: 4 px per world unit, exact world mapping.
    const float worldW=CellsX*10.0f,worldH=CellsY*10.0f;Target ortho=MakeTarget(device.Get(),static_cast<UINT>(worldW*4),static_cast<UINT>(worldH*4));
    const auto eye=DirectX::XMVectorSet(worldW/2,worldH/2,800,1);
    const auto orthoView=DirectX::XMMatrixLookAtRH(eye,DirectX::XMVectorSet(worldW/2,worldH/2,0,1),DirectX::XMVectorSet(0,1,0,0));
    const auto orthoProjection=DirectX::XMMatrixOrthographicRH(worldW,worldH,1,2000);
    render(ortho,false,orthoView,orthoProjection,eye);const auto off=read(ortho);save(off,ortho,"ortho-off.bin");
    render(ortho,true,orthoView,orthoProjection,eye);const auto on=read(ortho);save(on,ortho,"ortho-on.bin");
    auto at=[&](const std::vector<float> &p,float wx,float wy){const auto px=static_cast<std::size_t>(wx*4),py=static_cast<std::size_t>((worldH-wy)*4);return p.data()+(py*ortho.width+px)*4;};
    auto luma=[](const float *p){return .2126f*p[0]+.7152f*p[1]+.0722f*p[2];};
    for(std::size_t i=0;i<on.size();i+=4)for(unsigned k=0;k<3;++k){Check(std::isfinite(on[i+k])&&on[i+k]>=0&&on[i+k]<65504,"finite library radiance");Check(std::isfinite(off[i+k])&&off[i+k]>=0,"finite native radiance");}
    // Unknown (stripe 0) and Wood (stripe 9) ground, flat rows, keep the native atlas path.
    double unknownDiff=0;
    for(float wy=2;wy<118;wy+=.5f)for(float wx:{1.0f,5.0f,9.0f,13.0f,18.5f,271.0f,276.0f,281.0f,286.0f,291.0f,298.5f}){const float *a=at(on,wx,wy),*b=at(off,wx,wy);for(unsigned k=0;k<3;++k)unknownDiff=std::max<double>(unknownDiff,std::abs(a[k]-b[k])/std::max(b[k],1e-3f));}
    Check(unknownDiff<=2e-3,"unknown ground equals the native atlas path");
    // Steep cliff faces (cell row 14, n.z .37) keep the native chart mapping.
    double cliffDiff=0;
    for(float wy=141;wy<149;wy+=.5f)for(float wx=1;wx<299;wx+=.5f){const float *a=at(on,wx,wy),*b=at(off,wx,wy);for(unsigned k=0;k<3;++k)cliffDiff=std::max<double>(cliffDiff,std::abs(a[k]-b[k])/std::max(b[k],1e-3f));}
    Check(cliffDiff<=2e-3,"cliff faces equal the native path");
    // Named ground: photo layers, map palette kept by the region tint.
    report<<"\"classes\":[";double worstLuma=1,worstCos=1;
    for(unsigned s=1;s<9;++s){double sumOn[3]{},sumOff[3]{},sq=0,sqOff=0;unsigned n=0;
        for(float wy=4;wy<116;wy+=.25f)for(float wx=s*30+1.0f;wx<s*30+19.0f;wx+=.25f){const float *a=at(on,wx,wy),*b=at(off,wx,wy);
            for(unsigned k=0;k<3;++k){sumOn[k]+=a[k];sumOff[k]+=b[k];}sq+=luma(a)*luma(a);sqOff+=luma(b)*luma(b);++n;}
        double dot=0,la=0,lb=0;for(unsigned k=0;k<3;++k){dot+=sumOn[k]*sumOff[k];la+=sumOn[k]*sumOn[k];lb+=sumOff[k]*sumOff[k];}
        const double lumOn=(.2126*sumOn[0]+.7152*sumOn[1]+.0722*sumOn[2])/n,lumOff=(.2126*sumOff[0]+.7152*sumOff[1]+.0722*sumOff[2])/n;
        const double ratio=lumOn/lumOff,cosine=dot/std::sqrt(la*lb),cvOn=std::sqrt(std::max(0.0,sq/n-lumOn*lumOn))/lumOn,cvOff=std::sqrt(std::max(0.0,sqOff/n-lumOff*lumOff))/lumOff;
        worstLuma=std::min(worstLuma,std::min(ratio,1/ratio));worstCos=std::min(worstCos,cosine);
        report<<(s>1?",":"")<<"{\"ground\":"<<s<<",\"luma_ratio\":"<<ratio<<",\"chroma_cos\":"<<cosine<<",\"detail_cv_on\":"<<cvOn<<",\"detail_cv_off\":"<<cvOff<<"}";
        std::cerr<<"ground "<<s<<" luma_on "<<lumOn<<" luma_off "<<lumOff<<" ratio "<<ratio<<" cos "<<cosine<<" cv "<<cvOn<<"/"<<cvOff<<"\n";
        Check(ratio>.6&&ratio<1.6,"region tint keeps the map's brightness");Check(cosine>.97,"region tint keeps the map's hue");
        Check(std::abs(lumOn-lumOff)>1e-4||cvOn!=cvOff,"library actually changes named ground");
    }
    report<<"],";
    // World-space layers: no seam where the native 2x2 tiles change inside a stripe.
    double seam=0,interior=0;unsigned seamN=0,interiorN=0;
    for(unsigned s=1;s<9;++s)for(float wy=4;wy<116;wy+=.25f){
        const float edge=s*30+10.0f;seam+=std::abs(luma(at(on,edge-.125f,wy))-luma(at(on,edge+.125f,wy)));++seamN;
        for(float wx=s*30+2.0f;wx<s*30+8.0f;wx+=1.0f){interior+=std::abs(luma(at(on,wx-.125f,wy))-luma(at(on,wx+.125f,wy)));++interiorN;}}
    const double seamRatio=(seam/seamN)/(interior/interiorN);Check(seamRatio<1.5,"no cell seams in world-space layers");

    // 5b. terrain97: production TerrainHS/TerrainDS with the library, macro relief off/on.
    auto hsCode=compile("TerrainHS","hs_5_0"),dsCode=compile("TerrainDS","ds_5_0");ComPtr<ID3D11HullShader> hs;ComPtr<ID3D11DomainShader> ds;
    HR(device->CreateHullShader(hsCode->GetBufferPointer(),hsCode->GetBufferSize(),nullptr,hs.GetAddressOf()),"terrain hull");
    HR(device->CreateDomainShader(dsCode->GetBufferPointer(),dsCode->GetBufferSize(),nullptr,ds.GetAddressOf()),"terrain domain");
    auto heightView=texture(prepared.pixels[3],prepared.width,prepared.height,true,DXGI_FORMAT_R8G8B8A8_UNORM);
    ComPtr<ID3D11Buffer> cellBuffer;ComPtr<ID3D11ShaderResourceView> cellView;
    {D3D11_BUFFER_DESC cd{};cd.ByteWidth=static_cast<UINT>(prepared.cells.size()*sizeof(D3D11TerrainCell));cd.Usage=D3D11_USAGE_IMMUTABLE;cd.BindFlags=D3D11_BIND_SHADER_RESOURCE;
     cd.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;cd.StructureByteStride=sizeof(D3D11TerrainCell);D3D11_SUBRESOURCE_DATA ci{prepared.cells.data(),0,0};
     HR(device->CreateBuffer(&cd,&ci,cellBuffer.GetAddressOf()),"terrain cells");D3D11_SHADER_RESOURCE_VIEW_DESC cv{};cv.Format=DXGI_FORMAT_UNKNOWN;cv.ViewDimension=D3D11_SRV_DIMENSION_BUFFER;cv.Buffer.NumElements=static_cast<UINT>(prepared.cells.size());
     HR(device->CreateShaderResourceView(cellBuffer.Get(),&cv,cellView.GetAddressOf()),"terrain cell view");}
    // terrain101: per-cell class records, as the terrain worker builds them.
    auto classBuffer=[&](const std::vector<D3D11TerrainCell> &cells){const auto records=BuildTerrainCellClasses(cells,CellsX,prepared.pixels[4],prepared.width,prepared.height);Check(records.size()==cells.size(),"cell class records");
        D3D11_BUFFER_DESC bd2{};bd2.ByteWidth=static_cast<UINT>(records.size()*16);bd2.Usage=D3D11_USAGE_IMMUTABLE;bd2.BindFlags=D3D11_BIND_SHADER_RESOURCE;D3D11_SUBRESOURCE_DATA i2{records.data(),0,0};ComPtr<ID3D11Buffer> b;HR(device->CreateBuffer(&bd2,&i2,b.GetAddressOf()),"class records");
        D3D11_SHADER_RESOURCE_VIEW_DESC v2{};v2.Format=DXGI_FORMAT_R32G32B32A32_UINT;v2.ViewDimension=D3D11_SRV_DIMENSION_BUFFER;v2.Buffer.NumElements=static_cast<UINT>(records.size());ComPtr<ID3D11ShaderResourceView> v;HR(device->CreateShaderResourceView(b.Get(),&v2,v.GetAddressOf()),"class record view");return v;};
    const auto classRecordView=classBuffer(prepared.cells);
    const float dimensions[4]{static_cast<float>(CellsX),static_cast<float>(CellsY),0,10};auto mapInfo=constantBuffer(16);context->UpdateSubresource(mapInfo.Get(),0,nullptr,dimensions,0,0);
    D3D11_SAMPLER_DESC ts{};ts.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR;ts.AddressU=ts.AddressV=ts.AddressW=D3D11_TEXTURE_ADDRESS_WRAP;ts.MaxLOD=D3D11_FLOAT32_MAX;
    ComPtr<ID3D11SamplerState> terrainSampler;HR(device->CreateSamplerState(&ts,terrainSampler.GetAddressOf()),"terrain sampler");
    auto renderTess=[&](Target &t,bool relief,const DirectX::XMMATRIX &view,const DirectX::XMMATRIX &projection,const DirectX::XMVECTOR &eye,unsigned repeat=1){
        render(t,true,view,projection,eye,0);
        c.modernMaterial[3]=static_cast<float>(1|2|8|16|4096|(relief?8192:0));HR(calibrated.Prepare(context.Get(),c,true),"light preparation");context->UpdateSubresource(cb.Get(),0,nullptr,&c,0,0);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST);context->HSSetShader(hs.Get(),nullptr,0);context->DSSetShader(ds.Get(),nullptr,0);
        ID3D11Buffer *b0=cb.Get(),*b2=sb.Get(),*b3=mapInfo.Get();
        for(auto set:{&ID3D11DeviceContext::HSSetConstantBuffers,&ID3D11DeviceContext::DSSetConstantBuffers}){(context.Get()->*set)(0,1,&b0);(context.Get()->*set)(2,1,&b2);(context.Get()->*set)(3,1,&b3);}
        ID3D11ShaderResourceView *terrainViews[]={heightView.Get(),classView.Get(),cellView.Get()};
        context->HSSetShaderResources(11,3,terrainViews);context->DSSetShaderResources(11,3,terrainViews);
        ID3D11SamplerState *rawSampler=terrainSampler.Get();context->DSSetSamplers(9,1,&rawSampler);loader.Bind(context.Get(),true);
        for(unsigned i=0;i<repeat;++i){const float clear[4]{-1,-1,-1,-1};context->ClearRenderTargetView(t.view.Get(),clear);context->ClearDepthStencilView(t.depthView.Get(),D3D11_CLEAR_DEPTH,1,0);
            context->Draw(static_cast<UINT>(vertices.size()),0);}
        context->HSSetShader(nullptr,nullptr,0);context->DSSetShader(nullptr,nullptr,0);context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    };
    auto readDepth=[&](Target &t){context->CopyResource(t.depthStaging.Get(),t.depth.Get());D3D11_MAPPED_SUBRESOURCE map{};HR(context->Map(t.depthStaging.Get(),0,D3D11_MAP_READ,0,&map),"depth readback");
        std::vector<float> depth(static_cast<std::size_t>(t.width)*t.height);
        for(UINT y=0;y<t.height;++y)std::memcpy(depth.data()+static_cast<std::size_t>(y)*t.width,static_cast<const unsigned char*>(map.pData)+y*map.RowPitch,t.width*4);
        context->Unmap(t.depthStaging.Get(),0);return depth;};
    // Top-down: world z = 799 - depth*1999 for the orthographic camera at z=800 (near 1, far 2000).
    renderTess(ortho,false,orthoView,orthoProjection,eye);const auto flatDepth=readDepth(ortho);const auto flatColor=read(ortho);
    renderTess(ortho,true,orthoView,orthoProjection,eye);const auto reliefDepth=readDepth(ortho);const auto reliefColor=read(ortho);save(reliefColor,ortho,"orthorelief-on.bin");save(flatColor,ortho,"orthorelief-off.bin");
    auto lift=[&](float wx,float wy){const auto i=static_cast<std::size_t>((worldH-wy)*4)*ortho.width+static_cast<std::size_t>(wx*4);return (flatDepth[i]-reliefDepth[i])*1999.0f;};
    for(std::size_t i=0;i<reliefColor.size();i+=4)Check(reliefColor[i]>=0&&std::isfinite(reliefColor[i]),"relief covers the whole map without holes (top-down)");
    double stoneMean=0,stoneMax=0,flatMax=0,unknownMax=0,anyMax=0;unsigned stoneN=0;
    for(float wy=4;wy<116;wy+=.5f){
        for(float wx=126;wx<144;wx+=.5f){const double v=std::abs(lift(wx,wy));stoneMean+=v;stoneMax=std::max(stoneMax,v);++stoneN;}
        for(float wx=216;wx<234;wx+=.5f)flatMax=std::max<double>(flatMax,std::abs(lift(wx,wy)));
        for(float wx=1;wx<14;wx+=.5f)unknownMax=std::max<double>(unknownMax,std::abs(lift(wx,wy)));
        for(float wx=1;wx<299;wx+=.5f)anyMax=std::max<double>(anyMax,std::abs(lift(wx,wy)));
    }
    stoneMean/=stoneN;
    Check(stoneMean>.05&&stoneMax>.2,"stone ground carries real macro relief");Check(stoneMax<=1.2+.2,"stone relief within its amplitude");
    Check(anyMax<=1.2+.2,"no relief beyond the largest amplitude");std::cerr<<"relief stone mean "<<stoneMean<<" max "<<stoneMax<<" any "<<anyMax<<" flat "<<flatMax<<" unknown "<<unknownMax<<"\n";
    // Asphalt keeps only its native .025 relief, resampled where neighbouring relief refines the tessellation.
    Check(flatMax<.03,"asphalt/concrete carry no library relief");Check(unknownMax<1e-3,"unknown ground keeps the native geometry");
    // Cracks: every pixel of a full-screen ground view is covered (cleared to -1), relief on and off.
    const auto crackEye=DirectX::XMVectorSet(150,10,70,1);const auto crackView=DirectX::XMMatrixLookAtRH(crackEye,DirectX::XMVectorSet(150,50,0,1),DirectX::XMVectorSet(0,0,1,0));
    const auto crackProjection=DirectX::XMMatrixPerspectiveFovRH(DirectX::XMConvertToRadians(50),16.0f/9,5,3000);
    Target wideTess=MakeTarget(device.Get(),1920,1080);unsigned holes[2]{};
    for(int relief=0;relief<2;++relief){renderTess(wideTess,relief!=0,crackView,crackProjection,crackEye);const auto pixels=read(wideTess);
        for(UINT y=wideTess.height/6;y<wideTess.height;++y)for(UINT x=0;x<wideTess.width;++x)if(pixels[(static_cast<std::size_t>(y)*wideTess.width+x)*4]<0)++holes[relief];}
    Check(holes[0]==0&&holes[1]==0,"no tessellation cracks with or without relief");
    const auto rtsEyeT=DirectX::XMVectorSet(150,-55,165,1);const auto rtsViewT=DirectX::XMMatrixLookAtRH(rtsEyeT,DirectX::XMVectorSet(150,62,0,1),DirectX::XMVectorSet(0,0,1,0));
    renderTess(wideTess,false,rtsViewT,crackProjection,rtsEyeT);save(read(wideTess),wideTess,"rtsrelief-off.bin");renderTess(wideTess,true,rtsViewT,crackProjection,rtsEyeT);save(read(wideTess),wideTess,"rtsrelief-on.bin");
    const auto lowEye=DirectX::XMVectorSet(135,-30,40,1);const auto lowView=DirectX::XMMatrixLookAtRH(lowEye,DirectX::XMVectorSet(135,60,0,1),DirectX::XMVectorSet(0,0,1,0));
    renderTess(wideTess,false,lowView,crackProjection,lowEye);save(read(wideTess),wideTess,"lowrelief-off.bin");renderTess(wideTess,true,lowView,crackProjection,lowEye);save(read(wideTess),wideTess,"lowrelief-on.bin");
    ComPtr<ID3D11Query> tessDisjoint,tessBegin,tessEnd;{D3D11_QUERY_DESC q{D3D11_QUERY_TIMESTAMP_DISJOINT,0};HR(device->CreateQuery(&q,tessDisjoint.GetAddressOf()),"disjoint");q.Query=D3D11_QUERY_TIMESTAMP;
     HR(device->CreateQuery(&q,tessBegin.GetAddressOf()),"begin");HR(device->CreateQuery(&q,tessEnd.GetAddressOf()),"end");}
    auto tessTimed=[&](bool relief){context->Begin(tessDisjoint.Get());context->End(tessBegin.Get());renderTess(wideTess,relief,rtsViewT,crackProjection,rtsEyeT,4);context->End(tessEnd.Get());context->End(tessDisjoint.Get());
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};while(context->GetData(tessDisjoint.Get(),&dj,sizeof(dj),0)==S_FALSE){}
        UINT64 t0=0,t1=0;while(context->GetData(tessBegin.Get(),&t0,8,0)==S_FALSE){}while(context->GetData(tessEnd.Get(),&t1,8,0)==S_FALSE){}
        Check(!dj.Disjoint,"stable GPU clock");return static_cast<double>(t1-t0)/static_cast<double>(dj.Frequency)*1000/4;};
    double tessOff=1e9,tessOn=1e9;for(unsigned i=0;i<30;++i){tessOff=std::min(tessOff,tessTimed(false));tessOn=std::min(tessOn,tessTimed(true));}
    report<<"\"relief\":{\"stone_mean_abs\":"<<stoneMean<<",\"stone_max_abs\":"<<stoneMax<<",\"max_abs\":"<<anyMax<<",\"flat_max_abs\":"<<flatMax<<",\"unknown_max_abs\":"<<unknownMax
          <<",\"holes_off\":"<<holes[0]<<",\"holes_on\":"<<holes[1]<<",\"gpu_ms_1080p_rts_off\":"<<tessOff<<",\"gpu_ms_1080p_rts_on\":"<<tessOn<<"},";

    // 5c. terrain97: GPU grass on the relief terrain.
    std::vector<float> elevationHeights((CellsX+1)*(CellsY+1));std::vector<std::uint8_t> elevationFlips(CellsX*CellsY);
    for(unsigned vy=0;vy<=CellsY;++vy)for(unsigned vx=0;vx<=CellsX;++vx)elevationHeights[vy*(CellsX+1)+vx]=VertexHeight(vy)+Bump(vx,vy);
    for(unsigned row=0;row<CellsY;++row)for(unsigned col=0;col<CellsX;++col)elevationFlips[row*CellsX+col]=FlippedCell(col,row)?1:0;
    auto plain=[&](UINT width,UINT height,DXGI_FORMAT format,const void *data,UINT pitch){D3D11_TEXTURE2D_DESC td{};td.Width=width;td.Height=height;td.MipLevels=td.ArraySize=td.SampleDesc.Count=1;td.Format=format;td.Usage=D3D11_USAGE_IMMUTABLE;td.BindFlags=D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA init{data,pitch,0};ComPtr<ID3D11Texture2D> t;ComPtr<ID3D11ShaderResourceView> v;HR(device->CreateTexture2D(&td,&init,t.GetAddressOf()),"elevation texture");HR(device->CreateShaderResourceView(t.Get(),nullptr,v.GetAddressOf()),"elevation view");return v;};
    auto elevationView=plain(CellsX+1,CellsY+1,DXGI_FORMAT_R32_FLOAT,elevationHeights.data(),(CellsX+1)*4),flipView=plain(CellsX,CellsY,DXGI_FORMAT_R8_UINT,elevationFlips.data(),CellsX);
    TerrainGrassGPU grass;TerrainGrassInputs grassInputs;grassInputs.height=heightView.Get();grassInputs.identity=classView.Get();grassInputs.cells=cellView.Get();grassInputs.classes=classRecordView.Get();
    grassInputs.elevation=elevationView.Get();grassInputs.flips=flipView.Get();grassInputs.atlas=baseView.Get();grassInputs.terrainSampler=terrainSampler.Get();grassInputs.library=&loader;
    grassInputs.dimensions={static_cast<float>(CellsX),static_cast<float>(CellsY),0,10};
    grassInputs.minHeight=*std::min_element(elevationHeights.begin(),elevationHeights.end());grassInputs.maxHeight=*std::max_element(elevationHeights.begin(),elevationHeights.end());
    auto drawGrass=[&](float seconds=1.0f){HR(grass.Draw(device.Get(),context.Get(),c,grassInputs,ps.Get(),seconds)==S_OK?S_OK:E_FAIL,"GPU grass draw: "+grass.Stats().lastError);};
    auto blades=[&](){std::vector<std::array<float,8>> out;HR(grass.ReadBlades(device.Get(),context.Get(),out),"grass readback");return out;};
    // Roots on the rendered (tessellated, relief) surface, grass ground only, no cliffs.
    // The top-down camera is far away: full density for these placement checks.
    const auto savedSettings=grass.settings;grass.settings.fullDensityDistance=1e5f;grass.settings.maxDistance=5000;
    renderTess(ortho,true,orthoView,orthoProjection,eye);const auto surfaceDepth=readDepth(ortho);drawGrass();const auto topBlades=blades();
    Check(topBlades.size()>2000,"grass blades placed");
    std::vector<double> rootError;double xMin=1e9,xMax=-1e9;unsigned onCliff=0;
    for(const auto &b:topBlades){
        xMin=std::min<double>(xMin,b[0]);xMax=std::max<double>(xMax,b[0]);if(b[1]>140.5f&&b[1]<149.5f)++onCliff;
        // Depth is read at the pixel centre (0.25-unit texels): measure off the steep rows,
        // where that offset alone would move the height by up to .15.
        const auto px=static_cast<std::size_t>(b[0]*4),py=static_cast<std::size_t>((worldH-b[1])*4);if(px>=ortho.width||py>=ortho.height||b[1]>=118)continue;
        rootError.push_back(std::abs(b[2]-(799.0-surfaceDepth[py*ortho.width+px]*1999.0)));
    }
    std::sort(rootError.begin(),rootError.end());const double rootMedian=rootError[rootError.size()/2],rootP99=rootError[rootError.size()*99/100];
    std::cerr<<"roots n "<<rootError.size()<<" median "<<rootMedian<<" p90 "<<rootError[rootError.size()*9/10]<<" p99 "<<rootP99<<" max "<<rootError.back()<<" x "<<xMin<<".."<<xMax<<" cliff "<<onCliff<<"\n";
    {std::ofstream dump(output/"grass-roots.csv");for(const auto &b:topBlades){const auto px=static_cast<std::size_t>(b[0]*4),py=static_cast<std::size_t>((worldH-b[1])*4);if(px<ortho.width&&py<ortho.height)dump<<b[0]<<","<<b[1]<<","<<b[2]<<","<<(799.0-surfaceDepth[py*ortho.width+px]*1999.0)<<"\n";}}
    Check(xMin>=20&&xMax<=60,"grass only on grass ground (stripe and its blends)");Check(onCliff==0,"no grass on cliff faces");
    Check(rootMedian<.02&&rootP99<.06,"grass roots on the rendered terrain surface (flipped cells and relief included)");
    // Stable placement: a fixed world grid, so a moved camera keeps every shared blade.
    renderTess(ortho,true,orthoView,orthoProjection,eye);drawGrass();const auto stableA=blades();
    const auto movedEye=DirectX::XMVectorSet(worldW/2+20,worldH/2+10,800,1);const auto movedView=DirectX::XMMatrixLookAtRH(movedEye,DirectX::XMVectorSet(worldW/2+20,worldH/2+10,0,1),DirectX::XMVectorSet(0,1,0,0));
    renderTess(ortho,true,movedView,orthoProjection,movedEye);drawGrass();const auto stableB=blades();grass.settings=savedSettings;
    std::set<std::pair<int,int>> setB;for(const auto &b:stableB)setB.insert({static_cast<int>(std::lround(b[0]*1000)),static_cast<int>(std::lround(b[1]*1000))});
    unsigned shared=0,kept=0;for(const auto &b:stableA)if(b[0]>23&&b[0]<worldW-3&&b[1]>13&&b[1]<worldH-3){++shared;kept+=setB.count({static_cast<int>(std::lround(b[0]*1000)),static_cast<int>(std::lround(b[1]*1000))})?1u:0u;}
    Check(shared>1000&&kept==shared,"grass placement stable when the camera moves");
    // Brightness: blades shade like the ground (no dark spikes).
    Target grassView=MakeTarget(device.Get(),1920,1080);
    const auto grassEye=DirectX::XMVectorSet(40,-25,60,1);const auto grassLook=DirectX::XMMatrixLookAtRH(grassEye,DirectX::XMVectorSet(40,60,0,1),DirectX::XMVectorSet(0,0,1,0));
    renderTess(grassView,true,grassLook,crackProjection,grassEye);const auto bare=read(grassView);save(bare,grassView,"grassclose-off.bin");
    drawGrass();const auto grown=read(grassView);save(grown,grassView,"grassclose-on.bin");
    double bareLuma=0,grownLuma=0;unsigned changed=0,dark=0;
    for(std::size_t i=0;i<bare.size();i+=4){if(bare[i]<0)continue;const float a=luma(&bare[i]),b=luma(&grown[i]);if(std::abs(a-b)<1e-5f)continue;++changed;bareLuma+=a;grownLuma+=b;if(b<.35f*a)++dark;}
    Check(changed>20000,"grass covers the grass ground");const double brightness=grownLuma/std::max(bareLuma,1e-9),darkFraction=double(dark)/changed;
    std::cerr<<"grass blades "<<topBlades.size()<<" root median "<<rootMedian<<" p99 "<<rootP99<<" stable "<<kept<<"/"<<shared<<" brightness "<<brightness<<" dark "<<darkFraction<<"\n";
    Check(brightness>.7&&brightness<1.6,"grass is as bright as its ground (no dark spikes)");Check(darkFraction<.05,"almost no dark blade pixels");
    renderTess(wideTess,true,rtsViewT,crackProjection,rtsEyeT);save(read(wideTess),wideTess,"grassrts-off.bin");drawGrass();save(read(wideTess),wideTess,"grassrts-on.bin");
    const auto rtsBlades=blades().size();
    ComPtr<ID3D11Query> grassDisjoint,grassBegin,grassEnd;{D3D11_QUERY_DESC q{D3D11_QUERY_TIMESTAMP_DISJOINT,0};HR(device->CreateQuery(&q,grassDisjoint.GetAddressOf()),"disjoint");q.Query=D3D11_QUERY_TIMESTAMP;
     HR(device->CreateQuery(&q,grassBegin.GetAddressOf()),"begin");HR(device->CreateQuery(&q,grassEnd.GetAddressOf()),"end");}
    double grassMs=1e9;
    for(unsigned i=0;i<20;++i){renderTess(wideTess,true,rtsViewT,crackProjection,rtsEyeT);context->Begin(grassDisjoint.Get());context->End(grassBegin.Get());drawGrass();context->End(grassEnd.Get());context->End(grassDisjoint.Get());
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};while(context->GetData(grassDisjoint.Get(),&dj,sizeof(dj),0)==S_FALSE){}
        UINT64 t0=0,t1=0;while(context->GetData(grassBegin.Get(),&t0,8,0)==S_FALSE){}while(context->GetData(grassEnd.Get(),&t1,8,0)==S_FALSE){}
        Check(!dj.Disjoint,"stable GPU clock");grassMs=std::min(grassMs,static_cast<double>(t1-t0)/static_cast<double>(dj.Frequency)*1000);}
    report<<"\"grass\":{\"blades_top\":"<<topBlades.size()<<",\"blades_rts\":"<<rtsBlades<<",\"root_median\":"<<rootMedian<<",\"root_p99\":"<<rootP99<<",\"stable\":"<<kept<<",\"shared\":"<<shared
          <<",\"brightness\":"<<brightness<<",\"dark_fraction\":"<<darkFraction<<",\"gpu_ms_1080p_rts\":"<<grassMs<<"},";

    // 5d. terrain98: vehicles flatten grass; tracks recover; low rotors wash; high aircraft do not.
    auto groundAt=[&](float x,float y){const unsigned vx=std::min(static_cast<unsigned>(x/10),CellsX-1),vy=std::min(static_cast<unsigned>(y/10),CellsY-1);
        return elevationHeights[vy*(CellsX+1)+vx];};
    auto interactionAt=[&](float x,float y){std::array<float,4> v{};HR(grass.ReadInteraction(device.Get(),context.Get(),x,y,v),"interaction readback");return v;};
    float clock=10;
    renderTess(grassView,true,grassLook,crackProjection,grassEye);
    grass.SetVehicles({{40,60,groundAt(40,60)+1.5f,6.5f},{45,30,groundAt(45,30)+20,6.5f},{35,90,groundAt(35,90)+60,6.5f}});
    drawGrass(clock);clock+=.05f;drawGrass(clock);
    const auto under=interactionAt(40,60),beside=interactionAt(40,60+15),rotor=interactionAt(45,30),jet=interactionAt(35,90);
    Check(under[2]>.9f,"a ground vehicle flattens the grass under it");Check(beside[2]<.05f,"grass 15 units away stands");
    Check(rotor[2]>.25f&&rotor[2]<.45f,"rotor wash of a helicopter 20 units up lays grass partly");Check(jet[2]<1e-3f,"aircraft 60 units up leave grass standing");
    const auto pushed=interactionAt(44,60);Check(pushed[0]>.9f&&std::abs(pushed[1])<.2f,"grass is pushed away from the vehicle");
    grass.SetVehicles({});for(unsigned i=0;i<24;++i){clock+=.25f;drawGrass(clock);}
    const auto recovered=interactionAt(40,60);Check(recovered[2]>.3f&&recovered[2]<.45f,"tracks recover with a 6 s time constant");
    for(unsigned i=0;i<80;++i){clock+=.25f;drawGrass(clock);}Check(interactionAt(40,60)[2]<.05f,"tracks fade out");
    auto renderShaded=[&](){renderTess(grassView,true,grassLook,crackProjection,grassEye,0);
        c.modernMaterial[3]=static_cast<float>(1|2|8|16|4096|8192|16384|32768);HR(calibrated.Prepare(context.Get(),c,true),"light preparation");context->UpdateSubresource(cb.Get(),0,nullptr,&c,0,0);
        ID3D11ShaderResourceView *cellPS=cellView.Get();context->PSSetShaderResources(13,1,&cellPS);ID3D11Buffer *mapPS=mapInfo.Get();context->PSSetConstantBuffers(3,1,&mapPS);grass.BindPixel(context.Get());
        const float clear[4]{-1,-1,-1,-1};context->ClearRenderTargetView(grassView.view.Get(),clear);context->ClearDepthStencilView(grassView.depthView.Get(),D3D11_CLEAR_DEPTH,1,0);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST);context->HSSetShader(hs.Get(),nullptr,0);context->DSSetShader(ds.Get(),nullptr,0);
        context->Draw(static_cast<UINT>(vertices.size()),0);context->HSSetShader(nullptr,nullptr,0);context->DSSetShader(nullptr,nullptr,0);context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);};
    // A tank crossing the meadow leaves a flattened track (preview).
    renderTess(grassView,true,grassLook,crackProjection,grassEye);drawGrass(clock);renderShaded();drawGrass(clock);save(read(grassView),grassView,"grasstrack-off.bin");
    for(float x=22;x<=58;x+=1.5f){grass.SetVehicles({{x,45+.15f*(x-22),groundAt(x,45)+1.5f,6.5f}});clock+=.1f;renderShaded();drawGrass(clock);}
    grass.SetVehicles({});clock+=.1f;renderShaded();drawGrass(clock);save(read(grassView),grassView,"grasstrack-on.bin");
    // Grass shadows: the terrain pixel shader casts the shadows of the placed tufts
    // analytically (flag 32768); cascade texels are wider than a blade in game.
    for(unsigned i=0;i<80;++i){clock+=.25f;drawGrass(clock);} // the track has recovered
    renderTess(grassView,true,grassLook,crackProjection,grassEye);drawGrass(clock);const auto unshadowed=read(grassView);
    renderShaded();
    drawGrass(clock);const auto shadowed=read(grassView);save(unshadowed,grassView,"grassshadow-off.bin");save(shadowed,grassView,"grassshadow-on.bin");
    {ID3D11ShaderResourceView *none=nullptr;context->PSSetShaderResources(13,1,&none);}
    double litLuma=0,shadeLuma=0;unsigned shaded=0,meadow=0;bool finiteShadow=true;
    for(std::size_t i=0;i<shadowed.size();i+=4){if(unshadowed[i]<0)continue;++meadow;finiteShadow&=std::isfinite(shadowed[i])&&shadowed[i]>=0;
        const float a=luma(&unshadowed[i]),b=luma(&shadowed[i]);if(a-b<a*1e-3f)continue;++shaded;litLuma+=a;shadeLuma+=b;}
    const double grassShade=shadeLuma/std::max(litLuma,1e-9),shadedFraction=double(shaded)/std::max(meadow,1u);
    std::cerr<<"grass interaction under "<<under[2]<<" beside "<<beside[2]<<" rotor "<<rotor[2]<<" jet "<<jet[2]<<" recovered "<<recovered[2]<<" shaded "<<shaded<<" ("<<shadedFraction<<") ratio "<<grassShade<<"\n";
    Check(finiteShadow,"finite radiance with grass shadows");Check(shaded>20000,"grass tufts shade the ground");Check(grassShade>.45&&grassShade<.97,"grass shadows darken softly, never black");
    // terrain99: GPU cost of the analytic grass shadow on the full-screen RTS terrain view.
    auto drawTerrainFlags=[&](Target &t,const DirectX::XMMATRIX &view,const DirectX::XMVECTOR &eyeAt,UINT flags){renderTess(t,true,view,crackProjection,eyeAt,0);
        c.modernMaterial[3]=static_cast<float>(flags);HR(calibrated.Prepare(context.Get(),c,true),"light preparation");context->UpdateSubresource(cb.Get(),0,nullptr,&c,0,0);
        ID3D11ShaderResourceView *cellPS=cellView.Get();context->PSSetShaderResources(13,1,&cellPS);ID3D11ShaderResourceView *recordPS=classRecordView.Get();context->PSSetShaderResources(TerrainGrassGPU::ClassesSlot,1,&recordPS);ID3D11Buffer *mapPS=mapInfo.Get();context->PSSetConstantBuffers(3,1,&mapPS);grass.BindPixel(context.Get());
        const float clear[4]{-1,-1,-1,-1};context->ClearRenderTargetView(t.view.Get(),clear);context->ClearDepthStencilView(t.depthView.Get(),D3D11_CLEAR_DEPTH,1,0);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST);context->HSSetShader(hs.Get(),nullptr,0);context->DSSetShader(ds.Get(),nullptr,0);
        context->Draw(static_cast<UINT>(vertices.size()),0);context->HSSetShader(nullptr,nullptr,0);context->DSSetShader(nullptr,nullptr,0);context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);};
    auto timeTerrain=[&](UINT flags){double best=1e9;for(unsigned i=0;i<20;++i){context->Begin(grassDisjoint.Get());context->End(grassBegin.Get());drawTerrainFlags(wideTess,crackView,crackEye,flags);
        context->End(grassEnd.Get());context->End(grassDisjoint.Get());D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};while(context->GetData(grassDisjoint.Get(),&dj,sizeof(dj),0)==S_FALSE){}
        UINT64 t0=0,t1=0;while(context->GetData(grassBegin.Get(),&t0,8,0)==S_FALSE){}while(context->GetData(grassEnd.Get(),&t1,8,0)==S_FALSE){}
        Check(!dj.Disjoint,"stable GPU clock");best=std::min(best,static_cast<double>(t1-t0)/static_cast<double>(dj.Frequency)*1000);}return best;};
    // Profiling only (GENZH_LIBRARY_PROFILE=<folder>): each <folder>/*.hlsl is a full
    // program variant; its LegacyPS is timed on this view with and without the ground
    // field, then the run ends (no other check is affected when the variable is unset).
    {wchar_t folder[1024]{};if(GetEnvironmentVariableW(L"GENZH_LIBRARY_PROFILE",folder,1024)){
        const auto keep=ps;
        for(const auto &entry:std::filesystem::directory_iterator(folder)){if(entry.path().extension()!=".hlsl")continue;
            std::ifstream in(entry.path(),std::ios::binary);std::stringstream text;text<<in.rdbuf();ComPtr<ID3DBlob> code;bool hit=false;std::string message;
            HR(CompileCachedShader(text.str(),"LegacyPS","ps_5_0",code,message,hit),entry.path().filename().string()+": "+message);
            ComPtr<ID3D11PixelShader> variant;HR(device->CreatePixelShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,variant.GetAddressOf()),"variant PS");ps=variant;
            const double profiledOff=timeTerrain(1|2|8|16|4096|8192|16384|32768),withField=timeTerrain(1|2|8|16|4096|8192|16384|32768|65536);
            std::cout<<entry.path().filename().string()<<" off "<<profiledOff<<" field "<<withField<<"\n";}
        ps=keep;return 0;}}
    const double terrainPlainMs=timeTerrain(1|2|8|16|4096|8192|16384),terrainShadowMs=timeTerrain(1|2|8|16|4096|8192|16384|32768);
    {ID3D11ShaderResourceView *none=nullptr;context->PSSetShaderResources(13,1,&none);}
    std::cerr<<"terrain 1080p full-screen ground: "<<terrainPlainMs<<" ms, with grass shadows "<<terrainShadowMs<<" ms\n";
    report<<"\"grass_interaction\":{\"under\":"<<under[2]<<",\"beside\":"<<beside[2]<<",\"rotor\":"<<rotor[2]<<",\"aircraft\":"<<jet[2]<<",\"recovered_6s\":"<<recovered[2]
          <<"},\"grass_shadow\":{\"shaded_pixels\":"<<shaded<<",\"shaded_fraction\":"<<shadedFraction<<",\"shaded_luma_ratio\":"<<grassShade<<",\"gpu_ms_1080p_terrain\":"<<terrainPlainMs<<",\"gpu_ms_1080p_terrain_grass_shadow\":"<<terrainShadowMs<<"},";

    // 6. RTS perspective previews and GPU cost at 1920x1080.
    Target wide=MakeTarget(device.Get(),1920,1080);
    const auto rtsEye=DirectX::XMVectorSet(150,-55,165,1);const auto rtsView=DirectX::XMMatrixLookAtRH(rtsEye,DirectX::XMVectorSet(150,62,0,1),DirectX::XMVectorSet(0,0,1,0));
    const auto rtsProjection=DirectX::XMMatrixPerspectiveFovRH(DirectX::XMConvertToRadians(50),16.0f/9,5,3000);
    render(wide,false,rtsView,rtsProjection,rtsEye);save(read(wide),wide,"rts-off.bin");render(wide,true,rtsView,rtsProjection,rtsEye);save(read(wide),wide,"rts-on.bin");
    const auto closeEye=DirectX::XMVectorSet(105,18,48,1);const auto closeView=DirectX::XMMatrixLookAtRH(closeEye,DirectX::XMVectorSet(105,70,0,1),DirectX::XMVectorSet(0,0,1,0));
    render(wide,false,closeView,rtsProjection,closeEye);save(read(wide),wide,"close-off.bin");render(wide,true,closeView,rtsProjection,closeEye);save(read(wide),wide,"close-on.bin");
    // Full-screen terrain: camera inside the field so every pixel is ground.
    const auto fillEye=DirectX::XMVectorSet(150,10,70,1);const auto fillView=DirectX::XMMatrixLookAtRH(fillEye,DirectX::XMVectorSet(150,50,0,1),DirectX::XMVectorSet(0,0,1,0));
    ComPtr<ID3D11Query> disjoint,begin,end;D3D11_QUERY_DESC qd{D3D11_QUERY_TIMESTAMP_DISJOINT,0};HR(device->CreateQuery(&qd,disjoint.GetAddressOf()),"disjoint");qd.Query=D3D11_QUERY_TIMESTAMP;
    HR(device->CreateQuery(&qd,begin.GetAddressOf()),"begin");HR(device->CreateQuery(&qd,end.GetAddressOf()),"end");
    auto timed=[&](bool on){context->Begin(disjoint.Get());context->End(begin.Get());render(wide,on,fillView,rtsProjection,fillEye,4);context->End(end.Get());context->End(disjoint.Get());
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};while(context->GetData(disjoint.Get(),&dj,sizeof(dj),0)==S_FALSE){}
        UINT64 t0=0,t1=0;while(context->GetData(begin.Get(),&t0,8,0)==S_FALSE){}while(context->GetData(end.Get(),&t1,8,0)==S_FALSE){}
        Check(!dj.Disjoint,"stable GPU clock");return static_cast<double>(t1-t0)/static_cast<double>(dj.Frequency)*1000/4;};
    double msOn=1e9,msOff=1e9;for(unsigned i=0;i<30;++i){msOff=std::min(msOff,timed(false));msOn=std::min(msOn,timed(true));}
    {const auto fill=read(wide);unsigned covered=0;for(std::size_t i=0;i<fill.size();i+=4)if(fill[i]+fill[i+1]+fill[i+2]>0)++covered;Check(covered>fill.size()/4*95/100,"timing view is full-screen ground");}

    // 7. terrain101: continuous ground field (65536) against the per-cell class staircase.
    // A grass meadow painted on dirt as Zero Hour's brush does: whole cells inside,
    // border cells blend with corner alpha 1 inside / 0 outside (staircase + 45 degree cuts).
    std::vector<D3D11TerrainCell> stairCells(static_cast<std::size_t>(CellsX)*CellsY);std::vector<FixedVertex> stairVertices;
    auto inMeadow=[](float x,float y){const float dx=x-150,dy=(y-80)*1.25f;return dx*dx+dy*dy<52.0f*52.0f;};
    for(unsigned row=0;row<CellsY;++row)for(unsigned col=0;col<CellsX;++col){auto &cell=stairCells[static_cast<std::size_t>(row)*CellsX+col];bool in[4];unsigned count=0;
        for(unsigned k=0;k<4;++k){in[k]=inMeadow((col+corner[k][0])*10,(row+corner[k][1])*10);count+=in[k]?1u:0u;}
        const unsigned base=count==4?1u:2u;
        for(unsigned k=0;k<4;++k){const auto a=uvAt(base,col,row,corner[k][0],corner[k][1]),b=uvAt(1,col,row,corner[k][0],corner[k][1]);
            cell.coordinates[k][0]=a[0];cell.coordinates[k][1]=a[1];cell.coordinates[k][2]=b[0];cell.coordinates[k][3]=b[1];cell.alpha[k]=count>0&&count<4&&in[k]?1.0f:0.0f;}
        FixedVertex v[4];
        for(unsigned k=0;k<4;++k){v[k].position[0]=(col+corner[k][0])*10;v[k].position[1]=(row+corner[k][1])*10;v[k].position[2]=0;v[k].diffuse[3]=cell.alpha[k];
            v[k].uv[0][0]=cell.coordinates[k][0];v[k].uv[0][1]=cell.coordinates[k][1];v[k].uv[1][0]=cell.coordinates[k][2];v[k].uv[1][1]=cell.coordinates[k][3];}
        for(unsigned k:{0u,1u,2u,0u,2u,3u})stairVertices.push_back(v[k]);}
    ComPtr<ID3D11Buffer> stairVB,stairCellBuffer;ComPtr<ID3D11ShaderResourceView> stairCellView;
    {D3D11_BUFFER_DESC d{};d.ByteWidth=static_cast<UINT>(stairVertices.size()*sizeof(FixedVertex));d.Usage=D3D11_USAGE_IMMUTABLE;d.BindFlags=D3D11_BIND_VERTEX_BUFFER;D3D11_SUBRESOURCE_DATA i{stairVertices.data(),0,0};HR(device->CreateBuffer(&d,&i,stairVB.GetAddressOf()),"meadow vertices");
     D3D11_BUFFER_DESC cd{};cd.ByteWidth=static_cast<UINT>(stairCells.size()*sizeof(D3D11TerrainCell));cd.Usage=D3D11_USAGE_IMMUTABLE;cd.BindFlags=D3D11_BIND_SHADER_RESOURCE;cd.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;cd.StructureByteStride=sizeof(D3D11TerrainCell);
     D3D11_SUBRESOURCE_DATA ci{stairCells.data(),0,0};HR(device->CreateBuffer(&cd,&ci,stairCellBuffer.GetAddressOf()),"meadow cells");
     D3D11_SHADER_RESOURCE_VIEW_DESC cv{};cv.Format=DXGI_FORMAT_UNKNOWN;cv.ViewDimension=D3D11_SRV_DIMENSION_BUFFER;cv.Buffer.NumElements=static_cast<UINT>(stairCells.size());HR(device->CreateShaderResourceView(stairCellBuffer.Get(),&cv,stairCellView.GetAddressOf()),"meadow cell view");}
    const auto stairRecordView=classBuffer(stairCells);
    auto drawMeadow=[&](Target &t,bool field,const DirectX::XMMATRIX &view,const DirectX::XMMATRIX &projection,const DirectX::XMVECTOR &at,UINT extra=0){render(t,true,view,projection,at,0);
        c.modernMaterial[3]=static_cast<float>(1|2|8|16|4096|(field?65536:0)|extra);HR(calibrated.Prepare(context.Get(),c,true),"light preparation");context->UpdateSubresource(cb.Get(),0,nullptr,&c,0,0);
        ID3D11ShaderResourceView *cellPS=stairCellView.Get();context->PSSetShaderResources(13,1,&cellPS);ID3D11Buffer *mapPS=mapInfo.Get();context->PSSetConstantBuffers(3,1,&mapPS);
        ID3D11ShaderResourceView *recordPS=stairRecordView.Get();context->PSSetShaderResources(TerrainGrassGPU::ClassesSlot,1,&recordPS);
        UINT stride=sizeof(FixedVertex),offset=0;auto *raw=stairVB.Get();context->IASetVertexBuffers(0,1,&raw,&stride,&offset);
        const float clear[4]{-1,-1,-1,-1};context->ClearRenderTargetView(t.view.Get(),clear);context->ClearDepthStencilView(t.depthView.Get(),D3D11_CLEAR_DEPTH,1,0);
        context->Draw(static_cast<UINT>(stairVertices.size()),0);ID3D11ShaderResourceView *none=nullptr;context->PSSetShaderResources(13,1,&none);};
    // Grass/dirt mask: greenness blurred over 1.5 units (texture detail removed),
    // split halfway between the two grounds' interior greenness.
    struct Mask {std::vector<float> green;std::vector<std::uint8_t> grass;double area=0;};
    auto classify=[&](const std::vector<float> &p){Mask m;const UINT w=ortho.width,h=ortho.height;std::vector<float> g(static_cast<std::size_t>(w)*h),row(g.size());
        for(std::size_t i=0;i<g.size();++i){const float s=p[i*4]+p[i*4+1]+p[i*4+2];g[i]=s>0?p[i*4+1]/s:0;}
        const int r=6;for(UINT y=0;y<h;++y)for(UINT x=0;x<w;++x){float s=0;int n=0;for(int k=-r;k<=r;++k){const int xx=static_cast<int>(x)+k;if(xx>=0&&xx<static_cast<int>(w)){s+=g[y*w+xx];++n;}}row[y*w+x]=s/n;}
        m.green.resize(g.size());for(UINT y=0;y<h;++y)for(UINT x=0;x<w;++x){float s=0;int n=0;for(int k=-r;k<=r;++k){const int yy=static_cast<int>(y)+k;if(yy>=0&&yy<static_cast<int>(h)){s+=row[yy*w+x];++n;}}m.green[y*w+x]=s/n;}
        auto meanIn=[&](float x0,float y0){double s=0;unsigned n=0;for(float wy=y0;wy<y0+10;wy+=.25f)for(float wx=x0;wx<x0+10;wx+=.25f){s+=m.green[static_cast<std::size_t>((worldH-wy)*4)*w+static_cast<std::size_t>(wx*4)];++n;}return s/n;};
        const double split=(meanIn(145,75)+meanIn(15,10))/2;m.grass.resize(g.size());for(std::size_t i=0;i<g.size();++i){m.grass[i]=m.green[i]>split?1:0;m.area+=m.grass[i];}m.area/=static_cast<double>(g.size());return m;};
    // Border orientation: share of border pixels whose gradient lies within 6 degrees of
    // a multiple of 45 (the cell staircase and its corner-alpha cuts); ~27% at random.
    auto gridLocked=[&](const Mask &m){const UINT w=ortho.width,h=ortho.height;unsigned border=0,locked=0;
        for(UINT y=8;y+8<h;++y)for(UINT x=8;x+8<w;++x){const std::size_t i=static_cast<std::size_t>(y)*w+x;if(m.grass[i]==m.grass[i+1]&&m.grass[i]==m.grass[i+w])continue;
            const auto G=[&](int dx,int dy){return m.green[(y+dy)*w+x+dx];};
            const double gx=(G(1,-1)+2*G(1,0)+G(1,1))-(G(-1,-1)+2*G(-1,0)+G(-1,1)),gy=(G(-1,1)+2*G(0,1)+G(1,1))-(G(-1,-1)+2*G(0,-1)+G(1,-1));
            if(gx*gx+gy*gy<1e-12)continue;double a=std::fmod(std::atan2(gy,gx)*180/3.14159265358979+360,45.0);++border;if(a<6||a>39)++locked;}
        return std::pair<double,unsigned>{border?double(locked)/border:1.0,border};};
    drawMeadow(ortho,false,orthoView,orthoProjection,eye);const auto cellMeadow=read(ortho);save(cellMeadow,ortho,"meadow-off.bin");
    drawMeadow(ortho,true,orthoView,orthoProjection,eye);const auto fieldMeadow=read(ortho);save(fieldMeadow,ortho,"meadow-on.bin");
    bool meadowFinite=true;for(std::size_t i=0;i<fieldMeadow.size();i+=4)for(unsigned k=0;k<3;++k)meadowFinite&=std::isfinite(fieldMeadow[i+k])&&fieldMeadow[i+k]>=0&&fieldMeadow[i+k]<65504;
    Check(meadowFinite,"finite radiance with the ground field");
    const auto cellMask=classify(cellMeadow),fieldMask=classify(fieldMeadow);const auto cellLock=gridLocked(cellMask),fieldLock=gridLocked(fieldMask);
    std::cerr<<"meadow grid-locked border cell "<<cellLock.first<<" ("<<cellLock.second<<") field "<<fieldLock.first<<" ("<<fieldLock.second<<"), grass area cell "<<cellMask.area<<" field "<<fieldMask.area<<"\n";
    Check(cellLock.first>.45,"fixture: the per-cell path draws a grid-locked staircase");
    Check(fieldLock.first<.35&&fieldLock.first<cellLock.first*.7,"ground field borders are not locked to the cell grid (random ~.27)");
    Check(std::abs(fieldMask.area-cellMask.area)<.06*cellMask.area,"ground field keeps the painted grass area");
    // Interiors are unchanged ground (same layers and region tint): far from the border.
    double coreDiff=0;unsigned coreN=0;
    for(float wy=70;wy<90;wy+=.5f)for(float wx:{140.0f,150.0f,160.0f,20.0f,30.0f,270.0f}){const float *a=at(fieldMeadow,wx,wy),*b=at(cellMeadow,wx,wy);coreDiff+=std::abs(luma(a)-luma(b))/std::max(luma(b),1e-4f);++coreN;}
    coreDiff/=coreN;Check(coreDiff<.02,"ground field leaves class interiors unchanged");
    // The GPU grass grows on the very meadow the ground draws (same field function).
    TerrainGrassInputs meadowInputs=grassInputs;meadowInputs.cells=stairCellView.Get();meadowInputs.classes=stairRecordView.Get();grass.settings.fullDensityDistance=1e5f;grass.settings.maxDistance=5000;
    auto meadowBlades=[&](bool field){drawMeadow(ortho,field,orthoView,orthoProjection,eye);c.modernMaterial[3]=static_cast<float>(1|2|8|16|4096|16384|(field?65536:0));
        HR(grass.Draw(device.Get(),context.Get(),c,meadowInputs,ps.Get(),1.0f)==S_OK?S_OK:E_FAIL,"meadow grass: "+grass.Stats().lastError);return blades();};
    auto agreement=[&](const std::vector<std::array<float,8>> &roots,const Mask &m){unsigned n=0,on=0;for(const auto &b:roots){const auto px=static_cast<std::size_t>(b[0]*4),py=static_cast<std::size_t>((worldH-b[1])*4);
        if(px>=ortho.width||py>=ortho.height)continue;++n;on+=m.grass[py*ortho.width+px];}return std::pair<double,unsigned>{n?double(on)/n:0,n};};
    const auto cellRoots=agreement(meadowBlades(false),cellMask),fieldRoots=agreement(meadowBlades(true),fieldMask);grass.settings=savedSettings;
    std::cerr<<"meadow grass on drawn grass: cell "<<cellRoots.first<<" ("<<cellRoots.second<<") field "<<fieldRoots.first<<" ("<<fieldRoots.second<<")\n";
    Check(fieldRoots.second>2000&&fieldRoots.first>.97,"grass follows the ground field's meadow");
    drawMeadow(wide,false,rtsView,rtsProjection,rtsEye);save(read(wide),wide,"meadowrts-off.bin");drawMeadow(wide,true,rtsView,rtsProjection,rtsEye);save(read(wide),wide,"meadowrts-on.bin");
    // A map-like view (broad classes, one meadow border) filling the screen: cell path against field.
    const auto meadowEye=DirectX::XMVectorSet(150,30,60,1);const auto meadowView=DirectX::XMMatrixLookAtRH(meadowEye,DirectX::XMVectorSet(150,75,0,1),DirectX::XMVectorSet(0,0,1,0));
    auto timeMeadow=[&](bool field){double best=1e9;for(unsigned i=0;i<20;++i){context->Begin(grassDisjoint.Get());context->End(grassBegin.Get());drawMeadow(wideTess,field,meadowView,crackProjection,meadowEye);
        context->End(grassEnd.Get());context->End(grassDisjoint.Get());D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};while(context->GetData(grassDisjoint.Get(),&dj,sizeof(dj),0)==S_FALSE){}
        UINT64 t0=0,t1=0;while(context->GetData(grassBegin.Get(),&t0,8,0)==S_FALSE){}while(context->GetData(grassEnd.Get(),&t1,8,0)==S_FALSE){}
        Check(!dj.Disjoint,"stable GPU clock");best=std::min(best,static_cast<double>(t1-t0)/static_cast<double>(dj.Frequency)*1000);}return best;};
    double meadowCellMs=1e9,meadowFieldMs=1e9;for(unsigned round=0;round<6;++round){meadowCellMs=std::min(meadowCellMs,timeMeadow(false));meadowFieldMs=std::min(meadowFieldMs,timeMeadow(true));}
    {const auto fill=read(wideTess);unsigned covered=0;for(std::size_t i=0;i<fill.size();i+=4)if(fill[i]>=0)++covered;Check(covered>fill.size()/4*95/100,"meadow timing view is full-screen ground");}
    std::cerr<<"meadow view 1080p: cell "<<meadowCellMs<<" ms, field "<<meadowFieldMs<<" ms\n";
    // Interleaved rounds: the field's cost against the same view without it (clock drift cancels).
    double fieldOffMs=1e9,terrainFieldMs=1e9;for(unsigned round=0;round<6;++round){fieldOffMs=std::min(fieldOffMs,timeTerrain(1|2|8|16|4096|8192|16384|32768));terrainFieldMs=std::min(terrainFieldMs,timeTerrain(1|2|8|16|4096|8192|16384|32768|65536));}{ID3D11ShaderResourceView *none=nullptr;context->PSSetShaderResources(13,1,&none);}
    std::cerr<<"terrain 1080p with the ground field "<<terrainFieldMs<<" ms (without "<<fieldOffMs<<")\n";
    report<<"\"ground_field\":{\"grid_locked_cell\":"<<cellLock.first<<",\"grid_locked_field\":"<<fieldLock.first<<",\"border_pixels_field\":"<<fieldLock.second<<",\"grass_area_cell\":"<<cellMask.area<<",\"grass_area_field\":"<<fieldMask.area
          <<",\"interior_rel_diff\":"<<coreDiff<<",\"grass_on_meadow_cell\":"<<cellRoots.first<<",\"grass_on_meadow_field\":"<<fieldRoots.first<<",\"meadow_blades\":"<<fieldRoots.second
          <<",\"gpu_ms_1080p_terrain_field\":"<<terrainFieldMs<<",\"gpu_ms_1080p_terrain_field_off\":"<<fieldOffMs<<",\"gpu_ms_1080p_meadow_cell\":"<<meadowCellMs<<",\"gpu_ms_1080p_meadow_field\":"<<meadowFieldMs<<"},";

    // 8. terrain102: hex stochastic tiling (131072) against the plain repeating layer.
    // Top-down over the dirt around the meadow (primary layer Ground054, 20-unit tile):
    // correlation of the image with itself one tile period away (a repeating tile
    // correlates strongly), plus the mean and contrast the ground must keep.
    drawMeadow(ortho,true,orthoView,orthoProjection,eye);const auto plainDirt=read(ortho);save(plainDirt,ortho,"hexdirt-off.bin");
    drawMeadow(ortho,true,orthoView,orthoProjection,eye,131072);const auto hexDirt=read(ortho);save(hexDirt,ortho,"hexdirt-on.bin");
    bool hexFinite=true;for(std::size_t i=0;i<hexDirt.size();i+=4)for(unsigned k=0;k<3;++k)hexFinite&=std::isfinite(hexDirt[i+k])&&hexDirt[i+k]>=0&&hexDirt[i+k]<65504;
    Check(hexFinite,"finite radiance with hex tiling");
    const float dirtTile=loader.Manifest().layers[loader.Manifest().classes[2].primary].tile;
    struct Stats {double mean=0,deviation=0,correlation=0;};
    auto repeatStats=[&](const std::vector<float> &p){Stats s;std::vector<double> a,b;
        for(float wy=10;wy<150-dirtTile;wy+=.25f)for(float wx=5;wx<85-dirtTile;wx+=.25f){a.push_back(luma(at(p,wx,wy)));b.push_back(.5*(luma(at(p,wx+dirtTile,wy))+luma(at(p,wx,wy+dirtTile))));}
        double ma=0,mb=0;for(std::size_t i=0;i<a.size();++i){ma+=a[i];mb+=b[i];}ma/=a.size();mb/=b.size();
        double sab=0,saa=0,sbb=0;for(std::size_t i=0;i<a.size();++i){sab+=(a[i]-ma)*(b[i]-mb);saa+=(a[i]-ma)*(a[i]-ma);sbb+=(b[i]-mb)*(b[i]-mb);}
        s.mean=ma;s.deviation=std::sqrt(saa/a.size());s.correlation=sab/std::sqrt(std::max(saa*sbb,1e-30));return s;};
    const auto plainStats=repeatStats(plainDirt),hexStats=repeatStats(hexDirt);
    std::cerr<<"dirt tile "<<dirtTile<<": tile-period correlation plain "<<plainStats.correlation<<" hex "<<hexStats.correlation<<", mean "<<plainStats.mean<<" / "<<hexStats.mean<<", deviation "<<plainStats.deviation<<" / "<<hexStats.deviation<<"\n";
    Check(plainStats.correlation>.3,"fixture: the plain layer repeats at its tile period");
    Check(hexStats.correlation<plainStats.correlation*.5,"hex tiling removes the tile repeat");
    Check(std::abs(hexStats.mean/plainStats.mean-1)<.04,"hex tiling keeps the ground's mean");
    Check(hexStats.deviation>plainStats.deviation*.8,"hex tiling keeps the ground's contrast (no washed-out blend)");
    drawMeadow(wide,true,rtsView,rtsProjection,rtsEye);save(read(wide),wide,"hexrts-off.bin");drawMeadow(wide,true,rtsView,rtsProjection,rtsEye,131072);save(read(wide),wide,"hexrts-on.bin");
    auto timeHex=[&](UINT extra){double best=1e9;for(unsigned i=0;i<20;++i){context->Begin(grassDisjoint.Get());context->End(grassBegin.Get());drawMeadow(wideTess,true,meadowView,crackProjection,meadowEye,extra);
        context->End(grassEnd.Get());context->End(grassDisjoint.Get());D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};while(context->GetData(grassDisjoint.Get(),&dj,sizeof(dj),0)==S_FALSE){}
        UINT64 t0=0,t1=0;while(context->GetData(grassBegin.Get(),&t0,8,0)==S_FALSE){}while(context->GetData(grassEnd.Get(),&t1,8,0)==S_FALSE){}
        Check(!dj.Disjoint,"stable GPU clock");best=std::min(best,static_cast<double>(t1-t0)/static_cast<double>(dj.Frequency)*1000);}return best;};
    double hexOffMs=1e9,hexOnMs=1e9;for(unsigned round=0;round<6;++round){hexOffMs=std::min(hexOffMs,timeHex(0));hexOnMs=std::min(hexOnMs,timeHex(131072));}
    std::cerr<<"meadow view 1080p: plain layers "<<hexOffMs<<" ms, hex "<<hexOnMs<<" ms\n";
    report<<"\"hex_tiling\":{\"tile\":"<<dirtTile<<",\"correlation_plain\":"<<plainStats.correlation<<",\"correlation_hex\":"<<hexStats.correlation<<",\"mean_plain\":"<<plainStats.mean<<",\"mean_hex\":"<<hexStats.mean
          <<",\"deviation_plain\":"<<plainStats.deviation<<",\"deviation_hex\":"<<hexStats.deviation<<",\"gpu_ms_1080p_meadow_plain\":"<<hexOffMs<<",\"gpu_ms_1080p_meadow_hex\":"<<hexOnMs<<"},";

    // 9. terrain104: wet ground (262144) on the meadow (grass on dirt, flat): the weather
    // constants (b7: wetness z, rain y, time in wind z) as the atmosphere system sets them.
    struct Weather {float inverseVP[16],viewProjection[16],camera[4],sun[4],sunColor[4],weather[4],wind[4],range[4],shadowArea[4],viewport[4],ambient[4],flashPosition[4],flashRadiance[4];};
    static_assert(sizeof(Weather)==304);auto weatherBuffer=constantBuffer(sizeof(Weather));
    auto setWeather=[&](float wetness,float rain,float time){Weather wx{};wx.weather[1]=rain;wx.weather[2]=wetness;wx.wind[2]=time;context->UpdateSubresource(weatherBuffer.Get(),0,nullptr,&wx,0,0);
        ID3D11Buffer *b7=weatherBuffer.Get();context->PSSetConstantBuffers(7,1,&b7);};
    const UINT wetGround=131072|262144;
    auto wetMeadow=[&](float wetness,float rain,float time){setWeather(wetness,rain,time);drawMeadow(ortho,true,orthoView,orthoProjection,eye,wetGround);return read(ortho);};
    const auto dry=wetMeadow(0,0,0),soaked=wetMeadow(1,0,0),damp=wetMeadow(.4f,0,0),raining=wetMeadow(1,1,3.7f);
    save(dry,ortho,"wetmeadow-off.bin");save(raining,ortho,"wetmeadow-on.bin");
    // Region statistics: dirt (x 10..80) and grass (meadow core) luma ratios; puddles are the
    // pixels well below their wet surroundings' median (dark standing water, top-down view).
    auto lumaIn=[&](const std::vector<float> &p,float x0,float x1,float y0,float y1){std::vector<float> v;for(float wy=y0;wy<y1;wy+=.25f)for(float wx=x0;wx<x1;wx+=.25f)v.push_back(luma(at(p,wx,wy)));return v;};
    auto mean=[](const std::vector<float> &v){double s=0;for(float x:v)s+=x;return s/std::max<std::size_t>(v.size(),1);};
    auto puddles=[&](const std::vector<float> &wetPixels,const std::vector<float> &dryPixels){std::vector<float> ratio;for(std::size_t i=0;i<wetPixels.size();++i)ratio.push_back(wetPixels[i]/std::max(dryPixels[i],1e-4f));
        auto sorted=ratio;std::nth_element(sorted.begin(),sorted.begin()+sorted.size()/2,sorted.end());const float median=sorted[sorted.size()/2];
        std::size_t n=0;for(float x:ratio)n+=x<median*.75f?1u:0u;return double(n)/std::max<std::size_t>(ratio.size(),1);};
    const auto dryDirt=lumaIn(dry,10,80,10,150),soakedDirt=lumaIn(soaked,10,80,10,150),dampDirt=lumaIn(damp,10,80,10,150),rainDirt=lumaIn(raining,10,80,10,150);
    const auto dryGrass=lumaIn(dry,135,165,65,95),soakedGrass=lumaIn(soaked,135,165,65,95);
    const double dirtRatio=mean(soakedDirt)/mean(dryDirt),grassRatio=mean(soakedGrass)/mean(dryGrass);
    const double puddleSoaked=puddles(soakedDirt,dryDirt),puddleDamp=puddles(dampDirt,dryDirt);
    // Rain rings move only the standing water: measured on the soaked puddle pixels.
    double rippleDiff=0,rippleOutside=0;std::size_t rippleN=0,outsideN=0;
    {std::vector<float> ratio;for(std::size_t i=0;i<soakedDirt.size();++i)ratio.push_back(soakedDirt[i]/std::max(dryDirt[i],1e-4f));auto sorted=ratio;std::nth_element(sorted.begin(),sorted.begin()+sorted.size()/2,sorted.end());const float median=sorted[sorted.size()/2];
     for(std::size_t i=0;i<rainDirt.size();++i){const double diff=std::abs(rainDirt[i]-soakedDirt[i])/std::max(soakedDirt[i],1e-4f);if(ratio[i]<median*.75f){rippleDiff+=diff;++rippleN;}else if(ratio[i]>median*.95f){rippleOutside+=diff;++outsideN;}}
     rippleDiff/=std::max<std::size_t>(rippleN,1);rippleOutside/=std::max<std::size_t>(outsideN,1);}
    bool wetFinite=true;for(std::size_t i=0;i<raining.size();i+=4)for(unsigned k=0;k<3;++k)wetFinite&=std::isfinite(raining[i+k])&&raining[i+k]>=0&&raining[i+k]<65504;
    std::cerr<<"wet ground: dirt "<<dirtRatio<<" grass "<<grassRatio<<" puddles soaked "<<puddleSoaked<<" damp "<<puddleDamp<<" rain rings |diff| "<<rippleDiff<<" outside "<<rippleOutside<<"\n";
    Check(wetFinite,"finite radiance on wet ground");
    Check(dirtRatio<.85&&dirtRatio>.4,"soaked dirt darkens");Check(grassRatio>dirtRatio,"porous dirt darkens more than grass");
    Check(puddleSoaked>.04&&puddleSoaked<.4,"standing water covers part of the flat dirt when soaked");Check(puddleDamp<puddleSoaked,"puddles grow with wetness");
    Check(rippleDiff>.01&&rippleDiff>4*rippleOutside,"rain rings move the standing water (not the ground around it)");
    setWeather(0,0,0);const auto dryAgain=read(ortho);(void)dryAgain;
    drawMeadow(wide,true,rtsView,rtsProjection,rtsEye,131072);save(read(wide),wide,"wetrts-off.bin");setWeather(1,1,3.7f);drawMeadow(wide,true,rtsView,rtsProjection,rtsEye,wetGround);save(read(wide),wide,"wetrts-on.bin");
    auto timeWet=[&](bool wet){setWeather(wet?1.0f:0.0f,wet?1.0f:0.0f,3.7f);double best=1e9;for(unsigned i=0;i<20;++i){context->Begin(grassDisjoint.Get());context->End(grassBegin.Get());drawMeadow(wideTess,true,meadowView,crackProjection,meadowEye,wetGround);
        context->End(grassEnd.Get());context->End(grassDisjoint.Get());D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};while(context->GetData(grassDisjoint.Get(),&dj,sizeof(dj),0)==S_FALSE){}
        UINT64 t0=0,t1=0;while(context->GetData(grassBegin.Get(),&t0,8,0)==S_FALSE){}while(context->GetData(grassEnd.Get(),&t1,8,0)==S_FALSE){}
        Check(!dj.Disjoint,"stable GPU clock");best=std::min(best,static_cast<double>(t1-t0)/static_cast<double>(dj.Frequency)*1000);}return best;};
    double dryMs=1e9,wetMs=1e9;for(unsigned round=0;round<6;++round){dryMs=std::min(dryMs,timeWet(false));wetMs=std::min(wetMs,timeWet(true));}
    {ID3D11Buffer *none=nullptr;context->PSSetConstantBuffers(7,1,&none);}
    std::cerr<<"wet ground 1080p: dry "<<dryMs<<" ms, soaked and raining "<<wetMs<<" ms\n";
    report<<"\"wet_ground\":{\"dirt_luma_ratio\":"<<dirtRatio<<",\"grass_luma_ratio\":"<<grassRatio<<",\"puddles_soaked\":"<<puddleSoaked<<",\"puddles_damp\":"<<puddleDamp
          <<",\"rain_ring_diff\":"<<rippleDiff<<",\"gpu_ms_1080p_dry\":"<<dryMs<<",\"gpu_ms_1080p_wet\":"<<wetMs<<"},";

    // 10. accum105: settled snow (524288) and dust (1048576) on world objects, not the ground.
    // Test faces (flat, 30, 60 and 90 degrees) seen head-on by an orthographic camera, drawn
    // retail-lit (vertex lighting, texture x diffuse) and through MaterialLight, with and
    // without the layer; then a small town on the meadow for the previews.
    const UINT snowBit=524288,dustBit=1048576;
    auto makeVB=[&](const std::vector<FixedVertex> &v){D3D11_BUFFER_DESC d{};d.ByteWidth=static_cast<UINT>(v.size()*sizeof(FixedVertex));d.Usage=D3D11_USAGE_IMMUTABLE;d.BindFlags=D3D11_BIND_VERTEX_BUFFER;
        D3D11_SUBRESOURCE_DATA i{v.data(),0,0};ComPtr<ID3D11Buffer> b;HR(device->CreateBuffer(&d,&i,b.GetAddressOf()),"object vertices");return b;};
    auto regionUV=[&](unsigned region,float a,float b){const auto o=RegionOrigin(region);return std::array<float,2>{(o[0]+Region*(.2f+.6f*a))/prepared.width,(o[1]+Region*(.2f+.6f*b))/prepared.height};};
    using P3=std::array<float,3>;
    // One triangle; its normal is flipped to face away from the given interior point.
    auto addTri=[&](std::vector<FixedVertex> &out,const P3 &p0,const P3 &p1,const P3 &p2,const P3 &inside,unsigned region){
        const float e1[3]{p1[0]-p0[0],p1[1]-p0[1],p1[2]-p0[2]},e2[3]{p2[0]-p0[0],p2[1]-p0[1],p2[2]-p0[2]};
        float n[3]{e1[1]*e2[2]-e1[2]*e2[1],e1[2]*e2[0]-e1[0]*e2[2],e1[0]*e2[1]-e1[1]*e2[0]};const float l=std::sqrt(n[0]*n[0]+n[1]*n[1]+n[2]*n[2]);
        if(n[0]*(p0[0]-inside[0])+n[1]*(p0[1]-inside[1])+n[2]*(p0[2]-inside[2])<0)for(float &x:n)x=-x;
        const P3 *p[3]{&p0,&p1,&p2};const float ab[3][2]{{0,0},{1,0},{1,1}};
        for(unsigned k=0;k<3;++k){FixedVertex v;for(unsigned i=0;i<3;++i){v.position[i]=(*p[k])[i];v.normal[i]=n[i]/l;}const auto uv=regionUV(region,ab[k][0],ab[k][1]);v.uv[0][0]=uv[0];v.uv[0][1]=uv[1];out.push_back(v);}};
    auto addQuad=[&](std::vector<FixedVertex> &out,const P3 &a,const P3 &b,const P3 &cc,const P3 &dd,const P3 &inside,unsigned region){addTri(out,a,b,cc,inside,region);addTri(out,a,cc,dd,inside,region);};
    // A face through (100,100,20) tilted about x by the given angle (0 = flat roof, 90 = wall).
    auto faceGeometry=[&](float degrees,float half,unsigned region){const float a=DirectX::XMConvertToRadians(degrees),sn=std::sin(a),cs=std::cos(a);
        const P3 centre{100,100,20},inside{100,100+sn*5,20-cs*5};std::vector<FixedVertex> q;
        auto facePoint=[&](float u,float v){return P3{centre[0]+u*half,centre[1]+v*half*cs,centre[2]+v*half*sn};};
        addQuad(q,facePoint(-1,-1),facePoint(1,-1),facePoint(1,1),facePoint(-1,1),inside,region);return q;};
    auto drawObjects=[&](Target &t,const std::vector<FixedVertex> &geometry,ID3D11Buffer *buffer,const DirectX::XMMATRIX &view,const DirectX::XMMATRIX &projection,const DirectX::XMVECTOR &eyeAt,bool modern,UINT bits,bool vehicle,bool clear){
        const LegacyConstants saved=c;
        DirectX::XMFLOAT4X4 m;DirectX::XMStoreFloat4x4(&m,view);std::memcpy(c.view,&m,64);DirectX::XMStoreFloat4x4(&m,projection);std::memcpy(c.projection,&m,64);
        DirectX::XMFLOAT3 e;DirectX::XMStoreFloat3(&e,eyeAt);c.cameraWorld[0]=e.x;c.cameraWorld[1]=e.y;c.cameraWorld[2]=e.z;
        c.viewport[2]=static_cast<float>(t.width);c.viewport[3]=static_cast<float>(t.height);
        c.modernMaterial[0]=modern?1.0f:0.0f;c.modernMaterial[1]=.7f;c.modernMaterial[3]=static_cast<float>(4u|bits);
        c.geometryFlags[1]=modern?0.0f:1.0f;for(unsigned i=0;i<4;++i)c.materialAmbient[i]=1;
        c.colorStage[0][0]=4;c.colorStage[0][1]=2;c.colorStage[0][2]=0;c.colorStage[1][0]=1;c.vehicleAge[3]=vehicle?1.0f:0.0f;
        HR(calibrated.Prepare(context.Get(),c,true),"light preparation");context->UpdateSubresource(cb.Get(),0,nullptr,&c,0,0);c=saved;
        UINT stride=sizeof(FixedVertex),offset=0;context->IASetVertexBuffers(0,1,&buffer,&stride,&offset);context->IASetInputLayout(layout.Get());context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->VSSetShader(vs.Get(),nullptr,0);context->PSSetShader(ps.Get(),nullptr,0);
        ID3D11Buffer *b0=cb.Get();context->VSSetConstantBuffers(0,1,&b0);context->PSSetConstantBuffers(0,1,&b0);
        ID3D11Buffer *b2=sb.Get();context->PSSetConstantBuffers(2,1,&b2);ID3D11Buffer *b4=ib.Get();context->VSSetConstantBuffers(4,1,&b4);context->PSSetConstantBuffers(4,1,&b4);ID3D11Buffer *b13=control.Get();context->PSSetConstantBuffers(13,1,&b13);
        ID3D11ShaderResourceView *views[2]{baseView.Get(),baseView.Get()};context->PSSetShaderResources(0,2,views);
        ID3D11SamplerState *samplers[2]{atlasSampler.Get(),atlasSampler.Get()};context->PSSetSamplers(0,2,samplers);
        auto *rtv=t.view.Get();context->OMSetRenderTargets(1,&rtv,t.depthView.Get());context->OMSetDepthStencilState(depthState.Get(),0);
        D3D11_VIEWPORT vp{0,0,static_cast<float>(t.width),static_cast<float>(t.height),0,1};context->RSSetViewports(1,&vp);context->RSSetState(raster.Get());
        if(clear){const float black[4]{};context->ClearRenderTargetView(rtv,black);context->ClearDepthStencilView(t.depthView.Get(),D3D11_CLEAR_DEPTH,1,0);}
        context->Draw(static_cast<UINT>(geometry.size()),0);};
    Target faceTarget=MakeTarget(device.Get(),128,128);
    auto faceRender=[&](Target &t,float degrees,float half,float width,float height,bool modern,UINT bits,bool vehicle,unsigned region=7){const auto q=faceGeometry(degrees,half,region);const auto vb=makeVB(q);
        const float a=DirectX::XMConvertToRadians(degrees),sn=std::sin(a),cs=std::cos(a);
        const auto centre=DirectX::XMVectorSet(100,100,20,1),eyeAt=DirectX::XMVectorSet(100,100-sn*50,20+cs*50,1);
        const auto view=DirectX::XMMatrixLookAtRH(eyeAt,centre,DirectX::XMVectorSet(0,cs,sn,0));
        drawObjects(t,q,vb.Get(),view,DirectX::XMMatrixOrthographicRH(width,height,1,200),eyeAt,modern,bits,vehicle,true);};
    struct Layer {double changed=0,lumaRatio=0,meanDiff=0,maxDiff=0,chroma=0,redBlueOff=0,redBlueOn=0;};
    auto compareLayer=[&](const std::vector<float> &off,const std::vector<float> &on){Layer r;double mo[3]{},mn[3]{};std::size_t n=0,changed=0;
        for(std::size_t i=0;i<off.size();i+=4){++n;double dsum=0;for(int k=0;k<3;++k){mo[k]+=off[i+k];mn[k]+=on[i+k];dsum+=std::abs(double(on[i+k])-off[i+k]);}
            r.meanDiff+=dsum/3;r.maxDiff=std::max(r.maxDiff,dsum);if(dsum>1e-6)++changed;}
        for(int k=0;k<3;++k){mo[k]/=std::max<std::size_t>(n,1);mn[k]/=std::max<std::size_t>(n,1);}
        auto L=[](const double *x){return .2126*x[0]+.7152*x[1]+.0722*x[2];};
        r.changed=double(changed)/std::max<std::size_t>(n,1);r.meanDiff/=std::max<std::size_t>(n,1);r.lumaRatio=L(mn)/std::max(L(mo),1e-9);
        r.chroma=(std::max({mn[0],mn[1],mn[2]})-std::min({mn[0],mn[1],mn[2]}))/std::max(std::max({mn[0],mn[1],mn[2]}),1e-9);
        r.redBlueOff=mo[0]/std::max(mo[2],1e-9);r.redBlueOn=mn[0]/std::max(mn[2],1e-9);return r;};
    // Snow and dust are judged on the most neutral roof of the fixture atlas.
    auto faceLayer=[&](float degrees,bool modern,UINT bits,bool vehicle,unsigned region=7){faceRender(faceTarget,degrees,10,20,20,modern,0,vehicle,region);const auto off=read(faceTarget);
        faceRender(faceTarget,degrees,10,20,20,modern,bits,vehicle,region);return compareLayer(off,read(faceTarget));};
    // The fixture atlas regions are tinted; the most neutral one (lowest chroma) is the test roof.
    unsigned roof=0;double roofChroma=1e9;
    for(unsigned region=0;region<10;++region){faceRender(faceTarget,0,10,20,20,false,0,false,region);const auto roofPixels=read(faceTarget);
        double m[3]{};for(std::size_t i=0;i<roofPixels.size();i+=4)for(int k=0;k<3;++k)m[k]+=roofPixels[i+k];
        const double chroma=(std::max({m[0],m[1],m[2]})-std::min({m[0],m[1],m[2]}))/std::max(std::max({m[0],m[1],m[2]}),1e-9);
        if(chroma<roofChroma){roofChroma=chroma;roof=region;}}
    struct Path {Layer flat,slope30,slope60,wall,dust,vehicle;};
    Path paths[2];
    for(int modern=0;modern<2;++modern){auto &pth=paths[modern];
        pth.flat=faceLayer(0,modern!=0,snowBit,false,roof);pth.slope30=faceLayer(30,modern!=0,snowBit,false,roof);pth.slope60=faceLayer(60,modern!=0,snowBit,false,roof);
        pth.wall=faceLayer(90,modern!=0,snowBit,false,roof);pth.dust=faceLayer(0,modern!=0,dustBit,false,roof);pth.vehicle=faceLayer(0,modern!=0,snowBit,true,roof);
        const char *name=modern?"modern":"retail";
        std::cerr<<"accumulation "<<name<<" (roof region "<<roof<<", chroma "<<roofChroma<<"): snow flat changed "<<pth.flat.changed<<" luma x"<<pth.flat.lumaRatio<<" chroma "<<pth.flat.chroma<<", 30deg "<<pth.slope30.changed<<", 60deg "<<pth.slope60.changed
                 <<", wall max diff "<<pth.wall.maxDiff<<"; dust changed "<<pth.dust.changed<<" luma x"<<pth.dust.lumaRatio<<" red/blue "<<pth.dust.redBlueOff<<" -> "<<pth.dust.redBlueOn
                 <<"; vehicle/static "<<pth.vehicle.meanDiff/std::max(pth.flat.meanDiff,1e-12)<<"\n";
        Check(pth.flat.changed>.95&&pth.flat.lumaRatio>1.3&&pth.flat.chroma<.15,"snow covers a flat roof (white, brighter than the dark roof)");
        Check(pth.slope30.changed>.9,"snow holds on a 30-degree roof");
        Check(pth.slope60.changed>.05&&pth.slope60.changed<.9,"snow lies only in drifts on a 60-degree face");
        Check(pth.wall.maxDiff==0,"walls are untouched (bit-identical)");
        Check(pth.dust.changed>.9&&pth.dust.redBlueOn>pth.dust.redBlueOff*1.05,"dust tints a neutral roof toward sand");
        Check(pth.vehicle.meanDiff<.6*pth.flat.meanDiff,"vehicles carry much less of the layer");
    }
    // The ground itself never takes the object layer (its own snow/sand classes stay).
    drawMeadow(ortho,true,orthoView,orthoProjection,eye);const auto groundPlain=read(ortho);
    drawMeadow(ortho,true,orthoView,orthoProjection,eye,snowBit);const auto groundSnow=read(ortho);
    drawMeadow(ortho,true,orthoView,orthoProjection,eye,dustBit);const auto groundDust=read(ortho);
    // photo107: the settled layer reaches the library ground's flat areas (the plateau, the low meadow
    // flats) and keeps off the steep ramp (cell rows 13-14, normals z .64 and .37).
    auto regionChange=[&](const std::vector<float> &a,const std::vector<float> &b,float x0,float x1,float y0,float y1){
        double changed=0,meanLuma=0;unsigned n=0;
        for(float wy=y0;wy<y1;wy+=.5f)for(float wx=x0;wx<x1;wx+=.5f){const float *pa=at(a,wx,wy),*pb=at(b,wx,wy);const double d=std::abs(luma(pa)-luma(pb));if(d>1e-5)changed+=1;meanLuma+=luma(pb)-luma(pa);++n;}
        return std::pair<double,double>{changed/std::max(n,1u),meanLuma/std::max(n,1u)};};
    // The steep band is the real ramp geometry (VertexHeight), drawn by render(); drawMeadow is flat.
    render(ortho,true,orthoView,orthoProjection,eye);const auto rampPlain=read(ortho);
    render(ortho,true,orthoView,orthoProjection,eye,1,snowBit);const auto rampSnow=read(ortho);
    render(ortho,true,orthoView,orthoProjection,eye,1,dustBit);const auto rampDust=read(ortho);
    const auto flatSnow=regionChange(groundPlain,groundSnow,20,120,20,100),steepSnow=regionChange(rampPlain,rampSnow,20,250,132,148);
    const auto flatDust=regionChange(groundPlain,groundDust,20,120,20,100),steepDust=regionChange(rampPlain,rampDust,20,250,132,148);
    std::cerr<<"settled layer on the library ground: snow flats "<<flatSnow.first<<" (luma "<<flatSnow.second<<"), steep "<<steepSnow.first<<"; dust flats "<<flatDust.first<<", steep "<<steepDust.first<<"\n";{auto band=[&](const std::vector<float> &a,const std::vector<float> &b,double threshold){double maxDiff=0,meanA=0,meanB=0,over=0;unsigned n=0;for(float wy=132;wy<148;wy+=.5f)for(float wx=20;wx<250;wx+=.5f){const float *pa=at(a,wx,wy),*pb=at(b,wx,wy);const double d=std::abs(luma(pa)-luma(pb));maxDiff=std::max(maxDiff,d);if(d>threshold)over+=1;meanA+=luma(pa);meanB+=luma(pb);++n;}return std::array<double,4>{maxDiff,meanA/std::max(n,1u),meanB/std::max(n,1u),over/std::max(n,1u)};};render(ortho,true,orthoView,orthoProjection,eye);const auto again=read(ortho);const auto ctl=band(rampPlain,again,1e-5),chg=band(rampPlain,rampSnow,1e-5);Check(ctl[0]==0&&ctl[3]==0,"steep band renders the same twice");std::cerr<<"steep diagnostics: plain-vs-plain max "<<ctl[0]<<" over "<<ctl[3]<<", plain mean "<<ctl[1]<<", snow mean "<<chg[2]<<", plain-vs-snow max "<<chg[0]<<" over "<<chg[3]<<" (over .01: "<<band(rampPlain,rampSnow,.01)[3]<<")\n";}
    Check(flatSnow.first>.5&&flatSnow.second>.02,"settled snow covers the library ground's flat areas and brightens them");
    Check(steepSnow.first<.01,"steep ground keeps its look under settled snow");
    Check(flatDust.first>.5&&steepDust.first<.01,"settled dust covers the flats only");
    // photo107: scattered rocks (flag 4194304) on the stone and dirt: some library ground changes, most of it stays.
    render(ortho,true,orthoView,orthoProjection,eye,1,4194304);const auto rockPixels=read(ortho);
    {std::size_t rockGround=0,rockChanged=0;double rockLargest=0;
     for(std::size_t i=0;i<on.size();i+=4){if(on[i]+on[i+1]+on[i+2]<=0)continue;++rockGround;const double rockDiff=std::abs(luma(&on[i])-luma(&rockPixels[i]));if(rockDiff>1e-5)++rockChanged;rockLargest=std::max(rockLargest,rockDiff);}
     std::cerr<<"scattered rocks: changed "<<rockChanged<<" of "<<rockGround<<" ground pixels, largest luma change "<<rockLargest<<"\n";
     Check(rockChanged>rockGround/200&&rockChanged<rockGround*6/10,"scattered rocks change some library ground and leave most of it");
     Check(rockLargest<.6,"rocks stay within the ground's brightness range");}
    // photo107: object motion (aux target 6). A quad that moved 20 units in x writes its screen offset, -20/W in UV, and no other pixel moves.
    {std::vector<FixedVertex> moverQuad;addQuad(moverQuad,{80.0f,80.0f,5.0f},{120.0f,80.0f,5.0f},{120.0f,120.0f,5.0f},{80.0f,120.0f,5.0f},{100.0f,100.0f,0.0f},7);
     const auto moverVB=makeVB(moverQuad);
     Target moverColour=MakeTarget(device.Get(),static_cast<UINT>(worldW*4),static_cast<UINT>(worldH*4));
     Target moverMotion=MakeTarget(device.Get(),static_cast<UINT>(worldW*4),static_cast<UINT>(worldH*4));
     const auto moverPrevious=DirectX::XMMatrixTranslation(-20.0f,0.0f,0.0f);
     render(moverColour,false,orthoView,orthoProjection,eye,1,0,&moverPrevious,&moverMotion,moverVB.Get(),static_cast<UINT>(moverQuad.size()));
     const auto motionPixels=read(moverMotion);
     const double moverExpected=-20.0/worldW;std::size_t moverInside=0,moverGood=0,moverOutside=0;
     for(UINT rowIndex=0;rowIndex<moverMotion.height;++rowIndex)for(UINT colIndex=0;colIndex<moverMotion.width;++colIndex){
         const float *moverPixel=&motionPixels[(static_cast<std::size_t>(rowIndex)*moverMotion.width+colIndex)*4];
         const float mapX=(colIndex+.5f)/4,mapY=worldH-(rowIndex+.5f)/4;
         if(mapX>80&&mapX<120&&mapY>80&&mapY<120){++moverInside;if(std::abs(moverPixel[0]-moverExpected)<.02*std::abs(moverExpected)&&std::abs(moverPixel[1])<1e-4)++moverGood;}
         else if(std::abs(moverPixel[0])>1e-6||std::abs(moverPixel[1])>1e-6)++moverOutside;}
     std::cerr<<"object motion: "<<moverGood<<" of "<<moverInside<<" quad pixels at "<<moverExpected<<", stray "<<moverOutside<<"\n";
     Check(moverInside>0&&moverGood>moverInside*95/100,"a moving object writes its screen motion");
     Check(moverOutside==0,"only the moving object writes motion");}
    // A small town on the meadow: gabled houses (30-degree roofs), a tower (60) and a flat bunker.
    std::vector<FixedVertex> town;
    auto building=[&](float x0,float y0,float w,float dpt,float h,float slope){const float x1=x0+w,y1=y0+dpt,yc=(y0+y1)/2;const P3 inside{(x0+x1)/2,yc,h*.5f};
        addQuad(town,{x0,y0,0},{x1,y0,0},{x1,y0,h},{x0,y0,h},inside,8);addQuad(town,{x0,y1,0},{x1,y1,0},{x1,y1,h},{x0,y1,h},inside,8);
        addQuad(town,{x0,y0,0},{x0,y1,0},{x0,y1,h},{x0,y0,h},inside,8);addQuad(town,{x1,y0,0},{x1,y1,0},{x1,y1,h},{x1,y0,h},inside,8);
        if(slope<=0){addQuad(town,{x0,y0,h},{x1,y0,h},{x1,y1,h},{x0,y1,h},inside,7);return;}
        const float ridge=h+(dpt/2)*std::tan(DirectX::XMConvertToRadians(slope));const P3 roofInside{(x0+x1)/2,yc,h};
        addQuad(town,{x0,y0,h},{x1,y0,h},{x1,yc,ridge},{x0,yc,ridge},roofInside,7);addQuad(town,{x0,y1,h},{x1,y1,h},{x1,yc,ridge},{x0,yc,ridge},roofInside,7);
        addTri(town,{x0,y0,h},{x0,y1,h},{x0,yc,ridge},roofInside,8);addTri(town,{x1,y0,h},{x1,y1,h},{x1,yc,ridge},roofInside,8);};
    building(110,48,18,12,8,30);building(150,66,16,12,7,30);building(183,40,8,8,14,60);building(128,82,14,14,5,0);building(95,72,12,10,6,30);
    const auto townVB=makeVB(town);
    auto townView=[&](UINT bits,const char *name){drawMeadow(wide,true,rtsView,rtsProjection,rtsEye);drawObjects(wide,town,townVB.Get(),rtsView,rtsProjection,rtsEye,false,0,false,false);save(read(wide),wide,(std::string(name)+"-off.bin").c_str());
        drawMeadow(wide,true,rtsView,rtsProjection,rtsEye);drawObjects(wide,town,townVB.Get(),rtsView,rtsProjection,rtsEye,false,bits,false,false);save(read(wide),wide,(std::string(name)+"-on.bin").c_str());};
    townView(snowBit,"snowrts");townView(dustBit,"dustrts");
    // Cost: a full-screen flat roof at 1080p, interleaved rounds (clock drift cancels).
    auto timeLayer=[&](bool modern,UINT bits){double best=1e9;for(unsigned i=0;i<20;++i){context->Begin(grassDisjoint.Get());context->End(grassBegin.Get());faceRender(wideTess,0,20,32,18,modern,bits,false);
        context->End(grassEnd.Get());context->End(grassDisjoint.Get());D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};while(context->GetData(grassDisjoint.Get(),&dj,sizeof(dj),0)==S_FALSE){}
        UINT64 t0=0,t1=0;while(context->GetData(grassBegin.Get(),&t0,8,0)==S_FALSE){}while(context->GetData(grassEnd.Get(),&t1,8,0)==S_FALSE){}
        Check(!dj.Disjoint,"stable GPU clock");best=std::min(best,static_cast<double>(t1-t0)/static_cast<double>(dj.Frequency)*1000);}return best;};
    double layerMs[2][2]{{1e9,1e9},{1e9,1e9}};
    for(unsigned round=0;round<6;++round)for(int modern=0;modern<2;++modern){layerMs[modern][0]=std::min(layerMs[modern][0],timeLayer(modern!=0,0));layerMs[modern][1]=std::min(layerMs[modern][1],timeLayer(modern!=0,snowBit));}
    std::cerr<<"accumulation 1080p full-screen roof: retail "<<layerMs[0][0]<<" -> "<<layerMs[0][1]<<" ms, modern "<<layerMs[1][0]<<" -> "<<layerMs[1][1]<<" ms\n";
    report<<"\"accumulation\":{";
    for(int modern=0;modern<2;++modern){const auto &pth=paths[modern];report<<(modern?",":"")<<"\""<<(modern?"modern":"retail")<<"\":{\"flat_changed\":"<<pth.flat.changed<<",\"flat_luma_ratio\":"<<pth.flat.lumaRatio<<",\"flat_chroma\":"<<pth.flat.chroma
        <<",\"slope30_changed\":"<<pth.slope30.changed<<",\"slope60_changed\":"<<pth.slope60.changed<<",\"wall_max_diff\":"<<pth.wall.maxDiff<<",\"dust_changed\":"<<pth.dust.changed<<",\"dust_luma_ratio\":"<<pth.dust.lumaRatio
        <<",\"dust_red_blue_off\":"<<pth.dust.redBlueOff<<",\"dust_red_blue_on\":"<<pth.dust.redBlueOn<<",\"vehicle_ratio\":"<<pth.vehicle.meanDiff/std::max(pth.flat.meanDiff,1e-12)
        <<",\"gpu_ms_1080p_off\":"<<layerMs[modern][0]<<",\"gpu_ms_1080p_snow\":"<<layerMs[modern][1]<<"}";}
    report<<",\"roof_region\":"<<roof<<",\"roof_chroma\":"<<roofChroma<<",\"terrain_unchanged\":true},";

    // 11. water107: the water's shore opacity per pixel from the scene depth (flag 2097152).
    // The main fixture terrain rises 0/6/18/43 units over cell rows 12-15 (no bumps, so the
    // height depends on y only); a water plane at z=10 covers it. The shader's opacity is
    // compared with the native curve min(depth/(transparent*floor), floor) on the analytic
    // ground, top-down (orthographic) and from the RTS camera (perspective, ray-traced).
    const float waterZ=10,waterTransparent=8,waterFloor=1;
    auto groundZ=[&](float wy){const float v=wy/10;const unsigned row=static_cast<unsigned>(v);const float f=v-static_cast<float>(row);
        return VertexHeight(row)+(VertexHeight(row+1)-VertexHeight(row))*f;};
    auto expected=[&](float ground){return std::min(std::max(waterZ-ground,0.0f)/(waterTransparent*waterFloor),waterFloor);};
    // A depth copy the shader can read (the fixture's depth targets have no SRV binding).
    auto depthCopy=[&](const Target &t){D3D11_TEXTURE2D_DESC d{};t.depth->GetDesc(&d);d.BindFlags=D3D11_BIND_SHADER_RESOURCE;ComPtr<ID3D11Texture2D> copy;HR(device->CreateTexture2D(&d,nullptr,copy.GetAddressOf()),"depth copy");
        context->CopyResource(copy.Get(),t.depth.Get());D3D11_SHADER_RESOURCE_VIEW_DESC v{};v.Format=DXGI_FORMAT_R32_FLOAT;v.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2D;v.Texture2D.MipLevels=1;
        ComPtr<ID3D11ShaderResourceView> view;HR(device->CreateShaderResourceView(copy.Get(),&v,view.GetAddressOf()),"depth view");return view;};
    std::vector<FixedVertex> waterQuad;{FixedVertex v[4];const float corner2[4][2]{{-50,-50},{worldW+50,-50},{worldW+50,worldH+50},{-50,worldH+50}};
        for(unsigned k=0;k<4;++k){v[k].position[0]=corner2[k][0];v[k].position[1]=corner2[k][1];v[k].position[2]=waterZ;v[k].diffuse[0]=.18f;v[k].diffuse[1]=.32f;v[k].diffuse[2]=.42f;v[k].diffuse[3]=1;}
        for(unsigned k:{0u,1u,2u,0u,2u,3u})waterQuad.push_back(v[k]);}
    const auto waterVB=makeVB(waterQuad);auto waterBuffer=constantBuffer(16);
    D3D11_BLEND_DESC waterBlend{};waterBlend.RenderTarget[0].BlendEnable=TRUE;waterBlend.RenderTarget[0].SrcBlend=D3D11_BLEND_SRC_ALPHA;waterBlend.RenderTarget[0].DestBlend=D3D11_BLEND_INV_SRC_ALPHA;waterBlend.RenderTarget[0].BlendOp=D3D11_BLEND_OP_ADD;
    waterBlend.RenderTarget[0].SrcBlendAlpha=D3D11_BLEND_ONE;waterBlend.RenderTarget[0].DestBlendAlpha=D3D11_BLEND_ZERO;waterBlend.RenderTarget[0].BlendOpAlpha=D3D11_BLEND_OP_ADD;waterBlend.RenderTarget[0].RenderTargetWriteMask=D3D11_COLOR_WRITE_ENABLE_ALL;
    ComPtr<ID3D11BlendState> alphaBlend;HR(device->CreateBlendState(&waterBlend,alphaBlend.GetAddressOf()),"water blend");
    D3D11_DEPTH_STENCIL_DESC nd{};nd.DepthEnable=FALSE;ComPtr<ID3D11DepthStencilState> noDepth;HR(device->CreateDepthStencilState(&nd,noDepth.GetAddressOf()),"no depth");
    // Draws the water plane over target t (no depth test; `scene` = depth copy of the ground).
    auto drawWater=[&](Target &t,ID3D11ShaderResourceView *scene,const DirectX::XMMATRIX &view,const DirectX::XMMATRIX &projection,const DirectX::XMVECTOR &eyeAt,bool shore,float foam,bool blend,bool clear,ID3D11ShaderResourceView *colour=nullptr,bool reflect=false){
        const LegacyConstants saved=c;
        DirectX::XMFLOAT4X4 m;DirectX::XMStoreFloat4x4(&m,view);std::memcpy(c.view,&m,64);DirectX::XMStoreFloat4x4(&m,projection);std::memcpy(c.projection,&m,64);
        DirectX::XMFLOAT3 e;DirectX::XMStoreFloat3(&e,eyeAt);c.cameraWorld[0]=e.x;c.cameraWorld[1]=e.y;c.cameraWorld[2]=e.z;
        c.viewport[2]=static_cast<float>(t.width);c.viewport[3]=static_cast<float>(t.height);
        c.modernMaterial[0]=0;c.modernMaterial[3]=static_cast<float>((shore?2097152u:0u)|(reflect?8388608u:0u));c.geometryFlags[1]=0;
        c.colorStage[0][0]=2;c.colorStage[0][1]=0;c.colorStage[1][0]=1;c.alphaStage[0][0]=2;c.alphaStage[0][1]=0;c.alphaStage[0][3]=0;
        HR(calibrated.Prepare(context.Get(),c,true),"light preparation");context->UpdateSubresource(cb.Get(),0,nullptr,&c,0,0);c=saved;
        const float settings[4]{waterTransparent,waterFloor,foam,scene?1.0f:0.0f};context->UpdateSubresource(waterBuffer.Get(),0,nullptr,settings,0,0);
        UINT stride=sizeof(FixedVertex),offset=0;auto *buffer=waterVB.Get();context->IASetVertexBuffers(0,1,&buffer,&stride,&offset);context->IASetInputLayout(layout.Get());context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->VSSetShader(vs.Get(),nullptr,0);context->PSSetShader(ps.Get(),nullptr,0);
        ID3D11Buffer *b0=cb.Get();context->VSSetConstantBuffers(0,1,&b0);context->PSSetConstantBuffers(0,1,&b0);
        ID3D11Buffer *b2=sb.Get();context->PSSetConstantBuffers(2,1,&b2);ID3D11Buffer *b4=ib.Get();context->VSSetConstantBuffers(4,1,&b4);context->PSSetConstantBuffers(4,1,&b4);ID3D11Buffer *b13=control.Get();context->PSSetConstantBuffers(13,1,&b13);
        ID3D11Buffer *b6=waterBuffer.Get();context->PSSetConstantBuffers(6,1,&b6);context->PSSetShaderResources(60,1,&scene);ID3D11ShaderResourceView *colourView=colour;context->PSSetShaderResources(61,1,&colourView);
        auto *rtv=t.view.Get();context->OMSetRenderTargets(1,&rtv,nullptr);context->OMSetDepthStencilState(noDepth.Get(),0);context->OMSetBlendState(blend?alphaBlend.Get():nullptr,nullptr,0xffffffff);
        D3D11_VIEWPORT vp{0,0,static_cast<float>(t.width),static_cast<float>(t.height),0,1};context->RSSetViewports(1,&vp);context->RSSetState(raster.Get());
        if(clear){const float black[4]{};context->ClearRenderTargetView(rtv,black);}
        context->Draw(static_cast<UINT>(waterQuad.size()),0);
        ID3D11ShaderResourceView *none=nullptr;context->PSSetShaderResources(60,1,&none);context->PSSetShaderResources(61,1,&none);context->OMSetBlendState(nullptr,nullptr,0xffffffff);};
    // Top-down: ground (with depth), then the water's raw output (alpha) into a separate target.
    render(ortho,true,orthoView,orthoProjection,eye);const auto orthoDepth=depthCopy(ortho);
    Target waterTarget=MakeTarget(device.Get(),ortho.width,ortho.height);
    drawWater(waterTarget,orthoDepth.Get(),orthoView,orthoProjection,eye,true,0,false,true);const auto orthoWater=read(waterTarget);
    drawWater(waterTarget,orthoDepth.Get(),orthoView,orthoProjection,eye,true,1,false,true);const auto orthoFoam=read(waterTarget);
    drawWater(waterTarget,orthoDepth.Get(),orthoView,orthoProjection,eye,false,0,false,true);const auto orthoPlain=read(waterTarget);
    double orthoError=0,deepAlpha=1,fadePixels=0,foamOutside=0;std::size_t orthoN=0,foamInside=0;
    for(UINT py=0;py<ortho.height;++py)for(UINT px=0;px<ortho.width;++px){const float wx=(px+.5f)/4,wy=worldH-(py+.5f)/4;if(wx<20||wx>worldW-20||wy<121||wy>149)continue;
        const std::size_t i=(std::size_t(py)*ortho.width+px)*4;const float ground=groundZ(wy),want=expected(ground);
        orthoError=std::max(orthoError,double(std::abs(orthoWater[i+3]-want)));++orthoN;if(want>0&&want<1)++fadePixels;if(waterZ-ground>waterTransparent+1)deepAlpha=std::min(deepAlpha,double(orthoWater[i+3]));
        const float colourDiff=std::abs(orthoFoam[i]-orthoWater[i])+std::abs(orthoFoam[i+1]-orthoWater[i+1])+std::abs(orthoFoam[i+2]-orthoWater[i+2]);
        if(colourDiff>1e-4f){if(waterZ-ground<1.5f&&waterZ-ground>0)++foamInside;else foamOutside+=1;}}
    bool plainOpaque=true;for(std::size_t i=3;i<orthoPlain.size();i+=4)plainOpaque&=orthoPlain[i]==1;
    // Perspective: the RTS camera; each pixel's ray meets the water plane, then the ground
    // (bisection on the analytic height) for the expected depth below the water.
    render(wide,true,rtsView,rtsProjection,rtsEye);const auto wideDepth=depthCopy(wide);Target wideWater=MakeTarget(device.Get(),wide.width,wide.height);
    drawWater(wideWater,wideDepth.Get(),rtsView,rtsProjection,rtsEye,true,0,false,true);const auto rtsWater=read(wideWater);
    double rtsError=0;std::size_t rtsN=0;DirectX::XMFLOAT3 eyePoint;DirectX::XMStoreFloat3(&eyePoint,rtsEye);
    for(UINT py=0;py<wide.height;py+=3)for(UINT px=0;px<wide.width;px+=3){
        const auto far3=DirectX::XMVector3Unproject(DirectX::XMVectorSet(px+.5f,py+.5f,1,0),0,0,static_cast<float>(wide.width),static_cast<float>(wide.height),0,1,rtsProjection,rtsView,DirectX::XMMatrixIdentity());
        DirectX::XMFLOAT3 f;DirectX::XMStoreFloat3(&f,far3);const float dx=f.x-eyePoint.x,dy=f.y-eyePoint.y,dz=f.z-eyePoint.z;if(dz>=0)continue;
        const float tw=(waterZ-eyePoint.z)/dz;const float hx=eyePoint.x+dx*tw,hy=eyePoint.y+dy*tw;if(hx<20||hx>worldW-20||hy<121||hy>149)continue;
        // The ground hit: below the water plane, before the ray passes under the terrain.
        float lo=tw,hi=tw;auto below=[&](float t){const float y=eyePoint.y+dy*t,z=eyePoint.z+dz*t;return y<120.5f||y>149.5f?false:z<=groundZ(y);};
        if(below(lo)){lo=hi=tw;}else{const float step=tw*.002f;while(!below(hi)&&hi<tw*3){lo=hi;hi+=step;}if(!below(hi))continue;for(int it=0;it<40;++it){const float mid=.5f*(lo+hi);if(below(mid))hi=mid;else lo=mid;}}
        const float groundY=eyePoint.y+dy*hi;const float want=expected(groundZ(groundY));
        const std::size_t i=(std::size_t(py)*wide.width+px)*4;rtsError=std::max(rtsError,double(std::abs(rtsWater[i+3]-want)));++rtsN;}
    std::cerr<<"water shore: top-down max |alpha - native curve| "<<orthoError<<" ("<<orthoN<<" px, "<<fadePixels<<" fading), deep "<<deepAlpha<<"; RTS view "<<rtsError<<" ("<<rtsN<<" px); foam inside band "<<foamInside<<" outside "<<foamOutside<<"\n";
    Check(orthoN>10000&&fadePixels>1000,"fixture: a shore with a fading band");
    Check(orthoError<.02,"top-down: the shore opacity follows the native curve on the true depth");
    Check(rtsN>2000&&rtsError<.04,"RTS camera: the shore opacity follows the native curve on the true depth");
    Check(deepAlpha>.999,"deep water keeps the floor opacity");
    Check(foamInside>100&&foamOutside==0,"foam only in the waterline band");
    Check(plainOpaque,"water without the flag is unchanged (vertex alpha)");
    // photo107: flat-water reflection (flag 8388608) against an analytic wall. Water points in front of the wall reflect a
    // ray; where it reaches the wall within the march (34 units) the reflection adds the wall colour times Fresnel and 0.9,
    // and where it misses the wall it adds nothing. The water pass has no depth test, so points behind the wall are skipped.
    {
     const float wallY=60,wallX0=50,wallX1=250,wallZ0=0,wallZ1=60,marchReach=34;
     std::vector<FixedVertex> wall;addQuad(wall,{wallX0,wallY,wallZ0},{wallX1,wallY,wallZ0},{wallX1,wallY,wallZ1},{wallX0,wallY,wallZ1},{150.0f,wallY-20.0f,30.0f},7);
     const auto wallVB=makeVB(wall);
     Target wallScene=MakeTarget(device.Get(),wide.width,wide.height);
     render(wallScene,true,rtsView,rtsProjection,rtsEye);
     drawObjects(wallScene,wall,wallVB.Get(),rtsView,rtsProjection,rtsEye,false,0,false,false);
     const auto wallDepth=depthCopy(wallScene);
     auto colourCopy=[&](const Target &t){D3D11_TEXTURE2D_DESC d{};t.texture->GetDesc(&d);d.BindFlags=D3D11_BIND_SHADER_RESOURCE;ComPtr<ID3D11Texture2D> copy;HR(device->CreateTexture2D(&d,nullptr,copy.GetAddressOf()),"wall colour copy");
         context->CopyResource(copy.Get(),t.texture.Get());ComPtr<ID3D11ShaderResourceView> view;HR(device->CreateShaderResourceView(copy.Get(),nullptr,view.GetAddressOf()),"wall colour view");return view;};
     const auto wallColour=colourCopy(wallScene);
     const auto wallPixels=read(wallScene);
     Target wallWater=MakeTarget(device.Get(),wide.width,wide.height);
     drawWater(wallWater,wallDepth.Get(),rtsView,rtsProjection,rtsEye,true,0,false,true,wallColour.Get(),false);
     const auto waterPlain=read(wallWater);
     drawWater(wallWater,wallDepth.Get(),rtsView,rtsProjection,rtsEye,true,0,false,true,wallColour.Get(),true);
     const auto waterReflected=read(wallWater);
     DirectX::XMFLOAT3 reflEye;DirectX::XMStoreFloat3(&reflEye,rtsEye);
     const auto screenOf=[&](float x,float y,float z){const auto p=DirectX::XMVector3Project(DirectX::XMVectorSet(x,y,z,1),0,0,static_cast<float>(wide.width),static_cast<float>(wide.height),0,1,rtsProjection,rtsView,DirectX::XMMatrixIdentity());
         return std::array<UINT,2>{static_cast<UINT>(std::clamp(DirectX::XMVectorGetX(p),0.0f,static_cast<float>(wide.width-1))),static_cast<UINT>(std::clamp(DirectX::XMVectorGetY(p),0.0f,static_cast<float>(wide.height-1)))};};
     std::size_t reflHits=0,reflHitsOk=0,reflMisses=0,reflMissesOk=0;double reflExpectedSum=0;
     for(UINT py=0;py<wide.height;py+=2)for(UINT px=0;px<wide.width;px+=2){
         const auto rfFar=DirectX::XMVector3Unproject(DirectX::XMVectorSet(px+.5f,py+.5f,1,0),0,0,static_cast<float>(wide.width),static_cast<float>(wide.height),0,1,rtsProjection,rtsView,DirectX::XMMatrixIdentity());
         DirectX::XMFLOAT3 rfF;DirectX::XMStoreFloat3(&rfF,rfFar);
         const float rdx=rfF.x-reflEye.x,rdy=rfF.y-reflEye.y,rdz=rfF.z-reflEye.z;if(rdz>=0)continue;
         const float rt=(waterZ-reflEye.z)/rdz;const float wx=reflEye.x+rdx*rt,wy=reflEye.y+rdy*rt;
         if(wy>=wallY)continue;
         const float vx=reflEye.x-wx,vy=reflEye.y-wy,vz=reflEye.z-waterZ;const float len=std::sqrt(vx*vx+vy*vy+vz*vz);
         const float tx=vx/len,ty=vy/len,tz=vz/len;
         const float rx=-tx,ry=-ty,rz=tz;
         const float fresnel=.02f+.98f*std::pow(1.0f-std::clamp(tz,0.0f,1.0f),5.0f);
         const std::size_t i=(std::size_t(py)*wide.width+px)*4;
         if(ry>1e-4f){
             const float s=(wallY-wy)/ry;const float ax=wx+rx*s,az=waterZ+rz*s;
             if(s>0&&s<=marchReach&&ax>=wallX0&&ax<=wallX1&&az>=wallZ0&&az<=wallZ1){
                 const auto hit=screenOf(ax,wallY,az);const std::size_t j=(std::size_t(hit[1])*wide.width+hit[0])*4;
                 bool ok=true;double expectedSum=0;
                 for(int k=0;k<3;++k){const float wantReflection=fresnel*.9f*wallPixels[j+k];const float measured=waterReflected[i+k]-waterPlain[i+k];expectedSum+=wantReflection;ok=ok&&std::abs(measured-wantReflection)<=.1f*std::abs(wantReflection)+2e-4f;}
                 ++reflHits;reflHitsOk+=ok?1:0;reflExpectedSum+=expectedSum;
                 continue;
             }
         }
         ++reflMisses;bool none=true;for(int k=0;k<3;++k)none=none&&std::abs(waterReflected[i+k]-waterPlain[i+k])<=2e-4f;reflMissesOk+=none?1:0;
     }
     std::cerr<<"flat water reflection: "<<reflHitsOk<<" of "<<reflHits<<" wall-reflecting points match Fresnel*0.9*wall, "<<reflMissesOk<<" of "<<reflMisses<<" other points add nothing, mean expected sum "<<(reflHits?reflExpectedSum/(3.0*reflHits):0.0)<<"\n";
     Check(reflHits>2000&&reflExpectedSum>0,"fixture: the flat water reflects the wall at useful points");
     Check(reflHitsOk>=.9*reflHits,"flat water adds the wall colour times Fresnel where the reflected ray meets the wall");
     Check(reflMissesOk>=.99*reflMisses,"flat water adds nothing where the reflected ray misses the wall");
    }
    // Previews: the RTS view with the water blended over the ground, a flat native-like
    // opacity (no shore) against the per-pixel shore with foam.
    render(wide,true,rtsView,rtsProjection,rtsEye);drawWater(wide,nullptr,rtsView,rtsProjection,rtsEye,true,0,true,false);save(read(wide),wide,"waterrts-off.bin");
    render(wide,true,rtsView,rtsProjection,rtsEye);drawWater(wide,wideDepth.Get(),rtsView,rtsProjection,rtsEye,true,1,true,false);save(read(wide),wide,"waterrts-on.bin");
    // Cost: the water over the full RTS view at 1080p, interleaved (clock drift cancels).
    auto timeWater=[&](bool shore){double best=1e9;for(unsigned i=0;i<20;++i){context->Begin(grassDisjoint.Get());context->End(grassBegin.Get());drawWater(wideWater,wideDepth.Get(),rtsView,rtsProjection,rtsEye,shore,1,true,false);
        context->End(grassEnd.Get());context->End(grassDisjoint.Get());D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};while(context->GetData(grassDisjoint.Get(),&dj,sizeof(dj),0)==S_FALSE){}
        UINT64 t0=0,t1=0;while(context->GetData(grassBegin.Get(),&t0,8,0)==S_FALSE){}while(context->GetData(grassEnd.Get(),&t1,8,0)==S_FALSE){}
        Check(!dj.Disjoint,"stable GPU clock");best=std::min(best,static_cast<double>(t1-t0)/static_cast<double>(dj.Frequency)*1000);}return best;};
    double waterOffMs=1e9,waterOnMs=1e9;for(unsigned round=0;round<6;++round){waterOffMs=std::min(waterOffMs,timeWater(false));waterOnMs=std::min(waterOnMs,timeWater(true));}
    std::cerr<<"water 1080p full view: native "<<waterOffMs<<" ms, per-pixel shore "<<waterOnMs<<" ms\n";
    report<<"\"water_shore\":{\"ortho_max_error\":"<<orthoError<<",\"ortho_pixels\":"<<orthoN<<",\"fade_pixels\":"<<fadePixels<<",\"rts_max_error\":"<<rtsError<<",\"rts_pixels\":"<<rtsN<<",\"deep_alpha\":"<<deepAlpha
          <<",\"foam_inside\":"<<foamInside<<",\"foam_outside\":"<<foamOutside<<",\"gpu_ms_1080p_off\":"<<waterOffMs<<",\"gpu_ms_1080p_on\":"<<waterOnMs<<"},";

    std::string adapter;for(wchar_t ch:ad.Description){if(!ch)break;adapter.push_back(ch<128?static_cast<char>(ch):'?');}
    report<<"\"unknown_max_rel_diff\":"<<unknownDiff<<",\"cliff_max_rel_diff\":"<<cliffDiff<<",\"worst_luma_ratio\":"<<worstLuma<<",\"worst_chroma_cos\":"<<worstCos
          <<",\"seam_ratio\":"<<seamRatio<<",\"region_mean_max_error\":"<<meanError<<",\"region_mean_samples\":"<<meanChecked
          <<",\"library_load_ms\":"<<loadMs<<",\"library_pumps\":"<<pumps<<",\"library_bytes\":"<<loader.Uploaded()<<",\"ibl\":"<<(ibl?"true":"false")
          <<",\"gpu_ms_1080p_off\":"<<msOff<<",\"gpu_ms_1080p_on\":"<<msOn<<",\"adapter\":\""<<adapter<<"\",\"checks\":"<<checks<<",\"passed\":true}";
    std::cout<<report.str()<<"\n";return 0;
}catch(const std::exception &e){std::cout<<"{\"passed\":false,\"checks\":"<<checks<<",\"error\":\""<<e.what()<<"\"}\n";return 1;}}
