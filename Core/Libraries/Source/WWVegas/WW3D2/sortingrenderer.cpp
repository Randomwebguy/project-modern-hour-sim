/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2025 Electronic Arts Inc.
**
**	This program is free software: you can redistribute it and/or modify
**	it under the terms of the GNU General Public License as published by
**	the Free Software Foundation, either version 3 of the License, or
**	(at your option) any later version.
**
**	This program is distributed in the hope that it will be useful,
**	but WITHOUT ANY WARRANTY; without even the implied warranty of
**	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
**	GNU General Public License for more details.
**
**	You should have received a copy of the GNU General Public License
**	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

/***********************************************************************************************
 ***              C O N F I D E N T I A L  ---  W E S T W O O D  S T U D I O S               ***
 ***********************************************************************************************
 *                                                                                             *
 *                 Project Name : ww3d                                                         *
 *                                                                                             *
 *                     $Archive:: /Commando/Code/ww3d2/sortingrenderer.cpp                    $*
 *                                                                                             *
 *              Original Author:: Greg Hjelstrom                                               *
 *                                                                                             *
 *                       Author : Kenny Mitchell                                               *
 *                                                                                             *
 *                     $Modtime:: 06/27/02 1:27p                                              $*
 *                                                                                             *
 *                    $Revision:: 2                                                           $*
 *                                                                                             *
 * 06/26/02 KM Matrix name change to avoid MAX conflicts                                       *
 * 06/27/02 KM Changes to max texture stage caps																*
 *---------------------------------------------------------------------------------------------*
 * Functions:                                                                                  *
 * - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

#include "sortingrenderer.h"
#include "dx8vertexbuffer.h"
#include "dx8indexbuffer.h"
#include "dx8wrapper.h"
#include "vertmaterial.h"
#include "texture.h"
#include "d3d8.h"
#include "d3dx8math.h"
#include "statistics.h"
#include <WWDebug/wwprofile.h>
#include <algorithm>
#include <list>
#include "Backend/D3D11/NativeSceneIdentity.h"
#include "Backend/D3D11/OpaqueSortPolicy.h"
#include "Backend/D3D11/NativeSubmitGate.h"
#include "Backend/D3D11/LoadTelemetry.h"
#include "Backend/D3D11/OrderedSortBatch.h"
#include "Backend/D3D11/SortedWorldTranslation.h"
#include "Backend/D3D11/StableDepthSort.h"
#include "Backend/D3D11/CaptureWorkTelemetry.h"


bool SortingRendererClass::_EnableTriangleDraw=true;
static unsigned DEFAULT_SORTING_POLY_COUNT = 16384;	// initial allocation, grows on demand
static unsigned DEFAULT_SORTING_VERTEX_COUNT = 32768;	// initial allocation, grows on demand

void SortingRendererClass::SetMinVertexBufferSize( unsigned val )
{
	DEFAULT_SORTING_VERTEX_COUNT = val;
	DEFAULT_SORTING_POLY_COUNT = val/2;	//typically have 2:1 vertex:triangle ratio.
}

struct ShortVectorIStruct
{
	unsigned int i;
	unsigned int j;
	unsigned int k;
};

struct TempIndexStruct
{
	ShortVectorIStruct tri;
	unsigned int idx;
	float z;
};

bool operator <(const TempIndexStruct &l, const TempIndexStruct &r) { return l.z < r.z; }
bool operator <=(const TempIndexStruct &l, const TempIndexStruct &r) { return l.z <= r.z; }
bool operator >(const TempIndexStruct &l, const TempIndexStruct &r) { return l.z > r.z; }
bool operator >=(const TempIndexStruct &l, const TempIndexStruct &r) { return l.z >= r.z; }
bool operator ==(const TempIndexStruct &l, const TempIndexStruct &r) { return l.z == r.z; }
// ----------------------------------------------------------------------------
static
void InsertionSort(TempIndexStruct *begin, TempIndexStruct *end)
{
	for (TempIndexStruct *iter = begin + 1; iter < end; ++iter) {
		TempIndexStruct val = iter[0];
		TempIndexStruct *insert = iter;
		while (insert != begin && insert[-1] > val) {
			insert[0] = insert[-1];
			insert -= 1;
		}
		insert[0] = val;
	}
}

// ----------------------------------------------------------------------------
static
void Sort(TempIndexStruct *begin, TempIndexStruct *end)
{
	const int diff = end - begin;
	if (diff <= 16) {
		// Insertion sort has less overhead for small arrays
		InsertionSort(begin, end);
	} else {
		// Choose the median of begin, mid, and (end - 1) as the partitioning element.
		// Rearrange so that *(begin + 1) <= *begin <= *(end - 1).  These will be guard
		// elements.
		TempIndexStruct *mid = begin + diff/2;
		std::swap(mid[0], begin[1]);
		if (begin[1] > end[-1]) {
			std::swap(begin[1], end[-1]);
		}
		if (begin[0] > end[-1]) {
			std::swap(begin[0], end[-1]);
		}
		if (begin[1] > begin[0]) {
			std::swap(begin[1], begin[0]);
		}

		// *begin is now the partitioning element
		TempIndexStruct *begin1 = begin + 1;	// TODO: Temp fix until I find out who is passing me NaN
		TempIndexStruct *end1 = end - 1;			// TODO: Temp fix until I find out who is passing me NaN
		TempIndexStruct *left = begin + 1;
		TempIndexStruct *right = end - 1;
		for (;;) {
#if 0		// TODO: Temp fix until I find out who is passing me NaN.
			do ++left; while (left[0] < begin[0]);		// Scan up to find element >= than partition
			do --right; while (right[0] > begin[0]);	// Scan down to find element <= than partition
#else
			do ++left; while (left < end1 && left[0] < begin[0]);		// Scan up to find element >= than partition
			do --right; while (right > begin1 && right[0] > begin[0]);	// Scan down to find element <= than partition
#endif
			if (right < left) break;									// Pointers crossed.  Partitioning completed.
			std::swap(left[0], right[0]);							// Exchange elements.
		}
		std::swap(begin[0], right[0]);							// Insert partition element

		// Sort the smaller subarray first then the larger
		if (right - begin > end - (right + 1)) {
			Sort(right + 1, end);
			Sort(begin, right);
		} else {
			Sort(begin, right);
			Sort(right + 1, end);
		}
	}
}

// ----------------------------------------------------------------------------

class SortingNodeStruct
{
	W3DMPO_CODE(SortingNodeStruct)

public:
	RenderStateStruct sorting_state;
	GenZH::Modern::NativeMeshSource native_source;

	Vector3 transformed_center;
	W3DIndexCount start_index;			// First index used in the ib
	W3DIndexCount polygon_count;			// Polygon count to process (3 indices = one polygon)
	unsigned int min_vertex_index;		// First index used in the vb
	unsigned int vertex_count;			// Number of vertices used in vb
};

typedef std::list<SortingNodeStruct*> SortingNodeStructList;
static SortingNodeStructList sorted_list;
static SortingNodeStructList unsorted_list;
static SortingNodeStructList clean_list;
static unsigned total_sorting_vertices;

static SortingNodeStruct* Get_Sorting_Struct()
{
	if (!clean_list.empty()) {
		SortingNodeStruct* state = clean_list.front();
		clean_list.pop_front();
		return state;
	}
	return W3DNEW SortingNodeStruct();
}

// ----------------------------------------------------------------------------
//
// Temporary arrays for the sorting system
//
// ----------------------------------------------------------------------------

static TempIndexStruct* temp_index_array;
static unsigned temp_index_array_count;

static TempIndexStruct* Get_Temp_Index_Array(unsigned count)
{
	if (count < DEFAULT_SORTING_POLY_COUNT)
		count = DEFAULT_SORTING_POLY_COUNT;
	if (count>temp_index_array_count) {
		delete[] temp_index_array;
		temp_index_array=W3DNEWARRAY TempIndexStruct[count];
		temp_index_array_count=count;
	}
	return temp_index_array;
}

// ----------------------------------------------------------------------------
//
// Insert triangles to the sorting system.
//
// ----------------------------------------------------------------------------

void SortingRendererClass::Insert_Triangles(
	const SphereClass& bounding_sphere,
	W3DIndexCount start_index,
	W3DIndexCount polygon_count,
	unsigned int min_vertex_index,
	unsigned int vertex_count)
{
#ifdef RTS_HAS_D3D11_BRIDGE
 // The mesh SORT flag applies to every category when even one window blends.
 // Sorting its opaque armor by individual triangle interleaves categories and
 // expands a handful of draws into hundreds of thousands. Depth-writing opaque
 // categories stay in original index order; transparent/unsafe stencil/custom passes
 // retain the original sorter and its ordering contract.
 if (GenZH::Modern::nativeMeshDispatch.GeometryEnabled()&&!GenZH::Modern::opaqueSortReference) {
  DX8Wrapper::Apply_Render_State_Changes(true);
  auto *device=DX8Wrapper::_Get_D3D_Device8();
  DWORD blend=TRUE,src=0,dst=0,depth=0,write=0,compare=0,stencil=TRUE,pixel=1;
  DWORD stencilFunction=0,stencilFail=0,stencilDepthFail=0,stencilPass=0;
  bool captured=SUCCEEDED(device->GetRenderState(D3DRS_ALPHABLENDENABLE,&blend))&&
   SUCCEEDED(device->GetRenderState(D3DRS_SRCBLEND,&src))&&SUCCEEDED(device->GetRenderState(D3DRS_DESTBLEND,&dst))&&
   SUCCEEDED(device->GetRenderState(D3DRS_ZENABLE,&depth))&&SUCCEEDED(device->GetRenderState(D3DRS_ZWRITEENABLE,&write))&&
   SUCCEEDED(device->GetRenderState(D3DRS_ZFUNC,&compare))&&SUCCEEDED(device->GetRenderState(D3DRS_STENCILENABLE,&stencil))&&
   SUCCEEDED(device->GetPixelShader(&pixel))&&
   SUCCEEDED(device->GetRenderState(D3DRS_STENCILFUNC,&stencilFunction))&&
   SUCCEEDED(device->GetRenderState(D3DRS_STENCILFAIL,&stencilFail))&&
   SUCCEEDED(device->GetRenderState(D3DRS_STENCILZFAIL,&stencilDepthFail))&&
   SUCCEEDED(device->GetRenderState(D3DRS_STENCILPASS,&stencilPass));
  if(captured&&GenZH::Modern::OpaqueSortBypass(blend,src,dst,depth,write,compare,stencil,pixel,
       stencilFunction,stencilFail,stencilDepthFail,stencilPass)) {
   static unsigned audit=0;
   if(audit++<64)GenZHLoad::Record("opaque_sort_batch",std::to_string(polygon_count)+" triangles retained in one category draw",0,static_cast<std::size_t>(polygon_count)*12);
   // Sorting buffers are CPU-owned, but a rigid category can still borrow the
   // original immutable model arrays. Submit them before any dynamic repack.
   // Unsupported layouts/custom passes keep their exact compatibility path.
   if(!DX8Wrapper::Try_Native_Geometry_Triangles(start_index,polygon_count,min_vertex_index,vertex_count))
    DX8Wrapper::Draw_Triangles(start_index,polygon_count,min_vertex_index,vertex_count);
   return;
  }
  // Bounded diagnostics expose real category states, rather than inferring
  // eligibility from a small fixture or the final UI state after a match.
  static unsigned rejected=0;
  if(rejected++<64){
   const auto source=GenZH::Modern::GetNativeMeshSource();
   GenZHLoad::Record("opaque_sort_reject","mesh="+std::to_string(source.mesh)+" triangles="+std::to_string(polygon_count)+
    " capture="+std::to_string(captured)+" blend="+std::to_string(blend)+" depth="+std::to_string(depth)+
    " write="+std::to_string(write)+" compare="+std::to_string(compare)+" stencil="+std::to_string(stencil)+
    " stencilFunc="+std::to_string(stencilFunction)+" fail="+std::to_string(stencilFail)+
    " zfail="+std::to_string(stencilDepthFail)+" pass="+std::to_string(stencilPass)+" shader="+std::to_string(pixel),0,0);
  }
 }
#endif
	if (!WW3D::Is_Sorting_Enabled()) {
		DX8Wrapper::Draw_Triangles(start_index,polygon_count,min_vertex_index,vertex_count);
		return;
	}

	SNAPSHOT_SAY(("SortingRenderer::Insert(start_i: %d, polygons : %d, min_vi: %d, vertex_count: %d)",
		start_index,polygon_count,min_vertex_index,vertex_count));


	DX8_RECORD_SORTING_RENDER(polygon_count,vertex_count);

	SortingNodeStruct* state=Get_Sorting_Struct();

	DX8Wrapper::Get_Render_State(state->sorting_state);
	state->native_source=GenZH::Modern::GetNativeMeshSource();
	state->native_source.sorted=true;

 	WWASSERT(
		((state->sorting_state.index_buffer_type==BUFFER_TYPE_SORTING || state->sorting_state.index_buffer_type==BUFFER_TYPE_DYNAMIC_SORTING) &&
		(state->sorting_state.vertex_buffer_types[0]==BUFFER_TYPE_SORTING || state->sorting_state.vertex_buffer_types[0]==BUFFER_TYPE_DYNAMIC_SORTING)));


	state->start_index=start_index;
	state->polygon_count=polygon_count;
	state->min_vertex_index=min_vertex_index;
	state->vertex_count=vertex_count;

	if (bounding_sphere.Is_Valid())
	{
		D3DXMATRIX mtx=(D3DXMATRIX&)state->sorting_state.world*(D3DXMATRIX&)state->sorting_state.view;
		D3DXVECTOR3 vec=(D3DXVECTOR3&)bounding_sphere.Center;
		D3DXVECTOR4 transformed_vec;
		D3DXVec3Transform(
			&transformed_vec,
			&vec,
			&mtx);
		state->transformed_center=Vector3(transformed_vec[0],transformed_vec[1],transformed_vec[2]);

		Insert_To_Sorted_List(state);
	}
	else
	{
		// TheSuperHackers @perf stephanmeesters 04/07/2026 Nodes without bounding information do not require sorting.
		state->transformed_center = Vector3(0.0f, 0.0f, 0.0f);
		unsorted_list.push_back(state);
	}

#ifdef DEBUG_CRASHING
	SortingVertexBufferClass* vertex_buffer=static_cast<SortingVertexBufferClass*>(state->sorting_state.vertex_buffers[0]);
	WWASSERT(vertex_buffer);
	WWASSERT(state->vertex_count<=vertex_buffer->Get_Vertex_Count());

	unsigned int* indices=nullptr;
	SortingIndexBufferClass* index_buffer=static_cast<SortingIndexBufferClass*>(state->sorting_state.index_buffer);
	WWASSERT(index_buffer);
	indices=index_buffer->index_buffer;
	WWASSERT(indices);
	indices+=state->start_index;
	indices+=state->sorting_state.iba_offset;

	for (int i=0;i<state->polygon_count;++i) {
		unsigned int idx1=indices[i*3]-state->min_vertex_index;
		unsigned int idx2=indices[i*3+1]-state->min_vertex_index;
		unsigned int idx3=indices[i*3+2]-state->min_vertex_index;
		WWASSERT(idx1<state->vertex_count);
		WWASSERT(idx2<state->vertex_count);
		WWASSERT(idx3<state->vertex_count);
	}
#endif // DEBUG_CRASHING
}

// ----------------------------------------------------------------------------
//
// Insert triangles to the sorting system, with no bounding information.
//
// ----------------------------------------------------------------------------

void SortingRendererClass::Insert_Triangles(
	W3DIndexCount start_index,
	W3DIndexCount polygon_count,
	unsigned int min_vertex_index,
	unsigned int vertex_count)
{
	Insert_Triangles(SphereClass(),start_index,polygon_count,min_vertex_index,vertex_count);
}

// ----------------------------------------------------------------------------
//
// Flush all sorting polygons.
//
// ----------------------------------------------------------------------------

void Release_Refs(SortingNodeStruct* state)
{
	int i;
	for (i=0;i<MAX_VERTEX_STREAMS;++i) {
		REF_PTR_RELEASE(state->sorting_state.vertex_buffers[i]);
	}
	REF_PTR_RELEASE(state->sorting_state.index_buffer);
	REF_PTR_RELEASE(state->sorting_state.material);
	for (i=0;i<DX8Wrapper::Get_Current_Caps()->Get_Max_Textures_Per_Pass();++i)
	{
		REF_PTR_RELEASE(state->sorting_state.Textures[i]);
	}
}

static unsigned overlapping_node_count;
static unsigned overlapping_polygon_count;
static unsigned overlapping_vertex_count;
static const unsigned MAX_OVERLAPPING_NODES=4096;
static SortingNodeStruct* overlapping_nodes[MAX_OVERLAPPING_NODES];

// ----------------------------------------------------------------------------

void SortingRendererClass::Insert_To_Sorted_List(SortingNodeStruct *state)
{
	/// @todo lorenzen sez use a bucket sort here... and stop copying so much data so many times

	for (SortingNodeStructList::iterator node = sorted_list.begin(); node != sorted_list.end(); ++node)
	{
		if (state->transformed_center.Z > (*node)->transformed_center.Z) {
			sorted_list.insert(node, state);
			return;
		}
	}

	sorted_list.push_back(state);
}

// ----------------------------------------------------------------------------

void SortingRendererClass::Insert_To_Sorting_Pool(SortingNodeStruct* state)
{
	if (overlapping_node_count>=MAX_OVERLAPPING_NODES) {
		Release_Refs(state);
		delete state;
		WWASSERT(0);
		return;
	}

	overlapping_nodes[overlapping_node_count]=state;
	overlapping_vertex_count+=state->vertex_count;
	overlapping_polygon_count+=state->polygon_count;
	overlapping_node_count++;
}

// ----------------------------------------------------------------------------
//static unsigned prevLight = 0xffffffff;

static void Apply_Render_State(RenderStateStruct& render_state)
{
	DX8Wrapper::Set_Shader(render_state.shader);

	DX8Wrapper::Set_Material(render_state.material);

	for (int i=0;i<DX8Wrapper::Get_Current_Caps()->Get_Max_Textures_Per_Pass();++i)
	{
		DX8Wrapper::Set_Texture(i,render_state.Textures[i]);
	}

	DX8Wrapper::_Set_DX8_Transform(D3DTS_WORLD,render_state.world);
	DX8Wrapper::_Set_DX8_Transform(D3DTS_VIEW,render_state.view);


	if (!render_state.material->Get_Lighting())
		return;	//no point changing lights if they are ignored.
  //prevLight = render_state.lightsHash;

	if (render_state.LightEnable[0]) {
		DX8Wrapper::Set_DX8_Light(0,&render_state.Lights[0]);
		if (render_state.LightEnable[1]) {
			DX8Wrapper::Set_DX8_Light(1,&render_state.Lights[1]);
			if (render_state.LightEnable[2]) {
				DX8Wrapper::Set_DX8_Light(2,&render_state.Lights[2]);
				if (render_state.LightEnable[3]) {
					DX8Wrapper::Set_DX8_Light(3,&render_state.Lights[3]);
				}
				else {
					DX8Wrapper::Set_DX8_Light(3,nullptr);
				}
			}
			else {
				DX8Wrapper::Set_DX8_Light(2,nullptr);
			}
		}
		else {
			DX8Wrapper::Set_DX8_Light(1,nullptr);
		}
	}
	else {
		DX8Wrapper::Set_DX8_Light(0,nullptr);
	}


}

// ----------------------------------------------------------------------------

void SortingRendererClass::Flush_Sorting_Pool()
{
	if (!overlapping_node_count) return;

    DWORD fixedPixelProgram=1;
    const bool mayTranslate=GenZH::Modern::nativeMeshDispatch.GeometryEnabled()&&
        !GenZH::Modern::orderedSortReference&&!GenZH::Modern::opaqueSortReference&&
        !GenZH::Modern::sortedSubmitReference&&
        SUCCEEDED(DX8Wrapper::_Get_D3D_Device8()->GetPixelShader(&fixedPixelProgram))&&!fixedPixelProgram;

	SNAPSHOT_SAY(("SortingSystem - Flush"));

	// Fill dynamic index buffer with sorting index buffer vertices
	TempIndexStruct* tis=Get_Temp_Index_Array(overlapping_polygon_count);

	unsigned vertexAllocCount = overlapping_vertex_count;
	if (DynamicVBAccessClass::Get_Default_Vertex_Count() < DEFAULT_SORTING_VERTEX_COUNT)
		vertexAllocCount = DEFAULT_SORTING_VERTEX_COUNT;	//make sure that we force the DX8 dynamic vertex buffer to maximum size
	if (overlapping_vertex_count > vertexAllocCount)
		vertexAllocCount = overlapping_vertex_count;
	WWASSERT(DEFAULT_SORTING_VERTEX_COUNT == 1 || vertexAllocCount <= DEFAULT_SORTING_VERTEX_COUNT);
	DynamicVBAccessClass dyn_vb_access(BUFFER_TYPE_DYNAMIC_DX8,dynamic_fvf_type,vertexAllocCount/*overlapping_vertex_count*/);
	{
        GenZH::Modern::CaptureWorkScope depthScope(GenZH::Modern::CaptureWork::DepthPrepare);
		DynamicVBAccessClass::WriteLockClass lock(&dyn_vb_access);
		VertexFormatXYZNDUV2* dest_verts=(VertexFormatXYZNDUV2 *)lock.Get_Formatted_Vertex_Array();

		unsigned polygon_array_offset=0;
		unsigned vertex_array_offset=0;
		for (unsigned node_id=0;node_id<overlapping_node_count;++node_id) {
			SortingNodeStruct* state=overlapping_nodes[node_id];
			VertexFormatXYZNDUV2* src_verts=nullptr;
			SortingVertexBufferClass* vertex_buffer=static_cast<SortingVertexBufferClass*>(state->sorting_state.vertex_buffers[0]);
			WWASSERT(vertex_buffer);
			src_verts=vertex_buffer->VertexBuffer;
			WWASSERT(src_verts);
			src_verts+=state->sorting_state.vba_offset;
			src_verts+=state->sorting_state.index_base_offset;
			src_verts+=state->min_vertex_index;

			// If you have a crash in here and "dest_verts" points to illegal memory area,
			// it is because D3D is in illegal state, and the only known cure is rebooting.
			// This illegal state is usually caused by Quake3-engine powered games such as MOHAA.
            float translation[3]{};
            const auto &source=state->native_source;
            const bool translated=mayTranslate&&source.Valid()&&!source.skinned&&source.pass!=UINT32_MAX&&
                state->sorting_state.material&&state->sorting_state.material->Is_Static_Draw_Equivalent(*state->sorting_state.material)&&
                GenZH::Modern::SortedWorldTranslation(state->sorting_state.world,translation);
            memcpy(dest_verts, src_verts, sizeof(VertexFormatXYZNDUV2)*state->vertex_count);
            if(translated){
                GenZH::Modern::TranslateSortedVertices(dest_verts,state->vertex_count,translation);
                ++GenZH::Modern::orderedSortCounts.translatedNodes;
                GenZH::Modern::orderedSortCounts.translatedVertices+=state->vertex_count;
            }
			dest_verts += state->vertex_count;

			D3DXMATRIX d3d_mtx=(D3DXMATRIX&)state->sorting_state.world*(D3DXMATRIX&)state->sorting_state.view;
			const Matrix4x4& mtx=(const Matrix4x4&)d3d_mtx;

			unsigned int* indices=nullptr;
			SortingIndexBufferClass* index_buffer=static_cast<SortingIndexBufferClass*>(state->sorting_state.index_buffer);
			WWASSERT(index_buffer);
			indices=index_buffer->index_buffer;
			WWASSERT(indices);
			indices+=state->start_index;
			indices+=state->sorting_state.iba_offset;

			if (mtx[0][2] == 0.0f && mtx[1][2] == 0.0f && mtx[3][2] == 0.0f && mtx[2][2] == 1.0f) {
				// The common case for particle systems.
				for (int i=0;i<state->polygon_count;++i) {
					unsigned int idx1=indices[i*3]-state->min_vertex_index;
					unsigned int idx2=indices[i*3+1]-state->min_vertex_index;
					unsigned int idx3=indices[i*3+2]-state->min_vertex_index;
					WWASSERT(idx1<state->vertex_count);
					WWASSERT(idx2<state->vertex_count);
					WWASSERT(idx3<state->vertex_count);
					const VertexFormatXYZNDUV2 *v1 = src_verts + idx1;
					const VertexFormatXYZNDUV2 *v2 = src_verts + idx2;
					const VertexFormatXYZNDUV2 *v3 = src_verts + idx3;
					unsigned array_index=i+polygon_array_offset;
					WWASSERT(array_index<overlapping_polygon_count);
					TempIndexStruct *tis_ptr = tis + array_index;
					tis_ptr->tri.i = idx1 + vertex_array_offset;
					tis_ptr->tri.j = idx2 + vertex_array_offset;
					tis_ptr->tri.k = idx3 + vertex_array_offset;
					tis_ptr->idx = node_id;
					tis_ptr->z = (v1->z + v2->z + v3->z)/3.0f;
					DEBUG_ASSERTCRASH((! _isnan(tis_ptr->z) && _finite(tis_ptr->z)), ("Triangle has invalid center"));
				}
			} else {
				for (int i=0;i<state->polygon_count;++i) {
					unsigned int idx1=indices[i*3]-state->min_vertex_index;
					unsigned int idx2=indices[i*3+1]-state->min_vertex_index;
					unsigned int idx3=indices[i*3+2]-state->min_vertex_index;
					WWASSERT(idx1<state->vertex_count);
					WWASSERT(idx2<state->vertex_count);
					WWASSERT(idx3<state->vertex_count);
					const VertexFormatXYZNDUV2 *v1 = src_verts + idx1;
					const VertexFormatXYZNDUV2 *v2 = src_verts + idx2;
					const VertexFormatXYZNDUV2 *v3 = src_verts + idx3;
					unsigned array_index=i+polygon_array_offset;
					WWASSERT(array_index<overlapping_polygon_count);
					TempIndexStruct *tis_ptr = tis + array_index;
					tis_ptr->tri.i = idx1 + vertex_array_offset;
					tis_ptr->tri.j = idx2 + vertex_array_offset;
					tis_ptr->tri.k = idx3 + vertex_array_offset;
					tis_ptr->idx = node_id;
					tis_ptr->z = (mtx[0][2]*(v1->x + v2->x + v3->x) +
												mtx[1][2]*(v1->y + v2->y + v3->y) +
												mtx[2][2]*(v1->z + v2->z + v3->z))/3.0f + mtx[3][2];
					DEBUG_ASSERTCRASH((! _isnan(tis_ptr->z) && _finite(tis_ptr->z)), ("Triangle has invalid center"));
				}
			}

            // Sorting depth above still uses the untouched original vertices
            // and pose. Only the copied GPU vertices have moved to world space.
            if(translated){
                D3DMATRIX identity{};identity._11=identity._22=identity._33=identity._44=1;
                state->sorting_state.world=identity;
            }
			state->native_source.sortedVertexCount=overlapping_vertex_count;
			state->min_vertex_index=vertex_array_offset;

			polygon_array_offset+=state->polygon_count;
			vertex_array_offset+=state->vertex_count;
		}
	}

    static GenZH::Modern::StableDepthSort<TempIndexStruct> depthSorter;
    {
        GenZH::Modern::CaptureWorkScope sortScope(GenZH::Modern::CaptureWork::DepthSort);
        auto *radix=(!GenZH::Modern::stableDepthSortReference&&!GenZH::Modern::orderedSortReference&&overlapping_polygon_count>=256)
            ?depthSorter.Sort(tis,overlapping_polygon_count):nullptr;
        if(radix){tis=radix;++GenZH::Modern::nativeDepthRadixBatches;}else Sort(tis,tis+overlapping_polygon_count);
    }
    GenZH::Modern::CaptureWorkScope submitScope(GenZH::Modern::CaptureWork::SortedSubmit);
    // Diagnostics only: pool size and runs that resubmit a node already drawn in this pool.
    auto &poolCounts=GenZH::Modern::orderedSortCounts;
    ++poolCounts.pools;poolCounts.poolNodes+=overlapping_node_count;poolCounts.poolVertices+=overlapping_vertex_count;
    poolCounts.poolNodeMax=(std::max)(poolCounts.poolNodeMax,static_cast<std::uint64_t>(overlapping_node_count));
    static std::vector<unsigned char> nodeSubmitted;nodeSubmitted.assign(overlapping_node_count,0);

	// TheSuperHackers @fix stephanmeesters 10/06/2026
	// Bound transient uploads; this is a work budget, not an index-value limit.
	constexpr const unsigned MAX_INDEX_CHUNK = 1048575;
	unsigned chunkOffset = 0;
	while (chunkOffset < overlapping_polygon_count)
	{
		unsigned chunkCount = overlapping_polygon_count - chunkOffset;
		if (chunkCount * 3 > MAX_INDEX_CHUNK) {
			chunkCount = MAX_INDEX_CHUNK / 3;
		}
		const unsigned chunkEnd = chunkOffset + chunkCount;

		DynamicIBAccessClass dyn_ib_access(BUFFER_TYPE_DYNAMIC_DX8,chunkCount*3);
		{
			DynamicIBAccessClass::WriteLockClass lock(&dyn_ib_access);
			ShortVectorIStruct* sorted_polygon_index_array=(ShortVectorIStruct*)lock.Get_Index_Array();

			for (unsigned a=0;a<chunkCount;++a) {
				sorted_polygon_index_array[a]=tis[chunkOffset + a].tri;
			}
		}

		// Set index buffer and render!

		DX8Wrapper::Set_Index_Buffer(dyn_ib_access,0); // Override with this buffer (do something to prevent need for this!)
		DX8Wrapper::Set_Vertex_Buffer(dyn_vb_access); // Override with this buffer (do something to prevent need for this!)

		DX8Wrapper::Apply_Render_State_Changes();

        // Keep the original index sequence byte-for-byte. Only adjacent runs
        // with exactly equivalent fixed state may share a DrawIndexed call.
        // All source resources stay referenced until this entire pool is drawn.
        DWORD pixelProgram=1;
        const bool batchEnabled=GenZH::Modern::nativeMeshDispatch.GeometryEnabled()&&
            !GenZH::Modern::orderedSortReference&&
            SUCCEEDED(DX8Wrapper::_Get_D3D_Device8()->GetPixelShader(&pixelProgram))&&!pixelProgram;
        GenZH::Modern::ScopedNativeFixedWrites fixedWrites(DX8Wrapper::_Get_D3D_Device8());
        unsigned count_to_render=1;
        unsigned start_index=0;
        unsigned node_id=tis[chunkOffset].idx;
        unsigned previous_node_id=node_id;
        unsigned minimum=overlapping_nodes[node_id]->min_vertex_index;
        unsigned maximum=minimum+overlapping_nodes[node_id]->vertex_count;
        ++GenZH::Modern::orderedSortCounts.beforeDraws;
        const auto submit=[&](){
            auto *state=overlapping_nodes[node_id];
            GenZH::Modern::ScopedNativeMeshSource source(state->native_source);
            if(nodeSubmitted[node_id])++poolCounts.repeatedNodeRuns;else nodeSubmitted[node_id]=1;
            Apply_Render_State(state->sorting_state);
            DX8Wrapper::Draw_Triangles(start_index*3,count_to_render,minimum,maximum-minimum);
            ++GenZH::Modern::orderedSortCounts.afterDraws;
        };
        for(unsigned i=chunkOffset+1;i<chunkEnd;++i){
            const unsigned next=tis[i].idx;
            const bool nodeChanged=previous_node_id!=next;
            if(nodeChanged)++GenZH::Modern::orderedSortCounts.beforeDraws;
            previous_node_id=next;
            if(nodeChanged&&node_id!=next){
                auto *a=overlapping_nodes[node_id],*b=overlapping_nodes[next];
                if(batchEnabled&&GenZH::Modern::SameOrderedSortState(a->sorting_state,b->sorting_state,a->native_source,b->native_source)){
                    minimum=(std::min)(minimum,b->min_vertex_index);
                    maximum=(std::max)(maximum,b->min_vertex_index+b->vertex_count);
                }else{
                    submit();count_to_render=0;start_index=i-chunkOffset;node_id=next;
                    minimum=b->min_vertex_index;maximum=minimum+b->vertex_count;
                }
            }
            ++count_to_render;
        }
        submit();
        GenZH::Modern::orderedSortCounts.triangles+=chunkCount;

		chunkOffset += chunkCount;
	}

    // One aggregate record per second, never per triangle or draw.
    static double lastBatchReport=0;
    const double now=GenZHLoad::Seconds();
    if(now-lastBatchReport>=1){
        const auto &c=GenZH::Modern::orderedSortCounts;
        GenZHLoad::Record("ordered_sort_batch_v72","original_draws="+std::to_string(c.beforeDraws)+
            " submitted_draws="+std::to_string(c.afterDraws)+" triangles="+std::to_string(c.triangles)+
            " translated_nodes="+std::to_string(c.translatedNodes)+" translated_vertices="+std::to_string(c.translatedVertices)+
            " source_reject="+std::to_string(c.sourceReject)+" material_reject="+std::to_string(c.materialReject)+
            " shader_reject="+std::to_string(c.shaderReject)+" texture_reject="+std::to_string(c.textureReject)+
            " transform_reject="+std::to_string(c.transformReject)+" light_reject="+std::to_string(c.lightReject)+
            " pools="+std::to_string(c.pools)+" pool_nodes="+std::to_string(c.poolNodes)+" pool_vertices="+std::to_string(c.poolVertices)+
            " pool_node_max="+std::to_string(c.poolNodeMax)+" repeated_node_runs="+std::to_string(c.repeatedNodeRuns),0);
        GenZHLoad::Record("depth_radix_v80","batches="+std::to_string(depthSorter.batches)+" items="+std::to_string(depthSorter.items)+
            " bypassed="+std::to_string(depthSorter.bypassed)+" scratch_bytes="+std::to_string(depthSorter.ScratchBytes()),0);
        lastBatchReport=now;
    }
	// Release all references and return nodes back to the clean list for the frame...
	for (unsigned node_id=0;node_id<overlapping_node_count;++node_id) {
		SortingNodeStruct* state=overlapping_nodes[node_id];
		Release_Refs(state);
		clean_list.push_front(state);
	}
	overlapping_node_count=0;
	overlapping_polygon_count=0;
	overlapping_vertex_count=0;

	SNAPSHOT_SAY(("SortingSystem - Done flushing"));

}

// ----------------------------------------------------------------------------

void SortingRendererClass::Flush()
{
	WWPROFILE("SortingRenderer::Flush");
	Matrix4x4 old_view;
	Matrix4x4 old_world;
	DX8Wrapper::Get_Transform(D3DTS_VIEW,old_view);
	DX8Wrapper::Get_Transform(D3DTS_WORLD,old_world);

	// TheSuperHackers @perf stephanmeesters 04/07/2026
	// Splice nodes that have no bounding information (Z=0.0) at the correct location into the sorted list.
	SortingNodeStructList::iterator node = sorted_list.begin();
	while (node != sorted_list.end() && (*node)->transformed_center.Z > 0.0f) {
		++node;
	}
	sorted_list.splice(node, unsorted_list);

	while (!sorted_list.empty()) {
		SortingNodeStruct* state = sorted_list.front();
		sorted_list.pop_front();

		if ((state->sorting_state.index_buffer_type==BUFFER_TYPE_SORTING || state->sorting_state.index_buffer_type==BUFFER_TYPE_DYNAMIC_SORTING) &&
			(state->sorting_state.vertex_buffer_types[0]==BUFFER_TYPE_SORTING || state->sorting_state.vertex_buffer_types[0]==BUFFER_TYPE_DYNAMIC_SORTING)) {
			Insert_To_Sorting_Pool(state);
		}
		else {
			GenZH::Modern::ScopedNativeMeshSource nativeSource(state->native_source);
			DX8Wrapper::Set_Render_State(state->sorting_state);
			DX8Wrapper::Draw_Triangles(state->start_index,state->polygon_count,state->min_vertex_index,state->vertex_count);
			DX8Wrapper::Release_Render_State();
			Release_Refs(state);
			clean_list.push_front(state);
		}
	}

	bool old_enable=DX8Wrapper::_Is_Triangle_Draw_Enabled();
	DX8Wrapper::_Enable_Triangle_Draw(_EnableTriangleDraw);
	Flush_Sorting_Pool();
	DX8Wrapper::_Enable_Triangle_Draw(old_enable);

	DX8Wrapper::Set_Index_Buffer(nullptr,0);
	DX8Wrapper::Set_Vertex_Buffer(nullptr);
	total_sorting_vertices=0;

	DynamicIBAccessClass::_Reset(false);
	DynamicVBAccessClass::_Reset(false);


	DX8Wrapper::Set_Transform(D3DTS_VIEW,old_view);
	DX8Wrapper::Set_Transform(D3DTS_WORLD,old_world);

}

// ----------------------------------------------------------------------------

void SortingRendererClass::Deinit()
{
	//
	//	Flush the sorted list
	//
	while (!sorted_list.empty()) {
		delete sorted_list.front();
		sorted_list.pop_front();
	}

	//
	//	Flush the unsorted list
	//
	while (!unsorted_list.empty()) {
		delete unsorted_list.front();
		unsorted_list.pop_front();
	}

	//
	//	Flush the clean list
	//
	while (!clean_list.empty()) {
		delete clean_list.front();
		clean_list.pop_front();
	}

	delete[] temp_index_array;
	temp_index_array=nullptr;
	temp_index_array_count=0;
}
