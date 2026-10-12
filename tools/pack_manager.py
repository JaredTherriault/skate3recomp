#!/usr/bin/env python3
"""Custom Challenge Pack Manager for Skate 3 Recomp.

Manage, inspect, enable, disable, and sync modular challenge packs.
"""

import argparse
import json
import os
import sys
from pathlib import Path

# Paths
REPO_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO_ROOT / 'tools'))
from pack_compositor import (
    DEFAULT_PACKS_DIR,
    DEFAULT_OUT_DIR,
    discover_packs,
    compose,
    restore_stock_retail
)


def cmd_list(args):
    packs_dir = Path(args.packs_dir)
    packs = discover_packs(packs_dir)
    if not packs:
        print(f"No custom challenge packs found in {packs_dir}")
        return 0

    print("=" * 80)
    print(f"{'STATUS':<10} {'ID':<20} {'NAME':<24} {'VERSION':<8} {'CHALLENGES'}")
    print("=" * 80)
    for p in packs:
        status = "[ENABLED]" if p['enabled'] else "[DISABLED]"
        spots_str = f"{len(p['spots'])} spots"
        print(f"{status:<10} {p['id']:<20} {p['name'][:23]:<24} {p['version']:<8} {spots_str}")
        for s in p['spots']:
            spot_id = s.get('id', 'ots_univ_%02d' % s.get('num', 0))
            title = s.get('title', 'Unknown')
            district = s.get('district', 'dist_university')
            print(f"    - {spot_id:<12} '{title}' ({district})")
    print("=" * 80)

    # Show active cache state
    state_file = Path(args.out_dir) / '.mod_state.json'
    if state_file.exists():
        try:
            with open(state_file, 'r', encoding='utf-8') as f:
                saved = json.load(f)
            active = saved.get('active_packs', [])
            print(f"Active in game: {len(active)} pack(s) ({', '.join(active) if active else 'none, 100% stock'})")
        except Exception:
            pass
    return 0


def cmd_enable(args):
    packs_dir = Path(args.packs_dir)
    pack_id = args.pack_id.strip()
    found = False

    for entry in sorted(packs_dir.iterdir()):
        if not entry.is_dir():
            continue
        stem = entry.name.replace('.disabled', '').replace('.off', '')
        manifest_file = entry / 'pack.json'
        if not manifest_file.exists():
            continue

        try:
            with open(manifest_file, 'r', encoding='utf-8') as f:
                data = json.load(f)
        except Exception:
            continue

        curr_id = data.get('id', stem)
        if curr_id.lower() == pack_id.lower() or entry.name.lower() == pack_id.lower():
            found = True
            # 1. Unrename folder if disabled
            target_entry = entry
            if entry.name.endswith(('.disabled', '.off')):
                clean_name = entry.name
                for ext in ('.disabled', '.off'):
                    if clean_name.endswith(ext):
                        clean_name = clean_name[:-len(ext)]
                target_entry = entry.parent / clean_name
                entry.rename(target_entry)
                manifest_file = target_entry / 'pack.json'

            # 2. Update pack.json
            data['enabled'] = True
            with open(manifest_file, 'w', encoding='utf-8') as f:
                json.dump(data, f, indent=2)

            print(f"Enabled pack: {curr_id} ('{data.get('name', curr_id)}')")
            break

    if not found:
        print(f"Error: Pack '{pack_id}' not found in {packs_dir}", file=sys.stderr)
        return 1

    # Auto sync
    print("Synchronizing custom bigs...")
    return compose(packs_dir, Path(args.out_dir), force=False)


def cmd_disable(args):
    packs_dir = Path(args.packs_dir)
    pack_id = args.pack_id.strip()
    found = False

    for entry in sorted(packs_dir.iterdir()):
        if not entry.is_dir():
            continue
        stem = entry.name.replace('.disabled', '').replace('.off', '')
        manifest_file = entry / 'pack.json'
        if not manifest_file.exists():
            continue

        try:
            with open(manifest_file, 'r', encoding='utf-8') as f:
                data = json.load(f)
        except Exception:
            continue

        curr_id = data.get('id', stem)
        if curr_id.lower() == pack_id.lower() or entry.name.lower() == pack_id.lower():
            found = True
            # Update pack.json
            data['enabled'] = False
            with open(manifest_file, 'w', encoding='utf-8') as f:
                json.dump(data, f, indent=2)

            print(f"Disabled pack: {curr_id} ('{data.get('name', curr_id)}')")
            break

    if not found:
        print(f"Error: Pack '{pack_id}' not found in {packs_dir}", file=sys.stderr)
        return 1

    # Auto sync
    print("Synchronizing custom bigs...")
    return compose(packs_dir, Path(args.out_dir), force=False)


def cmd_sync(args):
    return compose(Path(args.packs_dir), Path(args.out_dir), force=args.force)


def cmd_restore_stock(args):
    restore_stock_retail(Path(args.out_dir))
    return 0


def cmd_scaffold(args):
    packs_dir = Path(args.packs_dir)
    pack_id = args.pack_id.strip()
    target_dir = packs_dir / pack_id
    if target_dir.exists():
        print(f"Error: Directory {target_dir} already exists!", file=sys.stderr)
        return 1

    district = (args.district or 'dist_university').lower()
    district_defaults = {
        'dist_university': {
            'id': 'ots_univ_21', 'num': 21, 'title': 'University Custom Spot',
            'locator': 'ots_univ_10_challengelocator_01',
            'site': [296.88, 68.10, -155.16], 'cam': [294.0, 69.5, -157.0]
        },
        'dist_downtown': {
            'id': 'ots_dwtn_21', 'num': 21, 'title': 'Downtown Custom Spot',
            'locator': 'ots_dwtn_10_challengelocator',
            'site': [-90.43, 32.18, 181.87], 'cam': [-92.0, 33.5, 179.0]
        },
        'dist_industrial': {
            'id': 'ots_indu_21', 'num': 21, 'title': 'Industrial Custom Spot',
            'locator': 'ots_indu_10_challengelocator_01',
            'site': [-84.82, 39.47, -463.11], 'cam': [-86.5, 41.0, -461.0]
        },
        'dist_dannyway': {
            'id': 'ots_dwmc_03', 'num': 3, 'title': 'Hawaiian Dream Custom Spot',
            'locator': 'film_dwmc_01_challengelocator_01',
            'site': [173.63, -20.43, 2.57], 'cam': [165.0, -18.5, 2.57]
        },
        'dist_maloof': {
            'id': 'ots_mmcn_03', 'num': 3, 'title': 'Maloof NYC Custom Spot',
            'locator': 'street_mmcn_01_challengelocator_01',
            'site': [-0.25, 5.43, 20.31], 'cam': [-0.25, 6.5, 25.0]
        },
        'dist_artgallery': {
            'id': 'ots_artg_05', 'num': 5, 'title': 'Art Gallery Custom Spot',
            'locator': 'skate_artg_01_challengelocator_01',
            'site': [129.15, 43.89, -34.80], 'cam': [125.0, 45.0, -38.0]
        },
    }
    defaults = district_defaults.get(district, district_defaults['dist_university'])

    target_dir.mkdir(parents=True, exist_ok=True)
    (target_dir / 'missions').mkdir(exist_ok=True)
    tpl = {
        "id": pack_id,
        "name": args.name or pack_id.replace('_', ' ').title(),
        "version": "1.0.0",
        "author": "Your Name",
        "description": f"Custom challenge pack for {district}.",
        "enabled": True,
        "spots": [
            {
                "id": defaults['id'],
                "num": defaults['num'],
                "title": defaults['title'],
                "desc": "Lay down your best lines here to own this spot.",
                "district": district,
                "site": defaults['site'],
                "cam": defaults['cam'],
                "locator": defaults['locator'],
                "runway": 16.0,
                "scores": {
                    "own": 600,
                    "kill": 1000
                }
            }
        ]
    }
    with open(target_dir / 'pack.json', 'w', encoding='utf-8') as f:
        json.dump(tpl, f, indent=2)
    print(f"Scaffolded new custom pack at: {target_dir} (district: {district})")
    print(f"Edit {target_dir}/pack.json and add your spots, then run 'pack_manager.py sync'.")
    return 0


def main():
    ap = argparse.ArgumentParser(description="Skate 3 Recomp Mod Pack Manager")
    ap.add_argument('--packs-dir', default=str(DEFAULT_PACKS_DIR), help="Directory containing raw mod packs (mods/raw)")
    ap.add_argument('--out-dir', default=str(DEFAULT_OUT_DIR), help="Output compiled mods directory (mods/compiled)")

    subs = ap.add_subparsers(dest='command', help='Command to run')

    # list
    p_list = subs.add_parser('list', help='List all discovered challenge packs and their status')

    # enable
    p_enable = subs.add_parser('enable', help='Enable a custom pack')
    p_enable.add_argument('pack_id', help='Pack ID or folder name')

    # disable
    p_disable = subs.add_parser('disable', help='Disable a custom pack')
    p_disable.add_argument('pack_id', help='Pack ID or folder name')

    # sync
    p_sync = subs.add_parser('sync', help='Synchronize compiled mods with enabled packs')
    p_sync.add_argument('--force', action='store_true', help='Force recomposition')

    # restore-stock
    p_stock = subs.add_parser('restore-stock', help='Restore mods to 100%% clean stock retail disc')

    # scaffold
    p_scaffold = subs.add_parser('scaffold', help='Create a new empty custom pack template')
    p_scaffold.add_argument('pack_id', help='Folder name / ID of the new pack')
    p_scaffold.add_argument('--name', help='Display name of the pack')
    p_scaffold.add_argument('--district', choices=['dist_university', 'dist_downtown', 'dist_industrial', 'dist_dannyway', 'dist_maloof', 'dist_artgallery'], default='dist_university', help='Target district for the scaffolded spot')

    args = ap.parse_args()
    if not args.command or args.command == 'list':
        return cmd_list(args)
    elif args.command == 'enable':
        return cmd_enable(args)
    elif args.command == 'disable':
        return cmd_disable(args)
    elif args.command == 'sync':
        return cmd_sync(args)
    elif args.command == 'restore-stock':
        return cmd_restore_stock(args)
    elif args.command == 'scaffold':
        return cmd_scaffold(args)


if __name__ == '__main__':
    sys.exit(main() or 0)
