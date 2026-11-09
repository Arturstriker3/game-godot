"""Gera a biblioteca semi-realista de edifícios do WarEconomy no Blender 5.x.

Uso (dentro do Blender, via MCP ou console):

  exec(compile(open(r"G:/projetos/opengs-game/tools/blender/build_city_library_v2.py").read(),
               "build_city_library_v2.py", "exec"))

Saídas:
  assets/models/city_modules/<id>.glb       12 módulos exportados
  assets/models/city_modules/library.json   metadados (categoria, zona, nível, footprint)
  tools/blender/war_economy_city_library_v2.blend   biblioteca editável

O script monta tudo em cenas temporárias e devolve a cena original ao final.
"""

import importlib
import json
import os
import sys

import bpy

ROOT = r"G:/projetos/opengs-game"
KIT_DIR = os.path.join(ROOT, "tools/blender")
GLB_DIR = os.path.join(ROOT, "assets/models/city_modules")
BLEND_PATH = os.path.join(ROOT, "tools/blender/war_economy_city_library_v2.blend")

if KIT_DIR not in sys.path:
    sys.path.insert(0, KIT_DIR)

import city_kit  # noqa: E402

importlib.reload(city_kit)


def clear_previous_glb():
    if not os.path.isdir(GLB_DIR):
        return
    for name in os.listdir(GLB_DIR):
        if name.endswith(".glb"):
            os.remove(os.path.join(GLB_DIR, name))


def export_glb(obj, path):
    bpy.ops.export_scene.gltf(
        filepath=path, export_format="GLB",
        use_active_scene=True, use_selection=False, export_apply=True,
    )


def main():
    os.makedirs(GLB_DIR, exist_ok=True)
    os.makedirs(os.path.dirname(BLEND_PATH), exist_ok=True)
    clear_previous_glb()

    window = bpy.context.window
    previous_scene = window.scene if window else bpy.context.scene

    export_scene = bpy.data.scenes.new("WE_CityExport")
    export_collection = bpy.data.collections.new("WE_CityExportModules")
    export_scene.collection.children.link(export_collection)

    library_scene = bpy.data.scenes.new("WE_CityLibrary")
    library_collection = bpy.data.collections.new("WE_CityModules")
    library_scene.collection.children.link(library_collection)

    if window:
        window.scene = export_scene

    library = []
    try:
        for index, module in enumerate(city_kit.MODULES):
            module_id, category, zone, level, description, builder = module
            obj, footprint, height = city_kit.build_mesh(module_id, builder())
            export_collection.objects.link(obj)
            export_glb(obj, os.path.join(GLB_DIR, module_id + ".glb"))

            export_collection.objects.unlink(obj)
            library_collection.objects.link(obj)
            obj.location = ((index % 4) * 1.6, (index // 4) * 1.8, 0.0)

            library.append({
                "id": module_id,
                "file": "res://assets/models/city_modules/%s.glb" % module_id,
                "category": category,
                "zone": zone,
                "level": level,
                "description": description,
                "footprint": footprint,
                "height": height,
            })

        with open(os.path.join(GLB_DIR, "library.json"), "w", encoding="utf-8") as handle:
            json.dump({"modules": library}, handle, indent=2)

        bpy.ops.wm.save_as_mainfile(filepath=BLEND_PATH, copy=True)
    finally:
        for obj in list(library_collection.objects):
            mesh = obj.data
            bpy.data.objects.remove(obj, do_unlink=True)
            if mesh is not None:
                bpy.data.meshes.remove(mesh)
        bpy.data.collections.remove(export_collection)
        bpy.data.collections.remove(library_collection)
        bpy.data.scenes.remove(export_scene)
        bpy.data.scenes.remove(library_scene)
        if window:
            window.scene = previous_scene

    print("Biblioteca semi-realista gerada:", len(library), "módulos")
    for entry in library:
        print(" - %-22s %-12s h=%.2f footprint=%s" % (
            entry["id"], entry["category"], entry["height"], entry["footprint"]))


main()
