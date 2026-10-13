#!/usr/bin/env python3
"""Modular Challenge Pack Compositor for Skate 3 Recomp.

Discovers enabled packs from mods/raw/, validates challenges, and composes:
- Base game: mods/compiled/{db_custom,missions_custom,miscboot_custom}.mod
  and global_locators_univ.rx2 directly from stock retail disc archives.
- Official DLCs:
  - Danny Way's Hawaiian Dream -> mods/compiled/dway_park_custom.mod
  - Maloof Money Cup 2010 NYC -> mods/compiled/maloof_custom.mod
  - San Van Party Pack -> mods/compiled/play_custom.mod
  directly from stock DLC packages.

If 0 packs are enabled, restores mods/compiled to stock retail disc state.
Includes dynamic camera flythroughs, chevron suppression, and runway start calculation.
"""

import argparse
import collections
import hashlib
import json
import math
import os
import re
import shutil
import struct
import sys
from pathlib import Path

# Paths
REPO_ROOT = Path(__file__).resolve().parents[1]
TOOLS_DIR = Path(__file__).resolve().parent
OTS_V2 = REPO_ROOT / 'analysis' / 'custom' / 'ots_v2'
XEX_DIR = REPO_ROOT / 'analysis' / 'xex'
for p in (XEX_DIR, OTS_V2, TOOLS_DIR):
    if str(p) in sys.path:
        sys.path.remove(str(p))
    sys.path.insert(0, str(p))

import bigfile                         # noqa: E402
from s3hash import vlt64, loc_hash     # noqa: E402
from vault import Vault, clone_collections, replace_u64, append_array, entry_data_addr  # noqa: E402
from decrypt import game_refpack_decode  # noqa: E402

STOCK_DATA = REPO_ROOT / 'Skate3+DLC' / 'Skate 3' / 'data'
STOCK_BIGS_DIR = REPO_ROOT / 'build' / 'stock_bigs'
LOCAL_SHARE_DLC = Path.home() / '.local' / 'share' / 'skate3' / '0000000000000000' / '454108E6' / '00000002'

DEFAULT_PACKS_DIR = REPO_ROOT / 'mods' / 'raw'
DEFAULT_OUT_DIR = REPO_ROOT / 'mods' / 'compiled'


def find_stock_data(game_dir=None, out_dir=None, packs_dir=None):
    candidates = []
    if game_dir:
        candidates.extend([
            Path(game_dir) / 'data',
            Path(game_dir),
        ])
    if out_dir:
        candidates.extend([
            Path(out_dir).resolve().parent.parent / 'game' / 'data',
            Path(out_dir).resolve().parent.parent / 'data',
            Path(out_dir).resolve().parent.parent / 'Skate 3' / 'data',
        ])
    if packs_dir:
        candidates.extend([
            Path(packs_dir).resolve().parent.parent / 'game' / 'data',
            Path(packs_dir).resolve().parent.parent / 'data',
            Path(packs_dir).resolve().parent.parent / 'Skate 3' / 'data',
        ])
    candidates.extend([
        REPO_ROOT / 'game' / 'data',
        REPO_ROOT / 'data',
        REPO_ROOT / 'Skate3+DLC' / 'Skate 3' / 'data',
    ])
    for c in candidates:
        if c.exists() and (c / 'big' / 'db.big').exists():
            return c
    return STOCK_DATA


def find_stock_dlc_big(big_name, folder_id, game_dir=None, user_data_dir=None, out_dir=None):
    candidates = [
        STOCK_BIGS_DIR / big_name,
    ]
    if out_dir:
        candidates.append(Path(out_dir).parent / '.stock_cache' / big_name)
    if user_data_dir:
        candidates.extend([
            Path(user_data_dir) / 'content' / '0000000000000000' / '454108E6' / '00000002' / folder_id / big_name,
            Path(user_data_dir) / 'dlc' / folder_id / big_name,
            Path(user_data_dir) / 'dlc' / big_name,
        ])
    appdata = os.environ.get('APPDATA')
    if appdata:
        candidates.extend([
            Path(appdata) / 'skate3' / 'content' / '0000000000000000' / '454108E6' / '00000002' / folder_id / big_name,
            Path(appdata) / 'skate3' / 'dlc' / folder_id / big_name,
            Path(appdata) / 'skate3' / 'dlc' / big_name,
        ])
    if game_dir:
        candidates.extend([
            Path(game_dir) / 'dlc' / folder_id / big_name,
            Path(game_dir) / 'dlc' / big_name,
            Path(game_dir).parent / 'dlc' / folder_id / big_name,
            Path(game_dir).parent / 'dlc' / big_name,
        ])
    if out_dir:
        candidates.extend([
            Path(out_dir).resolve().parent.parent / 'dlc' / folder_id / big_name,
            Path(out_dir).resolve().parent.parent / 'dlc' / big_name,
        ])
    candidates.extend([
        REPO_ROOT / 'dlc' / folder_id / big_name,
        REPO_ROOT / 'dlc' / big_name,
        REPO_ROOT / 'game' / 'dlc' / folder_id / big_name,
        REPO_ROOT / 'game' / 'dlc' / big_name,
        LOCAL_SHARE_DLC / folder_id / big_name,
    ])
    for c in candidates:
        if c.exists() and c.stat().st_size > 0:
            return c
    return None


KF = ['data/db/challenge_local_data', 'data/db/challenge_stategraphs', 'data/db/challenge_stategraph']
TEMPLATE = 'ots_univ_02'
TEMPLATE_SITE = (296.88, 68.10, -155.16)
RX2_SRC = 'content/global_locators/BAM/DIST_University/DIST_Univers_[0x0f7a42a9282864ac].rx2'
RX2_OUT = 'global_locators_univ.rx2'
LANG = 'data/fe/languages/english/LANGUAGE_English_Global_skate3ng.BIN'

BASE_DISTRICT_CONFIGS = {
    'dist_university': {
        'name': 'University',
        'rx2_src': 'content/global_locators/BAM/DIST_University/DIST_Univers_[0x0f7a42a9282864ac].rx2',
        'rx2_out': 'global_locators_univ.rx2',
        'template': 'ots_univ_02',
        'orig_site': (296.88, 68.10, -155.16),
        'axis_win': [(250.0, 450.0), (40.0, 110.0), (-210.0, -50.0)],
        'default_locators': [f'ots_univ_{i:02d}_challengelocator_01' for i in range(10, 15)],
        'districts': {'dist_university', 'university', 'univ'},
    },
    'dist_downtown': {
        'name': 'Downtown',
        'rx2_src': 'content/global_locators/BAM/DIST_DownTown/DIST_DownTow_[0x71ff8817a4de4033].rx2',
        'rx2_out': 'global_locators_dt.rx2',
        'template': 'ots_univ_02',
        'orig_site': (296.88, 68.10, -155.16),
        'axis_win': [(250.0, 450.0), (40.0, 110.0), (-210.0, -50.0)],
        'default_locators': [
            'ots_dwtn_10_challengelocator',
            'ots_dwtn_11_challengelocator',
            'ots_dwtn_12_challengelocator',
            'ots_dwtn_13_challengelocator',
            'ots_dwtn_14_challengelocator',
        ],
        'districts': {'dist_downtown', 'downtown', 'dt', 'dist_dwtn'},
    },
    'dist_industrial': {
        'name': 'Industrial',
        'rx2_src': 'content/global_locators/BAM/DIST_Industrial/DIST_Industr_[0xa5cc938d0194a85d].rx2',
        'rx2_out': 'global_locators_ind.rx2',
        'template': 'ots_univ_02',
        'orig_site': (296.88, 68.10, -155.16),
        'axis_win': [(250.0, 450.0), (40.0, 110.0), (-210.0, -50.0)],
        'default_locators': [
            'ots_indu_10_challengelocator_01',
            'ots_indu_11_challengelocator_01',
        ],
        'districts': {'dist_industrial', 'industrial', 'ind', 'dist_indu'},
    },
}

TEMPLATE_IDS = [
    (0x0008a87a, 0x03e38706), (0x0008a87b, 0x03e38706), (0x0008a87c, 0x03e38706),
    (0x0008a87d, 0x03e38706),
    (0x0001d98c, 0x03e38703), (0x0001d98d, 0x03e38703), (0x0001d98e, 0x03e38703),
    (0x000219c2, 0x03e38707), (0x000219c3, 0x03e38707), (0x000219c4, 0x03e38707),
]

DLC_TEMPLATE_IDS = {
    'ots_artg_01': [
        (0x2c701706, 0x003d1360),
        (0x2c701706, 0x003d1361),
        (0x2c701706, 0x003d1371),
    ],
    'ots_dwmc_01': [
        (0x00061b05, 0x03070d03),
        (0x2c701706, 0x003d0bac),
        (0x2c701706, 0x003d0bb5),
    ],
    'ots_mmcn_01': [
        (0x2c701706, 0x003d0962),
        (0x2c701706, 0x003d0972),
        (0x2c701706, 0x003d0973),
    ],
}

COLLS = [
    ('challenges', ''), ('challenge_global_data', ''),
    ('challenge_objectives_group', ''), ('challenge_objectives_group', '_0'),
    ('challenge_objectives_group', '_1'), ('challenge_objective', ''),
    ('challenge_objective', '_0')
]

AXIS_WIN = [(250.0, 450.0), (40.0, 110.0), (-210.0, -50.0)]

DLC_CONFIGS = {
    'dannyway': {
        'name': "Danny Way's Hawaiian Dream",
        'stock_big': 'dway_park_00000000.big',
        'custom_big': 'dway_park_custom.big',
        'folder_id': 'E8CB22B7FEA270B0D0188C31BDFE69A71F0B94D145',
        'districts': {'dist_dannyway', 'dist_dwmc', 'dist_dwgh', 'dannyway', 'danny_way', 'hawaii'},
        'template': 'ots_dwmc_01',
        'orig_site': (137.307571, -17.272011, -23.229794),
        'start_offs': (0x1300, 0x1620),
        'challenge_bank': 'data/db/challengebanks/dlc_dwgh',
        'progression_bank': 'data/db/progressionbanks/dlc_dwgh',
        'progression_group': 'dlc_dwmc_ots_challenges',
        'progression_all': 'dlc_dwmc_all',
        'rx2_path': 'data/content/global_locators/DLC_BAM/DLC_DW_MegaCompund/DLC_DW_MegaC_[0x5a77fc8428ce315e].rx2',
        'lang_file': 'data/fe/languages/english/LANGUAGE_English_danny_way_park_skate3ng-dlc_danny_way_park.BIN',
        'known_folders': [
            'data/content/missions/freeskate_dannyway_megacoumpound',
            'data/content/global_locators/DLC_BAM/DLC_DW_MegaCompund'
        ],
        'default_locators': [
            'film_dwmc_01_challengelocator_01',
            'film_dwmc_02_challengelocator_01',
            'demo_dwmc_01_challengelocator_01',
            'demo_dwmc_02_challengelocator_01',
            'vert_dwmc_01_challengelocator_01',
            'photo_dwmc_01_challengelocator_01',
            'photo_dwmc_02_challengelocator_01',
        ],
    },
    'maloof': {
        'name': 'Maloof Money Cup 2010 NYC',
        'stock_big': 'maloof_money_cup_00000000.big',
        'custom_big': 'maloof_custom.big',
        'folder_id': '0165E5B906309F63ACA64BC2DF01667B9930C47D45',
        'districts': {'dist_maloof', 'dist_mmcn', 'maloof', 'maloof_nyc', 'nyc'},
        'template': 'ots_mmcn_01',
        'orig_site': (-53.903229, 3.563075, -21.946873),
        'start_offs': (0x1020, 0x11a0),
        'challenge_bank': 'data/db/challengebanks/dlc_mmcn',
        'progression_bank': 'data/db/progressionbanks/dlc_mmcn',
        'progression_group': 'dlc_maloof_ots',
        'progression_all': 'dlc_maloof_all',
        'rx2_path': 'data/content/global_locators/DLC_BAM/DIST_MaloofMoneyCupDLC/DIST_MaloofM_[0xf67e45e7a4b58ac4].rx2',
        'lang_file': 'data/fe/languages/english/LANGUAGE_English_maloofmc_skate3ng-dlc_maloofmc.BIN',
        'known_folders': [],
        'default_locators': [
            'street_mmcn_01_challengelocator_01',
            'photo_mmcn_01_challengelocator_01',
            'photo_mmcn_02_challengelocator_01',
        ],
    },
    'play': {
        'name': 'San Van Party Pack',
        'stock_big': 'play_00000000.big',
        'custom_big': 'play_custom.big',
        'folder_id': '8576AE236D00CF0FD8836A72A2CC5B4E3FAC12D445',
        'districts': {'dist_artgallery', 'dist_zen', 'dist_play', 'play', 'party', 'san_van_party', 'artgallery'},
        'template': 'ots_artg_01',
        'orig_site': (74.213365, 45.980183, 63.870182),
        'start_offs': (0x10b0, 0x1230),
        'challenge_bank': 'data/db/challengebanks/dlc_play',
        'progression_bank': 'data/db/progressionbanks/dlc_play',
        'progression_group': 'dlc_play_ots',
        'progression_all': 'dlc_play_all',
        'rx2_path': 'data/content/global_locators/DLC_BAM/DIST_AGPark/DIST_AGPark_[0x51909cc5f99f6b97].rx2',
        'lang_file': 'data/fe/languages/english/LANGUAGE_English_play_skate3ng-dlc_play.BIN',
        'known_folders': [
            'data/content/global_locators/DLC_BAM/DIST_AGPark',
            'data/content/global_locators/DLC_Park/Dist_Zen',
            'data/content/dynamic/model/dynamic/dmo_barriers'
        ],
        'default_locators': [
            'skate_artg_01_challengelocator_01',
            'film_artg_01_challengelocator_01',
            'film_artg_02_challengelocator_01',
            'film_artg_03_challengelocator_01',
            'photo_artg_01_challengelocator_01',
            'photo_artg_02_challengelocator_01',
            'photo_artg_03_challengelocator_01',
        ],
    },
}


def ensure_stock_dlc_bigs(game_dir=None, user_data_dir=None, out_dir=None):
    STOCK_BIGS_DIR.mkdir(parents=True, exist_ok=True)
    mapping = {
        'dway_park_00000000.big': 'E8CB22B7FEA270B0D0188C31BDFE69A71F0B94D145',
        'maloof_money_cup_00000000.big': '0165E5B906309F63ACA64BC2DF01667B9930C47D45',
        'play_00000000.big': '8576AE236D00CF0FD8836A72A2CC5B4E3FAC12D445',
        'presell_00000000.big': 'D1472E11CFD502BC1AFD05CFC2BBEFF0C059E1DB45',
    }
    for big_name, folder_id in mapping.items():
        dst = STOCK_BIGS_DIR / big_name
        if not dst.exists() or dst.stat().st_size == 0:
            src = find_stock_dlc_big(big_name, folder_id, game_dir, user_data_dir, out_dir)
            if src and src != dst and src.exists() and src.stat().st_size > 0:
                print(f"Caching stock DLC BIG {big_name} to {dst}...")
                try:
                    shutil.copy2(src, dst)
                except Exception as e:
                    print(f"Warning: Failed to copy {src} to {dst}: {e}", file=sys.stderr)
        if dst.exists() and dst.stat().st_size > 0:
            if not LOCAL_SHARE_DLC.exists():
                pass
            else:
                src = LOCAL_SHARE_DLC / folder_id / big_name
                if not src.exists() or src.stat().st_size == 0:
                    (LOCAL_SHARE_DLC / folder_id).mkdir(parents=True, exist_ok=True)
                    try:
                        shutil.copy2(dst, src)
                    except Exception:
                        pass


def get_spot_district(spot):
    raw = spot.get('district', 'dist_university').lower()
    for dist_id, cfg in BASE_DISTRICT_CONFIGS.items():
        if raw in cfg['districts'] or raw == dist_id:
            return dist_id
    for dlc_id, cfg in DLC_CONFIGS.items():
        if raw in cfg['districts'] or raw == dlc_id:
            return dlc_id
    return 'dist_university'


def get_spot_target(spot):
    dist = get_spot_district(spot)
    for dlc_id, cfg in DLC_CONFIGS.items():
        if dist in cfg['districts'] or dist == dlc_id:
            return dlc_id
    return 'base'


def id_map(num):
    m = {}
    for k, (hi, lo) in enumerate(TEMPLATE_IDS):
        m[(hi << 32) | lo] = ((0x000F0000 + num * 0x100 + k) << 32) | lo
    return m


def dlc_id_map(template, num):
    tpl_ids = DLC_TEMPLATE_IDS.get(template, [])
    m = {}
    for k, (hi, lo) in enumerate(tpl_ids):
        m[(hi << 32) | lo] = ((0x000E0000 + num * 0x100 + k) << 32) | lo
    return m


def dlc_text_map(spot, template):
    tpl_prefix = template[:-2]
    num = spot.get('num', 1)
    n = spot.get('id', f"{tpl_prefix}%02d" % num)
    loc = spot.get('locator', template + '_challengelocator_01')
    subs = [
        (template + '_challengelocator_01', loc),
        (template, n),
        (template.upper(), n.upper()),
        (template.capitalize(), n.capitalize()),
    ]
    for old, new in dlc_id_map(template, num).items():
        subs.append(('0x%016x' % old, '0x%016x' % new))
        subs.append(('%d' % old, '%d' % new))
    return [(a.encode(), b.encode()) for a, b in subs]


def text_map(spot, template=None):
    if template is None:
        dist = get_spot_district(spot)
        dist_cfg = BASE_DISTRICT_CONFIGS.get(dist, BASE_DISTRICT_CONFIGS['dist_university'])
        template = dist_cfg['template']
    tpl_prefix = template[:-2]
    num = spot.get('num', 1)
    n = spot.get('id', f"{tpl_prefix}%02d" % num)
    loc = spot.get('locator', template + '_challengelocator_01')
    subs = [
        (template + '_challengelocator_01', loc),
        (template, n),
        (template.upper(), n.upper())
    ]
    for old, new in id_map(num).items():
        subs.append(('0x%016x' % old, '0x%016x' % new))
        subs.append(('%d' % old, '%d' % new))
    return [(a.encode(), b.encode()) for a, b in subs]


def apply_text(s, subs):
    out = s
    marks = []
    for i, (a, b) in enumerate(subs):
        tok = b'\x01%d\x02' % i
        if a in out:
            out = out.replace(a, tok)
            marks.append((tok, b))
    for tok, b in marks:
        out = out.replace(tok, b)
    return out


def refpack_literals(data):
    out = bytearray(b'\x10\xfb' + struct.pack('>I', len(data))[1:])
    i, n = 0, len(data)
    while n - i >= 4:
        run = min(112, (n - i) & ~3)
        out.append(0xE0 + run // 4 - 1)
        out += data[i:i + run]
        i += run
    out.append(0xFC | (n - i))
    out += data[i:]
    return bytes(out)


def refpack_decode(data):
    dec, st = game_refpack_decode(data)
    assert st == 'STOP'
    return dec


def forward_matrix(ux, uz):
    return [
        [round(uz, 4), 0.0, round(-ux, 4), 0.0],
        [0.0, 1.0, 0.0, 0.0],
        [round(ux, 4), 0.0, round(uz, 4), 0.0],
    ]


def compute_approach_and_start(spot):
    if 'start' in spot:
        start_pos = tuple(spot['start'])
        sx, sy, sz = spot['site']
        dx, dz = sx - start_pos[0], sz - start_pos[2]
        L = math.hypot(dx, dz)
        if L < 1e-4:
            ux, uz = 0.0, 1.0
        else:
            ux, uz = dx / L, dz / L
        px, pz = -uz, ux
        return (ux, uz), (px, pz), start_pos
    runway = spot.get('runway', 16.0)
    sx, sy, sz = spot['site']
    cx, cy, cz = spot['cam']
    dx, dz = sx - cx, sz - cz
    L = math.hypot(dx, dz)
    if L < 1e-4:
        ux, uz = 0.0, 1.0
    else:
        ux, uz = dx / L, dz / L
    px, pz = -uz, ux
    start_pos = (round(sx - runway * ux, 2), round(sy, 2), round(sz - runway * uz, 2))
    return (ux, uz), (px, pz), start_pos


def retarget_csim(dec, spot, tpl=None, axis_win=None, log=None):
    d = bytearray(dec)
    if tpl is None:
        dist = get_spot_district(spot)
        dist_cfg = BASE_DISTRICT_CONFIGS.get(dist, BASE_DISTRICT_CONFIGS['dist_university'])
        tpl = dist_cfg['template']
        if axis_win is None:
            axis_win = dist_cfg.get('axis_win')
    tpl_prefix = tpl[:-2]
    num = spot.get('num', 1)
    n = spot.get('id', f"{tpl_prefix}%02d" % num)
    subs = text_map(spot, template=tpl)
    d2 = apply_text(bytes(d), subs)
    assert len(d2) == len(d)
    d = bytearray(d2)
    c_ids = replace_u64(d, id_map(num))

    guids = set()
    o = d.find(b'\xfe\xff\xff\xff')
    while o >= 0:
        guids.add(struct.unpack_from('>Q', d, o + 4)[0])
        o = d.find(b'\xfe\xff\xff\xff', o + 4)
    gmap = {g: vlt64('%s:%016x' % (n, g)) for g in guids}
    c_g = replace_u64(d, gmap)
    if log is not None:
        log.append('cSim: %d id u64, %d guid u64 (%d guids)' % (c_ids, c_g, len(guids)))

    vx0, vx1 = struct.unpack_from('>f', d, 0x290)[0], struct.unpack_from('>f', d, 0x2a0)[0]
    vy0 = struct.unpack_from('>f', d, 0x294)[0]
    vz0, vz1 = struct.unpack_from('>f', d, 0x298)[0], struct.unpack_from('>f', d, 0x2a8)[0]
    sx, sy, sz = spot['site']
    delta = [round(sx - (vx0 + vx1) / 2, 3), round(sy - 5.0 - vy0, 3), round(sz - (vz0 + vz1) / 2, 3)]
    if log is not None:
        log.append('cSim: delta %s' % delta)
    string_spans = [(0x520, 0x8c0), (0x1720, 0x1910)]

    def centre_delta(rec):
        x0, y0, z0 = struct.unpack_from('>3f', dec, rec)
        x1, y1, z1 = struct.unpack_from('>3f', dec, rec + 0x10)
        return [round(sx - (x0 + x1) / 2, 3), delta[1], round(sz - (z0 + z1) / 2, 3)]

    ranges = [
        ((0x330, 0x400), centre_delta(0x380)), ((0xb10, 0xd80), centre_delta(0x380)),
        ((0x420, 0x4f0), centre_delta(0x470)), ((0xd80, 0xff0), centre_delta(0x470))
    ]

    def delta_at(offset):
        for (a, b), dd in ranges:
            if a <= offset < b:
                return dd
        return delta

    if axis_win is None:
        margin = 150.0
        axis_win = [
            (min(vx0, vx1) - margin, max(vx0, vx1) + margin),
            (vy0 - margin, vy0 + margin),
            (min(vz0, vz1) - margin, max(vz0, vz1) + margin),
        ]

    moved = 0
    for offset in range(0, len(d) - 3, 4):
        if (0xff0 <= offset < 0x1720) or any(a <= offset < b for a, b in string_spans):
            continue
        w = struct.unpack_from('>I', d, offset)[0]
        if w == 0:
            continue
        f = struct.unpack_from('>f', d, offset)[0]
        for ax, (lo, hi) in enumerate(axis_win):
            if lo <= f <= hi:
                struct.pack_into('>f', d, offset, f + delta_at(offset)[ax])
                moved += 1
                break

    (ux, uz), (px, pz), start_pos = compute_approach_and_start(spot)
    skater_rot = forward_matrix(ux, uz)

    wait_pts = {
        'wait_1': (round(sx + 3.0 * ux + 5.5 * px, 2), round(sy + 2.4, 2), round(sz + 3.0 * uz + 5.5 * pz, 2)),
        'wait_2': (round(sx + 0.5 * ux + 4.5 * px, 2), round(sy + 1.4, 2), round(sz + 0.5 * uz + 4.5 * pz, 2)),
        'wait_3': (round(sx - 3.5 * ux + 3.5 * px, 2), round(sy + 1.8, 2), round(sz - 3.5 * uz + 3.5 * pz, 2)),
        'wait_4': (round(sx - 7.5 * ux + 2.0 * px, 2), round(sy + 2.0, 2), round(sz - 7.5 * uz + 2.0 * pz, 2)),
        'wait_5': (round(sx - 12.0 * ux + 0.5 * px, 2), round(sy + 1.8, 2), round(sz - 12.0 * uz + 0.5 * pz, 2)),
        'wait_6': start_pos,
    }

    def write_loc(base, pos_off, is_80, pos, rot):
        for r in range(3):
            struct.pack_into('>4f', d, pos_off - 48 + r * 16, *rot[r])
        struct.pack_into('>4f', d, pos_off, pos[0], pos[1], pos[2], 1.0)
        if is_80:
            struct.pack_into('>4f', d, pos_off + 0x10, pos[0] - 1.0, pos[1] - 1.0, pos[2] - 1.0, 0.0)
            struct.pack_into('>4f', d, pos_off + 0x20, pos[0] + 1.0, pos[1] + 1.0, pos[2] + 1.0, 0.0)

    def hide_loc(base, pos_off, is_80, ref_val):
        struct.pack_into('>4f', d, pos_off, 0.0, -5000.0, 0.0, 1.0)
        if is_80:
            struct.pack_into('>4f', d, pos_off + 0x10, -1.0, -5001.0, -1.0, 0.0)
            struct.pack_into('>4f', d, pos_off + 0x20, 1.0, -4999.0, 1.0, 0.0)
        str_off = 0xff0 + ref_val
        old_str = d[str_off:str_off + 40].split(b'\0')[0]
        new_str = old_str.replace(b'_vis_', b'_dum_')
        assert len(new_str) == len(old_str), (old_str, new_str)
        d[str_off:str_off + len(new_str)] = new_str

    for i in range(8):
        base = 0x1000 + i * 0x80
        ref_off = base + 0x78
        ref_val = struct.unpack_from('>I', d, ref_off)[0]
        s = d[0xff0 + ref_val:].split(b'\0')[0].decode('latin1')
        pos_off = base + 0x40
        if s.endswith('_waitlocator'):
            write_loc(base, pos_off, True, (sx, sy + 0.4, sz), skater_rot)
        elif s.endswith('_vis_2'):
            write_loc(base, pos_off, True, (sx, sy + 0.3, sz), skater_rot)
        elif s.endswith('_startlocator'):
            write_loc(base, pos_off, True, start_pos, skater_rot)
        elif any(s.endswith(suf) for suf in ('_vis_1', '_vis_3', '_vis_chev_1', '_vis_chev_2', '_vis_chev_3')):
            hide_loc(base, pos_off, True, ref_val)
        else:
            raise ValueError('Unknown 0x80 locator: ' + s)

    for i in range(7):
        base = 0x1400 + i * 0x70
        ref_off = base + 0x70
        ref_val = struct.unpack_from('>I', d, ref_off)[0]
        s = d[0xff0 + ref_val:].split(b'\0')[0].decode('latin1')
        pos_off = base + 0x40
        if s.endswith('_spawn_1'):
            write_loc(base, pos_off, False, start_pos, skater_rot)
        else:
            matched = False
            for wn, wpt in wait_pts.items():
                if s.endswith('_' + wn):
                    write_loc(base, pos_off, False, wpt, skater_rot)
                    matched = True
                    break
            if not matched:
                raise ValueError('Unknown 0x70 locator: ' + s)

    if log is not None:
        log.append('cSim: start runway at %s, flythrough waypoints + vis_2 updated' % (start_pos,))
    return bytes(d)


def update_challenge_scores(v, spot_id, own_score, kill_score):
    if not hasattr(v, 'datn_payload'):
        return
    dat_off, _ = v.ch['DatN']
    payload_base = dat_off + 8
    v.datn_payload = bytearray(v.datn_payload)
    for suf, target_score in [('_0', own_score), ('_1', kill_score)]:
        if target_score is None:
            continue
        eid = vlt64('challenge_objectives_group/' + spot_id + suf)
        exp = v.export_by_id(eid)
        if not exp:
            continue
        eoff = exp[3]
        rel_off = eoff - payload_base
        cnt = struct.unpack_from('>I', v.datn_payload, rel_off + 0x20)[0]
        tlen = struct.unpack_from('>H', v.datn_payload, rel_off + 0x26)[0]
        base = rel_off + 0x30 + 8 * tlen
        for i in range(cnt):
            ak = struct.unpack_from('>Q', v.datn_payload, base + 16 * i)[0]
            if ak == 0x0c0cfefb9fc6eb73:
                struct.pack_into('>I', v.datn_payload, base + 16 * i + 8, int(target_score))


def build_db(db, spots):
    vlt = bigfile.entry_bytes(bigfile.find(db, 'data/db/challengebanks/main.vlt'))
    binb = bigfile.entry_bytes(bigfile.find(db, 'data/db/challengebanks/main.bin'))
    v = Vault(vlt, binb)

    for spot in spots:
        dist = get_spot_district(spot)
        dist_cfg = BASE_DISTRICT_CONFIGS.get(dist, BASE_DISTRICT_CONFIGS['dist_university'])
        tpl = dist_cfg['template']
        tpl_prefix = tpl[:-2]

        lvlt = bigfile.entry_bytes(bigfile.find(db, f'data/db/challenge_local_data/{tpl}.vlt'))
        lbin = bigfile.entry_bytes(bigfile.find(db, f'data/db/challenge_local_data/{tpl}.bin'))

        num = spot.get('num', 1)
        n = spot.get('id', f"{tpl_prefix}%02d" % num)
        subs = text_map(spot, template=tpl)
        km = dict(id_map(num))
        for suf in ('', '_0', '_1'):
            km[vlt64(tpl + suf)] = vlt64(n + suf)
        specs = [(c, tpl + s, n + s) for c, s in COLLS]
        rep = clone_collections(v, specs, {tpl: n}, km,
                                lambda s: apply_text(s, subs))
        scores = spot.get('scores', {})
        if scores:
            update_challenge_scores(v, n, scores.get('own'), scores.get('kill'))
        # local data vault
        lv, lb = bytearray(lvlt), bytearray(lbin)
        um = dict(km)
        um[vlt64(tpl + '.vlt')] = vlt64(n + '.vlt')
        um[vlt64(tpl + '.bin')] = vlt64(n + '.bin')
        um[vlt64('challenge_local_data/' + tpl)] = vlt64('challenge_local_data/' + n)
        replace_u64(lv, um)
        replace_u64(lb, um)
        lv2 = apply_text(bytes(lv), subs)
        lb2 = apply_text(bytes(lb), subs)
        assert len(lv2) == len(lv) and len(lb2) == len(lb)

        # VisualIndicators: keep only vis_2 obstacle marker if present
        try:
            lvault = Vault(lv2, lb2)
            addr = entry_data_addr(lvault, lvault.exports[0][3], vlt64('VisualIndicators'))
            if addr is not None:
                arr = [dd for f, i, dd in lvault.ptrs[0] if f == addr][0]
                lb2 = bytearray(lb2)
                cnt, cap, stride, _ = struct.unpack_from('>HHHH', lb2, arr)
                e0, e1 = arr + 8, arr + 8 + stride
                if cnt >= 2:
                    lb2[e0:e0 + stride] = lb2[e1:e1 + stride]
                    rows = lvault.ptrs[1]
                    dmap = {f: dd for f, i, dd in rows}
                    rows[:] = [(f, i, dmap[f - e0 + e1] if e0 <= f < e1 else dd) for f, i, dd in rows]
                    struct.pack_into('>H', lb2, arr, 1)
                    lvault.bin = lb2
                    lv2, lb2 = lvault.serialize(), bytes(lb2)
        except Exception:
            pass

        bigfile.add_raw(db, 'data/db/challenge_local_data', n + '.vlt', lv2)
        bigfile.add_raw(db, 'data/db/challenge_local_data', n + '.bin', lb2)

    bigfile.replace_raw(db, 'data/db/challengebanks/main.vlt', v.serialize())
    bigfile.replace_raw(db, 'data/db/challengebanks/main.bin', bytes(v.bin))

    pv = Vault(bigfile.entry_bytes(bigfile.find(db, 'data/db/progressionbanks/main.vlt')),
               bigfile.entry_bytes(bigfile.find(db, 'data/db/progressionbanks/main.bin')))
    elems = [struct.pack('>QQ', vlt64(s.get('id', f"{BASE_DISTRICT_CONFIGS.get(get_spot_district(s), BASE_DISTRICT_CONFIGS['dist_university'])['template'][:-2]}%02d" % s.get('num', 1))), 0) for s in spots]
    append_array(pv, 'progression_group', 'ots', 0xec1c213bbdbf1a68, elems)
    bigfile.replace_raw(db, 'data/db/progressionbanks/main.vlt', pv.serialize())
    bigfile.replace_raw(db, 'data/db/progressionbanks/main.bin', bytes(pv.bin))


def build_missions(mb, spots, pack_dirs_by_spot):
    for spot in spots:
        dist = get_spot_district(spot)
        dist_cfg = BASE_DISTRICT_CONFIGS.get(dist, BASE_DISTRICT_CONFIGS['dist_university'])
        tpl = dist_cfg['template']
        tpl_prefix = tpl[:-2]
        src_dir = 'data/content/missions/' + tpl
        files = [e for e in mb.entries if e.folder == src_dir]

        num = spot.get('num', 1)
        n = spot.get('id', f"{tpl_prefix}%02d" % num)
        dst_dir = 'data/content/missions/' + n
        resid = struct.pack('>Q', vlt64(n + '/cSim_Global'))
        pdir = pack_dirs_by_spot.get(n)
        pack_mission_dir = pdir / 'missions' / n if pdir else None

        csim_size = 0
        for e in files:
            name = e.name.replace(tpl, n)
            custom_file = pack_mission_dir / name if (pack_mission_dir and (pack_mission_dir / name).exists()) else None

            if custom_file:
                data = bytearray(custom_file.read_bytes())
                if name == 'cSim_Global.xsf':
                    csim_size = len(data)
                bigfile.add_raw(mb, dst_dir, name, bytes(data))
            else:
                data = bytearray(bigfile.entry_bytes(e))
                log = []
                if e.name == 'cSim_Global.xsf':
                    hdr = bytearray(data[:0x150])
                    dec = refpack_decode(bytes(data[0x150:]))
                    new = retarget_csim(dec, spot, tpl=tpl, axis_win=dist_cfg['axis_win'], log=log)
                    enc = refpack_literals(new)
                    hdr[0x80:0x88] = resid
                    struct.pack_into('>I', hdr, 0x88, len(enc) + 0x50)
                    struct.pack_into('>I', hdr, 0x104, len(enc))
                    data = hdr + enc
                    data += b'\0' * ((-len(data)) % 0x80)
                    struct.pack_into('>I', data, 0x90, len(data) - 0x80)
                    csim_size = len(data)
                elif e.name.endswith('_Sim.xst'):
                    data[0x18:0x20] = resid
                bigfile.add_raw(mb, dst_dir, name, bytes(data))

        # Sim.xsm needs the final cSim size
        xsm = bigfile.find(mb, dst_dir + '/' + n + '_Sim.xsm')
        if xsm:
            d = bytearray(xsm.data)
            d[0x22] = 0xcf
            d[0x27] = num & 0xff
            struct.pack_into('>I', d, 0x34, csim_size - 0x80)
            xsm.data = bytes(d)
            xsm.size = len(d)
        print('  mission %s: %d files, cSim %d B' % (dst_dir, len(files), csim_size))


def get_localized_string(val, lang_name='english'):
    if isinstance(val, dict):
        short_code = lang_name[:2]
        for key in (lang_name, short_code, 'english', 'en', 'default'):
            if key in val:
                return str(val[key])
        return str(next(iter(val.values())))
    return str(val or '')


def update_loc_data(d, spots, lang_name='english'):
    magic, inner, count, hsz, pool_off = struct.unpack_from('<5I', d, 0)
    assert magic == 0x39000 and inner == len(d) - 8 and pool_off == 0x1c + 8 * count
    rows = [struct.unpack_from('<II', d, 0x24 + 8 * i) for i in range(count)]
    pool = bytearray(d[8 + pool_off:])
    have = {k for k, _ in rows}

    for spot in spots:
        n = spot.get('id', 'OTS_UNIV_%02d' % spot.get('num', 0)).upper()
        title_str = get_localized_string(spot.get('title', 'Custom Spot'), lang_name)
        desc_str = get_localized_string(spot.get('desc', 'Lay down your best lines here to own this spot.'), lang_name)
        for suf, text in (('TITLE', title_str), ('DESC', desc_str)):
            k = loc_hash('ID_CHALLENGE_%s_%s' % (n, suf))
            new_off = len(pool)
            pool += text.encode('latin1', errors='replace') + b'\0'
            if k in have:
                rows = [(k2, new_off if k2 == k else o) for k2, o in rows]
            else:
                rows.append((k, new_off))
                have.add(k)
    rows.sort()

    out = bytearray(struct.pack('<5I', magic, 0, len(rows), hsz, 0x1c + 8 * len(rows)))
    out += d[0x14:0x24]
    for k, o in rows:
        out += struct.pack('<II', k, o)
    out += pool
    struct.pack_into('<I', out, 4, len(out) - 8)
    return bytes(out)


def build_loc(mb, spots):
    patched_count = 0
    for entry in mb.entries:
        if 'fe/languages' in entry.folder and entry.name.startswith('LANGUAGE_') and entry.name.endswith('.BIN') and 'HISTOGRAM' not in entry.name:
            lang_name = 'english'
            for candidate in ('english', 'italian', 'french', 'german', 'spanish', 'dutch', 'japanese'):
                if candidate in entry.folder.lower() or candidate in entry.name.lower():
                    lang_name = candidate
                    break
            d = bigfile.entry_bytes(entry)
            try:
                out = update_loc_data(d, spots, lang_name=lang_name)
                if getattr(entry, 'ctype', 0) == 0:
                    entry.data, entry.size, entry.csize, entry.ctype = out, len(out), 0, 0
                else:
                    enc = refpack_literals(out)
                    entry.data, entry.csize, entry.size, entry.ctype = enc, len(enc), len(out), 1
                entry.modified = True
                patched_count += 1
            except Exception as ex:
                print(f"Warning: Failed to patch language table {entry.name}: {ex}", file=sys.stderr)
    print(f"  + Injected strings into {patched_count} language tables")

    # scene_000.bin
    sc_entry = bigfile.find(mb, 'data/scene/scene_000.bin')
    sc_d = bytearray(bigfile.entry_bytes(sc_entry))
    sc_magic, sc_ver, sc_c1, sc_c2 = struct.unpack_from('>4sIII', sc_d, 0)
    sc_sec1_end = 16 + sc_c1 * 104
    new_recs = bytearray()
    for spot in spots:
        n = spot.get('id', 'ots_univ_%02d' % spot['num'])
        name = '%s_intro' % n
        h = loc_hash(name)
        rec = bytearray(104)
        struct.pack_into('>10I', rec, 0, h, 0x0c000000, 0, 0, 0, 0, 0, 0xbf800000, 0x40e00000, 1259)
        name_bytes = name.encode('latin1') + b'\0'
        rec[0x28:0x28 + len(name_bytes)] = name_bytes
        new_recs += rec
    sc_out = sc_d[:sc_sec1_end] + new_recs + sc_d[sc_sec1_end:]
    struct.pack_into('>I', sc_out, 8, sc_c1 + len(spots))
    sc_enc = refpack_literals(bytes(sc_out))
    sc_entry.data, sc_entry.csize, sc_entry.size, sc_entry.ctype = sc_enc, len(sc_enc), len(sc_out), 1
    sc_entry.modified = True


def resolve_base_locators(spots):
    spots_by_dist = collections.defaultdict(list)
    for s in spots:
        dist = get_spot_district(s)
        if dist not in BASE_DISTRICT_CONFIGS:
            dist = 'dist_university'
        spots_by_dist[dist].append(s)

    for dist, d_spots in spots_by_dist.items():
        cfg = BASE_DISTRICT_CONFIGS[dist]
        src = STOCK_DATA / cfg['rx2_src']
        if not src.exists():
            continue
        d = bytearray(src.read_bytes())
        base = 0x230
        cnt = struct.unpack_from('>I', d, base)[0]

        def cs(o):
            return d[o:d.index(b'\0', o)].decode('latin1')

        recs = set()
        for i in range(cnt):
            r = base + 0x20 + i * 0x80
            npt = struct.unpack_from('>I', d, r + 0x68)[0]
            if npt and base + npt < len(d):
                recs.add(cs(base + npt))

        claimed = set()
        for spot in d_spots:
            loc = spot.get('locator')
            if loc and loc in recs and loc not in claimed:
                claimed.add(loc)
            else:
                spot['locator'] = None

        for spot in d_spots:
            if not spot.get('locator'):
                avail = [l for l in cfg['default_locators'] if l in recs and l not in claimed]
                if avail:
                    loc = avail[0]
                else:
                    unclaimed = [l for l in recs if l not in claimed]
                    loc = unclaimed[0] if unclaimed else (list(recs)[0] if recs else '')
                spot['locator'] = loc
                claimed.add(loc)


def build_rx2(out_dir, spots):
    spots_by_dist = collections.defaultdict(list)
    for s in spots:
        dist = get_spot_district(s)
        if dist not in BASE_DISTRICT_CONFIGS:
            dist = 'dist_university'
        spots_by_dist[dist].append(s)

    for dist, d_spots in spots_by_dist.items():
        cfg = BASE_DISTRICT_CONFIGS[dist]
        src = STOCK_DATA / cfg['rx2_src']
        if not src.exists():
            print(f"Warning: Stock RX2 not found: {src}")
            continue
        d = bytearray(src.read_bytes())
        base = 0x230
        cnt = struct.unpack_from('>I', d, base)[0]

        def cs(o):
            return d[o:d.index(b'\0', o)].decode('latin1')

        recs = {}
        for i in range(cnt):
            r = base + 0x20 + i * 0x80
            npt = struct.unpack_from('>I', d, r + 0x68)[0]
            if npt and base + npt < len(d):
                recs[cs(base + npt)] = r

        for spot in d_spots:
            loc = spot.get('locator')
            if not loc or loc not in recs:
                continue

            r = recs[loc]
            sx, sy, sz = spot['site']
            (ux, uz), (px, pz), start_pos = compute_approach_and_start(spot)
            rot = forward_matrix(ux, uz)
            nc, cp = struct.unpack_from('>II', d, r + 0x60)

            struct.pack_into('>3f', d, r + 0x30, sx, sy, sz)
            struct.pack_into('>3f', d, r + 0x40, sx - 1.0, sy - 1.0, sz - 1.0)
            struct.pack_into('>3f', d, r + 0x50, sx + 1.0, sy + 1.0, sz + 1.0)

            for j in range(nc):
                c = base + cp + j * 0x70
                for rot_row in range(3):
                    struct.pack_into('>4f', d, c + rot_row * 16, *rot[rot_row])
                sp_pos = (
                    round(start_pos[0] + (j - (nc - 1) / 2.0) * 0.4 * px, 3),
                    round(start_pos[1], 3),
                    round(start_pos[2] + (j - (nc - 1) / 2.0) * 0.4 * pz, 3),
                )
                struct.pack_into('>3f', d, c + 0x30, *sp_pos)

        out_path = out_dir / cfg['rx2_out']
        out_path.write_bytes(bytes(d))
        print(f"  Wrote {cfg['name']} global locators ({len(d_spots)} spot(s)) -> {out_path.name}")

    # Clean up inactive district rx2 files
    for dist, cfg in BASE_DISTRICT_CONFIGS.items():
        if dist not in spots_by_dist:
            out_path = out_dir / cfg['rx2_out']
            if out_path.exists():
                out_path.unlink()


def update_xml_location(xml_bytes, loc_name, site):
    text = xml_bytes.decode('latin1')
    pattern = r'(<Name>' + re.escape(loc_name) + r'</Name>\s*<Category[^>]*>(?:</Category>)?\s*<Transform>)(.*?)(</Transform>)'
    def repl(m):
        vals = [v.strip() for v in m.group(2).split(',')]
        if len(vals) == 16:
            vals[12] = f"{site[0]:.6f}"
            vals[13] = f"{site[1]:.6f}"
            vals[14] = f"{site[2]:.6f}"
            return f"{m.group(1)}{', '.join(vals)}{m.group(3)}"
        return m.group(0)
    return re.sub(pattern, repl, text, flags=re.DOTALL).encode('latin1')


def update_rx2_in_place(d, spot):
    d = bytearray(d)
    base = 0x230
    cnt = struct.unpack_from('>I', d, base)[0]

    def cs(o):
        return d[o:d.index(b'\0', o)].decode('latin1')

    recs = {}
    for i in range(cnt):
        r = base + 0x20 + i * 0x80
        npt = struct.unpack_from('>I', d, r + 0x68)[0]
        if npt and base + npt < len(d):
            recs[cs(base + npt)] = r

    loc = spot.get('locator')
    if not loc or loc not in recs:
        return bytes(d)

    r = recs[loc]
    sx, sy, sz = spot['site']
    (ux, uz), (px, pz), start_pos = compute_approach_and_start(spot)
    rot = forward_matrix(ux, uz)
    nc, cp = struct.unpack_from('>II', d, r + 0x60)

    # Update main locator position and bounds
    struct.pack_into('>3f', d, r + 0x30, sx, sy, sz)
    struct.pack_into('>3f', d, r + 0x40, sx - 1.0, sy - 1.0, sz - 1.0)
    struct.pack_into('>3f', d, r + 0x50, sx + 1.0, sy + 1.0, sz + 1.0)

    # Update children spawn positions and rotations
    for j in range(nc):
        c = base + cp + j * 0x70
        for rot_row in range(3):
            struct.pack_into('>4f', d, c + rot_row * 16, *rot[rot_row])
        sp_pos = (
            round(start_pos[0] + (j - (nc - 1) / 2.0) * 0.4 * px, 3),
            round(start_pos[1], 3),
            round(start_pos[2] + (j - (nc - 1) / 2.0) * 0.4 * pz, 3),
        )
        struct.pack_into('>3f', d, c + 0x30, *sp_pos)

    return bytes(d)


def update_sim_loc(loc_bytes, tpl, n, dx, dy, dz, start_pos=None, skater_rot=None):
    text = loc_bytes.decode('latin1')
    text = text.replace(tpl, n)

    def shift_match(m):
        vals = [v.strip() for v in m.group(1).split(',')]
        if len(vals) == 16:
            vals[12] = f"{float(vals[12]) + dx:.6f}"
            vals[13] = f"{float(vals[13]) + dy:.6f}"
            vals[14] = f"{float(vals[14]) + dz:.6f}"
            return f"<Transform>{', '.join(vals)}</Transform>"
        return m.group(0)

    text = re.sub(r'<Transform>(.*?)</Transform>', shift_match, text, flags=re.DOTALL)

    if start_pos is not None:
        def start_sub(m):
            vals = [v.strip() for v in m.group(2).split(',')]
            if len(vals) == 16:
                if skater_rot is not None:
                    vals[0] = f"{skater_rot[0][0]:.6f}"
                    vals[1] = f"{skater_rot[0][1]:.6f}"
                    vals[2] = f"{skater_rot[0][2]:.6f}"
                    vals[4] = f"{skater_rot[1][0]:.6f}"
                    vals[5] = f"{skater_rot[1][1]:.6f}"
                    vals[6] = f"{skater_rot[1][2]:.6f}"
                    vals[8] = f"{skater_rot[2][0]:.6f}"
                    vals[9] = f"{skater_rot[2][1]:.6f}"
                    vals[10] = f"{skater_rot[2][2]:.6f}"
                vals[12] = f"{start_pos[0]:.6f}"
                vals[13] = f"{start_pos[1]:.6f}"
                vals[14] = f"{start_pos[2]:.6f}"
                return f"{m.group(1)}<Transform>{', '.join(vals)}</Transform>"
            return m.group(0)

        pattern = r'(<Name>' + re.escape(n) + r'_startlocator</Name>\s*<Category />\s*)<Transform>(.*?)</Transform>'
        text = re.sub(pattern, start_sub, text, flags=re.DOTALL)

    return text.encode('latin1')


def update_boundary_xml(xml_bytes, dx, dz):
    text = xml_bytes.decode('latin1')

    def shift_point(m):
        coords = [c.strip() for c in m.group(1).split(',')]
        if len(coords) == 2:
            x = round(float(coords[0]) + dx)
            z = round(float(coords[1]) + dz)
            return f"<Point>{x}, {z}</Point>"
        return m.group(0)

    text = re.sub(r'<Point>(.*?)</Point>', shift_point, text)
    return text.encode('latin1')


def update_stream_xml(xml_bytes, dx, dz):
    text = xml_bytes.decode('latin1')

    def shift_coord(tag):
        def _shift(m):
            coords = [c.strip() for c in m.group(1).split(',')]
            if len(coords) == 2:
                x = round(float(coords[0]) + dx)
                z = round(float(coords[1]) + dz)
                return f"<{tag}>{x}, {z}</{tag}>"
            return m.group(0)
        return _shift

    text = re.sub(r'<Center>(.*?)</Center>', shift_coord('Center'), text)
    text = re.sub(r'<Tile>(.*?)</Tile>', shift_coord('Tile'), text)
    return text.encode('latin1')


def resolve_dlc_locators(b, cfg, spots):
    rx2_path = cfg.get('rx2_path')
    if not rx2_path:
        return
    rx2_entry = bigfile.find(b, rx2_path)
    if not rx2_entry:
        return
    d = bigfile.entry_bytes(rx2_entry)
    base = 0x230
    cnt = struct.unpack_from('>I', d, base)[0]

    def cs(o):
        return d[o:d.index(b'\0', o)].decode('latin1')

    recs = set()
    for i in range(cnt):
        r = base + 0x20 + i * 0x80
        npt = struct.unpack_from('>I', d, r + 0x68)[0]
        if npt and base + npt < len(d):
            recs.add(cs(base + npt))

    claimed = set()
    for spot in spots:
        loc = spot.get('locator')
        if loc and loc in recs and loc not in claimed:
            claimed.add(loc)
        else:
            spot['locator'] = None

    defaults = cfg.get('default_locators', [])
    for spot in spots:
        if not spot.get('locator'):
            avail = [l for l in defaults if l in recs and l not in claimed]
            if avail:
                loc = avail[0]
            else:
                unclaimed = [l for l in recs if l not in claimed]
                loc = unclaimed[0] if unclaimed else (list(recs)[0] if recs else '')
            spot['locator'] = loc
            claimed.add(loc)


def build_dlc_big(dlc_id, cfg, spots, pack_dirs_by_spot, out_dir):
    print(f"Building {cfg['custom_big']} ({cfg['name']}) with {len(spots)} spot(s)...")
    stock_path = find_stock_dlc_big(cfg['stock_big'], cfg['folder_id'], GAME_DIR, USER_DATA_DIR, out_dir)
    if not stock_path or not stock_path.exists():
        ensure_stock_dlc_bigs(GAME_DIR, USER_DATA_DIR, out_dir)
        stock_path = find_stock_dlc_big(cfg['stock_big'], cfg['folder_id'], GAME_DIR, USER_DATA_DIR, out_dir)
    if not stock_path or not stock_path.exists():
        print(f"Warning: Stock DLC big not found for {cfg['name']} ({cfg['stock_big']}). Skipping DLC pack.", file=sys.stderr)
        return False

    b = bigfile.read(stock_path, cfg['known_folders'])
    resolve_dlc_locators(b, cfg, spots)
    tpl = cfg['template']

    # 1. Challenge bank
    cvlt = bigfile.entry_bytes(bigfile.find(b, cfg['challenge_bank'] + '.vlt'))
    cbin = bigfile.entry_bytes(bigfile.find(b, cfg['challenge_bank'] + '.bin'))
    v = Vault(cvlt, cbin)
    for spot in spots:
        num = spot.get('num', 1)
        n = spot.get('id', f"{tpl[:-2]}%02d" % num)
        specs = [(c, tpl + s, n + s) for c, s in COLLS]
        km = dict(dlc_id_map(tpl, num))
        for s in ('', '_0', '_1'):
            km[vlt64(tpl + s)] = vlt64(n + s)
        subs = dlc_text_map(spot, tpl)
        clone_collections(
            v, specs, {tpl: n}, km,
            lambda s, subs=subs: apply_text(s, subs)
        )
        scores = spot.get('scores', {})
        if scores:
            update_challenge_scores(v, n, scores.get('own'), scores.get('kill'))
    bigfile.replace_raw(b, cfg['challenge_bank'] + '.vlt', v.serialize())
    bigfile.replace_raw(b, cfg['challenge_bank'] + '.bin', bytes(v.bin))

    # 2. Progression bank
    pvlt = bigfile.entry_bytes(bigfile.find(b, cfg['progression_bank'] + '.vlt'))
    pbin = bigfile.entry_bytes(bigfile.find(b, cfg['progression_bank'] + '.bin'))
    pv = Vault(pvlt, pbin)
    elems = [struct.pack('>QQ', vlt64(s.get('id', f"{tpl[:-2]}%02d" % s['num'])), 0) for s in spots]
    append_array(pv, 'progression_group', cfg['progression_group'], 0xec1c213bbdbf1a68, elems)
    if 'progression_all' in cfg and cfg['progression_all'] != cfg['progression_group']:
        append_array(pv, 'progression_group', cfg['progression_all'], 0xec1c213bbdbf1a68, elems)
    bigfile.replace_raw(b, cfg['progression_bank'] + '.vlt', pv.serialize())
    bigfile.replace_raw(b, cfg['progression_bank'] + '.bin', bytes(pv.bin))

    # 3. Local data
    lvlt = bigfile.entry_bytes(bigfile.find(b, f'data/db/challenge_local_data/{tpl}.vlt'))
    lbin = bigfile.entry_bytes(bigfile.find(b, f'data/db/challenge_local_data/{tpl}.bin'))
    for spot in spots:
        num = spot.get('num', 1)
        n = spot.get('id', f"{tpl[:-2]}%02d" % num)
        subs = dlc_text_map(spot, tpl)
        lv, lb = bytearray(lvlt), bytearray(lbin)
        um = dict(dlc_id_map(tpl, num))
        for s in ('', '_0', '_1'):
            um[vlt64(tpl + s)] = vlt64(n + s)
        um[vlt64(tpl + '.vlt')] = vlt64(n + '.vlt')
        um[vlt64(tpl + '.bin')] = vlt64(n + '.bin')
        um[vlt64('challenge_local_data/' + tpl)] = vlt64('challenge_local_data/' + n)
        replace_u64(lv, um)
        replace_u64(lb, um)
        lv2 = apply_text(bytes(lv), subs)
        lb2 = apply_text(bytes(lb), subs)
        assert len(lv2) == len(lv) and len(lb2) == len(lb)
        bigfile.add_raw(b, 'data/db/challenge_local_data', n + '.vlt', lv2)
        bigfile.add_raw(b, 'data/db/challenge_local_data', n + '.bin', lb2)

    # 4. Missions
    src_dir = 'data/content/missions/' + tpl
    files = [e for e in b.entries if e.folder == src_dir]
    for spot in spots:
        n = spot.get('id', f"{tpl[:-2]}%02d" % spot['num'])
        dst_dir = 'data/content/missions/' + n
        resid = struct.pack('>Q', vlt64(n + '/cSim_Global'))
        pdir = pack_dirs_by_spot.get(n)
        pack_mission_dir = pdir / 'missions' / n if pdir else None

        dx, dy, dz = 0.0, 0.0, 0.0
        if 'orig_site' in cfg and 'site' in spot:
            orig = cfg['orig_site']
            target = spot['site']
            dx = target[0] - orig[0]
            dy = target[1] - orig[1]
            dz = target[2] - orig[2]

        (ux, uz), (px, pz), start_pos = compute_approach_and_start(spot)
        skater_rot = forward_matrix(ux, uz)

        csim_size = 0
        for e in files:
            name = e.name.replace(tpl, n)
            custom_file = pack_mission_dir / name if (pack_mission_dir and (pack_mission_dir / name).exists()) else None

            if custom_file:
                data = bytearray(custom_file.read_bytes())
                if name == 'cSim_Global.xsf':
                    csim_size = len(data)
                bigfile.add_raw(b, dst_dir, name, bytes(data))
            else:
                data = bytearray(bigfile.entry_bytes(e))
                if e.name == 'cSim_Global.xsf':
                    hdr = bytearray(data[:0x150])
                    dec = refpack_decode(bytes(data[0x150:]))
                    dec = dec.replace(tpl.encode(), n.encode())
                    guids = set()
                    o = dec.find(b'\xfe\xff\xff\xff')
                    while o >= 0:
                        guids.add(struct.unpack_from('>Q', dec, o + 4)[0])
                        o = dec.find(b'\xfe\xff\xff\xff', o + 4)
                    gmap = {g: vlt64('%s:%016x' % (n, g)) for g in guids}
                    gmap.update(dlc_id_map(tpl, spot.get('num', 1)))
                    dec_ba = bytearray(dec)
                    replace_u64(dec_ba, gmap)
                    if dx != 0.0 or dy != 0.0 or dz != 0.0:
                        s_off = dec_ba.find((n + '_startlocator\0').encode())
                        if s_off < 0:
                            s_off = dec_ba.find((n + '_').encode())
                        if s_off > 0:
                            for off in range(0x200, s_off, 16):
                                w = struct.unpack_from('>I', dec_ba, off + 12)[0]
                                if w in (0x3f800000, 0x00000000):
                                    x, y, z = struct.unpack_from('>3f', dec_ba, off)
                                    mag_sq = x * x + y * y + z * z
                                    if mag_sq > 4.0 and -500 < x < 500 and -200 < y < 200 and -500 < z < 500:
                                        struct.pack_into('>3f', dec_ba, off, x + dx, y + dy, z + dz)
                    if 'start_offs' in cfg:
                        o1, o2 = cfg['start_offs']
                        struct.pack_into('>3f', dec_ba, o1, *start_pos)
                        struct.pack_into('>3f', dec_ba, o1 + 0x10, start_pos[0] - 1.0, start_pos[1] - 1.0, start_pos[2] - 1.0)
                        struct.pack_into('>3f', dec_ba, o1 + 0x20, start_pos[0] + 1.0, start_pos[1] + 1.0, start_pos[2] + 1.0)
                        for r in range(3):
                            struct.pack_into('>4f', dec_ba, o1 - 48 + r * 16, *skater_rot[r])

                        for j in range(6):
                            sp_off = o2 + j * 0x70
                            sp_pos = (round(start_pos[0] + (j - 2.5) * 0.3 * px, 2), start_pos[1], round(start_pos[2] + (j - 2.5) * 0.3 * pz, 2))
                            struct.pack_into('>3f', dec_ba, sp_off, *sp_pos)
                            for r in range(3):
                                struct.pack_into('>4f', dec_ba, sp_off - 48 + r * 16, *skater_rot[r])

                    new_dec = bytes(dec_ba)
                    enc = refpack_literals(new_dec)
                    hdr[0x80:0x88] = resid
                    struct.pack_into('>I', hdr, 0x88, len(enc) + 0x50)
                    struct.pack_into('>I', hdr, 0x104, len(enc))
                    data = hdr + enc
                    data += b'\0' * ((-len(data)) % 0x80)
                    struct.pack_into('>I', data, 0x90, len(data) - 0x80)
                    csim_size = len(data)
                elif e.name.endswith('_Sim.loc'):
                    data = bytearray(update_sim_loc(data, tpl, n, dx, dy, dz, start_pos, skater_rot))
                elif e.name.endswith('_Sim.xst'):
                    data[0x18:0x20] = resid
                bigfile.add_raw(b, dst_dir, name, bytes(data))

        xsm = bigfile.find(b, dst_dir + '/' + n + '_Sim.xsm')
        d = bytearray(xsm.data)
        d[0x22] = 0xcf
        d[0x27] = spot.get('num', 1) & 0xff
        struct.pack_into('>I', d, 0x34, csim_size - 0x80)
        xsm.data = bytes(d)
        xsm.size = len(d)

    # 5. Boundary and Stream XMLs
    for spot in spots:
        n = spot.get('id', f"{tpl[:-2]}%02d" % spot['num'])
        if 'orig_site' in cfg and 'site' in spot:
            orig = cfg['orig_site']
            target = spot['site']
            dx = target[0] - orig[0]
            dz = target[2] - orig[2]

            for xml_type in ('boundary', 'stream'):
                for suffix in ('', '_ol'):
                    src_xml = f'data/{xml_type}/{tpl}{suffix}.xml'
                    entry = bigfile.find(b, src_xml)
                    if entry:
                        raw_xml = bigfile.entry_bytes(entry)
                        if xml_type == 'boundary':
                            new_xml = update_boundary_xml(raw_xml, dx, dz)
                        else:
                            new_xml = update_stream_xml(raw_xml, dx, dz)
                        bigfile.add_raw(b, f'data/{xml_type}', f'{n}{suffix}.xml', new_xml)

    # 6. Localization in BIG (all available languages)
    for entry in b.entries:
        if 'fe/languages' in entry.folder and entry.name.startswith('LANGUAGE_') and entry.name.endswith('.BIN') and 'HISTOGRAM' not in entry.name:
            lang_name = 'english'
            for candidate in ('english', 'italian', 'french', 'german', 'spanish', 'dutch', 'japanese'):
                if candidate in entry.folder.lower() or candidate in entry.name.lower():
                    lang_name = candidate
                    break
            ld = bigfile.entry_bytes(entry)
            try:
                out_loc = update_loc_data(ld, spots, lang_name=lang_name)
                if getattr(entry, 'ctype', 0) == 0:
                    entry.data, entry.size, entry.csize, entry.ctype = out_loc, len(out_loc), 0, 0
                else:
                    enc = refpack_literals(out_loc)
                    entry.data, entry.csize, entry.size, entry.ctype = enc, len(enc), len(out_loc), 1
                entry.modified = True
            except Exception as ex:
                print(f"Warning: Failed to patch DLC language table {entry.name}: {ex}", file=sys.stderr)

    # 7. Scene intro
    scene_file = cfg.get('scene_file')
    if scene_file:
        sc_entry = bigfile.find(b, scene_file)
        if sc_entry:
            sc_d = bytearray(bigfile.entry_bytes(sc_entry))
            sc_magic, sc_ver, sc_c1, sc_c2 = struct.unpack_from('>4sIII', sc_d, 0)
            sc_sec1_end = 16 + sc_c1 * 104
            new_recs = bytearray()
            for spot in spots:
                sn = spot.get('id', f"{tpl[:-2]}%02d" % spot['num'])
                name = f'{sn}_intro'
                h = loc_hash(name)
                rec = bytearray(104)
                struct.pack_into('>10I', rec, 0, h, 0x0c000000, 0, 0, 0, 0, 0, 0xbf800000, 0x40e00000, 1259)
                name_bytes = name.encode('latin1') + b'\0'
                rec[0x28:0x28 + len(name_bytes)] = name_bytes
                new_recs += rec
            sc_out = sc_d[:sc_sec1_end] + new_recs + sc_d[sc_sec1_end:]
            struct.pack_into('>I', sc_out, 8, sc_c1 + len(spots))
            sc_entry.data = bytes(sc_out)
            sc_entry.csize = 0
            sc_entry.size = len(sc_out)
            sc_entry.ctype = 0
            sc_entry.modified = True

    # 8. World Locators (RX2 and XML LOCs)
    rx2_path = cfg.get('rx2_path')
    if rx2_path:
        rx2_entry = bigfile.find(b, rx2_path)
        if rx2_entry:
            rx2_data = bytearray(bigfile.entry_bytes(rx2_entry))
            for spot in spots:
                rx2_data = bytearray(update_rx2_in_place(rx2_data, spot))
            rx2_entry.data = bytes(rx2_data)
            rx2_entry.size = len(rx2_data)
            rx2_entry.csize = 0
            rx2_entry.ctype = 0
            rx2_entry.modified = True

        # BAM .loc XML
        loc_path = rx2_path.replace('.rx2', '.loc')
        loc_xml_entry = bigfile.find(b, loc_path)
        if loc_xml_entry:
            loc_data = bigfile.entry_bytes(loc_xml_entry)
            for spot in spots:
                loc_target = spot.get('locator')
                if loc_target:
                    loc_data = update_xml_location(loc_data, loc_target, spot['site'])
            loc_xml_entry.data = loc_data
            loc_xml_entry.size = len(loc_data)
            loc_xml_entry.csize = 0
            loc_xml_entry.ctype = 0
            loc_xml_entry.modified = True

        # Stream _Sim.loc XMLs
        for stream_entry in b.entries:
            if 'stream' in stream_entry.folder and stream_entry.name.endswith('_Sim.loc'):
                s_data = bigfile.entry_bytes(stream_entry)
                for spot in spots:
                    loc_target = spot.get('locator')
                    if loc_target:
                        s_data = update_xml_location(s_data, loc_target, spot['site'])
                stream_entry.data = s_data
                stream_entry.size = len(s_data)
                stream_entry.csize = 0
                stream_entry.ctype = 0
                stream_entry.modified = True

    # Write out .mod overlay
    mod_name = cfg['custom_big'].replace('.big', '.mod')
    mod_path = out_dir / mod_name
    bigfile.write_delta(b, mod_path, stock_path)
    print(f"  + Wrote {mod_name} ({os.path.getsize(mod_path)} bytes, {len(spots)} spot(s))")
    return True


def discover_packs(packs_dir):
    packs = []
    if not packs_dir.is_dir():
        return packs
    for entry in sorted(packs_dir.iterdir()):
        if not entry.is_dir():
            continue
        # Disabled extensions
        is_disabled_folder = entry.name.endswith(('.disabled', '.off', '.bak', '.tmp'))
        manifest_file = entry / 'pack.json'
        if not manifest_file.exists():
            continue
        try:
            with open(manifest_file, 'r', encoding='utf-8') as f:
                data = json.load(f)
        except Exception as e:
            print(f"Warning: Failed to parse {manifest_file}: {e}", file=sys.stderr)
            continue
        pack_id = data.get('id', entry.stem.replace('.disabled', ''))
        enabled = data.get('enabled', True) and not is_disabled_folder
        packs.append({
            'id': pack_id,
            'name': data.get('name', pack_id),
            'version': data.get('version', '1.0.0'),
            'author': data.get('author', 'Unknown'),
            'description': data.get('description', ''),
            'enabled': enabled,
            'spots': data.get('spots', []),
            'path': entry,
            'manifest_file': manifest_file
        })
    return packs


def hash_raw_pack(p):
    """Compute deterministic hash for a single raw mod manifest + mission files."""
    hasher = hashlib.sha256()
    hasher.update(p['id'].encode('utf-8'))
    try:
        hasher.update(p['manifest_file'].read_bytes())
    except Exception:
        pass
    missions_dir = p['path'] / 'missions'
    if missions_dir.is_dir():
        for f in sorted(missions_dir.rglob('*')):
            if f.is_file():
                hasher.update(f.name.encode('utf-8'))
                hasher.update(struct.pack('>Q', int(f.stat().st_mtime)))
                hasher.update(struct.pack('>Q', f.stat().st_size))
    return hasher.hexdigest()


def compute_target_hash(spots, pack_hashes):
    """Compute composite hash for all spots assigned to a target container."""
    hasher = hashlib.sha256()
    for s in sorted(spots, key=lambda x: str(x.get('id', ''))):
        hasher.update(json.dumps(s, sort_keys=True).encode('utf-8'))
        pack_id = s.get('pack_id', '')
        if pack_id in pack_hashes:
            hasher.update(pack_hashes[pack_id].encode('utf-8'))
    return hasher.hexdigest()


def restore_stock_retail(out_dir):
    """Restore mods/compiled: clean up all overrides so game runs 100% stock."""
    out_dir.mkdir(parents=True, exist_ok=True)
    cleanup_bases = [
        'db_custom', 'miscboot_custom', 'missions_custom', 'presell_custom'
    ]
    for cfg in DLC_CONFIGS.values():
        cleanup_bases.append(cfg['custom_big'].replace('.big', ''))

    for base in cleanup_bases:
        p = out_dir / f"{base}.mod"
        if p.exists():
            p.unlink()

    for dcfg in BASE_DISTRICT_CONFIGS.values():
        rx_out = out_dir / dcfg['rx2_out']
        if rx_out.exists():
            rx_out.unlink()

    active_marker = out_dir / '.active'
    if active_marker.exists():
        active_marker.unlink()

    sf = out_dir / '.mod_state.json'
    with open(sf, 'w', encoding='utf-8') as f:
        json.dump({'active_packs': [], 'active_count': 0, 'state_hash': 'STOCK', 'targets': {}}, f, indent=2)
    print("Cleaned mods/compiled: 0 custom packs active. Game runs pure stock retail data.")


def compose(packs_dir=DEFAULT_PACKS_DIR, out_dir=DEFAULT_OUT_DIR, force=False, game_dir=None, user_data_dir=None, reset_registry=False):
    global STOCK_DATA, GAME_DIR, USER_DATA_DIR
    packs_dir = Path(packs_dir)
    out_dir = Path(out_dir)
    GAME_DIR = Path(game_dir) if game_dir else None
    USER_DATA_DIR = Path(user_data_dir) if user_data_dir else None
    STOCK_DATA = find_stock_data(GAME_DIR, out_dir, packs_dir)

    all_packs = discover_packs(packs_dir)
    enabled_packs = [p for p in all_packs if p['enabled']]
    state_file = out_dir / '.mod_state.json'

    if not enabled_packs:
        restore_stock_retail(out_dir)
        return 0

    out_dir.mkdir(parents=True, exist_ok=True)

    # 1. Compute per-pack hashes
    pack_hashes = {p['id']: hash_raw_pack(p) for p in enabled_packs}

    # 2. Collect spots and group by target container with automatic conflict resolution & auto-generation
    all_spots = []
    pack_dirs_by_spot = {}
    spots_by_target = collections.defaultdict(list)

    CONTAINER_START_NUMS = {
        'base': 16,
        'dannyway': 2,
        'maloof': 2,
        'play': 2,
    }

    # Locate and load persistent spot registry to ensure IDs never drift across pack add/remove/swap
    if (packs_dir.parent / 'raw').exists() or (packs_dir.parent / 'compiled').exists():
        registry_file = packs_dir.parent / '.spot_registry.json'
    else:
        registry_file = out_dir.parent / '.spot_registry.json'

    if reset_registry and registry_file.exists():
        try:
            registry_file.unlink()
            print(f"Reset persistent spot registry: {registry_file}")
        except Exception:
            pass

    spot_registry = {'version': 1, 'spots': {}}
    if not reset_registry and registry_file.exists():
        try:
            with open(registry_file, 'r', encoding='utf-8') as f:
                loaded = json.load(f)
                if isinstance(loaded, dict) and 'spots' in loaded:
                    spot_registry = loaded
        except Exception as e:
            print(f"Warning: Failed to load spot registry from {registry_file}: {e}", file=sys.stderr)

    registered_spots = spot_registry.setdefault('spots', {})

    # Pass 1: Collect spots, assign defaults for missing optional fields, and build unique registry keys
    for p in enabled_packs:
        seen_keys_in_pack = set()
        for idx, s in enumerate(p['spots']):
            s['pack_id'] = p['id']
            dist = get_spot_district(s)
            s['district'] = dist
            target = get_spot_target(s)

            # Unique key within this pack, preferring explicit key/id/title
            base_k = str(s.get('key') or s.get('id') or s.get('title') or f"spot_{idx}")
            k = base_k
            suffix = 1
            while k in seen_keys_in_pack:
                suffix += 1
                k = f"{base_k}_{suffix}"
            seen_keys_in_pack.add(k)
            s['_reg_key'] = f"{p['id']}:{k}"

            # Default optional fields
            s.setdefault('runway', 16.0)
            s.setdefault('desc', "Lay down your best lines here to own this spot.")
            s.setdefault('scores', {'own': 600, 'kill': 1000})

            # Auto-calculate camera if omitted
            if 'cam' not in s and 'site' in s:
                sx, sy, sz = s['site']
                s['cam'] = [round(sx - 4.0, 2), round(sy + 2.5, 2), round(sz - 4.0, 2)]

            all_spots.append(s)
            spots_by_target[target].append(s)

    # Pass 2: Assign num and id per target container with persistent registry stability
    for target, t_spots in spots_by_target.items():
        used_nums = set()
        used_ids = set()
        start_num = CONTAINER_START_NUMS.get(target, 2)

        # Gather all known numbers/IDs already allocated in registry for this container
        target_regs = {k: v for k, v in registered_spots.items() if v.get('target') == target}
        all_registered_nums = {v['num'] for v in target_regs.values() if isinstance(v.get('num'), int)}
        all_registered_ids = {v['id'] for v in target_regs.values() if isinstance(v.get('id'), str)}
        max_registered_num = max(all_registered_nums, default=start_num - 1)

        # 2a: Preserve valid, non-conflicting explicit numbers and IDs from pack.json
        for s in t_spots:
            dist = s['district']
            dist_cfg = BASE_DISTRICT_CONFIGS.get(dist, BASE_DISTRICT_CONFIGS['dist_university'])
            tpl = dist_cfg['template'] if target == 'base' else DLC_CONFIGS[target]['template']
            tpl_prefix = tpl[:-2]

            req_num = s.get('num')
            req_id = s.get('id')

            if isinstance(req_num, int) and req_num >= start_num and req_num not in used_nums:
                candidate_id = req_id if req_id else f"{tpl_prefix}%02d" % req_num
                if candidate_id not in used_ids:
                    s['num'] = req_num
                    s['id'] = candidate_id
                    used_nums.add(req_num)
                    used_ids.add(candidate_id)
                    continue

            # Clear missing or conflicting identifiers for registry / auto-assignment
            s['num'] = None
            s['id'] = None

        # 2b: Restore known numbers and IDs from persistent registry
        for s in t_spots:
            if s.get('num') is not None and s.get('id') is not None:
                continue

            reg = target_regs.get(s['_reg_key'])
            if reg:
                reg_num = reg.get('num')
                reg_id = reg.get('id')
                if isinstance(reg_num, int) and reg_num not in used_nums:
                    if isinstance(reg_id, str) and reg_id not in used_ids:
                        s['num'] = reg_num
                        s['id'] = reg_id
                        if reg.get('locator') and not s.get('locator'):
                            s['locator'] = reg['locator']
                        used_nums.add(reg_num)
                        used_ids.add(reg_id)

        # 2c: Auto-assign brand new spots sequentially, strictly above any existing registered num
        next_num = max(start_num, max_registered_num + 1)
        for s in t_spots:
            if s.get('num') is not None and s.get('id') is not None:
                continue

            dist = s['district']
            dist_cfg = BASE_DISTRICT_CONFIGS.get(dist, BASE_DISTRICT_CONFIGS['dist_university'])
            tpl = dist_cfg['template'] if target == 'base' else DLC_CONFIGS[target]['template']
            tpl_prefix = tpl[:-2]

            while next_num in used_nums or next_num in all_registered_nums:
                next_num += 1

            candidate_id = f"{tpl_prefix}%02d" % next_num
            while candidate_id in used_ids or candidate_id in all_registered_ids:
                next_num += 1
                candidate_id = f"{tpl_prefix}%02d" % next_num

            s['num'] = next_num
            s['id'] = candidate_id
            used_nums.add(next_num)
            used_ids.add(candidate_id)
            all_registered_nums.add(next_num)
            all_registered_ids.add(candidate_id)
            next_num += 1

        # 2d: Update registry mapping for active spots in this container
        for s in t_spots:
            registered_spots[s['_reg_key']] = {
                'pack_id': s['pack_id'],
                'target': target,
                'district': s['district'],
                'num': s['num'],
                'id': s['id'],
                'locator': s.get('locator')
            }
            pack_dirs_by_spot[s['id']] = next(p['path'] for p in enabled_packs if p['id'] == s['pack_id'])

    # Pass 3: Pre-resolve base locators so locator is immediately recorded and stable
    base_spots = spots_by_target.get('base', [])
    if base_spots and STOCK_DATA:
        resolve_base_locators(base_spots)
        for s in base_spots:
            if s.get('locator') and s['_reg_key'] in registered_spots:
                registered_spots[s['_reg_key']]['locator'] = s['locator']

    # 3. Compute per-target composite hashes
    current_target_hashes = {}
    all_targets = ['base'] + list(DLC_CONFIGS.keys())
    for target in all_targets:
        t_spots = spots_by_target.get(target, [])
        if t_spots:
            current_target_hashes[target] = compute_target_hash(t_spots, pack_hashes)
        else:
            current_target_hashes[target] = None
    current_target_hashes['miscboot'] = compute_target_hash(all_spots, pack_hashes)

    # 4. Compare with saved state to determine dirty targets
    saved_targets = {}
    target_state_file = state_file
    if target_state_file.exists():
        try:
            with open(target_state_file, 'r', encoding='utf-8') as f:
                saved_state = json.load(f)
            saved_targets = saved_state.get('targets', {})
        except Exception:
            saved_targets = {}

    dirty_targets = set()
    if force:
        dirty_targets = set(all_targets) | {'miscboot'}
    else:
        for target in all_targets:
            t_spots = spots_by_target.get(target, [])
            if t_spots:
                # Check if hash changed
                if saved_targets.get(target) != current_target_hashes[target]:
                    dirty_targets.add(target)
                else:
                    # Check if output file exists on disk
                    if target == 'base':
                        if not (out_dir / 'db_custom.mod').exists() or not (out_dir / 'missions_custom.mod').exists():
                            dirty_targets.add('base')
                    else:
                        cfg = DLC_CONFIGS[target]
                        mod_file = out_dir / cfg['custom_big'].replace('.big', '.mod')
                        if not mod_file.exists():
                            dirty_targets.add(target)
            else:
                # Target has no spots: clean up if leftover
                if target == 'base':
                    for name in ['db_custom', 'missions_custom']:
                        p = out_dir / f"{name}.mod"
                        if p.exists():
                            p.unlink()
                    for dcfg in BASE_DISTRICT_CONFIGS.values():
                        rx_out = out_dir / dcfg['rx2_out']
                        if rx_out.exists():
                            rx_out.unlink()
                else:
                    cfg = DLC_CONFIGS[target]
                    p = out_dir / cfg['custom_big'].replace('.big', '.mod')
                    if p.exists():
                        p.unlink()

        # If any container changed or miscboot.mod missing, miscboot is dirty
        if dirty_targets or not (out_dir / 'miscboot_custom.mod').exists() or saved_targets.get('miscboot') != current_target_hashes['miscboot']:
            dirty_targets.add('miscboot')

    # If nothing is dirty and sentinel exists, we are done in < 5ms!
    if not dirty_targets and (out_dir / '.active').exists():
        try:
            with open(registry_file, 'w', encoding='utf-8') as f:
                json.dump(spot_registry, f, indent=2)
        except Exception:
            pass
        print(f"All {len(enabled_packs)} mod(s) are up to date in {out_dir.name}.")
        return 0

    print(f"Composing {len(enabled_packs)} custom mod pack(s) (targets to build: {', '.join(sorted(dirty_targets))})...")
    for p in enabled_packs:
        print(f"  + Pack: {p['name']} v{p['version']} ({len(p['spots'])} spots)")

    # 5. Build Base Game Container
    if 'base' in dirty_targets:
        base_spots = spots_by_target.get('base', [])
        if base_spots:
            resolve_base_locators(base_spots)
            stock_db = STOCK_DATA / 'big' / 'db.big'
            print(f"Building db_custom.mod ({len(base_spots)} base challenge(s))...")
            db = bigfile.read(stock_db, KF)
            build_db(db, base_spots)
            bigfile.write_delta(db, out_dir / 'db_custom.mod', stock_db)

            stock_mb = STOCK_DATA / 'content' / 'missions.big'
            print(f"Building missions_custom.mod ({len(base_spots)} base challenge(s))...")
            mb = bigfile.read(stock_mb)
            build_missions(mb, base_spots, pack_dirs_by_spot)
            bigfile.write_delta(mb, out_dir / 'missions_custom.mod', stock_mb)

            print("Building district global locators...")
            build_rx2(out_dir, base_spots)

    # 6. Build Official DLC Containers
    for dlc_id, cfg in DLC_CONFIGS.items():
        if dlc_id in dirty_targets:
            dlc_spots = spots_by_target.get(dlc_id, [])
            if dlc_spots:
                ok = build_dlc_big(dlc_id, cfg, dlc_spots, pack_dirs_by_spot, out_dir)
                if not ok:
                    current_target_hashes[dlc_id] = None

    # 7. Build Global Localization & Intro Cameras (miscboot_custom.mod)
    if 'miscboot' in dirty_targets:
        print(f"Building miscboot_custom.mod ({len(all_spots)} total challenges)...")
        stock_misc = STOCK_DATA / 'big' / 'miscboot.big'
        misc = bigfile.read(stock_misc)
        build_loc(misc, all_spots)
        bigfile.write_delta(misc, out_dir / 'miscboot_custom.mod', stock_misc)

    # 8. Update resolved locators in the registry and save
    for s in all_spots:
        reg_entry = registered_spots.get(s['_reg_key'])
        if reg_entry and s.get('locator'):
            reg_entry['locator'] = s['locator']

    try:
        registry_file.parent.mkdir(parents=True, exist_ok=True)
        with open(registry_file, 'w', encoding='utf-8') as f:
            json.dump(spot_registry, f, indent=2)
    except Exception as e:
        print(f"Warning: Failed to save spot registry to {registry_file}: {e}", file=sys.stderr)

    # 9. Mark active state and save composite hashes
    (out_dir / '.active').touch()
    new_state = {
        'version': 2,
        'active_packs': [p['id'] for p in enabled_packs],
        'active_count': len(enabled_packs),
        'spots_count': len(all_spots),
        'targets': current_target_hashes
    }
    with open(state_file, 'w', encoding='utf-8') as f:
        json.dump(new_state, f, indent=2)

    print(f"Successfully composed {len(enabled_packs)} pack(s) with {len(all_spots)} challenge(s).")
    return 0


def main():
    ap = argparse.ArgumentParser(description="Compose raw challenge packs into .mod binaries.")
    ap.add_argument('--packs-dir', default=str(DEFAULT_PACKS_DIR), help="Directory containing raw mod packs (mods/raw)")
    ap.add_argument('--out-dir', default=str(DEFAULT_OUT_DIR), help="Output compiled mods directory (mods/compiled)")
    ap.add_argument('--game-dir', default=None, help="Game data directory containing data/big/db.big")
    ap.add_argument('--user-data-dir', default=None, help="User data directory containing installed DLC")
    ap.add_argument('--force', action='store_true', help="Force recomposition even if state hashes match")
    ap.add_argument('--reset-registry', action='store_true', help="Reset persistent spot registry to reclaim IDs")
    a = ap.parse_args()
    return compose(Path(a.packs_dir), Path(a.out_dir), a.force, game_dir=a.game_dir, user_data_dir=a.user_data_dir, reset_registry=a.reset_registry)


if __name__ == '__main__':
    sys.exit(main() or 0)

