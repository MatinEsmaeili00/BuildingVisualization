// Copyright Matin. All Rights Reserved.

#include "BuildingVisualizationSettings.h"

UBuildingVisualizationSettings::UBuildingVisualizationSettings()
	: ClipParameterPrefix(TEXT("Clip"))
	, GlobalsParameterName(TEXT("ClipGlobals"))
	, FocusSlabParameterName(TEXT("ClipFocusSlab"))
	, GhostParameterName(TEXT("ClipGhost"))
	, FloorTagPrefix(TEXT("Floor."))
	, RoomTagPrefix(TEXT("Room."))
	, SystemTagPrefix(TEXT("System."))
	, FocusSlabFeather(25.0f)
	, FocusSlabPadding(50.0f)
	, GhostOpacity(0.15f)
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
