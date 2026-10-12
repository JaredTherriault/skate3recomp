# Skate 3 Reverse Engineering & Custom Content Architecture Findings

## 1. Executive Summary

This document summarizes the reverse engineering findings, file formats, and runtime architecture discovered during the development of custom content loading for Skate 3 on the PC recompilation (`skate3recomp`).

Initially, reverse engineering focused on the official Xbox 360 STFS LIVE DLC mechanism to determine whether user-created missions and challenges (Own The Spot / OTS, Own The Lot / OTL) could be packaged as standard retail DLC. In-depth disassembly of the Title Update 3 (TU3) executable revealed that the game's internal DLC registration pipeline enforces a hardcoded module table in `sub_82D083D8`, fixed entitlement token validation, and strict cross-vault collection array bounds. Consequently, injecting custom challenges for base-game districts via STFS packages proved structurally incompatible with retail game binaries without intrusive binary patches.

To provide a robust, clean, and extensible solution, the architecture was pivoted to a recomp-level Virtual File System (VFS) override combined with a modular custom pack compositor and an in-game UI management system.

---

## 2. EA Attrib / Vault Architecture

Skate 3 stores gameplay metadata, challenge definitions, skater models, and world configurations in EA Attrib Vaults, split across `.vlt` (metadata, schema, chunk headers) and `.bin` (raw data tables) file pairs.

### 2.1 Chunk Types & Layout
Vault files are structured as sequentially framed chunks:

| FourCC | Magic (BE) | Purpose |
|---|---|---|
| `Vers` | `0x56657273` | Version header, build timestamp, and schema generation hash |
| `StrN` | `0x5374724E` | String table containing null-terminated attribute/symbol names |
| `DepN` | `0x4465704E` | Dependency list specifying prerequisite vaults and namespaces |
| `DatN` | `0x4461744E` | Attribute data chunk; offset index into the companion `.bin` |
| `ExpN` | `0x4578704E` | Export table listing exported collection symbols and IDs |
| `PtrN` | `0x5074724E` | Pointer fixup table for runtime memory relocation |
| `EndC` | `0x456E6443` | End chunk terminator |

### 2.2 Relocation Semantics (`PtrN`)
The `PtrN` chunk contains relative offset pairs `{uint32_t target_offset, uint32_t fixup_type}`. When the game engine mounts a vault into memory:
1. Data blocks from `.bin` are loaded into a contiguous pool.
2. The runtime iterates over `PtrN` fixup entries.
3. Target pointers are resolved by adding the base allocation address of the target vault or referenced dependent vault.
4. Cross-vault pointers require that all dependencies declared in `DepN` are already loaded and indexed in the global vault registry.

---

## 3. BIG File System & RefPack Compression

Game assets are packed into EA BIG archives (`.big`), specifically the EB v3 variant.

### 3.1 EB v3 Header & Directory Structure
- **Magic**: `0x4542` (`EB`), followed by version `3`.
- **TOC Entry**: 16-byte record `[offset_raw (u32)][csize (u32)][usize (u32)][hash1 (u32)]`:
  - `offset = offset_raw << 6` (64-byte alignment).
  - In `missions.big`, `csize` is typically stored as `0`, indicating uncompressed stream payloads stored verbatim at the 64-byte block offset.
- **FAT Region**:
  - Contains directory hierarchy, file nodes, and path string pool.
  - Node layout: `[key = string offset][is_file flag][parent_id][file_idx / child_count][tbl_offset / 0xFFFFFFFF]`.
  - Strides must be 4-byte aligned; folder slots are null-padded to 4-byte boundaries.

### 3.2 RefPack LZ Compression (`0x82A5E038`)
Compressed chunks (such as localized text and `cSim_Global.xsf` simulation payloads) use EA RefPack LZ compression, handled by `rw::core::codec::RefpackInflate::Decompress` at `0x82A5E038`.
- **Header**:
  - `sig16`: `(b0 << 8) | b1`
  - Large size flag: `L = sig & 0x8000`
  - Compressed size flag: `C = sig & 0x100`
  - Uncompressed size `usize`: decoded from the following 3 or 4 bytes big-endian.
- **Command Stream**:
  - `0x00 - 0x7F` (2-byte copy): Literal count = `b0 & 0x03`, back-reference length = `((b0 >> 2) & 0x07) + 3`, offset = `((b0 & 0x60) << 3) | b1 + 1`.
  - `0x80 - 0xBF` (3-byte copy): Literal count = `b1 >> 6`, length = `(b0 & 0x3F) + 3`, offset = `((b1 & 0x3F) << 8) | b2 + 1`.
  - `0xC0 - 0xDF` (4-byte copy): Literal count = `b0 & 0x03`, length = `((b0 & 0x0C) << 6) | b3 + 5`, offset = `((b0 & 0x10) << 12) | (b1 << 8) | b2 + 1`.
  - `0xE0 - 0xFB`: Literal sequence of `((b0 & 0x1F) << 2) + 4` bytes.
  - `0xFC - 0xFF`: Trailing literal run of `b0 & 0x03` bytes followed by stop condition.

---

## 4. Retail DLC Pipeline & LIVE Package Limitations

Extensive reverse engineering was performed on official DLC packages (Danny Way, Maloof Money Cup, San Van Party Pack, Black Box Park):

### 4.1 STFS Packaging
Official DLC packages are Xbox STFS containers (PIRS/LIVE) containing:
- Package header with RSA signature and content ID.
- File system metadata mapping virtual files (e.g. `dlc_danny_way_park.vlt`, `missions.big`).
- `vaultlist.xml` declaring the vaults to load upon mounting.

### 4.2 The Hardcoded DLC Dispatch Gate
At runtime, the title update mounts DLC containers to `dlcbig:`. However, mission registration does not dynamically scan directories. Instead:
- Function `sub_82D083D8` contains a static dispatch table for exactly 9 recognized DLC modules.
- Pack identity is cross-referenced with entitlement licenses.
- Base-game districts (such as `dist_university`, `dist_downtown`, `dist_industrial`) have their challenge arrays statically sized in `db.big:main.vlt`. Adding new missions to an existing base district via an external DLC vault triggers out-of-bounds collection array writes or silent omission because the district collection allocator rejects entries beyond the compiled capacity.

**Verdict**: Authoring custom challenges as standalone STFS LIVE packages cannot seamlessly inject new spots into base-game districts without runtime binary patching.

---

## 5. Recomp VFS Custom Archive Architecture

To solve the DLC limitation cleanly, the recompilation leverages its native Virtual File System:

```
[Retail Game Assets]      --> Skate3+DLC/Skate 3/data/content/missions.big
[Raw Mod Packages]        --> mods/raw/west_coast_ots/
                                  |
                                  v
[Pack Compositor Tool]    --> tools/pack_compositor.py
                                  |
                                  v
[Active Mod Overlays]     --> mods/compiled/missions_custom.mod
                                  |
                                  v
[Recomp VFS Mount]        --> skate3mods: mapped with priority override
                                  |
                                  v
[Game Engine]             --> Transparently loads patched missions & db mod overlays
```

### 5.1 VFS Prioritization
The game wrapper mounts `mods/compiled/` as `skate3mods:`. File open requests for core archives (`missions.big`, `db.big`, `miscboot.big`, and DLC bigs) check `skate3mods:` first. If present, the custom mod overlay is loaded transparently; if absent, the engine falls back to the clean retail files in `Skate3+DLC/`.

---

## 6. Modular Custom Pack System

Mods are organized as self-contained packages under `mods/raw/<pack_id>/`:

### 6.1 Pack Manifest (`pack.json`)
```json
{
  "id": "west_coast_ots",
  "name": "West Coast OTS",
  "version": "1.0.0",
  "author": "Mod Author",
  "description": "5 custom Own The Spot challenges in University district",
  "challenges": [
    {
      "id": "ots_univ_16",
      "name": "Snoop",
      "district": "dist_university",
      "type": "ots"
    }
  ]
}
```

### 6.2 Tooling Pipeline
- `tools/pack_compositor.py`: Merges enabled pack assets into `mods/compiled/`. Validates TOC entries, recalculates DJB2 hashes, adjusts FAT directory blocks, and updates string tables.
- `tools/pack_manager.py`: Command-line tool to list, enable, disable, and validate mod packages.

### 6.3 In-Game Esc "Mods" Menu
The PC recompilation includes a native Esc Settings overlay:
- Added a dedicated **"Mods"** navigation category.
- Dynamically scans `mods/raw/` and displays installed packs with checkboxes, versioning, author details, and challenge counts.
- Toggling a mod stages the change in `config/mods.json` and displays an **"Apply & Restart"** prompt to seamlessly re-compose archives and restart the executable.

---

## 7. Challenge Authoring & OTS Mechanics

Custom Own The Spot (OTS) challenges require precise authoring across simulation and mission layers:

### 7.1 Mission Data Structure (`cSim_Global.xsf`)
Inside `missions.big`, each mission folder (e.g. `ots_univ_16_Sim/`) contains `cSim_Global.xsf`, an RW4xb2 stream container:
- **Trigger Volumes**: Big-endian float matrices defining bounding boxes for spot trick detection.
- **Locator Nodes**: Entity spawn points, orientation quaternions, and bail/reset anchors.

### 7.2 Gameplay & Presentation Fixes
When authoring custom spots from existing templates:
1. **Dynamic Camera Flythrough**: Camera intro splines require valid anchor points. Interpolation keyframes are computed relative to the trigger volume center to avoid camera clipping through ground geometry.
2. **HUD Chevron Suppression**: Objective directional chevrons are suppressed during pre-trick approach by setting appropriate bitmasks in the mission state flags.
3. **Runway Start Position**: Starting skate positions are automatically offset backwards along the approach vector to ensure adequate speed generation before entering the trick scoring volume.
