# Copyright Matin. All Rights Reserved.
#
# Creates the parameter collection, then injects the clip test directly into
# existing materials in bulk.
#
# Run from the editor's Python console:
#
#     import setup_building_visualization as bv
#     bv.create_assets()
#     bv.inject_into_path("/Game/Building", dry_run=True)   # check the list
#     bv.inject_into_path("/Game/Building")
#
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
# materials. Injecting into thirty materials is a script, not a project.
#
#
# WHY THERE IS NO MATERIAL FUNCTION
# ---------------------------------
# The natural design is one MF_BuildingClip material function that every
# material calls - one place that knows the collection layout, one node to
# inject. That was built, and it does not work.
#
# Connections made INSIDE a material function through the Python API do not
# persist. connect_material_expressions returns True,
# update_material_function reports no error, and the saved graph has every node
# present with its wires missing. The material then compiles cleanly against
# unconnected pins sitting at their defaults, so the entire effect silently
# does nothing, with no error in any log. Verified by opening the generated
# function and looking at it.
#
# Material-LEVEL graph building through the same API works reliably. So the
# whole node group is built directly in each material instead. The cost is
# ~21 nodes per material rather than one, which is only a cost at injection
# time - the compiled shader is identical either way, because a material
# function is inlined at compile time regardless.
#
# The trade is real, though: the collection layout is now duplicated into every
# injected material, so changing MAX_CLIP_VOLUMES or the parameter names means
# re-running the injector over everything rather than editing one asset.
# _remove_existing_clip_nodes exists to make that re-run safe.
#
# Nothing here runs at runtime. It is an authoring step.

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

INCLUDE_PATH = "/BuildingVisualization/Private/BuildingClipping.ush"

# Stamped into every node this script creates, so a re-run can find and remove
# the previous generation instead of stacking a second copy on top of it.
CLIP_NODE_TAG = "BuildingClipGenerated"

_asset_tools = unreal.AssetToolsHelpers.get_asset_tools()
_mel = unreal.MaterialEditingLibrary


def _log(msg):
    unreal.log("[BuildingVisualization] {}".format(msg))


def _connect(src, src_out, dst, dst_in, what):
    """
    connect_material_expressions returns False on failure and does not raise.

    Every silent failure here produces the same downstream symptom: the graph
    compiles fine, the pin sits at its default, and the feature does nothing
    with no error anywhere. So every connection is checked, and failure is loud.
    """
    if not _mel.connect_material_expressions(src, src_out, dst, dst_in):
        raise RuntimeError(
            "Failed to connect {}: output '{}' -> input '{}'.".format(what, src_out, dst_in))


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


def _hlsl_identifier(param_name):
    """ClipRow0_2 -> Row0_2, ClipGlobals -> Globals, ClipGhost -> Ghost."""
    return param_name[len(CLIP_PREFIX):]


# ---------------------------------------------------------------------------
# Parameter collection
# ---------------------------------------------------------------------------

def create_parameter_collection():
    """
    Creates the MPC with one vector per matrix row plus globals, slab and ghost.

    All parameters are vectors, never scalars. An MPC packs scalars four to a
    register but vectors one per register, and mixing the two makes the layout
    harder to reason about for no saving - whole float4s are needed anyway.
    """
    package = "{}/{}".format(ASSET_PATH, MPC_NAME)

    if unreal.EditorAssetLibrary.does_asset_exist(package):
        collection = unreal.EditorAssetLibrary.load_asset(package)
        _log("Parameter collection already exists, updating layout: {}".format(package))
    else:
        unreal.EditorAssetLibrary.make_directory(ASSET_PATH)
        collection = _asset_tools.create_asset(
            MPC_NAME, ASSET_PATH,
            unreal.MaterialParameterCollection,
            unreal.MaterialParameterCollectionFactoryNew())
        _log("Created parameter collection: {}".format(package))

    vectors = []
    for name in _param_names():
        entry = unreal.CollectionVectorParameter()
        entry.set_editor_property("parameter_name", name)
        # All zeros means mode 0 (Disabled) and no focus slab, so a freshly
        # created collection clips nothing rather than hiding the building.
        entry.set_editor_property("default_value", unreal.LinearColor(0.0, 0.0, 0.0, 0.0))
        vectors.append(entry)

    collection.set_editor_property("vector_parameters", vectors)
    unreal.EditorAssetLibrary.save_asset(package)
    return collection


def create_assets():
    """Creates the parameter collection. There is no material function any more."""
    create_parameter_collection()
    _log("Setup complete. Point Project Settings > Plugins > Building Visualization "
         "at {}/{}, then run inject_into_path() over your building materials."
         .format(ASSET_PATH, MPC_NAME))


# ---------------------------------------------------------------------------
# Injection
# ---------------------------------------------------------------------------

def _custom_node_code():
    """
    The Custom node body: one call into the .ush, every parameter explicit.

    The maths lives in the .ush rather than in this string so it is
    version-controlled, diffable and editable without touching any asset. A
    long HLSL blob living inside a .uasset is effectively unreviewable.

    ScreenUV for the ghost stipple arrives as an explicit INPUT, wired from a
    ScreenPosition node. It is emphatically NOT read from the Custom node's
    implicit Parameters struct: Parameters.SvPosition exists only in the
    pixel-shader parameter struct, and a material is translated for several
    stages including the vertex shader, so touching it implicitly makes the
    node valid in some permutations and not others. A ScreenPosition node is
    translated correctly for whichever stage is being generated.
    """
    args = []
    for i in range(MAX_CLIP_VOLUMES):
        args.extend([
            "Row0_{}".format(i), "Row1_{}".format(i),
            "Row2_{}".format(i), "Params_{}".format(i),
        ])
    return ("return BuildingClip_Evaluate(WorldPos, {}, Globals, FocusSlab, Ghost, "
            "ScreenUV);".format(", ".join(args)))


def _remove_existing_clip_nodes(material):
    """
    Deletes every node a previous run of this script created.

    Without this, re-running would leave the old Custom node and its twenty
    collection parameters orphaned in the graph - harmless to the compiled
    shader, since nothing reads them, but it makes the material unreadable
    after a few iterations and grows the asset every time.

    Returns how many were removed.
    """
    doomed = []
    for expr in _mel.get_material_expressions(material):
        try:
            if expr.get_editor_property("desc") == CLIP_NODE_TAG:
                doomed.append(expr)
                continue
        except Exception:
            pass

        # Also sweep up the previous architecture's leftovers: a call to the
        # old MF_BuildingClip, or a call whose function has gone null because
        # that asset was deleted. Both are inert, but a null function call is
        # indistinguishable from a working one at a glance, which makes it
        # exactly the kind of thing that wastes an afternoon.
        if isinstance(expr, unreal.MaterialExpressionMaterialFunctionCall):
            fn = expr.get_editor_property("material_function")
            if fn is None or fn.get_name() == "MF_BuildingClip":
                doomed.append(expr)

    for expr in doomed:
        _mel.delete_material_expression(material, expr)
    return len(doomed)


def _tag(expr):
    """Marks a node as ours so a later run can clean it up."""
    expr.set_editor_property("desc", CLIP_NODE_TAG)
    return expr


def inject_into_material(material, collection):
    """
    Builds the clip node group inside one material and routes it to Opacity Mask.

    Two things have to happen together, and the second is the one people miss.

    1. The mask is MULTIPLIED into Opacity Mask rather than replacing it, so a
       material that already uses masking - foliage cards, grates, perforated
       panels - keeps its own cutout instead of having it silently discarded.

    2. Blend Mode becomes Masked. This is not cosmetic. An Opaque material
       still renders in the depth prepass, and the prepass does not evaluate
       the opacity mask; the clipped pixels would write depth and then be
       discarded in the base pass, leaving the cut-away wall still occluding
       the pipes behind it. Masked makes the prepass run the same test, so
       depth and colour agree.

    Returns True if the material was modified.
    """
    if material is None:
        return False

    # Read the pre-existing opacity mask BEFORE touching anything.
    #
    # Order matters and the failure is severe. Reading it after creating our
    # own nodes can return one of them - in particular the multiply we are
    # about to build - and wiring that into its own input creates a cycle in
    # the graph. A cyclic material does not error: the translator follows it
    # forever, so the editor hangs while LOADING any level that uses the
    # material, before the map load is even logged. Diagnosed by diffing a
    # working boot log against a wedged one and seeing that "MAP LOAD" never
    # appeared.
    #
    # Anything tagged as ours is treated as absent, because on a re-run it is
    # about to be deleted a few lines below.
    existing_mask = _mel.get_material_property_input_node(
        material, unreal.MaterialProperty.MP_OPACITY_MASK)
    try:
        if existing_mask is not None and existing_mask.get_editor_property("desc") == CLIP_NODE_TAG:
            existing_mask = None
    except Exception:
        pass

    removed = _remove_existing_clip_nodes(material)

    # The Custom node. Its description becomes the generated HLSL function
    # name, which makes the compiled shader readable when something goes wrong.
    custom = _tag(_mel.create_material_expression(
        material, unreal.MaterialExpressionCustom, -900, 400))
    custom.set_editor_property("code", _custom_node_code())
    custom.set_editor_property("output_type", unreal.CustomMaterialOutputType.CMOT_FLOAT1)
    custom.set_editor_property("description", "BuildingClip")
    custom.set_editor_property("include_file_paths", [INCLUDE_PATH])

    # Inputs, in the order the generated HLSL expects.
    inputs = [
        ("WorldPos", _tag(_mel.create_material_expression(
            material, unreal.MaterialExpressionWorldPosition, -1400, 240))),
        ("ScreenUV", _tag(_mel.create_material_expression(
            material, unreal.MaterialExpressionScreenPosition, -1400, 300))),
    ]

    y = 380
    for name in _param_names():
        node = _tag(_mel.create_material_expression(
            material, unreal.MaterialExpressionCollectionParameter, -1400, y))
        node.set_editor_property("collection", collection)
        node.set_editor_property("parameter_name", name)
        inputs.append((_hlsl_identifier(name), node))
        y += 60

    custom_inputs = []
    for ident, _node in inputs:
        entry = unreal.CustomInput()
        entry.set_editor_property("input_name", ident)
        custom_inputs.append(entry)
    custom.set_editor_property("inputs", custom_inputs)

    for ident, node in inputs:
        _connect(node, "", custom, ident, "{} -> custom.{}".format(node.get_name(), ident))

    # BLEND MODE MUST BE SET BEFORE CONNECTING THE OPACITY MASK.
    #
    # This single line ordering is the difference between the feature working
    # and doing absolutely nothing. Connecting MP_OPACITY_MASK on a material
    # that is still Opaque is accepted and then ignored: the connection is
    # stored, get_material_property_input_node reports it correctly, the
    # material compiles without a warning - and the mask is never evaluated,
    # because an Opaque material has no opacity mask to evaluate.
    #
    # Setting Masked afterwards does not retroactively make the stored
    # connection live. It has to be Masked at the moment the connection is made.
    #
    # Diagnosed by wiring the Custom node to return a literal 0.0 - which should
    # have erased every surface - and watching the building render perfectly
    # intact, while a base-colour change on the same material turned it red
    # instantly. Same material, same recompile, one edit visible and the other
    # inert: that is what pointed at the property rather than at the graph.
    material.set_editor_property("blend_mode", unreal.BlendMode.BLEND_MASKED)

    if existing_mask is not None:
        mult = _tag(_mel.create_material_expression(
            material, unreal.MaterialExpressionMultiply, -600, 400))
        _connect(custom, "", mult, "B", "clip mask -> multiply B")
        _connect(existing_mask, "", mult, "A", "existing mask -> multiply A")
        _mel.connect_material_property(mult, "", unreal.MaterialProperty.MP_OPACITY_MASK)
    else:
        _mel.connect_material_property(custom, "", unreal.MaterialProperty.MP_OPACITY_MASK)

    _mel.recompile_material(material)

    if removed:
        _log("Replaced {} previously generated node(s) in {}.".format(removed, material.get_name()))
    return True


def remove_from_material(material):
    """
    Strips everything this script added and puts the material back to Opaque.

    A bulk injector needs a bulk uninstall. Without one the only way back is
    source control, and "revert thirty assets" is a much worse answer than
    "run the inverse function" - especially while the system is still being
    developed and injection is being re-run often.

    It is also the first thing to reach for when the editor starts behaving
    badly after an injection: strip everything, confirm the project is healthy
    again, and you have bisected the problem to this plugin in one step.
    """
    if material is None:
        return False

    removed = _remove_existing_clip_nodes(material)
    if removed == 0:
        return False

    # Opacity mask now points at a deleted node. Put the material back to
    # Opaque so nothing is left reading a dangling input.
    material.set_editor_property("blend_mode", unreal.BlendMode.BLEND_OPAQUE)
    _mel.recompile_material(material)
    _log("Removed {} node(s) from {} and restored Opaque.".format(removed, material.get_name()))
    return True


def remove_from_path(content_path):
    """Strips the clip nodes from every material under a content path."""
    changed = 0
    for asset_path in unreal.EditorAssetLibrary.list_assets(content_path, recursive=True):
        data = unreal.EditorAssetLibrary.find_asset_data(asset_path)
        if data.asset_class_path.asset_name != "Material":
            continue
        if remove_from_material(data.get_asset()):
            unreal.EditorAssetLibrary.save_asset(asset_path)
            changed += 1
    _log("Stripped {} material(s) under {}.".format(changed, content_path))
    return changed


def inject_into_path(content_path, dry_run=False):
    """
    Injects into every material under a content path.

    Only Material assets are touched, never Material Instances - an instance
    inherits its parent's graph, so injecting into the parent covers every
    instance of it for free. That is why a building sharing one master material
    across thousands of meshes costs exactly one injection.

    Pass dry_run=True first on a real building. This edits assets in place, and
    while it is safely re-runnable it is not automatically undoable across
    hundreds of files.
    """
    collection = unreal.EditorAssetLibrary.load_asset("{}/{}".format(ASSET_PATH, MPC_NAME))
    if collection is None:
        raise RuntimeError("Run create_assets() first - the parameter collection is missing.")

    assets = unreal.EditorAssetLibrary.list_assets(content_path, recursive=True)
    changed = 0

    for asset_path in assets:
        data = unreal.EditorAssetLibrary.find_asset_data(asset_path)
        if data.asset_class_path.asset_name != "Material":
            continue

        if dry_run:
            _log("would inject into {}".format(asset_path))
            changed += 1
            continue

        if inject_into_material(data.get_asset(), collection):
            unreal.EditorAssetLibrary.save_asset(asset_path)
            changed += 1

    _log("{} {} material(s) under {}."
         .format("would modify" if dry_run else "modified", changed, content_path))
    return changed
