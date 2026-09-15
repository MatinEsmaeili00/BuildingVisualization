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
# Material-LEVEL graph building through the same API works reliably, so the
# whole node group is built directly in each material instead.
#
#
# WHY THERE IS NO CUSTOM NODE EITHER
# ----------------------------------
# The function was replaced by a single Custom node calling into
# BuildingClipping.ush, with the collection parameters wired in as twenty-one
# named inputs. That compiled cleanly, every parameter verified correct at the
# C++ end, the collection layout verified valid, masking verified working in an
# isolated test material - and it cut nothing.
#
# A Custom node with twenty-one inputs has exactly one thing that cannot be
# inspected from outside: whether each named input actually bound to the
# argument the HLSL expects. Everything else had been checked. Rather than keep
# testing the one opaque link, it was removed.
#
# So the graph below is built from NATIVE material nodes only. No include path,
# no HLSL string, no input binding. Around 120 nodes per material, all of them
# visible in the material editor, all of them things the material editor itself
# would have produced. When it misbehaves now, you can look at it.
#
# The cost of that choice is rotation. A world-to-local matrix in native nodes
# is three dot products and an Append before the box test even begins; a
# centre/extent pair is a Subtract, an Abs and a Subtract. So the volumes are
# treated as AXIS-ALIGNED, and a rotated clip box cuts its bounding box. The
# C++ still publishes the matrix rows for when that is added back - see
# README.md and Shaders/Private/BuildingClipping.ush, which hold the rotated
# maths.
#
# The other cost is that the collection layout is now duplicated into every
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


def _notify_changed(material):
    """
    Makes the material re-derive the state it caches from its own graph.

    Two separate things are needed, and neither is optional.

    1. A change notification. UObject::PostEditChange is not exposed to Python,
       but set_editor_property takes a notify mode, and ALWAYS fires the
       notification even when the value written equals the value already there.
       Writing the blend mode back over itself is a no-op that produces exactly
       that notification.

    2. RepairMaskedBlendMode, in C++. `UMaterial::GetBlendMode()` returns
       **Opaque** for a Masked material whenever `bCanMaskedBeAssumedOpaque` is
       set - an optimisation flag for materials whose mask is trivially 1. It is
       serialized into the asset, invisible in the material editor, absent from
       the Python API, and recomputed by nothing the scripting API can reach.

       A material authored Opaque and later made Masked by script therefore
       keeps the stale flag and is drawn fully opaque, while every check
       available from script insists it is correct: blend mode reads Masked, the
       Opacity Mask pin reads connected, the cached connected-property data
       reads true, and routing the mask to Base Colour shows the right value.
       Only GetBlendMode() disagrees, and only from C++.
    """
    try:
        material.set_editor_property(
            "blend_mode", material.get_editor_property("blend_mode"),
            unreal.PropertyAccessChangeNotifyMode.ALWAYS)
    except Exception as e:
        _log("WARNING: could not notify {} of its change: {}".format(material.get_name(), e))

    try:
        unreal.BuildingVisualizationLibrary.repair_masked_blend_mode(material)
    except Exception as e:
        _log("WARNING: could not repair the blend mode of {}: {}. Is the plugin's "
             "C++ module built?".format(material.get_name(), e))


def _param_names():
    """The full ordered list of vector parameters, matching the C++ naming."""
    names = []
    for i in range(MAX_CLIP_VOLUMES):
        names.append("{}Row0_{}".format(CLIP_PREFIX, i))
        names.append("{}Row1_{}".format(CLIP_PREFIX, i))
        names.append("{}Row2_{}".format(CLIP_PREFIX, i))
        names.append("{}Params_{}".format(CLIP_PREFIX, i))
        # What the material graph actually reads. Row0..Params describe the
        # same box as a matrix and are written but not yet consumed.
        names.append("{}Center_{}".format(CLIP_PREFIX, i))
        names.append("{}Extent_{}".format(CLIP_PREFIX, i))
    names.append(GLOBALS_NAME)
    names.append(FOCUS_SLAB_NAME)
    names.append(GHOST_NAME)
    return names


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

def _remove_existing_clip_nodes(material):
    """
    Deletes every node a previous run of this script created.

    Without this, re-running would leave a hundred orphaned nodes in the graph -
    harmless to the compiled shader, since nothing reads them, but it makes the
    material unreadable after a couple of iterations and grows the asset every
    time. At ~100 nodes per injection this is not optional housekeeping.

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

        # Also sweep up the previous architectures' leftovers: a call to the old
        # MF_BuildingClip, or a call whose function has gone null because that
        # asset was deleted. Both are inert, but a null function call is
        # indistinguishable from a working one at a glance, which makes it
        # exactly the kind of thing that wastes an afternoon.
        if isinstance(expr, unreal.MaterialExpressionMaterialFunctionCall):
            fn = expr.get_editor_property("material_function")
            if fn is None or fn.get_name() == "MF_BuildingClip":
                doomed.append(expr)

        # And the Custom node generation, in case its desc tag was lost - the
        # node uses "description" for its generated HLSL function name, which
        # is easy to confuse with the "desc" the tag lives in.
        elif isinstance(expr, unreal.MaterialExpressionCustom):
            if expr.get_editor_property("description") == "BuildingClip":
                doomed.append(expr)

    for expr in doomed:
        _mel.delete_material_expression(material, expr)
    return len(doomed)


# ---------------------------------------------------------------------------
# Native node graph construction
# ---------------------------------------------------------------------------
#
# Every node built below is tagged and positioned. The positions matter: a
# hundred untagged nodes piled at the origin is not a graph anyone can debug,
# and the whole reason this generation exists is that the previous one could
# not be inspected.

class _Builder(object):
    """Creates tagged, positioned nodes in one material."""

    def __init__(self, material, collection):
        self.material = material
        self.collection = collection
        self.count = 0

    def node(self, cls, x, y):
        expr = _mel.create_material_expression(self.material, cls, x, y)
        expr.set_editor_property("desc", CLIP_NODE_TAG)
        self.count += 1
        return expr

    def param(self, name, x, y):
        expr = self.node(unreal.MaterialExpressionCollectionParameter, x, y)
        expr.set_editor_property("collection", self.collection)
        expr.set_editor_property("parameter_name", name)
        return expr

    def mask(self, src, channels, x, y, src_out=""):
        """ComponentMask. channels is a string like "rgb", "b" or "a"."""
        expr = self.node(unreal.MaterialExpressionComponentMask, x, y)
        for channel in "rgba":
            expr.set_editor_property(channel, channel in channels)
        _connect(src, src_out, expr, "", "mask .{}".format(channels))
        return expr

    def unary(self, cls, src, x, y, what):
        expr = self.node(cls, x, y)
        _connect(src, "", expr, "", what)
        return expr

    def binary(self, cls, a, b, x, y, what):
        expr = self.node(cls, x, y)
        _connect(a, "", expr, "A", what + " (A)")
        _connect(b, "", expr, "B", what + " (B)")
        return expr

    def binary_const(self, cls, a, const_b, x, y, what):
        """Binary op with B left as a literal. Saves a Constant node each time."""
        expr = self.node(cls, x, y)
        _connect(a, "", expr, "A", what + " (A)")
        expr.set_editor_property("const_b", const_b)
        return expr

    def constant(self, value, x, y):
        expr = self.node(unreal.MaterialExpressionConstant, x, y)
        expr.set_editor_property("r", value)
        return expr

    def select_on_mode(self, mode, when_cut_outside, otherwise, x, y, what):
        """
        If(mode >= 1.5, when_cut_outside, otherwise).

        1.5 rather than 2 so no float comparison lands on the boundary, and
        A == B is deliberately left unconnected - with no equals pin the
        translator emits a plain two-way select, which is what is wanted.
        """
        expr = self.node(unreal.MaterialExpressionIf, x, y)
        expr.set_editor_property("const_b", 1.5)
        _connect(mode, "", expr, "A", what + " (mode)")
        _connect(when_cut_outside, "", expr, "A > B", what + " (cut outside)")
        _connect(otherwise, "", expr, "A < B", what + " (other modes)")
        return expr

    def reduce(self, cls, nodes, x, y, what):
        """Pairwise tree rather than a chain: shorter, and it reads as a tree."""
        row = 0
        while len(nodes) > 1:
            merged = []
            for i in range(0, len(nodes) - 1, 2):
                merged.append(self.binary(cls, nodes[i], nodes[i + 1],
                                          x + row * 200, y + i * 300, what))
            if len(nodes) % 2:
                merged.append(nodes[-1])
            nodes = merged
            row += 1
        return nodes[0]


def _build_volume_keep(b, index, world_pos, feather, one, zero, x, y):
    """
    Evaluates one volume into the three terms the combiner needs.

    The maths is the standard axis-aligned box test written out as nodes:

        q       = abs(worldPos - centre) - extent     // negative on every axis = inside
        outside = saturate(max(q.x, q.y, q.z) / feather)

    outside is 0 well inside the box, 1 well outside it, and ramps across
    `feather` world units at the surface. Dividing by the feather rather than
    comparing against zero is what stops the cut edge crawling with pixel-sized
    stair steps when a wall is viewed at a glancing angle.

    The mode then picks which way round that means, and - this is the part that
    is not obvious - the two modes must be combined differently, so they go to
    different accumulators rather than to one keep value:

        Cut Inside  is SUBTRACTIVE. Each removes its own contents, so a pixel
                    must survive all of them -> the cut terms combine with min.
        Cut Outside is RESTRICTIVE. Each keeps only its contents, so a pixel
                    inside ANY of them survives -> the isolate terms combine
                    with max.

    Collapsing both into one keep value and taking the min looks like it works,
    because the common case is a single isolation. It fails the moment there are
    two: isolating the pipes and the HVAC would show their empty intersection -
    nothing at all - rather than both systems.

    So this returns (cut, isolate, isolateFlag):

        cut         = 1 for a Cut Outside volume (it subtracts nothing),
                      else `outside`
        isolate     = `1 - outside` for a Cut Outside volume, else 0
        isolateFlag = 1 for a Cut Outside volume, else 0, so the combiner can
                      tell "no isolation requested" from "isolation that keeps
                      nothing here"

    Mode 0 (Disabled) lands in the non-Cut-Outside branch, which is correct for
    free: the C++ never packs a disabled volume, so an unused slot arrives as
    centre 0 / extent 0, a zero-sized box every pixel is outside of. `outside`
    is therefore 1 and the slot cuts nothing. No branch, no special case, no
    wasted parameter.
    """
    cen = b.param("{}Center_{}".format(CLIP_PREFIX, index), x, y)
    ext = b.param("{}Extent_{}".format(CLIP_PREFIX, index), x, y + 80)

    cen_xyz = b.mask(cen, "rgb", x + 200, y)
    ext_xyz = b.mask(ext, "rgb", x + 200, y + 80)
    mode = b.mask(cen, "a", x + 200, y + 160)

    delta = b.binary(unreal.MaterialExpressionSubtract, world_pos, cen_xyz,
                     x + 400, y, "volume {} worldPos - centre".format(index))
    adelta = b.unary(unreal.MaterialExpressionAbs, delta,
                     x + 560, y, "volume {} abs".format(index))
    q = b.binary(unreal.MaterialExpressionSubtract, adelta, ext_xyz,
                 x + 700, y, "volume {} - extent".format(index))

    # A material graph has no component-wise reduce, so the max over the three
    # axes is three masks and two Max nodes. This is the price of native nodes
    # over one line of HLSL, and it is worth paying for a graph that can be
    # read in the editor.
    qx = b.mask(q, "r", x + 860, y - 60)
    qy = b.mask(q, "g", x + 860, y + 20)
    qz = b.mask(q, "b", x + 860, y + 100)
    m1 = b.binary(unreal.MaterialExpressionMax, qx, qy,
                  x + 1020, y - 20, "volume {} max xy".format(index))
    m2 = b.binary(unreal.MaterialExpressionMax, m1, qz,
                  x + 1160, y + 20, "volume {} max xyz".format(index))

    ratio = b.binary(unreal.MaterialExpressionDivide, m2, feather,
                     x + 1300, y, "volume {} / feather".format(index))
    outside = b.unary(unreal.MaterialExpressionSaturate, ratio,
                      x + 1440, y, "volume {} saturate".format(index))
    inside = b.unary(unreal.MaterialExpressionOneMinus, outside,
                     x + 1440, y + 80, "volume {} invert".format(index))

    cut = b.select_on_mode(mode, one, outside, x + 1600, y - 80,
                           "volume {} cut term".format(index))
    isolate = b.select_on_mode(mode, inside, zero, x + 1600, y + 40,
                               "volume {} isolate term".format(index))
    flag = b.select_on_mode(mode, one, zero, x + 1600, y + 160,
                            "volume {} isolate flag".format(index))
    return cut, isolate, flag


def _build_focus_slab(b, world_pos, x, y):
    """
    The SelectFloor term: 1 inside the focused storey, 0 outside it.

    A floor is a Z range, so this is two feathered one-sided tests multiplied
    together - no per-object data and no bounds test, which is why selecting a
    floor costs one vector however large the building is.

    ClipFocusSlab is (minZ, maxZ, active, feather). With nothing selected,
    active is 0 and the Lerp returns a constant 1, so the whole term drops out
    rather than every pixel testing against a garbage Z range.
    """
    slab = b.param(FOCUS_SLAB_NAME, x, y)
    min_z = b.mask(slab, "r", x + 200, y - 80)
    max_z = b.mask(slab, "g", x + 200, y)
    active = b.mask(slab, "b", x + 200, y + 80)
    slab_feather = b.mask(slab, "a", x + 200, y + 160)

    # Guarded for the same reason as the clip feather below: a zero here is a
    # divide by zero, and the NaN propagates to the opacity mask and erases the
    # whole material rather than failing visibly in one place.
    safe_feather = b.binary_const(unreal.MaterialExpressionMax, slab_feather, 0.01,
                                  x + 360, y + 160, "slab feather guard")

    wz = b.mask(world_pos, "b", x + 360, y - 160)

    above = b.binary(unreal.MaterialExpressionSubtract, wz, min_z,
                     x + 520, y - 120, "worldZ - minZ")
    above_r = b.binary(unreal.MaterialExpressionDivide, above, safe_feather,
                       x + 660, y - 120, "above / feather")
    above_s = b.unary(unreal.MaterialExpressionSaturate, above_r,
                      x + 800, y - 120, "above saturate")

    below = b.binary(unreal.MaterialExpressionSubtract, max_z, wz,
                     x + 520, y + 40, "maxZ - worldZ")
    below_r = b.binary(unreal.MaterialExpressionDivide, below, safe_feather,
                       x + 660, y + 40, "below / feather")
    below_s = b.unary(unreal.MaterialExpressionSaturate, below_r,
                      x + 800, y + 40, "below saturate")

    in_slab = b.binary(unreal.MaterialExpressionMultiply, above_s, below_s,
                       x + 940, y - 40, "in slab")

    keep = b.node(unreal.MaterialExpressionLinearInterpolate, x + 1100, y)
    keep.set_editor_property("const_a", 1.0)          # slab off -> keep everything
    _connect(in_slab, "", keep, "B", "slab on -> slab test")
    _connect(active, "", keep, "Alpha", "slab active")
    return keep


def _build_ghost(b, x, y):
    """
    The stipple that lets out-of-focus geometry survive at reduced density.

    The material is Masked, not Translucent, so there is no real alpha to fade
    with - a pixel is drawn or it is not. Ghosting is therefore a screen-space
    dither: keep a pixel when its noise value falls under the ghost opacity, so
    15% opacity means 15% of pixels survive and the wall reads as a haze.

    Interleaved gradient noise rather than a texture lookup or a Bayer matrix:
    four instructions, no sampler, and its high-frequency distribution is what
    TAA resolves into an even tone instead of a crawling pattern.

    Returns a mask that is 0 everywhere when ghosting is off, so the caller can
    max() it into the focus term unconditionally.
    """
    ghost = b.param(GHOST_NAME, x, y)
    opacity = b.mask(ghost, "r", x + 200, y)
    enabled = b.mask(ghost, "g", x + 200, y + 80)

    screen = b.node(unreal.MaterialExpressionScreenPosition, x, y + 200)
    magic = b.node(unreal.MaterialExpressionConstant2Vector, x, y + 300)
    magic.set_editor_property("r", 0.06711056)
    magic.set_editor_property("g", 0.00583715)

    # PixelPosition, not ViewportUV. The dither has to be locked to the pixel
    # grid or it swims across the surface as the camera moves, which is far
    # more distracting than the ghosting itself.
    dot = b.node(unreal.MaterialExpressionDotProduct, x + 200, y + 240)
    _connect(screen, "PixelPosition", dot, "A", "screen pixel position")
    _connect(magic, "", dot, "B", "IGN constants")

    f1 = b.unary(unreal.MaterialExpressionFrac, dot, x + 360, y + 240, "IGN frac 1")
    scaled = b.binary_const(unreal.MaterialExpressionMultiply, f1, 52.9829189,
                            x + 500, y + 240, "IGN scale")
    noise = b.unary(unreal.MaterialExpressionFrac, scaled, x + 640, y + 240, "IGN frac 2")

    # ceil(saturate(opacity - noise)) is 1 where the pixel survives and 0 where
    # it does not. Saturate first, or a negative difference ceils to 0 by luck
    # rather than by construction.
    diff = b.binary(unreal.MaterialExpressionSubtract, opacity, noise,
                    x + 800, y + 120, "opacity - noise")
    clamped = b.unary(unreal.MaterialExpressionSaturate, diff, x + 940, y + 120, "ghost saturate")
    stipple = b.unary(unreal.MaterialExpressionCeil, clamped, x + 1080, y + 120, "ghost step")

    return b.binary(unreal.MaterialExpressionMultiply, stipple, enabled,
                    x + 1220, y + 120, "ghost gate")


def is_protected(material):
    """
    True for assets this script must never modify.

    Engine content lives inside the engine INSTALL, not the project, so editing
    it damages every project on the machine and cannot be undone with source
    control - only by verifying the install in the Epic launcher. It is also
    easy to hit by accident: a sky dome, a default material or an engine
    primitive sitting in a test level is picked up by any "inject into every
    material used here" sweep.
    """
    if material is None:
        return True
    path = material.get_path_name()
    return path.startswith("/Engine/") or path.startswith("/Script/")


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
    if is_protected(material):
        if material is not None:
            _log("SKIPPED {} - engine content is never modified.".format(material.get_path_name()))
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

    b = _Builder(material, collection)

    # Shared inputs. One WorldPosition node feeds every volume and the slab -
    # the translator would collapse duplicates anyway, but one node is also one
    # thing to check when the cut lands in the wrong place.
    world_pos = b.node(unreal.MaterialExpressionWorldPosition, -3600, -900)
    globals_p = b.param(GLOBALS_NAME, -3600, -800)
    feather_raw = b.mask(globals_p, "g", -3400, -800)

    # Guarded against zero. The collection defaults to all zeros, and there is
    # a window on load between a material compiling and the subsystem's first
    # push where the feather really is 0 - unguarded, that is a divide by zero
    # whose NaN reaches the opacity mask and blanks the entire building.
    feather = b.binary_const(unreal.MaterialExpressionMax, feather_raw, 0.01,
                             -3240, -800, "clip feather guard")

    # Two literals, shared by every volume's mode select. A Constant per volume
    # would be twelve more nodes saying the same thing.
    one = b.constant(1.0, -3400, -650)
    zero = b.constant(0.0, -3400, -580)

    terms = [_build_volume_keep(b, i, world_pos, feather, one, zero, -3000, i * 420)
             for i in range(MAX_CLIP_VOLUMES)]
    cuts = [t[0] for t in terms]
    isolates = [t[1] for t in terms]
    flags = [t[2] for t in terms]

    # Cuts intersect on keep (a pixel must survive every subtractive box);
    # isolations union (a pixel inside ANY restrictive box survives). Standard
    # CSG, and the asymmetry is the whole reason the two are tracked apart.
    cut_keep = b.reduce(unreal.MaterialExpressionMin, cuts, -1100, 0, "combine cuts")
    isolate_keep = b.reduce(unreal.MaterialExpressionMax, isolates, -1100, 1200, "combine isolations")
    any_isolate = b.reduce(unreal.MaterialExpressionMax, flags, -1100, 2400, "any isolation")

    # With no isolation volume present, isolate_keep is 0 everywhere, which
    # would erase the building. The flag is what distinguishes "nothing is
    # isolated" from "the isolation keeps nothing here".
    isolate_term = b.node(unreal.MaterialExpressionLinearInterpolate, -700, 1800)
    isolate_term.set_editor_property("const_a", 1.0)
    _connect(isolate_keep, "", isolate_term, "B", "isolation keep")
    _connect(any_isolate, "", isolate_term, "Alpha", "isolation present")

    clip_keep = b.binary(unreal.MaterialExpressionMin, cut_keep, isolate_term,
                         -500, 900, "cuts and isolations")

    slab_keep = _build_focus_slab(b, world_pos, -3000, 1900)
    ghost_floor = _build_ghost(b, -3000, 2500)

    # Max, not Multiply: ghosting must ADD survivors back to the out-of-focus
    # region, not remove more. It is deliberately applied only to the focus
    # term - a clip box is an explicit "remove this", and having it fade to a
    # haze instead of cutting would make the inspection cube useless.
    focus_term = b.binary(unreal.MaterialExpressionMax, slab_keep, ghost_floor,
                          -1400, 2200, "focus or ghost")

    final = b.binary(unreal.MaterialExpressionMin, clip_keep, focus_term,
                     -800, 600, "clip and focus")

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
    material.set_editor_property("blend_mode", unreal.BlendMode.BLEND_MASKED)

    if existing_mask is not None:
        mult = b.binary(unreal.MaterialExpressionMultiply, existing_mask, final,
                        -400, 600, "existing mask * clip")
        _mel.connect_material_property(mult, "", unreal.MaterialProperty.MP_OPACITY_MASK)
    else:
        _mel.connect_material_property(final, "", unreal.MaterialProperty.MP_OPACITY_MASK)

    # PostEditChange, not just recompile.
    #
    # A UMaterial derives cached state from its graph when the editor tells it
    # the asset changed - among it the flags that decide whether the renderer
    # may treat a Masked material as opaque. Nothing in the Python material API
    # fires that: set_editor_property, connect_material_property and
    # recompile_material all leave the derived state exactly as it was when the
    # asset was last edited by hand.
    #
    # On a material that was authored Opaque with no Opacity Mask, that stale
    # state says "this may be drawn as opaque", and it survives being set to
    # Masked, having an expression wired to Opacity Mask, being recompiled,
    # saved, reloaded in a new editor, and even being duplicated. The result is
    # a material that reports Masked, shows a correct mask value when that same
    # node is routed to Base Colour, and never clips a pixel.
    _notify_changed(material)
    _mel.recompile_material(material)

    _log("Injected {} node(s) into {}{}.".format(
        b.count, material.get_name(),
        " (replaced {} previously generated)".format(removed) if removed else ""))
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
    if is_protected(material):
        return False

    removed = _remove_existing_clip_nodes(material)
    if removed == 0:
        return False

    # Opacity mask now points at a deleted node. Put the material back to
    # Opaque so nothing is left reading a dangling input.
    material.set_editor_property("blend_mode", unreal.BlendMode.BLEND_OPAQUE)
    _notify_changed(material)
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


def update_instances_of(material_paths, search_root="/Game"):
    """
    Recompiles every Material Instance descended from a material we injected into.

    Injection changes the parent's blend mode to Masked and adds nodes, and an
    instance does NOT necessarily pick that up on its own: an instance that sets
    any static parameter owns a separate shader map, compiled from the parent as
    it was at the time. Leave it alone and it keeps rendering with the old
    permutation - one that has no opacity mask in it at all.

    The symptom is the single most confusing one this system produces: the
    parent material is Masked, its Opacity Mask pin is connected, the mask value
    is provably correct when routed to Base Colour, and the geometry is still
    solid - because the mesh in the viewport is drawn with the instance's stale
    shader, not the parent's.

    Instances are searched for from `search_root` rather than from the injected
    material's own folder, because an instance is very often authored somewhere
    else entirely (a level's own folder, a variant folder).
    """
    targets = set(material_paths)
    updated = 0

    for asset_path in unreal.EditorAssetLibrary.list_assets(search_root, recursive=True):
        data = unreal.EditorAssetLibrary.find_asset_data(asset_path)
        if str(data.asset_class_path.asset_name) != "MaterialInstanceConstant":
            continue

        instance = data.get_asset()

        # Walk the whole chain: instances of instances are common, and only the
        # root is ever injected into.
        node = instance
        hit = False
        while isinstance(node, unreal.MaterialInstance):
            node = node.get_editor_property("parent")
            if node is not None and node.get_path_name() in targets:
                hit = True
                break
        if not hit:
            continue

        _mel.update_material_instance(instance)
        unreal.EditorAssetLibrary.save_asset(asset_path)
        updated += 1
        _log("Updated instance {}".format(asset_path))

    _log("Refreshed {} material instance(s).".format(updated))
    return updated


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
    injected = []

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
            injected.append(data.get_asset().get_path_name())
            changed += 1

    if not dry_run and injected:
        update_instances_of(injected)

    _log("{} {} material(s) under {}."
         .format("would modify" if dry_run else "modified", changed, content_path))
    return changed
