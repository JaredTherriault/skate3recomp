# Master Guide: Designing & Authoring Own-The-Spot (OTS) Challenges

This guide documents the technical architecture, design principles, mathematical tools, and lessons learned from reverse-engineering and authoring custom **Own-The-Spot (OTS)** challenges across Skate 3 (both the retail disc base game and all official DLC expansions).

---

## 1. Overview & Philosophy

In Skate 3, an Own-The-Spot (OTS) challenge is a self-contained, micro-session built around a single skateable feature or line. The player must beat an **Own It** score to conquer the spot and an optional **Kill It** score to master it.

A successful OTS challenge requires four elements working in harmony:
1. **Compelling Geometry**: A skateable obstacle with an intuitive approach and landing.
2. **Cohesive Visuals**: Clean, unified markers with no floating junk or misaligned chevrons.
3. **Smooth Flow**: A reliable spawn on solid ground oriented directly at the feature with proper lead-in speed.
4. **Fair, Engaging Scoring**: Point thresholds balanced against the obstacle's physics, airtime, and combo potential.

---

## 2. The Four Architectural Layers

Every OTS spot lives simultaneously across four distinct subsystems in the Skate 3 engine:

```
┌────────────────────────────────────────────────────────────────────────┐
│                        1. WORLD / GEOMETRY LAYER                       │
│  Global Locators (RX2) ─── Freeskate Map Pin ─── World Activation Arrow │
└───────────────────────────────────┬────────────────────────────────────┘
                                    │ Activates
┌───────────────────────────────────▼────────────────────────────────────┐
│                       2. MISSION RUNTIME LAYER                         │
│  cSim_Global.xsf ─── ots_*_Sim.loc ─── Spawn Points ─── Trick Indicator│
└───────────────────────────────────┬────────────────────────────────────┘
                                    │ Scores inside Volume
┌───────────────────────────────────▼────────────────────────────────────┐
│                       3. DATABASE / ATTRIB VAULT                       │
│  main.vlt / dlc_*.vlt ─── challenge_objectives_group ─── Own/Kill Pts  │
└───────────────────────────────────┬────────────────────────────────────┘
                                    │ UI & Presentation
┌───────────────────────────────────▼────────────────────────────────────┐
│                    4. INTERFACE / LOCALIZATION LAYER                   │
│  LANGUAGE_*.BIN ─── Title & Description ─── scene_*.bin (Flythrough)   │
└────────────────────────────────────────────────────────────────────────┘
```

1. **World / Geometry Layer**:
   - Packaged in `.rx2` (RenderWare / EA Graphic binary) and companion `.loc` XMLs.
   - Defines the **Freeskate Activation Marker** (the overhead glowing arrow visible while skating around the city) and the initial spawn anchor.
2. **Mission Runtime Layer**:
   - Packaged in `data/content/missions/ots_<id>/`.
   - `cSim_Global.xsf`: Binary XML containing mission state machines, player spawn waypoints, camera tracks, and volume bounds.
   - `ots_<id>_Sim.loc`: 16-float transformation matrices for spawn locators (`startlocator`), visual indicators (`_vis_2`), and waiting pads (`waitlocator`).
3. **Database / Attrib Vault Layer**:
   - Packaged in `data/db/challengebanks/*.vlt` and `*.bin`.
   - Defines the challenge registry (`challenges`), objective groups (`challenge_objectives_group`), target point values (`BaseScore`), and progression links (`progressionbanks`).
4. **Interface / Localization Layer**:
   - Packaged in `LANGUAGE_English_*.BIN` (hashed string lookup table) and `scene_*.bin` (cinematic camera intro registry).

---

## 3. Arrow Unification (Canon Location Theory)

A common issue in early custom OTS spots is **"split arrows"**:
- The **Activation Arrow** (visible in Freeskate) appears at one location (or at `0, 0, 0`).
- The **Trick Region Arrow** (the glowing target marker once the challenge begins) appears at a completely different location.

### Why Arrows Separate
The Freeskate activation arrow is driven by the district `.rx2` global locator record (`film_*_challengelocator_01` or `street_*_challengelocator_01`), whereas the in-challenge trick marker is driven by the mission's `_vis_2` transform in `_Sim.loc`. If these coordinates do not match to the millimeter, the player experiences a jarring disconnect between where they start the challenge and where they are judged.

### The Canon Spot Rule
Every OTS spot must define **one single canon obstacle coordinate** (`site` in `pack.json`):
- Both the district `.rx2` locator and its companion BAM XML locator must sit at `site`.
- The mission's `_vis_2` indicator transform in `_Sim.loc` must sit at `site`.
- The in-challenge active trick volume must be centered on `site`.

### Critical Constraint: RX2 In-Place Modification
`.rx2` files use strict internal table-of-contents structures with absolute byte offsets to child spawn arrays and name string tables.
> [!CAUTION]
> **Never append new locator records or resize `.rx2` files.**
> Appending new locators shifts internal offsets. When the game's TOC parser encounters shifted offsets, the locator silently fails to load and the game defaults its position to `(0, 0, 0)` under the world map or falls into the kill plane.

Always use **in-place updating** (`update_rx2_in_place` in `pack_compositor.py`):
1. Locate the existing target locator record by scanning the name table at `0x230`.
2. Overwrite the locator's 3-float position (`+0x30`) and bounding box (`+0x40`, `+0x50`) directly.
3. Overwrite the child spawn records (`cp + j * 0x70`) with calculated spawn positions and forward matrices.
4. The file length and TOC offsets remain 100% byte-exact.

---

## 4. Spawn Placement, Runway & Approach Geometry

A great spot requires a predictable, clean approach. If the player spawns facing the wrong way, too close to the obstacle, or suspended over empty air/water, the spot is unplayable.

### Coordinate Space
Skate 3 uses a Right-Handed Coordinate System:
- **$+X$**: East / Right
- **$+Y$**: Up (Elevation above sea level)
- **$+Z$**: South / Forward (depending on local quadrant)

### Calculating Approach Vector and Rotation Matrix
Given a canon obstacle `site` $(s_x, s_y, s_z)$ and a chosen starting coordinate `start` $(p_x, p_y, p_z)$:
1. Calculate the ground displacement vector in the horizontal plane ($X, Z$):
   $$\Delta x = s_x - p_x, \quad \Delta z = s_z - p_z$$
2. Normalize to unit direction vector $(u_x, u_z)$:
   $$L = \sqrt{\Delta x^2 + \Delta z^2}, \quad u_x = \frac{\Delta x}{L}, \quad u_z = \frac{\Delta z}{L}$$
3. Compute the lateral perpendicular vector $(p_x, p_z)$ for multiplayer spawn spread:
   $$p_x = -u_z, \quad p_z = u_x$$
4. Construct the $3 \times 4$ rotation matrix (row 2 represents the forward direction in world space):
   $$\mathbf{R} = \begin{bmatrix} u_z & 0.0 & -u_x & 0.0 \\ 0.0 & 1.0 & 0.0 & 0.0 \\ u_x & 0.0 & u_z & 0.0 \end{bmatrix}$$

### Multi-Player Spawn Spread
For multiplayer and retry stability, spawn locators feature child spawn positions (`nc = 6`). To avoid players spawning on top of each other, distribute child spawns along the lateral vector $(p_x, p_z)$:
$$\text{pos}_j = \left( p_x + \left(j - \frac{n_c - 1}{2}\right) \cdot w \cdot p_x, \; p_y, \; p_z + \left(j - \frac{n_c - 1}{2}\right) \cdot w \cdot p_z \right)$$
where $w \approx 0.3\text{m} \dots 0.4\text{m}$ lateral spacing. Ensure the total spread $\approx 1.5\text{m} \dots 2.0\text{m}$ fits within the runway width.

### Runway Length Calibration
- **Short / Technical Runway (8m – 11m)**:
  - Yields 1–2 pushes.
  - Ideal for low ledges, small funboxes, flat ground pop-ins, and tight courtyard corners.
- **Standard Street Runway (14m – 18m)**:
  - Yields 3–4 pushes, reaching near maximum flatland speed.
  - Ideal for handrails, 8–10 stair sets, and flat-to-down features.
- **High-Speed / Bank Runway (18m – 25m+)**:
  - Roll-ins down slopes or banks.
  - Ideal for mega-gaps, 12+ stair sets, or features requiring maximum pop and airtime.

### Elevation ($Y$) Continuity & Void Checks
> [!WARNING]
> Always verify that the runway surface is solid ground. In DLC maps (such as Hawaii or Art Gallery), waterways, trenches, and planters run adjacent to plazas.
- Check surrounding prop elevations in the target area.
- Ensure the runway slope $(s_y - p_y)$ is gentle ($\le 1.0\text{m}$ change per 15m) unless intentionally placing the start atop a roll-in bank.

---

## 5. Chevron Suppression & Visual Clarity

Retail EA missions include various auxiliary visual indicators:
- `_vis_1`, `_vis_3`: Secondary prop / landing zone highlights.
- `_vis_chev_1`, `_vis_chev_2`, `_vis_chev_3`: Animated floating neon chevrons intended to guide the player's path.

In custom OTS challenges, these legacy chevrons often point towards the old template obstacle (e.g. pointing into walls or off cliffs).

### The Suppression Technique
During mission synthesis in `pack_compositor.py`:
1. **String Renaming**: Rename string references from `_vis_` to `_dum_` in `.xst` and `.xsf` files.
2. **Coordinate Parking**: In `retarget_csim()`, identify all secondary indicator transforms (`_vis_1`, `_vis_3`, `_vis_chev_*`) and park their coordinates $5,000\text{m}$ below the world ($Y = -5000.0$).
3. Only the canon `_vis_2` indicator is retained and positioned directly over the obstacle at $(s_x, s_y + 0.3, s_z)$.

---

## 6. Dynamic Camera & Flythrough Sequences

When a player selects an OTS challenge from the menu or world map, a cinematic camera flythrough plays before handing control to the skater.

### Scene Intro Registration
- Every district has a companion scene file (e.g. `data/scene/scene_002.bin` for Maloof, `scene_003.bin` for Hawaii, `scene_006.bin` for Art Gallery).
- The compositor injects a 104-byte camera intro record for `<challenge_id>_intro` using `loc_hash`.
- This ensures the UI preview sequence plays seamlessly without crashing the game's presentation engine.

### Camera Orientation (`cam`)
- Place `cam` along the approach corridor, slightly offset and elevated:
  - Position: Approximately midway between `start` and `site`, elevated by $1.5\text{m} \dots 2.5\text{m}$.
  - Look-at target: Pointing toward `site`.

---

## 7. Obstacle Selection & Evaluation Criteria

Not every object in Port Carverton or the DLC compounds makes a great OTS spot. Follow these evaluation criteria before committing to coordinates:

| Feature | Viability | Notes |
|---|---|---|
| **Stair Sets (6–10 steps)** | ⭐⭐⭐⭐⭐ **Ideal** | Standard OTS staple. Easy to score with flips, spins, and grabs. |
| **Center Handrails / Hubbas** | ⭐⭐⭐⭐⭐ **Ideal** | Long contact time for grinds and slides. Ample combo potential. |
| **Courtyard Funboxes** | ⭐⭐⭐⭐ **Great** | Good for technical low-speed street lines. Keep target scores moderate. |
| **Bridge Rails / Gaps** | ⭐⭐⭐⭐ **Great** | Great visual appeal. Watch out for kinks that eject the skater early. |
| **Bank-to-Wall / Tranny** | ⭐⭐⭐ **Good** | Good for carving and transfers, but trick detection can be finicky. |
| **Flatland Only** | ❌ **Unviable** | Mechanically impossible to reach high score thresholds without an obstacle. |
| **Tight Blind Landings** | ❌ **Unviable** | Landing directly into walls, benches, or out-of-bounds volumes ruins player flow. |

The best, most memorable OTS challenges are built on obstacles where players can build great speed or see a great view.

---

## 8. Score Theory & Target Balancing

Skate 3's trick engine awards points based on:
1. **Base Trick Value**:
   - Basic flip trick (kickflip, heelflip): ~100 pts.
   - Advanced flip trick (360 flip, hardflip, laser flip): ~150–200 pts.
   - Rotations: $+50$ pts (180°), $+150$ pts (360°).
2. **Grind & Slide Distance**:
   - 50-50 / boardslide base: ~100–150 pts + distance accumulation.
   - Crooked / bluntslide / darkslide base: ~200–350 pts + distance accumulation.
3. **Airtime & Drops**:
   - Drops and gap clearances add multipliers and airtime bonuses ($+100 \dots +300$ pts).
4. **Multiplier Acceleration**:
   - Consecutive trick chains, manuals, and high-speed pop build up multipliers ($1.5\times, 2\times, 3\times$).

### The Three-Tier Scoring Matrix (EA Black Box Benchmark)

Extracted from all 16 retail disc OTS challenges and official DLC banks:

| Difficulty Tier | Own It | Kill It | Typical Geometry & Conditions |
|---|:---:|:---:|---|
| **Tier 1: Low / Technical** | **500 – 650** | **850 – 1000** | Short flat runway ($< 11\text{m}$), low drops ($\le 1.2\text{m}$), compact funbox, tight alley ledges, or high kinked rail that limits slide duration. |
| **Tier 2: Mid / Standard** | **700 – 750** | **1000 – 1150** | Flat plaza runway ($14\text{m} – 17\text{m}$), standard handrail, 6–10 stair set, long flat rail, medium gap. |
| **Tier 3: High / Banger** | **850 – 1000** | **1200 – 1500+** | High-speed bank roll-in ($> 18\text{m}$), 12+ stairs, massive vertical drops ($\ge 2.5\text{m}$), huge transfer walls, giant industrial rails. |

### Technical Score Injection
Target scores are stored in the challenge bank (`data/db/challengebanks/*.vlt`) under attribute key `0x0c0cfefb9fc6eb73` (`BaseScore`) inside the DatN payload of:
- `<challenge_id>_0` $\to$ **Own It** score
- `<challenge_id>_1` $\to$ **Kill It** score

`update_challenge_scores()` automatically patches these values directly during package compilation.

---

## 9. Base Game Disc vs DLC Architectures

| Characteristic | Base Game (Port Carverton) | Official DLC (Maloof, Art Gallery, Hawaii) |
|---|---|---|
| **District IDs** | `dist_university`, `dist_downtown`, `dist_industrial` | `dist_maloof`, `dist_artgallery`, `dist_dannyway` |
| **Primary Container** | Disc `db.big`, `missions.big` | Standalone `<dlc_pack>_00000000.big` in STFS |
| **Composed Output** | `custom_bigs/{db,missions,miscboot}_custom.big` | `custom_bigs/<dlc_custom>.big` |
| **Locator Storage** | `global_locators_univ.rx2` | `DLC_BAM/.../*.rx2` inside the DLC big |
| **VFS Redirection** | Virtual filesystem symlink overrides | Guest path intercepts (`\Device\Content\...` & `dlcbig:\`) |
| **Challenge Bank** | `data/db/challengebanks/main.vlt` | `data/db/challengebanks/dlc_<name>.vlt` |
| **Progression Group** | `progression_group/ots` | `progression_group/dlc_<name>_ots` |

---

## 10. Authoring Checklist for New Spots

When authoring a new OTS challenge:

1. [ ] **Survey Coordinates**:
   - Find candidate obstacle in `analysis/dlc/` or via in-game locator dumps.
   - Record exact `site` coordinates on the obstacle center.
2. [ ] **Determine Runway & Start**:
   - Choose a flat, dry start position $10\text{m} \dots 18\text{m}$ back from the feature.
   - Confirm runway elevation ($Y$) matches the terrain without clipping or falling.
3. [ ] **Select Target Scores**:
   - Classify obstacle into Tier 1 (500–650), Tier 2 (700–750), or Tier 3 (850–1000).
   - Set `"scores": { "own": X, "kill": Y }`.
4. [ ] **Name & Wordplay**:
   - Craft a creative, skate-culture pun or landmark reference for `"title"` and `"desc"`.
5. [ ] **Compose & Verify**:
   - Run `python3 tools/pack_compositor.py --force`.
   - Verify binary outputs in `build/custom_bigs/`:
     - Global RX2 locator at `site`.
     - RX2 child spawns at `start` facing $(u_x, 0, u_z)$.
     - `_Sim.loc` startlocator and `_vis_2` transforms.
     - `challengebanks/*.vlt` Own It and Kill It score integers.
