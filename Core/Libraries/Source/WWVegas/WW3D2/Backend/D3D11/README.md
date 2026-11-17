# Zero Hour D3D11 renderer - renderer 26

Eligible static rigid normal-XYZ registration now retains shared position,
normal, color, UV and topology arrays, without eagerly packing or creating
the DX8 fallback buffers. Buffer wrappers reserve capacity and lazy writers;
they do not retain MeshModel objects or create renderer ownership cycles.
The selected native polygon recipe preserves material order and base offsets.
Array allocation owners survive mesh/model destruction and copy-on-write
replacement; the normal immutable registration lifecycle remains authoritative.
In-place array mutation without re-registration is not new supported behavior.

Native drawing applies material/light/texture/matrix state and the required
FVF once, retaining pending stream/index state. Geometry-only submissions
cannot consume the previously bound COM buffers if native preparation fails.
Fallback first creates/fills/binds its own buffers, then submits once. The
consumed FVF flag prevents overwriting a later custom shader during fallback.
Unsupported, sorting, skin, NPatch/gap and special paths retain compatibility.
Buffers may materialize eagerly when such paths or direct locks require them.
Once materialized, later appends fill immediately; warm buffers do not replay
writers. Failed writes/locks retain recipes for retry and release locks safely.
Destruction of a never-materialized buffer safely releases only its recipes.

GENZH_LAZY_LEGACY_BUFFERS=off restores eager buffer preparation/binding, while
GENZH_NATIVE_GEOMETRY=off and GENZH_NATIVE_SUBMIT=off retain prior diagnostic
switches. renderer-legacy-v26.csv records cumulative reserved/materialized/
packed/error counters and live unmaterialized capacity every 120 EndScene calls.
The report filters PID/session times and takes the last chronological counters,
not their sum. Live capacity is not measured total RAM/VRAM or FPS; historical
WW3D allocation counters still describe logical buffer capacity.

Headless x86/x64 checks cover native/eager/lazy packed vertex byte equivalence,
R32 selection/base offsets, deferred retry/lifetime and actual seven-model
array ownership after MeshModel destruction. Production wrappers are created,
queued and destroyed with no device; the deferred queue materialization logic
has instrumented lock/writer checks. Real RTX3060 PBR pixels and grouping remain
equivalent. Game-level driver/state-block/first-fallback behavior and heavy
scene FPS still need gameplay acceptance. No game was launched in this phase.
Shared model-wide resource organization, GPU scene/indirect and P2-P7 remain
open. Daily runtime is x86 LAA/native32; the full AMD64 multimedia port remains
incomplete. This is conditional lazy allocation, not full DX8 removal.

# Zero Hour D3D11 renderer - renderer 25

Eligible registered rigid, static indexed mesh draws build immutable DX11
geometry directly from MeshModel position, normal, packed color and up to
eight two-dimensional UV arrays. Registration retains the original material
category polygon selection and order, including gap fillers. The compact
192-byte FixedVertex builder preserves the packed-FVF decoder defaults and
uses UInt32 local indices. This entry does not read DX8 vertex/index buffers
or query their bindings. Existing DX8 buffers are still created and bound for
compatibility; their removal/lazy allocation is a later phase.

Native recipes have independent lifetime IDs and revisions. Their cache
namespace cannot alias legacy buffer entries; the existing bounded CPU/GPU
geometry cache and full-detail uncached fallback remain. Borrowed model arrays
exist only in the synchronous polygon draw scope. Cache misses validate live
model identity, counts and array pointers before reading; deferred packets
retain decoded geometry and recipe ownership, not dereferenceable model data.
Copied polygon registrations use compatibility until registered again.
Unsupported UV dimensions, skinning, sorting and incompatible draw states
retain the previous path. Preparation failures fall back before submission;
handled submission failures do not draw twice.

Texture ownership recovery is separate from legacy geometry bindings. Initial
and recovery captures query texture owners only; successful texture setters
update the mirror. Fixed-state Snapshot equations and PBR/shadow/FOW paths are
unchanged. GENZH_NATIVE_GEOMETRY=off selects the Renderer24 buffer entry;
GENZH_NATIVE_SUBMIT=off selects the general compatibility entry.
renderer-geometry-v25.csv reports attempts, accepted, fallback, cache and
compact CPU decode counters per 120 EndScene calls. The launcher/report filters
PID and session timestamps. These counters are not FPS or unique object counts.

x86/x64 native decoder and actual seven-model/107-mesh reader checks pass,
including 70005 vertices with index 70004, UV and hierarchy preservation.
Headless RTX3060 fixtures verify production-builder GPU uploads, PBR channel
pixel effects and scene grouping equivalence. No game or gameplay FPS test was
performed. Shared model-wide vertex resources, lazy legacy allocation, GPU
scene/indirect and heavy gameplay acceptance remain open. Daily runtime remains
x86 LAA/native32; the AMD64 Miles/Bink multimedia game port is incomplete.

# Zero Hour D3D11 renderer - renderer 24

Eligible native rigid, static indexed normal-XYZ opaque/cutout mesh draws now
enter the shared DX11 immutable geometry cache/scene queue directly from
WW3D Draw. Native fixed transforms, material, viewport, lights and shader
handles mirror successful mutations; stream/index/texture owners and binding
ranges avoid repeated getters after the initial/recovery capture. Snapshot
equations and all 2240 constant bytes remain the same. Mesh UVs, revisions,
bone transforms, material channels, FOW and shadow preparation are preserved.
Skinning, dynamic buffers, sorting, additional passes, custom shaders,
offscreen and unsupported depth/blend/stencil/clip states use compatibility.
State-block recording cannot submit directly; end/apply, reset, render-target
viewport changes, multiplication and unread light enables reseed safely.
The dispatch matches a registered device address without foreign COM RTTI.
Handled submission errors never cause a duplicate compatibility draw.

GENZH_NATIVE_SUBMIT=off disables this entry for diagnosis. The isolated
renderer-submit-v24.csv counts attempts, accepted/errors, fallback reasons,
state/binding refresh and geometry hits/misses per 120 EndScene calls. The
launcher/report joins PID and session times. Counters are not FPS. Native DX8
buffer creation/state setters and some texture-resource queries remain;
direct native MeshModel resource upload, GPU scene/indirect and heavy-scene
gameplay acceptance are still pending. Daily runtime remains x86 LAA/native32;
AMD64 builds do not complete the Miles/Bink game port.

# Zero Hour D3D11 renderer — renderer 23

P1 now captures native mesh provenance at category/procedural submission and
through deferred sorting nodes. Process-local lifetime IDs distinguish root
objects, mesh instances and model resources without pointer-reuse aliasing;
W3DModelDraw binds the current drawable ID explicitly. Clones get new object
IDs and no inherited drawable owner. Current parent/bone, root LOD and pass are
captured by value; the actual draw transform/material/UV snapshot remains in
DrawPacket. Sorted replay restores its source with a nested thread-local scope.
Native skin meshes explicitly retain the compatibility/non-instanced path.
renderer-native-v23.csv counts native-linked scene packets and exclusive
surface classes before camera culling. Counts are not unique object counts.
This adds native identity/attachment extraction to the compatibility scene;
direct native immutable mesh upload, GPU scene/indirect and all effect paths
are unfinished. No gameplay FPS gain has been measured.

# Zero Hour D3D11 renderer — renderer 22

P1 continues with conservative camera rejection after the original shadow
caster preparation/pass. Cached fixed-XYZ opaque triangle meshes outside the
padded NDC XY rectangle can be skipped. Unsupported shaders/layouts, skinning,
blend/stencil, near-plane crossings and uncertain bounds retain their draws.
Survivors keep submission order; their projected bounds are reused by scene
grouping. GENZH_CAMERA_CULLING=off disables this rejection for diagnosis.
Native visibility/fog rules remain authoritative. Complete native object scene
extraction, occlusion and indirect drawing remain unfinished.

Terrain grass exclusion reuses its existing mask when captured road buffer
identity/revision/range/original topology, world transforms, source lifetimes,
terrain dimensions and rotated structure footprints are unchanged. UP draws
without source owners rebuild; buffer rewrites invalidate conservatively.
Reset/no-terrain also invalidate. The raster algorithm and grass/material/model
quality are unchanged. renderer-scene-v22.csv reports preparation, rejection,
original shadow input and exclusion reuse/build/upload/vertex counters per
120-frame window. Older renderer-work-v18 scene counts are after camera rejection;
v22 scene_seen is before rejection. No gameplay FPS gain is yet measured.

Historical phase details follow.

# Zero Hour D3D11 renderer — renderer 18

Dynamic fixed-function draw packets now keep the same 192-byte vertex channels
as the static cache: position, normal, diffuse/specular and all eight UVs. The
unused 256-byte programmable register block is removed only when no vertex
program is active. CPU full vertices are freed after conversion; dynamic GPU
buffers stay dynamic. No vertex, index, UV or material precision is reduced.

Exclusively owned draw/vertex-program/shadow/terrain/instance constant buffers
skip identical uploads. Cache entries commit only after successful Map and
invalidate when buffers are recreated. Context bindings are still made each
draw, including after tone mapping and offscreen transitions.

Opaque instance grouping can cross up to 32 preceding groups only when the
moved packet's conservative camera-projected AABB is disjoint from every
crossed group's union. Two-pixel bounds padding, identical view/projection and
viewport, and fixed-function opaque depth-write eligibility are required.
Near-plane/invalid bounds, shaders, terrain, decals, transparency and stencil
are barriers. Equivalent adjacent packets retain the old behavior. Shadow
grouping remains adjacent: camera-space separation cannot prove shadow-space
separation. This improves eligible interleaved object/submesh batches; it is
not a complete native mesh/object scene or global material sort.

renderer-work-v18.csv adds constant upload/skip counts, dynamic vertex/index
bytes, grouping/eligibility counts, and eight sparse GPU submission intervals.
GpuPassTimer samples every eight frames, uses eight query slots and at most 40
timestamps per sampled frame. Poll uses DONOTFLUSH and never waits. Backlogged
samples are skipped; overflow/disjoint samples are discarded. Capture time
and other submission intervals include CPU gaps, not pure GPU busy time.
There is no per-frame P95/P99 or GPU utilization claim. Existing v16/v17 log
schemas and all material/terrain settings remain unchanged.

Renderer17C streaming architecture retained below:

Authored model/material maps now use one CPU map at a time: DDS disk read/parsing or original TGA fallback, with source/sidecar size and FILETIME revalidation across the complete set. The render callback only enqueues names and binds completed views. It performs no authored map disk reads/decoding. Native DX8 base asset decoding/capture remains and is not made asynchronous by this package.

Albedo/normal/ORM/height/emissive publish together only after all available maps are uploaded and a final whole-set source-signature check. Missing maps remain optional; absent sets use a bounded negative cache. Corrupt/changed sets discard partial GPU resources and retain the complete native material. Source edits after publication require renderer reset; this is not live hot reload.

Terrain and model uploads share one BeginScene upload slot: total 4 MiB/frame and approximately 2 ms CPU scheduling target, at most one GPU allocation in that slot. Driver allocation/mip calls may exceed time targets. CPU preparation admission is serialized between terrain and model queues, so terrain's worst-case buffers and an authored decode are not opened together. Model admission has 64 active+queued sets, 512 metadata entries, one CPU result retained until its GPU map upload finishes. A map holds at most a 96 MiB prepared DDS file or 256 MiB 8K RGBA fallback, then releases it. Queue progress depends on rendered frames; modern material detail may appear after initial native drawing.

BC7 XYZ normals/linear ORM, BC7 albedo via UNORM view and explicit shader sRGB decode, BC4 height and all prepared mips remain byte exact. Compressed subresource boxes use physical block-rounded extents; whole small mips use an unboxed full update. TGA fallback retains RGBA8 UNORM and GPU mip generation. No map generation, UV, mesh or shader feature change.

GPU cache ownership uses 512 MiB LRU target and may temporarily exceed it if current-frame materials are pinned. A single material exceeding the budget falls back before exceeding its individual budget. This excludes terrain/native textures, driver overhead and COM views retained by queued draw packets. Completed/failed cache entries can be evicted, with retained packet references preserving texture lifetime.

loading-v17.csv adds material_prepare_worker/material_read_dds/material_read_tga_fallback/material_upload_slice/material_ready/material_fallback. Separate material-streaming-v17c.csv records cache-owned bytes, entry/queue counts, active set, cumulative eviction/transfers and current upload CPU bytes every 120 frames; the CPU byte sample excludes decoding/ready worker buffers. Existing renderer-performance-v16.csv schema stays readable; CPU/GPU render intervals include the selected upload slot.

186 real DDS textures and 2,208 mip/base readbacks including RGBA/rectangular BC fixtures pass on headless hardware. Source edits after partial upload never publish stale maps. x86 LAA game and AMD64 renderer libraries compile. No game launch, gameplay FPS or map-load-time measurement. Loading-screen labels, uncommon per-map shader manifests and actual cold/warm gameplay timing remain P0; native scene/culling/LOD and new light/shadow/weather are subsequent phases. Historical implementation notes follow.

# Zero Hour D3D11 renderer — renderer 17B

P0 continues. Full canonical terrain atlases (albedo/normal/ORM/height/semantic class) now persist in a checksummed, exact-key, atomic RGBA cache with a 1 GiB disk budget, separate from the 512 MiB tile cache. Native base pixels and canonical cells are hashed; material paths/size/FILETIME and the prepared content-hash manifest invalidate edited sources. Lighting/weather draw constants are excluded from this static cache. This does not freshly SHA every TGA on a warm game load.

One CPU worker prepares or reads terrain without DX8/11 objects. Only one request/result is admitted through publication, with a 768 MiB worst-case preparation estimate. Native snapshots still happen on the rendering thread. GPU allocation, row/cell upload and mip generation run on the immediate context, at most one allocation per frame, with a 4 MiB transfer limit and approximately 2 ms CPU submission scheduling budget. Single driver allocations/mip calls can exceed that time; no frame-time guarantee is implied. CPU surfaces are released progressively. Complete native terrain remains until all five maps and canonical UV/blend cells are ready, then the set is published atomically for the current owner/revision. Modern terrain detail can appear shortly after the first rendered map frame. Reset/map revision/owner expiry cancel old results; another drawn atlas waits rather than repeatedly cancelling the current job.

loading-v17.csv adds terrain_snapshot_native, terrain_prepare_worker, terrain_upload_slice and terrain_ready. Frame CPU/GPU intervals include upload slices. map_start does not cover subsequent rendering frames or deferred readiness. Inclusive/nested times must not be summed; CPU API upload duration is not GPU completion.

New headless CPU/GPU phase checks: cache/edit/corrupt/truncated/exact-key validation, frozen17A pixel equality, worker admission/cancel/replacement and hardware GPU texture/cell readback including nonzero partial row offsets and padded staging pitch. x86 LAA game and x64 renderer libraries build; full x64 game remains unfinished. Authored model material uploads remain synchronous; loading-screen labels, uncommon per-map shader manifests and gameplay timing remain P0 work. No game launched. Historical phase notes follow.

# Zero Hour D3D11 renderer — renderer 17A

First loading package (roadmap P0 in progress). Authored model maps use offline DirectXTex DDS BC7 (normal XYZ and linear ORM), BC7 sRGB albedo metadata, BC4 height and complete mip chains. Legacy shader sRGB decoding uses a deliberately UNORM SRV. Source file size/FILETIME and prepared size must match; invalid/missing/stale DDS falls back to the preserved TGA. No model or UV change.

Terrain material tiles now use the native CPU TGA decoder and bilinear clamp resizing, bypassing per-tile D3DX8 loading/conversion. Exact source path/size/time/algorithm/target-size keys and pixel checksums protect persisted RGBA tiles. Atomic saves and a 512 MiB disk budget bound generated tiles. Common 128/256/512 tiles are prepared offline; 24 source maps produce 72 tiles. The existing full atlas still assembles and uploads on map load; this is not a whole-atlas streaming implementation. Common native BGRA8/XRGB/1555/565/4444 atlases copy directly to CPU RGBA, retaining fallback for unusual formats.

ProgramSource.h is shared between runtime and offline shader preparation. 534 requests (21 program variants with common texture binding masks plus foundation shaders) use identical exact-source cache keys. Missing bindings/mod shaders still compile synchronously. Cache writes now replace atomically. Map start/INI/parse/native terrain/progress intervals, model decode including reads, material decode/DDS, tile reuse, atlas assembly/upload and shader preparation write loading-v17.csv with PID and UTC timestamps. Events nest: do not sum inclusive durations. Map-start excludes deferred preparation on first rendering frames; upload durations are CPU API times, not GPU completion.

Game remains x86 LAA. x64 renderer/bridge builds; full game x64, bounded async upload/streaming, native scene batching, new lighting/shadow/weather systems remain on the roadmap. No game was launched for this package; the reported 60-second map load has not yet been remeasured.

Previous renderer and verification notes follow.

# Zero Hour D3D11 renderer — renderer 16

Compact 192-byte fixed geometry preserves positions, normals, colors and all eight UV channels. A 256 MiB combined CPU/GPU cache uses immutable buffers with identity/revision/range/layout invalidation. Consecutive equivalent opaque fixed draws use GPU instancing without reordering coplanar, blended, stencil, UI or terrain draws. Successful native render/stage setters are mirrored, refreshing after state blocks/reset and reading authoritative native state for draws during recording.

Fixed and programmable input signatures are separate. Authored opaque materials use albedo, tangent normal, linear RGB AO/roughness/metalness, optional height cavity and emissive maps. Model height affects cavity shading, not geometry displacement; terrain retains canonical height displacement. Linear GGX, analytic hemisphere indirect fill, indirect-only AO, derivative roughness filtering and anisotropic mip sampling are active. This is not HDR reflection-probe IBL or GI. Grass density follows projected blade size.

DXGI high-performance hardware selection has an adapter fallback. Exact source/entry/profile validated bytecode is cached on disk. Eight GPU timestamp/disjoint slots use DONOTFLUSH polling and skip samples rather than waiting. renderer-performance-v16.csv records CPU/GPU intervals, cache evictions, geometry uploads and draw/instance counts per 120 frames. GPU intervals can include submission starvation and CPU frame time excludes Present.

Verification: x86 full engine and AMD64 renderer bridge compile; 5,962 CPU assertions include exact UV equality, 90,000 indices, state/block invalidation and scene-sized cache replay. 23 compatibility variants, three terrain stages and six foundation shader entries compile strictly. 92 headless RTX 3060 GPU assertions verify material-channel pixel effects, indirect-only AO, timestamp queries and instanced/individual draw equality. No game launch or gameplay FPS measurement for renderer 16. Full x64 game, all asset LODs, HDR probes, SSAO/TAA/GI and remaining content variants are unfinished.

Historical content description follows; current evidence is outputs/ZeroHour2026Engine/render16.

# Zero Hour D3D11 renderer — batch 07

Native scene geometry and presentation are submitted to D3D11 through the DX8-facing compatibility device. Native DX8 interfaces remain for asset staging, capabilities and state/shader storage; full removal is unfinished.

Current batch connects an imagegen-restored original battlefield menu and 22 native menu scripts, detailed original Crusader W3D meshes with preserved native UV/hierarchy/material definitions, its actual avleopard texture atlas and correctly framed icon. Fonts use bilinear atlas filtering in modern mode. Three actual Alpine Assault grass basemaps are upgraded by basename.

Terrain atlas metadata contains true region semantics and three native map lights. t8/t9 are authored normal/ORM, t10 comparison shadows, t11/t12 height/identity, t13 canonical world UV/blend cells (80-byte stride), b3 dimensions (16 bytes), t14 per-scene ground occupancy. Adaptive HS/DS maintain shared canonical relief; GS generates bent crossed grass ribbons with independent leaf color. Occupancy uses actual rotated structure footprints and registered RoadType/bib triangles and flat opaque geometry. Terrain sinks slightly under these overlays and grass is excluded. Unrelated semantic normal/height fallback maps were removed; unknown regions retain native basemaps with flat normal/height.

GENZH_RENDERER=d3d11 selects this renderer; GENZH_VISUAL_STYLE=2026 selects UI theme. GENZH_MATERIAL_DIR points to basename_albedo/normal/orm/height.tga. Normal is tangent +Y; ORM RGB is linear AO/roughness/metalness. GENZH_TERRAIN=off disables geometry detail; GENZH_FOLIAGE=off disables grass. Cloud/fog draws are excluded from occupancy; actual registered road triangles also cover slopes. Native blend overlays retain legacy pixel behavior. Existing visual opt-outs remain in the launcher.

Build: x86 Release, MSVC 19.51; D3D11 bridge/checker C++17 /W4 /WX. 23 strict compatibility shader variants × 5 VS/PS/GS/shadow entries plus 3 terrain stages compiled and CPU layout/buffer checks passed without a graphics device or window. No game was launched or input controlled for batch 07. UI appearance, geometry/terrain interactions and FPS are unverified. See batch07-review.md, batch07-file-checks.json and source checks in the export. Historical verification/ records are not batch07 evidence.
