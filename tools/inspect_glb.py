"""Mede o AABB e os eixos de modelos GLB (para descobrir a direcao frontal).

Uso: python inspect_glb.py assets/models/truck.glb ...
"""
import json
import os
import struct
import sys

import numpy as np


def load_glb(path):
    with open(path, "rb") as handle:
        magic, version, _length = struct.unpack("<4sII", handle.read(12))
        if magic != b"glTF":
            raise ValueError(f"{path}: nao e GLB")
        chunk_len, chunk_type = struct.unpack("<II", handle.read(8))
        if chunk_type != 0x4E4F534A:
            raise ValueError(f"{path}: primeiro chunk nao e JSON")
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


def collect_points(gltf, node_index, parent, out):
    nodes = gltf.get("nodes", [])
    node = nodes[node_index]
    world = parent @ node_matrix(node)

    if "mesh" in node:
        mesh = gltf["meshes"][node["mesh"]]
        for primitive in mesh["primitives"]:
            accessor_index = primitive.get("attributes", {}).get("POSITION")
            if accessor_index is None:
                continue
            accessor = gltf["accessors"][accessor_index]
            low = np.array(accessor["min"], dtype=np.float64)
            high = np.array(accessor["max"], dtype=np.float64)
            for corner in range(8):
                point = np.array([
                    high[0] if corner & 1 else low[0],
                    high[1] if corner & 2 else low[1],
                    high[2] if corner & 4 else low[2],
                    1.0,
                ])
                out.append((world @ point)[:3])

    for child in node.get("children", []):
        collect_points(gltf, child, world, out)


def main() -> int:
    for path in sys.argv[1:]:
        gltf = load_glb(path)
        points = []
        scene = gltf.get("scenes", [{}])[gltf.get("scene", 0)]
        for root in scene.get("nodes", []):
            collect_points(gltf, root, np.eye(4), points)

        if not points:
            print(f"{os.path.basename(path)}: sem malhas")
            continue

        arr = np.array(points)
        low = arr.min(axis=0)
        high = arr.max(axis=0)
        size = high - low
        longest = int(np.argmax(size))
        print(
            f"{os.path.basename(path):12s} size=(X:{size[0]:6.2f} Y:{size[1]:6.2f} Z:{size[2]:6.2f}) "
            f"eixo mais longo={'XYZ'[longest]}  centro=({arr.mean(axis=0)[0]:.2f},{arr.mean(axis=0)[1]:.2f},{arr.mean(axis=0)[2]:.2f})"
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
