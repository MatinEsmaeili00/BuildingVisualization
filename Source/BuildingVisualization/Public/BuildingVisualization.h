// Copyright Matin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

DECLARE_LOG_CATEGORY_EXTERN(LogBuildingVisualization, Log, All);

/**
 * Runtime module for the building visualization system.
 *
 * Loads at PostConfigInit because it maps this plugin's Shaders/ directory into
 * the engine's virtual shader filesystem, and that mapping has to exist before
 * any material that #includes from it is compiled. A material Custom node
 * referencing /BuildingVisualization/... will fail to compile if the mapping is
 * registered later than the first shader compile of the session.
 */
class FBuildingVisualizationModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;
};
