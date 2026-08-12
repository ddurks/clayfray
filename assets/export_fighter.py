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

out = os.path.join(os.path.dirname(bpy.data.filepath), "fighter.glb")
bpy.ops.export_scene.gltf(
    filepath=out,
    export_format='GLB',
    export_skins=True,
    export_yup=True,
    export_apply=True,
    export_animations=False,
)
print("exported", out)
