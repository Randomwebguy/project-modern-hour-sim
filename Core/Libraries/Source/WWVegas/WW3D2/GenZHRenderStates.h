// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Render states the native game sets for the modern D3D11 backend. DX8 render states end at 164 and the wrapper caches 256
// entries, so these indices never collide with the DX8 enumeration.
enum : unsigned
{
	// Photoreal decal (track marks and building aprons). The native overlay sets it around its draws; the D3D11 backend
	// (Backend/D3D11/CompatibilityRenderer.cpp) turns those draws into a multiplicative stain of the lit ground.
	GENZH_D3DRS_DECAL = 240,
};
