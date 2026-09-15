// Copyright Matin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "BuildingVisualizationLibrary.generated.h"

class UMaterial;

/**
 * Editor-side repairs the material scripting API cannot perform.
 *
 * Everything here exists because the Python material API edits the graph but
 * never tells the material that its graph changed, and a UMaterial caches
 * decisions taken from that graph.
 */
UCLASS()
class BUILDINGVISUALIZATION_API UBuildingVisualizationLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	/**
	 * Makes the renderer agree that a Masked material is masked.
	 *
	 * `UMaterial::GetBlendMode()` returns **Opaque** for a material whose blend
	 * mode is Masked whenever `bCanMaskedBeAssumedOpaque` is set. That flag is
	 * an optimisation for materials whose opacity mask is trivially 1, it is
	 * serialized into the asset, it is not shown in the material editor, it is
	 * not exposed to Python, and nothing in the scripting API recomputes it.
	 *
	 * So a material authored Opaque and later given a Masked blend mode and an
	 * Opacity Mask expression by script keeps the stale flag, and is drawn
	 * fully opaque. Every check available from script - blend mode, the
	 * Opacity Mask pin, the cached connected-property data, even routing the
	 * mask to Base Colour to see its value - reports that everything is
	 * correct. Only `GetBlendMode()` disagrees, and only from C++.
	 *
	 * This recomputes the flag the way the engine's own conversion code does,
	 * then notifies the material so its shaders rebuild.
	 *
	 * @return true if the flag was wrong and has been corrected.
	 */
	UFUNCTION(BlueprintCallable, Category = "Building Visualization|Repair")
	static bool RepairMaskedBlendMode(UMaterial* Material);

	/** Whether the renderer currently treats this material as masked. */
	UFUNCTION(BlueprintPure, Category = "Building Visualization|Repair")
	static bool IsRendererMasked(UMaterialInterface* Material);
};
