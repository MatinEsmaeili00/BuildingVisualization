// Copyright Matin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "BuildingVisualizationSettings.generated.h"

class UMaterialParameterCollection;

/**
 * Project-wide configuration. Project Settings > Plugins > Building Visualization.
 *
 * A world subsystem has no details panel of its own, so this is where the
 * system is actually configured; the subsystem reads these values when it
 * initialises.
 */
UCLASS(config = Game, defaultconfig, meta = (DisplayName = "Building Visualization"))
class BUILDINGVISUALIZATION_API UBuildingVisualizationSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	UBuildingVisualizationSettings();

	virtual FName GetCategoryName() const override;

	static const UBuildingVisualizationSettings& Get();

	// --- The bridge to materials -------------------------------------------

	/**
	 * The Material Parameter Collection every clippable material reads from.
	 *
	 * This asset is the entire reason the system scales. An MPC is a single
	 * global uniform buffer: writing to it once per frame updates every
	 * material referencing it, with no per-mesh, per-component or per-material
	 * -instance work at all. The alternative - dynamic material instances
	 * carrying their own copies of these parameters - would mean touching
	 * thousands of objects each time the box moves.
	 *
	 * Create it with Generate Parameter Collection (see Docs/03-setup.md), or
	 * run the Python setup script which creates it with the right layout.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Material Bridge", meta = (AllowedClasses = "/Script/Engine.MaterialParameterCollection"))
	TSoftObjectPtr<UMaterialParameterCollection> ParameterCollection;

	/**
	 * Prefix for the per-volume vector parameters in the collection.
	 *
	 * Slot i uses: <Prefix>Row0_i, <Prefix>Row1_i, <Prefix>Row2_i, <Prefix>Params_i.
	 * The names are built from this prefix on both sides - C++ when writing and
	 * the generated material function when reading - so the two can never drift
	 * apart by a typo in one place only.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Material Bridge")
	FName ClipParameterPrefix;

	/** Vector parameter carrying global state: X = active volume count, Y = feather width, Z/W reserved. */
	UPROPERTY(config, EditAnywhere, Category = "Material Bridge")
	FName GlobalsParameterName;

	/** Vector carrying the focused Z slab: X = min Z, Y = max Z, Z = enabled, W = feather. */
	UPROPERTY(config, EditAnywhere, Category = "Material Bridge")
	FName FocusSlabParameterName;

	/** Vector carrying ghost state: X = ghost opacity, Y = enabled, Z/W reserved. */
	UPROPERTY(config, EditAnywhere, Category = "Material Bridge")
	FName GhostParameterName;

	// --- Semantic selection -------------------------------------------------

	/**
	 * Actor tag prefixes the selection API matches against.
	 *
	 * Plain FName actor tags rather than Gameplay Tags, deliberately. Gameplay
	 * Tags must exist in a central registry before they can be applied, which
	 * means a building import would have to register several thousand of them
	 * (Floor.01 through Floor.40, every room number) before a single actor
	 * could be tagged. Actor tags are free-form strings, so an importer can
	 * write whatever the BIM data says without a registration step, and
	 * hierarchy still works by prefix: "Floor." matches "Floor.08".
	 */
	UPROPERTY(config, EditAnywhere, Category = "Selection")
	FName FloorTagPrefix;

	UPROPERTY(config, EditAnywhere, Category = "Selection")
	FName RoomTagPrefix;

	UPROPERTY(config, EditAnywhere, Category = "Selection")
	FName SystemTagPrefix;

	/**
	 * World units the focus slab fades over at its top and bottom faces.
	 *
	 * Larger than the clip feather because this edge is a soft visual
	 * transition between a focused storey and its neighbours, not a cut - a
	 * hard line across a stairwell reads as an error, a gradient reads as
	 * depth of field.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Selection", meta = (ClampMin = "0.0"))
	float FocusSlabFeather;

	/**
	 * How much the slab is grown beyond the tagged geometry's bounds.
	 *
	 * A floor's tagged actors stop at the ceiling, but the slab derived from
	 * them should include the ceiling itself and a little of the structure
	 * above, or the top surface of the storey you selected fades out.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Selection")
	float FocusSlabPadding;

	// --- Ghosting -----------------------------------------------------------

	/**
	 * Opacity of out-of-focus geometry, 0..1. 0 hides it entirely.
	 *
	 * Delivered as a dither pattern on an already-Masked material rather than
	 * as true translucency, so it costs nothing and needs no blend mode
	 * change. Very low values stipple visibly before TAA resolves them;
	 * around 0.1-0.25 reads as glass.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Ghosting", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float GhostOpacity;

	// --- Behaviour ----------------------------------------------------------

	/**
	 * World units over which a cut edge fades from solid to removed.
	 *
	 * Zero gives a hard, aliased edge: the clip test is a binary decision per
	 * pixel, so the boundary lands wherever the pixel centres happen to fall
	 * and crawls as the box moves. A few units of feather turns that into a
	 * gradient the opacity mask can dither or blend, which reads as a clean
	 * edge instead of a stair-step. Costs nothing - it is a saturate() on a
	 * distance that has already been computed.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Clipping", meta = (ClampMin = "0.0", UIMax = "20.0"))
	float EdgeFeatherWidth;

	/** Push the collection every frame rather than only when a volume actually changes. Diagnostic only. */
	UPROPERTY(config, EditAnywhere, Category = "Advanced", meta = (AdvancedDisplay))
	bool bForcePushEveryFrame;
};
