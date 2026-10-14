#!/usr/bin/env python3
"""Finds the lamps baked into the field terrain models and writes their positions
as a C table, for the PC port's night lighting (src/overlay005/field_point_lights.c).

Lamps aren't map props, they're part of each map's terrain model, but they use
their own materials (c1_lamp01, dun_light, ...). This decodes the display lists
of the shapes drawn with those materials and groups their vertices into lamps.

Usage: extract_map_lamps.py res/field/maps/data include/data/field_map_lamps.h
"""

import glob
import os
import re
import struct
import sys

# Materials of the glowing part of a street lamp. Other light-ish materials
# (c1_lamp01, c5_light, dun_light, ...) aren't confirmed to be lamps yet
LAMP_MATERIALS = {
    "c1_o02b",     # Jubilife City lamp heads
    "c2_light_a",  # Canalave City lamp glass
}

# Vertices horizontally closer than this (in map units, a tile is 16) belong to
# the same lamp
CLUSTER_DISTANCE = 12.0

LAND_DATA_HEADER_SIZE = 16


def read_dict(data, offset):
    """Reads an NNSG3dResDict, returning [(name, entry offset)]"""
    num_entries = data[offset + 1]
    ofs_entry = struct.unpack_from("<H", data, offset + 6)[0]
    entry = offset + ofs_entry
    size_unit, ofs_name = struct.unpack_from("<HH", data, entry)
    entries = []
    for i in range(num_entries):
        name_ofs = entry + ofs_name + i * 16
        name = data[name_ofs:name_ofs + 16].split(b"\0")[0].decode("ascii", "replace")
        entries.append((name, entry + 4 + i * size_unit))
    return entries


def material_for_shapes(data, sbc):
    """Walks the model's render bytecode and returns {shape index: material index}"""
    lengths = {0x00: 1, 0x01: 1, 0x02: 3, 0x03: 2, 0x05: 2, 0x0A: 9, 0x0B: 1, 0x0C: 3, 0x0D: 3}
    shape_mats = {}
    cur_mat = None
    i = sbc
    while True:
        op = data[i]
        cmd = op & 0x1F
        flags = op >> 5
        if cmd == 0x01:
            break
        if cmd == 0x04:
            cur_mat = data[i + 1]
            i += 2
        elif cmd == 0x05:
            shape_mats[data[i + 1]] = cur_mat
            i += 2
        elif cmd == 0x06:
            i += 4 + (1 if flags & 1 else 0) + (1 if flags & 2 else 0)
        elif cmd in (0x07, 0x08):
            i += 2 + (1 if flags & 1 else 0) + (1 if flags & 2 else 0)
        elif cmd == 0x09:
            i += 3 + 3 * data[i + 2]
        elif cmd in lengths:
            i += lengths[cmd]
        else:
            raise ValueError(f"unknown SBC command {op:#x}")
    return shape_mats


def signed(value, bits):
    return value - (1 << bits) if value & (1 << (bits - 1)) else value


# Parameter word counts of the geometry commands
GX_PARAM_COUNTS = {
    0x00: 0, 0x10: 1, 0x11: 0, 0x12: 1, 0x13: 1, 0x14: 1, 0x15: 0, 0x16: 16, 0x17: 12,
    0x18: 16, 0x19: 12, 0x1A: 9, 0x1B: 3, 0x1C: 3, 0x20: 1, 0x21: 1, 0x22: 1, 0x23: 2,
    0x24: 1, 0x25: 1, 0x26: 1, 0x27: 1, 0x28: 1, 0x29: 1, 0x2A: 1, 0x2B: 1, 0x30: 1,
    0x31: 1, 0x32: 1, 0x33: 1, 0x34: 32, 0x40: 1, 0x41: 0, 0x50: 1, 0x60: 1, 0x70: 3,
    0x71: 2, 0x72: 1,
}


def decode_vertices(data, start, size):
    """Decodes a packed display list, returning vertex positions in fx16 units"""
    verts = []
    x = y = z = 0
    i = start
    end = start + size
    while i < end:
        ops = data[i:i + 4]
        i += 4
        for op in ops:
            count = GX_PARAM_COUNTS.get(op)
            if count is None:
                raise ValueError(f"unknown GX command {op:#x}")
            params = struct.unpack_from(f"<{count}I", data, i)
            i += 4 * count
            if op == 0x23:
                x = signed(params[0] & 0xFFFF, 16)
                y = signed(params[0] >> 16, 16)
                z = signed(params[1] & 0xFFFF, 16)
            elif op == 0x24:
                x = signed(params[0] & 0x3FF, 10) << 6
                y = signed((params[0] >> 10) & 0x3FF, 10) << 6
                z = signed((params[0] >> 20) & 0x3FF, 10) << 6
            elif op == 0x25:
                x = signed(params[0] & 0xFFFF, 16)
                y = signed(params[0] >> 16, 16)
            elif op == 0x26:
                x = signed(params[0] & 0xFFFF, 16)
                z = signed(params[0] >> 16, 16)
            elif op == 0x27:
                y = signed(params[0] & 0xFFFF, 16)
                z = signed(params[0] >> 16, 16)
            elif op == 0x28:
                x += signed(params[0] & 0x3FF, 10)
                y += signed((params[0] >> 10) & 0x3FF, 10)
                z += signed((params[0] >> 20) & 0x3FF, 10)
            else:
                continue
            verts.append((x, y, z))
    return verts


def find_lamps(model_data):
    """Returns [(x, y, z)] in map units for each lamp in a terrain model"""
    if model_data[:4] != b"BMD0":
        return []
    mdl0 = struct.unpack_from("<I", model_data, 0x10)[0]
    models = read_dict(model_data, mdl0 + 8)
    _, model_entry = models[0]
    model = mdl0 + struct.unpack_from("<I", model_data, model_entry)[0]
    ofs_sbc, ofs_mat, ofs_shp = struct.unpack_from("<III", model_data, model + 4)
    pos_scale = struct.unpack_from("<i", model_data, model + 20 + 8)[0] / 4096.0

    materials = [name for name, _ in read_dict(model_data, model + ofs_mat + 4)]
    shapes = read_dict(model_data, model + ofs_shp)
    shape_mats = material_for_shapes(model_data, model + ofs_sbc)

    points = []
    for shape_index, (_, shape_entry) in enumerate(shapes):
        mat = shape_mats.get(shape_index)
        if mat is None:
            continue
        # Material names carry a lightmap suffix, e.g. c1_lamp01_lm2
        name = re.sub(r"_lm\d+$", "", materials[mat])
        if name not in LAMP_MATERIALS:
            continue
        shape = model + ofs_shp + struct.unpack_from("<I", model_data, shape_entry)[0]
        ofs_dl, size_dl = struct.unpack_from("<II", model_data, shape + 8)
        for vx, vy, vz in decode_vertices(model_data, shape + ofs_dl, size_dl):
            points.append((vx / 4096.0 * pos_scale, vy / 4096.0 * pos_scale, vz / 4096.0 * pos_scale))

    lamps = []
    for cluster in cluster_points(points):
        cx = sum(p[0] for p in cluster) / len(cluster)
        cy = sum(p[1] for p in cluster) / len(cluster)
        cz = sum(p[2] for p in cluster) / len(cluster)
        lamps.append((cx, cy, cz))
    return lamps


def cluster_points(points):
    clusters = []
    for p in points:
        near = [c for c in clusters if any(dist_sq(p, q) <= CLUSTER_DISTANCE ** 2 for q in c)]
        merged = [p]
        for c in near:
            merged.extend(c)
            clusters.remove(c)
        clusters.append(merged)
    return clusters


def dist_sq(a, b):
    return (a[0] - b[0]) ** 2 + (a[2] - b[2]) ** 2


def main():
    map_dir, out_path = sys.argv[1], sys.argv[2]
    rows = []
    for path in sorted(glob.glob(os.path.join(map_dir, "map_data_*.bin"))):
        map_id = int(re.search(r"(\d+)\.bin$", path).group(1))
        data = open(path, "rb").read()
        terrain_size, props_size, model_size, _ = struct.unpack_from("<4I", data, 0)
        model_start = LAND_DATA_HEADER_SIZE + terrain_size + props_size
        for x, y, z in find_lamps(data[model_start:model_start + model_size]):
            rows.append((map_id, x, y, z))

    with open(out_path, "w") as out:
        out.write("// Generated by tools/scripts/extract_map_lamps.py, do not edit\n\n")
        out.write("static const FieldMapLamp sFieldMapLamps[] = {\n")
        for map_id, x, y, z in rows:
            out.write(f"    {{ {map_id}, {{ FX32_CONST({x:.2f}), FX32_CONST({y:.2f}), FX32_CONST({z:.2f}) }} }},\n")
        out.write("};\n")

    print(f"{len(rows)} lamps in {len({r[0] for r in rows})} maps")


if __name__ == "__main__":
    main()
