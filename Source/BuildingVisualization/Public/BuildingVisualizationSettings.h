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
