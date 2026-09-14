// Copyright Matin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "BuildingVisualizationTypes.h"
#include "Subsystems/WorldSubsystem.h"
#include "BuildingVisualizationSubsystem.generated.h"

class AClippingVolumeActor;
class UMaterialParameterCollection;

/**
 * The single writer of the clipping parameter collection.
 *
 * Every clip volume in the world registers here; once per frame - and only
 * when something actually changed - this packs them into the Material
 * Parameter Collection that clippable materials read. That is the entire
 * CPU-side cost of the system, and it is O(number of clip volumes), not
 * O(number of meshes). A building with 400,000 components costs exactly as
 * much as an empty level.
 *
 * A world subsystem rather than a manager actor because there must be exactly
 * one writer. Two actors both pushing to one global collection would fight,
 * and the loser would be whichever ticked first.
 */
UCLASS()
class BUILDINGVISUALIZATION_API UBuildingVisualizationSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	// --- USubsystem ---------------------------------------------------------
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	virtual bool DoesSupportWorldType(EWorldType::Type WorldType) const override;

	// --- FTickableGameObject ------------------------------------------------
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;

	/** Editor worlds must tick, or dragging the box in the viewport wouldn't update the cut. */
	virtual bool IsTickableInEditor() const override { return true; }

	// --- Volume registry ----------------------------------------------------

	void RegisterClipVolume(AClippingVolumeActor* Volume);
	void UnregisterClipVolume(AClippingVolumeActor* Volume);

	/** Flags the packed data stale. Called whenever a volume moves, resizes or changes mode. */
	void MarkVolumesDirty() { bVolumesDirty = true; }

	/** Master switch. Turning clipping off zeroes every slot rather than leaving stale ones live. */
	UFUNCTION(BlueprintCallable, Category = "Building Visualization")
	void EnableClipping(bool bEnabled);

	UFUNCTION(BlueprintPure, Category = "Building Visualization")
	bool IsClippingEnabled() const { return bClippingEnabled; }

	/** Drops every volume back to inert and pushes that immediately. */
	UFUNCTION(BlueprintCallable, Category = "Building Visualization")
	void ResetVisualization();

private:
	/** Gathers registered volumes, sorts by priority, writes the collection. */
	void PushClipParameters();

	/** Resolved once at Initialize; a soft pointer so a project not using it never loads it. */
	UPROPERTY(Transient)
	TObjectPtr<UMaterialParameterCollection> ParameterCollection = nullptr;

	UPROPERTY(Transient)
	TArray<TWeakObjectPtr<AClippingVolumeActor>> RegisteredVolumes;

	/** Cached from settings at Initialize so the names aren't rebuilt every frame. */
	TArray<FName> Row0Names;
	TArray<FName> Row1Names;
	TArray<FName> Row2Names;
	TArray<FName> ParamNames;
	FName GlobalsName;

	float EdgeFeatherWidth = 2.0f;
	bool bForcePushEveryFrame = false;

	bool bClippingEnabled = true;
	bool bVolumesDirty = true;
	bool bParameterNamesValid = false;
};
