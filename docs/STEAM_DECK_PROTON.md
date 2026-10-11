# Running Skate 3 Recomp on Steam Deck (Windows Build via Proton)

This guide explains how to deploy and test Skate 3 Recomp and custom Own-The-Spot (OTS) challenge packs on the **Steam Deck** using the Windows (`skate3.exe`) build through Steam's Proton compatibility layer.

---

## Why Use the Windows Build via Proton?

On Steam Deck (SteamOS), the native Linux binary can occasionally encounter toolchain, glibc, or display server inconsistencies across SteamOS updates. Running the Windows x86_64 binary (`skate3.exe`) via **Proton Experimental** or **GE-Proton** provides:
- Seamless controller mapping and Steam Input integration.
- Stable Vulkan presentation pipeline via DXVK / VKD3D-Proton.
- Standardized directory layout matching native Windows game folders.

---

## 1. Directory Structure on Steam Deck

Place your game files in a single folder on your Steam Deck (for example: `/home/deck/Games/Skate3/` or on your MicroSD card at `/run/media/mmcblk0p1/Games/Skate3/`):

```text
Skate3/
├── skate3.exe                      # Windows binary
├── skate3.toml                     # Configuration file
├── game/                           # Retail game disc dump (data/...)
│   └── data/
├── dlc/                            # Official DLC packages (raw STFS containers)
│   ├── E8CB22B7FEA270B0...         # Danny Way's Hawaiian Dream
│   ├── 0165E5B906309F63...         # Maloof Money Cup 2010 NYC
│   ├── 8576AE236D00CF0FD...        # San Van Party Pack (Art Gallery)
│   └── D1472E11CFD502BC...         # Black Box Park
└── custom_bigs/                    # Pre-composed custom challenge bigs
    ├── .active                     # Sentinel file indicating active modded state
    ├── db_custom.big
    ├── miscboot_custom.big
    ├── missions_custom.big
    ├── global_locators_univ.rx2
    ├── dway_park_custom.big
    ├── maloof_custom.big
    └── play_custom.big
```

---

## 2. Transferring Custom Challenges (`custom_bigs/`)

Under Proton, Python 3 is typically not installed inside the Wine prefix. Therefore, **always pre-compose your packs on your PC before transferring them to Steam Deck**:

1. **On your Development PC**:
   Compose all active challenge packs:
   ```bash
   python3 tools/pack_compositor.py --force
   ```
2. **Copy the Output**:
   Copy the entire `build/custom_bigs/` folder from your PC into your Steam Deck game directory as `custom_bigs/` (sitting next to `skate3.exe`).
   
   > [!IMPORTANT]
   > Make sure the hidden `.active` sentinel file inside `custom_bigs/` is copied over. If `.active` is missing, the engine assumes no mods are enabled and will boot purely into stock retail disc/DLC data.

---

## 3. Official DLC Setup on Steam Deck

The recomp engine includes automatic DLC discovery and mounting.

1. Create a folder named `dlc` next to `skate3.exe`:
   ```bash
   mkdir -p dlc
   ```
2. Drop your official DLC files (the unextracted STFS packages) directly into `dlc/`.
3. At launch, `DiscoverDlcSourceDirectories()` scans `<executable_folder>/dlc/`, reads the STFS package headers, and automatically mounts them to the virtual console device slots (`\Device\Content\1..32\...`).
4. In turn, `custom_bigs/` provides the corresponding patched big overrides (`dway_park_custom.big`, `maloof_custom.big`, `play_custom.big`) to inject custom spots, unified arrows, and tuned scores into the DLC environments.

---

## 4. Quiver Launcher Setup on Steam Deck

If you use **Quiver Launcher** (the popular recomp/decomp manager for Steam Deck), Quiver automates downloading releases directly from GitHub, staging them into `Apps/`, and launching Windows binaries through Proton.

### Pointing Quiver to Your Fork

Because you forked the repository to `JaredTherriault/skate3recomp`, you need Quiver to pull from your repository instead of upstream `mchughalex/skate3recomp`:

#### Option A: Inside the Quiver UI (Recommended)
1. Open **Quiver Launcher** on your Steam Deck (Desktop or Gaming Mode).
2. If Skate 3 is already in your library:
   - Highlight the **Skate 3** card.
   - Press **Select / Options** (or right-click in Desktop Mode) and choose **Edit Entry**.
   - Change the **Repository** field to:
     ```text
     JaredTherriault/skate3recomp
     ```
   - Save your changes.
3. If adding fresh:
   - Click **+ Add New Entry**.
   - **Name**: `Skate 3`
   - **Repository**: `JaredTherriault/skate3recomp`
   - **Folder Name**: `Skate3-Skate3Recompiled`
   - Save.

#### Option B: Editing `apps.json` Directly
1. On Steam Deck in Desktop Mode, open Quiver's library file:
   - Portable: `<Quiver_Folder>/apps.json`
   - System install: `~/.local/share/QuiverLauncher/apps.json`
2. Locate the `"name": "Skate 3"` entry and update `"repository"`:
   ```json
   {
     "name": "Skate 3",
     "project": "Skate 3: Recompiled",
     "repository": "JaredTherriault/skate3recomp",
     "folderName": "Skate3-Skate3Recompiled",
     "appIconUrl": "https://raw.githubusercontent.com/mchughalex/skate3recomp/refs/heads/main/banner.png",
     "tags": ["recomp", "x360", "skate", "xbox", "xbox 360", "recompilation"]
   }
   ```
3. Save the file and restart Quiver Launcher.

### GitHub Release Asset Requirement

Quiver detects releases via the GitHub API (`/repos/JaredTherriault/skate3recomp/releases/latest`). For Quiver to install the Windows version, the release **must have a release asset archive attached**:
- Asset Name: `Skate3Recomp-Windows.zip`
- Archive Contents:
  ```text
  Skate3Recomp-Windows/
  ├── skate3.exe
  ├── rexruntime.dll
  ├── skate3.toml
  └── custom_bigs/
      ├── .active
      └── ... (.big files)
  ```
When you click **Download / Install** in Quiver, it will download `Skate3Recomp-Windows.zip`, extract it to `Apps/Skate3-Skate3Recompiled/`, and configure Proton runner automatically.

---

## 5. Manual Steam / Proton Setup

---

## 5. Verification on Steam Deck

When the game launches, the engine logs its mount operations to `rexglue.log` in the game directory. You can verify that custom bigs and DLC are functioning by checking the log:

```text
[INFO] Skate 3 custom bigs: RESOLVE 'data\big\db.big' -> FOUND
[INFO] Skate 3 custom bigs: RESOLVE 'dway_park_00000000.big' -> FOUND
[INFO] Skate 3 custom bigs: RESOLVE 'play_00000000.big' -> FOUND
[INFO] Skate 3 custom bigs: RESOLVE 'maloof_money_cup_00000000.big' -> FOUND
[INFO] Installed Skate 3 custom big overrides (7 files) from Z:\path\to\custom_bigs
```

All custom spots (**"Bridge Over Tranquil Water"**, **"Nine to Five"**, **"Regs to Riches"**, and West Coast spots) will appear on the Challenge Map with their custom flythroughs, unified obstacle markers, and tuned point targets.
