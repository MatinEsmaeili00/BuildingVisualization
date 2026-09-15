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
FOCUS_SLAB_NAME = "ClipFocusSlab"
GHOST_NAME = "ClipGhost"

ASSET_PATH = "/Game/BuildingVisualization"
MPC_NAME = "MPC_BuildingClip"
FUNCTION_NAME = "MF_BuildingClip"

INCLUDE_PATH = "/BuildingVisualization/Private/BuildingClipping.ush"

# Engine-provided, and a material function rather than a native expression -
# which is why it is referenced by path instead of constructed by class.
#
# Both spellings are tried because load_asset is inconsistent about whether it
# wants the trailing object name, and which one works has changed between
# engine versions. Cheap to try both; a wrong guess costs a confusing failure
# well downstream of the actual cause.
DITHER_FUNCTION_PATHS = [
    "/Engine/Functions/Engine_MaterialFunctions02/Utility/DitherTemporalAA",
    "/Engine/Functions/Engine_MaterialFunctions02/Utility/DitherTemporalAA.DitherTemporalAA",
]


def _load_dither_function():
    """
    Loads the engine dither function, tolerating both path spellings.

    unreal.load_asset() is used in preference to
    EditorAssetLibrary.load_asset(): the latter resolves only the FULL object
    path (".../DitherTemporalAA.DitherTemporalAA") and returns None for the
    package-only form, while the global accepts either. Verified by probe on
    5.8 - EditorAssetLibrary.does_asset_exist() likewise reports False for the
    package-only path, which is what makes this failure so confusing when you
    hit it: the file is plainly there on disk.
    """
    for path in DITHER_FUNCTION_PATHS:
        for loader in (unreal.load_asset, unreal.EditorAssetLibrary.load_asset):
            try:
                asset = loader(path)
            except Exception:
                asset = None
            if asset is not None:
                _log("Using dither function: {}".format(path))
                return asset
    return None

_asset_tools = unreal.AssetToolsHelpers.get_asset_tools()
_mel = unreal.MaterialEditingLibrary


def _log(msg):
    unreal.log("[BuildingVisualization] {}".format(msg))


def _connect(src, src_out, dst, dst_in, what):
    """
    connect_material_expressions returns False on failure and does not raise.

    Every silent failure here produces the same downstream symptom: the graph
    compiles fine, the pin sits at its default, and the feature does nothing
    with no error anywhere. An unconnected dither alpha in particular defaults
    to fully opaque, which is indistinguishable from "ghosting is broken".
    So every connection is checked, and a failure is loud.
    """
    if not _mel.connect_material_expressions(src, src_out, dst, dst_in):
        raise RuntimeError(
            "Failed to connect {}: output '{}' -> input '{}'. The pin name is "
            "probably wrong for this engine version.".format(what, src_out, dst_in))


# The engine dither function's input is "Alpha Threshold" - WITH A SPACE - not
# "Alpha", which is the obvious guess and silently fails. Determined by probing
# the live editor, because neither the asset (compressed) nor Python reflection
# (no function_inputs property) will tell you. An empty name also works, since
# that binds the first input, but relying on input ORDER is worse than relying
# on a name: adding an input upstream would silently rebind us.
DITHER_ALPHA_PIN_CANDIDATES = ["Alpha Threshold", "AlphaThreshold", "Alpha"]

# Set False to bypass the dither entirely - a diagnostic for separating "the
# ghost value is wrong" from "the dither is not working".
USE_DITHER = True


def _connect_any(src, src_out, dst, candidates, what):
    """Connects to the first pin name that takes, and says which one it used."""
    for name in candidates:
        if _mel.connect_material_expressions(src, src_out, dst, name):
            _log("Connected {} via pin '{}'.".format(what, name))
            return name
    raise RuntimeError(
        "Failed to connect {} - none of the candidate pin names {} were accepted. "
        "The engine function's signature has changed; probe it and update "
        "DITHER_ALPHA_PIN_CANDIDATES.".format(what, candidates))


def _param_names():
    """The full ordered list of vector parameters, matching the C++ naming."""
    names = []
    for i in range(MAX_CLIP_VOLUMES):
        names.append("{}Row0_{}".format(CLIP_PREFIX, i))
        names.append("{}Row1_{}".format(CLIP_PREFIX, i))
        names.append("{}Row2_{}".format(CLIP_PREFIX, i))
        names.append("{}Params_{}".format(CLIP_PREFIX, i))
    names.append(GLOBALS_NAME)
    names.append(FOCUS_SLAB_NAME)
    names.append(GHOST_NAME)
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
    # Parameters.SvPosition.xy is the pixel coordinate, used for the ghost
    # stipple. It is read directly from the Custom node's implicit Parameters
    # struct rather than wired in as a ScreenPosition input - one less
    # connection to build, and connections are the fragile part here.
    return ("return BuildingClip_Evaluate(WorldPos, {}, Globals, FocusSlab, Ghost, "
            "Parameters.SvPosition.xy);".format(", ".join(args)))


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

    # ALWAYS delete and recreate. Do not "rebuild in place".
    #
    # Rebuilding in place is the obvious choice - it preserves the references
    # in every already-injected material - and it does not work.
    # connect_material_expressions returns True, update_material_function
    # reports no error, and the resulting graph has every node present with its
    # wires missing. The material then compiles cleanly against an unconnected
    # pin sitting at its default, so the whole effect silently stops, with no
    # error in any log. It was verified by eye in the material editor: the
    # nodes were there, the connections were not.
    #
    # Creating the asset fresh works reliably. The cost is that references
    # dangle, so create_assets() re-injects afterwards to repair them - see
    # reinject_paths there. Slower and less elegant, but it actually works,
    # which beats elegant every time.
    if unreal.EditorAssetLibrary.does_asset_exist(package):
        unreal.EditorAssetLibrary.delete_asset(package)
        _log("Deleted previous material function (in-place rebuild is unreliable).")

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
    # A single float. Clip and ghost are combined inside the .ush, so nothing
    # downstream has to split channels or multiply - see the graph note below.
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
        _connect(node, "", custom, ident, "collection param -> custom." + ident)

    output = _mel.create_material_expression_in_function(
        func, unreal.MaterialExpressionFunctionOutput, 100, 0)
    output.set_editor_property("output_name", "ClipMask")
    output.set_editor_property(
        "description",
        "1 keeps the pixel, 0 removes it. Multiply into Opacity Mask on a Masked material.")
    _connect(custom, "", output, "", "custom -> function output")

    func.set_editor_property(
        "description",
        "Building Visualization: world-space clip mask. Drive Opacity Mask with this "
        "and set Blend Mode to Masked.")

    # Finalise the function.
    #
    # connect_material_expressions returns True for connections made inside a
    # material function, but without this call they do not survive to the
    # compiled function - the graph ends up with the nodes present and the
    # wires missing. Every dependent material then compiles cleanly against an
    # unconnected pin sitting at its default, so the feature silently does
    # nothing and there is no error anywhere to find.
    #
    # This is also what rebuilds the materials that reference the function, so
    # in-place edits actually reach the things using them.
    _mel.update_material_function(func)

    unreal.EditorAssetLibrary.save_asset(package)
    _log("Created material function: {}".format(package))
    return func


def create_assets(reinject_paths=None):
    """
    Creates both assets, then repairs materials that referenced the old function.

    reinject_paths matters whenever the system has already been injected
    somewhere. The material function is deleted and recreated rather than
    edited in place (see create_material_function for why), which leaves every
    previously injected material pointing at nothing - and a null function call
    is silent, so the effect just stops. Passing the content paths you injected
    into re-points them.

        bv.create_assets(reinject_paths=["/Game/Building", "/Game/LevelPrototyping"])
    """
    unreal.EditorAssetLibrary.make_directory(ASSET_PATH)
    create_parameter_collection()
    create_material_function()

    for path in (reinject_paths or []):
        inject_into_path(path)

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

    # Idempotent, and self-repairing.
    #
    # Because the function asset is deleted and recreated on every setup run,
    # materials injected earlier are left holding a call node whose function is
    # now null. Re-pointing that node is strictly better than adding a second
    # one: it keeps whatever downstream wiring the node already had, which in a
    # material that already used Opacity Mask is the multiply we built for it.
    for expr in unreal.MaterialEditingLibrary.get_material_expressions(material):
        if isinstance(expr, unreal.MaterialExpressionMaterialFunctionCall):
            existing = expr.get_editor_property("material_function")
            if existing is None:
                expr.set_editor_property("material_function", func)
                material.set_editor_property("blend_mode", unreal.BlendMode.BLEND_MASKED)
                _mel.recompile_material(material)
                _log("Repaired dangling function reference in {}.".format(material.get_name()))
                return True
            if existing.get_name() == FUNCTION_NAME:
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
