// Copyright Matin. All Rights Reserved.

#include "BuildingVisualizationSettings.h"

UBuildingVisualizationSettings::UBuildingVisualizationSettings()
	: ClipParameterPrefix(TEXT("Clip"))
	, GlobalsParameterName(TEXT("ClipGlobals"))
	, EdgeFeatherWidth(2.0f)
	, bForcePushEveryFrame(false)
{
}

FName UBuildingVisualizationSettings::GetCategoryName() const
{
	return TEXT("Plugins");
}

const UBuildingVisualizationSettings& UBuildingVisualizationSettings::Get()
{
	const UBuildingVisualizationSettings* Settings = GetDefault<UBuildingVisualizationSettings>();
	check(Settings);
	return *Settings;
}
