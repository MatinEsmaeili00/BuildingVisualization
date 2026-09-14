// Copyright Matin. All Rights Reserved.

#include "BuildingVisualization.h"

#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"
#include "ShaderCore.h"

DEFINE_LOG_CATEGORY(LogBuildingVisualization);

#define LOCTEXT_NAMESPACE "FBuildingVisualizationModule"

void FBuildingVisualizationModule::StartupModule()
{
	// Map Shaders/ into the virtual shader filesystem so materials can
	// #include "/BuildingVisualization/Private/BuildingClipping.ush".
	//
	// Without this the path simply doesn't resolve and every material carrying
	// the clip function fails to compile - which, because a failed material is
	// silently swapped for the Default Material, looks like "the effect does
	// nothing" rather than like an error.
	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("BuildingVisualization"));
	if (Plugin.IsValid())
	{
		const FString ShaderDir = FPaths::Combine(Plugin->GetBaseDir(), TEXT("Shaders"));
		AddShaderSourceDirectoryMapping(TEXT("/BuildingVisualization"), ShaderDir);
	}
	else
	{
		UE_LOG(LogBuildingVisualization, Error,
			TEXT("Could not find the BuildingVisualization plugin to map its shader directory. "
			     "Any material including the clip function will fail to compile."));
	}
}

void FBuildingVisualizationModule::ShutdownModule()
{
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FBuildingVisualizationModule, BuildingVisualization)
