"""Mostra o centro de nos nomeados do GLB para descobrir a direcao frontal."""
import json
import struct
import sys

import numpy as np


def load_glb(path):
    with open(path, "rb") as handle:
        struct.unpack("<4sII", handle.read(12))
        chunk_len, _ = struct.unpack("<II", handle.read(8))
        return json.loads(handle.read(chunk_len).decode("utf-8"))


def node_matrix(node):
    if "matrix" in node:
        return np.array(node["matrix"], dtype=np.float64).reshape(4, 4).T
    matrix = np.eye(4)
    if "scale" in node:
        matrix = np.diag(list(node["scale"]) + [1.0]) @ matrix
    if "rotation" in node:
        x, y, z, w = node["rotation"]
        rot = np.array([
            [1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w), 0],
            [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w), 0],
            [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y), 0],
            [0, 0, 0, 1],
        ])
        matrix = rot @ matrix
    if "translation" in node:
        trans = np.eye(4)
        trans[:3, 3] = node["translation"]
        matrix = trans @ matrix
    return matrix


def walk(gltf, index, parent, out):
    nodes = gltf.get("nodes", [])
    node = nodes[index]
    world = parent @ node_matrix(node)

    if "mesh" in node:
        points = []
        for primitive in gltf["meshes"][node["mesh"]]["primitives"]:
            accessor = gltf["accessors"][primitive["attributes"]["POSITION"]]
            low = np.array(accessor["min"], dtype=np.float64)
            high = np.array(accessor["max"], dtype=np.float64)
            for corner in range(8):
                point = np.array([
                    high[0] if corner & 1 else low[0],
                    high[1] if corner & 2 else low[1],
                    high[2] if corner & 4 else low[2],
                    1.0,
                ])
                points.append((world @ point)[:3])
        if points:
            center = np.array(points).mean(axis=0)
            out.append((node.get("name", "?"), center))

    for child in node.get("children", []):
        walk(gltf, child, world, out)


def main() -> int:
    keys = ("nose", "cockpit", "grille", "head", "hood", "bow", "fin", "bridge", "mudflap", "hitch")
    for path in sys.argv[1:]:
        gltf = load_glb(path)
        out = []
        scene = gltf.get("scenes", [{}])[gltf.get("scene", 0)]
        for root in scene.get("nodes", []):
            walk(gltf, root, np.eye(4), out)

        print(f"=== {path}")
        for name, center in out:
            if any(key in name for key in keys):
                print(f"  {name:22s} X={center[0]:+7.3f}  Y={center[1]:+6.3f}  Z={center[2]:+6.3f}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
