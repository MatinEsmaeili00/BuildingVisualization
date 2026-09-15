# Building Visualization

Scalable clipping, isolation and ghosting for large digital-twin buildings in
Unreal Engine 5.8.

Drag a box through a wall and the wall opens up, revealing the pipes inside it.
Select floor 8 and the rest of the building fades to a ghost. Nothing is
swapped, duplicated, or iterated over — the GPU does the spatial test per pixel,
and the CPU cost is proportional to the number of clip volumes, not the number
of objects in the building.

> **Status: in development.** Increment 1 (clip volumes, global parameters,
> world-space box clipping) is implemented and verified to compile. Selection,
> ghosting and capped cross-sections are in progress — see [Roadmap](#roadmap).

---

## The problem this solves

A digital twin of a real building is hundreds of thousands of components across
dozens of floors. The obvious implementations of "cut the building open" all
fail at that scale:

| Approach | Why it fails |
|---|---|
| Swap materials on affected meshes | Thousands of asset touches per frame; hitches; unmanageable |
| Hide whole actors | Only cuts at object granularity — you cannot open *part* of a wall |
| Per-mesh dynamic material instances | Every object needs its own parameter copy, updated every frame |
| Boolean mesh operations | Nowhere near real-time at this scale |

The cost of all of these grows with the size of the building, which is exactly
backwards: a bigger building is precisely when you need the tool most.

## The approach

One global uniform buffer, read by every clippable material, written once per
frame by a single subsystem.

```
AClippingVolumeActor ──┐
AClippingVolumeActor ──┼──► UBuildingVisualizationSubsystem
AClippingVolumeActor ──┘         │
                                 │  writes 17 vectors, only when something changed
                                 ▼
                    MPC_BuildingClip  (Material Parameter Collection)
                                 │
                                 │  read by
                                 ▼
                       MF_BuildingClip  (material function)
                                 │
                                 │  injected once per material, by script
                                 ▼
                  every clippable material → Opacity Mask
```

**The CPU never looks at the building.** It packs each volume's transform into
a matrix and writes it to the collection. Whether the level contains one mesh or
four hundred thousand, the work is identical.

### "But that means editing every material"

No — and this is the distinction the whole design rests on.

**Meshes and materials are not the same count.** A 400,000-component building
typically shares a few dozen materials. You are not editing thousands of
assets; you are editing perhaps thirty, and
[`Content/Python/setup_building_visualization.py`](Content/Python/setup_building_visualization.py)
does it in one call:

```python
import setup_building_visualization as bv
bv.create_assets()
bv.inject_into_path("/Game/Building", dry_run=True)   # check the list
bv.inject_into_path("/Game/Building")
```

Only **Material** assets are touched, never Material Instances — an instance
inherits its parent's graph, so injecting into the parent covers every instance
for free. That is why a building sharing one master material across thousands
of meshes costs exactly one injection.

A per-pixel cut has to execute in the material shader — `clip()` runs nowhere
else, and no engine switch injects it globally. Given that constraint, the goal
is to make the per-material cost *one scripted step*, which is what this does.

### Why there is no material function

The natural design is one `MF_BuildingClip` material function that every
material calls — one place that knows the collection layout, one node to
inject. That was built, and it does not work.

**Connections made inside a material function through the Python API do not
persist.** `connect_material_expressions` returns `True`,
`update_material_function` reports no error, and the saved graph has every node
present with its wires missing. The material then compiles cleanly against
unconnected pins sitting at their defaults, so the effect silently does nothing
with no error in any log. Verified by opening the generated function and
looking at it.

Material-**level** graph building through the same API works reliably, so the
whole node group is built directly in each material instead. The compiled
shader is identical either way — a material function is inlined at compile time
regardless — so the only cost is ~21 nodes per material at injection time.

The real trade: the collection layout is now duplicated into every injected
material, so changing `MaxClipVolumes` or the parameter names means re-running
the injector rather than editing one asset. `_remove_existing_clip_nodes` makes
that re-run safe by sweeping the previous generation first.

---

## How the clipping works

### The volume sends a matrix, not a centre and a rotation

`AClippingVolumeActor::BuildGPUData` packs the box's **inverse world
transform**. In the shader, a world position multiplied by that matrix lands in
the box's local space, where the oriented-box test collapses to:

```hlsl
abs(local) <= extents
```

Rotation and non-uniform scale are already inside the matrix, so they cost
nothing extra — no quaternion, no per-axis scale, no branching on shape. This is
why the actor can be rotated and scaled freely with the standard gizmo and the
shader never learns about it.

### The box SDF, and why it is two terms

```hlsl
q = abs(local) - extents
d = length(max(q, 0)) + min(max(q.x, q.y, q.z), 0)
```

The two terms look bolted together and are not — exactly one is active at a
time, which is what makes this branch-free:

- **Outside**, at least one component of `q` is positive. `max(q, 0)` zeroes the
  axes already within the box, leaving the true Euclidean distance to the
  nearest face, edge or corner — which is what rounds corners correctly instead
  of reporting distance to an infinite slab. The second term is zero here.
- **Inside**, every component of `q` is negative, so the first term is zero and
  the largest (least-negative) component is the distance to the nearest face,
  correctly signed.

Their sum is the signed distance everywhere. Being a true metric distance in
world units on *both* sides is what lets one feather width in centimetres behave
identically on every volume regardless of its scale.

### Feathering is not cosmetic

The clip test is a step function being point-sampled once per pixel. With a hard
edge the boundary lands wherever pixel centres happen to fall and *crawls* as
the box moves. Dividing the signed distance by a few units and saturating turns
it into a ramp the opacity mask can dither against. It costs one `saturate` on a
distance already computed.

### Combining volumes: intersect the cuts, union the isolations

Two modes compose differently, and getting it wrong produces a confusing tool:

- **Cut Inside** volumes are *subtractive*. Each removes its own contents, so a
  pixel must survive all of them → **minimum** (soft boolean AND).
- **Cut Outside** volumes are *restrictive*. Each keeps only its contents, so a
  pixel inside **any** of them survives → **maximum** (soft boolean OR).
  Taking the minimum here would show the empty intersection of two isolated
  rooms rather than both rooms.

Intersect the keeps, union the isolates, intersect those two results. Standard
CSG, and it generalises: a new mode just picks which accumulator it feeds.

### Blend Mode must become Masked — and must be set *first*

The injector sets `BLEND_MASKED`, and this matters twice over.

**Why Masked at all.** An **Opaque** material still renders in the depth
prepass, and **the prepass does not evaluate the opacity mask**. Clipped pixels
would write depth, then be discarded in the base pass — so the wall you cut away
would still occlude the pipes behind it. You would see through the wall to
*nothing*. Masked makes the prepass run the same test, so depth and colour agree.

**Why the order matters.** This one cost hours, so it is worth stating plainly:

> Connecting `MP_OPACITY_MASK` on a material that is still **Opaque** is
> accepted and then **ignored**.

The connection is stored. `get_material_property_input_node` reports it
correctly. The material compiles without a warning. And the mask is never
evaluated, because an Opaque material has no opacity mask to evaluate. Setting
Masked *afterwards* does **not** retroactively make the stored connection live —
it has to be Masked at the moment the connection is made.

Every symptom this produces looks like something else: clipping that silently
stops, ghosting that never starts, correct parameter values feeding a node that
is visibly connected and does nothing.

**How it was found**, because the technique generalises: the Custom node was
wired to return a literal `0.0`, which should have erased every surface — the
building rendered perfectly intact. Then a constant was connected to Base Colour
on the *same* material, and it turned red instantly. Same material, same
`recompile_material` call, one edit visible and the other inert. That ruled out
the graph, the shader, the parameter collection and the recompile in a single
step, and left only the property.

When an edit is accepted but has no effect, stop investigating the thing being
edited and find a second edit through the same path that *does* have an effect.
The difference between them is the bug.

---

## Cost

| | Cost |
|---|---|
| CPU, per frame, nothing moving | zero — the push is skipped entirely |
| CPU, per frame, box moving | 17 vector writes, independent of level size |
| GPU, per clippable pixel | 4 × (one 3×4 transform + box SDF + saturate) |
| Per mesh | **nothing** |
| Per material | one function call node, added once by script |

`MaxClipVolumes` is 4 and is a **compile-time constant**. A material graph has
no loops, so volumes are unrolled: every extra slot costs four more collection
vectors and fixed shader work in *every* clippable material, used or not. Four
covers an inspection box plus a couple of section cuts. Needing dozens is not a
bigger number here — it is a structured buffer and a custom vertex factory,
which is a different and much larger piece of work.

---

## Setup

1. Enable the plugin (it lives in `Plugins/`, so it is picked up automatically).
2. Build the project — the plugin has a C++ runtime module.
3. In the editor's Python console:
   ```python
   import setup_building_visualization as bv
   bv.create_assets()
   ```
4. Point **Project Settings → Plugins → Building Visualization →
   Parameter Collection** at `/Game/BuildingVisualization/MPC_BuildingClip`.
5. Inject into your building's materials:
   ```python
   bv.inject_into_path("/Game/Building")
   ```
6. Drop a **Clipping Volume** actor in the level and drag it into a wall.

It works in the editor viewport, not just in PIE — the subsystem ticks in editor
worlds deliberately, because dragging the box through the building while
building the level is the primary way this gets used.

---

## Selection and ghosting

```cpp
Subsystem->SelectFloor(TEXT("Floor.08"));   // focus one storey
Subsystem->EnableGhostMode(true);           // fade everything else
Subsystem->SelectRoom(TEXT("Room.802"));    // isolate a room
Subsystem->ClearSelection();
```

Selection resolves tags **once, when it changes** — never on tick. Iterating
actors the moment a user clicks "floor 8" is fine; doing it at 60 Hz is not,
and nothing in `Tick` does anything like it.

### Floors are slabs; rooms are not

The two take deliberately different routes:

- **`SelectFloor`** resolves the tag to a Z range and publishes it as a slab
  every material tests against. A storey *is* a slab in Z, so this needs **no
  per-object data at all** — one vector, and it works identically on a building
  of any size. Only the Z range is kept: the horizontal extent of a storey is
  the whole footprint, so testing against it would only break an L-shaped plan.
- **`SelectRoom` / `SelectSystem`** cannot work that way — a room is not a
  world-space predicate. These derive an AABB from the tagged actors and drive
  an internal **Cut Outside** volume with it, reusing the clip machinery rather
  than inventing a second mechanism. Note this consumes one of the
  `MaxClipVolumes` slots, and it is inserted *first* so that user-placed boxes
  are the ones dropped if the level exceeds the slot count.

### Plain actor tags, not Gameplay Tags

Gameplay Tags must exist in a central registry before they can be applied,
which would mean registering several thousand (`Floor.01`…`Floor.40`, every
room number) before a single actor could be tagged. Actor tags are free-form,
so an importer can write whatever the BIM data says with no registration step —
and hierarchy still works by prefix, since `"Floor."` matches `"Floor.08"`.

### Ghosting is dithered, not tinted

Out-of-focus geometry fades to `GhostOpacity` rather than vanishing, so the
building keeps its context — you can still read where the isolated floor sits
inside the whole structure.

**A post-process tint cannot make an opaque wall see-through.** Whatever is
behind it was never rendered; those pixels do not exist to blend with. Making
materials Translucent would work but changes sorting, gives up much of the
deferred pipeline, and cannot be toggled per frame.

So the ghost alpha is fed through the engine's `DitherTemporalAA`, which turns
coverage into a jitter-aware stipple that TAA resolves into apparent
translucency — the same mechanism as UE's own LOD dithering. **It costs
nothing**, because the injector already had to make every clippable material
Masked in order to clip correctly in the depth prepass. Ghost mode rides on
machinery the clip feature was forced to build anyway.

The clip mask and ghost alpha are returned separately (`float2`) rather than
pre-multiplied, because only the ghost half should be dithered — stippling the
clip mask would make the cut edge crawl instead of staying crisp.

## Console commands

Everything is drivable from the console, in the **editor viewport as well as
PIE** — no recompile, no Blueprint wiring, no play session needed.

| Command | Does |
|---|---|
| `BV.SelectFloor Floor.03` | Focus one storey |
| `BV.SelectRoom Room.301` | Isolate a room |
| `BV.SelectSystem System.Pipe` | Isolate a system |
| `BV.ClearSelection` | Clear selection, leave placed clip volumes alone |
| `BV.Ghost 1` | Fade out-of-focus geometry |
| `BV.GhostOpacity 0.15` | How faint the ghost is |
| `BV.Clip 0` | Master clipping switch |
| `BV.Reset` | Reset everything |
| `BV.Status` | Print all current state |
| `BV.ListTags Room.` | Every actor tag in the world, with counts |
| `BV.DescribeTag Room.301` | Every actor with a tag: **class** and bounds |

The three inspection commands exist because **every failure this system can
have looks identical from the viewport: nothing happens.** A tag typo, geometry
imported without metadata, an unset parameter collection, and a material
missing the clip function are four unrelated problems with one symptom.
`BV.Status` distinguishes them in one line each.

`BV.DescribeTag` is the one that answers *"is my room isolation using the
trigger volume or the walls?"* — both produce a box, and a wrong one just looks
like a badly sized room, so the **class** column is the actual answer.
`SelectRoom` also warns on its own if a tag is carried by more than one actor
class, since that union is silently larger than the room.

## Troubleshooting

Every failure in this system presents identically — **nothing happens** — so
the order below is the order that separates the causes fastest.

**1. `BV.Status` first.** It prints the parameter collection and whether its
layout validated *before* anything else, because if that is wrong every other
setting reads as correct while having no effect.

**2. `BV.ListTags` / `BV.DescribeTag`.** Confirms the tag is spelled as you
think, and — via the class column — which actors actually defined the bounds.

**3. Check which materials your geometry really uses.** The most common cause
of "it does nothing" is injecting into the wrong material. In our own test
setup, 16 of 19 tagged walls used the template's `MI_PrototypeGrid_Gray` while
only 3 used the wall material we had injected. Enumerate it rather than
assuming:

```python
for a in unreal.get_editor_subsystem(unreal.EditorActorSubsystem).get_all_level_actors():
    for c in a.get_components_by_class(unreal.StaticMeshComponent):
        for m in c.get_materials():
            print(a.get_name(), m.get_path_name() if m else None)
```

Remember instances inherit from their parent — inject into the **parent
Material**, not the instance.

**4. Plugin `.ush` edits require an editor restart.** `recompileshaders
changed` does **not** pick up an include pulled in through a material Custom
node. If you have edited the shader and nothing changed, you are still running
the old code — restart before drawing any conclusion.

**5. If an edit is accepted but has no effect,** test a second edit through the
same path that you *know* should be visible (base colour is ideal). If that one
shows and yours does not, the problem is a material property, not your graph.
See [the blend mode section](#blend-mode-must-become-masked--and-must-be-set-first).

## Roadmap

- [x] Movable/scalable clip volume actor
- [x] Global shader parameters via MPC
- [x] World-space oriented-box clipping, multi-volume CSG
- [x] Scripted bulk injection into existing materials
- [x] Manager API — `SelectFloor` / `SelectRoom` / `SelectSystem` / `ResetVisualization`
- [x] Semantic selection via actor tags (`Floor.08`, `System.Pipe`, …)
- [x] Ghost mode via dithered opacity
- [x] Combined floor isolation + clipping + ghosting
- [ ] Capped cross-sections (solid cut faces)

## Licence

Copyright Matin. All rights reserved.
