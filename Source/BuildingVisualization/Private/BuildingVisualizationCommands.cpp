// Copyright Matin. All Rights Reserved.
//
// Console commands for driving and inspecting the system without a recompile.
//
// Console rather than Blueprint or a test actor, deliberately. The subsystem
// methods are already BlueprintCallable, so a Blueprint harness is possible -
// but it needs building, wiring to input, and a PIE session. These work by
// typing, in the editor viewport as well as in PIE, which matters because the
// clip volume is an authoring tool used outside play.
//
// The inspection commands (BV.Status, BV.ListTags, BV.DescribeTag) exist
// because every failure this system can have looks identical from the
// viewport: nothing happens. A tag typo, geometry imported without metadata,
// an unassigned parameter collection and a material missing the clip function
// are four completely different problems with one symptom, and guessing
// between them is miserable. These answer which one it is.

#include "BuildingVisualization.h"
#include "BuildingVisualizationSubsystem.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "HAL/IConsoleManager.h"
#include "Materials/Material.h"
#include "Materials/MaterialInterface.h"

namespace BuildingVisualizationCommands
{
	/**
	 * Finds the subsystem for whichever world the console is attached to.
	 *
	 * The world handed to a console command can legitimately be the editor
	 * world, a PIE world, or null. Reporting which one we resolved is worth
	 * the line: running a command against the editor world while staring at a
	 * PIE viewport produces "nothing happened" with no other clue.
	 */
	static UBuildingVisualizationSubsystem* GetSubsystem(UWorld* World)
	{
		if (!World)
		{
			// The console hands a world to commands typed into a viewport, but
			// not to ones dispatched with no context object - which is exactly
			// how automation calls them (Python's execute_console_command with
			// a null world, an -ExecCmds entry at startup). Refusing there made
			// every one of these commands untestable without a human at the
			// keyboard, which is the wrong way round for a tool whose entire
			// purpose is diagnosing a silent failure.
			//
			// Prefer a play world if one exists, on the grounds that if someone
			// is in PIE that is what they are looking at.
			for (const FWorldContext& Context : GEngine->GetWorldContexts())
			{
				if (Context.World() && (Context.WorldType == EWorldType::PIE || Context.WorldType == EWorldType::Game))
				{
					World = Context.World();
					break;
				}
			}

			if (!World)
			{
				for (const FWorldContext& Context : GEngine->GetWorldContexts())
				{
					if (Context.World() && Context.WorldType == EWorldType::Editor)
					{
						World = Context.World();
						break;
					}
				}
			}

			if (!World)
			{
				UE_LOG(LogBuildingVisualization, Error,
					TEXT("No world available for this command, and no Game, PIE or Editor world to fall ")
					TEXT("back to. Is a level loaded?"));
				return nullptr;
			}

			UE_LOG(LogBuildingVisualization, Verbose,
				TEXT("No world context supplied; using '%s'."), *World->GetName());
		}

		UBuildingVisualizationSubsystem* Subsystem = World->GetSubsystem<UBuildingVisualizationSubsystem>();
		if (!Subsystem)
		{
			UE_LOG(LogBuildingVisualization, Error,
				TEXT("No BuildingVisualization subsystem in world '%s' (type %d). The subsystem only ")
				TEXT("exists for Game, PIE, Editor and EditorPreview worlds."),
				*World->GetName(), static_cast<int32>(World->WorldType));
			return nullptr;
		}
		return Subsystem;
	}

	static FName ArgToTag(const TArray<FString>& Args, int32 Index)
	{
		return Args.IsValidIndex(Index) ? FName(*Args[Index]) : NAME_None;
	}

	static bool ArgToBool(const TArray<FString>& Args, int32 Index, bool bDefault)
	{
		if (!Args.IsValidIndex(Index))
		{
			return bDefault;
		}
		const FString& S = Args[Index];
		return S == TEXT("1") || S.Equals(TEXT("true"), ESearchCase::IgnoreCase)
			|| S.Equals(TEXT("on"), ESearchCase::IgnoreCase);
	}

	// --- Selection ----------------------------------------------------------

	static void SelectFloor(const TArray<FString>& Args, UWorld* World)
	{
		if (UBuildingVisualizationSubsystem* S = GetSubsystem(World))
		{
			const FName Tag = ArgToTag(Args, 0);
			if (Tag.IsNone())
			{
				UE_LOG(LogBuildingVisualization, Warning, TEXT("Usage: BV.SelectFloor Floor.03"));
				return;
			}
			UE_LOG(LogBuildingVisualization, Display,
				TEXT("BV.SelectFloor %s -> %s"), *Tag.ToString(),
				S->SelectFloor(Tag) ? TEXT("ok") : TEXT("FAILED (no actors matched)"));
		}
	}

	static void SelectRoom(const TArray<FString>& Args, UWorld* World)
	{
		if (UBuildingVisualizationSubsystem* S = GetSubsystem(World))
		{
			const FName Tag = ArgToTag(Args, 0);
			if (Tag.IsNone())
			{
				UE_LOG(LogBuildingVisualization, Warning, TEXT("Usage: BV.SelectRoom Room.301"));
				return;
			}
			UE_LOG(LogBuildingVisualization, Display,
				TEXT("BV.SelectRoom %s -> %s"), *Tag.ToString(),
				S->SelectRoom(Tag) ? TEXT("ok") : TEXT("FAILED (no actors matched)"));
		}
	}

	static void SelectSystem(const TArray<FString>& Args, UWorld* World)
	{
		if (UBuildingVisualizationSubsystem* S = GetSubsystem(World))
		{
			const FName Tag = ArgToTag(Args, 0);
			if (Tag.IsNone())
			{
				UE_LOG(LogBuildingVisualization, Warning, TEXT("Usage: BV.SelectSystem System.Pipe"));
				return;
			}
			UE_LOG(LogBuildingVisualization, Display,
				TEXT("BV.SelectSystem %s -> %s"), *Tag.ToString(),
				S->SelectSystem(Tag) ? TEXT("ok") : TEXT("FAILED (no actors matched)"));
		}
	}

	static void ClearSelection(const TArray<FString>& Args, UWorld* World)
	{
		if (UBuildingVisualizationSubsystem* S = GetSubsystem(World))
		{
			S->ClearSelection();
			UE_LOG(LogBuildingVisualization, Display, TEXT("BV.ClearSelection -> ok"));
		}
	}

	// --- Modes --------------------------------------------------------------

	static void Ghost(const TArray<FString>& Args, UWorld* World)
	{
		if (UBuildingVisualizationSubsystem* S = GetSubsystem(World))
		{
			const bool bOn = ArgToBool(Args, 0, true);
			S->EnableGhostMode(bOn);
			UE_LOG(LogBuildingVisualization, Display, TEXT("BV.Ghost %d -> ok"), bOn ? 1 : 0);
		}
	}

	static void GhostOpacity(const TArray<FString>& Args, UWorld* World)
	{
		if (UBuildingVisualizationSubsystem* S = GetSubsystem(World))
		{
			if (!Args.IsValidIndex(0))
			{
				UE_LOG(LogBuildingVisualization, Warning, TEXT("Usage: BV.GhostOpacity 0.15"));
				return;
			}
			const float Value = FCString::Atof(*Args[0]);
			S->SetGhostOpacity(Value);
			UE_LOG(LogBuildingVisualization, Display, TEXT("BV.GhostOpacity %.3f -> ok"), Value);
		}
	}

	static void Clip(const TArray<FString>& Args, UWorld* World)
	{
		if (UBuildingVisualizationSubsystem* S = GetSubsystem(World))
		{
			const bool bOn = ArgToBool(Args, 0, true);
			S->EnableClipping(bOn);
			UE_LOG(LogBuildingVisualization, Display, TEXT("BV.Clip %d -> ok"), bOn ? 1 : 0);
		}
	}

	static void Reset(const TArray<FString>& Args, UWorld* World)
	{
		if (UBuildingVisualizationSubsystem* S = GetSubsystem(World))
		{
			S->ResetVisualization();
			UE_LOG(LogBuildingVisualization, Display, TEXT("BV.Reset -> ok"));
		}
	}

	// --- Inspection ---------------------------------------------------------

	static void Status(const TArray<FString>& Args, UWorld* World)
	{
		if (UBuildingVisualizationSubsystem* S = GetSubsystem(World))
		{
			S->LogStatus();
		}
	}

	/**
	 * Every distinct actor tag in the world, with a count, optionally filtered
	 * by prefix. Answers "is the tag actually spelled how I think it is"
	 * before you spend an hour on a selection that silently matches nothing.
	 */
	static void ListTags(const TArray<FString>& Args, UWorld* World)
	{
		if (!World)
		{
			return;
		}

		const FString Filter = Args.IsValidIndex(0) ? Args[0] : FString();

		TMap<FName, int32> Counts;
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			for (const FName& Tag : It->Tags)
			{
				if (Filter.IsEmpty() || Tag.ToString().StartsWith(Filter))
				{
					++Counts.FindOrAdd(Tag);
				}
			}
		}

		if (Counts.Num() == 0)
		{
			UE_LOG(LogBuildingVisualization, Display,
				TEXT("BV.ListTags: no actor tags%s found in world '%s'."),
				Filter.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" matching '%s'"), *Filter),
				*World->GetName());
			return;
		}

		Counts.KeySort([](const FName& A, const FName& B) { return A.LexicalLess(B); });

		UE_LOG(LogBuildingVisualization, Display, TEXT("BV.ListTags (%d distinct):"), Counts.Num());
		for (const TPair<FName, int32>& Pair : Counts)
		{
			UE_LOG(LogBuildingVisualization, Display,
				TEXT("    %-28s %d actor(s)"), *Pair.Key.ToString(), Pair.Value);
		}
	}

	/**
	 * Every actor carrying one tag, with its class and bounds.
	 *
	 * This is the command that answers "is SelectRoom using my trigger volume
	 * or the walls?" - a question you cannot answer from the viewport, because
	 * both produce a box, and a wrong one just looks like a badly sized room.
	 */
	static void DescribeTag(const TArray<FString>& Args, UWorld* World)
	{
		if (!World)
		{
			return;
		}
		if (!Args.IsValidIndex(0))
		{
			UE_LOG(LogBuildingVisualization, Warning, TEXT("Usage: BV.DescribeTag Room.301"));
			return;
		}

		const FName Tag(*Args[0]);
		FBox Union(ForceInit);
		int32 Count = 0;

		for (TActorIterator<AActor> It(World); It; ++It)
		{
			AActor* Actor = *It;
			if (!Actor->ActorHasTag(Tag))
			{
				continue;
			}

			FVector Origin, Extent;
			Actor->GetActorBounds(/*bOnlyCollidingComponents=*/false, Origin, Extent);
			Union += FBox(Origin - Extent, Origin + Extent);
			++Count;

			// The class is the point of this line. A trigger volume and a wall
			// both report a box; only the class tells you which one you got.
			UE_LOG(LogBuildingVisualization, Display,
				TEXT("    %-40s %-24s centre (%.0f, %.0f, %.0f) extent (%.0f, %.0f, %.0f)"),
				*Actor->GetName(),
				*Actor->GetClass()->GetName(),
				Origin.X, Origin.Y, Origin.Z,
				Extent.X, Extent.Y, Extent.Z);
		}

		if (Count == 0)
		{
			UE_LOG(LogBuildingVisualization, Warning,
				TEXT("BV.DescribeTag '%s': nothing matched. Run BV.ListTags to see what exists."),
				*Tag.ToString());
			return;
		}

		UE_LOG(LogBuildingVisualization, Display,
			TEXT("BV.DescribeTag '%s': %d actor(s); union Z %.1f..%.1f, union extent (%.0f, %.0f, %.0f)"),
			*Tag.ToString(), Count, Union.Min.Z, Union.Max.Z,
			Union.GetExtent().X, Union.GetExtent().Y, Union.GetExtent().Z);
	}

	/**
	 * Prints what the RENDERER thinks a material is, not what the asset says.
	 *
	 * These can disagree, and when they do nothing else in this plugin's
	 * diagnostics will tell you. UMaterial::GetBlendMode() returns Opaque for a
	 * Masked material whenever bCanMaskedBeAssumedOpaque is set - a serialized
	 * flag, invisible in the material editor, not exposed to Python, and only
	 * recomputed by an editor-side change of the material. A material can
	 * therefore report Masked to every check you can make from script, carry a
	 * correct Opacity Mask expression, and still be drawn as fully opaque.
	 *
	 * Usage: BV.DumpMaterial /Game/Path/M_Thing
	 */
	static void DumpMaterial(const TArray<FString>& Args, UWorld* World)
	{
		if (!Args.IsValidIndex(0))
		{
			UE_LOG(LogBuildingVisualization, Warning,
				TEXT("Usage: BV.DumpMaterial /Game/Path/M_Thing"));
			return;
		}

		UMaterialInterface* Interface = LoadObject<UMaterialInterface>(nullptr, *Args[0]);
		if (!Interface)
		{
			UE_LOG(LogBuildingVisualization, Error, TEXT("No material at '%s'."), *Args[0]);
			return;
		}

		UMaterial* Material = Interface->GetMaterial();

		UE_LOG(LogBuildingVisualization, Display, TEXT("--- %s ---"), *Args[0]);
		UE_LOG(LogBuildingVisualization, Display, TEXT("  base material      : %s"),
			Material ? *Material->GetPathName() : TEXT("<none>"));

		// The whole point of this command: the stored value versus the value
		// the renderer actually uses.
		UE_LOG(LogBuildingVisualization, Display, TEXT("  BlendMode (stored) : %d"),
			Material ? static_cast<int32>(Material->BlendMode) : -1);
		UE_LOG(LogBuildingVisualization, Display, TEXT("  GetBlendMode()     : %d %s"),
			static_cast<int32>(Interface->GetBlendMode()),
			Interface->GetBlendMode() == BLEND_Masked ? TEXT("(Masked)")
				: Interface->GetBlendMode() == BLEND_Opaque ? TEXT("(Opaque) <-- mask will NOT be evaluated")
				: TEXT(""));
		UE_LOG(LogBuildingVisualization, Display, TEXT("  IsMasked()         : %s"),
			Interface->IsMasked() ? TEXT("true") : TEXT("false"));
		UE_LOG(LogBuildingVisualization, Display, TEXT("  OpacityMaskClipVal : %.4f"),
			Interface->GetOpacityMaskClipValue());

#if WITH_EDITOR
		if (Material)
		{
			UE_LOG(LogBuildingVisualization, Display, TEXT("  OpacityMask pin    : %s"),
				Material->GetEditorOnlyData()->OpacityMask.IsConnected() ? TEXT("connected") : TEXT("NOT connected"));
			UE_LOG(LogBuildingVisualization, Display, TEXT("  IsPropertyConnected: %s"),
				Material->IsPropertyConnected(MP_OpacityMask) ? TEXT("true") : TEXT("false <-- cached data is stale"));
		}
#endif
	}
}

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

#define BV_CONSOLE_COMMAND(Name, Help, Fn)                                  \
	static FAutoConsoleCommandWithWorldAndArgs BVCmd_##Fn(                  \
		TEXT(Name), TEXT(Help),                                             \
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(              \
			&BuildingVisualizationCommands::Fn))

BV_CONSOLE_COMMAND("BV.SelectFloor",   "Focus one storey by tag. Usage: BV.SelectFloor Floor.03",        SelectFloor);
BV_CONSOLE_COMMAND("BV.SelectRoom",    "Isolate a room by tag. Usage: BV.SelectRoom Room.301",           SelectRoom);
BV_CONSOLE_COMMAND("BV.SelectSystem",  "Isolate a system by tag. Usage: BV.SelectSystem System.Pipe",    SelectSystem);
BV_CONSOLE_COMMAND("BV.ClearSelection","Clear floor/room selection, leaving placed clip volumes alone.", ClearSelection);
BV_CONSOLE_COMMAND("BV.Ghost",         "Ghost out-of-focus geometry. Usage: BV.Ghost 1",                 Ghost);
BV_CONSOLE_COMMAND("BV.GhostOpacity",  "Opacity of ghosted geometry, 0..1. Usage: BV.GhostOpacity 0.15", GhostOpacity);
BV_CONSOLE_COMMAND("BV.Clip",          "Master clipping switch. Usage: BV.Clip 0",                       Clip);
BV_CONSOLE_COMMAND("BV.Reset",         "Reset all visualization state.",                                 Reset);
BV_CONSOLE_COMMAND("BV.Status",        "Print current clipping/selection/ghost state.",                  Status);
BV_CONSOLE_COMMAND("BV.ListTags",      "List actor tags in the world. Usage: BV.ListTags Room.",         ListTags);
BV_CONSOLE_COMMAND("BV.DescribeTag",   "List actors with a tag, their class and bounds.",                DescribeTag);
BV_CONSOLE_COMMAND("BV.DumpMaterial",  "What the renderer thinks a material is. Usage: BV.DumpMaterial /Game/X/M_Y", DumpMaterial);

#undef BV_CONSOLE_COMMAND
