// Copyright Matin. All Rights Reserved.

#include "BuildingVisualizationLibrary.h"

#include "BuildingVisualization.h"
#include "Materials/Material.h"
#include "Materials/MaterialInterface.h"

bool UBuildingVisualizationLibrary::IsRendererMasked(UMaterialInterface* Material)
{
	return Material && Material->GetBlendMode() == BLEND_Masked;
}

bool UBuildingVisualizationLibrary::RepairMaskedBlendMode(UMaterial* Material)
{
#if WITH_EDITOR
	if (!Material)
	{
		return false;
	}

	UMaterialEditorOnlyData* EditorOnly = Material->GetEditorOnlyData();
	if (!EditorOnly)
	{
		return false;
	}

	// The same expression the engine uses when it converts a material's blend
	// mode itself: the mask may be skipped only when there is no expression
	// driving it and no constant below 1.
	const bool bCorrect =
		!EditorOnly->OpacityMask.Expression &&
		!(EditorOnly->OpacityMask.UseConstant && EditorOnly->OpacityMask.Constant < 0.999f);

	if (Material->bCanMaskedBeAssumedOpaque == bCorrect)
	{
		return false;
	}

	UE_LOG(LogBuildingVisualization, Display,
		TEXT("%s: blend mode says Masked but the renderer reported %s. Correcting "
		     "bCanMaskedBeAssumedOpaque %s -> %s."),
		*Material->GetName(),
		Material->GetBlendMode() == BLEND_Masked ? TEXT("Masked") : TEXT("Opaque"),
		Material->bCanMaskedBeAssumedOpaque ? TEXT("true") : TEXT("false"),
		bCorrect ? TEXT("true") : TEXT("false"));

	Material->Modify();
	Material->bCanMaskedBeAssumedOpaque = bCorrect;

	// PostEditChange rather than just a recompile: the shader map has to be
	// rebuilt against the corrected blend mode, and every material instance
	// deriving from this one has to pick it up.
	Material->PostEditChange();
	Material->MarkPackageDirty();
	return true;
#else
	return false;
#endif
}
