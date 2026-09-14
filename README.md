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
[`Python/setup_building_visualization.py`](Python/setup_building_visualization.py)
does it in one call:

```python
import setup_building_visualization as bv
bv.create_assets()
bv.inject_into_path("/Game/Building", dry_run=True)   # check the list
bv.inject_into_path("/Game/Building")
```

A per-pixel cut has to execute in the material shader — `clip()` runs nowhere
else, and no engine switch injects it globally. Given that constraint, the goal
is to make the per-material cost *one scripted node*, which is what this does.

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

### Blend Mode must become Masked

The injector sets `BLEND_MASKED`, and this is not cosmetic either.

An **Opaque** material still renders in the depth prepass, and **the prepass
does not evaluate the opacity mask**. Clipped pixels would write depth, then be
discarded in the base pass — so the wall you cut away would still occlude the
pipes behind it. You would see through the wall to *nothing*.

Masked makes the prepass run the same test, so depth and colour agree. This is
the single most likely cause of "clipping looks broken", and it is hard to
diagnose from the symptom.

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

## Roadmap

- [x] Movable/scalable clip volume actor
- [x] Global shader parameters via MPC
- [x] World-space oriented-box clipping, multi-volume CSG
- [x] Scripted bulk injection into existing materials
- [ ] `BuildingVisualizationManager` — `SelectFloor` / `SelectRoom` / `SelectSystem`
- [ ] Semantic selection via Gameplay Tags (`Floor.08`, `System.Pipe`, …)
- [ ] Ghost / X-ray mode via Custom Depth-Stencil + post process
- [ ] Combined floor isolation + clipping + ghosting
- [ ] Capped cross-sections (solid cut faces)

## Licence

Copyright Matin. All rights reserved.
