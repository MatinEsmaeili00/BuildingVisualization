// Copyright Matin. All Rights Reserved.

#include "BuildingVisualizationSubsystem.h"

#include "BuildingVisualization.h"
#include "BuildingVisualizationSettings.h"
#include "ClippingVolumeActor.h"
#include "EngineUtils.h"
#include "Kismet/KismetMaterialLibrary.h"
#include "Materials/MaterialParameterCollection.h"

void UBuildingVisualizationSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	const UBuildingVisualizationSettings& Settings = UBuildingVisualizationSettings::Get();

	EdgeFeatherWidth = Settings.EdgeFeatherWidth;
	bForcePushEveryFrame = Settings.bForcePushEveryFrame;
	GlobalsName = Settings.GlobalsParameterName;
	FocusSlabName = Settings.FocusSlabParameterName;
	GhostName = Settings.GhostParameterName;

	FloorTagPrefix = Settings.FloorTagPrefix;
	RoomTagPrefix = Settings.RoomTagPrefix;
	SystemTagPrefix = Settings.SystemTagPrefix;

	FocusSlabFeather = Settings.FocusSlabFeather;
	FocusSlabPadding = Settings.FocusSlabPadding;
	GhostOpacity = Settings.GhostOpacity;

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
	CheckName(FocusSlabName);
	CheckName(GhostName);
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
	bGhostEnabled = false;
	bFocusSlabActive = false;
	bSelectionIsolationActive = false;
	SelectionIsolationBounds = FBox(ForceInit);
	bVolumesDirty = true;
	PushClipParameters();
}

// ---------------------------------------------------------------------------
// Semantic selection
// ---------------------------------------------------------------------------

bool UBuildingVisualizationSubsystem::ComputeTaggedBounds(FName Tag, FBox& OutBounds) const
{
	const UWorld* World = GetWorld();
	if (!World || Tag.IsNone())
	{
		return false;
	}

	OutBounds = FBox(ForceInit);
	int32 MatchCount = 0;

	// This is the one place the system looks at the building, and it runs on
	// selection, not on tick. An iteration over every actor is acceptable at
	// the moment a user clicks "floor 8"; it would not be acceptable at 60Hz,
	// which is why nothing in Tick does anything like it.
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		const AActor* Actor = *It;
		if (!Actor || !Actor->ActorHasTag(Tag))
		{
			continue;
		}

		FVector Origin, Extent;
		// bOnlyCollidingComponents = false: architectural geometry is very
		// often set to no-collision, and excluding it would silently produce
		// bounds covering only the handful of colliding actors.
		Actor->GetActorBounds(/*bOnlyCollidingComponents=*/false, Origin, Extent);
		OutBounds += FBox(Origin - Extent, Origin + Extent);
		++MatchCount;
	}

	if (MatchCount == 0)
	{
		UE_LOG(LogBuildingVisualization, Warning,
			TEXT("No actors carry the tag '%s'. Either the tag is misspelled, or the geometry was ")
			TEXT("imported without its metadata - check an actor's Tags array in the details panel."),
			*Tag.ToString());
		return false;
	}

	UE_LOG(LogBuildingVisualization, Verbose,
		TEXT("Tag '%s' matched %d actor(s); bounds Z %.1f..%.1f"),
		*Tag.ToString(), MatchCount, OutBounds.Min.Z, OutBounds.Max.Z);
	return true;
}

bool UBuildingVisualizationSubsystem::SelectFloor(FName FloorTag)
{
	// A warning rather than a refusal. The prefixes are a naming convention to
	// keep a large project legible, not a validation rule, and rejecting a tag
	// that resolves perfectly well would be obstructive.
	if (!FloorTagPrefix.IsNone() && !FloorTag.ToString().StartsWith(FloorTagPrefix.ToString()))
	{
		UE_LOG(LogBuildingVisualization, Warning,
			TEXT("SelectFloor('%s') does not start with the expected prefix '%s'. Proceeding anyway."),
			*FloorTag.ToString(), *FloorTagPrefix.ToString());
	}

	FBox Bounds;
	if (!ComputeTaggedBounds(FloorTag, Bounds))
	{
		return false;
	}

	// Only the Z range is kept. The horizontal extent of a storey is the whole
	// building footprint, so testing against it would do nothing except make
	// the slab wrong for an L-shaped floor plan.
	FocusSlabMinZ = static_cast<float>(Bounds.Min.Z) - FocusSlabPadding;
	FocusSlabMaxZ = static_cast<float>(Bounds.Max.Z) + FocusSlabPadding;
	bFocusSlabActive = true;
	bVolumesDirty = true;

	UE_LOG(LogBuildingVisualization, Log,
		TEXT("Focused '%s': Z %.1f..%.1f"), *FloorTag.ToString(), FocusSlabMinZ, FocusSlabMaxZ);
	return true;
}

bool UBuildingVisualizationSubsystem::SelectByTagAsIsolation(FName Tag, const FName& ExpectedPrefix, const TCHAR* DebugContext)
{
	if (!ExpectedPrefix.IsNone() && !Tag.ToString().StartsWith(ExpectedPrefix.ToString()))
	{
		UE_LOG(LogBuildingVisualization, Warning,
			TEXT("%s('%s') does not start with the expected prefix '%s'. Proceeding anyway."),
			DebugContext, *Tag.ToString(), *ExpectedPrefix.ToString());
	}

	FBox Bounds;
	if (!ComputeTaggedBounds(Tag, Bounds))
	{
		return false;
	}

	SelectionIsolationBounds = Bounds.ExpandBy(FocusSlabPadding);
	bSelectionIsolationActive = true;
	bVolumesDirty = true;

	UE_LOG(LogBuildingVisualization, Log,
		TEXT("%s('%s') isolating bounds %s"), DebugContext, *Tag.ToString(), *Bounds.ToString());
	return true;
}

bool UBuildingVisualizationSubsystem::SelectRoom(FName RoomTag)
{
	return SelectByTagAsIsolation(RoomTag, RoomTagPrefix, TEXT("SelectRoom"));
}

bool UBuildingVisualizationSubsystem::SelectSystem(FName SystemTag)
{
	return SelectByTagAsIsolation(SystemTag, SystemTagPrefix, TEXT("SelectSystem"));
}

void UBuildingVisualizationSubsystem::ClearSelection()
{
	// Deliberately leaves bClippingEnabled and the user's placed volumes
	// alone. Clearing a floor selection should not also switch off the
	// inspection box someone has positioned.
	bFocusSlabActive = false;
	bSelectionIsolationActive = false;
	SelectionIsolationBounds = FBox(ForceInit);
	bVolumesDirty = true;
}

// ---------------------------------------------------------------------------
// Ghosting
// ---------------------------------------------------------------------------

void UBuildingVisualizationSubsystem::EnableGhostMode(bool bEnabled)
{
	if (bGhostEnabled != bEnabled)
	{
		bGhostEnabled = bEnabled;
		bVolumesDirty = true;
	}
}

void UBuildingVisualizationSubsystem::SetGhostOpacity(float NewOpacity)
{
	const float Clamped = FMath::Clamp(NewOpacity, 0.0f, 1.0f);
	if (!FMath::IsNearlyEqual(GhostOpacity, Clamped))
	{
		GhostOpacity = Clamped;
		bVolumesDirty = true;
	}
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

	// Pack into the final slot list. The room/system isolation goes FIRST, so
	// that when a level has more volumes than slots it is the user's decorative
	// boxes that get dropped rather than the isolation they explicitly asked
	// for by clicking a room.
	TArray<FBuildingClipVolumeGPU> Packed;
	Packed.Reserve(BuildingVisualization::MaxClipVolumes);

	if (bClippingEnabled && bSelectionIsolationActive && SelectionIsolationBounds.IsValid)
	{
		// Axis-aligned, so the world-to-local matrix is a pure translation by
		// -centre and the rotation part stays identity. Building it by hand
		// rather than going through an FTransform keeps that obvious.
		const FVector Centre = SelectionIsolationBounds.GetCenter();
		const FVector Extent = SelectionIsolationBounds.GetExtent();

		FBuildingClipVolumeGPU Isolation;
		Isolation.Row0 = FVector4f(1.f, 0.f, 0.f, static_cast<float>(-Centre.X));
		Isolation.Row1 = FVector4f(0.f, 1.f, 0.f, static_cast<float>(-Centre.Y));
		Isolation.Row2 = FVector4f(0.f, 0.f, 1.f, static_cast<float>(-Centre.Z));
		Isolation.Params = FVector4f(
			static_cast<float>(Extent.X),
			static_cast<float>(Extent.Y),
			static_cast<float>(Extent.Z),
			static_cast<float>(static_cast<uint8>(EBuildingClipMode::CutOutside)));
		Packed.Add(Isolation);
	}

	for (AClippingVolumeActor* Volume : Active)
	{
		if (Packed.Num() >= BuildingVisualization::MaxClipVolumes)
		{
			UE_LOG(LogBuildingVisualization, Verbose,
				TEXT("More active clip volumes than the %d available slots; the lowest-priority are ignored."),
				BuildingVisualization::MaxClipVolumes);
			break;
		}
		Packed.Add(Volume->BuildGPUData());
	}

	for (int32 Index = 0; Index < BuildingVisualization::MaxClipVolumes; ++Index)
	{
		// Unused slots are written as an explicitly disabled volume rather
		// than skipped. Skipping would leave the previous occupant's matrix in
		// the collection, and it would keep cutting.
		const FBuildingClipVolumeGPU Data = Packed.IsValidIndex(Index)
			? Packed[Index]
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
		FLinearColor(static_cast<float>(Packed.Num()), EdgeFeatherWidth, 0.0f, 0.0f));

	// Focus slab: X = min Z, Y = max Z, Z = enabled, W = feather.
	UKismetMaterialLibrary::SetVectorParameterValue(this, ParameterCollection, FocusSlabName,
		FLinearColor(
			FocusSlabMinZ,
			FocusSlabMaxZ,
			bFocusSlabActive ? 1.0f : 0.0f,
			FocusSlabFeather));

	// Ghost: X = opacity, Y = enabled.
	//
	// Ghosting is gated on a focus slab existing. Without one every pixel is
	// "in focus", so enabling ghost mode alone would correctly change nothing -
	// but a user who toggled it and saw no difference would reasonably assume
	// it was broken. Sending it disabled keeps the shader honest, and the
	// warning below says why nothing happened.
	const bool bGhostActive = bGhostEnabled && bFocusSlabActive;
	if (bGhostEnabled && !bFocusSlabActive)
	{
		UE_LOG(LogBuildingVisualization, Verbose,
			TEXT("Ghost mode is on but no floor is selected, so nothing is out of focus to ghost. ")
			TEXT("Call SelectFloor first."));
	}

	UKismetMaterialLibrary::SetVectorParameterValue(this, ParameterCollection, GhostName,
		FLinearColor(GhostOpacity, bGhostActive ? 1.0f : 0.0f, 0.0f, 0.0f));
}
