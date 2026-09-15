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

	// --- Semantic selection -------------------------------------------------
	//
	// These DO iterate actors - once, when the selection changes. That is the
	// deliberate line: per-selection iteration is fine (it happens when a user
	// clicks something), per-frame iteration is not. Nothing below runs on tick.

	/**
	 * Focuses one storey, e.g. SelectFloor("Floor.08").
	 *
	 * Resolves the tag to a Z range from the bounds of the tagged actors, then
	 * publishes it as a slab every material tests against. A floor IS a slab
	 * in Z, which is why this needs no per-object data at all and costs one
	 * vector - it works identically on a building of any size.
	 */
	UFUNCTION(BlueprintCallable, Category = "Building Visualization|Selection")
	bool SelectFloor(FName FloorTag);

	/**
	 * Isolates a room by its tag, e.g. SelectRoom("Room.802").
	 *
	 * A room is not a world-space predicate the way a floor is, so this takes
	 * the different route: it derives an axis-aligned bounds from the tagged
	 * actors and drives an internal Cut Outside volume with it, reusing the
	 * clip machinery rather than inventing a second mechanism. Note that this
	 * consumes one of the MaxClipVolumes slots.
	 */
	UFUNCTION(BlueprintCallable, Category = "Building Visualization|Selection")
	bool SelectRoom(FName RoomTag);

	/** Isolates a system, e.g. SelectSystem("System.Pipe"). Same mechanism as SelectRoom. */
	UFUNCTION(BlueprintCallable, Category = "Building Visualization|Selection")
	bool SelectSystem(FName SystemTag);

	/** Clears floor and room/system selection, leaving user-placed clip volumes alone. */
	UFUNCTION(BlueprintCallable, Category = "Building Visualization|Selection")
	void ClearSelection();

	// --- Ghosting -----------------------------------------------------------

	/** Fades out-of-focus geometry instead of leaving it fully opaque. */
	UFUNCTION(BlueprintCallable, Category = "Building Visualization|Ghosting")
	void EnableGhostMode(bool bEnabled);

	UFUNCTION(BlueprintPure, Category = "Building Visualization|Ghosting")
	bool IsGhostModeEnabled() const { return bGhostEnabled; }

	/** 0 hides out-of-focus geometry entirely, 1 leaves it fully opaque. */
	UFUNCTION(BlueprintCallable, Category = "Building Visualization|Ghosting")
	void SetGhostOpacity(float NewOpacity);

	/**
	 * Dumps everything that decides what you are currently looking at.
	 *
	 * Worth having as a first-class function rather than a debugger watch,
	 * because every distinct failure of this system presents identically from
	 * the viewport - as nothing happening - and this separates them.
	 */
	UFUNCTION(BlueprintCallable, Category = "Building Visualization")
	void LogStatus() const;

private:
	/** Gathers registered volumes, sorts by priority, writes the collection. */
	void PushClipParameters();

	/**
	 * Union of the bounds of every actor carrying an exact tag.
	 *
	 * Returns false when nothing matched, which is the common authoring
	 * mistake (a typo, or geometry imported without metadata) and is worth
	 * distinguishing from "matched, but empty".
	 */
	bool ComputeTaggedBounds(FName Tag, FBox& OutBounds) const;

	/** Shared by SelectRoom and SelectSystem - they differ only in which prefix they expect. */
	bool SelectByTagAsIsolation(FName Tag, const FName& ExpectedPrefix, const TCHAR* DebugContext);

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
	TArray<FName> CenterNames;
	TArray<FName> ExtentNames;
	FName GlobalsName;
	FName FocusSlabName;
	FName GhostName;

	FName FloorTagPrefix;
	FName RoomTagPrefix;
	FName SystemTagPrefix;

	float EdgeFeatherWidth = 2.0f;
	float FocusSlabFeather = 25.0f;
	float FocusSlabPadding = 50.0f;
	float GhostOpacity = 0.15f;
	bool bForcePushEveryFrame = false;

	bool bClippingEnabled = true;
	bool bGhostEnabled = false;
	bool bVolumesDirty = true;
	bool bParameterNamesValid = false;

	/** Z range of the focused storey. Only meaningful while bFocusSlabActive. */
	float FocusSlabMinZ = 0.0f;
	float FocusSlabMaxZ = 0.0f;
	bool bFocusSlabActive = false;

	/**
	 * Isolation bounds from SelectRoom / SelectSystem.
	 *
	 * Held as a plain box rather than a spawned actor: spawning an actor to
	 * carry it would dirty the level, show up in the outliner, and get saved
	 * into the map, none of which is wanted for what is a transient view state.
	 */
	FBox SelectionIsolationBounds = FBox(ForceInit);
	bool bSelectionIsolationActive = false;
};
