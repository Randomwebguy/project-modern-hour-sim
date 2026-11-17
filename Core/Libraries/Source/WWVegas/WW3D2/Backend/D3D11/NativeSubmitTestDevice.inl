// Generated DX8 API stubs for a headless production Snapshot fixture.
HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObj) override { (void)riid;(void)ppvObj;return E_NOTIMPL; }
ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
ULONG STDMETHODCALLTYPE Release() override { return 1; }
HRESULT STDMETHODCALLTYPE TestCooperativeLevel() override { return E_NOTIMPL; }
UINT STDMETHODCALLTYPE GetAvailableTextureMem() override { return 1; }
HRESULT STDMETHODCALLTYPE ResourceManagerDiscardBytes(DWORD Bytes) override { (void)Bytes;return E_NOTIMPL; }
HRESULT STDMETHODCALLTYPE GetDirect3D(IDirect3D8** ppD3D8) override { (void)ppD3D8;return E_NOTIMPL; }
HRESULT STDMETHODCALLTYPE GetDeviceCaps(D3DCAPS8* pCaps) override { (void)pCaps;return E_NOTIMPL; }
HRESULT STDMETHODCALLTYPE GetDisplayMode(D3DDISPLAYMODE* pMode) override { (void)pMode;return E_NOTIMPL; }
HRESULT STDMETHODCALLTYPE GetCreationParameters(D3DDEVICE_CREATION_PARAMETERS *pParameters) override { (void)pParameters;return E_NOTIMPL; }
HRESULT STDMETHODCALLTYPE SetCursorProperties(UINT XHotSpot, UINT YHotSpot, IDirect3DSurface8* pCursorBitmap) override { (void)XHotSpot;(void)YHotSpot;(void)pCursorBitmap;return E_NOTIMPL; }
void STDMETHODCALLTYPE SetCursorPosition(UINT XScreenSpace, UINT YScreenSpace, DWORD Flags) override { (void)XScreenSpace;(void)YScreenSpace;(void)Flags; }
BOOL STDMETHODCALLTYPE ShowCursor(BOOL bShow) override { (void)bShow;return 1; }
HRESULT STDMETHODCALLTYPE CreateAdditionalSwapChain(D3DPRESENT_PARAMETERS* pPresentationParameters, IDirect3DSwapChain8** pSwapChain) override { (void)pPresentationParameters;(void)pSwapChain;return E_NOTIMPL; }
HRESULT STDMETHODCALLTYPE Reset(D3DPRESENT_PARAMETERS* pPresentationParameters) override { (void)pPresentationParameters;return E_NOTIMPL; }
HRESULT STDMETHODCALLTYPE Present(CONST RECT* pSourceRect, CONST RECT* pDestRect, HWND hDestWindowOverride, CONST RGNDATA* pDirtyRegion) override { (void)pSourceRect;(void)pDestRect;(void)hDestWindowOverride;(void)pDirtyRegion;return E_NOTIMPL; }
HRESULT STDMETHODCALLTYPE GetBackBuffer(UINT BackBuffer, D3DBACKBUFFER_TYPE Type, IDirect3DSurface8** ppBackBuffer) override { (void)BackBuffer;(void)Type;(void)ppBackBuffer;return E_NOTIMPL; }
HRESULT STDMETHODCALLTYPE GetRasterStatus(D3DRASTER_STATUS* pRasterStatus) override { (void)pRasterStatus;return E_NOTIMPL; }
void STDMETHODCALLTYPE SetGammaRamp(DWORD Flags, CONST D3DGAMMARAMP* pRamp) override { (void)Flags;(void)pRamp; }
void STDMETHODCALLTYPE GetGammaRamp(D3DGAMMARAMP* pRamp) override { (void)pRamp; }
HRESULT STDMETHODCALLTYPE CreateTexture(UINT Width, UINT Height, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DTexture8** ppTexture) override { (void)Width;(void)Height;(void)Levels;(void)Usage;(void)Format;(void)Pool;(void)ppTexture;return E_NOTIMPL; }
HRESULT STDMETHODCALLTYPE CreateVolumeTexture(UINT Width, UINT Height, UINT Depth, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DVolumeTexture8** ppVolumeTexture) override { (void)Width;(void)Height;(void)Depth;(void)Levels;(void)Usage;(void)Format;(void)Pool;(void)ppVolumeTexture;return E_NOTIMPL; }
HRESULT STDMETHODCALLTYPE CreateCubeTexture(UINT EdgeLength, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DCubeTexture8** ppCubeTexture) override { (void)EdgeLength;(void)Levels;(void)Usage;(void)Format;(void)Pool;(void)ppCubeTexture;return E_NOTIMPL; }
HRESULT STDMETHODCALLTYPE CreateVertexBuffer(UINT Length, DWORD Usage, DWORD FVF, D3DPOOL Pool, IDirect3DVertexBuffer8** ppVertexBuffer) override { (void)Length;(void)Usage;(void)FVF;(void)Pool;(void)ppVertexBuffer;return E_NOTIMPL; }
HRESULT STDMETHODCALLTYPE CreateIndexBuffer(UINT Length, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DIndexBuffer8** ppIndexBuffer) override { (void)Length;(void)Usage;(void)Format;(void)Pool;(void)ppIndexBuffer;return E_NOTIMPL; }
HRESULT STDMETHODCALLTYPE CreateRenderTarget(UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample, BOOL Lockable, IDirect3DSurface8** ppSurface) override { (void)Width;(void)Height;(void)Format;(void)MultiSample;(void)Lockable;(void)ppSurface;return E_NOTIMPL; }
HRESULT STDMETHODCALLTYPE CreateDepthStencilSurface(UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample, IDirect3DSurface8** ppSurface) override { (void)Width;(void)Height;(void)Format;(void)MultiSample;(void)ppSurface;return E_NOTIMPL; }
HRESULT STDMETHODCALLTYPE CreateImageSurface(UINT Width, UINT Height, D3DFORMAT Format, IDirect3DSurface8** ppSurface) override { (void)Width;(void)Height;(void)Format;(void)ppSurface;return E_NOTIMPL; }
HRESULT STDMETHODCALLTYPE CopyRects(IDirect3DSurface8* pSourceSurface, CONST RECT* pSourceRectsArray, UINT cRects, IDirect3DSurface8* pDestinationSurface, CONST POINT* pDestPointsArray) override { (void)pSourceSurface;(void)pSourceRectsArray;(void)cRects;(void)pDestinationSurface;(void)pDestPointsArray;return E_NOTIMPL; }
HRESULT STDMETHODCALLTYPE UpdateTexture(IDirect3DBaseTexture8* pSourceTexture, IDirect3DBaseTexture8* pDestinationTexture) override { (void)pSourceTexture;(void)pDestinationTexture;return E_NOTIMPL; }
HRESULT STDMETHODCALLTYPE GetFrontBuffer(IDirect3DSurface8* pDestSurface) override { (void)pDestSurface;return E_NOTIMPL; }
HRESULT STDMETHODCALLTYPE SetRenderTarget(IDirect3DSurface8* pRenderTarget, IDirect3DSurface8* pNewZStencil) override { (void)pRenderTarget;(void)pNewZStencil;return E_NOTIMPL; }
HRESULT STDMETHODCALLTYPE GetRenderTarget(IDirect3DSurface8** ppRenderTarget) override { (void)ppRenderTarget;return E_NOTIMPL; }
HRESULT STDMETHODCALLTYPE GetDepthStencilSurface(IDirect3DSurface8** ppZStencilSurface) override { (void)ppZStencilSurface;return E_NOTIMPL; }
HRESULT STDMETHODCALLTYPE BeginScene() override { return E_NOTIMPL; }
HRESULT STDMETHODCALLTYPE EndScene() override { return E_NOTIMPL; }
HRESULT STDMETHODCALLTYPE Clear(DWORD Count, CONST D3DRECT* pRects, DWORD Flags, D3DCOLOR Color, float Z, DWORD Stencil) override { (void)Count;(void)pRects;(void)Flags;(void)Color;(void)Z;(void)Stencil;return E_NOTIMPL; }
HRESULT STDMETHODCALLTYPE SetTransform(D3DTRANSFORMSTATETYPE State, CONST D3DMATRIX* pMatrix) override { (void)State;(void)pMatrix;return E_NOTIMPL; }
HRESULT STDMETHODCALLTYPE GetTransform(D3DTRANSFORMSTATETYPE State, D3DMATRIX* pMatrix) override { ++reads;if(fail)return E_FAIL;int index=State==D3DTS_WORLD?0:State==D3DTS_VIEW?1:State==D3DTS_PROJECTION?2:3+int(State-D3DTS_TEXTURE0);if(index<0||index>=11)return E_FAIL;*pMatrix=state.transforms[unsigned(index)];return S_OK; }
HRESULT STDMETHODCALLTYPE MultiplyTransform(D3DTRANSFORMSTATETYPE argument0, CONST D3DMATRIX* argument1) override { (void)argument0;(void)argument1;return E_NOTIMPL; }
HRESULT STDMETHODCALLTYPE SetViewport(CONST D3DVIEWPORT8* pViewport) override { (void)pViewport;return E_NOTIMPL; }
HRESULT STDMETHODCALLTYPE GetViewport(D3DVIEWPORT8* pViewport) override { ++reads;if(fail)return E_FAIL;*pViewport=state.viewport;return S_OK; }
HRESULT STDMETHODCALLTYPE SetMaterial(CONST D3DMATERIAL8* pMaterial) override { (void)pMaterial;return E_NOTIMPL; }
HRESULT STDMETHODCALLTYPE GetMaterial(D3DMATERIAL8* pMaterial) override { ++reads;if(fail)return E_FAIL;*pMaterial=state.material;return S_OK; }
HRESULT STDMETHODCALLTYPE SetLight(DWORD Index, CONST D3DLIGHT8* argument1) override { (void)Index;(void)argument1;return E_NOTIMPL; }
HRESULT STDMETHODCALLTYPE GetLight(DWORD Index, D3DLIGHT8* argument1) override { ++reads;if(Index>=4||!state.lightValid[Index])return E_FAIL;*argument1=state.lights[Index];return S_OK; }
HRESULT STDMETHODCALLTYPE LightEnable(DWORD Index, BOOL Enable) override { (void)Index;(void)Enable;return E_NOTIMPL; }
HRESULT STDMETHODCALLTYPE GetLightEnable(DWORD Index, BOOL* pEnable) override { ++reads;if(Index>=4)return E_FAIL;*pEnable=state.enabled[Index];return S_OK; }
HRESULT STDMETHODCALLTYPE SetClipPlane(DWORD Index, CONST float* pPlane) override { (void)Index;(void)pPlane;return E_NOTIMPL; }
HRESULT STDMETHODCALLTYPE GetClipPlane(DWORD Index, float* pPlane) override { (void)Index;(void)pPlane;return E_NOTIMPL; }
HRESULT STDMETHODCALLTYPE SetRenderState(D3DRENDERSTATETYPE State, DWORD Value) override { (void)State;(void)Value;return E_NOTIMPL; }
HRESULT STDMETHODCALLTYPE GetRenderState(D3DRENDERSTATETYPE State, DWORD* pValue) override { (void)State;(void)pValue;return E_NOTIMPL; }
HRESULT STDMETHODCALLTYPE BeginStateBlock() override { return E_NOTIMPL; }
HRESULT STDMETHODCALLTYPE EndStateBlock(DWORD* pToken) override { (void)pToken;return E_NOTIMPL; }
HRESULT STDMETHODCALLTYPE ApplyStateBlock(DWORD Token) override { (void)Token;return E_NOTIMPL; }
HRESULT STDMETHODCALLTYPE CaptureStateBlock(DWORD Token) override { (void)Token;return E_NOTIMPL; }
HRESULT STDMETHODCALLTYPE DeleteStateBlock(DWORD Token) override { (void)Token;return E_NOTIMPL; }
HRESULT STDMETHODCALLTYPE CreateStateBlock(D3DSTATEBLOCKTYPE Type, DWORD* pToken) override { (void)Type;(void)pToken;return E_NOTIMPL; }
HRESULT STDMETHODCALLTYPE SetClipStatus(CONST D3DCLIPSTATUS8* pClipStatus) override { (void)pClipStatus;return E_NOTIMPL; }
HRESULT STDMETHODCALLTYPE GetClipStatus(D3DCLIPSTATUS8* pClipStatus) override { (void)pClipStatus;return E_NOTIMPL; }
HRESULT STDMETHODCALLTYPE GetTexture(DWORD Stage, IDirect3DBaseTexture8** ppTexture) override { (void)Stage;(void)ppTexture;return E_NOTIMPL; }
HRESULT STDMETHODCALLTYPE SetTexture(DWORD Stage, IDirect3DBaseTexture8* pTexture) override { (void)Stage;(void)pTexture;return E_NOTIMPL; }
HRESULT STDMETHODCALLTYPE GetTextureStageState(DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD* pValue) override { (void)Stage;(void)Type;(void)pValue;return E_NOTIMPL; }
HRESULT STDMETHODCALLTYPE SetTextureStageState(DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD Value) override { (void)Stage;(void)Type;(void)Value;return E_NOTIMPL; }
HRESULT STDMETHODCALLTYPE ValidateDevice(DWORD* pNumPasses) override { (void)pNumPasses;return E_NOTIMPL; }
HRESULT STDMETHODCALLTYPE GetInfo(DWORD DevInfoID, void* pDevInfoStruct, DWORD DevInfoStructSize) override { (void)DevInfoID;(void)pDevInfoStruct;(void)DevInfoStructSize;return E_NOTIMPL; }
HRESULT STDMETHODCALLTYPE SetPaletteEntries(UINT PaletteNumber, CONST PALETTEENTRY* pEntries) override { (void)PaletteNumber;(void)pEntries;return E_NOTIMPL; }
HRESULT STDMETHODCALLTYPE GetPaletteEntries(UINT PaletteNumber, PALETTEENTRY* pEntries) override { (void)PaletteNumber;(void)pEntries;return E_NOTIMPL; }
HRESULT STDMETHODCALLTYPE SetCurrentTexturePalette(UINT PaletteNumber) override { (void)PaletteNumber;return E_NOTIMPL; }
HRESULT STDMETHODCALLTYPE GetCurrentTexturePalette(UINT *PaletteNumber) override { (void)PaletteNumber;return E_NOTIMPL; }
HRESULT STDMETHODCALLTYPE DrawPrimitive(D3DPRIMITIVETYPE PrimitiveType, UINT StartVertex, UINT PrimitiveCount) override { (void)PrimitiveType;(void)StartVertex;(void)PrimitiveCount;return E_NOTIMPL; }
HRESULT STDMETHODCALLTYPE DrawIndexedPrimitive(D3DPRIMITIVETYPE argument0, UINT minIndex, UINT NumVertices, UINT startIndex, UINT primCount) override { (void)argument0;(void)minIndex;(void)NumVertices;(void)startIndex;(void)primCount;return E_NOTIMPL; }
HRESULT STDMETHODCALLTYPE DrawPrimitiveUP(D3DPRIMITIVETYPE PrimitiveType, UINT PrimitiveCount, CONST void* pVertexStreamZeroData, UINT VertexStreamZeroStride) override { (void)PrimitiveType;(void)PrimitiveCount;(void)pVertexStreamZeroData;(void)VertexStreamZeroStride;return E_NOTIMPL; }
HRESULT STDMETHODCALLTYPE DrawIndexedPrimitiveUP(D3DPRIMITIVETYPE PrimitiveType, UINT MinVertexIndex, UINT NumVertexIndices, UINT PrimitiveCount, CONST void* pIndexData, D3DFORMAT IndexDataFormat, CONST void* pVertexStreamZeroData, UINT VertexStreamZeroStride) override { (void)PrimitiveType;(void)MinVertexIndex;(void)NumVertexIndices;(void)PrimitiveCount;(void)pIndexData;(void)IndexDataFormat;(void)pVertexStreamZeroData;(void)VertexStreamZeroStride;return E_NOTIMPL; }
HRESULT STDMETHODCALLTYPE ProcessVertices(UINT SrcStartIndex, UINT DestIndex, UINT VertexCount, IDirect3DVertexBuffer8* pDestBuffer, DWORD Flags) override { (void)SrcStartIndex;(void)DestIndex;(void)VertexCount;(void)pDestBuffer;(void)Flags;return E_NOTIMPL; }
HRESULT STDMETHODCALLTYPE CreateVertexShader(CONST DWORD* pDeclaration, CONST DWORD* pFunction, DWORD* pHandle, DWORD Usage) override { (void)pDeclaration;(void)pFunction;(void)pHandle;(void)Usage;return E_NOTIMPL; }
HRESULT STDMETHODCALLTYPE SetVertexShader(DWORD Handle) override { (void)Handle;return E_NOTIMPL; }
HRESULT STDMETHODCALLTYPE GetVertexShader(DWORD* pHandle) override { ++reads;if(fail)return E_FAIL;*pHandle=state.vertexShader;return S_OK; }
HRESULT STDMETHODCALLTYPE DeleteVertexShader(DWORD Handle) override { (void)Handle;return E_NOTIMPL; }
HRESULT STDMETHODCALLTYPE SetVertexShaderConstant(DWORD Register, CONST void* pConstantData, DWORD ConstantCount) override { (void)Register;(void)pConstantData;(void)ConstantCount;return E_NOTIMPL; }
HRESULT STDMETHODCALLTYPE GetVertexShaderConstant(DWORD Register, void* pConstantData, DWORD ConstantCount) override { (void)Register;(void)pConstantData;(void)ConstantCount;return E_NOTIMPL; }
HRESULT STDMETHODCALLTYPE GetVertexShaderDeclaration(DWORD Handle, void* pData, DWORD* pSizeOfData) override { (void)Handle;(void)pData;(void)pSizeOfData;return E_NOTIMPL; }
HRESULT STDMETHODCALLTYPE GetVertexShaderFunction(DWORD Handle, void* pData, DWORD* pSizeOfData) override { (void)Handle;(void)pData;(void)pSizeOfData;return E_NOTIMPL; }
HRESULT STDMETHODCALLTYPE SetStreamSource(UINT StreamNumber, IDirect3DVertexBuffer8* pStreamData, UINT Stride) override { (void)StreamNumber;(void)pStreamData;(void)Stride;return E_NOTIMPL; }
HRESULT STDMETHODCALLTYPE GetStreamSource(UINT StreamNumber, IDirect3DVertexBuffer8** ppStreamData, UINT* pStride) override { (void)StreamNumber;(void)ppStreamData;(void)pStride;return E_NOTIMPL; }
HRESULT STDMETHODCALLTYPE SetIndices(IDirect3DIndexBuffer8* pIndexData, UINT BaseVertexIndex) override { (void)pIndexData;(void)BaseVertexIndex;return E_NOTIMPL; }
HRESULT STDMETHODCALLTYPE GetIndices(IDirect3DIndexBuffer8** ppIndexData, UINT* pBaseVertexIndex) override { (void)ppIndexData;(void)pBaseVertexIndex;return E_NOTIMPL; }
HRESULT STDMETHODCALLTYPE CreatePixelShader(CONST DWORD* pFunction, DWORD* pHandle) override { (void)pFunction;(void)pHandle;return E_NOTIMPL; }
HRESULT STDMETHODCALLTYPE SetPixelShader(DWORD Handle) override { (void)Handle;return E_NOTIMPL; }
HRESULT STDMETHODCALLTYPE GetPixelShader(DWORD* pHandle) override { ++reads;if(fail)return E_FAIL;*pHandle=state.pixelShader;return S_OK; }
HRESULT STDMETHODCALLTYPE DeletePixelShader(DWORD Handle) override { (void)Handle;return E_NOTIMPL; }
HRESULT STDMETHODCALLTYPE SetPixelShaderConstant(DWORD Register, CONST void* pConstantData, DWORD ConstantCount) override { (void)Register;(void)pConstantData;(void)ConstantCount;return E_NOTIMPL; }
HRESULT STDMETHODCALLTYPE GetPixelShaderConstant(DWORD Register, void* pConstantData, DWORD ConstantCount) override { (void)Register;(void)pConstantData;(void)ConstantCount;return E_NOTIMPL; }
HRESULT STDMETHODCALLTYPE GetPixelShaderFunction(DWORD Handle, void* pData, DWORD* pSizeOfData) override { (void)Handle;(void)pData;(void)pSizeOfData;return E_NOTIMPL; }
HRESULT STDMETHODCALLTYPE DrawRectPatch(UINT Handle, CONST float* pNumSegs, CONST D3DRECTPATCH_INFO* pRectPatchInfo) override { (void)Handle;(void)pNumSegs;(void)pRectPatchInfo;return E_NOTIMPL; }
HRESULT STDMETHODCALLTYPE DrawTriPatch(UINT Handle, CONST float* pNumSegs, CONST D3DTRIPATCH_INFO* pTriPatchInfo) override { (void)Handle;(void)pNumSegs;(void)pTriPatchInfo;return E_NOTIMPL; }
HRESULT STDMETHODCALLTYPE DeletePatch(UINT Handle) override { (void)Handle;return E_NOTIMPL; }
