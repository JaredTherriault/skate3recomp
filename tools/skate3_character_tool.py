#!/usr/bin/env python3
"""Bidirectional glTF (.glb/.gltf) / Blender (.blend) <-> Skate 3 Character Tool.

Subcommands:
  to-skate3    Convert a .glb/.gltf OR .blend character into a Skate 3 S3B2 .bin
               file and optionally generate a ready-to-play Meta Menu mod pack in
               mods/raw/<mod_id>/pack.json.
  from-skate3  Convert a Skate 3 character (custom S3B2 .bin OR stock .rx2 character
               meshes) into a rigged, textured 70-bone .glb file.
  to-frostbite Convert a Skate 3 character (.bin or .rx2) directly into a Frostbite
               (skate. 2025) MeshSet (mesh_res.bin + real_lod0.bin + .cas).

Examples:
  # 1. Convert a .glb or .blend into a Skate 3 character mod:
  python3 tools/skate3_character_tool.py to-skate3 \
      --input analysis/yorha2b/model_edit.blend \
      --mod-id yorha_2b_player_model \
      --mod-name "2B - Player Model Replacement"

  # 2. Convert a Skate 3 S3B2 .bin back into a rigged .glb:
  python3 tools/skate3_character_tool.py from-skate3 \
      --input mods/raw/yorha_2b_player_model/yorha2b_skate3.bin \
      --out yorha2b_from_s3.glb

  # 3. Extract stock Skate 3 .rx2 body parts into a single rigged 70-bone .glb:
  python3 tools/skate3_character_tool.py from-skate3 \
      --rx2 analysis/yorha2b/s3_rostr0.rx2 analysis/yorha2b/s3_fidx15_163.rx2 \
            analysis/yorha2b/s3_fidx11_71.rx2 analysis/yorha2b/s3_feet0.rx2 \
      --out skate3_skater.glb

  # 4. Convert a Skate 3 model (.bin or .rx2) straight to Frostbite (skate. 2025):
  python3 tools/skate3_character_tool.py to-frostbite \
      --input mods/raw/yorha_2b_player_model/yorha2b_skate3.bin \
      --out-dir build/s3_to_frostbite
"""

import argparse
import io
import json
import math
import os
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

import numpy as np
from PIL import Image

TOOLS_DIR = Path(__file__).resolve().parent
REPO_ROOT = TOOLS_DIR.parent
if str(TOOLS_DIR) not in sys.path:
    sys.path.insert(0, str(TOOLS_DIR))

from frostbite_gltf import (  # noqa: E402
    FB_TO_S3,
    S3_BIND_POSITIONS,
    S3_BONE_INDEX,
    S3_BONE_NAMES,
    S3_PARENT_BONE,
    S3_TO_FB,
    load_gltf_mesh_and_skin,
    pack_frostbite_meshset,
    resolve_bone_name_to_s3,
    wrap_uncompressed_cas,
    write_rigged_glb,
)


# ---------------------------------------------------------------------------
# Headless Blender (.blend) -> Temporary Rigged .glb Extractor
# ---------------------------------------------------------------------------

BLENDER_EXTRACT_SCRIPT = r"""
import bpy
import sys
import os

argv = sys.argv[sys.argv.index("--") + 1:]
out_glb = argv[0]
bake_shapekeys = argv[1] == "1"

# Ensure all mesh objects are visible and optionally bake shape keys to 1.0
for obj in list(bpy.data.objects):
    if obj.type == 'MESH':
        obj.hide_viewport = False
        obj.hide_render = False
        try:
            obj.hide_set(False)
        except Exception:
            pass
        if bake_shapekeys and obj.data.shape_keys and obj.data.shape_keys.key_blocks:
            for kb in obj.data.shape_keys.key_blocks[1:]:
                if kb.value < 0.01:
                    kb.value = 1.0

bpy.ops.export_scene.gltf(
    filepath=out_glb,
    export_format='GLB',
    use_selection=False,
    export_apply=True,
    export_yup=True,
    export_skins=True,
    export_all_influences=True,
    export_morph=False,
)
print("[Blender] Exported temporary GLB:", out_glb)
"""


def convert_blend_to_temp_glb(blend_path: Path, bake_shapekeys: bool = True) -> Path:
    """Run headless Blender to convert a .blend file into a temporary rigged .glb."""
    blender_bin = os.environ.get("BLENDER_BIN", "blender")
    td = tempfile.mkdtemp(prefix="s3_blend_")
    out_glb = Path(td) / f"{blend_path.stem}.glb"
    script_p = Path(td) / "extract_blend.py"
    script_p.write_text(BLENDER_EXTRACT_SCRIPT, encoding="utf-8")

    cmd = [
        blender_bin,
        "-b",
        str(blend_path),
        "-P",
        str(script_p),
        "--",
        str(out_glb),
        "1" if bake_shapekeys else "0",
    ]
    print(f"[Blender] Running headless conversion on {blend_path.name} ...")
    subprocess.run(cmd, check=True)
    if not out_glb.exists():
        raise RuntimeError(f"Blender failed to produce {out_glb}")
    return out_glb


# ---------------------------------------------------------------------------
# Automatic Bone Weight Assignment & Pose Helpers
# ---------------------------------------------------------------------------

def compute_auto_s3_weights(pos: np.ndarray):
    """Fallback automatic 4-bone skinning weights for unrigged meshes using bone segment distances."""
    V = len(pos)
    bone_pos = np.array(S3_BIND_POSITIONS, dtype=np.float32)
    # Build segments (parent -> child) for primary body bones (excluding helper/twist duplicates)
    primary_bones = [
        0, 1, 2, 3, 4, 5, 6, 7,
        8, 9, 11, 14,
        32, 33, 35, 38,
        58, 59, 60, 61,
        63, 64, 65, 66,
    ]
    b_pts = bone_pos[primary_bones]  # (B, 3)
    # Compute squared distance from every vertex to each primary bone
    diff = pos[:, None, :] - b_pts[None, :, :]  # (V, B, 3)
    dist2 = np.sum(diff * diff, axis=2)  # (V, B)
    nearest4 = np.argsort(dist2, axis=1)[:, :4]  # (V, 4)

    s3_bi = np.zeros((V, 4), dtype=np.uint8)
    s3_bw = np.zeros((V, 4), dtype=np.uint8)
    for v in range(V):
        idxs = nearest4[v]
        d = np.sqrt(np.maximum(dist2[v, idxs], 1e-8))
        inv_d = 1.0 / (d ** 2.5)
        inv_d /= inv_d.sum()
        u8_w = [int(round(w * 255.0)) for w in inv_d]
        u8_w[0] = max(0, min(255, u8_w[0] + (255 - sum(u8_w))))
        for k in range(4):
            s3_bi[v, k] = primary_bones[int(idxs[k])]
            s3_bw[v, k] = u8_w[k]
    return s3_bi, s3_bw


def map_weights_to_skate3(
    pos: np.ndarray,
    joints: np.ndarray,
    weights: np.ndarray,
    joint_names: list[str],
    force_auto_weights: bool = False,
):
    """Map up to 8-bone glTF skinning weights to Skate 3's 70 canonical bones (4x uint8 indices + 4x uint8 weights)."""
    if force_auto_weights or not joint_names:
        print("[Skate 3 Rig] Using automatic nearest-joint skinning weights ...")
        return compute_auto_s3_weights(pos)

    joint_to_s3 = []
    mapped_count = 0
    for j_name in joint_names:
        s3_id = resolve_bone_name_to_s3(j_name)
        if s3_id is not None:
            mapped_count += 1
        joint_to_s3.append(s3_id)

    if mapped_count == 0:
        print("[Skate 3 Rig] No recognized bone names in input rig; falling back to automatic nearest-joint weights.")
        return compute_auto_s3_weights(pos)

    print(f"[Skate 3 Rig] Mapped {mapped_count}/{len(joint_names)} input bones to Skate 3's 70-bone skeleton.")
    V = len(pos)
    s3_bi = np.zeros((V, 4), dtype=np.uint8)
    s3_bw = np.zeros((V, 4), dtype=np.uint8)

    for v in range(V):
        accum = {}
        for k in range(joints.shape[1]):
            wt = float(weights[v, k])
            if wt <= 1e-5:
                continue
            j_idx = int(joints[v, k])
            s3_id = joint_to_s3[j_idx] if j_idx < len(joint_to_s3) else None
            if s3_id is None:
                s3_id = 0
            accum[s3_id] = accum.get(s3_id, 0.0) + wt
        if not accum:
            accum[0] = 1.0
        top4 = sorted(accum.items(), key=lambda x: -x[1])[:4]
        tot = sum(val for _, val in top4) or 1.0
        raw_u8 = [int(round((val / tot) * 255.0)) for _, val in top4]
        diff = 255 - sum(raw_u8)
        raw_u8[0] = max(0, min(255, raw_u8[0] + diff))
        for k, ((b_id, _), u8_w) in enumerate(zip(top4, raw_u8)):
            s3_bi[v, k] = b_id
            s3_bw[v, k] = u8_w

    return s3_bi, s3_bw


def apply_arm_pose_rotation(pos: np.ndarray, nrm: np.ndarray, s3_bi: np.ndarray, s3_bw: np.ndarray, angle_deg: float):
    """Rotate left and right arms around LeftArm (9) / RightArm (33) bind positions by angle_deg (negative lowers T-pose to A-pose)."""
    if abs(angle_deg) < 1e-3:
        return pos, nrm
    left_arm_bones = set(range(9, 32))
    right_arm_bones = set(range(33, 56))
    l_pivot = np.array(S3_BIND_POSITIONS[9], dtype=np.float32)
    r_pivot = np.array(S3_BIND_POSITIONS[33], dtype=np.float32)

    rad_l = math.radians(angle_deg)
    rad_r = math.radians(-angle_deg)
    cl, sl = math.cos(rad_l), math.sin(rad_l)
    cr, sr = math.cos(rad_r), math.sin(rad_r)
    rot_l = np.array([[cl, -sl, 0.0], [sl, cl, 0.0], [0.0, 0.0, 1.0]], dtype=np.float32)
    rot_r = np.array([[cr, -sr, 0.0], [sr, cr, 0.0], [0.0, 0.0, 1.0]], dtype=np.float32)

    out_pos = pos.copy()
    out_nrm = nrm.copy()
    for v in range(len(pos)):
        w_l = 0.0
        w_r = 0.0
        for k in range(4):
            b = int(s3_bi[v, k])
            w = float(s3_bw[v, k]) / 255.0
            if b in left_arm_bones:
                w_l += w
            elif b in right_arm_bones:
                w_r += w
        if w_l > 0.01:
            p_rot = (pos[v] - l_pivot) @ rot_l.T + l_pivot
            n_rot = nrm[v] @ rot_l.T
            out_pos[v] = (1.0 - w_l) * out_pos[v] + w_l * p_rot
            out_nrm[v] = (1.0 - w_l) * out_nrm[v] + w_l * n_rot
        if w_r > 0.01:
            p_rot = (pos[v] - r_pivot) @ rot_r.T + r_pivot
            n_rot = nrm[v] @ rot_r.T
            out_pos[v] = (1.0 - w_r) * out_pos[v] + w_r * p_rot
            out_nrm[v] = (1.0 - w_r) * out_nrm[v] + w_r * n_rot

    lens = np.linalg.norm(out_nrm, axis=1, keepdims=True)
    out_nrm = np.where(lens > 1e-8, out_nrm / np.maximum(lens, 1e-8), out_nrm)
    return out_pos, out_nrm


def generate_eyeballs_mesh(base_vert_idx: int):
    """Generate two 12x16 UV-sphere eyeballs at (+/-0.0286, 1.5920, 0.0825) weighted 100% to Head (bone 7)."""
    eye_centers = [(-0.0286, 1.5920, 0.0825), (0.0286, 1.5920, 0.0825)]
    radius = 0.0122
    rings, segs = 12, 16
    e_pos, e_nrm, e_uv, e_bi, e_bw, e_tris = [], [], [], [], [], []
    cur_base = base_vert_idx

    for cx, cy, cz in eye_centers:
        for r in range(rings + 1):
            v_p = r / rings
            phi = (v_p - 0.5) * math.pi
            cp, sp = math.cos(phi), math.sin(phi)
            for s in range(segs + 1):
                u_p = s / segs
                theta = (u_p - 0.5) * 2.0 * math.pi
                nx = cp * math.sin(theta)
                ny = sp
                nz = cp * math.cos(theta)
                e_pos.append((cx + radius * nx, cy + radius * ny, cz + radius * nz))
                e_nrm.append((nx, ny, nz))
                dist_iris = math.sqrt(nx * nx + ny * ny)
                if nz > 0.86 and dist_iris < 0.50:
                    if dist_iris < 0.22:
                        u_a, v_a = 0.275, 0.275
                    else:
                        u_a, v_a = 0.145, 0.720
                else:
                    u_a, v_a = 0.025, 0.960
                e_uv.append((u_a, v_a))
                e_bi.append((7, 0, 0, 0))
                e_bw.append((255, 0, 0, 0))
        stride = segs + 1
        for r in range(rings):
            for s in range(segs):
                a = cur_base + r * stride + s
                b = a + 1
                c = a + stride
                d = c + 1
                e_tris.append((a, b, d))
                e_tris.append((a, d, c))
        cur_base += (rings + 1) * (segs + 1)

    return (
        np.array(e_pos, dtype=np.float32),
        np.array(e_nrm, dtype=np.float32),
        np.array(e_uv, dtype=np.float32),
        np.array(e_bi, dtype=np.uint8),
        np.array(e_bw, dtype=np.uint8),
        np.array(e_tris, dtype=np.int32),
    )


def build_atlas_and_remap_uvs(uv: np.ndarray, mat: np.ndarray, pil_images: list[Image.Image], explicit_atlas_path: Path | None):
    """Build a 2048x2048 RGBA8 texture atlas (with specular gloss in alpha) and remap UVs if multi-material."""
    uv_out = uv.copy()
    num_mats = int(mat.max()) + 1 if len(mat) > 0 else 1

    if explicit_atlas_path and explicit_atlas_path.exists():
        atlas_img = Image.open(explicit_atlas_path).convert("RGBA").resize((2048, 2048), Image.Resampling.LANCZOS)
        if num_mats > 1:
            for v in range(len(uv_out)):
                sl = max(0, min(3, int(mat[v])))
                u_l = float(uv_out[v, 0]) % 1.0
                v_l = float(uv_out[v, 1]) % 1.0
                col, row = sl & 1, (sl >> 1) & 1
                uv_out[v, 0] = u_l * 0.5 + col * 0.5
                uv_out[v, 1] = v_l * 0.5 + row * 0.5
    elif len(pil_images) >= 2 or num_mats > 1:
        atlas_img = Image.new("RGBA", (2048, 2048), (200, 200, 200, 255))
        quads = [(0, 0), (1024, 0), (0, 1024), (1024, 1024)]
        for s in range(min(4, len(pil_images))):
            sub_im = pil_images[s].resize((1024, 1024), Image.Resampling.LANCZOS)
            atlas_img.paste(sub_im, quads[s])
        for v in range(len(uv_out)):
            sl = max(0, min(3, int(mat[v])))
            u_l = float(uv_out[v, 0]) % 1.0
            v_l = float(uv_out[v, 1]) % 1.0
            col, row = sl & 1, (sl >> 1) & 1
            uv_out[v, 0] = u_l * 0.5 + col * 0.5
            uv_out[v, 1] = v_l * 0.5 + row * 0.5
    elif len(pil_images) == 1:
        atlas_img = pil_images[0].resize((2048, 2048), Image.Resampling.LANCZOS)
    else:
        default_atlas = REPO_ROOT / "analysis" / "yorha2b" / "yorha2b_atlas_2048.png"
        if default_atlas.exists():
            atlas_img = Image.open(default_atlas).convert("RGBA").resize((2048, 2048), Image.Resampling.LANCZOS)
        else:
            atlas_img = Image.new("RGBA", (2048, 2048), (200, 200, 200, 255))

    tex_rgba = np.array(atlas_img, dtype=np.uint8)
    lum = (
        0.299 * tex_rgba[:, :, 0].astype(np.float32)
        + 0.587 * tex_rgba[:, :, 1].astype(np.float32)
        + 0.114 * tex_rgba[:, :, 2].astype(np.float32)
    )
    spec = np.clip(lum * 0.25 + 8.0, 4.0, 72.0).astype(np.uint8)
    tex_rgba[:, :, 3] = spec
    return uv_out, tex_rgba


def write_skate3_bin(
    out_bin_path: Path,
    pos: np.ndarray,
    nrm: np.ndarray,
    uv: np.ndarray,
    s3_bi: np.ndarray,
    s3_bw: np.ndarray,
    tris: np.ndarray,
    tex_rgba: np.ndarray,
):
    """Write a Skate 3 S3B2 .bin character file."""
    V = len(pos)
    if V > 65535:
        raise ValueError(
            f"Vertex count ({V:,}) exceeds Skate 3's 16-bit index buffer limit (65,535). "
            "Please decimate the mesh below 65,535 vertices."
        )
    idx_u16 = tris.astype("<u2").flatten()

    vb = np.zeros((V, 14), dtype=np.uint32)
    vb_f = vb.view(np.float32)
    vb_f[:, 0:3] = pos
    vb_f[:, 3:5] = uv
    vb_f[:, 5:7] = uv
    vb[:, 7] = (
        s3_bw[:, 0].astype(np.uint32)
        | (s3_bw[:, 1].astype(np.uint32) << 8)
        | (s3_bw[:, 2].astype(np.uint32) << 16)
        | (s3_bw[:, 3].astype(np.uint32) << 24)
    )
    vb[:, 8] = (
        s3_bi[:, 0].astype(np.uint32)
        | (s3_bi[:, 1].astype(np.uint32) << 8)
        | (s3_bi[:, 2].astype(np.uint32) << 16)
        | (s3_bi[:, 3].astype(np.uint32) << 24)
    )
    vb_f[:, 9:12] = nrm
    vb_f[:, 12:14] = uv

    bmin = pos.min(axis=0).astype(np.float32)
    bmax = pos.max(axis=0).astype(np.float32)
    tex_h, tex_w = tex_rgba.shape[:2]

    hdr = struct.pack(
        "<6I6f",
        0x32425333,  # "S3B2"
        1,           # version 1
        V,
        len(idx_u16),
        tex_w,
        tex_h,
        float(bmin[0]), float(bmin[1]), float(bmin[2]),
        float(bmax[0]), float(bmax[1]), float(bmax[2]),
    )

    out_bin_path.parent.mkdir(parents=True, exist_ok=True)
    with open(out_bin_path, "wb") as f:
        f.write(hdr)
        f.write(vb.tobytes())
        f.write(idx_u16.tobytes())
        f.write(tex_rgba.tobytes())


# ---------------------------------------------------------------------------
# Skate 3 S3B2 .bin & Stock RW4 (.rx2) Readers (for from-skate3)
# ---------------------------------------------------------------------------

def read_skate3_bin(bin_path: Path):
    """Read a custom Skate 3 S3B2 .bin character file."""
    data = bin_path.read_bytes()
    if len(data) < 48:
        raise ValueError(f"{bin_path} is too small to be an S3B2 .bin file")
    magic, ver, num_verts, num_indices, tex_w, tex_h, *bbox = struct.unpack_from("<6I6f", data, 0)
    if magic not in (0x32425333, 0x314E4253):
        raise ValueError(f"Invalid S3B2 magic {magic:#x} in {bin_path}")

    vb_off = 48
    vb_sz = num_verts * 56
    ib_off = vb_off + vb_sz
    ib_sz = num_indices * 2
    tex_off = ib_off + ib_sz

    vb_u32 = np.frombuffer(data, dtype="<u4", count=num_verts * 14, offset=vb_off).reshape(num_verts, 14)
    vb_f32 = vb_u32.view("<f4")

    pos = vb_f32[:, 0:3].copy()
    uv = vb_f32[:, 3:5].copy()
    nrm = vb_f32[:, 9:12].copy()

    bw_word = vb_u32[:, 7]
    bi_word = vb_u32[:, 8]
    bw = np.stack([(bw_word >> (k * 8)) & 0xFF for k in range(4)], axis=1).astype(np.uint8)
    bi = np.stack([(bi_word >> (k * 8)) & 0xFF for k in range(4)], axis=1).astype(np.uint16)

    tris = np.frombuffer(data, dtype="<u2", count=num_indices, offset=ib_off).reshape(-1, 3).astype(np.int32)

    tex_bytes = data[tex_off:tex_off + tex_w * tex_h * 4]
    tex_rgba = np.frombuffer(tex_bytes, dtype=np.uint8).reshape(tex_h, tex_w, 4).copy()
    # Restore opaque alpha for standard glTF/Blender viewing (alpha in S3B2 stores specular gloss)
    tex_rgba[:, :, 3] = 255
    img = Image.fromarray(tex_rgba, mode="RGBA")
    png_buf = io.BytesIO()
    img.save(png_buf, format="PNG")

    sl = np.zeros(num_verts, dtype=np.int32)
    return pos, nrm, uv, sl, bi, bw, tris, [png_buf.getvalue()]


def half_to_float_array(u16_arr: np.ndarray) -> np.ndarray:
    return u16_arr.astype(">u2").view(">f2").astype(np.float32)


def decode_rx2_character_mesh(rx2_path: Path):
    """Decode a stock Skate 3 RW4 (.rx2) character mesh into pos, nrm, uv, bi (0..69), bw, tris."""
    data = rx2_path.read_bytes()
    if data[:7] != b"\x89RW4xb2":
        raise ValueError(f"{rx2_path} is not a valid RW4 .rx2 archive")

    num_sec = struct.unpack_from(">I", data, 0x20)[0]
    sec_manifest_off = struct.unpack_from(">I", data, 0x30)[0]
    blob_base = struct.unpack_from(">I", data, 0x44)[0]

    secs = []
    for i in range(num_sec):
        off, _, sz, align, tidx, tid = struct.unpack_from(">6I", data, sec_manifest_off + i * 24)
        secs.append((off, sz, tid))

    # 1. Parse cModel (0xeb0001) bone names
    model_bone_names = []
    for off, sz, tid in secs:
        if tid == 0xeb0001:
            nmesh, nbones = struct.unpack_from(">HH", data, off + 0x32)
            base_off = off + 0x40
            name_ptrs_rel = struct.unpack_from(">I", data, off + 0x28)[0]
            name_table_off = base_off + name_ptrs_rel - 0x40
            for b in range(nbones):
                str_rel = struct.unpack_from(">I", data, name_table_off + b * 4)[0]
                str_off = base_off + str_rel - 0x40
                end = data.find(b"\x00", str_off)
                bname = data[str_off:end].decode("ascii", errors="replace")
                model_bone_names.append(bname)
            break

    # 2. Parse first cMesh (0xeb0023)
    mesh_sec = next((s for s in secs if s[2] == 0xeb0023), None)
    if not mesh_sec:
        raise ValueError(f"No cMesh section found in {rx2_path}")
    m_off = mesh_sec[0]
    vdecl_s, prim_s, ib_s, vb_s = struct.unpack_from(">4I", data, m_off + 0x28)
    mb_cnt, mb_rel = struct.unpack_from(">II", data, m_off + 0x48)
    mb_table = struct.unpack_from(f">{mb_cnt}H", data, m_off + mb_rel) if mb_cnt <= 256 else ()

    # Build palette index -> canonical S3 bone index (0..69)
    pal_to_s3 = []
    for pal_k, mb_idx in enumerate(mb_table):
        if mb_idx < len(model_bone_names):
            bname = model_bone_names[mb_idx]
            pal_to_s3.append(S3_BONE_INDEX.get(bname, 0))
        else:
            pal_to_s3.append(0)
    if not pal_to_s3:
        pal_to_s3 = [0]

    # 3. Parse VertexDeclaration (0x200e9)
    vd_off = secs[vdecl_s][0]
    num_elems = struct.unpack_from(">H", data, vd_off + 0x08)[0]
    stride = data[vd_off + 0x10 + num_elems * 16]
    pos_off, pos_fmt = 0, 26
    uv_off, uv_fmt = 0, 0
    bw_off, bi_off = 0, 0
    nrm_off, nrm_fmt = 0, 0

    for i in range(num_elems):
        e_off = vd_off + 0x10 + i * 16
        stream = struct.unpack_from(">H", data, e_off)[0]
        offset = struct.unpack_from(">H", data, e_off + 2)[0]
        fmt = struct.unpack_from(">I", data, e_off + 4)[0] & 0x3F
        usage = data[e_off + 9]
        if stream != 0:
            continue
        if usage == 0:
            pos_off, pos_fmt = offset, fmt
        elif usage == 5 and uv_fmt == 0:
            uv_off, uv_fmt = offset, fmt
        elif usage == 1:
            bw_off = offset
        elif usage == 2:
            bi_off = offset
        elif usage == 3 and nrm_fmt == 0:
            nrm_off, nrm_fmt = offset, fmt

    # 4. Parse VertexBuffer (0x200ea) & IndexBuffer (0x200eb)
    vb_desc_off = secs[vb_s][0]
    vb_phys_off, _, vb_bytes = struct.unpack_from(">3I", data, vb_desc_off + 0x1C)
    vb_phys_off &= 0xFFFFFFFC

    ib_desc_off = secs[ib_s][0]
    ib_phys_off, ib_bytes, ib_count = struct.unpack_from(">3I", data, ib_desc_off + 0x18)
    # Wait: check IB descriptor layout (+0x18 is section index or phys_off, +0x1c is size, +0x20 is count)
    # In s3_fidx15_163.rx2: IB desc at 0x62c had ['0x20000002', '0x1', '0x0', '0x0', '0x0', '0xffff0000', '0x5', '0x1940', '0xc9c']
    # Here word 6 (+0x18) = 5 (section index 5 in secs!), word 7 (+0x1c) = 0x1940 (byte size), word 8 (+0x20) = 0xc9c (index count = 3228)!
    ib_data_sec_idx = struct.unpack_from(">I", data, ib_desc_off + 0x18)[0]
    ib_count = struct.unpack_from(">I", data, ib_desc_off + 0x20)[0]
    ib_blob_off = blob_base + secs[ib_data_sec_idx][0]

    # And in VB desc at 0x604: word 8 (+0x20) = 0x8390 (vb_bytes), word 9 (+0x24) = 0x0 (vb_phys_off)!
    vb_bytes = struct.unpack_from(">I", data, vb_desc_off + 0x20)[0]
    vb_phys_off = struct.unpack_from(">I", data, vb_desc_off + 0x24)[0] & 0xFFFFFFFC
    vb_blob_off = blob_base + vb_phys_off

    num_verts = vb_bytes // stride
    pos = np.zeros((num_verts, 3), dtype=np.float32)
    nrm = np.zeros((num_verts, 3), dtype=np.float32)
    uv = np.zeros((num_verts, 2), dtype=np.float32)
    bi = np.zeros((num_verts, 4), dtype=np.uint16)
    bw = np.zeros((num_verts, 4), dtype=np.uint8)

    scale_s16 = 2.0 / 32767.0
    for v in range(num_verts):
        v_base = vb_blob_off + v * stride
        # Position
        p_ptr = v_base + pos_off
        if pos_fmt == 26:
            sx, sy, sz, _ = struct.unpack_from(">4h", data, p_ptr)
            pos[v] = (sx * scale_s16, sy * scale_s16 + 0.8, sz * scale_s16)
        elif pos_fmt == 57:
            pos[v] = struct.unpack_from(">3f", data, p_ptr)
        elif pos_fmt == 32:
            pos[v] = np.frombuffer(data, dtype=">f2", count=3, offset=p_ptr).astype(np.float32)

        # UV0
        if uv_fmt in (31, 32):
            uv[v] = np.frombuffer(data, dtype=">f2", count=2, offset=v_base + uv_off).astype(np.float32)
        elif uv_fmt in (37, 38):
            uv[v] = struct.unpack_from(">2f", data, v_base + uv_off)
        elif uv_fmt in (25, 26):
            su, sv = struct.unpack_from(">2h", data, v_base + uv_off)
            uv[v] = (su / 32767.0, sv / 32767.0)

        # Normal (k_10_11_11)
        if nrm_fmt == 16:
            word = struct.unpack_from(">I", data, v_base + nrm_off)[0]
            ix = ctypes_s32(word << 21) >> 21
            iy = ctypes_s32((word >> 11) << 21) >> 21
            iz = ctypes_s32((word >> 22) << 22) >> 22
            nrm[v] = (ix / 1023.0, iy / 1023.0, iz / 511.0)

        # Blend weights & indices (stored big-endian per 32-bit word)
        if bw_off and bi_off:
            bw_w = struct.unpack_from(">I", data, v_base + bw_off)[0]
            bi_w = struct.unpack_from(">I", data, v_base + bi_off)[0]
            for k in range(4):
                w_k = (bw_w >> (k * 8)) & 0xFF
                i_k = (bi_w >> (k * 8)) & 0xFF
                bw[v, k] = w_k
                bi[v, k] = pal_to_s3[i_k] if i_k < len(pal_to_s3) else 0
        else:
            bw[v, 0] = 255
            bi[v, 0] = 0

    raw_idx = np.frombuffer(data, dtype=">u2", count=ib_count, offset=ib_blob_off).astype(np.int32)
    # Check if triangle list (divisible by 3 and few degenerate indices)
    tris = raw_idx[: (len(raw_idx) // 3) * 3].reshape(-1, 3)

    # Recompute smooth normals if normal attribute was absent
    if nrm_fmt != 16 and len(tris) > 0:
        v0 = pos[tris[:, 0]]
        v1 = pos[tris[:, 1]]
        v2 = pos[tris[:, 2]]
        fn = np.cross(v1 - v0, v2 - v0)
        np.add.at(nrm, tris[:, 0], fn)
        np.add.at(nrm, tris[:, 1], fn)
        np.add.at(nrm, tris[:, 2], fn)
        lens = np.linalg.norm(nrm, axis=1, keepdims=True)
        nrm = np.where(lens > 1e-8, nrm / np.maximum(lens, 1e-8), np.array([0.0, 1.0, 0.0], dtype=np.float32))

    return pos, nrm, uv, bi, bw, tris


def ctypes_s32(val: int) -> int:
    val &= 0xFFFFFFFF
    return val - 0x100000000 if val & 0x80000000 else val


# ---------------------------------------------------------------------------
# CLI Commands
# ---------------------------------------------------------------------------

def cmd_to_skate3(args):
    in_p = Path(args.input)
    if not in_p.exists():
        raise FileNotFoundError(f"Input file not found: {in_p}")

    temp_glb = None
    try:
        if in_p.suffix.lower() == ".blend":
            temp_glb = convert_blend_to_temp_glb(in_p, bake_shapekeys=not args.no_bake_shapekeys)
            gltf_p = temp_glb
        else:
            gltf_p = in_p

        pos, nrm, uv, mat, joints, weights, tris, joint_names, pil_images = load_gltf_mesh_and_skin(gltf_p)
        print(f"[Skate 3 Converter] Loaded {in_p.name}: {len(pos):,} verts, {len(tris):,} tris, {len(joint_names)} joints")

        if args.auto_fit:
            min_y = float(pos[:, 1].min())
            max_y = float(pos[:, 1].max())
            cur_h = max(max_y - min_y, 1e-4)
            scale = args.target_height / cur_h
            pos[:, 1] -= min_y
            pos *= scale
            print(f"[Skate 3 Converter] Auto-fitted height ({cur_h:.3f}m -> {args.target_height:.3f}m, scale={scale:.4f})")

        s3_bi, s3_bw = map_weights_to_skate3(pos, joints, weights, joint_names, force_auto_weights=args.auto_weights)

        if abs(args.pose_arms) > 1e-3:
            pos, nrm = apply_arm_pose_rotation(pos, nrm, s3_bi, s3_bw, args.pose_arms)
            print(f"[Skate 3 Converter] Rotated arms by {args.pose_arms:+.1f} deg around LeftArm/RightArm bind joints")

        explicit_atlas = Path(args.atlas) if args.atlas else None
        uv_atlas, tex_rgba = build_atlas_and_remap_uvs(uv, mat, pil_images, explicit_atlas)

        if args.add_eyeballs:
            e_pos, e_nrm, e_uv, e_bi, e_bw, e_tris = generate_eyeballs_mesh(len(pos))
            pos = np.concatenate([pos, e_pos], axis=0)
            nrm = np.concatenate([nrm, e_nrm], axis=0)
            uv_atlas = np.concatenate([uv_atlas, e_uv], axis=0)
            s3_bi = np.concatenate([s3_bi, e_bi], axis=0)
            s3_bw = np.concatenate([s3_bw, e_bw], axis=0)
            tris = np.concatenate([tris, e_tris], axis=0)
            # Ensure dark pupil/iris swatch at top-left of atlas for procedural eyeballs
            tex_rgba[540:590, 540:590, :3] = 10
            tex_rgba[1450:1500, 270:320, :3] = np.array([62, 74, 86], dtype=np.uint8)

        # Determine output .bin path and optional mod pack generation
        if args.mod_id:
            mod_dir = REPO_ROOT / "mods" / "raw" / args.mod_id
            mod_dir.mkdir(parents=True, exist_ok=True)
            bin_filename = Path(args.out).name if args.out else f"{args.mod_id}.bin"
            out_bin_p = mod_dir / bin_filename
        elif args.out:
            out_bin_p = Path(args.out)
        else:
            out_bin_p = in_p.with_name(f"{in_p.stem}_skate3.bin")

        write_skate3_bin(out_bin_p, pos, nrm, uv_atlas, s3_bi, s3_bw, tris, tex_rgba)
        print(
            f"[Skate 3 Converter] Wrote {out_bin_p} ({out_bin_p.stat().st_size / (1024 * 1024):.2f} MB): "
            f"{len(pos):,} verts, {len(tris):,} tris"
        )

        if args.mod_id:
            pack_json_p = out_bin_p.parent / "pack.json"
            pack_meta = {
                "id": args.mod_id,
                "name": args.mod_name or f"{in_p.stem} - Player Model Replacement",
                "version": "1.0.0",
                "author": args.author or "Community Modder",
                "description": args.description or f"Replaces the player's Create-A-Skater with {in_p.stem}.",
                "enabled": True,
                "character_replacement": out_bin_p.name,
                "challenges": [],
            }
            pack_json_p.write_text(json.dumps(pack_meta, indent=2) + "\n", encoding="utf-8")
            print(f"[Skate 3 Converter] Generated Meta Menu character replacement mod at {pack_json_p}")
    finally:
        if temp_glb and temp_glb.parent.exists():
            shutil.rmtree(temp_glb.parent, ignore_errors=True)


def cmd_from_skate3(args):
    out_p = Path(args.out)
    if args.rx2:
        all_pos, all_nrm, all_uv, all_bi, all_bw, all_tris = [], [], [], [], [], []
        v_base = 0
        for rx2_file in args.rx2:
            rx2_p = Path(rx2_file)
            p, n, u, bi, bw, t = decode_rx2_character_mesh(rx2_p)
            print(f"[Skate 3 .rx2] Decoded {rx2_p.name}: {len(p):,} verts, {len(t):,} tris")
            all_pos.append(p)
            all_nrm.append(n)
            all_uv.append(u)
            all_bi.append(bi)
            all_bw.append(bw)
            all_tris.append(t + v_base)
            v_base += len(p)
        pos = np.concatenate(all_pos, axis=0)
        nrm = np.concatenate(all_nrm, axis=0)
        uv = np.concatenate(all_uv, axis=0)
        bi = np.concatenate(all_bi, axis=0)
        bw = np.concatenate(all_bw, axis=0)
        tris = np.concatenate(all_tris, axis=0)
        sl = np.zeros(len(pos), dtype=np.int32)
        img = Image.new("RGBA", (512, 512), (210, 210, 210, 255))
        buf = io.BytesIO()
        img.save(buf, format="PNG")
        png_list = [buf.getvalue()]
    elif args.input:
        pos, nrm, uv, sl, bi, bw, tris, png_list = read_skate3_bin(Path(args.input))
    else:
        raise ValueError("Must specify either --input <file.bin> or --rx2 <file1.rx2 ...>")

    # If user requested frostbite skeleton, convert S3 bone indices (0..69) -> Frostbite bone IDs
    if args.skeleton == "frostbite":
        fb_bi = np.zeros((len(pos), 8), dtype=np.uint16)
        fb_bw = np.zeros((len(pos), 8), dtype=np.uint8)
        for k in range(4):
            for v in range(len(pos)):
                fb_bi[v, k] = S3_TO_FB.get(int(bi[v, k]), 7)
                fb_bw[v, k] = bw[v, k]
        bi, bw = fb_bi, fb_bw

    write_rigged_glb(
        out_p,
        out_p.stem,
        pos, nrm, uv, sl, bi, bw, tris,
        png_list,
        skeleton_mode=args.skeleton,
    )
    print(
        f"[Skate 3 -> glTF] Wrote {out_p} ({out_p.stat().st_size / (1024 * 1024):.2f} MB): "
        f"{len(pos):,} verts, {len(tris):,} tris, skeleton={args.skeleton}"
    )


def cmd_to_frostbite(args):
    """One-step Skate 3 (.bin or .rx2) -> Frostbite (skate. 2025) MeshSet / ReSkate mod converter."""
    from frostbite_gltf import cmd_from_gltf
    with tempfile.TemporaryDirectory(prefix="s3_to_fb_") as td:
        tmp_glb = Path(td) / "temp_s3.glb"
        ns = argparse.Namespace(
            input=args.input,
            rx2=args.rx2,
            out=str(tmp_glb),
            skeleton="skate3",
        )
        cmd_from_skate3(ns)

        fb_ns = argparse.Namespace(
            input=str(tmp_glb),
            template_res=args.template_res,
            template_chunk=args.template_chunk,
            from_atlas=True,
            out_dir=args.out_dir,
            template_mod=args.template_mod,
            out_mod=args.out_mod,
            patch_mod=args.patch_mod,
            costume_id=args.costume_id,
            replace_costume=args.replace_costume,
            mod_name=args.mod_name,
            mod_author=args.mod_author,
            mod_description=args.mod_description,
        )
        cmd_from_gltf(fb_ns)


def cmd_extract_textures(args):
    """Extract slice_0.png .. slice_3.png, eyes.png, and atlas.png from a Skate 3 S3B2 .bin file for runtime texture modding."""
    from PIL import Image
    in_path = Path(args.input).resolve()
    out_dir = Path(args.out_dir).resolve() if args.out_dir else in_path.parent
    out_dir.mkdir(parents=True, exist_ok=True)

    data = in_path.read_bytes()
    magic, ver, v_cnt, i_cnt, tex_w, tex_h, *_ = struct.unpack_from("<6I6f", data, 0)
    if magic != 0x32425333:
        raise ValueError(f"Unsupported magic 0x{magic:08X} in {in_path}")

    tex_off = 48 + v_cnt * 56 + i_cnt * 2
    atlas = np.frombuffer(data, dtype=np.uint8, count=tex_w * tex_h * 4, offset=tex_off).reshape(tex_h, tex_w, 4)
    half_w, half_h = tex_w // 2, tex_h // 2

    for s in range(4):
        qx = (s % 2) * half_w
        qy = (s // 2) * half_h
        slice_rgba = atlas[qy:qy + half_h, qx:qx + half_w].copy()
        if not args.keep_specular_alpha:
            slice_rgba[:, :, 3] = 255
        out_p = out_dir / f"slice_{s}.png"
        Image.fromarray(slice_rgba).save(out_p)
        print(f"Wrote {out_p} ({half_w}x{half_h})")

    eye_w, eye_h = max(1, half_w // 8), max(1, half_h // 8)
    eye_rgba = atlas[eye_h:eye_h * 2, half_w:half_w + eye_w].copy()
    if not args.keep_specular_alpha:
        eye_rgba[:, :, 3] = 255
    eye_p = out_dir / "eyes.png"
    Image.fromarray(eye_rgba).save(eye_p)
    print(f"Wrote {eye_p} ({eye_w}x{eye_h})")

    if args.save_atlas:
        atlas_out = atlas.copy()
        if not args.keep_specular_alpha:
            atlas_out[:, :, 3] = 255
        atlas_p = out_dir / "atlas.png"
        Image.fromarray(atlas_out).save(atlas_p)
        print(f"Wrote {atlas_p} ({tex_w}x{tex_h})")


def main():
    parser = argparse.ArgumentParser(description="glTF (.glb/.gltf) / Blender (.blend) <-> Skate 3 Character Tool")
    sub = parser.add_subparsers(dest="cmd", required=True)

    p_to = sub.add_parser("to-skate3", help="Convert a .glb/.gltf or .blend character into a Skate 3 S3B2 .bin / mod")
    p_to.add_argument("--input", "-i", required=True, help="Input .glb, .gltf, or .blend character file")
    p_to.add_argument("--out", "-o", help="Output .bin path (optional if --mod-id is provided)")
    p_to.add_argument("--atlas", help="Optional explicit 2048x2048 texture atlas PNG")
    p_to.add_argument("--mod-id", help="Generate a ready-to-play mod in mods/raw/<mod_id>/pack.json")
    p_to.add_argument("--mod-name", help="Display name for the Meta Menu Player Character Replacement selector")
    p_to.add_argument("--author", help="Author name for pack.json")
    p_to.add_argument("--description", help="Description for pack.json")
    p_to.add_argument("--pose-arms", type=float, default=0.0,
                      help="Rotate arms (in degrees) around LeftArm/RightArm bind joints (e.g. -35 to lower T-pose to A-pose)")
    p_to.add_argument("--auto-fit", action="store_true",
                      help="Automatically scale character height to --target-height and plant feet at y=0")
    p_to.add_argument("--target-height", type=float, default=1.73, help="Target character height in meters for --auto-fit")
    p_to.add_argument("--auto-weights", action="store_true",
                      help="Force automatic nearest-joint skinning weights onto Skate 3's 70-bone skeleton")
    p_to.add_argument("--add-eyeballs", action="store_true",
                      help="Add procedural backup eyeballs weighted to Head (useful for Frostbite head-shell meshes)")
    p_to.add_argument("--no-bake-shapekeys", action="store_true",
                      help="Do not automatically set non-zero shape keys to 1.0 when importing a .blend file")
    p_to.set_defaults(func=cmd_to_skate3)

    p_from = sub.add_parser("from-skate3", help="Extract a Skate 3 character (.bin or .rx2) into a rigged .glb")
    p_from.add_argument("--input", "-i", help="Input Skate 3 S3B2 .bin character file")
    p_from.add_argument("--rx2", nargs="+", help="One or more stock Skate 3 .rx2 character files to combine")
    p_from.add_argument("--skeleton", choices=["skate3", "frostbite"], default="skate3",
                        help="Target armature in exported .glb: 'skate3' (70 canonical bones) or 'frostbite' (fb_bone_*)")
    p_from.add_argument("--out", "-o", required=True, help="Output .glb path")
    p_from.set_defaults(func=cmd_from_skate3)

    p_fb = sub.add_parser("to-frostbite", help="Convert a Skate 3 character (.bin or .rx2) directly into Frostbite MeshSet / ReSkate mod")
    p_fb.add_argument("--input", "-i", help="Input Skate 3 S3B2 .bin character file")
    p_fb.add_argument("--rx2", nargs="+", help="One or more stock Skate 3 .rx2 character files")
    p_fb.add_argument("--template-res", help="Template Frostbite mesh_res.bin")
    p_fb.add_argument("--template-chunk", help="Optional template LOD0 chunk")
    p_fb.add_argument("--out-dir", "-o", required=True, help="Output directory for Frostbite mesh_res.bin + real_lod0.bin + .cas")
    p_fb.add_argument("--template-mod", help="Source ReSkate Studio mod folder to clone when building a new mod")
    p_fb.add_argument("--out-mod", help="Output ReSkate Studio mod folder path (used with --template-mod)")
    p_fb.add_argument("--patch-mod", help="Existing ReSkate Studio mod folder to patch in-place")
    p_fb.add_argument("--costume-id", help="Unique standalone costume ID/name in the closet (e.g. 'EveEdit' -> 'Own_EveEdit')")
    p_fb.add_argument("--replace-costume", action="store_true",
                        help="Keep the template mod's costume ID instead of registering a new standalone costume")
    p_fb.add_argument("--mod-name", help="Mod name for manifest.json")
    p_fb.add_argument("--mod-author", help="Author name for manifest.json")
    p_fb.add_argument("--mod-description", help="Description for manifest.json")
    p_fb.set_defaults(func=cmd_to_frostbite)

    p_tex = sub.add_parser("extract-textures", help="Extract slice_0.png .. slice_3.png and eyes.png from a Skate 3 .bin for runtime texture modding")
    p_tex.add_argument("--input", "-i", required=True, help="Input Skate 3 S3B2 .bin character file")
    p_tex.add_argument("--out-dir", "-o", help="Output directory (defaults to the .bin file's directory)")
    p_tex.add_argument("--keep-specular-alpha", action="store_true",
                       help="Keep Skate 3's raw specular/roughness alpha channel instead of exporting opaque RGB PNGs")
    p_tex.add_argument("--save-atlas", action="store_true", help="Also export the combined 2048x2048 atlas.png")
    p_tex.set_defaults(func=cmd_extract_textures)

    args = parser.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
