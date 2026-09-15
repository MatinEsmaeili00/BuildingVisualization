// Copyright Matin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "BuildingVisualizationTypes.generated.h"

namespace BuildingVisualization
{
	/**
	 * How many clip volumes the material function evaluates.
	 *
	 * This is a COMPILE-TIME constant and it is deliberately small. A material
	 * graph has no loops, so the volumes are unrolled: every extra slot costs
	 * four more Material Parameter Collection vectors and a fixed amount of
	 * shader work in EVERY material carrying the clip function, whether or not
	 * that slot is in use. Four covers the realistic cases (one inspection box,
	 * plus a couple of section cuts) without taxing the whole building.
	 *
	 * Raising this means regenerating the MPC and reinjecting the material
	 * function. If you ever need dozens, the answer is not a bigger number
	 * here - it is a structured buffer and a custom vertex factory, which is a
	 * different and much larger piece of work. See the Cost section of README.md.
	 */
	static constexpr int32 MaxClipVolumes = 4;
}

/** What a clip volume does to the geometry it covers. */
UENUM(BlueprintType)
enum class EBuildingClipMode : uint8
{
	/** Volume is ignored entirely. Costs the same in-shader as any other mode. */
	Disabled = 0		UMETA(DisplayName = "Disabled"),

	/** Geometry INSIDE the box is removed. The inspection-cube case: carve a hole to see the pipes. */
	CutInside = 1		UMETA(DisplayName = "Cut Inside (carve a hole)"),

	/** Geometry OUTSIDE the box is removed. The isolation case: keep one room, drop the rest of the building. */
	CutOutside = 2		UMETA(DisplayName = "Cut Outside (isolate contents)"),
};

/**
 * One clip volume, packed the way the shader wants it.
 *
 * The box test is done in the volume's LOCAL space rather than world space:
 * transform the world position by the volume's inverse transform, then compare
 * against half-extents on each axis. That is what makes rotation and
 * non-uniform scale free - all of it is already baked into the matrix, so the
 * shader never needs a rotation, a quaternion, or a separate scale.
 *
 * Packed as three float4 rows (a 3x4 affine matrix; the fourth row of a
 * transform matrix is always (0,0,0,1), so storing it would waste a register)
 * plus one float4 of parameters.
 */
struct FBuildingClipVolumeGPU
{
	/** Rows 0-2 of the world-to-local matrix, each row's W holding the translation term. */
	FVector4f Row0 = FVector4f(1.f, 0.f, 0.f, 0.f);
	FVector4f Row1 = FVector4f(0.f, 1.f, 0.f, 0.f);
	FVector4f Row2 = FVector4f(0.f, 0.f, 1.f, 0.f);

	/** XYZ = half-extents in local space. W = EBuildingClipMode as a float. */
	FVector4f Params = FVector4f(0.f, 0.f, 0.f, 0.f);

	/**
	 * The same box again, as a world-space axis-aligned centre and half-extent.
	 *
	 * Redundant with the matrix above, and deliberately so. The matrix form is
	 * the correct one - it handles rotation - but expressing it in a material
	 * graph means three dot products and an Append before the test even starts,
	 * built out of nodes wired up by a script. The centre/extent form is a
	 * Subtract, an Abs and a Subtract, which is small enough to build reliably
	 * and small enough to read in the material editor when it misbehaves.
	 *
	 * So the shader uses this pair and ignores rotation; the matrix stays
	 * published for when the rotated path is added back. Two float4s of
	 * collection space per volume is a cheap price for a graph that can be
	 * verified by looking at it.
	 *
	 * Center.W carries the mode, so a material reads one vector and knows both
	 * where the box is and what it does.
	 */
	FVector4f Center = FVector4f(0.f, 0.f, 0.f, 0.f);
	FVector4f Extent = FVector4f(0.f, 0.f, 0.f, 0.f);
};
