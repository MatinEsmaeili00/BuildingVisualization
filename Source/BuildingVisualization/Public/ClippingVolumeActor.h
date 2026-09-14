// Copyright Matin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "BuildingVisualizationTypes.h"
#include "GameFramework/Actor.h"
#include "ClippingVolumeActor.generated.h"

class UBoxComponent;

/**
 * A movable, scalable box that cuts building geometry.
 *
 * Drop one in the level, drag it into a wall, and every clippable material
 * inside it stops drawing. The actor itself does almost nothing: it holds a
 * transform and some extents, and reports them to the subsystem, which packs
 * every registered volume into one Material Parameter Collection. The GPU does
 * the spatial test per pixel.
 *
 * Note what is NOT here. There is no overlap query, no list of affected
 * components, no iteration over the building. The actor does not know or care
 * what it is cutting, and its cost is independent of how many meshes are in
 * the level - which is the whole point when the level is a digital twin with
 * hundreds of thousands of objects.
 */
UCLASS(Blueprintable, ClassGroup = (BuildingVisualization), meta = (DisplayName = "Clipping Volume"))
class BUILDINGVISUALIZATION_API AClippingVolumeActor : public AActor
{
	GENERATED_BODY()

public:
	AClippingVolumeActor();

	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	/** Editor worlds never call BeginPlay, so registration hangs off component registration instead. */
	virtual void PostRegisterAllComponents() override;
	virtual void PostUnregisterAllComponents() override;

#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
	virtual void PostEditMove(bool bFinished) override;
#endif

	/** Packs this volume into the form the shader consumes. */
	FBuildingClipVolumeGPU BuildGPUData() const;

	/** What this volume does to geometry it covers. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Clipping")
	EBuildingClipMode ClipMode = EBuildingClipMode::CutInside;

	/**
	 * Lower numbers are packed into the collection first.
	 *
	 * Only the first MaxClipVolumes volumes in the world are sent to the GPU,
	 * so this is how you decide which ones win when a level contains more than
	 * the shader can evaluate.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Clipping")
	int32 Priority = 0;

	UFUNCTION(BlueprintCallable, Category = "Clipping")
	void SetClipMode(EBuildingClipMode NewMode);

	/** The box. Its unscaled extents plus the component transform define the volume. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Clipping")
	TObjectPtr<UBoxComponent> BoxComponent;

private:
	void RegisterWithSubsystem();
	void UnregisterFromSubsystem();

	/** Tells the subsystem its packed data is stale. Cheap - it just sets a dirty flag. */
	void NotifyVolumeChanged() const;
};
