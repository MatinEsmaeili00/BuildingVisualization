// Copyright Matin. All Rights Reserved.

#include "ClippingVolumeActor.h"

#include "BuildingVisualization.h"
#include "BuildingVisualizationSubsystem.h"
#include "Components/BoxComponent.h"
#include "Engine/World.h"

AClippingVolumeActor::AClippingVolumeActor()
{
	// Nothing to tick. The subsystem pulls from registered volumes; a volume
	// that is sitting still costs literally nothing per frame.
	PrimaryActorTick.bCanEverTick = false;

	BoxComponent = CreateDefaultSubobject<UBoxComponent>(TEXT("ClipBox"));
	BoxComponent->SetBoxExtent(FVector(200.0f, 200.0f, 200.0f));
	BoxComponent->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	BoxComponent->SetGenerateOverlapEvents(false);

	// Visible as a wireframe in the editor, invisible in game: it is a tool,
	// not set dressing. Drawn even when unselected so it can be found and
	// grabbed without hunting through the outliner.
	BoxComponent->SetHiddenInGame(true);
	BoxComponent->bDrawOnlyIfSelected = false;
	BoxComponent->ShapeColor = FColor(80, 200, 255);

	// Movable, or the transform is baked at build time and dragging it does
	// nothing - the whole feature is that this thing moves.
	BoxComponent->SetMobility(EComponentMobility::Movable);
	RootComponent = BoxComponent;
}

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

void AClippingVolumeActor::RegisterWithSubsystem()
{
	if (const UWorld* World = GetWorld())
	{
		if (UBuildingVisualizationSubsystem* Subsystem = World->GetSubsystem<UBuildingVisualizationSubsystem>())
		{
			Subsystem->RegisterClipVolume(this);
		}
	}
}

void AClippingVolumeActor::UnregisterFromSubsystem()
{
	if (const UWorld* World = GetWorld())
	{
		if (UBuildingVisualizationSubsystem* Subsystem = World->GetSubsystem<UBuildingVisualizationSubsystem>())
		{
			Subsystem->UnregisterClipVolume(this);
		}
	}
}

void AClippingVolumeActor::NotifyVolumeChanged() const
{
	if (const UWorld* World = GetWorld())
	{
		if (UBuildingVisualizationSubsystem* Subsystem = World->GetSubsystem<UBuildingVisualizationSubsystem>())
		{
			Subsystem->MarkVolumesDirty();
		}
	}
}

// PostRegisterAllComponents rather than BeginPlay, because an editor world
// never calls BeginPlay and this tool has to work while you are building the
// level - dragging the box through a wall in the viewport is the primary way
// it gets used. BeginPlay/EndPlay are still overridden so PIE behaves
// identically; registration is AddUnique so the double call is harmless.
void AClippingVolumeActor::PostRegisterAllComponents()
{
	Super::PostRegisterAllComponents();
	RegisterWithSubsystem();
}

void AClippingVolumeActor::PostUnregisterAllComponents()
{
	UnregisterFromSubsystem();
	Super::PostUnregisterAllComponents();
}

void AClippingVolumeActor::BeginPlay()
{
	Super::BeginPlay();
	RegisterWithSubsystem();
}

void AClippingVolumeActor::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	UnregisterFromSubsystem();
	Super::EndPlay(EndPlayReason);
}

#if WITH_EDITOR

void AClippingVolumeActor::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);
	NotifyVolumeChanged();
}

// Fires continuously while the gizmo is dragged, not just on release
// (bFinished == false during the drag), which is what makes the cut follow the
// box live instead of snapping when you let go.
void AClippingVolumeActor::PostEditMove(bool bFinished)
{
	Super::PostEditMove(bFinished);
	NotifyVolumeChanged();
}

#endif // WITH_EDITOR

// ---------------------------------------------------------------------------
// Packing
// ---------------------------------------------------------------------------

void AClippingVolumeActor::SetClipMode(EBuildingClipMode NewMode)
{
	if (ClipMode != NewMode)
	{
		ClipMode = NewMode;
		NotifyVolumeChanged();
	}
}

FBuildingClipVolumeGPU AClippingVolumeActor::BuildGPUData() const
{
	FBuildingClipVolumeGPU Out;

	if (!BoxComponent)
	{
		return Out;
	}

	// The inverse transform carries rotation AND scale, so the shader gets
	// oriented and non-uniformly scaled boxes for free: transform the world
	// position by this and the test collapses to abs(local) <= extents on
	// three axes. No quaternion, no per-axis scale, no branching on shape.
	//
	// ToInverseMatrixWithScale rather than plain ToMatrixNoScale: dropping
	// scale here would silently ignore the actor's scale handles, which is the
	// most obvious way a user expects to resize the box.
	const FMatrix WorldToLocal = BoxComponent->GetComponentTransform().ToInverseMatrixWithScale();

	// Rows, with the translation term in W. UE matrices are row-vector
	// convention (v * M), so row i of the transposed matrix is what a
	// column-vector shader multiply (mul(M, v)) needs against component i.
	const FMatrix T = WorldToLocal.GetTransposed();
	Out.Row0 = FVector4f(static_cast<float>(T.M[0][0]), static_cast<float>(T.M[0][1]), static_cast<float>(T.M[0][2]), static_cast<float>(T.M[0][3]));
	Out.Row1 = FVector4f(static_cast<float>(T.M[1][0]), static_cast<float>(T.M[1][1]), static_cast<float>(T.M[1][2]), static_cast<float>(T.M[1][3]));
	Out.Row2 = FVector4f(static_cast<float>(T.M[2][0]), static_cast<float>(T.M[2][1]), static_cast<float>(T.M[2][2]), static_cast<float>(T.M[2][3]));

	// Unscaled, because the scale is already inside the matrix above. Using
	// the scaled extents here would apply the actor's scale twice.
	const FVector Extents = BoxComponent->GetUnscaledBoxExtent();
	Out.Params = FVector4f(
		static_cast<float>(Extents.X),
		static_cast<float>(Extents.Y),
		static_cast<float>(Extents.Z),
		static_cast<float>(static_cast<uint8>(ClipMode)));

	return Out;
}
