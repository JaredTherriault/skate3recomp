
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

# PASS 42 (2026-10-01) — decode notes: the loc-DB record layout + the accumulation

* loc-DB (LANGUAGE_*_Global_skate3ng.BIN, decoded): header = [u32 @0]
  [u32 inner = filesize−8 @4][u32 count @8][u32 stride 0x1C @0xC]
  [u32 pool_off @0x10][8-byte NUL-padded module name @0x14 ("Global")].
  Record table = [0x1C, 0x1C+8·count): (key u32 LE, off u32 LE), keys
  ascending; pool = 8+pool_off; string(k) = pool+off (NUL-terminated).
  One 8-byte bridge pair — (0xffd7e190, 0) in the English Global — sits
  between the table end and the pool (preserved verbatim in rebuilds).
  Cross-checked against the runtime-registered loc-DB images
  (build/loc_dump/, the pass-27 captures) — the same layout parses all.
* Pass-30's Morshu row had off 0x1A338 (= "Rippon Ribbon Ripper"); the
  correct value 0x67128 = string_abs − pool. Repaired in the pass-42
  miscboot_custom.big; all 6 custom rows (Morshu + Snoop/Dre/Eazy-E/
  Cube/Warren at FA16672B/4710A3EC/940AE0AD/E1051D6E/187ACF86) now
  resolve by the official rule.
* challengebanks/main.vlt (decoded, 1,124,168 B): DatN [96, 562,480) with
  the Morshu state block = the LAST 208 B; ExpN = [u32][count u32] +
  24-B rows [id u64][type u64][bytes u32][datoff u32]; PtrN = 40-B prefix
  + 16-B rows [0][bin_off][vlt_off][0x00030001] (BIG-endian). The
  pass-42 accumulation appends 5 blocks (state keys 0x0F15C0DE5A5A0016..1A,
  cloned from export 191 = univ_11's, title slot rel 184), 5 ExpN rows
  (3,514 total), 5 PtrN rows, and 5 TITLE strings at the main.bin end
  (0x1b5e5b.., 31 B each). Chunk walk + export re-parse = green.
* The five mission cSim streams (ots_univ_16..20): cloned from the
  Morshu retarged dec, node hashes rebanded 00221d10..d54 (BE/LE u64 +
  ASCII hex forms), per-site DELTA (rails collective center x/z → site,
  rails ymin 173.47 → site_y+0.5), re-encoded with the literal-only
  RefPack framing (round-trip STOP), raw-stored (csize=0) with the
  original SFIL 0x150 header. Boundary boxes verified at the captured
  sites by decode re-parse (verify_ots42.py).

# PASS 43 (2026-10-01) — the vaultlist/challenge chain instrumented; verdict deferred to the next run

## The TU3 disasm index rebuilt (third time, same recipe)
/tmp/opencode/xexre/tu3_disasm.sqlite = 3,048,182 insns (capstone PPC-BE,
.text 0x82380000-0x82F9E16C, from the pass-40 image dump). Lower count
than the pass-27 index (3,945,024) = invalid-word skips + the signed
lis rendering; all sites used below verified present.

## The DLCDirectorySearch chain (string hi/lo xref-verified)
sub_826DAB98 (search driver: mgr+0x1450 handle, mgr+0x1458 functor
bctrl with the 'cVaultManager::DLCDirectorySearch' tag, wait state on
the global object, per-file call to sub_828D9378) → sub_828D9378
(per-file: path at handle+8, '/'->'\' normalize, cVaultListLoadCallback
object [vtable 0x82316304], request build 0x828e3458, enqueue
0x8247f9e8, FindNextFile 0x8298f430, FindClose 0x8298fff8);
sub_828D9510 = the cVaultBinLoadCallback side. '%s\%s' combiner at
0x82174c10 (no direct xref — r31-relative).

## The decisive negative
The base db.big has NO vaultlist.xml and NO challengebanks entries;
the packs' vaultlists live inside the pack .bigs. The dlcbig:→Partition1
mapping (wrapper) points the glob at a directory that does not exist in
the served layout — either the search finds nothing host-side (and the
vaultlist gate never runs for ANY pack, matching the total line silence
in every log), or the guest services the search from its own big-device
layer before the host sees it (also matching the silence). The pass-43
trace decides.

## The instrumentation (src/skate3_loc_dump.cpp, cvar skate3_loc_dump)
sub_826DAB98 SEARCH-DRIVER, sub_828D9378 VAULTLIST-FOUND (dumps the
found filename — the verdict datum), sub_828D9510 VAULTBIN-CB. All
read-only + __imp__ passthrough; all three verified codegen entries.
Session 047 smoke: links + boots clean; VAULTBIN-CB hot at menu
(~120/s); the search hooks silent at menu stage (game-load gated).

## Honest state
No verdict shape (a)/(b)/(c) proven yet; the mKeys/mParent suspicion
remains open. Next user run (to the in-game map/menu) populates
VAULTLIST-FOUND and closes the verdict.

# PASS 43b (2026-10-01) — the find-context layout corrected from the string-helper disasm

Run-048's VAULTLIST-FOUND dumps (path='' x4, unread x12) came from
reading handle+8 — wrong offset. The TU3 disasm of the guest's string
helper 0x82a884d0 (r3 = handle+8, r4 = caller r1+0x50) shows it
concatenates a WIN32_FIND_DATA-like record: dir at (handle+8)+0xc =
handle+0x14, CURRENT MATCH at (handle+8)+0x138 = handle+0x140, output
= dir+name at r1+0x50. The found filename = handle+0x140.

The hooks now dump (all SEH-guarded, cvar skate3_loc_dump):
- sub_828D9378 VAULTLIST-FOUND: name @+0x140, dir @+0x14, plus hex
  fallbacks (6 words @handle, 8 words @handle+0x130) — the next run
  prints the 16 names (or bytes) even if a layout detail is off;
- sub_826DAB98 SEARCH-DRIVER: mgr+0x1450..0x145c hex (the full glob
  spec the single fire carries);
- sub_8247F9E8 VAULTLOAD-ATTACH (NEW): the load-callback string-attach
  setter (obj{ptr,len} <- r4) — fires per queued vault load with the
  full path; LIVE-VERIFIED at menu stage (printed a real CAS-texture
  attach) — in-game its sequence brackets the vaultlist loads;
- 0x828e3458 checked: a bounded path trimmer, NOT the request builder —
  dropped from the plan.
Build links clean (one illegal-declaration fix: a declaration inside a
parenthesized &&-condition); boot smokes show no regression. No game-
tree file touched. Verdict still open pending the names.

# PASS 44 (2026-10-02) — decode notes: the two coordinate spaces are ONE

* The mission cSim streams (RW4xb2, decoded via the pass-28/29 pipeline) use
  the SAME world coordinates as the runtime skater F11 captures — verified
  by 4 loc-DB-proven correspondences (The Wall=univ_05, Stadium Bank=univ_02,
  Library Exit=univ_04, Drainage Ditch=univ_03): each capture sits INSIDE its
  mission's spot/scoring volumes at 1:1 values, and the inter-anchor
  distances match the cSim locator geometry to ≤2.5 m over 250–614 m
  baselines. Kabsch 7-DoF returns scale 0.998 / yaw −0.38° / t≈5 m =
  standing-offset noise; the 48-permutation scan = identity wins. The
  earlier axis-swap hypothesis and the "not 1:1" premise (from misbinding
  'The Wall' to univ_14, whose real name = 'The Ditch') are both overturned.
* Morshu's ots_univ_15 cSim entry (missions.big idx 5321, raw-stored,
  0x150 SFIL header + literal-only RefPack frame) retargeted by DELTA
  (317.07, −108.05, 814.615): 5 volume records (rel rows 0x78/0x68/0x58/
  0x48/0x38/0x28 per node hash 2c70170600221d00..04) + 16 locator transforms
  (marker 0x7f8 records + their bbox rows). Node hashes/names/stamps
  unchanged — the db bank's scoring-node hash refs remain valid.
* missions.big EB note: a mission cSim entry's size field = the FULL payload
  length (0x150 header + RefPack frame), NOT the decoded usize — writing the
  dec length truncates the read and the RefPack decode ends 'eof' (caught by
  the == saved-dec oracle).
* The master loc map (base_loc_map_en.txt) = regenerable from the game-tree
  miscboot.big English Global with the pass-42 layout recipe; the official
  OTS titles for univ_01..05 = single-digit ID strings
  (ID_CHALLENGE_OTS_UNIV_1..5_TITLE), univ_10..14 = two-digit.

# PASS 45 (2026-10-01) — decode notes: the DLC pack internal formats (fresh exemplars)

* The pack inner .big = the db-variant EB WITH the ctype column: hdr ver 3 /
  flags 0x50 / align 6; names_off = align16(0x30 + N·16 + N); name slots =
  [u16 BE folderIndex][NUL-padded name]; folder slots = LEFT-aligned full
  paths in order; hash1 = djb2(folder_path + '/' + name); ALL pack entries
  = raw (csize=0) — the pack tree mounts without any RefPack layer at the
  EB level (the .xsf streamfiles carry their own RefPack frame inside).
* Official vaultlist.xml frame (dannyway, 284 B): '<?xml version="1.0"
  encoding="utf-8"?>\r\n<VaultList>\r\n  <VaultFile>NAME.vlt</VaultFile>\r\n
  … '</VaultList>' — subfolder entries use BACKSLASHES
  (challengebanks\dlc_dwgh.vlt); the loader pairs each listed .vlt with its
  <stem>.bin by convention (the .bin is never listed).
* Pack vault file families (all RAW Vers/StrE/EndC): dlc_<pack>.vlt = the
  LOCATION-MODULE bank (N location-definition exports; the .bin = StrE
  strings: .ems filters, ID_LOCATION_* keys, model paths, ID_MAP_HELPER_*,
  ID_WORLD_*); <pack>_local_data_framework.vlt = the pack class-defaults
  hierarchy; challengebanks/dlc_<pack>.vlt = the pack challenge list bank;
  challenge_local_data/<challenge>.vlt (1,312 B class = 46a56addf284c381
  group with 45 default entries) = the per-challenge bank shape our six
  pairs already follow.
  + dlc_<pack>.opt = 16 B 'EndC' + zeros.
* The pack strings channel = data/fe/languages/english/LANGUAGE_<Lang>_
  <token>_skate3ng-dlc_<pack>.BIN — the SAME structured loc-DB format
  (header [00039000][inner][count][0x1C][pool_off][8B name]; sorted
  (key,off) LE records; pool = 8+field; dannyway = 190 keys, presell = 3).
  The 8-byte bridge pair slot sits between the record table end and the
  pool (presell EN: rows end 0x34, pool = 8+0x34 = 0x3C) — the same
  geometry as the English Global (pass 42/44).
* data/unlocks/<token>.unlock = plaintext 'ENTITLEMENTS,<tok>\r\n
  LOCATIONS,<loc>\r\nCHALLENGES,<challenge>\r\n…'; <token>0.ver =
  '667130:sk8build' (the build stamp).
* Our pack (pass 45): morshu_00000000.big (89,472 B, 100 raw entries,
  6-deep folder table incl. data/content/missions/ots_univ_15..20) inside
  the pass-31 STFS wrapper (project10 header template, LIVE, read-only,
  base 0xB000, single directory entry, L0 hash chain) = morshus_paradise
  (143,360 B) — the channel the run-053 vaultlist glob consumes.

# PASS 46 (2026-10-01) — 0x828AAE90 re-identified as the pool allocator; the functor-method runtime capture added

The pass-46 headless smokes (055/056) re-identified the pass-46
FIND-CREATE hook target 0x828AAE90: GENERAL POOL ALLOCATOR (size,
pool_key, tag; 1965 callers; menu-stage fires (0x8d20, 0x11, ''),
(0x78, 0x11, ''), (0x14, 0x40, '')...). The 'cVaultManager::
DLCDirectorySearch' string = an ALLOC TAG (explaining the find record's
pool-header garbage in run-054/053 dumps).

The candidate enumeration source = the functor at mgr+0x1458
(registered by sub_828D8B58 <- single caller sub_826D65A4); its search
method (vtable slot +8) unknown statically (the subsystem's .rdata
name strings have zero refs in this build). The pass-46 SEARCH-DRIVER
hook now READS the functor/vtable/method8 VAs at runtime and prints
them — the next user run yields the pass-47 hook target.

The wrapper has NO big-file device (only disc_image/host_path/null/
stfs_container) — the dlcbig enumeration is guest-side over mounted
big virtual dirs; the likely failure link for dlc_morshu = the inner
big's directory view (folder-index/name-slot conventions vs the dway
exemplar) or the functor's runtime device registry. Verdict open.

# PASS 47a (2026-10-01) — splash auto-skip + the codegen-patch inertness found and fixed

The startup logos = FE movies through FEMoviePlayer::Update
(sub_825E0510); its codegen patch already integrates
skate3::demo_path::ShouldForceIntroMovieComplete() per movie tick
(force = ctx.r3 = 0 = complete → the FE flow advances). PASS 47a adds
cvar skate3_intro_movie_autoskip (default false): the force returns
true unconditionally → every FE movie (the startup Skate/BlackBox
splash) auto-completes with no input. Language-select/save prompts/
press-start = separate FE states, never movie-routed — untouched.

INCIDENTAL (important): the current generated tree had ZERO of the
ApplySkate3CodegenPatches.cmake patches (frustum/FOV/movie) — the patch
script had not re-executed after the last codegen; running it directly
applied all three (generated/skate3_recomp.39.cpp frustum+FOV; 20
movie+include). The user's FOV cvar and the ultrawide frustum patch are
restored for the first time since the Sep 25 codegen run, and the
press-to-skip actually works now.

Verification: links clean; negative control (cvar off = zero
demo-path lines); positive (cvar on = 'intro movie auto-skip active'
fired during the boot = the force path exercised); no crash; test toml
state restored; game tree untouched.

# PASS 50 (2026-10-01) — pack-big structural diff: our fnLen/folLen strides are NOT 4-aligned (dway exemplar: they are)

bigdir_diff.py + stfs re-extraction of both inner bigs:
- morshu_00000000.big (from build/dlc/morshus_paradise): 100 entries,
  fnLen=49 (odd), folLen=35 (odd), namesSize=0x14A5 (%4=1), folders
  11, data/db/dlc_morshu.vaultlist.xml PRESENT (445 B '<?xml version'),
  all six missions included — the artifact content is right, the
  STRIDES are not exemplar-shaped.
- dway_park_00000000.big (from the dannyway LIVE package): 1057
  entries, fnLen=124, folLen=56 (both 4-aligned), namesSize 0x211C0
  (%4=0), folder stream LEADS with 4 NUL bytes before the first path.
The divergence = a reader consuming the slot table with a 4-byte
stride (or aligned u16 casts) misreads everything past our slot 0
(the u16 folderIndex lands on padding), so the pack-loc vaultlist
enumeration view of our pack = collapsed → the 'dlcbig:data\db\'
search never yields dlc_morshu. FIX CANDIDATE (pass-50b, deployment
incl. the stale-install delete): pad fn_len/fol_len to 4 (and the
folder stream lead) in build_ots45_pack.py, rebuild the pack, rewrap
STFS, redeploy, delete the stale install.

Also this pass: filesystem_debug_log_query_directory (host-side,
hot cvar) logs every kernel-level guest directory enumeration —
distinguishes 'guest-side big-device walk' from 'kernel-backed walk'
on the next run. Zero lines during game-load with the cvar on =
guest-side only = the strided-table fix above is THE fix candidate.

# PASS 51 (2026-10-01) — decode note: the pack .big directory-table conventions (final, raw-verified)

* Name slots = [u16 BE folderIndex][NUL-padded name], stride = align4
  (maxNameLen + 2) — dway 124.
* Folder slots = [u32 0][path][NUL pad], stride = align4(maxPathLen + 4) —
  dway 56, ours 40; every slot LED by 4 zero bytes (the '00000000' before
  'data/unlocks' at both stream heads).
* namesSize ≡ 0 (mod 4); namesOff = align16(TOC end + ctype); payload
  start = align64(namesOff + namesSize).
* The mystery around dway's namesSize (0x211c0 = 135,616 vs the computed
  135,604) = 12 spare bytes inside the region (a sentinel/pad row after the
  81 folder slots — harmless; we ship an exact N·fnLen + F·foLen size,
  %4==0.
* Everything else about the pack EB = unchanged (pass 45): ver 3, flags
  0x50, align 6, the ctype column after the TOC, hash1 = djb2(full path),
  raw csize=0 entries.

# PASS 52 (2026-10-01) — guest-side enumeration proven live; the read-path instrument staged

Session 063 (headless, both instruments on):
- QueryDirectory patterns = scene_*/ice_*/cas_db*.recipe only; no data\db.
- BIGREAD = base-disc bigs + custom overrides only; ZERO pack bigs read
  during boot.
=> The dlcbig:data\db\ vaultlist search is GUEST-SIDE over the mounted
pack bigs' virtual dirs; the wrapper's VFS never sees it. The open
question narrows to WHERE the guest's own big-tree build drops us (with
the stride fix already deployed): the next run's BIGREAD traces for
morshu_00000000.big decide (name-table reads present = parse stage;
absent = mount-registry exclusion).
The 12 empty VAULTLIST-FOUND records = end-of-walk sentinels, not slots
(static name tables ruled out: 'dlc_play'/'dlc_maloof'/etc. are ABSENT
from the TU3 image — the found names can only come from runtime pack
data).

# PASS 53 (2026-10-01) — the walk functor captured at runtime; the 0x8298xxxx find cluster statically seeded

The pass-53 identity hook (armed by default with skate3_loc_dump) now
prints functor=0x830E618C vtable=0x823178DC method8=0x8298E4E8 on the
live VAULTLIST-FOUND fires — the functor is a real static object whose
vtable+method only exist at runtime (run-060's empty hexdump = a boot
phase effect, NOT an unmapped window). Statically (in-slab branches):
method8=sub_8298E4E8 (a thunk over the functor fields + search flags,
tail into sub_8298E020) → sub_8298E020 (per-DEVICE-slot iterator over a
0x6C-stride registry: [pool+0xC1B4] + arg2*0x6C → entry → sub-object →
vtable+8 = sub_8298E2E8) → sub_8298E2E8 (the per-candidate primitive)
with the record strings built at the 0x82AE8798 string-merge. The found
farness (4 named + 12 sentinels) = per-slot outputs in this registry:
the closure for OUR pack = our submodule slot AND its [entry]
sub-object in that registry. That is the pass-54 instrument point (the
per-slot register log at every fire: r4 arg2/r5 arg3) and the concrete
next-run data; the pack-loc vault/module registry candidate (pass-34's
5 official module names) remains the strongest exclusion hypothesis.

# PASS 54 (2026-10-01) — per-slot walk hook live; the 0x6C-registry population site found

Statically closed in this pass (from the tu3_disasm slab):
- the 0x6C-stride table = [pool + 0xC1B4]-indexed device slots, selected
  by the walk's r4 index (mulli 0x6C), with slot words [0]=self,
  [4]=sub-object A, [8]=shared 0x830DB270, [C]=0x823178DC = THE WALK
  VTABLE, [10]=0x823178FC secondary, [14]=slot index, [18]=0xFFFFFFFF;
- the slot REGISTRATION site = sub_8298DE78 (alloc via 0x82a40368,
  parent-link via 0x82476088, then stw at slot+0x30/+0x34/+0x38 — its
  writer's r11 = [base+0xc1b4] + idx*0x6C) called from the functor's
  registry path (sub_8298D938 -> several provider calls);
- the walk per-slot iterator = sub_8298E020 (the walk method8's
  tail-branch target), and the per-record primitive = sub_8298E2E8
  (string-merge at 0x82AE8798).
The pass-54 WALK-SLOT hook (live in session 066) prints the per-slot
universe on 120+ fires: idx 11/13/17/45/63/64 with the slot head words
readable on the later fires (earlier fires read pre-init zero — same
story as the pass-53 functor reads; later = reliable).
The next run's per-idx tally + the pass's sub-object shapes decide the
exact exclusion point (our slot present-but-broken vs never registered).

# PASS 55 (2026-10-01) — the full-boot headless capture: the walk registry is a ~28-slot dynamic universe (not a 5-slot gate)

Session 067 (180 s, both filesystem instruments + WALK-SLOT live):
- WALK-SLOT fired 175 times; the registry = 28 DISTINCT slots
  (idx 0..76 pattern: 0/2/8/11/13/14/17/19/29/30/32/35/37/41/43/45/46/
  52/54/55/58/63/64/66/71/73/76; hot band 29/37/45/64 re-seeks all boot
  long). NOT a fixed 5-slot official manifest.
- The VAULTLIST-FOUND burst never triggered this run (0 lines) and no
  pack-big BIGREAD lines — the walk window did not start headlessly
  this time (the 065 smoke DID burst: boot-flow variance documented;
  the pass-53 identity + found-list data remain the established
  evidence line).
- The registry population site (pass-54: sub_8298DE78's slot writer)
  runs PER find-context creation, so one instrument round there =
  one log line per registry slot with the mount correlation — the
  pass-56 work; the pass-53 identity stays canonical for walk
  identity.

# PASS 56 (2026-10-01) — REG-POP live; the find-context registry's shape decoded at its population site

sub_8298DE78 (the population site; single caller via the pass-54
sub_8298D938 family) constructed 64 slots back-to-back in one
boot burst (the rec pointer family marched 0x1C apart) with empty
records AT CONSTRUCTION TIME (the caller passes an empty-slot record;
the callee fills it from the walk later — the pre-call probe's all-zero
reads = construct-time state, not read failures).
POST-call ctx.r3 = the callee's RETURN = the pool SELF (mr r3, r28 at
its epilogue), NOT a created context — recording that so future
instrumentation reads the right field.
The registry = a generic per-manager 64-slot find-context pool
(pool 0x830d9a40 in this boot's constant; heap self objects in the
0x70.. region). The per-slot pack attribution remains the open item:
either the mount build never creates our find-context (the pass-34
install/loc-load registry exclusion leading theory stands) or it does
and the walk's per-slot filtering drops it. Pass-57 rubric in
STATUS_PASS56.txt.

# PASS 59 (2026-10-02) — the found-record read path re-verified at raw words; the run-070..074 empty names = the probe code, not the guest

## Raw-word re-verification (tu3_disasm.sqlite, 3,259,473 insns)
Caller sub_828D9378 prologue:
  lwz r31, 0x1450(r3)      ; r31 = handle (mgr+0x1450, the find record)
  mr  r27, r3              ; mgr
  cmplwi cr6, r31, 0 / beq → skip everything
  lbz r11, 8(r31)          ; record MODE byte @handle+8
  cmplwi cr6, r11, 1 / bne → 0x828d94d8
  addi r3, r31, 8          ; r3 = handle+8 (the WIN32_FIND_DATA-like base)
  addi r4, r1, 0x50        ; out buffer
  bl 0x82a884d0            ; the dir+name string-helper
  ... copy r1+0x50 with '/'-fold (0x2f→0x5c) into r1+0x150 ...
Then the functor vtable call (functor = lwz 0x1458(r27), [vtable]
dispatch at [vtable+0xC] via ctr) per found record, and
stw r9=0, 0x1450(r27) clears the record pointer (FindNext).

Helper 0x82a884d0 (called with r3 = handle+8):
  lbz r11, 0(r3) ... beq → li r3,0
  addi r11, r9, 0xc        ; DIR  string   @(handle+8)+0xC = handle+0x14
  stb-loop copies it to r4
  addi r10, r9, 0x138      ; NAME string   @(handle+8)+0x138 = handle+0x140
  lbz-loop APPENDS it after the dir at r4
  => out(r4) = dir + name concatenation; matches pass-43b exactly.

The !=1 path at 0x828d94d8:
  addi r3, r31, 8; bl 0x82a88058   ; = the {ptr,len} string-object DTOR
   (0x82a88058 reads field @+8 (len/flag?), if ptr @+4 non-null calls
    [vtable+0x4C] free, then nulls +8/+4/+0)
  then functor bctrl with r4 = r31 (the RAW handle) and clear mgr+0x1450.

## The instrumentation diagnosis (this pass's evidence)
Current hook body (pre-fix): `dir, name, hex_head, hex_tail` declared at
line 438 and printed at 484-486, with NO code that fills them — the
only reads were mgr+0x1450 into a local `handle` that was never
dereferenced, plus a DEAD DUPLICATE functor/vtable/method8 read block
(shadowed, results discarded). So runs 070-074 printed the format
string with empty fields by construction; the guest record was never
sampled. Run-060 (pass-50 STATUS quoting its own log) DID print the
official names from this same hook family — the reads existed in that
binary and the records were readable then. The reads were lost in the
pass-53..58 hook rewrites (STATUS_PASS53 notes it removed 'the earlier
duplicate capture block' — the casualty was the record reads, and the
pass-56/58 rewrites rebuilt around the print without restoring them).

## Restored probe (src/skate3_loc_dump.cpp, PASS 59)
sub_828D9378 hook: read handle (SEH-guarded); when handle != 0:
  dir  = string @handle+0x14   (buf 128)
  name = string @handle+0x140  (buf 128)
  head = 6 BE words @handle    (HexDumpWords, '...' on read failure)
  tail = 8 BE words @handle+0x130 (when name non-empty)
non-empty NAME promoted to INFO ('Skate 3 pass59: VAULTLIST-FOUND #N
dir= name= tail=') so the decisive record is visible at any log level;
per-fire chatter (identity + empty-name debug line) stays DEBUG.
Dead duplicate functor block removed (the printed one stays).

## Open decode item (documented, not decoded this pass)
What the ==1 mode byte means and why the !=1 path treats handle+8 as a
C++ string object (dtor shape) while the ==1 path treats it as a flat
find-data struct: both readings coexist in the guest; the restored
probe reads the flat-struct fields regardless of mode and the head hex
names the record when a layout detail shifts between modes.

# PASS 61 (2026-10-02) — the EB index-FAT region: FINAL decode (exemplar-roundtrip-proven) + the fatSize=0 pack fix

## Decoder fact sheet (supersedes the pass-60 "unpinned" notes)
The FAT region following the names block = [u32 nodesLen][rows][u32 tblLen]
[tbl u32s][u32 stringsLen][strings][zero pad], ALL BIG-ENDIAN. Row stride
5 u32 (0x14). Node id = ROW INDEX (0-based; row 0 = the ROOT sentinel — the
pass-60 "row+1" line was an off-by-one; every file parent ref and the
language-dir id run 7/10/13/.../31 validate against the row index map).

- File row = [keyOff][1][parent][fileOrdinal][0xFFFFFFFF]. The ordinal =
  the EMITTER's visit order (NOT the TOC order: dwaypark.unlock = TOC[1045]
  with ordinal 0).
- Dir row = [keyOff][0][parent][childCount][childListOffset]. The offset =
  the dir's child list inside the tbl array; the running sum of child counts
  over all previously created dirs.
- Emission (walk) order = the FOLDER TABLE slot order; a dir row emits when
  its path segment first appears; the folder's files emit immediately after
  its path walk, sorted by strcmp(name) ascending.
- String offsets: first emission appends "name\0" to the strings blob;
  repeats REUSE the first occurrence's offset (dway: 'stream' x2 both key
  29; 'static' x3 both key 3029; 'danny_way_vert.abin' x2 both key 416).
- tbl = the ROOT's children in reverse row order, then every other dir's
  children in reverse row order, dirs in creation order.
- fatSize = align16(structural total); the zero pad sits INSIDE the region
  (pads observed: presell 0, dway 9, play 3).
- The folder-table region = zero-initialized buffer; each slot's path is
  memcpy'd UNBOUNDED at slot_base+4 (the original builder's paths overflow
  1-4 bytes into the next slot's prefix area: presell slots 2-10 = the
  spill fragments, dway slots 56/57 = '...MegaComp' + 'und').  The TRUE
  folder list = the NUL-split of the whole folder-table region in order.
  (Our builder emits clean padded slots; conforming to the exemplar parser.)

## The 0x82A829B0-family reader, cross-checked
The dir-row reader pair stays unambiguous under this model: count=f1,
list=f1 range [Y, Y+count) into tbl (big-endian descending). dway's
'DLC_DW_MegaCompund' (108 children, Y=64) and 'missions' (48, Y=221) fit
the recursion exactly; tu3_disasm.sqlite's dir-row reader not needed —
the pair was fully pinned by the presell+dway two-big comparison.

## Pack fix shipped
build_ots45_pack.py: fatSize>0 for morshu_00000000.big (1 root + 15 dirs +
100 entries; the missions/misc files per exemplar layout), header
asize/fatSize updated, payload pointers re-strided; verify_ots45_pack.py
extended with the FAT roundtrip + index-region checks. Roundtrip gate:
presell and dway (and dway-faithful dwayprk/dw variants) regenerate
byte-identical.

# PASS 69 (2026-10-04) — THE WALKER PAIR-TABLE FULL DECODE (init/append/attach/remove) + the key-independent gate-pin + the cvar-gated wrapper shim

## The PAIR-TABLE GATE (all word addresses from tu3_disasm.sqlite live-disasm re-derivation, full-binary scan)
- **count word [0x82FDE294]** (lis 0x82FE0000 + lwz/stw -0x1D6C): s32, -1 = empty
  (INIT), per-APPEND = max_key+1 (sub_8290C548's count+1 write). All 14 binary
  accesses pin to it; **0x82FE9294 = ZERO accesses** (the pass-66/67 sheet's
  "0x82FE9294" = NO SUCH ACCESS).
- **pairs word [0x830684F4]** (lis 0x83070000 + lwz/stw -0x7B0C): pointer to the
  PAIR rows, **stride 8 {key s32, val s32}** — INIT fills all 100 as
  {key=-1, val=0}; find (sub_8290C2A8/sub_8290C3F0), append (sub_8290C548's
  key+idx write), removal (sub_8290C6C8: find/shift/count--/*last={-1,0}*) and the
  full clear (sub_8290C7D0: ALL rows {-1,0}) ALL stride 8. **The pass-67 comment's
  "0x830684E4 / stride-0x10" line NO SUCH ACCESS; the dump's stride-0x10 reads
  = misinterpretations.** The "{stride-0x10 row-obj {count, ptr 0x830F092C@4,
  fn 0x82E50D30@8, 0@0xC}}" pair-table shape = NOT THE WALKER PAIR TABLE — it
  computed stride-0x10 offsets INTO the stride-8 pair array (a jump-table
  corpus). No stride-0x10 pair stride exists in the walkers.
- **gate word [0x8306DCA4]** (lis 0x83070000 + lwz/stw -0x235C): pointer to the
  ENTRY rows, stride 0x10 {word0, word1, word2 key, word3} (INIT = 100 zero
  slots, 0x640 B).
- **base-id word [0x83084110]** (lis 0x83080000 + lwz/stw +0x4110): INIT value 2
  (the officials' observed ENTRY word1 — not a pointer).

## The chain (per module/registry slot)
- **INIT** sub_8290BEB8 (self call from 0x82D08090 ONLY — the block's first
  sub_8290BEB8 call = the INIT itself): m3(r3 = registry slot+0xC, r4 = 0x640,
  r5 = 0x821812A8(IID-words), r6 = 0) = alloc 100×0x10 → **the ENTRY array**;
  m3(r3 = slot+0xC, r4 = 0x320, r5 = 0x821812B4) = alloc 100×8 → **the PAIR
  array**; store pair base to [0x830684F4]; store ENTRY base to [0x8306DCA4];
  zero all (pairs {key=-1, val=0}); count [0x82FDE294] = **-1**; base-id
  [0x83084110] = **2**; sub_8290D828(rec, "en") + sub_8290D8D8 = the global
  ("en"-prefix) registrations (r31 = str length; sub_8290BDF8(rec, len,
  r5 = {val 1, str}) — the name-key build, val 1 = a BOOL (the "en" case-bool,
  not DEP-N).
- **PAIRFIND-APPEND** sub_8290C548: gate [0x8306DCA4] nonzero + count+1 = 0x64
  (bail ret −1); r6 = pair base; max over [count+1 pair keys] → r30 = max+1 =
  **the new key**; ENTRY free slot = the first with **word1 == 0**, val = the
  free-slot idx; order-reverse/copy-link per r3's zero flag; then the dispatch:
  m3(r3 = registry slot+0xC, **r4 = key*0x1C**, r5-vtable+8, r6 = 0) — the
  key-strided loc-DB setup → ENTRY.word2 (at +8) = the m3 result.
- **SWAP REGISTER/ATTACH** sub_8290C2A8 (the walker pairs;arg shape r3 = the
  record pointer, r4 = the key): signed find(rec_key, key) over the stride-8
  pairs; byte-swap rec words 0/1 (the record's in-memory form is LE-swapped in
  the guest image = the retail 0x828E4A88 attach fn); then
  **sub_8290D638(rec, pair_val) = the dispatcher**: rec[0] (BE) == 0x00039000
  (the loc-DB image magic) → the DEP copy into the val-keyed [8+i*0x1C] loc-DB
  row: DEP row's word @+0x10 = pair_val (the row key), word @+0x18 = rec[0]
  (the .BIG pool pointer), word @+0x14 = rec (the loc-DB image pointer),
  row word @+8..+0x17 bytes = rec[+0xC +0..0xF] (the DEP blob pack), DEP word0
  (+0) = its used-count++ (append at the first used == count slot). Other ret
  0 (image magic mismatch = the 0x82AB8C40 EAString compare, NOT a depper).
- **UNREGISTER/REMOVE** sub_8290C6C8(rec_key): find over the pairs (signed);
  sub_8290D5B0(rec, pair_val) = the release; the pair-table shift =
  per-pair **[row+4] = [next_row+4]**, **[next_row_key] <- [row+2_key]**
  (stride-4-pair move: the pair's val moves first, then its neighbor's key);
  count-- ; final row = **{key -1, val 0}** (the killed pair NO LONGER EXISTS
  in the count-stale zone).
- **DRAIN/CLEAR** sub_8290C7D0: ALL count-stale pairs = {-1, 0}.
- **The walk runner** 0x82D0833C: sub_82D08ED0 (drain) + sub_8290C7D0 (clear) +
  the module INIT sub_8290C7D0 == sub_82D08ED0's clear phase.

## THE WALKER-SIDE GATE-PIN (the pass-68A exclusion, statically resolved)
- sub_8290C2A8's rec = **the record the WALKER passes** (a .BIG image pointer
  or loc-DB buffer), NOT the [0x830CFEEC]+8 vault-table rows. Our pack's .vlt
  byte-conformance (pass-65/68) = NOT REQUIRED for the registration+ATTACH —
  the FULL pair-table gate-pinning EXCLUDES the word-address and
  walker-side-registration causes: the word addresses ALL match the actual
  walkem CODE, and the walkers DO carry our rec's arg.
- CALL-SITE arg-shape: sub_82D083D8+0x384-0x388 (0x82D08424/0x82D089B0 PAIRFIND;
  0x82D08544 C3F0 / 0x82D0854C C2A8 via the r31/r10 0x82C5B40+ region) and
  0x82D086C7 (the walker drain): the pairs row pointer to the caller's
  queue, NOT a per-.vlt-DEP-thing; the queue = the caller-side drain
  (the caller's [result+4] = the next-used slot).
- **THE LITERAL HARNESS**: 0x82D08544: slwi r11, r24, 2 — r24 = the
  found-per-lang index into [0x82C5B40]'s per-lang n64-literal-vector
  [][]; the record byte = **r5** = the literal at *(&n64[k]+v_idx*4),
  NOT the pack's data/db-relative r4/loc-DATA — i.e. the walkem-visible
  `rec` arg IS the per-lang image pointer literal.
- sub_8290C2A8 does NOT dereference rec's own .vlt/.bin name/hash — the
  registration key is handed by the caller dispatch (the key genre is not the
  DEP-DATA). So the pass-68A "walking official vaultlist rows carry the
  registration" theory = the DECODE'S SINGULAR CONTRADICTION: the registration
  is wrapped in CALLER-side keyed dispatch, not the walker's rec-scan.
- ARGUMENT-SHAPE pin (0x82D08340-0x82D08350, the block's first
  sub_82D083D8 caller): the module list walker passes
  **r3 = module-DESCRIPTOR (the [0x830CFEEC]+8 module ptr)**, **r4 = the
  0x82C5B40+ region's [rc4 = the per-lang literal vector base id slot]**,
  **r5 = the found record's @value+0xC** (the found-DEP pointer) — the caller
  paths drive the sub_82D083D8 dispatch layer.

## THE SHIM (implemented this pass, cvar-gated default OFF)
- src/skate3_loc_dump.cpp: `Skate3RegisterCustomPackShim` at the
  sub_8290C2A8 hook entry (cvar `skate3_register_custom_packs`, kHotReload)
  + the plain-C `sub_8290C2A8_guest_dispatch(rec, entry)` guest-side wrapper
  (private guest stack r1 = 0x80000000 via __imp__sub_8290D638).
- src/sub_8290C2A8_shim.cpp (the C++ shim binary; the guest-side reading via
  GuestTryCopy SEH-guarded everywhere, no std::string): the ENTRY walk
  ([0x8306DCA4] gate slots stride 0x10) → the officials' still-visible window
  (a WAIT state) vs the killed-tail key (the walk CLOSED = the shim window) →
  patch ENTRY.key = the tail key, DEP.key = ENTRY, DEP.$count = 1 → the
  plain-C guest dispatch (the real pair-table append path's second half —
  the ATTACH) → verify (re-read the count word + the attach-point state).
- THE GATE-PIN (static, zero runtime cost): the WRAPPER-side registration
  shim at the walker's caller-dispatch is THE (b)-shape fix (the walker's
  rec-scan registration cannot be reached — the caller keys it). The shim
  ignores the walkers' key contracts entirely: our pack's loc-DB entry
  registers via the SUB_8290C2A8 ENTRY-walk key, NOT the walkers' caller
  arg (the pass-65/68 byte-conformance shutdown NOT required for this lane).
- cvar: `skate3_register_custom_packs` default false = 0 effect (bail-once,
  no-op, harmless when off, no default-cvar pollution; the negative-control
  boot = the pass-63/68 session baseline). Hooked IF a walker-side
  registration exists (the shim detects the walker's arg and joins it,
  not the DEP data scan).
