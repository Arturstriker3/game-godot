"""Gera a biblioteca modular de edifícios do WarEconomy no Blender.

Cada módulo é um único mesh com vários materiais (paredes, telhado, vidro),
origem no centro do lote no chão e frente para +Y (equivale a -Z no Godot).
Saídas:

  assets/models/city_modules/<id>.glb   modelos exportados
  assets/models/city_modules/library.json   metadados (footprint, altura, zona)
  tools/blender/war_economy_city_library.blend  arquivo editável com a biblioteca

Uso no Blender:
  exec(compile(open(r"G:/projetos/opengs-game/tools/blender/build_city_library.py").read(),
               "build_city_library.py", "exec"))

O script cria uma cena temporária e devolve a cena original ao final; o arquivo
aberto do usuário não é alterado.
"""

import bmesh
import bpy
import json
import os
from mathutils import Vector

ROOT = r"G:/projetos/opengs-game"
GLB_DIR = os.path.join(ROOT, "assets/models/city_modules")
BLEND_PATH = os.path.join(ROOT, "tools/blender/war_economy_city_library.blend")

# Mesma paleta do city.glb existente, para os módulos novos casarem com o mapa.
PALETTE = {
    "plaster": (0.90, 0.90, 0.86),
    "beige": (0.76, 0.64, 0.45),
    "brick": (0.58, 0.24, 0.16),
    "concrete": (0.60, 0.56, 0.47),
    "white": (0.90, 0.90, 0.86),
    "roof_red": (0.66, 0.13, 0.10),
    "roof_dark": (0.22, 0.24, 0.26),
    "glass": (0.16, 0.46, 0.72),
    "steel": (0.55, 0.58, 0.60),
    "asphalt": (0.10, 0.10, 0.12),
    "grass": (0.16, 0.40, 0.14),
    "plaza": (0.52, 0.47, 0.34),
    "paint": (0.94, 0.90, 0.62),
}


def material(name):
    mat = bpy.data.materials.get(name)
    if mat:
        return mat

    mat = bpy.data.materials.new(name)
    mat.use_nodes = True
    bsdf = mat.node_tree.nodes.get("Principled BSDF")
    bsdf.inputs["Base Color"].default_value = (*PALETTE[name], 1.0)
    bsdf.inputs["Roughness"].default_value = 0.28 if name == "glass" else 0.85
    bsdf.inputs["Metallic"].default_value = 0.35 if name in ("steel", "glass") else 0.0
    return mat


def add_box(bm, center, size, index):
    verts = bmesh.ops.create_cube(bm, size=1.0)["verts"]
    bmesh.ops.scale(bm, vec=Vector(size), verts=verts)
    bmesh.ops.translate(bm, vec=Vector(center), verts=verts)
    for vert in verts:
        for face in vert.link_faces:
            face.material_index = index


def add_cylinder(bm, center, radius, height, index, segments=10):
    verts = bmesh.ops.create_cone(
        bm, cap_ends=True, cap_tris=False, segments=segments,
        radius1=radius, radius2=radius, depth=height,
    )["verts"]
    bmesh.ops.translate(bm, vec=Vector(center), verts=verts)
    for vert in verts:
        for face in vert.link_faces:
            face.material_index = index


def windows_row(parts, y, z, count, width, wmat="glass"):
    step = width * 1.8
    start = -step * (count - 1) * 0.5
    for index in range(count):
        parts.append(("box", (start + index * step, y, z), (width, 0.012, 0.045), wmat))


def residencial_casa():
    parts = [
        ("box", (0, 0, 0.015), (0.32, 0.32, 0.03), "concrete"),
        ("box", (0, 0, 0.105), (0.26, 0.26, 0.15), "plaster"),
        ("box", (0, 0, 0.21), (0.30, 0.30, 0.06), "roof_red"),
        ("box", (0, -0.135, 0.065), (0.06, 0.012, 0.07), "asphalt"),
    ]
    windows_row(parts, -0.135, 0.115, 2, 0.045)
    return parts


def residencial_medio():
    parts = [
        ("box", (0, 0, 0.015), (0.36, 0.36, 0.03), "concrete"),
        ("box", (0, 0, 0.28), (0.30, 0.30, 0.50), "brick"),
        ("box", (0, 0, 0.545), (0.34, 0.34, 0.03), "concrete"),
    ]
    for floor in range(3):
        z = 0.10 + floor * 0.14
        windows_row(parts, -0.152, z, 3, 0.05)
        windows_row(parts, 0.152, z, 3, 0.05)
    return parts


def residencial_torre():
    parts = [
        ("box", (0, 0, 0.02), (0.32, 0.32, 0.04), "concrete"),
        ("box", (0, 0, 0.60), (0.26, 0.26, 1.12), "concrete"),
        ("box", (0, 0, 1.20), (0.28, 0.28, 0.06), "white"),
        ("cyl", (0, 0, 1.32), (0.012, 0.18), "steel"),
    ]
    for band in range(5):
        parts.append(("box", (0, 0, 0.20 + band * 0.21), (0.272, 0.272, 0.07), "glass"))
    return parts


def comercio_loja():
    return [
        ("box", (0, 0, 0.015), (0.34, 0.34, 0.03), "concrete"),
        ("box", (0, 0, 0.165), (0.30, 0.30, 0.28), "beige"),
        ("box", (0, -0.152, 0.09), (0.24, 0.014, 0.14), "glass"),
        ("box", (0, -0.175, 0.195), (0.28, 0.07, 0.02), "roof_red"),
        ("box", (0, 0, 0.32), (0.33, 0.33, 0.03), "concrete"),
        ("box", (0, -0.152, 0.245), (0.12, 0.014, 0.05), "paint"),
    ]


def comercio_escritorio():
    parts = [
        ("box", (0, 0, 0.02), (0.36, 0.36, 0.04), "concrete"),
        ("box", (0, 0, 0.47), (0.28, 0.28, 0.86), "white"),
        ("box", (0, 0, 0.92), (0.30, 0.30, 0.04), "concrete"),
        ("box", (0.06, 0.06, 0.97), (0.12, 0.12, 0.06), "steel"),
    ]
    for band in range(5):
        parts.append(("box", (0, 0, 0.16 + band * 0.17), (0.292, 0.292, 0.075), "glass"))
    return parts


def industria_galpao():
    parts = [
        ("box", (0, 0, 0.01), (0.90, 0.60, 0.02), "asphalt"),
        ("box", (0, 0, 0.13), (0.70, 0.40, 0.22), "steel"),
        ("box", (0, 0, 0.255), (0.74, 0.44, 0.03), "roof_dark"),
    ]
    for door in range(3):
        parts.append(("box", (-0.22 + door * 0.22, -0.202, 0.10),
                      (0.12, 0.012, 0.16), "asphalt"))
    return parts


def industria_fabrica():
    return [
        ("box", (0, 0, 0.01), (0.62, 0.62, 0.02), "concrete"),
        ("box", (0, 0, 0.16), (0.50, 0.42, 0.28), "brick"),
        ("box", (0, 0, 0.315), (0.54, 0.46, 0.03), "roof_dark"),
        ("cyl", (-0.15, 0.06, 0.44), (0.045, 0.46), "steel"),
        ("cyl", (0.02, 0.06, 0.40), (0.045, 0.38), "steel"),
        ("cyl", (0.24, -0.16, 0.16), (0.09, 0.30), "concrete"),
    ]


def infra_praca():
    parts = [
        ("box", (0, 0, 0.01), (0.48, 0.48, 0.02), "plaza"),
        ("box", (0, 0.20, 0.008), (0.50, 0.10, 0.012), "asphalt"),
    ]
    for dx in (-0.15, 0.15):
        for dy in (-0.15, 0.15):
            parts.append(("box", (dx, dy, 0.06), (0.05, 0.05, 0.09), "grass"))
    return parts


MODULES = [
    ("residencial_casa", "residential", "residential", residencial_casa),
    ("residencial_medio", "residential", "residential", residencial_medio),
    ("residencial_torre", "residential", "residential", residencial_torre),
    ("comercio_loja", "commercial", "commercial", comercio_loja),
    ("comercio_escritorio", "commercial", "commercial", comercio_escritorio),
    ("industria_galpao", "industrial", "industrial", industria_galpao),
    ("industria_fabrica", "industrial", "industrial", industria_fabrica),
    ("infra_praca", "infrastructure", "public", infra_praca),
]


def build_object(module_id, parts, collection):
    mat_names = []
    for part in parts:
        if part[3] not in mat_names:
            mat_names.append(part[3])

    mesh = bpy.data.meshes.new(module_id)
    for name in mat_names:
        mesh.materials.append(material(name))

    bm = bmesh.new()
    for part in parts:
        index = mat_names.index(part[3])
        if part[0] == "box":
            add_box(bm, part[1], part[2], index)
        else:
            add_cylinder(bm, part[1], part[2][0], part[2][1], index)
    bmesh.ops.recalc_face_normals(bm, faces=bm.faces)

    low = Vector((1e9, 1e9, 1e9))
    high = Vector((-1e9, -1e9, -1e9))
    for vert in bm.verts:
        for axis in range(3):
            low[axis] = min(low[axis], vert.co[axis])
            high[axis] = max(high[axis], vert.co[axis])

    bm.to_mesh(mesh)
    bm.free()

    obj = bpy.data.objects.new(module_id, mesh)
    collection.objects.link(obj)

    footprint = [round(high.x - low.x, 3), round(high.y - low.y, 3)]
    return obj, footprint, round(high.z - low.z, 3)


def export_glb(obj):
    """Exporta só a cena temporária (com um único módulo) para GLB."""
    path = os.path.join(GLB_DIR, obj.name + ".glb")
    bpy.ops.export_scene.gltf(
        filepath=path, export_format="GLB",
        use_active_scene=True, use_selection=False, export_apply=True,
    )


def main():
    os.makedirs(GLB_DIR, exist_ok=True)
    os.makedirs(os.path.dirname(BLEND_PATH), exist_ok=True)

    window = bpy.context.window
    previous_scene = window.scene if window else bpy.context.scene

    # Cena de export: guarda um módulo por vez.
    export_scene = bpy.data.scenes.new("WE_CityExport")
    export_collection = bpy.data.collections.new("WE_CityExportModules")
    export_scene.collection.children.link(export_collection)

    # Cena do .blend editável: recebe todos os módulos lado a lado.
    library_scene = bpy.data.scenes.new("WE_CityLibrary")
    library_collection = bpy.data.collections.new("WE_CityModules")
    library_scene.collection.children.link(library_collection)

    if window:
        window.scene = export_scene

    library = []
    try:
        for index, (module_id, category, zone, builder) in enumerate(MODULES):
            obj, footprint, height = build_object(module_id, builder(), export_collection)
            export_glb(obj)

            export_collection.objects.unlink(obj)
            library_collection.objects.link(obj)
            obj.location = ((index % 4) * 1.3, (index // 4) * 1.6, 0.0)

            library.append({
                "id": module_id,
                "file": "res://assets/models/city_modules/%s.glb" % module_id,
                "category": category,
                "zone": zone,
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
            bpy.data.meshes.remove(mesh)
        bpy.data.collections.remove(export_collection)
        bpy.data.collections.remove(library_collection)
        bpy.data.scenes.remove(export_scene)
        bpy.data.scenes.remove(library_scene)
        if window:
            window.scene = previous_scene

    print("Biblioteca gerada:", len(library), "módulos")


if __name__ == "__main__":
    main()
