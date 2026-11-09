"""Kit de arquitetura semi-realista das cidades do WarEconomy.

Cada módulo é um único mesh com materiais compartilhados (pedra, concreto,
tijolo, reboco, ardósia, vidro escuro, metal). A origem fica no centro do lote
no chão e a frente aponta para +Y (equivale a -Z no Godot).

Escala: 1 unidade de mundo ~ 50 m. Um lote do jogo mede 0,5 unidade (25 m), o
pé-direito residencial tem 0,055 unidade (~2,75 m) e as torres chegam a 1,3
unidade (~65 m). A direção é sóbria: sem branco puro, sem azul-claro, sem
plástico — materiais naturais e pouco saturados.
"""

import bmesh
import bpy
from mathutils import Vector

# (r, g, b, roughness, metallic)
PALETTE = {
    "stone": (0.58, 0.55, 0.49, 0.90, 0.0),
    "stone_dark": (0.42, 0.40, 0.36, 0.92, 0.0),
    "concrete": (0.52, 0.51, 0.47, 0.92, 0.0),
    "concrete_dark": (0.38, 0.38, 0.36, 0.90, 0.0),
    "plaster_warm": (0.74, 0.69, 0.60, 0.90, 0.0),
    "plaster_cool": (0.69, 0.70, 0.67, 0.90, 0.0),
    "brick": (0.43, 0.26, 0.21, 0.88, 0.0),
    "brick_dark": (0.33, 0.20, 0.17, 0.90, 0.0),
    "roof_tile": (0.40, 0.24, 0.19, 0.85, 0.0),
    "roof_slate": (0.19, 0.21, 0.22, 0.80, 0.0),
    "glass": (0.11, 0.15, 0.16, 0.16, 0.0),
    "glass_warm": (0.21, 0.19, 0.15, 0.18, 0.0),
    "metal": (0.35, 0.37, 0.38, 0.50, 0.70),
    "metal_dark": (0.22, 0.23, 0.24, 0.55, 0.60),
    "asphalt": (0.095, 0.095, 0.10, 0.98, 0.0),
    "paving": (0.42, 0.40, 0.36, 0.95, 0.0),
    "grass": (0.20, 0.27, 0.15, 1.0, 0.0),
    "trim": (0.28, 0.27, 0.25, 0.90, 0.0),
    "white_soft": (0.76, 0.75, 0.70, 0.90, 0.0),
    "rust": (0.34, 0.21, 0.16, 0.92, 0.10),
    "soot": (0.11, 0.11, 0.12, 0.95, 0.0),
    "steel_light": (0.50, 0.52, 0.53, 0.42, 0.75),
    "tank_shell": (0.62, 0.62, 0.58, 0.55, 0.30),
}

FLOOR_HEIGHT = 0.055


def material(name):
    mat = bpy.data.materials.get(name)
    if mat is not None:
        return mat

    mat = bpy.data.materials.new(name)
    mat.use_nodes = True
    bsdf = mat.node_tree.nodes.get("Principled BSDF")
    red, green, blue, roughness, metallic = PALETTE[name]
    bsdf.inputs["Base Color"].default_value = (red, green, blue, 1.0)
    bsdf.inputs["Roughness"].default_value = roughness
    bsdf.inputs["Metallic"].default_value = metallic
    return mat


# ------------------------------------------------------------------ geometria

def _assign(faces, index):
    for face in faces:
        face.material_index = index


def _faces_of(verts):
    faces = set()
    for vert in verts:
        faces.update(vert.link_faces)
    return faces


def add_box(bm, center, size, index):
    verts = bmesh.ops.create_cube(bm, size=1.0)["verts"]
    bmesh.ops.scale(bm, vec=Vector(size), verts=verts)
    bmesh.ops.translate(bm, vec=Vector(center), verts=verts)
    _assign(_faces_of(verts), index)


def add_cylinder(bm, center, radius, height, index, segments=12):
    verts = bmesh.ops.create_cone(
        bm, cap_ends=True, cap_tris=False, segments=segments,
        radius1=radius, radius2=radius, depth=height,
    )["verts"]
    bmesh.ops.translate(bm, vec=Vector(center), verts=verts)
    _assign(_faces_of(verts), index)


def add_cone(bm, center, radius, height, index, segments=12):
    verts = bmesh.ops.create_cone(
        bm, cap_ends=True, cap_tris=True, segments=segments,
        radius1=radius, radius2=0.0, depth=height,
    )["verts"]
    bmesh.ops.translate(bm, vec=Vector(center), verts=verts)
    _assign(_faces_of(verts), index)


def add_wedge(bm, base_center, width, depth, height, index, overhang=0.0):
    """Telhado de duas águas com cumeeira no eixo X. Centro = base."""
    half_w = width * 0.5 + overhang
    half_d = depth * 0.5 + overhang
    z = base_center[2]
    x = base_center[0]
    y = base_center[1]
    coords = [
        (x - half_w, y - half_d, z), (x + half_w, y - half_d, z),
        (x + half_w, y + half_d, z), (x - half_w, y + half_d, z),
        (x - half_w, y, z + height), (x + half_w, y, z + height),
    ]
    verts = [bm.verts.new(co) for co in coords]
    faces = [
        (verts[0], verts[1], verts[2], verts[3]),
        (verts[0], verts[1], verts[5], verts[4]),
        (verts[2], verts[3], verts[4], verts[5]),
        (verts[0], verts[4], verts[3]),
        (verts[1], verts[2], verts[5]),
    ]
    created = [bm.faces.new(face) for face in faces]
    _assign(created, index)


def add_pyramid(bm, base_center, width, depth, height, index, overhang=0.0):
    """Telhado de quatro águas. Centro = base."""
    half_w = width * 0.5 + overhang
    half_d = depth * 0.5 + overhang
    z = base_center[2]
    x = base_center[0]
    y = base_center[1]
    coords = [
        (x - half_w, y - half_d, z), (x + half_w, y - half_d, z),
        (x + half_w, y + half_d, z), (x - half_w, y + half_d, z),
        (x, y, z + height),
    ]
    verts = [bm.verts.new(co) for co in coords]
    faces = [
        (verts[0], verts[1], verts[2], verts[3]),
        (verts[0], verts[1], verts[4]),
        (verts[1], verts[2], verts[4]),
        (verts[2], verts[3], verts[4]),
        (verts[3], verts[0], verts[4]),
    ]
    created = [bm.faces.new(face) for face in faces]
    _assign(created, index)


def add_taper(bm, base_center, bottom, top, height, index):
    """Bloco afunilado (recuos de torre e coroamentos). Centro = base."""
    half_bx, half_by = bottom[0] * 0.5, bottom[1] * 0.5
    half_tx, half_ty = top[0] * 0.5, top[1] * 0.5
    x, y, z = base_center
    coords = [
        (x - half_bx, y - half_by, z), (x + half_bx, y - half_by, z),
        (x + half_bx, y + half_by, z), (x - half_bx, y + half_by, z),
        (x - half_tx, y - half_ty, z + height), (x + half_tx, y - half_ty, z + height),
        (x + half_tx, y + half_ty, z + height), (x - half_tx, y + half_ty, z + height),
    ]
    verts = [bm.verts.new(co) for co in coords]
    faces = [
        (verts[0], verts[1], verts[2], verts[3]),
        (verts[4], verts[5], verts[6], verts[7]),
        (verts[0], verts[1], verts[5], verts[4]),
        (verts[1], verts[2], verts[6], verts[5]),
        (verts[2], verts[3], verts[7], verts[6]),
        (verts[3], verts[0], verts[4], verts[7]),
    ]
    created = [bm.faces.new(face) for face in faces]
    _assign(created, index)


# ------------------------------------------------------------------ fachadas

def pane(parts, x, y, z, width=0.042, height=0.048, mat="glass", depth=0.008):
    parts.append(("box", (x, y, z), (width, depth, height), mat))


def facade_rows(parts, half_x, half_y, heights, cols_x, cols_y,
                width=0.042, height=0.048, mat="glass"):
    """Panes nas quatro fachadas, distribuídos por coluna e altura."""
    step_x = (2.0 * half_x) / max(cols_x, 1)
    step_y = (2.0 * half_y) / max(cols_y, 1)
    for z in heights:
        for column in range(cols_x):
            x = -half_x + step_x * (column + 0.5)
            pane(parts, x, -half_y - 0.004, z, width, height, mat)
            pane(parts, x, half_y + 0.004, z, width, height, mat)
        for column in range(cols_y):
            y = -half_y + step_y * (column + 0.5)
            pane(parts, -half_x - 0.004, y, z, width, height, mat, )
            pane(parts, half_x + 0.004, y, z, width, height, mat)


def band(parts, center_z, half_x, half_y, thickness=0.012, mat="trim", inset=0.004):
    parts.append(("box", (0.0, 0.0, center_z),
                  (2.0 * half_x + inset, 2.0 * half_y + inset, thickness), mat))


# ------------------------------------------------------------------ modelos

def res_casa_urbana():
    """Sobrado geminado de três pavimentos, telhado de duas águas."""
    parts = []
    half_x, half_y = 0.205, 0.185
    parts.append(("box", (0, 0, 0.012), (0.44, 0.40, 0.024), "stone"))
    body_z0 = 0.024
    body_h = FLOOR_HEIGHT * 3.0
    parts.append(("box", (0, 0, body_z0 + body_h * 0.5),
                  (2 * half_x, 2 * half_y, body_h), "plaster_warm"))
    band(parts, body_z0 + body_h - 0.006, half_x, half_y, 0.014, "trim", 0.010)
    parts.append(("wedge", (0, 0, body_z0 + body_h), 2 * half_x, 2 * half_y,
                  0.075, "roof_tile", 0.012))
    parts.append(("box", (0.10, 0.05, body_z0 + body_h + 0.062),
                  (0.032, 0.032, 0.055), "brick_dark"))
    facade_rows(parts, half_x, half_y,
                [body_z0 + FLOOR_HEIGHT * 0.55, body_z0 + FLOOR_HEIGHT * 1.55,
                 body_z0 + FLOOR_HEIGHT * 2.55], 3, 3)
    parts.append(("box", (0.0, -half_y - 0.004, body_z0 + 0.045),
                  (0.05, 0.010, 0.085), "trim"))
    return parts


def res_vila():
    """Casa isolada de dois pavimentos, telhado de quatro águas e muro."""
    parts = []
    half_x, half_y = 0.11, 0.12
    parts.append(("box", (0, 0, 0.008), (0.30, 0.30, 0.016), "grass"))
    parts.append(("box", (0, 0, 0.024), (0.26, 0.28, 0.016), "paving"))
    body_h = FLOOR_HEIGHT * 2.0
    parts.append(("box", (0, 0, 0.032 + body_h * 0.5),
                  (2 * half_x, 2 * half_y, body_h), "plaster_cool"))
    parts.append(("pyramid", (0, 0, 0.032 + body_h), 2 * half_x, 2 * half_y,
                  0.085, "roof_tile", 0.014))
    parts.append(("box", (-0.055, 0.02, 0.032 + body_h + 0.07),
                  (0.026, 0.026, 0.05), "brick_dark"))
    facade_rows(parts, half_x, half_y,
                [0.032 + FLOOR_HEIGHT * 0.55, 0.032 + FLOOR_HEIGHT * 1.5],
                2, 2)
    parts.append(("box", (0.0, -half_y - 0.004, 0.075), (0.05, 0.010, 0.08), "brick_dark"))
    return parts


def res_sobrado():
    """Quadra residencial de quatro pavimentos, cobertura plana com platibanda."""
    parts = []
    half_x, half_y = 0.215, 0.185
    body_h = FLOOR_HEIGHT * 4.0
    parts.append(("box", (0, 0, 0.014), (0.45, 0.39, 0.028), "stone"))
    parts.append(("box", (0, 0, 0.028 + body_h * 0.5),
                  (2 * half_x, 2 * half_y, body_h), "plaster_warm"))
    band(parts, 0.028 + FLOOR_HEIGHT - 0.004, half_x, half_y, 0.010, "trim")
    band(parts, 0.028 + body_h + 0.006, half_x, half_y, 0.012, "concrete")
    facade_rows(parts, half_x, half_y,
                [0.028 + FLOOR_HEIGHT * (f + 0.55) for f in range(4)], 5, 4)
    for floor in (2, 3):
        z = 0.028 + FLOOR_HEIGHT * floor + 0.012
        parts.append(("box", (0.0, -half_y - 0.014, z),
                      (2 * half_x - 0.04, 0.018, 0.008), "concrete_dark"))
    return parts


def res_predio_6():
    """Bloco de apartamentos de sete pavimentos com caixa d'água na laje."""
    parts = []
    half_x, half_y = 0.205, 0.205
    base_h = 0.03
    body_h = FLOOR_HEIGHT * 7.0
    parts.append(("box", (0, 0, 0.012), (0.43, 0.43, 0.024), "stone"))
    parts.append(("box", (0, 0, base_h + body_h * 0.5),
                  (2 * half_x, 2 * half_y, body_h), "concrete"))
    band(parts, base_h + body_h + 0.008, half_x, half_y, 0.016, "concrete_dark")
    facade_rows(parts, half_x, half_y,
                [base_h + FLOOR_HEIGHT * (f + 0.55) for f in range(7)], 4, 4)
    parts.append(("box", (-0.05, 0.03, base_h + body_h + 0.03),
                  (0.09, 0.07, 0.03), "metal_dark"))
    return parts


def res_predio_10():
    """Lâmina residencial de dez pavimentos com pilastras e recuo no topo."""
    parts = []
    half_x, half_y = 0.19, 0.205
    base_h = 0.03
    body_h = FLOOR_HEIGHT * 10.0
    parts.append(("box", (0, 0, 0.012), (0.40, 0.43, 0.024), "concrete_dark"))
    parts.append(("box", (0, 0, base_h + body_h * 0.5),
                  (2 * half_x, 2 * half_y, body_h), "concrete"))
    for offset in (-half_x + 0.012, half_x - 0.012):
        parts.append(("box", (offset, 0, base_h + body_h * 0.5),
                      (0.024, 2 * half_y + 0.008, body_h), "stone"))
    band(parts, base_h + body_h + 0.010, half_x, half_y, 0.020, "concrete_dark")
    facade_rows(parts, half_x - 0.02, half_y,
                [base_h + FLOOR_HEIGHT * (f + 0.55) for f in range(10)], 3, 4)
    parts.append(("box", (0.04, -0.04, base_h + body_h + 0.035),
                  (0.08, 0.06, 0.03), "metal_dark"))
    return parts


def res_conjunto():
    """Conjunto habitacional: lâmina alta com torre de circulação ao fundo."""
    parts = []
    half_x, half_y = 0.20, 0.12
    body_h = FLOOR_HEIGHT * 11.0
    parts.append(("box", (0, 0, 0.012), (0.44, 0.28, 0.024), "concrete_dark"))
    parts.append(("box", (0, 0, 0.03 + body_h * 0.5),
                  (2 * half_x, 2 * half_y, body_h), "plaster_cool"))
    facade_rows(parts, half_x, half_y,
                [0.03 + FLOOR_HEIGHT * (f + 0.55) for f in range(11)], 5, 2)
    band(parts, 0.03 + body_h + 0.008, half_x, half_y, 0.014, "concrete")
    core_h = body_h + 0.10
    parts.append(("box", (0.06, 0.10, 0.03 + core_h * 0.5),
                  (0.10, 0.10, core_h), "concrete_dark"))
    parts.append(("box", (0.06, 0.10, 0.03 + core_h + 0.015),
                  (0.11, 0.11, 0.03), "trim"))
    return parts


def com_loja():
    """Prédio misto de quatro pavimentos: loja no térreo, moradia em cima."""
    parts = []
    half_x, half_y = 0.205, 0.18
    parts.append(("box", (0, 0, 0.012), (0.43, 0.38, 0.024), "stone"))
    ground_h = 0.075
    upper_h = FLOOR_HEIGHT * 3.0
    parts.append(("box", (0, 0, 0.024 + ground_h * 0.5),
                  (2 * half_x, 2 * half_y, ground_h), "stone_dark"))
    parts.append(("box", (0, 0, 0.024 + ground_h + upper_h * 0.5),
                  (2 * half_x, 2 * half_y, upper_h), "plaster_warm"))
    parts.append(("box", (0.0, -half_y - 0.006, 0.024 + ground_h * 0.55),
                  (2 * half_x - 0.04, 0.012, 0.052), "glass_warm"))
    parts.append(("box", (0.0, -half_y - 0.02, 0.024 + ground_h),
                  (2 * half_x - 0.03, 0.04, 0.012), "metal_dark"))
    band(parts, 0.024 + ground_h + 0.004, half_x, half_y, 0.010, "trim")
    facade_rows(parts, half_x, half_y,
                [0.024 + ground_h + FLOOR_HEIGHT * (f + 0.55) for f in range(3)],
                4, 3)
    band(parts, 0.024 + ground_h + upper_h + 0.006, half_x, half_y, 0.014, "concrete")
    return parts


def com_escritorio_medio():
    """Escritório de nove pavimentos com base em pedra e faixas de vidro."""
    parts = []
    half_x, half_y = 0.195, 0.195
    base_h = 0.06
    body_h = FLOOR_HEIGHT * 8.0
    parts.append(("box", (0, 0, 0.012), (0.42, 0.42, 0.024), "stone"))
    parts.append(("box", (0, 0, 0.024 + base_h * 0.5),
                  (2 * half_x + 0.01, 2 * half_y + 0.01, base_h), "stone_dark"))
    parts.append(("box", (0, 0, 0.024 + base_h + body_h * 0.5),
                  (2 * half_x, 2 * half_y, body_h), "concrete"))
    for floor in range(8):
        z = 0.024 + base_h + FLOOR_HEIGHT * (floor + 0.35)
        parts.append(("box", (0.0, 0.0, z),
                      (2 * half_x + 0.006, 2 * half_y + 0.006, 0.030), "glass"))
    band(parts, 0.024 + base_h + body_h + 0.010, half_x, half_y, 0.018, "concrete_dark")
    parts.append(("box", (0.05, 0.05, 0.024 + base_h + body_h + 0.032),
                  (0.07, 0.06, 0.028), "metal_dark"))
    return parts


def ind_galpao():
    """Galpão logístico com docas de carga e telhado de pouca inclinação."""
    parts = []
    half_x, half_y = 0.42, 0.25
    parts.append(("box", (0, 0, 0.006), (0.92, 0.58, 0.012), "paving"))
    body_h = 0.105
    parts.append(("box", (0, 0.02, 0.012 + body_h * 0.5),
                  (2 * half_x, 2 * half_y, body_h), "concrete_dark"))
    parts.append(("wedge", (0, 0.02, 0.012 + body_h), 2 * half_x, 2 * half_y,
                  0.045, "roof_slate", 0.010))
    for index in range(3):
        x = -0.22 + index * 0.22
        parts.append(("box", (x, -half_y + 0.02 - 0.006, 0.012 + 0.040),
                      (0.13, 0.014, 0.070), "metal_dark"))
    parts.append(("box", (0.0, -half_y - 0.02, 0.012),
                  (2 * half_x + 0.06, 0.06, 0.012), "asphalt"))
    return parts


def ind_fabrica():
    """Fábrica de tijolo com dentes de serra, duas chaminés e caixa d'água."""
    parts = []
    half_x, half_y = 0.30, 0.23
    parts.append(("box", (0, 0, 0.008), (0.64, 0.50, 0.016), "paving"))
    body_h = 0.18
    parts.append(("box", (0, 0, 0.016 + body_h * 0.5),
                  (2 * half_x, 2 * half_y, body_h), "brick"))
    band(parts, 0.016 + body_h + 0.004, half_x, half_y, 0.010, "concrete_dark")
    tooth_w = (2 * half_x) / 3.0
    for index in range(3):
        x = -half_x + tooth_w * (index + 0.5)
        parts.append(("wedge", (x, 0, 0.016 + body_h), tooth_w * 0.98,
                      2 * half_y, 0.055, "roof_slate", 0.0))
    parts.append(("cylinder", (-0.18, 0.08, 0.016 + body_h + 0.20), 0.028, 0.40,
                  "brick_dark"))
    parts.append(("cylinder", (0.02, 0.10, 0.016 + body_h + 0.16), 0.024, 0.34,
                  "brick_dark"))
    parts.append(("cylinder", (0.20, -0.10, 0.016 + body_h + 0.055), 0.055, 0.11,
                  "metal"))
    parts.append(("box", (0.0, -half_y - 0.005, 0.016 + 0.05),
                  (0.16, 0.012, 0.10), "metal_dark"))
    return parts


def com_torre():
    """Torre de escritórios de vinte e dois pavimentos, com pódio e recuos."""
    parts = []
    parts.append(("box", (0, 0, 0.015), (0.48, 0.48, 0.030), "paving"))
    parts.append(("box", (0, 0, 0.030 + 0.05), (0.46, 0.46, 0.10), "stone"))
    shaft_h = FLOOR_HEIGHT * 22.0
    shaft_z = 0.13
    parts.append(("box", (0, 0, shaft_z + shaft_h * 0.5),
                  (0.30, 0.30, shaft_h), "concrete"))
    for floor in range(22):
        z = shaft_z + FLOOR_HEIGHT * (floor + 0.35)
        parts.append(("box", (0.0, 0.0, z),
                      (0.306, 0.306, 0.032), "glass"))
    setback_h = 0.14
    parts.append(("taper", (0, 0, shaft_z + shaft_h),
                  (0.30, 0.30), (0.22, 0.22), setback_h, "concrete_dark"))
    parts.append(("box", (0, 0, shaft_z + shaft_h + setback_h + 0.015),
                  (0.22, 0.22, 0.03), "metal_dark"))
    parts.append(("cylinder", (0, 0, shaft_z + shaft_h + setback_h + 0.10),
                  0.004, 0.14, "metal"))
    return parts


def marco_publico():
    """Edifício cívico: pódio, colunata, cúpula e lanterna."""
    parts = []
    parts.append(("box", (0, 0, 0.010), (0.46, 0.40, 0.020), "stone"))
    parts.append(("box", (0, 0, 0.030), (0.42, 0.36, 0.020), "paving"))
    hall_h = 0.16
    parts.append(("box", (0, 0, 0.040 + hall_h * 0.5),
                  (0.32, 0.26, hall_h), "stone"))
    for index in range(6):
        x = -0.13 + index * 0.052
        parts.append(("cylinder", (x, -0.135, 0.040 + 0.075), 0.010, 0.15, "stone"))
    parts.append(("box", (0, 0, 0.040 + hall_h + 0.015),
                  (0.36, 0.30, 0.030), "stone_dark"))
    parts.append(("cylinder", (0, 0, 0.040 + hall_h + 0.065), 0.085, 0.06, "stone"))
    parts.append(("pyramid", (0, 0, 0.040 + hall_h + 0.095), 0.16, 0.16,
                  0.075, "roof_slate", 0.0))
    parts.append(("cylinder", (0, 0, 0.040 + hall_h + 0.185), 0.018, 0.05, "metal"))
    return parts


# ---------------------------------------------------------------- indústria

def ind_siderurgia():
    """Complexo siderúrgico: alto-forno, hall, chaminés e pátio de minério."""
    parts = []
    parts.append(("box", (0, 0, 0.010), (1.24, 0.84, 0.020), "paving"))
    furnace_h = 0.50
    parts.append(("cylinder", (-0.30, 0.06, 0.020 + furnace_h * 0.5),
                  0.105, furnace_h, "brick_dark"))
    parts.append(("cylinder", (-0.30, 0.06, 0.545), 0.115, 0.05, "soot"))
    parts.append(("cone", (-0.30, 0.06, 0.625), 0.075, 0.10, "soot"))
    hall_h = 0.22
    parts.append(("box", (0.10, 0.10, 0.020 + hall_h * 0.5),
                  (0.42, 0.30, hall_h), "brick"))
    parts.append(("wedge", (0.10, 0.10, 0.020 + hall_h), 0.44, 0.32,
                  0.055, "roof_slate", 0.010))
    parts.append(("cylinder", (0.34, 0.24, 0.32), 0.028, 0.60, "brick_dark"))
    parts.append(("cylinder", (0.42, 0.16, 0.26), 0.022, 0.48, "brick_dark"))
    parts.append(("box", (-0.44, -0.26, 0.035), (0.40, 0.28, 0.030), "soot"))
    parts.append(("pyramid", (-0.44, -0.26, 0.050), 0.32, 0.22, 0.085, "soot", 0.0))
    parts.append(("box", (-0.12, -0.12, 0.190), (0.50, 0.030, 0.030), "steel_light"))
    parts.append(("box", (0.10, -0.16, 0.055), (0.46, 0.020, 0.070), "steel_light"))
    return parts


def ind_manufatura():
    """Parque de manufatura: dois galpões serrilhados, anexo e docas."""
    parts = []
    parts.append(("box", (0, 0, 0.010), (1.00, 0.72, 0.020), "paving"))
    hall1_h = 0.13
    parts.append(("box", (-0.18, 0.02, 0.020 + hall1_h * 0.5),
                  (0.44, 0.34, hall1_h), "concrete_dark"))
    parts.append(("wedge", (-0.18, 0.02, 0.020 + hall1_h), 0.46, 0.36,
                  0.040, "roof_slate", 0.0))
    hall2_h = 0.11
    parts.append(("box", (0.24, 0.02, 0.020 + hall2_h * 0.5),
                  (0.36, 0.30, hall2_h), "metal"))
    parts.append(("wedge", (0.24, 0.02, 0.020 + hall2_h), 0.38, 0.32,
                  0.035, "roof_slate", 0.0))
    parts.append(("box", (0.02, -0.26, 0.050), (0.20, 0.12, 0.060), "brick"))
    for index in range(3):
        parts.append(("box", (-0.26 + index * 0.20, -0.352, 0.055),
                      (0.12, 0.012, 0.070), "metal_dark"))
    return parts


def ind_refinaria():
    """Refinaria: colunas de destilação, racks de tubulação e tanques."""
    parts = []
    parts.append(("box", (0, 0, 0.010), (1.00, 0.80, 0.020), "paving"))
    for index, (x, height) in enumerate(((-0.22, 0.42), (0.0, 0.52), (0.22, 0.36))):
        parts.append(("cylinder", (x, 0.06, 0.020 + height * 0.5),
                      0.034, height, "steel_light"))
        parts.append(("cylinder", (x, 0.06, 0.020 + height + 0.012),
                      0.040, 0.024, "metal_dark"))
    for z in (0.110, 0.155):
        parts.append(("box", (0.0, -0.04, z), (0.72, 0.020, 0.024), "steel_light"))
    for x in (-0.34, -0.14, 0.14, 0.34):
        parts.append(("cylinder", (x, -0.24, 0.075), 0.070, 0.110, "tank_shell"))
        parts.append(("cylinder", (x, -0.24, 0.132), 0.074, 0.012, "metal_dark"))
    parts.append(("cylinder", (0.42, 0.24, 0.32), 0.012, 0.58, "rust"))
    parts.append(("box", (0.0, -0.06, 0.050), (0.16, 0.10, 0.060), "concrete"))
    return parts


def ind_energia():
    """Central térmica: hall de turbinas, torres de arrefecimento e chaminé."""
    parts = []
    parts.append(("box", (0, 0, 0.010), (1.10, 0.80, 0.020), "paving"))
    hall_h = 0.18
    parts.append(("box", (-0.16, 0.00, 0.020 + hall_h * 0.5),
                  (0.50, 0.34, hall_h), "concrete"))
    parts.append(("wedge", (-0.16, 0.00, 0.020 + hall_h), 0.52, 0.36,
                  0.050, "roof_slate", 0.0))
    parts.append(("cylinder", (-0.42, -0.18, 0.020 + 0.15), 0.105, 0.30, "tank_shell"))
    parts.append(("cylinder", (-0.42, -0.18, 0.326), 0.112, 0.016, "metal_dark"))
    parts.append(("cylinder", (-0.24, -0.26, 0.020 + 0.12), 0.082, 0.24, "tank_shell"))
    parts.append(("cylinder", (-0.24, -0.26, 0.264), 0.088, 0.014, "metal_dark"))
    parts.append(("cylinder", (0.30, 0.24, 0.31), 0.034, 0.58, "brick_dark"))
    parts.append(("box", (0.34, -0.20, 0.045), (0.20, 0.14, 0.055), "steel_light"))
    parts.append(("box", (0.34, -0.20, 0.085), (0.22, 0.16, 0.020), "metal_dark"))
    return parts


def ind_armazem():
    """Armazenagem: galpão, silos e pátio de carga."""
    parts = []
    parts.append(("box", (0, 0, 0.010), (1.00, 0.60, 0.020), "paving"))
    hall_h = 0.14
    parts.append(("box", (-0.14, 0.00, 0.020 + hall_h * 0.5),
                  (0.52, 0.34, hall_h), "metal"))
    parts.append(("wedge", (-0.14, 0.00, 0.020 + hall_h), 0.54, 0.36,
                  0.040, "roof_slate", 0.0))
    for x, y, height in ((0.30, 0.10, 0.26), (0.30, -0.08, 0.26), (0.18, -0.20, 0.20)):
        parts.append(("cylinder", (x, y, 0.020 + height * 0.5), 0.058, height,
                      "tank_shell"))
        parts.append(("cone", (x, y, 0.020 + height + 0.028), 0.062, 0.056,
                      "metal_dark"))
    for index in range(2):
        parts.append(("box", (-0.30 + index * 0.22, -0.302, 0.045),
                      (0.14, 0.012, 0.050), "metal_dark"))
    return parts


def ind_aux_chamine():
    """Chaminés auxiliares de um complexo industrial."""
    parts = []
    parts.append(("box", (0, 0, 0.010), (0.36, 0.28, 0.020), "paving"))
    parts.append(("cylinder", (-0.06, 0.0, 0.26), 0.026, 0.48, "brick_dark"))
    parts.append(("cylinder", (0.09, 0.04, 0.21), 0.020, 0.38, "brick_dark"))
    return parts


def ind_aux_tanques():
    """Bateria de tanques auxiliar."""
    parts = []
    parts.append(("box", (0, 0, 0.010), (0.44, 0.36, 0.020), "paving"))
    for x in (-0.10, 0.10):
        for y in (-0.08, 0.08):
            parts.append(("cylinder", (x, y, 0.065), 0.062, 0.090, "tank_shell"))
            parts.append(("cylinder", (x, y, 0.116), 0.066, 0.010, "metal_dark"))
    parts.append(("box", (0.0, -0.17, 0.045), (0.36, 0.016, 0.020), "steel_light"))
    return parts


def ind_aux_galpao():
    """Galpão auxiliar compacto."""
    parts = []
    parts.append(("box", (0, 0, 0.010), (0.44, 0.34, 0.020), "paving"))
    parts.append(("box", (0, 0.0, 0.070), (0.36, 0.26, 0.100), "metal"))
    parts.append(("wedge", (0, 0.0, 0.120), 0.38, 0.28, 0.035, "roof_slate", 0.0))
    parts.append(("box", (0.0, -0.132, 0.045), (0.12, 0.012, 0.060), "metal_dark"))
    return parts


def ind_aux_patio():
    """Pátio de estocagem a céu aberto com pilhas de material."""
    parts = []
    parts.append(("box", (0, 0, 0.010), (0.44, 0.36, 0.020), "paving"))
    parts.append(("pyramid", (-0.09, 0.0, 0.020), 0.18, 0.22, 0.075, "soot", 0.0))
    parts.append(("pyramid", (0.10, 0.06, 0.020), 0.14, 0.18, 0.060, "rust", 0.0))
    parts.append(("box", (0.12, -0.14, 0.045), (0.12, 0.08, 0.030), "metal_dark"))
    return parts


MODULES = [
    ("res_casa_urbana", "residential", "residential", 1, "Sobrado geminado de 3 pavimentos", res_casa_urbana),
    ("res_vila", "residential", "residential", 1, "Casa isolada de 2 pavimentos", res_vila),
    ("res_sobrado", "residential", "residential", 2, "Quadra residencial de 4 pavimentos", res_sobrado),
    ("res_predio_6", "residential", "residential", 3, "Prédio de apartamentos de 7 pavimentos", res_predio_6),
    ("res_predio_10", "residential", "residential", 4, "Lâmina residencial de 10 pavimentos", res_predio_10),
    ("res_conjunto", "residential", "residential", 4, "Conjunto habitacional com torre de circulação", res_conjunto),
    ("com_loja", "commercial", "commercial", 2, "Prédio misto com loja no térreo", com_loja),
    ("com_escritorio_medio", "commercial", "commercial", 3, "Escritório de 9 pavimentos", com_escritorio_medio),
    ("ind_galpao", "industrial", "industrial", 2, "Galpão logístico com docas", ind_galpao),
    ("ind_fabrica", "industrial", "industrial", 3, "Fábrica de tijolo com chaminés", ind_fabrica),
    ("com_torre", "commercial", "commercial", 5, "Torre de escritórios de 22 pavimentos", com_torre),
    ("marco_publico", "public", "public", 1, "Edifício cívico com cúpula", marco_publico),
    ("ind_siderurgia", "industrial", "industrial", 3, "Complexo siderúrgico com alto-forno", ind_siderurgia),
    ("ind_manufatura", "industrial", "industrial", 2, "Parque de manufatura com galpões", ind_manufatura),
    ("ind_refinaria", "industrial", "industrial", 3, "Refinaria com colunas e tanques", ind_refinaria),
    ("ind_energia", "industrial", "industrial", 3, "Central térmica com torres de arrefecimento", ind_energia),
    ("ind_armazem", "industrial", "industrial", 2, "Armazenagem com galpão e silos", ind_armazem),
    ("ind_aux_chamine", "industrial", "industrial", 1, "Chaminés auxiliares", ind_aux_chamine),
    ("ind_aux_tanques", "industrial", "industrial", 1, "Bateria de tanques auxiliar", ind_aux_tanques),
    ("ind_aux_galpao", "industrial", "industrial", 1, "Galpão auxiliar", ind_aux_galpao),
    ("ind_aux_patio", "industrial", "industrial", 1, "Pátio de estocagem", ind_aux_patio),
]

# A posição do material depende do tipo da peça; os parâmetros opcionais vêm
# depois dele (overhang, segments).
MATERIAL_INDEX = {"box": 3, "cylinder": 4, "cone": 4, "wedge": 5, "pyramid": 5, "taper": 5}

# Part types that are not boxes carry extra scalar arguments.
PART_BUILDERS = {
    "box": lambda bm, part, index: add_box(bm, part[1], part[2], index),
    "cylinder": lambda bm, part, index: add_cylinder(
        bm, part[1], part[2], part[3], index, segments=part[5] if len(part) > 5 else 12),
    "cone": lambda bm, part, index: add_cone(
        bm, part[1], part[2], part[3], index, segments=part[5] if len(part) > 5 else 12),
    "wedge": lambda bm, part, index: add_wedge(
        bm, part[1], part[2], part[3], part[4], index,
        part[6] if len(part) > 6 else 0.0),
    "pyramid": lambda bm, part, index: add_pyramid(
        bm, part[1], part[2], part[3], part[4], index,
        part[6] if len(part) > 6 else 0.0),
    "taper": lambda bm, part, index: add_taper(
        bm, part[1], part[2], part[3], part[4], index),
}


def build_mesh(module_id, parts):
    """Cria o mesh único do módulo e devolve (object, footprint, altura)."""
    mat_names = []
    for part in parts:
        name = part[MATERIAL_INDEX[part[0]]]
        if name not in mat_names:
            mat_names.append(name)

    mesh = bpy.data.meshes.new(module_id)
    for name in mat_names:
        mesh.materials.append(material(name))

    bm = bmesh.new()
    for part in parts:
        index = mat_names.index(part[MATERIAL_INDEX[part[0]]])
        PART_BUILDERS[part[0]](bm, part, index)

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
    footprint = [round(high.x - low.x, 3), round(high.y - low.y, 3)]
    return obj, footprint, round(high.z - low.z, 3)
