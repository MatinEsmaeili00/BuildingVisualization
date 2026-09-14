# Copyright Matin. All Rights Reserved.
#
# Creates the assets the clipping system needs, then injects the clip test into
# existing materials in bulk.
#
# Run from the editor's Python console:
#
#     import setup_building_visualization as bv
#     bv.create_assets()
#     bv.inject_into_path("/Game/Building")
#
# WHY THIS SCRIPT EXISTS
# ----------------------
# A per-pixel cut has to execute in the material shader. There is no engine
# switch that injects clip() into every material, so the clip test must live in
# the material graph - and the obvious reading of that is "edit thousands of
# assets by hand", which is untenable.
#
# It is untenable and also unnecessary, because meshes and materials are not
# the same count. A 400,000-component building typically shares a few dozen
# materials. Injecting a single function call into thirty materials is a script,
# not a project. This is that script.
#
# Nothing here runs at runtime. It is a one-time (or re-runnable) authoring
# step; the runtime cost is the material function itself.

import unreal

# Must match BuildingVisualization::MaxClipVolumes in BuildingVisualizationTypes.h.
# If you change one, change the other - the C++ writes slots the material never
# reads otherwise, and the extra volumes simply do nothing with no error.
MAX_CLIP_VOLUMES = 4

# Must match UBuildingVisualizationSettings defaults.
CLIP_PREFIX = "Clip"
GLOBALS_NAME = "ClipGlobals"

ASSET_PATH = "/Game/BuildingVisualization"
MPC_NAME = "MPC_BuildingClip"
FUNCTION_NAME = "MF_BuildingClip"

INCLUDE_PATH = "/BuildingVisualization/Private/BuildingClipping.ush"

_asset_tools = unreal.AssetToolsHelpers.get_asset_tools()
_mel = unreal.MaterialEditingLibrary


def _log(msg):
    unreal.log("[BuildingVisualization] {}".format(msg))


def _param_names():
    """The full ordered list of vector parameters, matching the C++ naming."""
    names = []
    for i in range(MAX_CLIP_VOLUMES):
        names.append("{}Row0_{}".format(CLIP_PREFIX, i))
        names.append("{}Row1_{}".format(CLIP_PREFIX, i))
        names.append("{}Row2_{}".format(CLIP_PREFIX, i))
        names.append("{}Params_{}".format(CLIP_PREFIX, i))
    names.append(GLOBALS_NAME)
    return names


# ---------------------------------------------------------------------------
# Asset creation
# ---------------------------------------------------------------------------

def create_parameter_collection():
    """
    Creates the MPC with one vector per matrix row plus the globals vector.

    All parameters are vectors, never scalars. An MPC packs scalars four to a
    register but vectors one per register, and mixing the two makes the layout
    harder to reason about for no saving - we need whole float4s anyway.
    """
    package = "{}/{}".format(ASSET_PATH, MPC_NAME)

    if unreal.EditorAssetLibrary.does_asset_exist(package):
        collection = unreal.EditorAssetLibrary.load_asset(package)
        _log("Parameter collection already exists, updating layout: {}".format(package))
    else:
        collection = _asset_tools.create_asset(
            MPC_NAME, ASSET_PATH,
            unreal.MaterialParameterCollection,
            unreal.MaterialParameterCollectionFactoryNew())
        _log("Created parameter collection: {}".format(package))

    vectors = []
    for name in _param_names():
        entry = unreal.CollectionVectorParameter()
        entry.set_editor_property("parameter_name", name)
        # Default of all zeros means mode 0 (Disabled), so a freshly created
        # collection clips nothing rather than making the building vanish.
        entry.set_editor_property("default_value", unreal.LinearColor(0.0, 0.0, 0.0, 0.0))
        vectors.append(entry)

    collection.set_editor_property("vector_parameters", vectors)
    unreal.EditorAssetLibrary.save_asset(package)
    return collection


def _build_custom_node_code():
    """
    The Custom node body: one call into the .ush, with every parameter passed
    through explicitly.

    The include does the real work. Putting the maths in a .ush rather than in
    this string means it is version-controlled, diffable, and editable without
    regenerating an asset - a long HLSL blob living inside a .uasset is
    effectively unreviewable.
    """
    args = []
    for i in range(MAX_CLIP_VOLUMES):
        args.extend([
            "Row0_{}".format(i), "Row1_{}".format(i),
            "Row2_{}".format(i), "Params_{}".format(i),
        ])
    return "return BuildingClip_Evaluate(WorldPos, {}, Globals);".format(", ".join(args))


def create_material_function():
    """
    Builds MF_BuildingClip: zero inputs, one output (the opacity mask).

    Reading the collection INSIDE the function is the key design choice. It
    means injecting the effect into a material is a single node plus a single
    connection - no parameter plumbing per material, and no chance of one
    material being wired to the wrong slot. The function is the only place that
    knows the collection layout.
    """
    package = "{}/{}".format(ASSET_PATH, FUNCTION_NAME)

    if unreal.EditorAssetLibrary.does_asset_exist(package):
        unreal.EditorAssetLibrary.delete_asset(package)
        _log("Removed previous material function so it can be rebuilt cleanly.")

    func = _asset_tools.create_asset(
        FUNCTION_NAME, ASSET_PATH,
        unreal.MaterialFunction,
        unreal.MaterialFunctionFactoryNew())

    collection = unreal.EditorAssetLibrary.load_asset("{}/{}".format(ASSET_PATH, MPC_NAME))
    if collection is None:
        raise RuntimeError("Create the parameter collection before the material function.")

    custom = _mel.create_material_expression_in_function(
        func, unreal.MaterialExpressionCustom, -400, 0)
    custom.set_editor_property("code", _build_custom_node_code())
    custom.set_editor_property("output_type", unreal.CustomMaterialOutputType.CMOT_FLOAT1)
    custom.set_editor_property("description", "BuildingClip")
    custom.set_editor_property("include_file_paths", [INCLUDE_PATH])

    # Custom node inputs, in the order the generated HLSL expects.
    inputs = []

    world_pos = _mel.create_material_expression_in_function(
        func, unreal.MaterialExpressionWorldPosition, -800, -200)
    inputs.append(("WorldPos", world_pos))

    y = -100
    for name in _param_names():
        node = _mel.create_material_expression_in_function(
            func, unreal.MaterialExpressionCollectionParameter, -800, y)
        node.set_editor_property("collection", collection)
        node.set_editor_property("parameter_name", name)
        # "Globals" rather than "ClipGlobals" as the HLSL identifier, so the
        # generated call reads cleanly.
        ident = "Globals" if name == GLOBALS_NAME else name[len(CLIP_PREFIX):]
        inputs.append((ident, node))
        y += 60

    custom_inputs = []
    for ident, _node in inputs:
        entry = unreal.CustomInput()
        entry.set_editor_property("input_name", ident)
        custom_inputs.append(entry)
    custom.set_editor_property("inputs", custom_inputs)

    for ident, node in inputs:
        _mel.connect_material_expressions(node, "", custom, ident)

    output = _mel.create_material_expression_in_function(
        func, unreal.MaterialExpressionFunctionOutput, 0, 0)
    output.set_editor_property("output_name", "ClipMask")
    output.set_editor_property(
        "description",
        "1 keeps the pixel, 0 removes it. Multiply into Opacity Mask on a Masked material.")
    _mel.connect_material_expressions(custom, "", output, "")

    func.set_editor_property(
        "description",
        "Building Visualization: world-space clip mask. Drive Opacity Mask with this "
        "and set Blend Mode to Masked.")

    unreal.EditorAssetLibrary.save_asset(package)
    _log("Created material function: {}".format(package))
    return func


def create_assets():
    """Creates both assets. Safe to re-run; the function is rebuilt each time."""
    unreal.EditorAssetLibrary.make_directory(ASSET_PATH)
    create_parameter_collection()
    create_material_function()
    _log("Setup complete. Point Project Settings > Plugins > Building Visualization "
         "at {}/{}.".format(ASSET_PATH, MPC_NAME))


# ---------------------------------------------------------------------------
# Bulk injection
# ---------------------------------------------------------------------------

def inject_into_material(material, func):
    """
    Adds the clip function to one material and routes it to Opacity Mask.

    Two things have to happen together, and the second is the one people miss.

    1. The mask is multiplied into Opacity Mask rather than replacing it, so a
       material that already uses masking - foliage cards, grates, perforated
       panels - keeps its own cutout instead of having it silently discarded.

    2. Blend Mode becomes Masked. This is not cosmetic. An Opaque material
       still renders in the depth prepass, and the prepass does not evaluate
       the opacity mask; the clipped pixels would write depth, then the base
       pass would discard them, leaving holes that occlude whatever is behind
       them. The pipes you were trying to reveal would be hidden by the depth
       of the wall you just cut away. Masked makes the prepass run the same
       test, so depth and colour agree.

    Returns True if the material was modified.
    """
    if material is None:
        return False

    # Idempotent: re-running the injector over a path must not stack up calls.
    for expr in unreal.MaterialEditingLibrary.get_material_expressions(material):
        if isinstance(expr, unreal.MaterialExpressionMaterialFunctionCall):
            existing = expr.get_editor_property("material_function")
            if existing and existing.get_name() == FUNCTION_NAME:
                return False

    call = _mel.create_material_expression(
        material, unreal.MaterialExpressionMaterialFunctionCall, -400, 400)
    call.set_editor_property("material_function", func)

    existing_mask = _mel.get_material_property_input_node(
        material, unreal.MaterialProperty.MP_OPACITY_MASK)

    if existing_mask is not None:
        mult = _mel.create_material_expression(
            material, unreal.MaterialExpressionMultiply, -200, 400)
        _mel.connect_material_expressions(call, "", mult, "B")
        # Reconnecting the original source into A preserves whatever cutout the
        # material already had.
        _mel.connect_material_expressions(existing_mask, "", mult, "A")
        _mel.connect_material_property(mult, "", unreal.MaterialProperty.MP_OPACITY_MASK)
    else:
        _mel.connect_material_property(call, "", unreal.MaterialProperty.MP_OPACITY_MASK)

    material.set_editor_property("blend_mode", unreal.BlendMode.BLEND_MASKED)

    _mel.recompile_material(material)
    return True


def inject_into_path(content_path, dry_run=False):
    """
    Injects into every material under a content path.

    Pass dry_run=True first on a real building. This edits assets in place, and
    while it is idempotent it is not automatically undoable across hundreds of
    files - check the list before committing to it.
    """
    func = unreal.EditorAssetLibrary.load_asset("{}/{}".format(ASSET_PATH, FUNCTION_NAME))
    if func is None:
        raise RuntimeError("Run create_assets() first.")

    assets = unreal.EditorAssetLibrary.list_assets(content_path, recursive=True)
    changed, skipped = 0, 0

    for asset_path in assets:
        data = unreal.EditorAssetLibrary.find_asset_data(asset_path)
        if data.asset_class_path.asset_name != "Material":
            continue

        material = data.get_asset()
        if dry_run:
            _log("would inject into {}".format(asset_path))
            changed += 1
            continue

        if inject_into_material(material, func):
            unreal.EditorAssetLibrary.save_asset(asset_path)
            changed += 1
        else:
            skipped += 1

    verb = "would modify" if dry_run else "modified"
    _log("{} {} material(s), skipped {} already carrying the function."
         .format(verb, changed, skipped))
    return changed
