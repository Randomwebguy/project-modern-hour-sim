// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Exact-equivalence acceleration of MeshGeometryClass::cast_ray_brute_force.
//
// Meshes without an AABTree (no collision bits in the W3D file) are ray-cast by
// the original brute-force loop, which with OPTIMIZE_PLANEEQ_RAM recomputes the
// plane equation of every triangle on every cast. The cursor pick does this each
// frame, so high-detail models cost milliseconds per frame.
//
// This structure keeps the plane equations the original Compute_Plane_Equations
// produced (same function, same vertices, same values) and a conservative box
// per run of 32 consecutive triangles. A triangle can only change the result if
// the segment's computed intersection point lies on its plane and inside its
// projection on the plane chosen by find_dominant_plane (largest |N|
// component). Each triangle's box covers its vertex bounds in the two projected
// axes and the plane's full range over that rectangle in the dropped axis, plus
// a rounding margin far larger than float error. Triangles with non-finite or
// zero normals, non-finite vertices or near-degenerate projections make their run
// unconditional. Accepted runs are visited in ascending triangle order, so the
// original loop body sees exactly the same candidate sequence, including ties.
#include "WWMath/vector3.h"
#include "WWMath/vector4.h"
#include "WWMath/Vector3i.h"
#include "WWMath/lineseg.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

// Checks only: -1 follows GENZH_MESH_RAY_ACCEL, 0 forces the original loop, 1 forces acceleration.
extern int MeshRayCastAccelOverride;

class MeshRayCastAccel
{
public:
	static constexpr int RunShift = 5;		// 32 consecutive triangles per run
	static constexpr int GroupShift = 5;	// 32 runs per group
	static constexpr int MinimumTriangles = 128;

	struct Box { float Lo[3]; float Hi[3]; bool Unbounded; };

	const void * VertexSource = nullptr;
	const void * PolySource = nullptr;
	int VertexCount = 0;
	int PolyCount = 0;
	std::vector<Vector4> Planes;

	bool Matches(const void * vertex_source, const void * poly_source, int vertex_count, int poly_count) const
	{
		return VertexSource == vertex_source && PolySource == poly_source && VertexCount == vertex_count && PolyCount == poly_count;
	}

	// Planes must already hold the original plane equations for these arrays.
	void Build_Boxes(const Vector3 * verts, const Vector3i32 * polys)
	{
		const int runs = (PolyCount + (1 << RunShift) - 1) >> RunShift;
		Runs.assign(runs, Empty());
		Groups.assign((runs + (1 << GroupShift) - 1) >> GroupShift, Empty());
		for (int t = 0; t < PolyCount; ++t) {
			Box & run = Runs[t >> RunShift];
			if (run.Unbounded) continue;
			Triangle_Box(verts[polys[t].I], verts[polys[t].J], verts[polys[t].K], Planes[t], run);
		}
		for (int r = 0; r < runs; ++r) {
			Expand(Runs[r]);
			Merge(Groups[r >> GroupShift], Runs[r]);
		}
	}

	std::size_t Bytes() const { return Planes.capacity() * sizeof(Vector4) + (Runs.capacity() + Groups.capacity()) * sizeof(Box); }

	// Calls visit(triangle) in ascending order for every triangle of a run whose box
	// the segment may touch. visit returns false to stop. Returns false only when
	// the segment itself is not finite (caller must then use the original loop).
	template <class Visit> bool For_Each_Candidate(const LineSegClass & ray, std::uint64_t & tested, Visit && visit) const
	{
		const Vector3 & p = ray.Get_P0();
		const Vector3 & d = ray.Get_DP();
		const float p0[3] = { p.X, p.Y, p.Z };
		const float dp[3] = { d.X, d.Y, d.Z };
		float scale = 0.0f;
		for (int a = 0; a < 3; ++a) {
			if (!std::isfinite(p0[a]) || !std::isfinite(dp[a])) return false;
			scale = (std::max)(scale, std::fabs(p0[a]) + std::fabs(dp[a]));
		}
		const float margin = 1.0e-4f * scale + 1.0e-3f;
		const int runs = static_cast<int>(Runs.size());
		for (int g = 0; g < static_cast<int>(Groups.size()); ++g) {
			if (!Touches(Groups[g], p0, dp, margin)) continue;
			const int last = (std::min)(runs, (g + 1) << GroupShift);
			for (int r = g << GroupShift; r < last; ++r) {
				if (!Touches(Runs[r], p0, dp, margin)) continue;
				const int end = (std::min)(PolyCount, (r + 1) << RunShift);
				for (int t = r << RunShift; t < end; ++t) {
					++tested;
					if (!visit(t)) return true;
				}
			}
		}
		return true;
	}

private:
	std::vector<Box> Runs;
	std::vector<Box> Groups;

	static Box Empty()
	{
		Box b;
		for (int a = 0; a < 3; ++a) { b.Lo[a] = INFINITY; b.Hi[a] = -INFINITY; }
		b.Unbounded = false;
		return b;
	}
	static void Merge(Box & into, const Box & b)
	{
		if (into.Unbounded) return;
		if (b.Unbounded) { into.Unbounded = true; return; }
		for (int a = 0; a < 3; ++a) { into.Lo[a] = (std::min)(into.Lo[a], b.Lo[a]); into.Hi[a] = (std::max)(into.Hi[a], b.Hi[a]); }
	}
	static void Expand(Box & b)
	{
		if (b.Unbounded) return;
		float scale = 0.0f;
		for (int a = 0; a < 3; ++a) {
			if (!(b.Lo[a] <= b.Hi[a])) return;	// empty run (cannot happen with PolyCount > 0)
			scale = (std::max)(scale, (std::max)(std::fabs(b.Lo[a]), std::fabs(b.Hi[a])));
		}
		const float margin = 1.0e-4f * scale + 1.0e-4f;
		for (int a = 0; a < 3; ++a) { b.Lo[a] -= margin; b.Hi[a] += margin; }
	}
	static void Triangle_Box(const Vector3 & v0, const Vector3 & v1, const Vector3 & v2, const Vector4 & plane, Box & run)
	{
		const float a[3] = { v0.X, v0.Y, v0.Z }, b[3] = { v1.X, v1.Y, v1.Z }, c[3] = { v2.X, v2.Y, v2.Z };
		const float n[3] = { plane.X, plane.Y, plane.Z };
		float lo[3], hi[3];
		for (int x = 0; x < 3; ++x) {
			if (!std::isfinite(a[x]) || !std::isfinite(b[x]) || !std::isfinite(c[x]) || !std::isfinite(n[x])) { run.Unbounded = true; return; }
			lo[x] = (std::min)({ a[x], b[x], c[x] });
			hi[x] = (std::max)({ a[x], b[x], c[x] });
		}
		// Same comparison sequence as find_dominant_plane in WWMath/tri.cpp.
		const float ax = std::fabs(n[0]), ay = std::fabs(n[1]), az = std::fabs(n[2]);
		int k = 0; float val = ax;
		if (ay > val) { k = 1; val = ay; }
		if (az > val) { k = 2; val = az; }
		if (!(val > 0.0f)) { run.Unbounded = true; return; }
		const int i = (k == 0) ? 1 : 0, j = (k == 2) ? 1 : 2;
		// A nearly collinear projection lets the 2D sign test accept points along a
		// line beyond the vertices; keep such triangles unconditional.
		const float area2 = std::fabs((b[i] - a[i]) * (c[j] - a[j]) - (b[j] - a[j]) * (c[i] - a[i]));
		if (!(area2 > 1.0e-4f * (hi[i] - lo[i]) * (hi[j] - lo[j]))) { run.Unbounded = true; return; }
		// Range of the plane through v0 over the projected rectangle (|slope| <= 1).
		const float si = -n[i] / n[k], sj = -n[j] / n[k];
		const float ci[2] = { lo[i] - a[i], hi[i] - a[i] }, cj[2] = { lo[j] - a[j], hi[j] - a[j] };
		float kmin = lo[k], kmax = hi[k];
		for (int u = 0; u < 2; ++u) for (int w = 0; w < 2; ++w) {
			const float value = a[k] + si * ci[u] + sj * cj[w];
			if (!std::isfinite(value)) { run.Unbounded = true; return; }
			kmin = (std::min)(kmin, value); kmax = (std::max)(kmax, value);
		}
		lo[k] = kmin; hi[k] = kmax;
		for (int x = 0; x < 3; ++x) { run.Lo[x] = (std::min)(run.Lo[x], lo[x]); run.Hi[x] = (std::max)(run.Hi[x], hi[x]); }
	}
	static bool Touches(const Box & b, const float p0[3], const float dp[3], float margin)
	{
		if (b.Unbounded) return true;
		float t0 = 0.0f, t1 = 1.0f;
		for (int a = 0; a < 3; ++a) {
			const float lo = b.Lo[a] - margin, hi = b.Hi[a] + margin;
			if (!(lo <= hi)) return false;	// empty
			if (dp[a] == 0.0f) {
				if (p0[a] < lo || p0[a] > hi) return false;
				continue;
			}
			float ta = (lo - p0[a]) / dp[a], tb = (hi - p0[a]) / dp[a];
			if (ta > tb) std::swap(ta, tb);
			// Generous parameter slack; the box margin already exceeds rounding.
			const float slack = 1.0e-4f;
			if (ta - slack > t0) t0 = ta - slack;
			if (tb + slack < t1) t1 = tb + slack;
			if (t0 > t1) return false;
		}
		return true;
	}
};
