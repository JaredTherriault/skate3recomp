#!/usr/bin/env python3
"""Bidirectional Frostbite (skate. 2025) <-> glTF 2.0 (.glb) Character Converter.

Subcommands:
  to-gltf   Extract a Frostbite MeshSet (from a ReSkate Studio mod folder or raw
            mesh_res.bin + chunk.bin + slice PNGs) into a rigged, textured .glb
            (with optional .obj/.mtl and 2x2 texture atlas export).
  from-gltf Convert a rigged .glb/.gltf (Frostbite fb_bone_*, Skate 3 70-bone,
            or standard humanoid rig) into a Frostbite MeshSet (mesh_res.bin +
            lod0_chunk.bin + .cas payload, with optional in-place ReSkate Studio
            mod folder patching).

Examples:
  # 1. Extract Frostbite MeshSet + slices into a rigged .glb:
  python3 tools/frostbite_gltf.py to-gltf \
      --mesh-res analysis/yorha2b/mesh_res.bin \
      --chunk analysis/yorha2b/real_lod0.bin \
      --textures-dir analysis/yorha2b \
      --out yorha2b.glb

  # 2. Extract directly from a ReSkate Studio mod folder:
  python3 tools/frostbite_gltf.py to-gltf \
      --mod-dir "/path/to/Skate/Mods/UniversalJiggle-YoRHa2B_Eve_v1" \
      --out yorha2b.glb

  # 3. Convert a .glb (Skate 3 or Frostbite rig) back to Frostbite MeshSet / mod:
  python3 tools/frostbite_gltf.py from-gltf \
      --input my_skater.glb \
      --template-res analysis/yorha2b/mesh_res.bin \
      --template-chunk analysis/yorha2b/real_lod0.bin \
      --out-dir build/frostbite_out
"""

import argparse
import hashlib
import io
import json
import math
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import uuid
from pathlib import Path

import numpy as np
from PIL import Image

REPO_ROOT = Path(__file__).resolve().parents[1]

# ---------------------------------------------------------------------------
# Canonical Skate 3 70-Bone Skeleton & Frostbite (skate. 2025) Bone Mapping
# ---------------------------------------------------------------------------

S3_BONE_NAMES = [
    "Hips", "Spine", "Spine1", "Spine2", "Spine3", "Neck", "Neck1", "Head",
    "LeftShoulder", "LeftArm", "LeftArmTwist", "LeftForeArm", "LeftForeArmTwist", "LeftForeArmTwist1", "LeftHand",
    "LeftHandIndex1", "LeftHandIndex2", "LeftHandIndex3", "LeftHandMiddle1", "LeftHandMiddle2", "LeftHandMiddle3",
    "LeftInHandPinky", "LeftHandPinky1", "LeftHandPinky2", "LeftHandPinky3", "LeftInHandRing", "LeftHandRing1",
    "LeftHandRing2", "LeftHandRing3", "LeftHandThumb1", "LeftHandThumb2", "LeftHandThumb3",
    "RightShoulder", "RightArm", "RightArmTwist", "RightForeArm", "RightForeArmTwist", "RightForeArmTwist1", "RightHand",
    "RightHandIndex1", "RightHandIndex2", "RightHandIndex3", "RightHandMiddle1", "RightHandMiddle2", "RightHandMiddle3",
    "RightInHandPinky", "RightHandPinky1", "RightHandPinky2", "RightHandPinky3", "RightInHandRing", "RightHandRing1",
    "RightHandRing2", "RightHandRing3", "RightHandThumb1", "RightHandThumb2", "RightHandThumb3",
    "RightShoulderHlp", "LeftShoulderHlp",
    "LeftUpLeg", "LeftLeg", "LeftFoot", "LeftToeBase", "LeftUpLegTwist",
    "RightUpLeg", "RightLeg", "RightFoot", "RightToeBase", "RightUpLegTwist", "RightUpLegHlp", "LeftUpLegHlp",
]

S3_BONE_INDEX = {name: idx for idx, name in enumerate(S3_BONE_NAMES)}

S3_PARENT_BONE = [
    -1, 0,  1,  2,  3,  4,  5,  6,  4,  8,  9,  9,  11, 12, 11, 14, 15, 16,
    14, 18, 19, 14, 21, 22, 23, 14, 25, 26, 27, 14, 29, 30, 4,  32, 33, 33,
    35, 36, 35, 38, 39, 40, 38, 42, 43, 38, 45, 46, 47, 38, 49, 50, 51, 38,
    53, 54, 32, 8,  0,  58, 59, 60, 58, 0,  63, 64, 65, 63, 63, 58,
]

S3_BIND_POSITIONS = [
    (-0.0, 0.921, 0.0), (-0.0, 1.01329, -0.01782), (-0.0, 1.11263, -0.0162), (-0.0, 1.2202, -0.02592), (-0.0, 1.35035, -0.05025),
    (-0.0, 1.48671, -0.03592), (-0.0, 1.54248, -0.01688), (-0.0, 1.58479, -0.00775), (0.03071, 1.4589, -0.00163), (0.18, 1.43, -0.04179),
    (0.27074, 1.32419, -0.02941), (0.37241, 1.20534, -0.01165), (0.4202, 1.1496, 0.02639), (0.4631, 1.09946, 0.06101), (0.51261, 1.04174, 0.10007),
    (0.5863, 1.01082, 0.158), (0.6185, 0.98821, 0.1748), (0.63185, 0.97085, 0.18332), (0.59301, 1.00713, 0.13233), (0.6277, 0.97989, 0.14982),
    (0.64557, 0.95867, 0.15895), (0.55405, 1.02817, 0.08601), (0.58735, 0.9942, 0.08658), (0.60932, 0.9679, 0.09198), (0.62042, 0.95121, 0.09662),
    (0.55268, 1.03316, 0.09796), (0.59559, 1.00107, 0.10899), (0.62426, 0.96889, 0.12028), (0.63529, 0.9515, 0.12549), (0.51066, 1.03124, 0.13808),
    (0.52374, 1.02204, 0.17155), (0.53981, 1.0051, 0.19995), (-0.03071, 1.4589, -0.00163), (-0.18, 1.43, -0.04179), (-0.27075, 1.3242, -0.02942),
    (-0.37241, 1.20534, -0.01165), (-0.42016, 1.14961, 0.02645), (-0.46303, 1.09949, 0.06113), (-0.51261, 1.04174, 0.10007), (-0.5863, 1.01082, 0.158),
    (-0.6185, 0.98821, 0.1748), (-0.63185, 0.97085, 0.18332), (-0.59301, 1.00713, 0.13233), (-0.6277, 0.97989, 0.14982), (-0.64557, 0.95867, 0.15895),
    (-0.55405, 1.02817, 0.08601), (-0.58735, 0.9942, 0.08658), (-0.60932, 0.9679, 0.09198), (-0.62042, 0.95121, 0.09662), (-0.55268, 1.03316, 0.09796),
    (-0.59559, 1.00107, 0.10899), (-0.62426, 0.96889, 0.12028), (-0.63529, 0.9515, 0.12549), (-0.51066, 1.03124, 0.13808), (-0.52374, 1.02204, 0.17155),
    (-0.53981, 1.0051, 0.19995), (-0.03071, 1.4589, -0.00163), (0.03071, 1.4589, -0.00163), (0.08118, 0.93111, -0.0), (0.10375, 0.51165, 0.00493),
    (0.11237, 0.11004, -0.03538), (0.11237, 0.03066, 0.10902), (0.09246, 0.72138, 0.00247), (-0.08118, 0.93111, 0.0), (-0.10375, 0.51165, 0.00493),
    (-0.11237, 0.11004, -0.03538), (-0.11237, 0.03066, 0.10902), (-0.09246, 0.72138, 0.00247), (-0.08118, 0.93111, 0.0), (0.08118, 0.93111, 0.0),
]

# Frostbite (skate. 2025) 115 bone IDs -> Skate 3 canonical bone index (0..69)
FB_TO_S3 = {
    7: 0, 39: 0, 40: 0, 41: 0,            # Hips / Pelvis / Skirt root
    42: 1,                                 # Spine
    43: 2,                                 # Spine1
    44: 3,                                 # Spine2
    45: 4, 207: 4, 329: 4,                 # Spine3 / UpperChest
    46: 5, 198: 5, 200: 5, 202: 5, 325: 5, 327: 5,  # Neck / Collar
    199: 6, 201: 6,                        # Neck1
    47: 7, 103: 7, 194: 7, 203: 7, 204: 7, 205: 7, 206: 7, 326: 7, 328: 7,  # Head / Hair
    208: 8, 338: 57,                       # LeftShoulder / LeftShoulderHlp
    209: 9, 263: 9, 266: 10, 264: 10, 265: 10,  # LeftArm / LeftArmTwist
    210: 11, 257: 11, 258: 12, 261: 12, 262: 12, 259: 13, 260: 13,  # LeftForeArm + Twist
    212: 14,                               # LeftHand
    248: 29, 249: 30, 250: 31,             # LeftHandThumb1..3
    215: 15, 216: 16, 217: 17,             # LeftHandIndex1..3
    223: 18, 224: 19, 225: 20,             # LeftHandMiddle1..3
    231: 25, 232: 26, 233: 27, 234: 28,    # LeftInHandRing..LeftHandRing3
    239: 21, 240: 22, 241: 23, 242: 24,    # LeftInHandPinky..LeftHandPinky3
    75: 32, 205: 56,                       # RightShoulder / RightShoulderHlp
    76: 33, 130: 33, 133: 34, 131: 34, 132: 34,  # RightArm / RightArmTwist
    77: 35, 124: 35, 125: 36, 128: 36, 129: 36, 126: 37, 127: 37,  # RightForeArm + Twist
    79: 38,                                # RightHand
    115: 53, 116: 54, 117: 55,             # RightHandThumb1..3
    82: 39, 83: 40, 84: 41,                # RightHandIndex1..3
    90: 42, 91: 43, 92: 44,                # RightHandMiddle1..3
    98: 49, 99: 50, 100: 51, 101: 52,      # RightInHandRing..RightHandRing3
    106: 45, 107: 46, 108: 47, 109: 48,    # RightInHandPinky..RightHandPinky3
    341: 58, 371: 69, 372: 58, 373: 62, 374: 62,  # LeftUpLeg + Hlp + Twist
    342: 59, 362: 59, 363: 59, 367: 59, 369: 59,  # LeftLeg
    343: 60, 361: 60,                      # LeftFoot
    344: 61,                               # LeftToeBase
    8: 63, 35: 68, 36: 63, 37: 67, 38: 67,        # RightUpLeg + Hlp + Twist
    9: 64, 26: 64, 27: 64, 31: 64, 33: 64,        # RightLeg
    10: 65, 25: 65,                        # RightFoot
    11: 66,                                # RightToeBase
}

# Primary 1-to-1 inverse: Skate 3 canonical bone index (0..69) -> primary Frostbite bone ID
S3_TO_FB = {
    0: 7,    # Hips
    1: 42,   # Spine
    2: 43,   # Spine1
    3: 44,   # Spine2
    4: 45,   # Spine3
    5: 46,   # Neck
    6: 199,  # Neck1
    7: 47,   # Head
    8: 208,  # LeftShoulder
    9: 209,  # LeftArm
    10: 266, # LeftArmTwist
    11: 210, # LeftForeArm
    12: 258, # LeftForeArmTwist
    13: 259, # LeftForeArmTwist1
    14: 212, # LeftHand
    15: 215, # LeftHandIndex1
    16: 216, # LeftHandIndex2
    17: 217, # LeftHandIndex3
    18: 223, # LeftHandMiddle1
    19: 224, # LeftHandMiddle2
    20: 225, # LeftHandMiddle3
    21: 239, # LeftInHandPinky
    22: 240, # LeftHandPinky1
    23: 241, # LeftHandPinky2
    24: 242, # LeftHandPinky3
    25: 231, # LeftInHandRing
    26: 232, # LeftHandRing1
    27: 233, # LeftHandRing2
    28: 234, # LeftHandRing3
    29: 248, # LeftHandThumb1
    30: 249, # LeftHandThumb2
    31: 250, # LeftHandThumb3
    32: 75,  # RightShoulder
    33: 76,  # RightArm
    34: 133, # RightArmTwist
    35: 77,  # RightForeArm
    36: 125, # RightForeArmTwist
    37: 126, # RightForeArmTwist1
    38: 79,  # RightHand
    39: 82,  # RightHandIndex1
    40: 83,  # RightHandIndex2
    41: 84,  # RightHandIndex3
    42: 90,  # RightHandMiddle1
    43: 91,  # RightHandMiddle2
    44: 92,  # RightHandMiddle3
    45: 106, # RightInHandPinky
    46: 107, # RightHandPinky1
    47: 108, # RightHandPinky2
    48: 109, # RightHandPinky3
    49: 98,  # RightInHandRing
    50: 99,  # RightHandRing1
    51: 100, # RightHandRing2
    52: 101, # RightHandRing3
    53: 115, # RightHandThumb1
    54: 116, # RightHandThumb2
    55: 117, # RightHandThumb3
    56: 75,  # RightShoulderHlp
    57: 338, # LeftShoulderHlp
    58: 341, # LeftUpLeg
    59: 342, # LeftLeg
    60: 343, # LeftFoot
    61: 344, # LeftToeBase
    62: 373, # LeftUpLegTwist
    63: 8,   # RightUpLeg
    64: 9,   # RightLeg
    65: 10,  # RightFoot
    66: 11,  # RightToeBase
    67: 37,  # RightUpLegTwist
    68: 35,  # RightUpLegHlp
    69: 371, # LeftUpLegHlp
}

# Common humanoid bone alias table (lowercased, stripped of mixamorig:/bip01/etc. -> Skate 3 index)
HUMANOID_ALIASES = {
    "hips": 0, "pelvis": 0, "root": 0, "c_traj": 0,
    "spine": 1, "spine01": 1, "spine_01": 1, "abdomen": 1,
    "spine1": 2, "spine02": 2, "spine_02": 2, "chest": 2,
    "spine2": 3, "spine03": 3, "spine_03": 3, "upperchest": 4, "upper_chest": 4, "spine3": 4, "spine04": 4, "spine_04": 4,
    "neck": 5, "neck01": 5, "neck_01": 5,
    "neck1": 6, "neck02": 6, "neck_02": 6,
    "head": 7,
    "leftshoulder": 8, "shoulder_l": 8, "clavicle_l": 8, "l_clavicle": 8, "l_shoulder": 8,
    "leftarm": 9, "upperarm_l": 9, "upper_arm_l": 9, "l_upperarm": 9, "leftupperarm": 9,
    "leftarmtwist": 10, "upperarm_twist_l": 10,
    "leftforearm": 11, "lowerarm_l": 11, "forearm_l": 11, "lower_arm_l": 11, "l_forearm": 11, "leftlowerarm": 11,
    "leftforearmtwist": 12, "lowerarm_twist_l": 12,
    "leftforearmtwist1": 13,
    "lefthand": 14, "hand_l": 14, "l_hand": 14, "wrist_l": 14,
    "lefthandindex1": 15, "index1_l": 15, "f_index01_l": 15, "leftindexproximal": 15,
    "lefthandindex2": 16, "index2_l": 16, "f_index02_l": 16, "leftindexintermediate": 16,
    "lefthandindex3": 17, "index3_l": 17, "f_index03_l": 17, "leftindexdistal": 17,
    "lefthandmiddle1": 18, "middle1_l": 18, "f_middle01_l": 18, "leftmiddleproximal": 18,
    "lefthandmiddle2": 19, "middle2_l": 19, "f_middle02_l": 19, "leftmiddleintermediate": 19,
    "lefthandmiddle3": 20, "middle3_l": 20, "f_middle03_l": 20, "leftmiddledistal": 20,
    "leftinhandpinky": 21, "pinky0_l": 21,
    "lefthandpinky1": 22, "pinky1_l": 22, "f_pinky01_l": 22, "leftlittleproximal": 22,
    "lefthandpinky2": 23, "pinky2_l": 23, "f_pinky02_l": 23, "leftlittleintermediate": 23,
    "lefthandpinky3": 24, "pinky3_l": 24, "f_pinky03_l": 24, "leftlittledistal": 24,
    "leftinhandring": 25, "ring0_l": 25,
    "lefthandring1": 26, "ring1_l": 26, "f_ring01_l": 26, "leftringproximal": 26,
    "lefthandring2": 27, "ring2_l": 27, "f_ring02_l": 27, "leftringintermediate": 27,
    "lefthandring3": 28, "ring3_l": 28, "f_ring03_l": 28, "leftringdistal": 28,
    "lefthandthumb1": 29, "thumb1_l": 29, "thumb01_l": 29, "leftthumbproximal": 29,
    "lefthandthumb2": 30, "thumb2_l": 30, "thumb02_l": 30, "leftthumbintermediate": 30,
    "lefthandthumb3": 31, "thumb3_l": 31, "thumb03_l": 31, "leftthumbdistal": 31,
    "rightshoulder": 32, "shoulder_r": 32, "clavicle_r": 32, "r_clavicle": 32, "r_shoulder": 32,
    "rightarm": 33, "upperarm_r": 33, "upper_arm_r": 33, "r_upperarm": 33, "rightupperarm": 33,
    "rightarmtwist": 34, "upperarm_twist_r": 34,
    "rightforearm": 35, "lowerarm_r": 35, "forearm_r": 35, "lower_arm_r": 35, "r_forearm": 35, "rightlowerarm": 35,
    "rightforearmtwist": 36, "lowerarm_twist_r": 36,
    "rightforearmtwist1": 37,
    "righthand": 38, "hand_r": 38, "r_hand": 38, "wrist_r": 38,
    "righthandindex1": 39, "index1_r": 39, "f_index01_r": 39, "rightindexproximal": 39,
    "righthandindex2": 40, "index2_r": 40, "f_index02_r": 40, "rightindexintermediate": 40,
    "righthandindex3": 41, "index3_r": 41, "f_index03_r": 41, "rightindexdistal": 41,
    "righthandmiddle1": 42, "middle1_r": 42, "f_middle01_r": 42, "rightmiddleproximal": 42,
    "righthandmiddle2": 43, "middle2_r": 43, "f_middle02_r": 43, "rightmiddleintermediate": 43,
    "righthandmiddle3": 44, "middle3_r": 44, "f_middle03_r": 44, "rightmiddledistal": 44,
    "rightinhandpinky": 45, "pinky0_r": 45,
    "righthandpinky1": 46, "pinky1_r": 46, "f_pinky01_r": 46, "rightlittleproximal": 46,
    "righthandpinky2": 47, "pinky2_r": 47, "f_pinky02_r": 47, "rightlittleintermediate": 47,
    "righthandpinky3": 48, "pinky3_r": 48, "f_pinky03_r": 48, "rightlittledistal": 48,
    "rightinhandring": 49, "ring0_r": 49,
    "righthandring1": 50, "ring1_r": 50, "f_ring01_r": 50, "rightringproximal": 50,
    "righthandring2": 51, "ring2_r": 51, "f_ring02_r": 51, "rightringintermediate": 51,
    "righthandring3": 52, "ring3_r": 52, "f_ring03_r": 52, "rightringdistal": 52,
    "righthandthumb1": 53, "thumb1_r": 53, "thumb01_r": 53, "rightthumbproximal": 53,
    "righthandthumb2": 54, "thumb2_r": 54, "thumb02_r": 54, "rightthumbintermediate": 54,
    "righthandthumb3": 55, "thumb3_r": 55, "thumb03_r": 55, "rightthumbdistal": 55,
    "rightshoulderhlp": 56,
    "leftshoulderhlp": 57,
    "leftupleg": 58, "thigh_l": 58, "upperleg_l": 58, "upper_leg_l": 58, "l_thigh": 58, "leftupperleg": 58,
    "leftleg": 59, "calf_l": 59, "shin_l": 59, "lowerleg_l": 59, "lower_leg_l": 59, "l_calf": 59, "leftlowerleg": 59,
    "leftfoot": 60, "foot_l": 60, "l_foot": 60, "ankle_l": 60,
    "lefttoebase": 61, "toe_l": 61, "ball_l": 61, "l_toe0": 61, "lefttoes": 61,
    "leftuplegtwist": 62, "thigh_twist_l": 62,
    "rightupleg": 63, "thigh_r": 63, "upperleg_r": 63, "upper_leg_r": 63, "r_thigh": 63, "rightupperleg": 63,
    "rightleg": 64, "calf_r": 64, "shin_r": 64, "lowerleg_r": 64, "lower_leg_r": 64, "r_calf": 64, "rightlowerleg": 64,
    "rightfoot": 65, "foot_r": 65, "r_foot": 65, "ankle_r": 65,
    "righttoebase": 66, "toe_r": 66, "ball_r": 66, "r_toe0": 66, "righttoes": 66,
    "rightuplegtwist": 67, "thigh_twist_r": 67,
    "rightupleghlp": 68,
    "leftupleghlp": 69,
}

STRIDES = [12, 8, 8, 4, 4, 8, 8, 4]  # 56 bytes/vertex across 8 SoA streams


def resolve_bone_name_to_s3(name: str):
    """Map a bone name (Skate 3, Frostbite fb_bone_*, Mixamo, VRM, Rigify) to Skate 3 bone index 0..69."""
    if name in S3_BONE_INDEX:
        return S3_BONE_INDEX[name]
    if name.startswith("fb_bone_"):
        try:
            fb_id = int(name.split("_")[-1])
            return FB_TO_S3.get(fb_id)
        except ValueError:
            pass
    clean = name.lower()
    for prefix in ("mixamorig:", "mixamorig_", "bip01_", "bip01 ", "valvebiped.bip01_", "def-", "org-"):
        if clean.startswith(prefix):
            clean = clean[len(prefix):]
    clean_nounder = clean.replace(".", "_").replace("-", "_").replace(" ", "_")
    if clean_nounder in HUMANOID_ALIASES:
        return HUMANOID_ALIASES[clean_nounder]
    if clean_nounder.replace("_", "") in HUMANOID_ALIASES:
        return HUMANOID_ALIASES[clean_nounder.replace("_", "")]
    return None


def resolve_bone_name_to_fb(name: str):
    """Map a bone name to Frostbite bone ID (0..407)."""
    if name.startswith("fb_bone_"):
        try:
            return int(name.split("_")[-1])
        except ValueError:
            pass
    s3_idx = resolve_bone_name_to_s3(name)
    if s3_idx is not None:
        return S3_TO_FB.get(s3_idx, 7)
    return None


# ---------------------------------------------------------------------------
# Frostbite CAS Framing & MeshSet Reader/Writer
# ---------------------------------------------------------------------------

def wrap_uncompressed_cas(raw_bytes: bytes) -> bytes:
    """Wrap raw payload bytes into uncompressed Frostbite CAS 64KB blocks (comp_type=0)."""
    out = bytearray()
    pos = 0
    n = len(raw_bytes)
    while pos < n:
        blk = min(0x10000, n - pos)
        packed = ((blk & 0xFFFFFF) << 32) | (0x7 << 20) | (blk & 0xFFFFF)
        out += struct.pack(">Q", packed)
        out += raw_bytes[pos:pos + blk]
        pos += blk
    return bytes(out)


def read_cas_slice(cas_path: Path, offset: int, size: int) -> bytes:
    """Decompress a slice from a Frostbite .cas archive (supports uncompressed blocks + linux_cas_tool fallback)."""
    with open(cas_path, "rb") as f:
        f.seek(offset)
        buf = f.read(size)
    out = bytearray()
    pos = 0
    needs_oodle = False
    while pos + 8 <= len(buf):
        dec = int.from_bytes(buf[pos:pos + 4], "big") & 0xFFFFFF
        if dec == 0:
            dec = 0x10000
        comp_raw = int.from_bytes(buf[pos + 4:pos + 6], "little")
        enc = int.from_bytes(buf[pos + 6:pos + 8], "big") + (((comp_raw >> 8) & 0xF) << 16)
        comp = comp_raw & 0x7F
        if comp != 0:
            needs_oodle = True
            break
        out += buf[pos + 8:pos + 8 + enc]
        pos += 8 + enc
    if not needs_oodle:
        return bytes(out)

    # Use linux_cas_tool if available
    cas_tool = REPO_ROOT / "analysis" / "yorha2b" / "linux_cas_tool"
    if not cas_tool.exists():
        raise RuntimeError(
            f"CAS slice at {offset:#x} uses Oodle compression (comp={comp:#x}) and {cas_tool} was not found."
        )
    with tempfile.TemporaryDirectory() as td:
        in_p = Path(td) / "in.cas"
        out_p = Path(td) / "out.bin"
        in_p.write_bytes(buf)
        subprocess.run([str(cas_tool), str(in_p), str(out_p)], check=True, stdout=subprocess.DEVNULL)
        return out_p.read_bytes()


def decode_frostbite_mesh(res_bytes: bytes, chunk_bytes: bytes, lod_idx: int = 0):
    """Decode a Frostbite MeshSet LOD chunk into positions, normals, uvs, slices, and 8-bone skin weights."""
    lb = 0x00D0 + lod_idx * 0x00C0
    nsec, sec_off = struct.unpack_from("<IQ", res_bytes, lb + 0x18)
    _, _, ib_sz, vb_sz = struct.unpack_from("<4I", res_bytes, lb + 0x60)

    all_pos = []
    all_nrm = []
    all_uv = []
    all_slice = []
    all_bi = []
    all_bw = []
    all_tris = []
    vert_base = 0

    for s in range(nsec):
        sb = sec_off + s * 0x180
        prim_cnt, start_idx, v_off, v_cnt = struct.unpack_from("<4I", res_bytes, sb + 0x30)
        if v_cnt <= 3:
            continue

        sec_pos = np.zeros((v_cnt, 3), dtype=np.float32)
        sec_uv = np.zeros((v_cnt, 2), dtype=np.float32)
        sec_sl = np.zeros(v_cnt, dtype=np.int32)
        sec_bi = np.zeros((v_cnt, 8), dtype=np.uint16)
        sec_bw = np.zeros((v_cnt, 8), dtype=np.uint8)

        off0 = v_off
        off1 = off0 + STRIDES[0] * v_cnt
        off2 = off1 + STRIDES[1] * v_cnt
        off3 = off2 + STRIDES[2] * v_cnt
        off4 = off3 + STRIDES[3] * v_cnt
        off5 = off4 + STRIDES[4] * v_cnt

        for v in range(v_cnt):
            sec_pos[v] = struct.unpack_from("<3f", chunk_bytes, off0 + v * 12)
            bi0 = struct.unpack_from("<4H", chunk_bytes, off1 + v * 8)
            bi1 = struct.unpack_from("<4H", chunk_bytes, off2 + v * 8)
            bw0 = list(struct.unpack_from("<4B", chunk_bytes, off3 + v * 4))
            bw1 = list(struct.unpack_from("<4B", chunk_bytes, off4 + v * 4))
            u0, v0 = struct.unpack_from("<2f", chunk_bytes, off5 + v * 8)
            sl = max(0, min(3, int(-math.floor(v0))))
            v_local = v0 - math.floor(v0)
            sec_uv[v] = (u0, v_local)
            sec_sl[v] = sl
            sec_bi[v, :4] = bi0
            if (sum(bw0) + sum(bw1) == 255) and all(b < 416 for b, w in zip(bi1, bw1) if w > 0):
                sec_bi[v, 4:] = bi1
                sec_bw[v, :4] = bw0
                sec_bw[v, 4:] = bw1
            else:
                wsum = sum(bw0)
                if wsum == 0:
                    bw0 = [255, 0, 0, 0]
                elif wsum != 255:
                    bw0 = [int(round(w * 255.0 / wsum)) for w in bw0]
                    bw0[0] = max(0, min(255, bw0[0] + (255 - sum(bw0))))
                sec_bw[v, :4] = bw0

        ib_off = vb_sz + start_idx * 2
        idx = np.frombuffer(chunk_bytes, dtype="<u2", count=prim_cnt * 3, offset=ib_off).reshape(-1, 3).astype(np.int32)

        v0 = sec_pos[idx[:, 0]]
        v1 = sec_pos[idx[:, 1]]
        v2 = sec_pos[idx[:, 2]]
        fn = np.cross(v1 - v0, v2 - v0)
        sec_nrm = np.zeros_like(sec_pos)
        np.add.at(sec_nrm, idx[:, 0], fn)
        np.add.at(sec_nrm, idx[:, 1], fn)
        np.add.at(sec_nrm, idx[:, 2], fn)
        lens = np.linalg.norm(sec_nrm, axis=1, keepdims=True)
        sec_nrm = np.where(lens > 1e-8, sec_nrm / np.maximum(lens, 1e-8), np.array([0.0, 1.0, 0.0], dtype=np.float32))

        all_pos.append(sec_pos)
        all_nrm.append(sec_nrm.astype(np.float32))
        all_uv.append(sec_uv)
        all_slice.append(sec_sl)
        all_bi.append(sec_bi)
        all_bw.append(sec_bw)
        all_tris.append(idx + vert_base)
        vert_base += v_cnt

    pos = np.concatenate(all_pos, axis=0)
    nrm = np.concatenate(all_nrm, axis=0)
    uv = np.concatenate(all_uv, axis=0)
    sl = np.concatenate(all_slice, axis=0)
    bi = np.concatenate(all_bi, axis=0)
    bw = np.concatenate(all_bw, axis=0)
    tris = np.concatenate(all_tris, axis=0)
    return pos, nrm, uv, sl, bi, bw, tris


def write_rigged_glb(
    out_path: Path,
    name: str,
    pos: np.ndarray,
    nrm: np.ndarray,
    uv: np.ndarray,
    sl: np.ndarray,
    bi: np.ndarray,
    bw: np.ndarray,
    tris: np.ndarray,
    slice_png_bytes: list[bytes],
    skeleton_mode: str = "frostbite",
):
    """Write a rigged glTF 2.0 binary (.glb) with either Frostbite fb_bone_* or Skate 3 70-bone armature."""
    slice_names = ["Slice0_Outfit", "Slice1_Boots", "Slice2_Face", "Slice3_BodyHair"]
    if len(slice_png_bytes) == 1:
        slice_names = ["Character_Atlas"]

    if skeleton_mode == "skate3":
        num_bones = len(S3_BONE_NAMES)
        bone_names = list(S3_BONE_NAMES)
        bone_heads = np.array(S3_BIND_POSITIONS, dtype=np.float32)
        parent_map = list(S3_PARENT_BONE)

        # Remap 8-bone Frostbite weights into top-4 Skate 3 weights
        V = len(pos)
        j0 = np.zeros((V, 4), dtype=np.uint16)
        w0 = np.zeros((V, 4), dtype=np.float32)
        for v in range(V):
            accum = {}
            for k in range(bi.shape[1]):
                wt = int(bw[v, k])
                if wt == 0:
                    continue
                fb_b = int(bi[v, k])
                s3_b = FB_TO_S3.get(fb_b, 0)
                accum[s3_b] = accum.get(s3_b, 0) + wt
            if not accum:
                accum[0] = 255
            top4 = sorted(accum.items(), key=lambda x: -x[1])[:4]
            tot = sum(val for _, val in top4) or 1.0
            for idx_k, (b_idx, val) in enumerate(top4):
                j0[v, idx_k] = b_idx
                w0[v, idx_k] = val / tot
        j1 = None
        w1 = None
    else:
        # Frostbite skeleton mode (fb_bone_XXX positioned at weighted vertex centroids)
        used_mask = bw > 0
        unique_bones = sorted(set(int(b) for b in bi[used_mask].flatten()))
        if not unique_bones:
            unique_bones = [7]
        bone_remap = {b: i for i, b in enumerate(unique_bones)}
        num_bones = len(unique_bones)
        bone_names = [f"fb_bone_{b:03d}" for b in unique_bones]
        parent_map = [-1] + [0] * (num_bones - 1)

        bone_heads = np.zeros((num_bones, 3), dtype=np.float32)
        for b, j_idx in bone_remap.items():
            mask = (bi == b) & (bw > 0)
            v_indices, k_indices = np.where(mask)
            if len(v_indices) > 0:
                weights = bw[v_indices, k_indices].astype(np.float64)
                bone_heads[j_idx] = np.average(pos[v_indices], axis=0, weights=weights)

        remapped_bi = np.zeros_like(bi, dtype=np.uint16)
        for k in range(bi.shape[1]):
            for v in range(len(pos)):
                if bw[v, k] > 0:
                    remapped_bi[v, k] = bone_remap.get(int(bi[v, k]), 0)
        j0 = remapped_bi[:, :4]
        j1 = remapped_bi[:, 4:8] if bi.shape[1] >= 8 else None
        w_f = bw.astype(np.float32) / 255.0
        w_sum = w_f.sum(axis=1, keepdims=True)
        w_f = np.where(w_sum > 0, w_f / np.maximum(w_sum, 1e-8), w_f)
        w0 = w_f[:, :4]
        w1 = w_f[:, 4:8] if bw.shape[1] >= 8 else None

    bin_blob = bytearray()
    buffer_views = []
    accessors = []

    def add_bv(data_bytes: bytes, target=None):
        pad = (4 - (len(bin_blob) % 4)) % 4
        if pad:
            bin_blob.extend(b"\x00" * pad)
        offset = len(bin_blob)
        bin_blob.extend(data_bytes)
        bv = {"buffer": 0, "byteOffset": offset, "byteLength": len(data_bytes)}
        if target is not None:
            bv["target"] = target
        idx = len(buffer_views)
        buffer_views.append(bv)
        return idx

    def add_acc(bv_idx, comp_type, count, acc_type, mins=None, maxs=None):
        acc = {
            "bufferView": bv_idx,
            "byteOffset": 0,
            "componentType": comp_type,
            "count": int(count),
            "type": acc_type,
        }
        if mins is not None:
            acc["min"] = [float(x) for x in mins]
        if maxs is not None:
            acc["max"] = [float(x) for x in maxs]
        idx = len(accessors)
        accessors.append(acc)
        return idx

    images = []
    textures = []
    materials = []
    for s, png_b in enumerate(slice_png_bytes):
        bv_img = add_bv(png_b)
        images.append({"bufferView": bv_img, "mimeType": "image/png", "name": slice_names[s]})
        textures.append({"source": s, "name": slice_names[s]})
        materials.append({
            "name": slice_names[s],
            "doubleSided": True,
            "pbrMetallicRoughness": {
                "baseColorTexture": {"index": s},
                "metallicFactor": 0.0,
                "roughnessFactor": 0.65,
            },
        })

    # Inverse bind matrices (column-major 4x4 translation(-bone_head))
    ibms = np.zeros((num_bones, 4, 4), dtype=np.float32)
    for j_idx in range(num_bones):
        ibms[j_idx] = np.eye(4, dtype=np.float32)
        ibms[j_idx, 3, :3] = -bone_heads[j_idx]
    ibm_acc = add_acc(add_bv(ibms.tobytes()), 5126, num_bones, "MAT4")

    primitives = []
    num_mats = len(slice_png_bytes)
    for s in range(num_mats):
        if num_mats == 1:
            s_tris = tris
        else:
            tri_sl = sl[tris[:, 0]]
            s_tris = tris[tri_sl == s]
        if len(s_tris) == 0:
            continue
        u_verts, inv = np.unique(s_tris.flatten(), return_inverse=True)
        local_tris = inv.reshape(-1, 3).astype(np.uint32)
        s_pos = pos[u_verts].astype(np.float32)
        s_nrm = nrm[u_verts].astype(np.float32)
        s_uv = uv[u_verts].astype(np.float32)
        s_j0 = j0[u_verts].astype(np.uint16)
        s_w0 = w0[u_verts].astype(np.float32)

        pos_acc = add_acc(add_bv(s_pos.tobytes(), 34962), 5126, len(u_verts), "VEC3", s_pos.min(axis=0), s_pos.max(axis=0))
        nrm_acc = add_acc(add_bv(s_nrm.tobytes(), 34962), 5126, len(u_verts), "VEC3")
        uv_acc = add_acc(add_bv(s_uv.tobytes(), 34962), 5126, len(u_verts), "VEC2")
        j0_acc = add_acc(add_bv(s_j0.tobytes(), 34962), 5123, len(u_verts), "VEC4")
        w0_acc = add_acc(add_bv(s_w0.tobytes(), 34962), 5126, len(u_verts), "VEC4")
        idx_acc = add_acc(add_bv(local_tris.tobytes(), 34963), 5125, local_tris.size, "SCALAR")

        attrs = {
            "POSITION": pos_acc,
            "NORMAL": nrm_acc,
            "TEXCOORD_0": uv_acc,
            "JOINTS_0": j0_acc,
            "WEIGHTS_0": w0_acc,
        }
        if j1 is not None and w1 is not None and np.any(w1[u_verts] > 0):
            s_j1 = j1[u_verts].astype(np.uint16)
            s_w1 = w1[u_verts].astype(np.float32)
            attrs["JOINTS_1"] = add_acc(add_bv(s_j1.tobytes(), 34962), 5123, len(u_verts), "VEC4")
            attrs["WEIGHTS_1"] = add_acc(add_bv(s_w1.tobytes(), 34962), 5126, len(u_verts), "VEC4")

        primitives.append({
            "attributes": attrs,
            "indices": idx_acc,
            "material": s,
            "mode": 4,
        })

    # Build node hierarchy: node 0 = Mesh, node 1 = Armature, nodes 2..2+num_bones-1 = Joints
    nodes = [
        {"name": name, "mesh": 0, "skin": 0},
        {"name": f"{name}_Armature", "children": []},
    ]
    children_lists = [[] for _ in range(num_bones)]
    root_joints = []
    for j_idx in range(num_bones):
        p_idx = parent_map[j_idx]
        if p_idx < 0:
            root_joints.append(2 + j_idx)
        else:
            children_lists[p_idx].append(2 + j_idx)
    nodes[1]["children"] = root_joints

    for j_idx in range(num_bones):
        p_idx = parent_map[j_idx]
        rel = bone_heads[j_idx] if p_idx < 0 else (bone_heads[j_idx] - bone_heads[p_idx])
        nd = {
            "name": bone_names[j_idx],
            "translation": [float(rel[0]), float(rel[1]), float(rel[2])],
        }
        if children_lists[j_idx]:
            nd["children"] = children_lists[j_idx]
        nodes.append(nd)

    gltf = {
        "asset": {"version": "2.0", "generator": "skate3recomp frostbite_gltf.py"},
        "scene": 0,
        "scenes": [{"name": "Scene", "nodes": [0, 1]}],
        "nodes": nodes,
        "skins": [{
            "name": f"{name}_Skin",
            "inverseBindMatrices": ibm_acc,
            "skeleton": 2,
            "joints": [2 + i for i in range(num_bones)],
        }],
        "meshes": [{"name": name, "primitives": primitives}],
        "materials": materials,
        "textures": textures,
        "images": images,
        "accessors": accessors,
        "bufferViews": buffer_views,
        "buffers": [{"byteLength": len(bin_blob)}],
    }

    json_bytes = json.dumps(gltf, separators=(",", ":")).encode("utf-8")
    json_pad = (4 - (len(json_bytes) % 4)) % 4
    json_bytes += b" " * json_pad

    bin_pad = (4 - (len(bin_blob) % 4)) % 4
    if bin_pad:
        bin_blob.extend(b"\x00" * bin_pad)

    total_len = 12 + 8 + len(json_bytes) + 8 + len(bin_blob)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    with open(out_path, "wb") as f:
        f.write(struct.pack("<III", 0x46546C67, 2, total_len))
        f.write(struct.pack("<II", len(json_bytes), 0x4E4F534A))
        f.write(json_bytes)
        f.write(struct.pack("<II", len(bin_blob), 0x004E4942))
        f.write(bin_blob)


# ---------------------------------------------------------------------------
# glTF 2.0 (.glb / .gltf) Reader
# ---------------------------------------------------------------------------

def load_gltf_mesh_and_skin(gltf_path: Path):
    """Read a rigged .glb or .gltf file into unified arrays:
    pos (V,3), nrm (V,3), uv (V,2), mat_idx (V,), joints (V,8), weights (V,8),
    tris (T,3), joint_names (list[str]), images (list[PIL.Image.Image]).
    """
    raw = gltf_path.read_bytes()
    if raw[:4] == b"glTF":
        _, _, total_len = struct.unpack_from("<III", raw, 0)
        j_len, j_type = struct.unpack_from("<II", raw, 12)
        gltf = json.loads(raw[20:20 + j_len].decode("utf-8"))
        b_off = 20 + j_len
        if b_off + 8 <= len(raw):
            b_len, _ = struct.unpack_from("<II", raw, b_off)
            bin_blob = raw[b_off + 8:b_off + 8 + b_len]
        else:
            bin_blob = b""
    else:
        gltf = json.loads(raw.decode("utf-8"))
        bin_blob = b""
        if gltf.get("buffers"):
            uri = gltf["buffers"][0].get("uri", "")
            if uri and not uri.startswith("data:"):
                bin_blob = (gltf_path.parent / uri).read_bytes()

    accessors = gltf.get("accessors", [])
    buffer_views = gltf.get("bufferViews", [])
    nodes = gltf.get("nodes", [])

    comp_dtype = {
        5120: np.int8,
        5121: np.uint8,
        5122: np.int16,
        5123: np.uint16,
        5125: np.uint32,
        5126: np.float32,
    }
    type_cnt = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4, "MAT4": 16}

    def read_acc(acc_idx):
        acc = accessors[acc_idx]
        bv = buffer_views[acc["bufferView"]]
        off = bv.get("byteOffset", 0) + acc.get("byteOffset", 0)
        cnt = acc["count"]
        nc = type_cnt[acc["type"]]
        dt = np.dtype(comp_dtype[acc["componentType"]]).newbyteorder("<")
        stride = bv.get("byteStride", 0)
        elem_sz = dt.itemsize * nc
        if stride and stride != elem_sz:
            arr = np.zeros((cnt, nc), dtype=dt)
            for i in range(cnt):
                arr[i] = np.frombuffer(bin_blob, dtype=dt, count=nc, offset=off + i * stride)
            return arr if nc > 1 else arr[:, 0]
        arr = np.frombuffer(bin_blob, dtype=dt, count=cnt * nc, offset=off)
        return arr.reshape(cnt, nc) if nc > 1 else arr

    # Extract joint names from first skin (or node list)
    joint_names = []
    if gltf.get("skins"):
        skin = gltf["skins"][0]
        for j_node in skin.get("joints", []):
            joint_names.append(nodes[j_node].get("name", f"bone_{j_node}"))

    # Extract embedded images
    pil_images = []
    for img_def in gltf.get("images", []):
        if "bufferView" in img_def:
            bv = buffer_views[img_def["bufferView"]]
            off = bv.get("byteOffset", 0)
            sz = bv["byteLength"]
            pil_images.append(Image.open(io.BytesIO(bin_blob[off:off + sz])).convert("RGBA"))
        elif "uri" in img_def and not img_def["uri"].startswith("data:"):
            img_p = gltf_path.parent / img_def["uri"]
            if img_p.exists():
                pil_images.append(Image.open(img_p).convert("RGBA"))

    all_pos, all_nrm, all_uv, all_mat, all_j, all_w, all_tris = [], [], [], [], [], [], []
    v_base = 0

    for mesh in gltf.get("meshes", []):
        for prim in mesh.get("primitives", []):
            attrs = prim["attributes"]
            if "POSITION" not in attrs:
                continue
            p_pos = read_acc(attrs["POSITION"]).astype(np.float32)
            V = len(p_pos)
            p_nrm = read_acc(attrs["NORMAL"]).astype(np.float32) if "NORMAL" in attrs else np.zeros((V, 3), dtype=np.float32)
            p_uv = read_acc(attrs["TEXCOORD_0"]).astype(np.float32) if "TEXCOORD_0" in attrs else np.zeros((V, 2), dtype=np.float32)

            p_j = np.zeros((V, 8), dtype=np.uint16)
            p_w = np.zeros((V, 8), dtype=np.float32)
            if "JOINTS_0" in attrs and "WEIGHTS_0" in attrs:
                j0 = read_acc(attrs["JOINTS_0"]).astype(np.uint16)
                w0 = read_acc(attrs["WEIGHTS_0"]).astype(np.float32)
                if w0.max() > 1.5:
                    w0 = w0 / 255.0
                p_j[:, :4] = j0
                p_w[:, :4] = w0
            else:
                p_w[:, 0] = 1.0
            if "JOINTS_1" in attrs and "WEIGHTS_1" in attrs:
                j1 = read_acc(attrs["JOINTS_1"]).astype(np.uint16)
                w1 = read_acc(attrs["WEIGHTS_1"]).astype(np.float32)
                if w1.max() > 1.5:
                    w1 = w1 / 255.0
                p_j[:, 4:8] = j1
                p_w[:, 4:8] = w1

            if "indices" in prim:
                idx = read_acc(prim["indices"]).astype(np.int32).reshape(-1, 3)
            else:
                idx = np.arange(V, dtype=np.int32).reshape(-1, 3)

            mat_id = prim.get("material", 0)
            all_pos.append(p_pos)
            all_nrm.append(p_nrm)
            all_uv.append(p_uv)
            all_mat.append(np.full(V, mat_id, dtype=np.int32))
            all_j.append(p_j)
            all_w.append(p_w)
            all_tris.append(idx + v_base)
            v_base += V

    pos = np.concatenate(all_pos, axis=0)
    nrm = np.concatenate(all_nrm, axis=0)
    uv = np.concatenate(all_uv, axis=0)
    mat = np.concatenate(all_mat, axis=0)
    joints = np.concatenate(all_j, axis=0)
    weights = np.concatenate(all_w, axis=0)
    tris = np.concatenate(all_tris, axis=0)
    return pos, nrm, uv, mat, joints, weights, tris, joint_names, pil_images


# ---------------------------------------------------------------------------
# glTF -> Frostbite MeshSet Packer
# ---------------------------------------------------------------------------

def pack_frostbite_meshset(
    pos: np.ndarray,
    nrm: np.ndarray,
    uv: np.ndarray,
    mat: np.ndarray,
    joints: np.ndarray,
    weights: np.ndarray,
    tris: np.ndarray,
    joint_names: list[str],
    template_res_bytes: bytes,
    template_chunk_bytes: bytes | None = None,
    from_atlas: bool = False,
):
    """Pack a rigged mesh into Frostbite MeshSet format (updated mesh_res.bin + LOD0 chunk bytes).

    Splits vertices across Sub[1] (FullbodyBottom_mat: slice 0 & 1) and Sub[5] (FullbodyTop_mat: slice 2 & 3),
    preserving dummy 3-vertex subsets Sub[0, 2, 3, 4] from the template so Frostbite's 6-subset material layout
    remains 100% intact.
    """
    V = len(pos)
    # Map each glTF joint index -> Frostbite bone ID
    joint_to_fb = []
    for j_idx, j_name in enumerate(joint_names):
        fb_id = resolve_bone_name_to_fb(j_name)
        if fb_id is None:
            fb_id = 7
        joint_to_fb.append(fb_id)
    if not joint_to_fb:
        joint_to_fb = [7]

    # Map vertex weights to Frostbite bone IDs + uint8 weights summing to 255
    fb_bi = np.zeros((V, 8), dtype=np.uint16)
    fb_bw = np.zeros((V, 8), dtype=np.uint8)
    for v in range(V):
        accum = {}
        for k in range(8):
            wt = float(weights[v, k])
            if wt <= 1e-5:
                continue
            j_idx = int(joints[v, k])
            fb_id = joint_to_fb[j_idx] if j_idx < len(joint_to_fb) else 7
            accum[fb_id] = accum.get(fb_id, 0.0) + wt
        if not accum:
            accum[7] = 1.0
        top8 = sorted(accum.items(), key=lambda x: -x[1])[:8]
        tot = sum(val for _, val in top8) or 1.0
        raw_u8 = [int(round((val / tot) * 255.0)) for _, val in top8]
        diff = 255 - sum(raw_u8)
        raw_u8[0] = max(0, min(255, raw_u8[0] + diff))
        for k, ((fb_id, _), u8_w) in enumerate(zip(top8, raw_u8)):
            fb_bi[v, k] = fb_id
            fb_bw[v, k] = u8_w

    # Determine Frostbite slice (0..3) and local UV (u0, v0 = v_local - slice_idx)
    num_mats = int(mat.max()) + 1 if len(mat) > 0 else 1
    u_out = np.zeros(V, dtype=np.float32)
    v_out = np.zeros(V, dtype=np.float32)
    slice_id = np.zeros(V, dtype=np.int32)

    if from_atlas or num_mats == 1:
        # Unpack 2x2 Skate 3 atlas UVs back into 4 Frostbite UDIM slices:
        # Top-Left (u<0.5, v<0.5) -> Slice 0
        # Top-Right (u>=0.5, v<0.5) -> Slice 1
        # Bottom-Left (u<0.5, v>=0.5) -> Slice 2
        # Bottom-Right (u>=0.5, v>=0.5) -> Slice 3
        for v in range(V):
            u_a = float(uv[v, 0]) % 1.0
            v_a = float(uv[v, 1]) % 1.0
            col = 1 if u_a >= 0.5 else 0
            row = 1 if v_a >= 0.5 else 0
            sl = row * 2 + col
            u_loc = (u_a - col * 0.5) * 2.0
            v_loc = min(0.9999, max(0.0001, (v_a - row * 0.5) * 2.0))
            slice_id[v] = sl
            u_out[v] = u_loc
            v_out[v] = v_loc - sl
    else:
        for v in range(V):
            sl = max(0, min(3, int(mat[v])))
            u_loc = float(uv[v, 0])
            v_loc = min(0.9999, max(0.0001, float(uv[v, 1]) % 1.0))
            slice_id[v] = sl
            u_out[v] = u_loc
            v_out[v] = v_loc - sl

    # Optional template chunk matching: preserve exact Sub[1]/Sub[5] split and Stream 7 tangentSpace
    v_sub_id = None
    v_ts4 = None
    if template_chunk_bytes is not None:
        try:
            from scipy.spatial import cKDTree
            nsec_t, sec_off_t = struct.unpack_from("<IQ", template_res_bytes, 0x00D0 + 0x18)
            tpl_pos, tpl_uv, tpl_sub, tpl_s7 = [], [], [], []
            for s in (1, 5):
                sb_t = sec_off_t + s * 0x180
                _, _, v_off_t, v_cnt_t = struct.unpack_from("<4I", template_res_bytes, sb_t + 0x30)
                if v_cnt_t <= 3:
                    continue
                soff_t = [v_off_t + sum(STRIDES[:k]) * v_cnt_t for k in range(8)]
                p_t = np.frombuffer(template_chunk_bytes, dtype="<f4", count=v_cnt_t * 3, offset=soff_t[0]).reshape(v_cnt_t, 3)
                u_t = np.frombuffer(template_chunk_bytes, dtype="<f4", count=v_cnt_t * 2, offset=soff_t[5]).reshape(v_cnt_t, 2)
                s7_t = np.frombuffer(template_chunk_bytes, dtype=np.uint8, count=v_cnt_t * 4, offset=soff_t[7]).reshape(v_cnt_t, 4)
                tpl_pos.append(p_t)
                tpl_uv.append(u_t)
                tpl_sub.append(np.full(v_cnt_t, s, dtype=np.int32))
                tpl_s7.append(s7_t)
            if tpl_pos:
                tpl_pos = np.concatenate(tpl_pos, axis=0)
                tpl_uv = np.concatenate(tpl_uv, axis=0)
                tpl_sub = np.concatenate(tpl_sub, axis=0)
                tpl_s7 = np.concatenate(tpl_s7, axis=0)
                # Query in 5D (UV weighted strongly + 3D position so lengthened hair ribbons still match their UV card)
                tpl_feat = np.column_stack([tpl_uv[:, 0] * 4.0, tpl_uv[:, 1] * 4.0, tpl_pos])
                in_feat = np.column_stack([u_out * 4.0, v_out * 4.0, pos])
                tree = cKDTree(tpl_feat)
                dists, idxs = tree.query(in_feat, k=1)
                if float(np.median(dists)) < 0.25:
                    v_sub_id = tpl_sub[idxs]
                    v_ts4 = tpl_s7[idxs].copy()
                    print(f"[Frostbite Packer] Inherited subset split & tangent space from template chunk (median dist={np.median(dists):.4f})")
        except Exception as ex:
            print(f"[Frostbite Packer] Note: template chunk lookup skipped ({ex})")

    if v_sub_id is not None:
        tri_sub = v_sub_id[tris[:, 0]]
        sub1_tris = tris[tri_sub == 1]
        sub5_tris = tris[tri_sub == 5]
    else:
        tri_sl = slice_id[tris[:, 0]]
        sub1_tris = tris[tri_sl <= 1]
        sub5_tris = tris[tri_sl >= 2]

    if len(sub1_tris) == 0 and len(sub5_tris) > 0:
        sub1_tris = sub5_tris[:1]
    elif len(sub5_tris) == 0 and len(sub1_tris) > 0:
        sub5_tris = sub1_tris[:1]

    def build_subset_streams(sub_tris):
        u_verts, inv = np.unique(sub_tris.flatten(), return_inverse=True)
        local_tris = inv.reshape(-1, 3).astype(np.uint16)
        v_cnt = len(u_verts)
        if v_cnt > 65535:
            raise ValueError(
                f"Subset vertex count ({v_cnt}) exceeds 16-bit index limit (65535). "
                "Decimate the mesh or split UV slices across Sub[1] and Sub[5]."
            )
        s0 = pos[u_verts].astype("<f4").tobytes()
        s1 = fb_bi[u_verts, :4].astype("<u2").tobytes()
        s2 = fb_bi[u_verts, 4:8].astype("<u2").tobytes()
        s3 = fb_bw[u_verts, :4].astype(np.uint8).tobytes()
        s4 = fb_bw[u_verts, 4:8].astype(np.uint8).tobytes()
        uv0_arr = np.stack([u_out[u_verts], v_out[u_verts]], axis=1).astype("<f4")
        s5 = uv0_arr.tobytes()
        s6 = uv0_arr.tobytes()
        if v_ts4 is not None:
            s7 = v_ts4[u_verts].tobytes()
        else:
            n_sub = nrm[u_verts]
            ts = np.clip(np.round((n_sub * 0.5 + 0.5) * 255.0), 0, 255).astype(np.uint8)
            ts4 = np.zeros((v_cnt, 4), dtype=np.uint8)
            ts4[:, :3] = ts
            ts4[:, 3] = 128
            s7 = ts4.tobytes()
        vb_blob = s0 + s1 + s2 + s3 + s4 + s5 + s6 + s7
        ib_blob = local_tris.astype("<u2").tobytes()
        return v_cnt, len(local_tris), vb_blob, ib_blob

    res = bytearray(template_res_bytes)
    bmin = pos.min(axis=0).astype(np.float32)
    bmax = pos.max(axis=0).astype(np.float32)
    struct.pack_into("<3f", res, 0x60, float(bmin[0]), float(bmin[1]), float(bmin[2]))
    struct.pack_into("<3f", res, 0x70, float(bmax[0]), float(bmax[1]), float(bmax[2]))

    dummy_vb = bytearray(3 * 56)
    dummy_ib = struct.pack("<3H", 0, 1, 2)

    vb_parts = []
    ib_parts = []
    cur_v_off = 0
    cur_start_idx = 0
    sec_records = []

    nsec0, _ = struct.unpack_from("<IQ", res, 0x00D0 + 0x18)
    for s in range(nsec0):
        if s == 1:
            v_cnt, prim_cnt, vb_b, ib_b = build_subset_streams(sub1_tris)
        elif s == 5:
            v_cnt, prim_cnt, vb_b, ib_b = build_subset_streams(sub5_tris)
        else:
            v_cnt, prim_cnt, vb_b, ib_b = 3, 1, bytes(dummy_vb), dummy_ib

        pad_v = (16 - (cur_v_off % 16)) % 16
        if pad_v:
            vb_parts.append(b"\x00" * pad_v)
            cur_v_off += pad_v

        sec_records.append((prim_cnt, cur_start_idx, cur_v_off, v_cnt))
        vb_parts.append(vb_b)
        cur_v_off += len(vb_b)

        ib_parts.append(ib_b)
        cur_start_idx += prim_cnt * 3

    pad_vb_end = (16 - (cur_v_off % 16)) % 16
    if pad_vb_end:
        vb_parts.append(b"\x00" * pad_vb_end)
        cur_v_off += pad_vb_end

    vb_all = b"".join(vb_parts)
    ib_all = b"".join(ib_parts)
    pad_ib_end = (16 - (len(ib_all) % 16)) % 16
    if pad_ib_end:
        ib_all += b"\x00" * pad_ib_end

    vb_sz = len(vb_all)
    ib_sz = len(ib_all)

    # Update all 6 LOD descriptors in mesh_res.bin so every LOD distance uses the new mesh
    for lod_idx in range(6):
        lb = 0x00D0 + lod_idx * 0x00C0
        if lb + 0x90 > len(res):
            break
        nsec, sec_off = struct.unpack_from("<IQ", res, lb + 0x18)
        if nsec != nsec0 or sec_off + nsec * 0x180 > len(res):
            continue
        for s, (prim_cnt, start_idx, v_off, v_cnt) in enumerate(sec_records):
            sb = sec_off + s * 0x180
            struct.pack_into("<4I", res, sb + 0x30, prim_cnt, start_idx, v_off, v_cnt)
        struct.pack_into("<II", res, lb + 0x68, ib_sz, vb_sz)

    chunk_out = vb_all + ib_all
    return bytes(res), chunk_out


# ---------------------------------------------------------------------------
# ReSkate Studio Mod Directory Reader/Patcher (items.toc + cas_01.cas)
# ---------------------------------------------------------------------------

TOC_ENVELOPE_SIZE = 0x22C
FNV_PRIME = 0x01000193


def toc_hash(key_bytes: bytes, seed: int = 0x811C9DC5) -> int:
    for b in key_bytes:
        sb = b - 256 if b >= 128 else b
        seed = ((seed * FNV_PRIME) & 0xFFFFFFFF) ^ (sb & 0xFFFFFFFF)
    return seed % FNV_PRIME


class HuffmanNames:
    def __init__(self, data: bytes, names_off: int, names_cnt: int, table_off: int, table_cnt: int):
        self.words = list(struct.unpack_from(f">{names_cnt}I", data, names_off))
        self.nodes = []
        by_val = {}
        left = None
        counter = 0
        for i in range(table_cnt):
            val = struct.unpack_from(">I", data, table_off + i * 4)[0]
            if val not in by_val:
                node = len(self.nodes)
                self.nodes.append((val, None, None))
                by_val[val] = node
            else:
                node = by_val[val]
            if left is None:
                left = node
                continue
            parent = len(self.nodes)
            self.nodes.append((counter, left, node))
            by_val.setdefault(counter, parent)
            self.root = parent
            counter += 1
            left = None

    def decode(self, bit: int) -> str:
        bit_cnt = len(self.words) * 32
        out = []
        while bit < bit_cnt:
            node = self.root
            while self.nodes[node][1] is not None:
                one = ((self.words[bit >> 5] >> (bit & 31)) & 1) != 0
                node = self.nodes[node][2] if one else self.nodes[node][1]
                bit += 1
            ch = (~self.nodes[node][0]) & 0xFFFF
            if ch == 0:
                return "".join(out)
            out.append(chr(ch))
        return "".join(out)


def parse_toc_bundles_and_chunks(toc_bytes: bytes):
    """Parse Frostbite Superbundle TOC (Win32/items.toc) into bundles and chunks."""
    if len(toc_bytes) < TOC_ENVELOPE_SIZE + 48 or toc_bytes[:4] != b"\x00\xd1\xce\x01":
        raise ValueError("Invalid Frostbite TOC envelope")
    body = memoryview(toc_bytes)[TOC_ENVELOPE_SIZE:]
    (
        b_hash_off, b_data_off, b_cnt,
        c_hash_off, c_guid_off, c_cnt,
        _, _, names_off, c_data_off, data_cnt, flags
    ) = struct.unpack_from(">IIiIIiIIIIiI", body, 0)

    huff = None
    if flags & 4:
        names_cnt, table_cnt, table_off = struct.unpack_from(">III", body, 48)
        huff = HuffmanNames(body, names_off, names_cnt, table_off, table_cnt)

    bundles = []
    for i in range(b_cnt):
        name_off, sz_flag, reg_off = struct.unpack_from(">IIQ", body, b_data_off + i * 16)
        sz = sz_flag & 0x3FFFFFFF
        if huff:
            name = huff.decode(name_off)
        else:
            s_start = names_off + name_off
            s_end = toc_bytes.find(b"\x00", TOC_ENVELOPE_SIZE + s_start) - TOC_ENVELOPE_SIZE
            name = bytes(body[s_start:s_end]).decode("latin1")
        reg = bytes(body[reg_off:reg_off + sz])
        bundles.append({"name": name, "region": reg, "region_off": TOC_ENVELOPE_SIZE + reg_off})

    words = list(struct.unpack_from(f">{data_cnt}I", body, c_data_off)) if data_cnt > 0 else []
    chunks = []
    for i in range(c_cnt):
        c_pos = c_guid_off + i * 20
        guid_rev = bytes(body[c_pos:c_pos + 16])[::-1]
        enc_idx = struct.unpack_from(">i", body, c_pos + 16)[0]
        if enc_idx == -1:
            continue
        w = enc_idx & 0xFFFFFF
        loc_id = (words[w] << 32) | words[w + 1]
        patch = ((loc_id >> 48) & 0xFF) != 0
        off = words[w + 2]
        sz = words[w + 3]
        chunks.append({
            "guid": guid_rev.hex(),
            "patch": patch,
            "offset": off,
            "size": sz,
            "word_pos": TOC_ENVELOPE_SIZE + c_data_off + w * 4,
        })
    return bundles, chunks


def parse_bundle_region_and_manifest(region_bytes: bytes, cas_path: Path):
    """Parse a bundle region and its BinaryBundle manifest from cas_01.cas."""
    meta_off, meta_sz, loc_off, total, data_off, res_off, chk_off, _, rep_total = struct.unpack_from(">iiIiiiiIi", region_bytes, 0)
    files = []
    pos = data_off
    cur_loc = None
    for i in range(total):
        flag = region_bytes[loc_off + i]
        if flag == 1:
            enc = struct.unpack_from(">I", region_bytes, pos)[0]
            pos += 4
            cur_loc = ((enc & 0x00FF0000) != 0, (enc >> 8) & 0xFF, enc & 0xFF)
        elif flag != 0:
            enc = struct.unpack_from(">Q", region_bytes, pos)[0]
            pos += 8
            cur_loc = (((enc >> 48) & 0xFF) != 0, (enc >> 16) & 0xFFFFFFFF, enc & 0xFFFF)
        f_off, f_sz = struct.unpack_from(">II", region_bytes, pos)
        pos += 8
        files.append({"patch": cur_loc[0], "ic": cur_loc[1], "arch": cur_loc[2], "offset": f_off, "size": f_sz})

    if meta_sz > 0:
        man_bytes = region_bytes[meta_off:meta_off + meta_sz]
    else:
        with open(cas_path, "rb") as f:
            f.seek(files[0]["offset"])
            man_bytes = f.read(files[0]["size"])

    if len(man_bytes) < 36:
        return files, None
    decl_sz = struct.unpack_from(">I", man_bytes, 0)[0]
    if decl_sz != len(man_bytes) - 4:
        return files, None
    magic_be = struct.unpack_from(">I", man_bytes, 4)[0] ^ 0x7065636E
    endian = ">" if magic_be == 0xED1CEDB8 else "<"
    tot, ebx_cnt, res_cnt, chk_cnt, str_off, _, _ = struct.unpack_from(f"{endian}7I", man_bytes, 8)
    str_base = str_off + 4

    def cstr(o):
        end = man_bytes.find(b"\x00", str_base + o)
        return man_bytes[str_base + o:end].decode("latin1")

    m_pos = 36 + tot * 20
    ebx_list = []
    for _ in range(ebx_cnt):
        n_off, orig_sz = struct.unpack_from(f"{endian}II", man_bytes, m_pos)
        m_pos += 8
        ebx_list.append({"name": cstr(n_off), "originalSize": orig_sz})
    res_list = []
    for _ in range(res_cnt):
        n_off, orig_sz = struct.unpack_from(f"{endian}II", man_bytes, m_pos)
        m_pos += 8
        res_list.append({"name": cstr(n_off), "originalSize": orig_sz})
    for i in range(res_cnt):
        res_list[i]["resourceType"] = struct.unpack_from(f"{endian}I", man_bytes, m_pos)[0]
        m_pos += 4
    m_pos += res_cnt * 24  # skip meta (16B) + id (8B)
    chk_list = []
    for _ in range(chk_cnt):
        guid = man_bytes[m_pos:m_pos + 16].hex()
        l_off, l_sz = struct.unpack_from(f"{endian}II", man_bytes, m_pos + 16)
        m_pos += 24
        chk_list.append({"guid": guid, "logicalOffset": l_off, "logicalSize": l_sz, "originalSize": (l_off & 0xFFFF) | l_sz})

    return files, {"ebx": ebx_list, "resources": res_list, "chunks": chk_list}


# ---------------------------------------------------------------------------
# CLI Commands
# ---------------------------------------------------------------------------

def cmd_to_gltf(args):
    mesh_res_bytes = None
    chunk_bytes = None
    slice_pngs = []

    if args.mod_dir:
        mod_dir = Path(args.mod_dir)
        toc_p = mod_dir / "Win32" / "items.toc"
        cas_p = mod_dir / "Win32" / "configurations" / "layout" / "initialinstallpackage" / "cas_01.cas"
        if not toc_p.exists() or not cas_p.exists():
            raise FileNotFoundError(f"Could not find Win32/items.toc and cas_01.cas inside {mod_dir}")
        bundles, chunks = parse_toc_bundles_and_chunks(toc_p.read_bytes())
        for b in bundles:
            if args.bundle_filter and args.bundle_filter not in b["name"]:
                continue
            files, man = parse_bundle_region_and_manifest(b["region"], cas_p)
            if not man:
                continue
            fi = 1 + len(man["ebx"])
            for r in man["resources"]:
                f_info = files[fi]
                fi += 1
                if f_info["patch"] and r["name"].endswith("_mesh"):
                    print(f"[Frostbite] Found patched MeshSet resource: {r['name']} (cas_off={f_info['offset']:#x})")
                    mesh_res_bytes = read_cas_slice(cas_p, f_info["offset"], f_info["size"])
                    break
            if mesh_res_bytes:
                break
        if mesh_res_bytes:
            # Find LOD chunk in TOC chunks whose decompressed size matches vb_sz + ib_sz
            lb = 0x00D0 + args.lod * 0x00C0
            _, _, ib_sz, vb_sz = struct.unpack_from("<4I", mesh_res_bytes, lb + 0x60)
            want_sz = vb_sz + ib_sz
            for c in chunks:
                if not c["patch"]:
                    continue
                try:
                    cand = read_cas_slice(cas_p, c["offset"], c["size"])
                    if len(cand) == want_sz:
                        print(f"[Frostbite] Found matching LOD{args.lod} chunk {c['guid']} ({len(cand):,} bytes)")
                        chunk_bytes = cand
                        break
                except Exception:
                    continue

    if args.mesh_res:
        mesh_res_bytes = Path(args.mesh_res).read_bytes()
    if args.chunk:
        chunk_bytes = Path(args.chunk).read_bytes()

    if not mesh_res_bytes or not chunk_bytes:
        raise ValueError("Must provide either --mod-dir or both --mesh-res and --chunk")

    tex_dir = Path(args.textures_dir) if args.textures_dir else (Path(args.mesh_res).parent if args.mesh_res else Path("."))
    for s in range(4):
        sp = tex_dir / f"slice_{s}.png"
        if sp.exists():
            slice_pngs.append(sp.read_bytes())
    if not slice_pngs:
        # Generate neutral placeholder 256x256 PNG slices if texture PNGs weren't supplied
        img = Image.new("RGBA", (256, 256), (200, 200, 200, 255))
        buf = io.BytesIO()
        img.save(buf, format="PNG")
        slice_pngs = [buf.getvalue() for _ in range(4)]

    pos, nrm, uv, sl, bi, bw, tris = decode_frostbite_mesh(mesh_res_bytes, chunk_bytes, args.lod)
    out_p = Path(args.out)
    write_rigged_glb(
        out_p,
        out_p.stem,
        pos, nrm, uv, sl, bi, bw, tris,
        slice_pngs,
        skeleton_mode=args.skeleton,
    )
    print(
        f"[Frostbite -> glTF] Wrote {out_p} ({out_p.stat().st_size / (1024 * 1024):.2f} MB): "
        f"{len(pos):,} verts, {len(tris):,} tris, skeleton={args.skeleton}"
    )


def _fb_hash32(s: str) -> bytes:
    v = 5381
    for c in s.encode("latin1"):
        v = ((v * 33) ^ c) & 0xFFFFFFFF
    return struct.pack("<I", v)


def _fb_hash64(s: str) -> bytes:
    v = 5381
    for c in s.encode("latin1"):
        v = ((v * 33) ^ c) & 0xFFFFFFFFFFFFFFFF
    return struct.pack("<Q", v)


def _fnv1a_64(s: str) -> int:
    v = 0xCBF29CE484222325
    for c in s.lower().encode("latin1"):
        v = ((v * 0x100000001B3) ^ c) & 0xFFFFFFFFFFFFFFFF
    return v


def _toc_hash(key_bytes: bytes, seed: int = 0x811C9DC5) -> int:
    fnv_prime = 0x01000193
    for b in key_bytes:
        sb = b - 256 if b >= 128 else b
        seed = ((seed * fnv_prime) ^ (sb & 0xFFFFFFFF)) & 0xFFFFFFFF
    return seed % fnv_prime


def _build_toc_perfect_hash(keys: list[bytes]):
    size = len(keys)
    hmap = [-1] * size
    order = [0] * size
    if size == 0:
        return hmap, order
    buckets = [[] for _ in range(size)]
    for idx, k in enumerate(keys):
        buckets[_toc_hash(k) % size].append(idx)
    buckets.sort(key=lambda b: len(b), reverse=True)
    used = [False] * size
    b_idx = 0
    while b_idx < len(buckets) and len(buckets[b_idx]) > 1:
        bucket = buckets[b_idx]
        seed = 1
        while True:
            indices = []
            collision = False
            for member in bucket:
                idx = _toc_hash(keys[member], seed) % size
                if used[idx] or idx in indices:
                    collision = True
                    break
                indices.append(idx)
            if not collision:
                break
            seed += 1
        hmap[_toc_hash(keys[bucket[0]]) % size] = seed
        for i, idx in enumerate(indices):
            order[idx] = bucket[i]
            used[idx] = True
        b_idx += 1
    free_idx = 0
    while b_idx < len(buckets) and len(buckets[b_idx]) > 0:
        while used[free_idx]:
            free_idx = (free_idx + 1) % size
        member = buckets[b_idx][0]
        hmap[_toc_hash(keys[member]) % size] = -free_idx - 1
        order[free_idx] = member
        used[free_idx] = True
        free_idx = (free_idx + 1) % size
        b_idx += 1
    return hmap, order


def _get_brt_text(buf: bytes | bytearray, pool: int, token: int) -> str:
    if token == 0xFFFFFFFF:
        return ""
    at = pool + (token & 0x7FFFFF)
    length = token >> 24
    frag = (at + 4) if (token & 0x800000) else (pool + struct.unpack_from("<I", buf, at + 4)[0])
    parent = struct.unpack_from("<I", buf, at)[0]
    return _get_brt_text(buf, pool, parent) + bytes(buf[frag:frag + length]).decode("latin1")


def _patch_bundle_ref_table(raw_bytes: bytes, old_slug_b: bytes, new_slug_b: bytes, old_uuid_b: bytes, new_uuid_b: bytes) -> bytes:
    buf = bytearray(raw_bytes)
    payload_end = len(buf) - 20
    lookups_off = struct.unpack_from("<Q", buf, 8)[0]
    pool_off = struct.unpack_from("<Q", buf, 16)[0]
    bundles_off = struct.unpack_from("<Q", buf, 24)[0]
    cnt = struct.unpack_from("<I", buf, 72)[0]
    rows = []
    for i in range(cnt):
        h, b_idx, tok = struct.unpack_from("<QII", buf, lookups_off + i * 16)
        old_p = _get_brt_text(buf, pool_off, tok)
        is_leaf = (h == _fnv1a_64(old_p.split("/")[-1]) and h != _fnv1a_64(old_p))
        rows.append([h, b_idx, tok, old_p, is_leaf])
    buf[pool_off:bundles_off] = (
        bytes(buf[pool_off:bundles_off])
        .replace(old_slug_b, new_slug_b)
        .replace(old_uuid_b, new_uuid_b)
    )
    for r_entry in rows:
        new_p = _get_brt_text(buf, pool_off, r_entry[2])
        if new_p != r_entry[3]:
            r_entry[0] = _fnv1a_64(new_p.split("/")[-1] if r_entry[4] else new_p)
    rows.sort(key=lambda x: x[0])
    for i, (h, b_idx, tok, _, _) in enumerate(rows):
        struct.pack_into("<QII", buf, lookups_off + i * 16, h, b_idx, tok)
    return bytes(buf)


def _derive_costume_slug7_and_disp7(costume_id: str) -> tuple[str, str]:
    clean = "".join(c for c in costume_id if c.isalnum() or c == "_")
    if not clean:
        clean = "Custom1"
    if len(clean) == 7 and clean.lower() != "yorha2b":
        disp7 = clean
        slug7 = clean.lower()
    elif len(clean) < 7:
        disp7 = (clean + "_______")[:7]
        slug7 = disp7.lower()
    else:
        h2 = hashlib.md5(costume_id.encode("utf-8")).hexdigest()[:2]
        disp7 = f"{clean[:4]}_{h2}"
        slug7 = disp7.lower()
    if slug7 == "yorha2b":
        disp7 = "EveEdit"
        slug7 = "eveedit"
    return slug7, disp7


def patch_reskate_mod_directory(
    mod_dir: Path,
    res_out: bytes,
    chunk_out: bytes,
    res_cas_bytes: bytes,
    chunk_cas_bytes: bytes,
    mod_name: str | None = None,
    mod_author: str | None = None,
    mod_desc: str | None = None,
    costume_id: str | None = None,
):
    """Patch a ReSkate Studio mod directory (cas_01.cas, BinaryBundle manifests, and Win32/items.toc) with a new MeshSet."""
    cas_p = mod_dir / "Win32" / "configurations" / "layout" / "initialinstallpackage" / "cas_01.cas"
    if not cas_p.exists():
        raise FileNotFoundError(f"Missing cas_01.cas in {mod_dir}")

    cas_sample = cas_p.read_bytes()

    # Dynamically auto-detect template parameters from cas_01.cas
    m_mesh = re.search(rb"characters/maincharacters/reskate/([0-9a-fA-F-]{36})_mesh", cas_sample)
    if m_mesh:
        old_uuid_str = m_mesh.group(1).decode("latin1")
    else:
        old_uuid_str = "97083fc0-36cf-4786-bac8-26fd407c7237"
    old_uuid_b = old_uuid_str.encode("latin1")

    m_item = re.search(rb"items/cust_fullbodycostume/own_own_([a-zA-Z0-9_]+)", cas_sample)
    if not m_item:
        m_item = re.search(rb"items/cust_fullbodycostume/own_([a-zA-Z0-9_]+)", cas_sample)
    if m_item:
        old_slug_str = m_item.group(1).decode("latin1")
    else:
        old_slug_str = "yorha2b"
    old_slug_b = old_slug_str.encode("latin1")

    old_disp_str = "Own_YoRHa2B"
    if m_item:
        m_disp = re.search(rb"(Own_[a-zA-Z0-9_]+)\x00", cas_sample[m_item.start():m_item.start() + 200])
        if m_disp:
            old_disp_str = m_disp.group(1).decode("latin1")
    old_disp_b = old_disp_str.encode("latin1")

    lod_guid_map: dict[str, bytes] = {}
    if costume_id:
        clean = "".join(c for c in costume_id if c.isalnum() or c == "_")
        if not clean:
            clean = "Custom1"
        if len(clean) >= len(old_slug_str):
            new_slug_str = clean[:len(old_slug_str)].lower()
        else:
            new_slug_str = (clean + "_" * len(old_slug_str))[:len(old_slug_str)].lower()
        new_slug_b = new_slug_str.encode("latin1")

        disp_prefix = f"Own_Own_{clean}" if old_disp_str.startswith("Own_Own_") else f"Own_{clean}"
        if len(disp_prefix) >= len(old_disp_str):
            new_disp_str = disp_prefix[:len(old_disp_str)]
        else:
            new_disp_str = (disp_prefix + "_" * len(old_disp_str))[:len(old_disp_str)]
        new_disp_b = new_disp_str.encode("latin1")

        new_uuid_str = str(uuid.uuid5(uuid.NAMESPACE_DNS, f"reskate.costume.{new_slug_str}"))
        new_uuid_b = new_uuid_str.encode("latin1")

        res_buf = bytearray(res_out)
        for lod_idx in range(6):
            lb = 0x00D0 + lod_idx * 0x00C0
            if lb + 0x94 <= len(res_buf):
                old_g = bytes(res_buf[lb + 0x84:lb + 0x94])
                new_g = hashlib.md5(old_g + new_slug_b).digest()
                lod_guid_map[old_g.hex()] = new_g
                res_buf[lb + 0x84:lb + 0x94] = new_g
        res_out = bytes(res_buf).replace(old_uuid_b, new_uuid_b)
        res_cas_bytes = wrap_uncompressed_cas(res_out)
        print(
            f"[ReSkate Mod] Standalone costume ID: '{new_disp_str}' "
            f"(items/cust_fullbodycostume/own_{new_slug_str}, mesh={new_uuid_str}_mesh)"
        )

    lod_guids = set()
    for lod_idx in range(6):
        lb = 0x00D0 + lod_idx * 0x00C0
        if lb + 0x94 <= len(res_out):
            lod_guids.add(res_out[lb + 0x84:lb + 0x94].hex())
    if lod_guid_map:
        lod_guids.update(lod_guid_map.keys())

    res_sha1 = hashlib.sha1(res_out).digest()
    chunk_sha1 = hashlib.sha1(chunk_out).digest()

    with open(cas_p, "ab") as f:
        res_cas_off = f.tell()
        f.write(res_cas_bytes)
        res_cas_sz = len(res_cas_bytes)
        chunk_cas_off = f.tell()
        f.write(chunk_cas_bytes)
        chunk_cas_sz = len(chunk_cas_bytes)

    toc_paths = list(mod_dir.glob("**/Win32/items.toc"))
    for toc_p in toc_paths:
        toc_data = bytearray(toc_p.read_bytes())
        bundles, chunks = parse_toc_bundles_and_chunks(bytes(toc_data))

        fg_map: dict[bytes, bytes] = {}
        rid_map: dict[bytes, bytes] = {}
        tex_chunk_guid_map: dict[str, bytes] = {}
        tex_chunk_owner_h64: dict[str, bytes] = {}
        if costume_id:
            for b in bundles:
                files, man = parse_bundle_region_and_manifest(b["region"], cas_p)
                if not man:
                    continue
                for idx, e in enumerate(man["ebx"]):
                    fi = files[1 + idx]
                    if fi["patch"] and not e["name"].endswith("_itemcollection"):
                        raw_ebx = read_cas_slice(cas_p, fi["offset"], fi["size"])
                        ebxd_chunk = b""
                        efix_chunk = b""
                        r_pos = 12
                        while r_pos + 8 <= len(raw_ebx):
                            r_tag = raw_ebx[r_pos:r_pos + 4]
                            r_sz = struct.unpack_from("<I", raw_ebx, r_pos + 4)[0]
                            if r_tag == b"EBXD":
                                ebxd_chunk = raw_ebx[r_pos + 8:r_pos + 8 + r_sz]
                            elif r_tag == b"EFIX":
                                efix_chunk = raw_ebx[r_pos + 8:r_pos + 8 + r_sz]
                            r_pos += 8 + r_sz + (r_sz & 1)
                        if len(efix_chunk) >= 28 and len(ebxd_chunk) >= 28:
                            old_fg = efix_chunk[:16]
                            fg_map[old_fg] = hashlib.md5(old_fg + new_slug_b).digest()
                            ef_p = 16
                            nt = struct.unpack_from("<I", efix_chunk, ef_p)[0]
                            ef_p += 4 + nt * 16
                            ns = struct.unpack_from("<I", efix_chunk, ef_p)[0]
                            ef_p += 4 + ns * 4
                            nexp = struct.unpack_from("<I", efix_chunk, ef_p)[0]
                            ef_p += 4
                            nio = struct.unpack_from("<I", efix_chunk, ef_p)[0]
                            ef_p += 4
                            ioffs = struct.unpack_from(f"<{nio}I", efix_chunk, ef_p)
                            for k, io in enumerate(ioffs[:nexp]):
                                old_ig = ebxd_chunk[12 + io - 16:12 + io]
                                if k == 0:
                                    fg_map[old_ig] = hashlib.md5(old_ig + new_slug_b).digest()
                                else:
                                    fg_map[old_ig] = old_ig[:8] + hashlib.md5(old_ig + new_slug_b).digest()[8:]
                with open(cas_p, "rb") as f:
                    f.seek(files[0]["offset"])
                    m_bytes = f.read(files[0]["size"])
                tot, ebx_cnt, res_cnt, _, _, _, _ = struct.unpack_from("<7I", m_bytes, 8)
                res_id_base = 36 + tot * 20 + ebx_cnt * 8 + res_cnt * 28
                for j in range(res_cnt):
                    fi_r = files[1 + ebx_cnt + j]
                    r_nm = man["resources"][j]["name"]
                    if fi_r["patch"] and not r_nm.endswith("_bundlereftable"):
                        old_rid = m_bytes[res_id_base + j * 8:res_id_base + (j + 1) * 8]
                        new_rid_val = struct.unpack("<Q", hashlib.md5(old_rid + new_slug_b).digest()[:8])[0] | 1
                        rid_map[old_rid] = struct.pack("<Q", new_rid_val)
                        if not r_nm.endswith("_mesh"):
                            raw_res = read_cas_slice(cas_p, fi_r["offset"], fi_r["size"])
                            if len(raw_res) == 180:
                                old_cg = raw_res[0x28:0x38]
                                new_cg = hashlib.md5(old_cg + new_slug_b).digest()
                                tex_chunk_guid_map[old_cg.hex()] = new_cg
                                new_r_nm = r_nm.lower().replace(
                                    old_slug_b.decode("latin1"), new_slug_b.decode("latin1")
                                )
                                tex_chunk_owner_h64[old_cg.hex()] = _fb_hash64(new_r_nm)

        patched_toc_chunks = 0
        if costume_id and (lod_guid_map or tex_chunk_guid_map):
            body = memoryview(toc_data)[TOC_ENVELOPE_SIZE:]
            (
                _, _, _,
                c_hash_off, c_guid_off, c_cnt,
                _, _, _, c_data_off, data_cnt, _
            ) = struct.unpack_from(">IIiIIiIIIIiI", body, 0)
            words = list(struct.unpack_from(f">{data_cnt}I", body, c_data_off)) if data_cnt > 0 else []
            toc_chunk_records = []
            for i in range(c_cnt):
                c_pos = c_guid_off + i * 20
                guid_key = bytes(body[c_pos:c_pos + 16])[::-1]
                enc_idx = struct.unpack_from(">i", body, c_pos + 16)[0]
                w = enc_idx & 0xFFFFFF
                w0, w1, off, sz = words[w], words[w + 1], words[w + 2], words[w + 3]
                is_p = (((w0 << 32) | w1) >> 48) & 0xFF != 0
                if is_p and guid_key.hex() in lod_guid_map:
                    guid_key = lod_guid_map[guid_key.hex()]
                    off, sz = chunk_cas_off, chunk_cas_sz
                    patched_toc_chunks += 1
                elif is_p and guid_key.hex() in tex_chunk_guid_map:
                    guid_key = tex_chunk_guid_map[guid_key.hex()]
                toc_chunk_records.append((guid_key, w0, w1, off, sz))
            hmap, order = _build_toc_perfect_hash([r[0] for r in toc_chunk_records])
            for i, val in enumerate(hmap):
                struct.pack_into(">i", toc_data, TOC_ENVELOPE_SIZE + c_hash_off + i * 4, val)
            for out_idx, src_idx in enumerate(order):
                g_key, w0, w1, off, sz = toc_chunk_records[src_idx]
                c_pos = TOC_ENVELOPE_SIZE + c_guid_off + out_idx * 20
                toc_data[c_pos:c_pos + 16] = g_key[::-1]
                struct.pack_into(">I", toc_data, c_pos + 16, (0x80 << 24) | (out_idx * 4))
                w_pos = TOC_ENVELOPE_SIZE + c_data_off + out_idx * 16
                struct.pack_into(">IIII", toc_data, w_pos, w0, w1, off, sz)
        else:
            for c in chunks:
                if c["patch"] and c["guid"] in lod_guids:
                    struct.pack_into(">II", toc_data, c["word_pos"] + 8, chunk_cas_off, chunk_cas_sz)
                    patched_toc_chunks += 1

        for b in bundles:
            reg = b["region"]
            reg_off_in_toc = b["region_off"]
            meta_off, meta_sz, loc_off, total, data_off, _, _, _, _ = struct.unpack_from(">iiIiiiiIi", reg, 0)
            pos = data_off
            cur_is_patch = False
            file_entries = []
            for i in range(total):
                flag = reg[loc_off + i]
                if flag == 1:
                    loc_word = struct.unpack_from(">I", reg, pos)[0]
                    pos += 4
                    cur_is_patch = (loc_word & 0x00FF0000) != 0
                elif flag != 0:
                    loc_word = struct.unpack_from(">Q", reg, pos)[0]
                    pos += 8
                    cur_is_patch = ((loc_word >> 48) & 0xFF) != 0
                f_off, f_sz = struct.unpack_from(">II", reg, pos)
                file_entries.append((reg_off_in_toc + pos, f_off, f_sz, cur_is_patch))
                pos += 8

            if not file_entries or meta_sz > 0 or not file_entries[0][3]:
                continue
            man_cas_off, man_cas_sz = file_entries[0][1], file_entries[0][2]
            with open(cas_p, "rb") as f:
                f.seek(man_cas_off)
                man_bytes = f.read(man_cas_sz)
            if len(man_bytes) < 36:
                continue
            decl_sz = struct.unpack_from(">I", man_bytes, 0)[0]
            if decl_sz != len(man_bytes) - 4:
                continue
            magic_be = struct.unpack_from(">I", man_bytes, 4)[0] ^ 0x7065636E
            endian = ">" if magic_be == 0xED1CEDB8 else "<"
            tot, ebx_cnt, res_cnt, chk_cnt, str_off, b_meta_off, b_meta_sz = struct.unpack_from(f"{endian}7I", man_bytes, 8)
            str_base = str_off + 4

            def cstr(o):
                end = man_bytes.find(b"\x00", str_base + o)
                return man_bytes[str_base + o:end].decode("latin1")

            man_buf = bytearray(man_bytes)
            modified_man = False

            ebx_table_base = 36 + tot * 20
            if costume_id:
                old_mesh_name_hash = _fb_hash32(f"characters/maincharacters/reskate/{old_uuid_str}_mesh")
                new_mesh_name_hash = _fb_hash32(f"characters/maincharacters/reskate/{new_uuid_str}_mesh")
                old_disp_hash = _fb_hash32(old_disp_str)
                new_disp_hash = _fb_hash32(new_disp_str)
                for i in range(ebx_cnt):
                    file_idx = 1 + i
                    if file_idx >= len(file_entries) or not file_entries[file_idx][3]:
                        continue
                    n_off, _ = struct.unpack_from(f"{endian}II", man_buf, ebx_table_base + i * 8)
                    e_name = cstr(n_off)
                    raw_ebx = read_cas_slice(cas_p, file_entries[file_idx][1], file_entries[file_idx][2])
                    new_ebx = bytes(raw_ebx)
                    for old_fg, new_fg in fg_map.items():
                        new_ebx = new_ebx.replace(old_fg, new_fg)
                    for old_rid, new_rid in rid_map.items():
                        new_ebx = new_ebx.replace(old_rid, new_rid)
                    if e_name.endswith("_mesh"):
                        new_ebx = new_ebx.replace(old_mesh_name_hash, new_mesh_name_hash)
                    if (old_slug_str in e_name) or e_name.startswith("items/cust_fullbodycostume/"):
                        # Update HashedAssetKey and set SortPriority to INT32_MAX so custom costumes sort to the top of the closet
                        new_ebx = new_ebx.replace(
                            old_disp_hash + b"\x00\x00\x00\x00",
                            new_disp_hash + struct.pack("<i", 2147483647),
                            1,
                        ).replace(old_disp_hash, new_disp_hash)
                    new_ebx = (
                        new_ebx.replace(old_uuid_b, new_uuid_b)
                        .replace(old_disp_b, new_disp_b)
                        .replace(old_slug_b, new_slug_b)
                    )
                    ebx_cas = wrap_uncompressed_cas(new_ebx)
                    with open(cas_p, "ab") as f:
                        e_cas_off = f.tell()
                        f.write(ebx_cas)
                        e_cas_sz = len(ebx_cas)
                    man_buf[36 + i * 20:36 + (i + 1) * 20] = hashlib.sha1(new_ebx).digest()
                    struct.pack_into(f"{endian}I", man_buf, ebx_table_base + i * 8 + 4, len(new_ebx))
                    struct.pack_into(">II", toc_data, file_entries[file_idx][0], e_cas_off, e_cas_sz)
                    modified_man = True

            res_table_base = 36 + tot * 20 + ebx_cnt * 8
            res_meta_base = res_table_base + res_cnt * 12
            res_id_base = res_meta_base + res_cnt * 16
            for j in range(res_cnt):
                n_off, _ = struct.unpack_from(f"{endian}II", man_buf, res_table_base + j * 8)
                r_name = cstr(n_off)
                asset_idx = ebx_cnt + j
                file_idx = 1 + asset_idx
                if file_idx >= len(file_entries) or not file_entries[file_idx][3]:
                    continue
                if costume_id:
                    old_rid = bytes(man_buf[res_id_base + j * 8:res_id_base + (j + 1) * 8])
                    if old_rid in rid_map:
                        man_buf[res_id_base + j * 8:res_id_base + (j + 1) * 8] = rid_map[old_rid]
                        modified_man = True
                if r_name.endswith("_mesh"):
                    man_buf[36 + asset_idx * 20:36 + (asset_idx + 1) * 20] = res_sha1
                    struct.pack_into(f"{endian}I", man_buf, res_table_base + j * 8 + 4, len(res_out))
                    struct.pack_into(">II", toc_data, file_entries[file_idx][0], res_cas_off, res_cas_sz)
                    modified_man = True
                elif costume_id and r_name.endswith("_bundlereftable"):
                    raw_brt = read_cas_slice(cas_p, file_entries[file_idx][1], file_entries[file_idx][2])
                    new_brt = _patch_bundle_ref_table(raw_brt, old_slug_b, new_slug_b, old_uuid_b, new_uuid_b)
                    brt_cas = wrap_uncompressed_cas(new_brt)
                    with open(cas_p, "ab") as f:
                        b_cas_off = f.tell()
                        f.write(brt_cas)
                        b_cas_sz = len(brt_cas)
                    man_buf[36 + asset_idx * 20:36 + (asset_idx + 1) * 20] = hashlib.sha1(new_brt).digest()
                    struct.pack_into(f"{endian}I", man_buf, res_table_base + j * 8 + 4, len(new_brt))
                    struct.pack_into(">II", toc_data, file_entries[file_idx][0], b_cas_off, b_cas_sz)
                    modified_man = True
                elif costume_id:
                    raw_res = read_cas_slice(cas_p, file_entries[file_idx][1], file_entries[file_idx][2])
                    if len(raw_res) == 180 and raw_res[0x28:0x38].hex() in tex_chunk_guid_map:
                        new_res_buf = bytearray(raw_res)
                        new_res_buf[0x28:0x38] = tex_chunk_guid_map[raw_res[0x28:0x38].hex()]
                        new_res_bytes = bytes(new_res_buf)
                        t_cas = wrap_uncompressed_cas(new_res_bytes)
                        with open(cas_p, "ab") as f:
                            t_cas_off = f.tell()
                            f.write(t_cas)
                            t_cas_sz = len(t_cas)
                        man_buf[36 + asset_idx * 20:36 + (asset_idx + 1) * 20] = hashlib.sha1(new_res_bytes).digest()
                        struct.pack_into(">II", toc_data, file_entries[file_idx][0], t_cas_off, t_cas_sz)
                        modified_man = True

            h64_positions: list[int] = []
            if b_meta_sz > 0 and 4 + b_meta_off + b_meta_sz <= len(man_buf):
                cm_start = 4 + b_meta_off
                cm_end = cm_start + b_meta_sz
                search_pos = cm_start
                while True:
                    found = man_buf.find(b"\x09h64\x00", search_pos, cm_end)
                    if found == -1 or found + 13 > cm_end:
                        break
                    h64_positions.append(found + 5)
                    search_pos = found + 13

            chk_table_base = res_table_base + res_cnt * 36
            for k in range(chk_cnt):
                c_off = chk_table_base + k * 24
                guid_hex = bytes(man_buf[c_off:c_off + 16]).hex()
                asset_idx = ebx_cnt + res_cnt + k
                file_idx = 1 + asset_idx
                if guid_hex in lod_guids and file_idx < len(file_entries) and file_entries[file_idx][3]:
                    if costume_id and guid_hex in lod_guid_map:
                        man_buf[c_off:c_off + 16] = lod_guid_map[guid_hex]
                    man_buf[36 + asset_idx * 20:36 + (asset_idx + 1) * 20] = chunk_sha1
                    struct.pack_into(f"{endian}II", man_buf, c_off + 16, 0, len(chunk_out))
                    struct.pack_into(">II", toc_data, file_entries[file_idx][0], chunk_cas_off, chunk_cas_sz)
                    modified_man = True
                elif costume_id and guid_hex in tex_chunk_guid_map and file_idx < len(file_entries) and file_entries[file_idx][3]:
                    man_buf[c_off:c_off + 16] = tex_chunk_guid_map[guid_hex]
                    if k < len(h64_positions):
                        man_buf[h64_positions[k]:h64_positions[k] + 8] = tex_chunk_owner_h64[guid_hex]
                    modified_man = True

            if costume_id:
                old_mesh_name_h64 = _fb_hash64(f"characters/maincharacters/reskate/{old_uuid_str}_mesh")
                new_mesh_name_h64 = _fb_hash64(f"characters/maincharacters/reskate/{new_uuid_str}_mesh")
                man_buf[str_base:4 + b_meta_off if b_meta_sz > 0 else len(man_buf)] = (
                    bytes(man_buf[str_base:4 + b_meta_off if b_meta_sz > 0 else len(man_buf)])
                    .replace(old_uuid_b, new_uuid_b)
                    .replace(old_slug_b, new_slug_b)
                )
                if b_meta_sz > 0:
                    cm_s = 4 + b_meta_off
                    man_buf[cm_s:cm_s + b_meta_sz] = bytes(man_buf[cm_s:cm_s + b_meta_sz]).replace(
                        old_mesh_name_h64, new_mesh_name_h64
                    )
                modified_man = True

            if modified_man:
                with open(cas_p, "r+b") as f:
                    f.seek(man_cas_off)
                    f.write(man_buf)

        toc_p.write_bytes(toc_data)
        print(f"[ReSkate Mod] Updated {toc_p.relative_to(mod_dir)} ({patched_toc_chunks} LOD chunk refs patched)")

    for mf_p in mod_dir.glob("manifest.json"):
        try:
            mf = json.loads(mf_p.read_text(encoding="utf-8"))
            if mod_name:
                mf["name"] = mod_name
            if mod_author:
                mf["author"] = mod_author
            if mod_desc:
                mf["description"] = mod_desc
            mf_p.write_text(json.dumps(mf, indent=4) + "\n", encoding="utf-8")
        except Exception:
            pass

    mods_json_p = mod_dir.parent / "mods.json"
    if mods_json_p.exists():
        try:
            mj = json.loads(mods_json_p.read_text(encoding="utf-8"))
            if "mods" in mj and isinstance(mj["mods"], list):
                mod_entry = None
                for entry in mj["mods"]:
                    if entry.get("name") == mod_dir.name:
                        mod_entry = entry
                        break
                if mod_entry:
                    mod_entry["enabled"] = True
                else:
                    mj["mods"].append({"enabled": True, "name": mod_dir.name})
                mods_json_p.write_text(json.dumps(mj, indent=2) + "\n", encoding="utf-8")
                print(f"[ReSkate Mod] Enabled '{mod_dir.name}' in {mods_json_p}")
        except Exception as e:
            print(f"[ReSkate Mod] Warning: Could not update {mods_json_p}: {e}")


def cmd_from_gltf(args):
    in_p = Path(args.input)
    out_dir = Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)

    template_res_bytes = None
    if args.template_res:
        template_res_p = Path(args.template_res)
        if not template_res_p.exists():
            raise FileNotFoundError(f"Template MeshSet resource not found: {template_res_p}")
        template_res_bytes = template_res_p.read_bytes()
    elif args.template_mod:
        src_mod = Path(args.template_mod)
        toc_path = src_mod / "Win32" / "items.toc"
        cas_path = src_mod / "Win32" / "configurations" / "layout" / "initialinstallpackage" / "cas_01.cas"
        if toc_path.exists() and cas_path.exists():
            bundles, _ = parse_toc_bundles_and_chunks(toc_path.read_bytes())
            candidate_meshes = []
            for b in bundles:
                files, man = parse_bundle_region_and_manifest(b["region"], cas_path)
                if man and man["resources"]:
                    for idx, r in enumerate(man["resources"]):
                        if r["name"].endswith("_mesh"):
                            fi = files[1 + len(man["ebx"]) + idx]
                            candidate_meshes.append((r["name"], fi))
            for r_name, fi in candidate_meshes:
                if "maincharacters/reskate/" in r_name:
                    template_res_bytes = read_cas_slice(cas_path, fi["offset"], fi["size"])
                    print(f"[glTF -> Frostbite] Auto-extracted costume MeshSet from {src_mod.name}: {r_name}")
                    break
            if not template_res_bytes and candidate_meshes:
                template_res_bytes = read_cas_slice(cas_path, candidate_meshes[0][1]["offset"], candidate_meshes[0][1]["size"])
                print(f"[glTF -> Frostbite] Auto-extracted fallback MeshSet from {src_mod.name}: {candidate_meshes[0][0]}")

    if not template_res_bytes:
        default_res_p = REPO_ROOT / "analysis" / "yorha2b" / "mesh_res.bin"
        if default_res_p.exists():
            template_res_bytes = default_res_p.read_bytes()
        else:
            raise FileNotFoundError("Could not find or extract a template MeshSet resource")

    template_chunk_bytes = None
    if args.template_chunk:
        template_chunk_p = Path(args.template_chunk)
        if template_chunk_p.exists():
            template_chunk_bytes = template_chunk_p.read_bytes()
    elif args.template_mod and template_res_bytes and len(template_res_bytes) >= 0x164:
        lod0_guid = template_res_bytes[0x154:0x164].hex()
        src_mod = Path(args.template_mod)
        toc_path = src_mod / "Win32" / "items.toc"
        cas_path = src_mod / "Win32" / "configurations" / "layout" / "initialinstallpackage" / "cas_01.cas"
        if toc_path.exists() and cas_path.exists():
            _, chunks = parse_toc_bundles_and_chunks(toc_path.read_bytes())
            for c in chunks:
                if c["guid"] == lod0_guid:
                    template_chunk_bytes = read_cas_slice(cas_path, c["offset"], c["size"])
                    print(f"[glTF -> Frostbite] Auto-extracted template LOD0 chunk {lod0_guid} from {src_mod.name}")
                    break
    if not template_chunk_bytes:
        default_chunk_p = REPO_ROOT / "analysis" / "yorha2b" / "real_lod0.bin"
        if default_chunk_p.exists():
            template_chunk_bytes = default_chunk_p.read_bytes()

    temp_glb = None
    try:
        if in_p.suffix.lower() == ".blend":
            from skate3_character_tool import convert_blend_to_temp_glb
            temp_glb = convert_blend_to_temp_glb(in_p, bake_shapekeys=True)
            gltf_p = temp_glb
        else:
            gltf_p = in_p

        pos, nrm, uv, mat, joints, weights, tris, joint_names, pil_images = load_gltf_mesh_and_skin(gltf_p)
        print(f"[glTF -> Frostbite] Loaded {in_p.name}: {len(pos):,} verts, {len(tris):,} tris, {len(joint_names)} joints")

        res_out, chunk_out = pack_frostbite_meshset(
            pos, nrm, uv, mat, joints, weights, tris, joint_names,
            template_res_bytes,
            template_chunk_bytes=template_chunk_bytes,
            from_atlas=args.from_atlas,
        )

        res_out_p = out_dir / "mesh_res.bin"
        chunk_out_p = out_dir / "real_lod0.bin"
        res_cas_p = out_dir / "mesh_res.cas"
        chunk_cas_p = out_dir / "real_lod0.cas"

        res_cas_bytes = wrap_uncompressed_cas(res_out)
        chunk_cas_bytes = wrap_uncompressed_cas(chunk_out)

        res_out_p.write_bytes(res_out)
        chunk_out_p.write_bytes(chunk_out)
        res_cas_p.write_bytes(res_cas_bytes)
        chunk_cas_p.write_bytes(chunk_cas_bytes)

        if len(pil_images) == 1 and (args.from_atlas or int(mat.max()) == 0):
            atlas = pil_images[0].resize((2048, 2048), Image.Resampling.LANCZOS)
            coords = [(0, 0), (1024, 0), (0, 1024), (1024, 1024)]
            for s, (cx, cy) in enumerate(coords):
                atlas.crop((cx, cy, cx + 1024, cy + 1024)).save(out_dir / f"slice_{s}.png")
        elif len(pil_images) >= 4:
            for s in range(4):
                pil_images[s].save(out_dir / f"slice_{s}.png")

        print(
            f"[glTF -> Frostbite] Wrote Frostbite MeshSet to {out_dir}:\n"
            f"  - {res_out_p.name} ({len(res_out):,} B raw / {len(res_cas_bytes):,} B .cas)\n"
            f"  - {chunk_out_p.name} ({len(chunk_out):,} B raw / {len(chunk_cas_bytes):,} B .cas)"
        )

        target_mod_dir = None
        costume_id = args.costume_id
        if args.template_mod and args.out_mod:
            src_mod = Path(args.template_mod)
            dst_mod = Path(args.out_mod)
            if dst_mod.exists():
                shutil.rmtree(dst_mod)
            print(f"[ReSkate Mod] Cloning template mod {src_mod.name} -> {dst_mod} ...")
            shutil.copytree(
                src_mod,
                dst_mod,
                ignore=lambda d, names: [
                    n for n in names
                    if Path(d) == src_mod and (src_mod / n).is_dir() and n.lower() != "win32"
                ],
            )
            target_mod_dir = dst_mod
            if not costume_id and not args.replace_costume:
                costume_id = args.mod_name or dst_mod.name.split("-")[-1]
        elif args.patch_mod:
            target_mod_dir = Path(args.patch_mod)

        if target_mod_dir:
            patch_reskate_mod_directory(
                target_mod_dir,
                res_out,
                chunk_out,
                res_cas_bytes,
                chunk_cas_bytes,
                mod_name=args.mod_name,
                mod_author=args.mod_author,
                mod_desc=args.mod_description,
                costume_id=costume_id,
            )
            print(f"[ReSkate Mod] Ready-to-install ReSkate mod written to: {target_mod_dir}")
    finally:
        if temp_glb and temp_glb.parent.exists():
            shutil.rmtree(temp_glb.parent, ignore_errors=True)


def main():
    parser = argparse.ArgumentParser(description="Frostbite (skate. 2025) <-> glTF 2.0 (.glb) / Blender (.blend) Character Converter")
    sub = parser.add_subparsers(dest="cmd", required=True)

    p_to = sub.add_parser("to-gltf", help="Extract Frostbite MeshSet + slices into a rigged .glb")
    p_to.add_argument("--mod-dir", help="Path to a ReSkate Studio mod folder (containing Win32/items.toc and cas_01.cas)")
    p_to.add_argument("--bundle-filter", default="", help="Optional substring filter for bundle name when using --mod-dir")
    p_to.add_argument("--mesh-res", help="Path to decompressed Frostbite mesh_res.bin")
    p_to.add_argument("--chunk", help="Path to decompressed Frostbite LOD chunk (e.g. real_lod0.bin)")
    p_to.add_argument("--textures-dir", help="Directory containing slice_0.png .. slice_3.png")
    p_to.add_argument("--lod", type=int, default=0, help="LOD index to extract (default: 0)")
    p_to.add_argument("--skeleton", choices=["frostbite", "skate3"], default="frostbite",
                      help="Target skeleton in exported .glb: 'frostbite' (fb_bone_*) or 'skate3' (70 canonical bones)")
    p_to.add_argument("--out", required=True, help="Output .glb file path")
    p_to.set_defaults(func=cmd_to_gltf)

    p_from = sub.add_parser("from-gltf", help="Convert a rigged .glb/.gltf or .blend back into a Frostbite MeshSet / ReSkate mod")
    p_from.add_argument("--input", "-i", required=True, help="Input rigged .glb, .gltf, or .blend file")
    p_from.add_argument("--template-res", help="Template Frostbite mesh_res.bin (defaults to analysis/yorha2b/mesh_res.bin)")
    p_from.add_argument("--template-chunk", help="Optional template LOD0 chunk (defaults to analysis/yorha2b/real_lod0.bin)")
    p_from.add_argument("--from-atlas", action="store_true",
                        help="Unpack single 2x2 Skate 3 texture atlas UVs back into 4 Frostbite UDIM slices")
    p_from.add_argument("--out-dir", "-o", required=True, help="Output directory for mesh_res.bin, real_lod0.bin, and .cas files")
    p_from.add_argument("--template-mod", help="Source ReSkate Studio mod folder to clone when building a new mod")
    p_from.add_argument("--out-mod", help="Output ReSkate Studio mod folder path (used with --template-mod)")
    p_from.add_argument("--patch-mod", help="Existing ReSkate Studio mod folder to patch in-place")
    p_from.add_argument("--costume-id", help="Unique standalone costume ID/name in the closet (e.g. 'EveEdit' -> 'Own_EveEdit')")
    p_from.add_argument("--replace-costume", action="store_true",
                        help="Keep the template mod's costume ID instead of registering a new standalone costume")
    p_from.add_argument("--mod-name", help="Mod name for manifest.json")
    p_from.add_argument("--mod-author", help="Author name for manifest.json")
    p_from.add_argument("--mod-description", help="Description for manifest.json")
    p_from.set_defaults(func=cmd_from_gltf)

    args = parser.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
