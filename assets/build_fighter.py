# Builds the clayfray test fighter and exports assets/fighter.glb.
# Run: /Applications/Blender.app/Contents/MacOS/Blender --background --python assets/build_fighter.py
#
# Deliberately mirrors the hand-coded charBodyAnalytic proportions (chunky
# Trap Door forms — they're load-bearing for the SDF pipeline). Engine
# coordinate mapping: engine (x, y-up, z-forward) == Blender (x, -z, y)
# via the glTF Y-up export.

import bpy
import math

# ---------- clean scene ----------
bpy.ops.wm.read_factory_settings(use_empty=True)
scene = bpy.context.scene

def eng(x, y, z):
    """engine coords (y up, +z forward) -> blender (z up, -y forward)"""
    return (x, -z, y)

# ---------- body: union of ellipsoid spheres, voxel-remeshed + smoothed ----------
# (metaball falloff makes element sizes lie; real spheres are deterministic)
parts = []

def ell(pos, radii):
    bpy.ops.mesh.primitive_uv_sphere_add(radius=1.0, segments=24, ring_count=16,
                                         location=eng(*pos))
    ob = bpy.context.view_layer.objects.active
    ob.scale = (radii[0], radii[2], radii[1])  # engine->blender axis swap
    bpy.ops.object.transform_apply(scale=True)
    parts.append(ob)

# proportions from charBodyAnalytic
ell((0.0, 0.40, 0.0), (0.31, 0.38, 0.28))          # body
ell((0.0, 0.82, 0.03), (0.235, 0.215, 0.205))      # head
for s in (1, -1):
    ell((s * 0.29, 0.50, 0.04), (0.09, 0.13, 0.09))      # shoulder
    ell((s * 0.38, 0.38, 0.09), (0.085, 0.13, 0.08))     # upper arm
    ell((s * 0.44, 0.28, 0.13), (0.08, 0.11, 0.07))      # forearm
    ell((s * 0.47, 0.235, 0.16), (0.085, 0.115, 0.055))  # mitten palm
    ell((s * 0.388, 0.27, 0.20), (0.042, 0.06, 0.042))   # thumb
    ell((s * 0.15, 0.05, 0.11), (0.13, 0.055, 0.17))     # foot

bpy.ops.object.select_all(action='DESELECT')
for ob in parts:
    ob.select_set(True)
bpy.context.view_layer.objects.active = parts[0]
bpy.ops.object.join()
body = bpy.context.view_layer.objects.active
body.name = "fighter_body"

# union + uniform topology, then soften the intersections into clay blends
body.data.remesh_voxel_size = 0.011
bpy.ops.object.voxel_remesh()
smooth = body.modifiers.new("smooth", 'SMOOTH')
smooth.factor = 1.3
smooth.iterations = 40
bpy.ops.object.modifier_apply(modifier="smooth")

# ---------- vertex color: dusky clay cyan (engine adds mottle at voxelize) ----------
# glTF COLOR_0 is linear; supply pre-linearized values so the engine's clay
# reads match the hand-coded character exactly
lin = lambda c: c ** 2.2
col = body.data.color_attributes.new(name="Col", type='BYTE_COLOR', domain='CORNER')
for c in col.data:
    c.color = (lin(0.15), lin(0.47), lin(0.53), 1.0)

# ---------- armature: few fat bones, chunky-form doctrine ----------
arm_data = bpy.data.armatures.new("fighter_arm")
arm_obj = bpy.data.objects.new("fighter_rig", arm_data)
scene.collection.objects.link(arm_obj)
bpy.context.view_layer.objects.active = arm_obj
bpy.ops.object.mode_set(mode='EDIT')

def bone(name, head, tail, parent=None):
    b = arm_data.edit_bones.new(name)
    b.head = eng(*head)
    b.tail = eng(*tail)
    if parent:
        b.parent = arm_data.edit_bones[parent]
    return b

bone("root", (0, 0.0, 0), (0, 0.18, 0))
bone("spine", (0, 0.18, 0), (0, 0.62, 0), "root")
bone("head", (0, 0.62, 0.02), (0, 1.02, 0.05), "spine")
bone("arm.L", (0.22, 0.58, 0.03), (0.47, 0.24, 0.16), "spine")
bone("arm.R", (-0.22, 0.58, 0.03), (-0.47, 0.24, 0.16), "spine")
bpy.ops.object.mode_set(mode='OBJECT')

# auto weights
bpy.ops.object.select_all(action='DESELECT')
body.select_set(True)
arm_obj.select_set(True)
bpy.context.view_layer.objects.active = arm_obj
bpy.ops.object.parent_set(type='ARMATURE_AUTO')

# ---------- marbles: glass-bead eyes + pupils (never clay) ----------
def marble(name, pos, radius, color):
    bpy.ops.mesh.primitive_uv_sphere_add(radius=radius, location=eng(*pos))
    ob = bpy.context.view_layer.objects.active
    ob.name = name
    mat = bpy.data.materials.new(name + "_mat")
    mat.use_nodes = True
    mat.node_tree.nodes["Principled BSDF"].inputs["Base Color"].default_value = (*color, 1.0)
    ob.data.materials.append(mat)
    return ob

for s, side in ((1, "L"), (-1, "R")):
    marble(f"marble_eye_{side}", (s * 0.084, 1.055, 0.055), 0.078, (0.84, 0.82, 0.72))
    marble(f"marble_pupil_{side}", (s * 0.094, 1.069, 0.123), 0.026, (0.02, 0.02, 0.02))

# ---------- save editable .blend, then export ----------
import os
base = os.path.dirname(os.path.abspath(__file__))
blend = os.path.join(base, "fighter.blend")
bpy.ops.wm.save_as_mainfile(filepath=blend)
print("saved", blend)

out = os.path.join(base, "fighter.glb")
bpy.ops.export_scene.gltf(
    filepath=out,
    export_format='GLB',
    export_skins=True,
    export_yup=True,
    export_apply=True,
    export_animations=False,
)
print("exported", out)
