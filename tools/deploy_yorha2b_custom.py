#!/usr/bin/env python3
"""Deploy custom YoRHa 2B (Eve) edit with longer hair and removed blindfold.

Preserves 100% authentic Frostbite skinning streams (Streams 1..7, bone palettes,
and untouched body/leg positions) so vertices never explode, while applying the
shape key hair deltas and collapsing blindfold geometry across both LOD 0 and LOD 1.
"""
import hashlib
import os
from pathlib import Path
import struct
import subprocess
import sys
import numpy as np
from scipy.spatial import cKDTree

TOOLS_DIR = Path(__file__).resolve().parent
REPO_ROOT = TOOLS_DIR.parent
if str(TOOLS_DIR) not in sys.path:
    sys.path.insert(0, str(TOOLS_DIR))

from frostbite_gltf import patch_reskate_mod_directory, wrap_uncompressed_cas

MOD_PATHS = [
    Path("/run/media/zephyrus/storage/Games/ReSkate/ReSkate-2.0.7/Mods/UniversalJiggle-YoRHa2B_Eve_v1"),
    Path("/home/zephyrus/.local/share/Steam/steamapps/common/Skate/Mods/UniversalJiggle-YoRHa2B_Eve_v1"),
]


def extract_hair_deltas_from_blend(blend_path: Path) -> tuple[np.ndarray, np.ndarray]:
    """Run Blender to extract basis vertex coordinates and hair shape key displacement vectors."""
    tmp_out = Path("/tmp/blend_hair_deltas.bin")
    py_script = f"""
import bpy, struct
obj = bpy.data.objects['yorha2b_lod0']
basis = obj.data.shape_keys.key_blocks['Basis'].data
s_long = obj.data.shape_keys.key_blocks['Longer_Hair']
s_back = obj.data.shape_keys.key_blocks['Back_Hair_Longer']
w_long = s_long.value
w_back = s_back.value

with open('{tmp_out}', 'wb') as f:
    for i in range(len(basis)):
        bx, by, bz = basis[i].co
        # glTF / Frostbite coordinates: (x, z, -y)
        b_fb_x, b_fb_y, b_fb_z = bx, bz, -by
        
        d_l = (s_long.data[i].co - basis[i].co) * w_long
        d_b = (s_back.data[i].co - basis[i].co) * w_back
        ex = bx + d_l.x + d_b.x
        ey = by + d_l.y + d_b.y
        ez = bz + d_l.z + d_b.z
        e_fb_x, e_fb_y, e_fb_z = ex, ez, -ey
        
        dx = e_fb_x - b_fb_x
        dy = e_fb_y - b_fb_y
        dz = e_fb_z - b_fb_z
        f.write(struct.pack('<6f', b_fb_x, b_fb_y, b_fb_z, dx, dy, dz))
"""
    subprocess.run(["blender", "-b", str(blend_path), "--python-expr", py_script], check=True)
    data = np.fromfile(tmp_out, dtype=np.float32).reshape(-1, 6)
    basis_pos = data[:, :3]
    deltas = data[:, 3:]
    return basis_pos, deltas


def process_lod0(
    res_bytes: bytes,
    lod0_raw_bytes: bytes,
    basis_pos: np.ndarray,
    hair_deltas: np.ndarray,
) -> bytes:
    """Displace hair vertices and collapse blindfold on authentic LOD 0 chunk."""
    lod0 = bytearray(lod0_raw_bytes)
    assert len(lod0) == 4207184, f"LOD 0 size expected 4207184, got {len(lod0)}"
    tree = cKDTree(basis_pos)

    sec_off0 = 0x550
    _, _, ib_sz0, vb_sz0 = struct.unpack_from("<4I", res_bytes, 0x00D0 + 0x60)

    # Sub 1 (Bottom): remove blindfold
    sb1 = sec_off0 + 1 * 0x180
    prim1, start1, voff1, vcnt1 = struct.unpack_from("<4I", res_bytes, sb1 + 0x30)
    pos1 = np.frombuffer(lod0, dtype="<f4", count=vcnt1 * 3, offset=voff1).reshape(vcnt1, 3).copy()
    ib_off1 = vb_sz0 + start1 * 2
    tris1 = np.frombuffer(lod0, dtype="<u2", count=prim1 * 3, offset=ib_off1).reshape(prim1, 3).copy()

    dists1, idxs1 = tree.query(pos1)
    missing1 = dists1 > 0.005
    pos1[missing1] = np.array([0.0, 1.56, 0.0], dtype=np.float32)
    tri_m1 = missing1[tris1[:, 0]] | missing1[tris1[:, 1]] | missing1[tris1[:, 2]]
    tris1[tri_m1, 1] = tris1[tri_m1, 0]
    tris1[tri_m1, 2] = tris1[tri_m1, 0]

    # Sub 5 (Top): remove deleted neck/tie pieces and apply hair deltas
    sb5 = sec_off0 + 5 * 0x180
    prim5, start5, voff5, vcnt5 = struct.unpack_from("<4I", res_bytes, sb5 + 0x30)
    pos5 = np.frombuffer(lod0, dtype="<f4", count=vcnt5 * 3, offset=voff5).reshape(vcnt5, 3).copy()
    ib_off5 = vb_sz0 + start5 * 2
    tris5 = np.frombuffer(lod0, dtype="<u2", count=prim5 * 3, offset=ib_off5).reshape(prim5, 3).copy()

    dists5, idxs5 = tree.query(pos5)
    missing5 = dists5 > 0.005
    pos5[missing5] = np.array([0.0, 1.56, 0.0], dtype=np.float32)
    tri_m5 = missing5[tris5[:, 0]] | missing5[tris5[:, 1]] | missing5[tris5[:, 2]]
    tris5[tri_m5, 1] = tris5[tri_m5, 0]
    tris5[tri_m5, 2] = tris5[tri_m5, 0]

    hair_mask5 = (dists5 < 0.002) & (np.linalg.norm(hair_deltas[idxs5], axis=1) > 1e-4)
    pos5[hair_mask5] += hair_deltas[idxs5[hair_mask5]]
    print(f"[LOD 0] Displaced {np.sum(hair_mask5)} hair vertices; collapsed {np.sum(missing1)} Sub1 + {np.sum(missing5)} Sub5 verts")

    lod0[voff1:voff1 + vcnt1 * 12] = pos1.tobytes()
    lod0[ib_off1:ib_off1 + prim1 * 6] = tris1.tobytes()
    lod0[voff5:voff5 + vcnt5 * 12] = pos5.tobytes()
    lod0[ib_off5:ib_off5 + prim5 * 6] = tris5.tobytes()

    assert len(lod0) == 4207184
    return bytes(lod0)


def process_lod1(
    res_bytes: bytes,
    lod1_raw_bytes: bytes,
    basis_pos: np.ndarray,
    hair_deltas: np.ndarray,
) -> bytes:
    """Displace hair vertices and collapse blindfold on authentic LOD 1 chunk."""
    lod1 = bytearray(lod1_raw_bytes)
    assert len(lod1) == 3237760, f"LOD 1 size expected 3237760, got {len(lod1)}"
    tree = cKDTree(basis_pos)

    sec_off1 = 0x0E50
    _, _, ib_sz1, vb_sz1 = struct.unpack_from("<4I", res_bytes, 0x0190 + 0x60)

    # Sub 1 (Bottom): remove blindfold
    sb1 = sec_off1 + 1 * 0x180
    prim1, start1, voff1, vcnt1 = struct.unpack_from("<4I", res_bytes, sb1 + 0x30)
    pos1 = np.frombuffer(lod1, dtype="<f4", count=vcnt1 * 3, offset=voff1).reshape(vcnt1, 3).copy()
    ib_off1 = vb_sz1 + start1 * 2
    tris1 = np.frombuffer(lod1, dtype="<u2", count=prim1 * 3, offset=ib_off1).reshape(prim1, 3).copy()

    dists1, idxs1 = tree.query(pos1)
    missing1 = (pos1[:, 1] > 1.50) & (pos1[:, 2] > 0.05) & (dists1 > 0.005)
    pos1[missing1] = np.array([0.0, 1.56, 0.0], dtype=np.float32)
    tri_m1 = missing1[tris1[:, 0]] | missing1[tris1[:, 1]] | missing1[tris1[:, 2]]
    tris1[tri_m1, 1] = tris1[tri_m1, 0]
    tris1[tri_m1, 2] = tris1[tri_m1, 0]

    # Sub 5 (Top): remove deleted pieces and apply hair deltas
    sb5 = sec_off1 + 5 * 0x180
    prim5, start5, voff5, vcnt5 = struct.unpack_from("<4I", res_bytes, sb5 + 0x30)
    pos5 = np.frombuffer(lod1, dtype="<f4", count=vcnt5 * 3, offset=voff5).reshape(vcnt5, 3).copy()
    ib_off5 = vb_sz1 + start5 * 2
    tris5 = np.frombuffer(lod1, dtype="<u2", count=prim5 * 3, offset=ib_off5).reshape(prim5, 3).copy()

    dists5, idxs5 = tree.query(pos5)
    missing5 = (pos5[:, 1] < 1.45) & (dists5 > 0.008) & (pos5[:, 1] > 1.25) & (np.abs(pos5[:, 0]) < 0.12)
    pos5[missing5] = np.array([0.0, 1.56, 0.0], dtype=np.float32)
    tri_m5 = missing5[tris5[:, 0]] | missing5[tris5[:, 1]] | missing5[tris5[:, 2]]
    tris5[tri_m5, 1] = tris5[tri_m5, 0]
    tris5[tri_m5, 2] = tris5[tri_m5, 0]

    hair_mask5 = (pos5[:, 1] > 1.40) & (dists5 < 0.015) & (np.linalg.norm(hair_deltas[idxs5], axis=1) > 1e-4)
    pos5[hair_mask5] += hair_deltas[idxs5[hair_mask5]]
    print(f"[LOD 1] Displaced {np.sum(hair_mask5)} hair vertices; collapsed {np.sum(missing1)} Sub1 + {np.sum(missing5)} Sub5 verts")

    lod1[voff1:voff1 + vcnt1 * 12] = pos1.tobytes()
    lod1[ib_off1:ib_off1 + prim1 * 6] = tris1.tobytes()
    lod1[voff5:voff5 + vcnt5 * 12] = pos5.tobytes()
    lod1[ib_off5:ib_off5 + prim5 * 6] = tris5.tobytes()

    assert len(lod1) == 3237760
    return bytes(lod1)


def main():
    blend_path = REPO_ROOT / "analysis" / "yorha2b" / "model_edit.blend"
    res_path = REPO_ROOT / "analysis" / "yorha2b" / "mesh_res.bin"
    lod0_path = REPO_ROOT / "analysis" / "yorha2b" / "real_lod0.bin"
    lod1_path = REPO_ROOT / "analysis" / "yorha2b" / "lod0_chunk.bin"

    print("[1/4] Extracting hair shape key deltas from Blender ...")
    basis_pos, hair_deltas = extract_hair_deltas_from_blend(blend_path)

    res_bytes = res_path.read_bytes()
    lod0_raw = lod0_path.read_bytes()
    lod1_raw = lod1_path.read_bytes()

    print("[2/4] Processing LOD 0 (hair deltas + no blindfold) ...")
    lod0_out = process_lod0(res_bytes, lod0_raw, basis_pos, hair_deltas)

    print("[3/4] Processing LOD 1 (hair deltas + no blindfold) ...")
    lod1_out = process_lod1(res_bytes, lod1_raw, basis_pos, hair_deltas)

    res_cas = wrap_uncompressed_cas(res_bytes)
    lod0_cas = wrap_uncompressed_cas(lod0_out)
    lod1_cas = wrap_uncompressed_cas(lod1_out)

    print(f"Generated CAS wrappers: res={len(res_cas):,} B, lod0={len(lod0_cas):,} B, lod1={len(lod1_cas):,} B")

    print("[4/4] Deploying to mod targets ...")
    for mod_p in MOD_PATHS:
        if not mod_p.exists():
            print(f"Skipping non-existent {mod_p}")
            continue
        print(f"Patching {mod_p} ...")
        patch_reskate_mod_directory(
            mod_dir=mod_p,
            res_out=res_bytes,
            chunk_out=lod0_out,
            res_cas_bytes=res_cas,
            chunk_cas_bytes=lod0_cas,
            chunk_lod1_out=lod1_out,
            chunk_lod1_cas_bytes=lod1_cas,
        )

    # Verification of SHA-256 sums
    print("\n--- Verification ---")
    for mod_p in MOD_PATHS:
        if not mod_p.exists():
            continue
        cas_p = mod_p / "Win32" / "configurations" / "layout" / "initialinstallpackage" / "cas_01.cas"
        toc_p = mod_p / "Win32" / "items.toc"
        cas_h = hashlib.sha256(cas_p.read_bytes()).hexdigest()
        toc_h = hashlib.sha256(toc_p.read_bytes()).hexdigest()
        print(f"{mod_p.name} ({mod_p.parent.parent.name}):")
        print(f"  cas_01.cas: size={os.path.getsize(cas_p):,} B, sha256={cas_h[:16]}...")
        print(f"  items.toc:  size={os.path.getsize(toc_p):,} B, sha256={toc_h[:16]}...")

    print("\nSuccess! Mod deployment complete.")


if __name__ == "__main__":
    main()
