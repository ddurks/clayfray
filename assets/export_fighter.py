# Exports assets/fighter.blend -> assets/fighter.glb with the settings the
# engine expects. Use after editing fighter.blend in Blender:
#   /Applications/Blender.app/Contents/MacOS/Blender --background assets/fighter.blend --python assets/export_fighter.py
#
# Authoring rules the engine relies on:
#  - body mesh: closed/watertight, chunky forms, skinned to the armature
#  - vertex colors: pre-linearized values (glTF COLOR_0 is linear)
#  - rigid props (eyes etc): separate objects named marble_*, material base
#    color = the marble color; never joined into the body

import bpy
import os

from mathutils import Matrix

out = os.path.join(os.path.dirname(bpy.data.filepath), "fighter.glb")

# The glTF exporter silently SKIPS modifier application on skinned meshes
# (applying would bake the pose), so a Subdivision Surface modifier never
# reaches the .glb. Pre-apply every non-Armature modifier here, in memory at
# rest pose: disable the armature, evaluate, swap the mesh in, drop the
# applied modifiers. The .blend is never saved — the low-poly cage survives.
body = bpy.data.objects.get("body")
if body and any(m.type != 'ARMATURE' for m in body.modifiers):
    for m in body.modifiers:
        if m.type == 'ARMATURE':
            m.show_viewport = False
        elif m.type == 'SUBSURF':
            m.levels = m.render_levels  # export at render quality
    dg = bpy.context.evaluated_depsgraph_get()
    me = bpy.data.meshes.new_from_object(body.evaluated_get(dg))
    body.data = me
    for m in list(body.modifiers):
        if m.type != 'ARMATURE':
            body.modifiers.remove(m)
        else:
            m.show_viewport = True
    print("pre-applied body modifiers: %d polys" % len(me.polygons))

# Marbles are rigid analytic beads in the engine: it reads position from the
# NODE TRANSLATION and radius from the mesh bound about its own origin, and
# binds them to the nearest bone itself. Skinned/parented marbles export with
# neither, which renders as a giant sphere at the world origin. Normalize
# here, in memory (the .blend is never saved): unparent at world transform,
# drop armature modifiers, recentre geometry so the origin carries position.
# In Blender, marbles may stay parented/skinned for preview — it won't leak.
for o in [x for x in bpy.data.objects if x.name.startswith("marble_")]:
    for m in list(o.modifiers):
        if m.type == 'ARMATURE':
            o.modifiers.remove(m)
    mw = o.matrix_world.copy()
    o.parent = None
    o.matrix_world = mw
    me = o.data
    n = len(me.vertices)
    c = [sum(v.co[k] for v in me.vertices) / n for k in range(3)]
    for v in me.vertices:
        v.co = (v.co[0] - c[0], v.co[1] - c[1], v.co[2] - c[2])
    o.matrix_world = o.matrix_world @ Matrix.Translation(c)
    print("marble %-16s world=%s" % (
        o.name, [round(v, 4) for v in o.matrix_world.translation]))

# The body's clay colour lives in COLOR_0, not in a material — the engine only
# reads material base colour for marble_* props. Blender's default
# export_vertex_color='MATERIAL' drops the attribute unless a material happens
# to reference it, so force ACTIVE.
if body and not body.data.color_attributes:
    raise SystemExit("export aborted: body has no colour attribute -> "
                     "COLOR_0 would be missing and the fighter renders flat grey")

bpy.ops.export_scene.gltf(
    filepath=out,
    export_format='GLB',
    export_skins=True,
    export_yup=True,
    export_apply=True,
    # M4 consumes clips; nothing reads them yet, but dropping them here would
    # silently lose authored animation from the asset
    export_animations=True,
    export_vertex_color='ACTIVE',
)
print("exported", out)
