
## PASS 26 (2026-09-24, fix-3) — the runtime capture: the hook designed + implemented; the offline annotation tool = ready; statically validated
### 1. THE HOOK SITE (a non-invasive guest observation hook)
**`sub_828E4A88`** — the pack-loc loader's loc-DB image register fn. The raw-word verified chain:
`sub_828E4A88` = the bswap of [r3]/[r3+4] (LE→BE for the game) → `bl sub_828E5E18` @0x828E4B80
(the loc-DB parser). The hook dumps the guest buffer BEFORE `__imp__sub_828E4A88` so the bytes stay
in the on-disk LE order (the pass-21's layout correction: the FILE = LE, the memory = BE).
The ABI: `r3 = the loaded loc-DB image`, `r4 = the pack's loc handle`. The image size = `8 + usize`
(the [4..8] LE). The hook = pure observation (no register/state mutation).
### 2. THE ACTIVATION (default OFF, cvar-gated, one line)
`skate3_loc_dump = true` (cvar, `kHotReload`) → the dump fires once per unique image to
`<app root>/loc_dump/` (or `skate3_loc_dump_dir`'s absolute override). The dedupe = a 16-byte FNV
hash + the usize, so the game's re-registration across the language-select re-entry = skipped. Zero
overhead when the cvar is off (a single 32-bit read + compare). The contract: the decrypted loc-DB
streams only (a non-0x00039000 magic = skipped, so the challenge-local-data banks and the .xsf
textures = not dumped).
### 3. WHY REX_FUNC (the SetFunction conflict recorded)
`0x828E4A88` / `0x828E5E18` = NOT explicit function entries in `config/skate3_functions.toml` — the
codegen's boundaries for this region = graph-derived (they sit inside `sub_82CDD390`'s chain). A
dispatcher `SetFunction` would require a boundary override (and could conflict with the codegen's
mid-chain boundaries). The `REX_FUNC(sub_828E4A88)` route resolves through the recomp function table
like any other direct reference. **The exact conflict is documented; the REX_FUNC route = proposed.**
### 4. STATIC VALIDATION against disasm.sqlite
The hook's xrefs: `sub_828E4A88 ← bl @0x82CDD504` and `bl @0x82CDD994` (the two archive registers
per pack). The parse chain: `sub_828E4A88` → the bswap loop → `bl sub_828E5E18` @0x828E4B80. The
wrapper `sub_82A5D348`: `[0x30] = [Job+0x28]`; `[0x34] = [Job+0x2c]`; then the machine; then
`[0x6c] = r3` and `[0x28]/[0x2c]` = the src/dst (the cursor rollback). The relocated-code
consideration: the recomp's skate3 module = guest addresses 1:1 with the retail image (no base
shift). `base` in rexglue = the guest-memory base, so the host address for `g` = `base + (g -
0x82000000)`.
### 5. THE OFFLINE FALLBACK ANALYSIS (`/tmp/opencode/xexre/loc_annotate.py`)
Implemented NOW: `parse_locdb` (the loc-DB layout parse with the self-describing size, so a partial
dump = detected); `ground_truth_check` (the ground-truth key resolution); `annotate_records` (writes
`otl_records_annotated.txt` and `ots_records_annotated.txt` kept SEPARATE per the standing rule, from
the record-table dumps in `analysis/xex/decoded_banks/dlc_decoded/`; the rows' `val` → the loc-key-
name substitution where the val = a loc-key hash present in the captured loc-DB's index; the
bitfield/0x5555/0xAAAA fills = the uninitialized defaults, skipped). `loc_map_<name>.txt` (the
loc-key hash → the display string pairs). `loc_annotate_report.txt` (the ground-truth resolution
report).
### 6. THE TOOL'S STATIC TEST (a synthetic loc-DB = the vault@0x440's crib)
Feeding `probe26_test_locdb.bin` (the dannyway pack vault's `@0x440` first 3,696 bytes = the ground
truth) to the tool:
    loc-DB danny_way_park count 190 stride 0x1c pool_off 0x60c
    ground-truth 00DF4A26 → pool+0x7d → Voltooi de tranny-demo van Danny Way
    ground-truth 149D7545 → pool+0x58 → Voltooi de street-demo van Danny Way
    OTL: matched 0/1072 rows (the bitfield vals = NOT the loc-key IDs — expected)
    OTS: matched 0/372 rows (the same)
    loc map: wrote loc_map_danny_way_park.txt
The tool = ready for a real capture. The 0/1072 matched for this image = expected (the pass-22's
finding: the record `val` = the bitfield data, NOT the loc-key IDs) — so the loc-key-name annotation
only closes once a language .BIN's capture provides the loc-key ID strings.
### 7. WHAT CANNOT BE BUILT LOCALLY AND WHY (documented)
A full compile requires `generated/sources.cmake` (the recomp codegen output, not in the tree; the
build instructs "run the generate-all target before building skate3") + the rexglue SDK submodule
(initialized in this tree) + the Windows/macOS host ABI. The hook file's includes match the
same-tree patterns, so a user build should resolve them; the local tree = LSP-limited (the include
path does not contain the SDK dir), so a full compile is NOT verifiable locally. The user must build
the recomp to run the capture; the cvar line = the only thing they need to flip.

---

# PASS 33 — the vaultlist.xml gate + the pack-loc vault loader

## The loader chain (0x826BC428)
'dlcbig:data\db\' + '*.vaultlist.xml' → parses the <VaultList><VaultFile> XML →
loads each listed vault from the pack's mounted content. The vaultlist.xml =
NOT XML in all packs: the dannyway's = the real <VaultList> XML (284 B, 4
<VaultFile> entries); the maloof/sanvanparty's = the 16B-record format
(encrypted/obfuscated — a different vaultlist generation); OUR pass-31 copy =
284 B of zeros (the misread) = an empty list = the loader loaded NOTHING.

## The pack loc vault format (verified: dlc_danny_way_park.vlt)
[zeros 0x440 pad][head: magic 0x9000/3 LE, usize LE, count LE, stride 0x1C LE,
pool_off LE][name 12B NUL-padded][rows [LE u32 key][LE u32 pool_off] sorted
by key][4B pad][pool strings]. Row0 of danny_way_park = (0x00DF4A26, 125) =
'Voltooi de tranny-demo van Danny Way' (the runtime-capture key). The row po =
the offset from the POOL START (after the 4B pad following the rows).

## The pack-loc vault loader gate
Our pack now carries the real vaultlist.xml with <VaultFile>dlc_morshu.vlt</
VaultFile> → the loader should open our pack-loc vault and register the
'Morshu's Paradise' loc row. The runtime proof = the loc-dump hook showing
a 6TH capture ('morshu', count 2).

## The probe matrix (the 024 log, re-read)
The failures follow a strict triangular pattern: the i-th mount's label probes
the tokens of the packs mounted AFTER it. The morshu token = probed on the
play mount (the label stable = the play pack across runs 021/024) and FAILED —
the cross-pack content scan = BENIGN (the morshu .big is not in the play pack).
Our own mount = NOT probed with the morshu token (the .big = found silently in
our own mount). => the pass-32 rename = correct and working; the probe
failures = the expected cross-pack scan.

---

# PASS 34 — the vault-gate forensics: the pack-loc vault loader instrumented

## The loc-DB capture count = the definitive evidence
All 5 sessions: the loc-dump hook captured 5 pack loc-DBs = the 5 OFFICIAL
pack-loc vaults. Our pack-loc vault ('morshu', count 2, the "Morshu's
Paradise" row) = NEVER captured. The 025 log confirms: the launcher
re-installed our package (the stale-copy delete = done); the installed copy
= the pass-33 rebuilt package (the real vaultlist XML verified at 0x780 of
the installed .big).

## The dlcbig: device alias
The guest's vaultlist glob = 'dlcbig:data\db\' + '*.vaultlist.xml'. The
recomp's InstallBigDeviceAliases maps 'dlcbig:' to \Device\Harddisk0\
Partition1 = the BASE game's partition. The packs' vaultlist.xml files =
inside the pack .bigs on the Content devices — never scanned via dlcbig:.
The official packs' pack-loc vaults = loaded via a DIFFERENT path (the
content devices or the pack registry).

## The pack-loc vault loader
sub_82D083D8 (TU3; retail 0x82CDD390) = called from the game-load module
chain sub_826D6BC8. The loader = a dispatcher-registered function, called
via the function-dispatcher table (the vault-open id from [0x82FEA50C]).
The 5 captures = the 5 official pack modules; our module = not in the
load list.

## The instrumentation
REX_FUNC(sub_82D083D8) added to skate3_loc_dump.cpp: the fire-trace +
the arg dump per loader invocation. The next user run tells us the module
count and the r3 descriptors.

# PASS 38 (2026-10-01) — player/camera coordinate debug display

## What was built (src/skate3_loc_dump.cpp, namespace skate3::coord_dump)
Two cvar-gated outputs, both default OFF, both verified in the 036 run:
(A) HUD `skate3_show_coords` (F7 in-game): ImGui window with the live
player + camera x/y/z. (B) log `skate3_coord_log`: ~4 Hz timestamped
lines to build/coord_dump/skate3_coords.log (tail -f).

## The data sources (existing hooks tapped — no new guest overrides)
1. PLAYER = the SkaterPresEntity world matrix: taps added to the
   EXISTING overrides sub_82783038 (SkaterPresEntity::DoubleBuffer) and
   sub_82785778 (skater-family UBT+Pack vtable driver) in
   skate3_native_render.cpp; r3 = the skater PresentationEntity. The
   tap reads m_MatLtoWTrans (+416, 16 floats: 3 affine rows with the
   translation in component 3 + (0,0,0,1) tail) — the same layout the
   native-entity identity store proved bit-exact against the accepted
   ROPA bank block (native/skate3_native_entity.cpp). Validation copied
   from ReadWorldRowsChecked; all reads via the SEH-guarded
   GuestTryCopy (native/skate3_native_guest_read.cpp).
2. CAMERA = the ViewCamera::SetViewMatrix override sub_82802A00 in
   skate3_native_scene.cpp (the freecam hook): r4 = the incoming view
   Matrix44; position = -t * R^T (the scene builder's formula at
   :9267-9277). Only the MAIN camera recorded (cam == g_sampler_viewcam,
   published by BuildFrameScene); the tap runs AFTER __imp__ so a
   freecam-engaged run reports the flown pose.
3. Player disambiguation: NPCs share the SkaterPresEntity class, so all
   live skater entities are tracked (16-slot table, 2 s age-out) and the
   reported "player" = nearest-to-camera (camera follows the player).
   The 036 run cross-validated: the camera sample trailed the player
   sample by ~2-3 m while both advanced coherently through real
   gameplay.

## Why it lives in skate3_loc_dump.cpp (the /tmp wipe + the configure trap)
Editing CMakeLists.txt forces a CMake re-configure; the configure
FATAL_ERRORs on the missing SKATE3_TITLE_UPDATE_PACKAGE
(/tmp/opencode/tu3.xex — lost when /tmp/opencode was wiped between
passes), and a FAILED regen edge retries on every subsequent ninja
invocation. Unblocked by rebuilding /tmp/opencode/tu3.xex as a minimal
hand-crafted STFS whose payload = the two already-staged byte-exact TU3
xexp files; cmake/ExtractTitleUpdateXexp.py extracts them BYTE-EXACT
(cmp-verified both). Package mtime backdated so ninja never re-runs
staging. The codegen custom commands depend on the staged xexps
ORDER-ONLY, so no codegen re-run was triggered. The ORIGINAL TU
12K2276 package is still missing from the machine — noted in
analysis/xex/findings.md PASS 38.

## Guest-thread safety
The taps run on guest threads: GuestTryCopy only (guarded), a short
mutex for the sample table (the same pattern native_entity uses on
guest threads), atomics for telemetry. The LOG write runs on a
dedicated thread reading atomics only — never on the guest thread
(the loc_dump synchronous-fwrite boot-hang lesson). No new entries in
config/skate3_functions.toml / skate3_tu_functions.toml were needed
(no new overridden addresses; the taps ride the existing overrides
documented in src/skate3_loc_dump.cpp / skate3_native_render.cpp).
No game-tree file touched.

# PASS 39 (2026-10-01) — "Player Coordinates" toggle in the ESC settings menu

## The menu plumbing (matched, not invented)
The wrapper's ESC menu = rex::ui::SimpleSettingsDialog
(third_party/rexglue-sdk/src/ui/overlay/simple_settings_overlay.cpp),
which already embeds app-layer cvars behind HasCvar gates (the FPS
Counter row, the Renderer Indicator row). The new row follows that
exact spec:
- staged `show_coords_` bool in the dialog class;
- LoadSettingsFromCvars reads skate3_show_coords at menu open;
- BuildRows (Video category) renders an Off/On enum row labeled
  "Player Coordinates" in the **Interface** section after "Renderer
  Indicator", desc text mentions F7 + the native-renderer requirement
  for the camera row;
- on_enum_change → SetBoolCvar("skate3_show_coords", value) +
  SaveSimpleSettingsConfig(config_path_) — apply + persist immediately;
- the cvar added to GetSimpleSettingsCvars() → persisted to the user
  settings toml (~/.local/share/skate3/settings.toml) by the menu's own
  SaveConfigValues path (same as show_fps_counter).
The hot side needs nothing new: the PASS 38 change-callback
(skate3::coord_dump::OnCreateDialogs) already creates/resets the HUD
dialog on the cvar change, so the menu toggle is live in both
directions.

## Verification (037 run)
Default OFF proven in-sim (no "HUD on" line, no coord_dump dir, no
coord-dump log lines); the persisted key written to the user settings
toml at boot by EnsureSimpleSettingsConfig; ninja link clean; row
strings + cvar symbol present in the linked artifacts (strings/nm).
NOT verifiable headlessly: the row's visual render + click dispatch
(no input injection available) — the chain is a byte-for-byte mirror
of the FPS Counter row and the cvar end was live-proven in 036/037;
user steps in analysis/xex/STATUS_PASS39.txt. No game-tree file
touched.

# PASS 40 (2026-10-01) — OTS candidate capture (F11) + district disambiguation

## The user's coordinate-space question (per the data)
ONE shared coordinate space city-wide; the districts' bounding ranges
overlap (the three districts' rendered-vocabulary boxes pairwise overlap
by hundreds of meters — see analysis/big/ots_all_geometry.md), so the
capture carries the district
field separately (null while unknown). Legs: (a) the volume-extraction
driver over ots_{dwtn,indu,univ}; (b) the freeskate region Center layout =
one contiguous 2D grid across all districts. Honest limit: the single-map
verdict rests on the region layout + the game's own cross-district
freeroam; no cross-loader co-location proof was generated.

## The implementation (all reusing verified reuses)
- F11 bind → skate3::coord_dump::CaptureOtsCandidate()
  (src/skate3_loc_dump.cpp): last-known skater sample (never expires,
  "pos_stale" flagged), camera (2 s freshness), district
  (rex::filesystem::dist_track, NEW: virtual_file_system.cpp OpenFile
  records the last chunk path's DIST_<Name> token — an unconditional,
  I/O-free substring scan), and the nearest anchored item via the NEW
  LastFrameScene() snapshot accessor (skate3_native_scene.cpp) filtered
  to plausible static anchors (skater family / lw instances / hair /
  transformless items excluded).
- JSON append: fresh ofstream per press (open/app/flush/close),
  <app root>/OTS_candidates.json; schema in
  analysis/xex/findings.md PASS 40 (name=null until a game-data
  name is resolved — nothing fabricated).
- ESC menu additions (PASS 39 pattern): "OTS Candidate Capture" row +
  persistence + load + change-check; the existing "Show player
  coordinates" already covers (A).
- Touched-obstacle physics contact: NOT solved (the census index is
  still missing); the full recipe (re-extract done this pass → rebuild
  the disasm index → xref 'mIsInContact=%d'/'TrucksOrDeckInContact' →
  contact record → collidable pointer → name) is in findings.md PASS 40.

## Verification
ninja link clean; 038 session boot = no crash, no OTS file on boot
(hotkey inert), zero coord activity; symbols present (nm); DIST-tracker
and anchor-probe behavior = in-sim only, user-verified next run. NO
game-tree file touched.

# PASS 41 (2026-10-01) — capture arming decoupled from the HUD

The sample-stamping taps (OnSkaterEntity / OnViewMatrix,
skate3_loc_dump.cpp) gated on show_coords || coord_log, so
skate3_coord_capture=true alone left every F11 capture with pos=null
(the user's report: captures at the same obstacle, one with the HUD on,
one without — the without-HUD ones null). Fixed with the user's
PREFERRED route (no fallback needed): a single AnyConsumerArmed()
(show_coords || coord_log || coord_capture) now drives both taps and
the capture's own arm check; only the HUD drawing stays behind
show_coords. The staleness contract unchanged: null only when no
sample was ever stamped; >2 s samples report with pos_stale=true.
Same-file cleanup: the coord-log thread's path announcement moved to
the first actual write (capture-only boots now print zero coord-dump
lines). Verified: ninja link clean; capture-only boot smoke (the
user's own settings.toml state, untouched) = zero coord activity, no
output files; the F11 press itself = user retest. NO game-tree file
touched.
