"""Monta uma cidade média europeia de referência e renderiza uma imagem.

A cidade usa apenas os 12 módulos da biblioteca semi-realista, distribuídos com
planejamento urbano plausível: centro denso, bairros residenciais em anéis, zona
industrial a leste e um marco cívico no centro. Serve para validar a direção
artística antes de ampliar a biblioteca.

Uso (dentro do Blender):

  exec(compile(open(r"G:/projetos/opengs-game/tools/blender/render_reference_city.py").read(),
               "render_reference_city.py", "exec"))

Saída:
  docs/urban_art/reference_city.png
"""

import importlib
import math
import os
import random
import sys

import bmesh
import bpy
from mathutils import Vector

ROOT = r"G:/projetos/opengs-game"
KIT_DIR = os.path.join(ROOT, "tools/blender")
OUT_DIR = os.path.join(ROOT, "docs/urban_art")
OUT_PATH = os.path.join(OUT_DIR, "reference_city.png")

if KIT_DIR not in sys.path:
    sys.path.insert(0, KIT_DIR)

import city_kit  # noqa: E402

importlib.reload(city_kit)

LOT_PITCH = 0.5
STREET_PERIOD = 4
STREET_OFFSET = 2
CITY_RADIUS = 14
FILL_RATIO = 0.78


def make_box_object(name, size, material_name):
    mesh = bpy.data.meshes.new(name)
    mesh.materials.append(city_kit.material(material_name))
    bm = bmesh.new()
    city_kit.add_box(bm, (0.0, 0.0, size[2] * 0.5), size, 0)
    bm.to_mesh(mesh)
    bm.free()
    return bpy.data.objects.new(name, mesh)


def is_street(i, j):
    return (i % STREET_PERIOD == STREET_OFFSET) or (j % STREET_PERIOD == STREET_OFFSET)


def ring_of(i, j):
    return max(abs(i), abs(j))


def is_industrial(i, j, ring):
    return ring >= 7 and i > abs(j) * 1.25


def pick_module(rng, i, j, ring):
    if i == 0 and j == 0:
        return "marco_publico"
    if is_industrial(i, j, ring):
        return "ind_fabrica" if rng.random() < 0.3 else "ind_galpao"
    if ring <= 1:
        if ring == 1 and rng.random() < 0.35:
            return "com_torre"
        return "com_escritorio_medio" if rng.random() < 0.55 else "com_loja"
    if ring <= 3:
        roll = rng.random()
        if roll < 0.35:
            return "com_loja"
        if roll < 0.55:
            return "res_predio_10"
        return "res_predio_6" if roll < 0.8 else "res_sobrado"
    if ring <= 6:
        roll = rng.random()
        if roll < 0.30:
            return "res_predio_6"
        if roll < 0.55:
            return "res_conjunto"
        if roll < 0.80:
            return "res_sobrado"
        return "com_loja"
    if ring <= 10:
        roll = rng.random()
        if roll < 0.45:
            return "res_casa_urbana"
        if roll < 0.75:
            return "res_vila"
        return "res_sobrado"
    return "res_casa_urbana" if rng.random() < 0.6 else "res_vila"


def build_templates(collection):
    templates = {}
    for module in city_kit.MODULES:
        module_id, _category, _zone, _level, _description, builder = module
        obj, _footprint, _height = city_kit.build_mesh("ref_" + module_id, builder())
        collection.objects.link(obj)
        templates[module_id] = obj
    return templates


def compose_city(scene, templates, street_object):
    rng = random.Random(20260101)
    placed = 0

    for i in range(-CITY_RADIUS, CITY_RADIUS + 1):
        for j in range(-CITY_RADIUS, CITY_RADIUS + 1):
            ring = ring_of(i, j)
            if ring > CITY_RADIUS:
                continue
            if ring > 3 and rng.random() > FILL_RATIO:
                continue

            x = i * LOT_PITCH + rng.uniform(-0.018, 0.018)
            y = -(j * LOT_PITCH) + rng.uniform(-0.018, 0.018)

            if is_street(i, j):
                copy = street_object.copy()
                copy.location = (i * LOT_PITCH, -(j * LOT_PITCH), 0.004)
                scene.collection.objects.link(copy)
                continue

            module_id = pick_module(rng, i, j, ring)
            template = templates[module_id]
            copy = template.copy()
            copy.location = (x, y, 0.0)
            copy.rotation_euler = (0.0, 0.0,
                                   rng.choice([0.0, math.pi * 0.5, math.pi, math.pi * 1.5])
                                   + rng.uniform(-0.03, 0.03))
            scene.collection.objects.link(copy)
            placed += 1

    return placed


def add_ground(scene):
    ground = make_box_object("RefGround", (40.0, 40.0, 0.02), "grass")
    ground.location = (0.0, 0.0, -0.02)
    scene.collection.objects.link(ground)

    city_plate = make_box_object("RefPlate", (CITY_RADIUS * LOT_PITCH * 2.1,
                                              CITY_RADIUS * LOT_PITCH * 2.1, 0.02),
                                 "paving")
    city_plate.location = (0.0, 0.0, -0.012)
    scene.collection.objects.link(city_plate)


def add_camera_and_light(scene):
    camera_data = bpy.data.cameras.new("RefCamera")
    camera_data.lens = 45.0
    camera = bpy.data.objects.new("RefCamera", camera_data)
    camera.location = (-10.0, -13.0, 9.5)
    target = Vector((1.0, 0.0, 0.4))
    camera.rotation_euler = (target - camera.location).to_track_quat("-Z", "Y").to_euler()
    scene.collection.objects.link(camera)
    scene.camera = camera

    sun_data = bpy.data.lights.new("RefSun", "SUN")
    sun_data.energy = 5.5
    sun_data.color = (1.0, 0.94, 0.86)
    sun_data.angle = math.radians(1.5)
    sun = bpy.data.objects.new("RefSun", sun_data)
    sun.rotation_euler = (math.radians(55.0), 0.0, math.radians(25.0))
    scene.collection.objects.link(sun)


def configure_world(scene):
    world = bpy.data.worlds.new("RefWorld")
    world.use_nodes = True
    background = world.node_tree.nodes.get("Background")
    background.inputs["Color"].default_value = (0.58, 0.62, 0.66, 1.0)
    background.inputs["Strength"].default_value = 0.16
    scene.world = world


def configure_render(scene):
    scene.render.engine = "BLENDER_EEVEE"
    scene.render.resolution_x = 1920
    scene.render.resolution_y = 1080
    scene.render.resolution_percentage = 100
    scene.render.film_transparent = False
    scene.render.image_settings.file_format = "PNG"
    scene.render.filepath = OUT_PATH

    eevee = getattr(scene, "eevee", None)
    if eevee is not None:
        for attribute, value in (("taa_render_samples", 96), ("use_gtao", True),
                                 ("gtao_distance", 1.2), ("use_shadows", True)):
            if hasattr(eevee, attribute):
                setattr(eevee, attribute, value)

    scene.view_settings.view_transform = "AgX"
    scene.view_settings.exposure = -0.25
    try:
        scene.view_settings.look = "AgX - Medium High Contrast"
    except TypeError:
        pass


def main():
    os.makedirs(OUT_DIR, exist_ok=True)

    window = bpy.context.window
    previous_scene = window.scene if window else bpy.context.scene

    scene = bpy.data.scenes.new("WE_ReferenceCity")
    templates_collection = bpy.data.collections.new("RefTemplates")
    scene.collection.children.link(templates_collection)

    if window:
        window.scene = scene

    templates = build_templates(templates_collection)
    street = make_box_object("RefStreet", (LOT_PITCH, LOT_PITCH, 0.008), "asphalt")

    add_ground(scene)
    placed = compose_city(scene, templates, street)
    add_camera_and_light(scene)
    configure_world(scene)
    configure_render(scene)

    templates_collection.hide_render = True
    templates_collection.hide_viewport = True

    bpy.ops.render.render(write_still=True)
    print("Cidade de referência renderizada:", placed, "edifícios ->", OUT_PATH)

    if window:
        window.scene = previous_scene


main()
