# Building Visualization

Scalable clipping, isolation and ghosting for large digital-twin buildings in
Unreal Engine 5.8.

Drag a box through a wall and the wall opens up, revealing the pipes inside it.
Select floor 8 and the rest of the building fades to a ghost. Nothing is
swapped, duplicated, or iterated over — the GPU does the spatial test per pixel,
and the CPU cost is proportional to the number of clip volumes, not the number
of objects in the building.

> **Status: not yet cutting.** The CPU half is verified end to end — the
> collection reads back exactly the volume's world centre, half-extent and mode
> (`ClipCenter_0 = (1205, 1260, 288, 1.0)`, `ClipExtent_0 = (200, 200, 200, 0)`)
> — and every clippable material is Masked with the generated node group on its
> Opacity Mask pin. Geometry inside the volume is nonetheless still drawn.
> A `BV.Clip 1` / `BV.Clip 0` screenshot pair differs by 0.15% of pixels, which
> is noise.
>
> **Do not trust a screenshot pair by eye.** The pair above this line used to be
> presented as a working before/after; diffing it programmatically showed the
> two images were the same, and the console command that was supposed to toggle
> the feature had failed silently with "No world available". Both of those are
> fixed; the remaining defect is real and is in the material graph or in how it
> reaches the pixel.
>
> Clip volumes are also **axis-aligned** — rotation is published by the C++ but
> not consumed by the graph, see [Axis-aligned, for now](#axis-aligned-for-now).
> Capped cross-sections come after this works — see [Roadmap](#roadmap).

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
                                 │  writes 27 vectors, only when something changed
                                 ▼
                    MPC_BuildingClip  (Material Parameter Collection)
                                 │
                                 │  read by
                                 ▼
              ~120 native material nodes, built by the injector
                                 │
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
regardless — so the only cost is nodes at injection time.

The real trade: the collection layout is now duplicated into every injected
material, so changing `MaxClipVolumes` or the parameter names means re-running
the injector rather than editing one asset. `_remove_existing_clip_nodes` makes
that re-run safe by sweeping the previous generation first.

### Why there is no Custom node either

The function was then replaced by a single `Custom` node calling into
`BuildingClipping.ush`, with the collection parameters wired in as twenty-one
named inputs. One node per material, the maths in a diffable file. That
compiled cleanly — and cut nothing.

Everything around it verified correct, individually:

| Checked | Result |
|---|---|
| `BV.Status` collection layout | valid |
| Values reaching the collection | `ClipParams_0 = (200, 200, 200, 2.0)`, rows consistent with the box |
| Collection parameters with a missing name | 0 |
| Material blend mode / Opacity Mask pin | `MASKED`, connected to the Custom node |
| Masking itself, in an isolated material | works |

A Custom node with twenty-one inputs has exactly **one** thing that cannot be
inspected from outside: whether each named input actually bound to the argument
the generated HLSL expects. Everything else had been eliminated. Rather than
keep testing the one opaque link, it was removed.

So the graph is now built from **native material nodes only** — no include
path, no HLSL string, no input binding. Around 120 nodes per material, every
one of them visible in the material editor and every one of them something the
material editor itself would have produced. When it misbehaves, you can look
at it.

The lesson generalises past this plugin: when a chain is long and every link
but one has been verified, stop verifying and delete the link you cannot see
into.

---

## How the clipping works

### Axis-aligned, for now

Each volume is published twice: as a **world-to-local matrix** and as a
**world-space centre and half-extent**. The matrix is the correct form — a
world position multiplied by it lands in the box's local space, where an
oriented-box test collapses to `abs(local) <= extents`, and rotation and
non-uniform scale come along for free inside the matrix.

The graph reads the centre/extent pair instead, because of what each form costs
in **nodes**:

| Form | In HLSL | In native material nodes |
|---|---|---|
| World-to-local matrix | one `mul` | 3 masks, 3 dot products, an Append — before the test starts |
| Centre + half-extent | one subtract | Subtract → Abs → Subtract |

Since the whole reason the graph is built from native nodes is that it has to
be inspectable when it goes wrong, the cheaper form wins. The cost is that a
**rotated clip volume cuts its bounding box**, not the rotated box.

The matrix rows are still written every frame, and
[`Shaders/Private/BuildingClipping.ush`](Shaders/Private/BuildingClipping.ush)
still holds the oriented implementation, so restoring rotation is a matter of
consuming what is already published — not rederiving it.

### The box test

```
q       = abs(worldPos - centre) - extent      // negative on every axis = inside
outside = saturate(max(q.x, q.y, q.z) / feather)
```

`outside` is 0 well inside the box, 1 well outside it, and ramps across
`feather` world units at the surface. Then the mode picks which way round that
means:

| Mode | Keep mask |
|---|---|
| Cut Inside | `outside` |
| Cut Outside | `1 - outside` |

**Disabled costs nothing and needs no branch.** The C++ never packs a disabled
volume, so an unused slot arrives as centre `(0,0,0)` and extent `(0,0,0)` — a
zero-sized box that every pixel is outside of. `outside` is therefore 1 and the
slot keeps everything, through exactly the same three nodes.

A material graph has no component-wise reduce, so `max(q.x, q.y, q.z)` is three
ComponentMasks and two Max nodes. That is the honest price of native nodes over
one line of HLSL.

### Why the max, rather than the full SDF

`max(q.x, q.y, q.z)` is the exact signed distance **inside** the box and the
distance to the nearest infinite slab outside it. The true metric distance
needs a second term:

```hlsl
d = length(max(q, 0)) + min(max(q.x, q.y, q.z), 0)
```

which is what rounds the corners correctly instead of reporting distance to a
slab. It matters when the distance is used as a distance — for a rounded cut,
or a falloff that must look right at a corner. Here it is only thresholded a
couple of centimetres either side of the surface, where the two agree, so the
second term would buy a visibly identical result for a `length`, a `max` and a
`min` in **every clippable material**. The full form is in the `.ush` for when
capping needs it.

### Feathering is not cosmetic

The clip test is a step function being point-sampled once per pixel. With a hard
edge the boundary lands wherever pixel centres happen to fall and *crawls* as
the box moves. Dividing by a few units and saturating turns it into a ramp
instead. It costs one `saturate` on a value already computed.

The divisor is guarded with `max(feather, 0.01)`. The collection defaults to all
zeros, and there is a window on load between a material compiling and the
subsystem's first push where the feather really is 0 — unguarded, that is a
divide by zero whose NaN reaches the opacity mask and blanks the whole building.

### Combining volumes: intersect the cuts, union the isolations

The two modes compose differently, and collapsing them into one accumulator
produces a tool that is subtly wrong:

- **Cut Inside** volumes are *subtractive*. Each removes its own contents, so a
  pixel must survive all of them → **min**. Two boxes each carving a hole carve
  both holes.
- **Cut Outside** volumes are *restrictive*. Each keeps only its contents, so a
  pixel inside **any** of them survives → **max**. Taking the min here would
  show the empty *intersection* of two isolated systems — isolate the pipes and
  the HVAC and you would see nothing at all.

So each volume emits three terms rather than one keep value, and they feed two
accumulators:

```
cut_i     = (mode == CutOutside) ? 1 : outside_i
isolate_i = (mode == CutOutside) ? 1 - outside_i : 0
flag_i    = (mode == CutOutside) ? 1 : 0

clip = min(min cut_i, lerp(1, max isolate_i, max flag_i))
```

The flag is not redundant. Without an isolation volume present, `max isolate_i`
is 0 everywhere, which would erase the building; the flag is what distinguishes
*"nothing is isolated"* from *"the isolation keeps nothing here"*.

Floor focus and ghosting compose on top:

```
focus = max(slabKeep, ghostStipple * ghostEnabled)
final = min(clip, focus)
```

`max` for the ghost, not multiply: ghosting must **add** survivors back to the
out-of-focus region, not remove more. And it is deliberately applied only to the
focus term — a clip box is an explicit "remove this", and having it fade to a
haze instead of cutting would make the inspection cube useless.

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
| CPU, per frame, box moving | 27 vector writes, independent of level size |
| GPU, per clippable pixel | 4 × (subtract, abs, subtract, 2 max, divide, saturate) + slab + dither |
| Per mesh | **nothing** |
| Per material | ~120 nodes, added once by script |

That node count reads alarming and is not: they collapse to a few dozen ALU
instructions, there is no texture fetch and no branch anywhere in the group,
and the node count is an **authoring-time** number, not a runtime one.

`MaxClipVolumes` is 4 and is a **compile-time constant**. A material graph has
no loops, so volumes are unrolled: every extra slot costs six more collection
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

### Verifying it, in order

Do these in order. Each one rules out a whole class of cause, and the earlier
ones are the ones that actually go wrong.

**1. Does the geometry you are looking at use an injected material?**
This is the single most common reason for "nothing happens", and it is not
guessable — in our own test level, most of the walls used the template's
`MI_PrototypeGrid_Gray` rather than the wall material that had been injected.
Select the mesh, look at its material, and confirm that material (or an
instance's **parent**) is in the list the injector printed.

**2. `BV.Status`.** It prints, in order: the collection and whether its layout
validated, the master switch, and then every registered volume with the centre
and extent it is actually sending. A box in the right place at the wrong size
and a box at the right size in the wrong place both present as "nothing
happened"; these numbers separate them.

**3. Drag the box.** Select the Clipping Volume actor in the outliner and move
it with the gizmo. The cut follows during the drag, not on release — that is
`PostEditMove` firing with `bFinished == false`. If the numbers in `BV.Status`
change as you drag but the geometry does not, the problem is in the material,
not in the C++.

**4. Prove the mask reaches the pixel.** Open one injected material, find the
final `Min` node feeding Opacity Mask, and temporarily connect a `Constant 0`
to Opacity Mask instead. Everything using that material should vanish. If it
does not, the material is not evaluating its opacity mask at all — check the
blend mode, and see
[the blend mode section](#blend-mode-must-become-masked--and-must-be-set-first).

**5. Then the features.** `BV.SelectFloor Floor.02` should leave one storey
standing; `BV.Ghost 1` should bring the rest back as a stipple; `BV.Reset`
should restore everything.

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

So the ghost alpha becomes a **screen-space stipple**: keep a pixel when its
noise value falls under the ghost opacity, so 15% opacity means 15% of pixels
survive and the wall reads as a haze. **It costs nothing extra**, because the
injector already had to make every clippable material Masked in order to clip
correctly in the depth prepass. Ghost mode rides on machinery the clip feature
was forced to build anyway.

The noise is interleaved gradient noise, open-coded in four nodes:

```
noise = frac(52.9829189 * frac(dot(pixelPosition, float2(0.06711056, 0.00583715))))
keep  = ceil(saturate(ghostOpacity - noise))
```

Interleaved gradient noise rather than a texture lookup or a Bayer matrix: no
sampler, and its high-frequency distribution is what TAA resolves into an even
tone instead of a crawling pattern. It is driven from **PixelPosition**, not
ViewportUV — the dither has to be locked to the pixel grid or it swims across
the surface as the camera moves, which is more distracting than the ghosting.

The stipple is applied only to the focus term, never to the clip mask.
Stippling the cut would make its edge crawl instead of staying crisp.

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
missing the clip nodes are four unrelated problems with one symptom.
`BV.Status` distinguishes them in one line each, and now also lists every
registered volume with the centre and extent it is sending — so a box in the
wrong place and a box of the wrong size stop looking like the same failure.

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

**4. Did you change the collection layout?** `MaxClipVolumes`,
`ClipParameterPrefix`, or any parameter name. Three things must agree, and
nothing checks two of them for you:

```python
import setup_building_visualization as bv
bv.create_assets()                       # rebuild the MPC
bv.inject_into_path("/Game/Building")    # rebuild every graph that reads it
```

`BV.Status` reports a mismatched layout as invalid on the C++ side, but a
material still carrying the *old* parameter names compiles perfectly and reads
zeros. Re-inject after any layout change.

**5. If an edit is accepted but has no effect,** test a second edit through the
same path that you *know* should be visible (base colour is ideal). If that one
shows and yours does not, the problem is a material property, not your graph.
See [the blend mode section](#blend-mode-must-become-masked--and-must-be-set-first).

**6. Driving these from a script?** The commands take their world from the
console context, and a dispatch with no context object — Python's
`execute_console_command(None, ...)`, an `-ExecCmds` entry at startup — supplies
none. They now fall back to the PIE world, then the editor world, so automation
works; but **read the state back rather than trusting the command**
(`IsClippingEnabled()`), because a command that silently did nothing and a
feature that silently did nothing look the same in a screenshot. That mistake
produced a convincing-looking before/after pair here that turned out to be two
copies of the same image.

**7. Nothing is disabled above you.** Master switch (`BV.Clip 1`), the volume's
own `Clip Mode` (Disabled is a valid state and looks exactly like broken), and
`BV.Status`'s per-volume list, which prints the centre and extent each volume is
actually sending. A box in the right place with the wrong extent, and a box with
the right extent in the wrong place, both present as "nothing happened".

### The editor hangs on startup after a script deleted an asset

Symptom: the editor reaches "Engine is initialized", registers its plugins, and
then stops responding — no crash, no error, the window may never finish
drawing. A prompt saying *"1 asset editor was open when the editor was last
closed. Would you like to re-open it?"* may flash up.

Cause: the editor restores whichever asset editors were open last session, and
one of those assets **no longer exists** — typically because a script deleted
or replaced it. The entry lives in:

```
Saved/Config/WindowsEditor/EditorPerProjectUserSettings.ini
  [AssetEditorToolkitTabLocation]
  /Game/Path/To/DeletedAsset.DeletedAsset=1
```

Fix: close the editor, delete that line, relaunch. **Edit this file only while
the editor is closed** — it is rewritten on shutdown, so changes made while it
is running are lost (and that is also how MCP autostart settings get silently
dropped).

This is a real hazard for *this* plugin specifically, because the injector
deletes and recreates assets. If you had one open when it ran, the next launch
hangs.

## Roadmap

- [x] Movable/scalable clip volume actor
- [x] Global shader parameters via MPC
- [x] World-space oriented-box clipping, multi-volume CSG
- [x] Scripted bulk injection into existing materials
- [x] Manager API — `SelectFloor` / `SelectRoom` / `SelectSystem` / `ResetVisualization`
- [x] Semantic selection via actor tags (`Floor.08`, `System.Pipe`, …)
- [x] Ghost mode via dithered opacity
- [x] Combined floor isolation + clipping + ghosting
- [x] Native-node injection — no material function, no Custom node
- [ ] **Make the injected mask actually reach the pixel** — parameters and graph
      verified, geometry still drawn
- [ ] Rotated clip volumes (matrix already published; graph reads centre/extent)
- [ ] Capped cross-sections (solid cut faces)

## Licence

Copyright Matin. All rights reserved.
