// Copyright Matin. All Rights Reserved.

#include "BuildingVisualizationSubsystem.h"

#include "BuildingVisualization.h"
#include "BuildingVisualizationSettings.h"
#include "ClippingVolumeActor.h"
#include "Kismet/KismetMaterialLibrary.h"
#include "Materials/MaterialParameterCollection.h"

void UBuildingVisualizationSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	const UBuildingVisualizationSettings& Settings = UBuildingVisualizationSettings::Get();

	EdgeFeatherWidth = Settings.EdgeFeatherWidth;
	bForcePushEveryFrame = Settings.bForcePushEveryFrame;
	GlobalsName = Settings.GlobalsParameterName;

	// Build the parameter names once. FName construction from a formatted
	// string is not free, and this would otherwise run 4 x MaxClipVolumes
	// times per frame forever, for a result that never changes.
	const FString Prefix = Settings.ClipParameterPrefix.ToString();
	Row0Names.Reserve(BuildingVisualization::MaxClipVolumes);
	Row1Names.Reserve(BuildingVisualization::MaxClipVolumes);
	Row2Names.Reserve(BuildingVisualization::MaxClipVolumes);
	ParamNames.Reserve(BuildingVisualization::MaxClipVolumes);
	for (int32 Index = 0; Index < BuildingVisualization::MaxClipVolumes; ++Index)
	{
		Row0Names.Add(FName(*FString::Printf(TEXT("%sRow0_%d"), *Prefix, Index)));
		Row1Names.Add(FName(*FString::Printf(TEXT("%sRow1_%d"), *Prefix, Index)));
		Row2Names.Add(FName(*FString::Printf(TEXT("%sRow2_%d"), *Prefix, Index)));
		ParamNames.Add(FName(*FString::Printf(TEXT("%sParams_%d"), *Prefix, Index)));
	}

	if (!Settings.ParameterCollection.IsNull())
	{
		ParameterCollection = Settings.ParameterCollection.LoadSynchronous();
	}

	if (!ParameterCollection)
	{
		UE_LOG(LogBuildingVisualization, Warning,
			TEXT("No Material Parameter Collection is set in Project Settings > Plugins > Building ")
			TEXT("Visualization. Clip volumes will have no effect, because there is nothing for ")
			TEXT("materials to read. Run Python/setup_building_visualization.py to create one."));
		return;
	}

	// Validate the layout up front rather than failing silently every frame.
	// SetVectorParameterValue on a name the collection does not have logs a
	// warning and does nothing, so at 60fps that is a wall of identical spam
	// and an effect that never appears with no obvious cause.
	bParameterNamesValid = true;
	auto CheckName = [this](const FName& Name)
	{
		if (ParameterCollection->GetVectorParameterByName(Name) == nullptr)
		{
			UE_LOG(LogBuildingVisualization, Error,
				TEXT("Parameter collection %s has no vector parameter %s. The collection layout does ")
				TEXT("not match what this plugin expects - regenerate it with the Python setup script, ")
				TEXT("or fix ClipParameterPrefix in Project Settings."),
				*ParameterCollection->GetName(), *Name.ToString());
			bParameterNamesValid = false;
		}
	};

	for (int32 Index = 0; Index < BuildingVisualization::MaxClipVolumes; ++Index)
	{
		CheckName(Row0Names[Index]);
		CheckName(Row1Names[Index]);
		CheckName(Row2Names[Index]);
		CheckName(ParamNames[Index]);
	}
	CheckName(GlobalsName);
}

void UBuildingVisualizationSubsystem::Deinitialize()
{
	RegisteredVolumes.Reset();
	ParameterCollection = nullptr;
	Super::Deinitialize();
}

bool UBuildingVisualizationSubsystem::DoesSupportWorldType(EWorldType::Type WorldType) const
{
	// Editor is deliberately included. This is an authoring and inspection
	// tool as much as a runtime one - the box gets dragged through the
	// building in the viewport, and that must update live without PIE.
	return WorldType == EWorldType::Game
		|| WorldType == EWorldType::PIE
		|| WorldType == EWorldType::Editor
		|| WorldType == EWorldType::EditorPreview;
}

TStatId UBuildingVisualizationSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UBuildingVisualizationSubsystem, STATGROUP_Tickables);
}

// ---------------------------------------------------------------------------
// Registry
// ---------------------------------------------------------------------------

void UBuildingVisualizationSubsystem::RegisterClipVolume(AClippingVolumeActor* Volume)
{
	if (Volume)
	{
		RegisteredVolumes.AddUnique(Volume);
		bVolumesDirty = true;
	}
}

void UBuildingVisualizationSubsystem::UnregisterClipVolume(AClippingVolumeActor* Volume)
{
	const int32 Removed = RegisteredVolumes.RemoveAll(
		[Volume](const TWeakObjectPtr<AClippingVolumeActor>& Weak)
		{
			return Weak.Get() == Volume;
		});

	if (Removed > 0)
	{
		// Must repush. The slot this volume occupied still holds its last
		// matrix on the GPU, so without a rewrite a deleted box carries on
		// cutting a hole in the building.
		bVolumesDirty = true;
	}
}

void UBuildingVisualizationSubsystem::EnableClipping(bool bEnabled)
{
	if (bClippingEnabled != bEnabled)
	{
		bClippingEnabled = bEnabled;
		bVolumesDirty = true;
	}
}

void UBuildingVisualizationSubsystem::ResetVisualization()
{
	bClippingEnabled = false;
	bVolumesDirty = true;
	PushClipParameters();
}

// ---------------------------------------------------------------------------
// Tick
// ---------------------------------------------------------------------------

void UBuildingVisualizationSubsystem::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	if (!ParameterCollection || !bParameterNamesValid)
	{
		return;
	}

	// A volume that is not moving costs nothing. Only a change - a drag, a
	// resize, a mode switch, a registration - triggers the write, because
	// setting a collection parameter dirties a uniform buffer that every
	// material referencing it then has to pick up.
	if (!bVolumesDirty && !bForcePushEveryFrame)
	{
		return;
	}

	PushClipParameters();
	bVolumesDirty = false;
}

void UBuildingVisualizationSubsystem::PushClipParameters()
{
	if (!ParameterCollection || !bParameterNamesValid)
	{
		return;
	}

	// Compact to live volumes, dropping any destroyed since the last push.
	TArray<AClippingVolumeActor*> Active;
	Active.Reserve(RegisteredVolumes.Num());
	for (auto It = RegisteredVolumes.CreateIterator(); It; ++It)
	{
		if (AClippingVolumeActor* Volume = It->Get())
		{
			if (bClippingEnabled && Volume->ClipMode != EBuildingClipMode::Disabled)
			{
				Active.Add(Volume);
			}
		}
		else
		{
			It.RemoveCurrent();
		}
	}

	// Stable order, so which volumes win when there are more than
	// MaxClipVolumes is a deliberate choice rather than registration order -
	// which would otherwise depend on actor spawn order and could differ
	// between two loads of the same level.
	Active.Sort([](const AClippingVolumeActor& A, const AClippingVolumeActor& B)
	{
		if (A.Priority != B.Priority)
		{
			return A.Priority < B.Priority;
		}
		return A.GetName() < B.GetName();
	});

	const int32 NumToSend = FMath::Min(Active.Num(), BuildingVisualization::MaxClipVolumes);

	if (Active.Num() > BuildingVisualization::MaxClipVolumes)
	{
		UE_LOG(LogBuildingVisualization, Verbose,
			TEXT("%d active clip volumes but only %d slots; the %d lowest-priority are ignored."),
			Active.Num(), BuildingVisualization::MaxClipVolumes,
			Active.Num() - BuildingVisualization::MaxClipVolumes);
	}

	for (int32 Index = 0; Index < BuildingVisualization::MaxClipVolumes; ++Index)
	{
		// Unused slots are written as an explicitly disabled volume rather
		// than skipped. Skipping would leave the previous occupant's matrix in
		// the collection, and it would keep cutting.
		const FBuildingClipVolumeGPU Data = (Index < NumToSend)
			? Active[Index]->BuildGPUData()
			: FBuildingClipVolumeGPU();

		// Component-wise rather than a FLinearColor(FVector4f) conversion: the
		// implicit path is not guaranteed across UE versions, and these are
		// packed matrix rows, not colours - being explicit says so.
		auto ToColor = [](const FVector4f& V)
		{
			return FLinearColor(V.X, V.Y, V.Z, V.W);
		};

		UKismetMaterialLibrary::SetVectorParameterValue(this, ParameterCollection, Row0Names[Index], ToColor(Data.Row0));
		UKismetMaterialLibrary::SetVectorParameterValue(this, ParameterCollection, Row1Names[Index], ToColor(Data.Row1));
		UKismetMaterialLibrary::SetVectorParameterValue(this, ParameterCollection, Row2Names[Index], ToColor(Data.Row2));
		UKismetMaterialLibrary::SetVectorParameterValue(this, ParameterCollection, ParamNames[Index], ToColor(Data.Params));
	}

	UKismetMaterialLibrary::SetVectorParameterValue(this, ParameterCollection, GlobalsName,
		FLinearColor(static_cast<float>(NumToSend), EdgeFeatherWidth, 0.0f, 0.0f));
}
