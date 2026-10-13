# Modular Challenge Packs (Recomp Custom DLC)

Skate 3 Recomp supports modular, toggleable custom challenge packs across **both the base game and all official DLC packages**. This allows authors to create and distribute challenge packs that players can enable or disable at will with a game restart, without touching or dirtying base game retail files or stock DLC archives.

---

## 1. Directory Structure

Custom packs live in `custom_packs/` (or `build/custom_packs/`):

```text
custom_packs/
├── west_coast_ots/
│   ├── pack.json             # Pack metadata (dist_university, 5 spots)
│   └── missions/             # (Optional) Pre-built custom mission payloads
└── hawaiian_dream_ots/
    └── pack.json             # Danny Way's Hawaiian Dream custom OTS (dist_dannyway)
```

---

## 2. In-Game Esc Menu ("Mods")

Players can manage their mods directly from the in-game pause/settings menu:

1. Press **`Escape`** (or gamepad **Start / Menu Chord**) during gameplay.
2. Select the **Mods** category in the left navigation rail.
3. All discovered mods in `custom_packs/` are listed with their name, description, and status toggle.
4. Toggle any mod between **`[Disabled]`** and **`[Enabled]`**.
5. When a mod toggle is changed, the menu displays a restart notification (*"Mod changes require a restart"*).
6. Select **`Apply & Restart`** (or press the corresponding gamepad button) to save the toggles and restart Skate 3.
7. On game startup, the recomp automatically compiles the newly enabled mods into the virtual filesystem cache (or reverts cleanly to pure retail disc/DLC state if all mods are turned off).

---

## 3. Supported Districts & Target Archives

The compositor automatically routes challenges to their correct destination container based on the `district` field in `pack.json`:

| District ID(s) | DLC / World Location | Source Archive | Composed Output | Template Challenge |
| :--- | :--- | :--- | :--- | :--- |
| `dist_university` | University District | Disc `db.big`, `missions.big` | `custom_bigs/db_custom.big`, `missions_custom.big`, `global_locators_univ.rx2` | `ots_univ_02` |
| `dist_downtown` | Downtown District | Disc `db.big`, `missions.big` | `custom_bigs/db_custom.big`, `missions_custom.big`, `global_locators_dt.rx2` | `ots_univ_02` |
| `dist_industrial` | Industrial District | Disc `db.big`, `missions.big` | `custom_bigs/db_custom.big`, `missions_custom.big`, `global_locators_ind.rx2` | `ots_univ_02` |
| `dist_dannyway`, `dist_dwmc`, `dist_dwgh` | Danny Way's Hawaiian Dream | `dway_park_00000000.big` | `custom_bigs/dway_park_custom.big` | `ots_dwmc_01` |
| `dist_maloof`, `dist_mmcn` | Maloof Money Cup 2010 NYC | `maloof_money_cup_00000000.big` | `custom_bigs/maloof_custom.big` | `ots_mmcn_01` |
| `dist_artgallery`, `dist_zen`, `dist_play` | San Van Party Pack (Art Gallery & Zen Plaza) | `play_00000000.big` | `custom_bigs/play_custom.big` | `ots_artg_01` |

> [!TIP]
> **Automatic Locator Assignment**: Pack authors do not need to look up or hardcode internal RenderWare locator strings. If the `"locator"` property is omitted from a spot in `pack.json`, the compositor dynamically detects and binds an available cut or spare locator in the target district or DLC world.

Global text localization strings and camera intro sequences are automatically merged into `custom_bigs/miscboot_custom.big` so challenge titles and descriptions resolve everywhere regardless of detection order.

---

## 4. Managing Packs (CLI)

Use `tools/pack_manager.py` to inspect, enable, disable, and scaffold packs:

### List Installed Packs
```bash
python3 tools/pack_manager.py list
```
Output:
```text
================================================================================
STATUS     ID                   NAME                     VERSION  CHALLENGES
================================================================================
[ENABLED]  hawaiian_dream_ots   Hawaiian Dream OTS       1.0.0    1 spots
    - ots_dwmc_03  'Mega Ramp Gap' (dist_dannyway)
[ENABLED]  west_coast_ots       West Coast OTS           1.0.0    5 spots
    - ots_univ_16  'Snoop' (dist_university)
    - ots_univ_17  'Dre' (dist_university)
    - ots_univ_18  'Eazy-E' (dist_university)
    - ots_univ_19  'Cube' (dist_university)
    - ots_univ_20  'Warren' (dist_university)
================================================================================
Active in game: 2 pack(s) (hawaiian_dream_ots, west_coast_ots)
```

### Enable a Pack
```bash
python3 tools/pack_manager.py enable hawaiian_dream_ots
```

### Disable a Pack
```bash
python3 tools/pack_manager.py disable hawaiian_dream_ots
```
*Note: When all packs targeting a DLC or the base game are disabled, their custom bigs are cleanly removed/reverted, and the recomp automatically runs stock retail data.*

### Manual / File-Manager Toggle
You can also toggle packs without the CLI:
- Set `"enabled": false` in `pack.json`
- Or rename the folder to `<pack_id>.disabled`

On next game restart, the recomp automatically synchronizes and loads the active configuration.

---

## 5. Authoring New Packs

### Scaffold a New Pack
```bash
# Scaffold for Base Game University:
python3 tools/pack_manager.py scaffold my_pack --name "My University Pack" --district dist_university

# Scaffold for Danny Way's Hawaiian Dream:
python3 tools/pack_manager.py scaffold hawaii_mod --name "Hawaii Mod" --district dist_dannyway

# Scaffold for Maloof Money Cup:
python3 tools/pack_manager.py scaffold maloof_mod --name "Maloof Mod" --district dist_maloof

# Scaffold for San Van Party Pack:
python3 tools/pack_manager.py scaffold artg_mod --name "Art Gallery Mod" --district dist_artgallery
```

### `pack.json` Schema
```json
{
  "id": "hawaiian_dream_ots",
  "name": "Hawaiian Dream OTS",
  "version": "1.0.0",
  "author": "Skate 3 Recomp Community",
  "description": "Custom Own-The-Spot challenge in Danny Way's Hawaiian Dream compound.",
  "enabled": true,
  "spots": [
    {
      "key": "mega_ramp_gap",
      "title": "Mega Ramp Gap",
      "desc": "Drop in and launch over the mega gap to own Danny Way's Hawaiian Dream.",
      "district": "dist_dannyway",
      "site": [137.31, -17.27, -23.23],
      "runway": 16.0,
      "scores": {
        "own": 800,
        "kill": 1200
      }
    }
  ]
}
```

### Spot Attributes:
- **`key`** *(Optional, Recommended)*: A unique identifier for the spot within your pack (e.g. `"mega_ramp_gap"` or `"cloud69_snoop_ledge"`). This allows you to safely rename the spot's display `title` in future pack updates without breaking players' career save game progress. If omitted, the compositor defaults to using `title`.
- **`title`**: The display name of the spot in menus and challenge banners. Supports per-language dictionaries or a plain string.
- **`desc`**: The challenge description shown on the intro screen and pause menu.
- **`district`**: The target DLC or base game district (`university`, `downtown`, `industrial`, `dannyway`, `maloof`, `play`, etc.).
- **`site`**: `[X, Y, Z]` coordinates of the obstacle prop / scoring area.
- **`runway`** *(Optional)*: Lead-in approach distance in meters (defaults to 16.0m) along the runway vector for skater spawn and orientation.
- **`scores`** *(Optional)*: Target `own` and `kill` point thresholds (defaults to 600 / 1000). See [OTS Design Guide](OTS_DESIGN_GUIDE.md#8-score-theory--target-balancing).
- **`cam`** *(Optional)*: Custom camera coordinates `[X, Y, Z]`. If omitted, the compositor automatically calculates a clean dynamic framing angle from `site`.
- **`locator`** *(Optional)*: Custom RenderWare locator pin. If omitted, the compositor automatically binds an available cut/spare locator from the district.

> [!NOTE]
> **Persistent Spot Registry (`mods/.spot_registry.json`)**:
> Low-level binary engine IDs (`ots_dwmc_03`, `num: 3`, GUIDs, and locators) are automatically managed and permanently pinned in `mods/.spot_registry.json`. Pack authors never need to worry about 11-byte ASCII limits, number collisions, or coordinating ID assignments with other modders. Player save files and high scores remain permanently locked to your spot across installing, removing, or re-ordering mods.

---

## 6. How the Recomp Loads Custom DLC & Bigs

1. At startup, `Skate3BaseApp::InstallCustomBigOverrides()` in `src/skate3_app_common.cpp` checks `custom_packs/`.
2. It invokes `tools/pack_compositor.py` to verify the state hash of active packs against `custom_bigs/.pack_state.json`.
   - If nothing changed: execution continues in < 15ms.
   - If packs were added, modified, enabled, or disabled: the compositor merges enabled packs on top of the clean stock disc and DLC bigs into `custom_bigs/`.
   - If 0 packs are enabled: overrides are bypassed, loading stock retail data.
3. VFS symlinks transparently redirect the guest filesystem calls:
   - Base game: `db.big`, `miscboot.big`, `missions.big`, and `global_locators_univ.rx2`.
   - DLC packages: symlinks are registered across `\Device\Content\1..32\<dlc_big>` and `dlcbig:\<dlc_big>` to intercept guest DLC loads regardless of enumeration order.

For instructions on deploying the Windows build on **Steam Deck** using Proton, see the [Steam Deck Proton Deployment Guide](STEAM_DECK_PROTON.md).
