# Modular Challenge Packs (Recomp Custom DLC)

Skate 3 Recomp supports modular, toggleable custom challenge packs. This allows authors to create and distribute challenge packs that players can enable or disable at will with a game restart, without touching or dirtying base game retail files.

---

## 1. Directory Structure

Custom packs live in `custom_packs/` (or `build/custom_packs/`):

```text
custom_packs/
└── west_coast_ots/
    ├── pack.json             # Pack metadata and challenge definitions
    └── missions/             # (Optional) Pre-built custom mission payloads
        └── ots_univ_16/
            └── cSim_Global.xsf
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
7. On game startup, the recomp automatically compiles the newly enabled mods into the virtual filesystem cache (or reverts cleanly to pure retail disc state if all mods are turned off).

---

## 3. Managing Packs (CLI)

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
[ENABLED]  west_coast_ots       West Coast OTS           1.0.0    5 spots
    - ots_univ_16  'Snoop' (dist_university)
    - ots_univ_17  'Dre' (dist_university)
    - ots_univ_18  'Eazy-E' (dist_university)
    - ots_univ_19  'Cube' (dist_university)
    - ots_univ_20  'Warren' (dist_university)
================================================================================
Active in game: 1 pack(s) (west_coast_ots)
```

### Enable a Pack
```bash
python3 tools/pack_manager.py enable west_coast_ots
```

### Disable a Pack
```bash
python3 tools/pack_manager.py disable west_coast_ots
```
*Note: When all packs are disabled, `custom_bigs/` is restored to 100% clean stock retail disc state, and the recomp automatically runs pure vanilla retail data.*

### Manual / File-Manager Toggle
You can also toggle packs without the CLI:
- Set `"enabled": false` in `pack.json`
- Or rename the folder to `<pack_id>.disabled`

On next game restart, the recomp automatically synchronizes and loads the active configuration.

---

## 4. Authoring New Packs

### Scaffold a New Pack
```bash
python3 tools/pack_manager.py scaffold my_new_pack --name "My New Pack"
```

### `pack.json` Schema
```json
{
  "id": "my_new_pack",
  "name": "My New Pack",
  "version": "1.0.0",
  "author": "Skater",
  "description": "Custom Own-The-Spot challenges.",
  "enabled": true,
  "spots": [
    {
      "id": "ots_univ_21",
      "num": 21,
      "title": "Stair Set Ledge",
      "desc": "Score big on this university stair set.",
      "district": "dist_university",
      "locator": "ots_univ_10_challengelocator_01",
      "site": [410.5, 72.0, -255.0],
      "cam": [408.0, 73.5, -257.5],
      "runway": 16.0,
      "scores": {
        "own": 600,
        "kill": 1000
      }
    }
  ]
}
```

### Key Spot Attributes:
- **`site`**: `[X, Y, Z]` coordinates of the obstacle prop / scoring area.
- **`cam`**: `[X, Y, Z]` coordinates of the camera capture position.
- **`runway`**: Lead-in distance in meters (defaults to 16.0m) along the approach vector for player spawn and orientation.
- **`locator`**: Base game district locator name to use for the map pin.
- **`scores`**: Target `own` and `kill` point thresholds.

---

## 5. Gameplay Engine Features Built-In

The compositor automatically enforces the 3 established gameplay fixes:
1. **Dynamic Camera Flythroughs**: Waypoints are generated in orbit around the obstacle, ending in a runway approach cut into gameplay.
2. **Visual Indicator Cleanup**: Extra start arrows and collapsing path chevrons are suppressed; only the downward obstacle indicator is rendered.
3. **Runway Starting Positions**: Player spawn and orientation vectors are computed along the lead-in approach angle with adequate distance to build trick speed.

---

## 6. How the Recomp Loads Custom Packs

1. At startup, `Skate3BaseApp::InstallCustomBigOverrides()` in `src/skate3_app_common.cpp` checks `custom_packs/`.
2. It invokes `tools/pack_compositor.py` to verify the state hash of active packs against `custom_bigs/.pack_state.json`.
   - If nothing changed: execution continues in < 15ms.
   - If packs were added, modified, enabled, or disabled: the compositor merges enabled packs on top of the clean stock disc bigs into `custom_bigs/`.
   - If 0 packs are enabled: overrides are bypassed, loading stock retail data.
3. VFS symlinks transparently redirect the guest filesystem calls (`db.big`, `miscboot.big`, `missions.big`, and `global_locators_univ.rx2`) to `custom_bigs/`.
