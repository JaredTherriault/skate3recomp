
## PASS 26 — the runtime capture: the hook designed + implemented in the recomp source; the offline annotation tool = ready; statically validated
### 1. THE CAPTURE DESIGN (a non-invasive guest observation hook)
The site: **`sub_828E4A88`** (the loc-DB image register fn, the pass-23's raw-word verified chain:
`sub_828E4A88` = the bswap of [r3]/[r3+4] → `bl sub_828E5E18` @0x828E4B80). The hook:
* **Entry**: `r3` = the loaded loc-DB image (the decrypted `LANGUAGE_*.BIN` stream for the pack vault
  OR the per-language set — the two archives the pack-loc loader opens per pack, verified in PASS 23/25).
* **ABI**: `r3 = the guest buffer`, `r4 = the pack's loc handle`. The image size = `8 + usize`, read from
  the buffer's own header ([4..8] LE = the usize, `filesize-8`), so the dump's length = self-describing.
* **Pre-`__imp__` dump**: the hook dumps the guest buffer BEFORE calling `__imp__sub_828E4A88`, so the
  bytes stay in the on-disk little-endian order — which is what the offline loc-DB parser wants
  (the pass-21's layout correction: the FILE = LE, the memory = BE).
* **No state modification**: the hook = read-only on guest memory. It does not touch r3/r4, the pack
  handle, or the guest's loc-DB registration; normal play = unaffected when the cvar is off.
### 2. THE HOOK IMPLEMENTATION (`src/skate3_loc_dump.cpp`)
Two cvars (`kHotReload` lifecycle): `skate3_loc_dump = false` (the master switch) and
`skate3_loc_dump_dir = ""` (the optional absolute dump-dir override). Activation = one line in the
launcher's cvar list / `skate3.toml`; the capture self-disables when the cvar is cleared. The hook:
on entry read `r3`, map to the host address `base + (r3 - 0x82000000)`, validate the `0x00039000` LE
magic and the self-describing size (`8 + usize`, the largest observed language .BIN = 10,308 B, the
window 0x20000), and dump once per unique image (a 16-byte FNV hash + the usize dedup so the game's
re-registration across the language-select re-entry = skipped). Then `__imp__sub_828E4A88(ctx, base)`.
The hook contract: the decrypted loc-DB streams only (a non-0x00039000 magic = skipped, so the
challenge-local-data banks and the .xsf textures = not dumped).
### 3. WHY REX_FUNC (the SetFunction conflict recorded)
`0x828E4A88` and `0x828E5E18` are NOT explicit function entries in `config/skate3_functions.toml` —
the codegen's boundaries for this region = graph-derived (they sit inside `sub_82CDD390`'s chain). A
dispatcher `SetFunction(0x828E4A88, ...)` would require a boundary override (and could conflict with
the codegen's mid-chain boundaries for `sub_82CDD390`). The `REX_FUNC(sub_828E4A88)` route resolves
through the recomp function table like any other direct reference, no codegen conflict, and the hook
= observation-only. **The exact conflict is documented and the REX_FUNC route proposed.**
### 4. STATIC VALIDATION against disasm.sqlite
The hook's xrefs: `sub_828E4A88 ← bl @0x82CDD504` and `bl @0x82CDD994` (the two archive registers per
pack, raw-word verified). The parse chain: `sub_828E4A88` → the bswap loop → `bl sub_828E5E18`
@0x828E4B80 (verified: the swap fn calls the parser once). The wrapper `sub_82A5D348`: `[0x30] =
[Job+0x28]` (the desc/src); `[0x34] = [Job+0x2c]` (the dst); then the machine; then `[0x6c] = r3` and
`[0x28]/[0x2c]` = the src/dst (the cursor rollback). The relocated-code consideration: the recomp's
skate3 module resolves guest addresses 1:1 with the retail image (no base shift). `base` in rexglue =
the guest-memory base, so the host address for a guest address `g` = `base + (g - 0x82000000)`. The
output landing: `<app root>/loc_dump/` (the default) or the absolute override; the file name =
`loc_dump_<loc-DB-name>_<guest-addr>.bin`; the dedupe = a 16-byte FNV hash + the usize.
### 5. THE OFFLINE FALLBACK ANALYSIS (`/tmp/opencode/xexre/loc_annotate.py`)
Implemented NOW and ready for a capture: `parse_locdb` (the loc-DB layout parse with the
self-describing size, so a partial dump = detected); `ground_truth_check` (the ground-truth key
resolution); `annotate_records` (writes `otl_records_annotated.txt` (the OTL = the own-the-lot) and
`ots_records_annotated.txt` (the OTS = the own-the-spots) kept SEPARATE per the standing rule, from
the record-table dumps in `analysis/xex/decoded_banks/dlc_decoded/`; the rows' `val` → the
loc-key-name substitution where the val = a loc-key hash present in the captured loc-DB's index; the
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
The tool = ready for a real capture. The 0/1072 matched for this image = expected: the pass-22's
finding was that the record `val` = the bitfield data, NOT the loc-key IDs — so the loc-key-name
annotation only closes once a language .BIN's capture provides the loc-key ID strings (the tool
handles it the moment the dump lands).
### 7. WHAT CANNOT BE BUILT LOCALLY AND WHY (documented)
A full compile requires `generated/sources.cmake` (the recomp codegen output, not in the tree; the
build instructs "run the generate-all target before building skate3") + the rexglue SDK submodule
(initialized in this tree) + the Windows/macOS host ABI. The hook file's includes match the
same-tree patterns, so a user build should resolve them; the local tree = LSP-limited (the include
path does not contain the SDK dir), so a full compile is NOT verifiable locally. The user must build
the recomp to run the capture; the cvar line = the only thing they need to flip.

## PASS 28 — the ots_univ_NN extraction + University spot geometry: the 10 missions' spot volumes and locator nodes dumped (objective = PICK a spot)
### 1. THE EXTRACTION (missions.big EB layer — two corrections to eb3_list.py)
The 10 University OTS missions (`ots_univ_01..05`, `ots_univ_10..14`) live in
`missions.big` (EB v3, align byte 0x06, 5321 entries, 130 ots_univ entries total).
This pass re-derived the container layer raw-word-by-raw-word and FIXED two stale
claims in `analysis/big/eb3_list.py`:
* TOC entry = `[offset_raw][csize][size][hash1]`, actual offset = `offset_raw << 6`,
  and **csize == 0 on ALL 5321 entries** — every payload is stored RAW with length = size.
  (The prior "REFPACK kind" rows in missions_listing.txt came from peeking `10 FB` INSIDE
  the payloads' embedded frames — the entry itself is never compressed.)
* the name-slot tag prefix is NOT always a single `0x99` byte: e.g. `ots_univ_11_*`
  slots start `01 00`. The robust decode = strip leading bytes `< 0x20 or >= 0x80`,
  then NUL-terminate. (Also: the WORD array after the ctype bytes is NOT a folder
  index on missions.big — the folder table maps by TOC-position clustering instead.)
Each mission's 12 Sim/Pres/Tex entries form ONE contiguous archive run (0x2c0 B);
the mission's `cSim_Global.xsf` sits just before its run and the pairing is unambiguous
(cSim offsets ascend with the run starts; each 0x27820-aligned zone owns exactly one).
### 2. THE cSim PAYLOAD DECODE (byte-exact, 10/10)
`cSim_Global.xsf` = 0x150-byte SFIL-family header + a RefPack frame at +0x150:
`[10 FB][flags 00][usize BE u24][data]`. Decoded with `decrypt.py game_refpack_decode`
(the 0x82A5E038 transcription) → STOP status + output length == usize EXACTLY on all
10 missions (usize 7564..12044 B). The decoded magic = `89 'RW4xb2' 00 0d 0a 1a 0a`
(PNG-style) = an RW4 xb2 streamfile — the X360 sibling of the PS3 `.xsf`.
### 3. THE TAGGED NODE STRUCTURE (raw-word verified, enough to retarget)
Inside the RW4xb2 (all floats BIG-endian):
* **Volume records** @~0x1e0..0x4fc: 3-4 boxes, each
  `[min x y z][0][max x y z][0] + [4 corner rows w=1] + [2 id words] + [node tail hash]`.
  Two tail-hash families: `0x0008xxxx` + magic `03e38706` (univ_01..03; min = tail-0x7c,
  max = tail-0x6c) and `0x2c7017xx` + 8-byte tails (univ_04..14; min = tail-0x78).
  Node identity = the tail hash matched against the node tag string
  (`ots_univ_NN_<node>_0x...:...`) — 34/34 boxes mapped across all 10 missions.
  Node naming varies per mission: `spotvolume_01`/`volume_1`, `scoreboundary_01`/
  `scoringboundary`, `challengeboundary_01`/`challengeboundary`.
* **Locator records** @~0x1048..0x1720: `[u32 id][u32 MARKER][3-4 w][3x3 rot][pos][1.0]`
  (+ optional bbox = pos∓(1,1,1)). MARKER = a per-mission u32 (0x6fb/0x748/0x779/0x7f2/
  0x7f8/0x6f2/0x6f9/0x772/0x872/0x9f7) detected by candidate-hit counting (>= 12).
  Record order == the node-name string order (validated on univ_01: waitlocator_01,
  wait_1..6, vi_01, startlocator_01, spawnpoint_01..06). The last record = header-only
  and carries the string table.
* **GEOMETRY CHECK (univ_01)**: the waitlocator pos (-231.1, 197.1, -719.8) is INSIDE
  the spot box (center (-231.1, 195.6, -719.6)) and the spawnpoint cluster
  (-267..-271, 198.1, -738..-743) lies ~40 m away outside all boundaries — the
  anchor/spawn interpretation is self-consistent.
### 4. THE QUICK-PICK TABLE (native (x,y,z); the .md carries the full per-mission detail)
| mission | spot center | footprint (m) |
|---|---|---|
| ots_univ_01 | (-231.1, 195.6, -719.6) | 14 x 6 |
| ots_univ_02 | (296.9, 68.1, -155.2) | 13 x 11 |
| ots_univ_03 | (32.4, 134.6, -622.6) | 33 x 12 |
| ots_univ_04 | (242.8, 77.9, -462.5) | 38 x 8 |
| ots_univ_05 | (112.9, 59.0, -65.6) | 25 x 15 |
| ots_univ_10 | (355.7, 108.5, -670.3) | 272 x 107 |
| ots_univ_11 | (-30.2, 34.1, 240.3) | 22 x 5 |
| ots_univ_12 | (145.1, 18.8, 272.7) | 54 x 12 |
| ots_univ_13 | (353.8, 5.2, 426.9) | 138 x 12 |
| ots_univ_14 | (-238.2, 195.9, -892.0) | 373 x 90 |
(univ_05/11/14 have additional `volume_2`/`volume_3` boxes = secondary spots.)
### 5. DELIVERABLES (analysis/big/)
`extract/ots_univ_NN/` (13 raw entries each) + `extract/ots_univ_NN/cSim_Global.xsf.dec.bin`
+ `univ_spots_geometry.md` (quick-pick summary + per-mission tables + locator positions +
Appendix A: the clone-authoring plan: which bytes to rewrite, the boundary-XML pattern from
`analysis/dlc/decoded/boundary_digest.txt`, the missions.big EB layer (TOC/name-slot/folder/
djb2 hash1 per pass 22), and the challenge-local-data records path) + `univ_spots_geometry_raw.txt`
(the full parse dump) + `extract_univ28.py` + `gen_univ_geometry_md.py` + `STATUS_PASS28.txt`.
### 6. HONEST GAPS
* spawnpoint/start_N name→position pairing = ORDER-based (proven on univ_01 only).
* the axis convention (y = up) is supported by the univ_01 anchor (+1.5 above box center)
  but was NOT cross-checked against a known in-world landmark.
* the AABB framing cannot represent rotated volumes (univ_14's big region).
* the marker word's semantic and the 2 junk id words before the tail hashes = not decoded.

## PASS 29 — 'Morshu's Paradise' OTS authoring: the mission DATA layer built + verified; the challengebank layer = bounded honest stop
### 1. The base loc-DB re-extracted + the record-layout correction
`LANGUAGE_English_Global_skate3ng.BIN` (miscboot.big entry 189) → RefPack STOP
531,068 B exact → 13,606 (pool_off, key) rows. CORRECTION vs the pass-21 note:
the record = [pool_off u32 LE][key u32 LE] — the pool_off = the FIRST word (the
pass-21 "(key, pool_off)" naming = swapped; its ground-truth pair 0x00029d00 ↔
'RANDOM BOARD' still holds under the corrected order, at rec1). Cross-verified
EN↔NL: 'The Wall' = C0F71A75 ↔ 'De muur'. The 13,606 keys = SORTED ascending
(the loc-DB lookup can be a binary search — appended rows must keep the order).
### 2. The loc-key binding mechanism VERIFIED + 'The Wall' unresolved (honest)
vlt32 (DJB2 seed 0xFFFFFFFF) over 'ID_CHALLENGE_*' strings = the loc-DB key —
runtime-confirmed against the maloof capture (13 exact hits incl. 'Maloof NY
1UP' = 8D5B510E). Applied to the 10 University OTS titles: 5 coherent spot
names ('Bench' univ_01, 'Small Yellow Dumpster' univ_02, 'The Pej' univ_10,
'Rasta Panda' univ_11, 'Classic Eagle' univ_12); 'The Wall' (C0F71A75) = NOT
one of the 10 title hashes; the 5 remaining titles hash to unrelated UI
strings; the wall-desc key B096CD70 ('A cool stone wall down a long curving
skate path...') matches no ID string in main.bin. → the mission→display
binding for 'The Wall' is UNRESOLVED offline (needs the pass-27 loc hook on
the base Global DB — the current build/loc_dump capture holds DLC packs only).
Geometric presumption (labeled): 'The Wall' = ots_univ_14 (the only mission
with a long-curving-path-scale volume: volume_1 373x90x96, volume_2 308x396).
### 3. The mission layer = DONE (analysis/custom/morshus_paradise/)
The new mission ots_univ_15 = a clone of ots_univ_11 (the only 3-volume plaza
rail run) RETARGETED into the presumed 'The Wall' region (delta (-210,+142,-1100)):
  * the cSim retagged: 34 'ots_univ_11' → 'ots_univ_15' byte-safe replacements;
    FRESH node hashes for the 5 tagged volume nodes (2c701706 00221aa7/aa8/
    ab1/ab9/aba → 00221d00..d04 — the 0022-family counter continues past
    main.bin's max 00221ca7); replaced BOTH the binary BE u64 (record tails)
    AND the ASCII hex text (tag strings); old hashes gone (verified), new 5/5.
  * 73 float triples retargeted structure-aware: the volume boxes via the
    node-hash-anchored framing (min=H-0x78, max=H-0x68, corners H-0x58..H-0x28)
    + the 15 locator positions (+ their ±1 bboxes) via the per-mission marker
    table (0x600-0xa00 candidate counting, NOT hard-coded offsets).
  * the cSim re-encoded with the NEW literal-only RefPack encoder
    ([10 FB][usize BE u24][0xE0+t-1 = 4t literals; 0xFC|tail STOP]) — the
    decode side of pass-6/21 inverted for the first time; round-trip = STOP
    + exact bytes.
  * the 12 Sim/Pres/Tex stubs cloned with FRESH per-set GUID stamps
    (0304B001/2/3 — above the archive max; the stamp = a per-set GUID verified
    NOT content-derived: univ_01/02's identical 24-byte stubs differ only there).
  * the archive = a full EB v3 rebuild: 5,334 TOC entries, new folder row
    'data/content/missions/ots_univ_15', hash1 = djb2(seed 5381) over
    folder+'/'+name for the new entries (the old entries keep their original
    hash1 — the per-file folder index is NOT recoverable offline, honest
    limitation).
  ORACLES (all green): 5,321/5,321 original payloads byte-exact; 13/13 new
  hash1 verified; the new cSim decodes STOP + exact usize from the repacked
  archive; the 5 boxes + 15 locators land at the new coordinates
  (challengeboundary x[-268,-216.3] y[167.8,191.1] z[-926.1,-824.3]; the rail
  run at rail-deck height y 173.5..178.7).
### 4. The challengebank layer = the bounded stop (no fabricated bank)
The challenge registration lives in db.big's challengebanks/main.vlt (3,508
exports; the per-challenge objects reach their TITLE loc-key via PtrN —
verified exports 166/185/187-192 = the OTS state objects). The blockers,
verified this pass:
  * the attr keys = vlt64 (lookup8) hashes of the attribute names; NO verified
    lookup8 exists (crypt.py's transcription = broken stub; the canonical
    Jenkins does not reproduce the S3Types ground truth 0x97c5113de9bd1ad6 =
    'Attrib::RefSpec' under any tested seed/endianness variant).
  * the scores = inline in the small goal objects (96 B, key 0c0cfefb9fc6eb73
    nf=0x40) but pointer-data in the main challenge objects (nf=0x02/0x08).
Specified but NOT written (the target values): Name loc-key AD1C2A6A =
vlt32('ID_CHALLENGE_OTS_UNIV_15_TITLE') ↔ 'Morshu's Paradise' (DESC key
FD742247), ChallengeType 9, BaseScore 600 (BE 0x00000258), KilledItScore 1000
(BE 0x000003E8), SpotVolumes RefSpec → the fresh volume hash 0x2c70170600221d02.
The loc-DB row add itself = fully specified (one sorted row into
English_Global: [pool_off=len(old_pool)][key=AD1C2A6A] + the string appended
to the pool + count 13,607 + usize recomputed; raw-stored in a rebuilt
miscboot.big = the proven csize=0 EB path) — ships together with the bank.
### 5. Continuation recipe (pass 30)
1. A verified lookup8: capture (attr-string, hash) pairs at runtime (the game
   hashes attr-name strings) or obtain S3Types.dat-style tables.
2. Clone the challenge-object chain in main.vlt: the export whose PtrN string =
   'ID_CHALLENGE_OTS_UNIV_NN_TITLE' + the own_the_spots group entry + the
   per-mission local-data bank ots_univ_15.bin/.vlt (cloned from univ_11) →
   repack db.big (the recedit.py big_fix_hash1 path, validated pass 22).
3. The loc-DB row add (English + optionally the other 8 languages).
4. Runtime verify: launch with the pass-27 loc cvar ON (the base Global DB
   capture should contain the new row) and check the challenge map.
Deliverables: analysis/custom/morshus_paradise/{missions_custom.big,
ots_univ_15_cSim_retarged.dec.bin, ots_univ_15_manifest.txt, build_morshus.py,
INSTALL.md, STATUS_PASS29.txt}.

## PASS 30 — the challengebank write layer: db_custom.big + miscboot_custom.big built with full oracles; the PtrN semantics SOLVED; the vlt64 = honest negative (the EA variant not reproducible)
### 1. THE PtrN SEMANTICS SOLVED (the forge vault's pointer chunk)
The row = [u32 0][u32 bin_off][u32 vlt_file_off][u32 0x00030001] where vlt_file_off =
the address of the TARGET AttribEntry's data word inside the collection block (rel =
104/120/136/152/184/200 = the entry zone after the 48B header + tlen*8 types), and
bin_off = the linked string/data in the .bin. The loader patches the entry's data with
the resolved pointer. (Pass-30 verified: main.vlt export 191's slots = the OTS
TITLE/DESC/locator strings; the framework/main.vlt pairs.)
### 2. THE VLT64 (LOOKUP8) = HONEST NEGATIVE (the EA variant, not the public Jenkins)
The VaultTest ground-truth tables (/tmp/opencode/VaultTest/keys/S3*.dat) are WIPED.
In-tree ground truth: 4,457 clean (object-key, name-string) pairs extracted from
main.vlt via the solved PtrN mapping + 2 from skaterschema.vlt. EVERY candidate = 0
hits against them: the canonical Jenkins lookup8 (level=0xABCDEF0011223344, c0
0xdeadbeef, both endians, NUL variants, len-first/last), the lookup4 32-byte variant,
reversed tails, crypt.py's inline variant (its tail = a stub: b += length + a
non-canonical avalanche; the c += length variant too). The pass-7 hint ('mix64 with
BOTH subtracts + the c += length tail') = applied to the canonical — still 0. The
hash = an EA-specific variant; the remaining derivation = the runtime capture of the
hash's input strings. THE BYPASS: the pass-30 write uses SYNTHETIC but UNIQUE keys
(0f15c0de00000015 / 0f15c0de5a5a0015) with the exemplar structure everywhere else.
### 3. THE CHALLENGE REGISTRATION CHAIN (the exemplar, end to end)
TWO main.bin/main.vlt pairs in db.big: progressionbanks/main (281/282) and
CHALLENGEBANKS/main (933/935) — the OTS registration lives in the CHALLENGEBANKS pair
(hash1 = djb2 over 'data/db/challengebanks/main.vlt' ✓). For one mission (ots_univ_02):
  a. the challenge-local-data bank pair data/db/challenge_local_data/ots_univ_02.bin/
     .vlt (RefPack, csize 249/432 → 696/736): the Forge vault class 46a56addf284c381,
     parent = bf65ab4c87f0a118 (the own_the_spots group) with the INLINE scores:
     attr 32e098540a7efcad = KilledItScore (univ_02 = 2500; the default 0x12c = 300)
     attr 0c0cfefb9fc6eb73 = BaseScore (univ_02 = 800; the default 0x64 = 100);
     the .bin StrE = 'ots_univ_02_intro' + the scoring-boundary node ref
     'DIST_University|ots_univ_02_scoringboundary|0x0008a87c03e38706' + the
     startlocator/vis_N strings; the .bin data = the node-ref blocks.
  b. challengebanks/main.vlt = the per-challenge state object (export 191 = univ_11's:
     class a4f35b159d7432b5, parent = 165db084a583fc60 = the ONLINE OTS group) with the
     ptr slots = the TITLE/DESC/locator id strings.
  c. challenge_local_data_framework.vlt = the class defaults (NOT modified).
### 4. THE WRITE (all oracles green)
build_bank30.py → db_custom.big (1,532 entries):
  * ots_univ_15.bin (696 B) + ots_univ_15.vlt (736 B): the pair cloned from ots_univ_02
    with the names retagged (the DepN vault filenames + all ots_univ_02 strings),
    the node ref → the pass-29 fresh volume hash (binary + the ASCII hex text), the
    fresh mKey 0f15c0de00000015, the INLINE scores = 1000/600 (verified by the forge
    re-parse: the exact AttribEntry values).
  * challengebanks/main.bin: the internal-ID string
    'ID_CHALLENGE_OTS_UNIV_15_TITLE\0' appended at the file end (bin 0x1b5e3c; the StrE
    pad = only 6 B — the in-place placement would shift every absolute data offset).
  * challengebanks/main.vlt: the state object cloned from export 191 (univ_11's;
    the ptr slot rel 184 = the TITLE), fresh key 0f15c0de5a5a0015, +1 ExpN row
    (3,509 exports), +1 PtrN row (bin 0x1b5e3c → the slot at vlt 0x89518).
  ORACLES: 1,528/1,528 untouched payloads byte-exact; the edited pair decodes STOP +
  exact usize + the full chunk walk ('Vers'@0/'StrE'@0); 3,509 exports re-parsed; the
  new bank pair = the intended inline values + the fresh mKey; all edited payloads
  round-trip through the literal-only RefPack encoder.
  CAUGHT DURING VERIFICATION: two self-inflicted bugs fixed en route — the db.big
  name slots = the DLC-variant layout ([u16 BE folderIndex][name]) NOT the 0x99-tag
  variant, and the db.big folder table truncates the last folder string at the slot
  boundary ('data/db/challenge_local_d') — the hash1 = the ground truth for the folder.
### 5. THE LOC-DB ROW (the Name — the pass-29 objective, DONE)
build_miscboot30.py → miscboot_custom.big: the English Global loc-DB +1 row
(key AD1C2A6A = vlt32('ID_CHALLENGE_OTS_UNIV_15_TITLE')) inserted in SORTED key order,
the string "Morshu's Paradise\0" appended to the pool, count 13,607, usize recomputed,
the pool_off +8 (the pool shifts by the row; the 4-byte records/pool pad preserved).
Oracles: 251/251 untouched miscboot entries byte-exact; the rebuilt English entry
re-encoded (535,842 B, STOP + exact usize); the row re-parses to "Morshu's Paradise";
the old probes intact ('RANDOM BOARD' 0x512ae, 'The Wall' C0F71A75). The row exists
ONLY in English (the other-language fallback = unverified offline).
### 6. THE REMAINING RISKS (documented, not hidden)
  * the engine's state-object→challenge linkage: the state object = exemplar-exact
    (class/parent/ptr-slot layout) but whether the resolution is via the vault
    filename (the DepN names = 'ots_univ_15.vlt'/'.bin' ✓ retagged) or a vlt64-keyed
    group entry = NOT runtime-proven.
  * the synthetic keys = unique (the 0f15c0de prefix unused in all banks) but their
    engine-side acceptance = the open risk (no verified lookup8).
  * the appended PtrN row = vlt_off-out-of-order (the exemplar rows = ascending);
    the sort = unproven requirement — the #1 runtime-check item.
  * the miscboot language loader's raw-entry acceptance (csize=0) = the archive-level
    EB path is proven but the LANGUAGE decoder's non-RefPack acceptance = the same
    codec family, not runtime-verified for this archive.
  * 'The Wall' owner = the geometric presumption ots_univ_14 (pass 29).
### 7. CONTINUATION (pass 31)
  1. Runtime: launch with the pass-27 loc hook → the Global loc-DB should contain
     AD1C2A6A → check the University challenge map for the new pin.
  2. If missing: sort the PtrN rows by vlt_off; capture the state-object resolution
     (which key the engine matches: the filename vs the mKey vs the export id).
  3. The vlt64 derivation: the runtime capture of (attr-string, hash) pairs (the
     retail hash fn = 0x82B4B300 per pass 7; the TU3 disasm = shifted — hunt by the
     mix64 rotate pattern) → rebuild the synthetic keys as proper hashes.
  Deliverables: analysis/custom/morshus_paradise/{missions_custom.big (pass 29),
  db_custom.big, miscboot_custom.big, build_bank30.py, build_miscboot30.py,
  build_morshus.py, ots_univ_15_cSim_retarged.dec.bin, ots_univ_15_manifest.txt,
  INSTALL.md, STATUS_PASS29.txt, STATUS_PASS30.txt}.

## PASS 31 — the TRUE drop-in STFS LIVE package: morshus_paradise built + self-validated (the drop-in dlc + the honest variant split)
### 1. The STFS container format = fully mapped against the recomp's own device
The repo's StfsContainerDevice (third_party/rexglue-sdk/src/filesystem/devices/
stfs_container_device.cpp) validates ONLY the magic (CON/LIVE/PIRS — :63/:207) and
then walks: the metadata volume descriptor @0x379 (the layout empirically fixed:
len@+0, version@+1, flags@+2 (read_only bit0), ftbc LE u16 @+3, ftbn u24 LE @+5,
top-hash @+8..0x1C, total BE u32 @+0x1C, free @+0x20), StfsBaseOffset =
max(0xB000, round_up(header_size,0x1000)), the block addressing = 0xB000 +
block<<12 with the hash-block insertion per level (the L0 = @0xB000 for the first
170 blocks when read_only=1 = blocks_per_hash_table=1), the directory chain =
file_table_block_number with the per-block hash's level0_next_block, the entries =
0x40 bytes (name[40], flags: name_len 6b | contiguous 1b (0x40) | directory 1b
(0x80), valid/alloc/start u24 LE, dir_index u16 BE (0xFFFF = root), length u32 BE,
the FAT dates).
The REAL-package layout verified: the descriptor @0x379, the metadata content_type
u32 BE @0x344 (=2 = kMarketplaceContent), the title_id inside execution_info @0x360,
the display_name UTF-16BE @0x411, the content_size u64 @0x34C.
The skateshare package (the smallest) = 1 entry project10_00000000.big (63,936 B);
the pack naming = '<pack>_00000000.big'; the blackboxpark = the same shape. The
challenge packs (dannyway) add spa.bin. The pack loc vault (.vlt) = the plaintext
loc-DB at 0x440 (the rows [LE u32 key][LE u32 pool_off] sorted, the pool strings;
row0 of danny_way_park = key 0x00DF4A26 po 125 — the runtime-capture key!). The
vaultlist.xml = NOT XML (the dannyway's = 284 B of all zeros). The packs' language
.BINs = encrypted (pass 23) — NOT replicable; the packs' OWN locs = read from the
pack loc .vlt plaintext (the runtime-capture evidence).
### 2. The new tooling
analysis/custom/morshus_paradise/stfs_parse.py = the full STFS parser mirroring
StfsContainerDevice's semantics exactly. VALIDATED BYTE-EXACT: the skateshare
package's project10_00000000.big extracted via my parser md5 = the carved_big/
skateshare.big md5. This parser = the self-validation oracle for the built package.
### 3. The built package (morshus_paradise, 73,728 B, bare filename)
morshu_pack_00000000.big (19,392 B, 22 entries, the EB v3 DLC variant flags=0x50):
the pack loc vault (dlc_morshu.vlt: 2 rows, key 0xAD1C2A6A → "Morshu's Paradise",
0xFD742247 → the description; the [key][po] rows sorted, the pool after the 4B pad —
the dannyway exemplar layout), the vaultlist.xml zeros-stub (the dannyway shape),
dlc_morshu.bin/.opt (the zeros pools), data/unlocks/morshu.unlock +
./morshu0.ver (the skateshare exemplar shapes), the challenge-local-data pair
ots_univ_15.bin/.vlt (the PASS-30 Forge banks, decoded from db_custom.big and
re-embedded raw), and the 13 mission payloads (data/content/missions/ots_univ_15/).
The STFS wrapper = the skateshare header template (byte-copied 0..0xB000: the cert/
licenses/title_id/content-id fields = the real skateshare's; the recomp validates
ONLY the magic) patched with the display/description/publisher strings, the content
size, the descriptor (read_only=1, ftbc=1, ftbn=0, total=6), the L0 hash table
(entry[0] = the directory block terminal; entries 1..5 = the data chain), the
directory block, the data blocks. Layout: 0xB000 + the L0 table + 6 blocks.
### 4. SELF-VALIDATION (all green)
stfs_parse.py (the repo-semantics parser) re-parses the built container: LIVE,
read_only, total=6, the directory walk = morshu_00000000.big (dir_index=0xFFFF,
length=19,392), the hash-chain extract = 19,392 B BYTE-IDENTICAL to
morshu_pack_00000000.big; the pack-loc rows resolve ("Morshu's Paradise" @
0xAD1C2A6A); the repo's own DLC EB parser (eb_dlc_list.py, validated on all 5
official packs) lists all 22 entries with the correct paths/sizes/heads
('ENTITLEMENTS', '667130:sk8build', 'StrE'@0, 'Vers'@0). The installer-path checks
mirror the recomp's scan: the magic-only header, content_type=2 (kMarketplaceContent),
the title_id 454108E6, the mount+extract — no cert/RSA verification anywhere
(stfs_container_device.cpp:63/207 + content_manager.cpp InstallContent:598).
TWO BUILD BUGS FIXED EN ROUTE (caught by the validation): the STFS directory
entry's length field was clobbered by the copied skateshare timestamps (the byte
range 54..62 overlaps the length @52..56 — the synthetic timestamps now); the
block-0 hash entry must be terminal (next=kEndOfChain) in the file-table chain
(the data-block chain starts at block 1) — the initial build chained 0→1 and the
walk never terminated.
### 5. WHICH VARIANT + THE HONEST LIMITS
DELIVERED = the hybrid-lite drop-in: the package carries the challenge-local-data
banks + the pack loc vault + the mission data (self-contained geometry — no
missions.big edit required for the geometry). The challenge REGISTRATION = still
the PASS-30 db_custom.big swap (the pack's own challengebank/progressionbank
record-format semantics = the pass-22 open blocker; NOT fabricated). The language
.BINs = absent (encrypted per pass 23; the pack's display strings = in the pack
loc .vlt plaintext, which the loader reads per the runtime capture). The
certificate block = the real skateshare's bytes (unused by the recomp's
magic-only device); the L0 SHA-1 fields = zeros (the device never reads them).
The pure variant (a) (the challenge registering from the pack alone) requires the
challengebank record-format semantics (the pass-22 obj) — honestly open.
### 6. THE USER STEPS (INSTALL.md updated)
1. Copy morshus_paradise → build/dlc/ (or the game-data/user-data dlc/).
2. Boot once → the log 'Installing Skate 3 DLC package' → the files extract to
   ~/.local/share/skate3/0000000000000000/454108E6/00000002/morshus_paradise/.
3. With the PASS-30 db_custom.big swap in place: the University → OTS list =
   "Morshu's Paradise" (600/1000). The missions = in-package (no missions.big edit
   needed for the geometry; the PASS-29 swap = the fallback if the pack-mission
   folder is not picked up by the challenge map).
### 7. CONTINUATION (pass 32, if the runtime test shows gaps)
  1. The in-game check = the ground truth: the challenge map pin + the name/scores.
  2. If the challenge does not appear: capture the DLC-load chain (the
     'dlcbig:' device mapping at skate3_app_common.cpp:1245 + the vaultlist glob
     0x826BC428) to see which pack files the loader opens at runtime.
  3. If the pack-format challengebank semantics are needed: the pass-22 record
     bank analysis continues (the tag→field map via the exemplar diffs).

## PASS 32 — the probe-token fix: the inner .big renamed to morshu_00000000.big (the pack-loc vault name block = the guest's token source; validated from the user's runtime log)
### 1. THE DERIVATION (the runtime log + the source, file:line)
The user's test (build/logs/skate3_021.log) captured the full probe matrix:
  * the launcher installed + mounted our package ('DLC package already installed:
    morshus_paradise' :154; ContentPackage root='\Device\Content\5\'
    host=...morshus_paradise :337 — the ContentPackage ctor at
    content_manager.cpp:40-57 registers '\Device\Content\{++id}' + the
    root_name+':' symlink).
  * 6 mounts: Content\4 = sanvanparty, \5 = OURS, \6 = presell, \7 = maloof,
    \8 = project10, \9 = dway_park.
  * the guest's probe matrix = '<token>_00000000.big' for the tokens
    {morshu, presell, maloof_money_cup, project10, dway_park} across the mounted
    DLC roots — a triangular matrix (the i-th token probed on the roots 0..i).
  THE TOKEN SOURCE: 'presell' and 'project10' = the official packs' pack-loc
  vault 12-byte name blocks (the runtime-capture names). 'morshu' = OUR pack-loc
  vault's name block — the guest read our pack-loc vault and registered our
  token, then probed 'morshu_00000000.big'. Our inner .big = 
  'morshu_pack_00000000.big' → 'entry not found' (0xc000000f, the log :342-343)
  → the challenge-local-data banks were never opened.
  The dlc<digits> root names = the guest's own mount labels — NOT a hash: vlt32/
  djb2 of the file names/display names/token strings all fail the 5 observed
  roots (tested exhaustively); the host registers whatever root_name the guest
  passes (xam_content.cpp xeXamContentCreate → ContentPackage ctor), so the
  digits = irrelevant to the fix.
### 2. THE FIX
build_stfs31.py: the inner .big renamed 'morshu_pack_00000000.big' →
'morshu_00000000.big' (the token 'morshu' from dlc_morshu.vlt's name block +
the '_00000000.big' suffix, exactly like presell/project10). Zero stale
'morshu_pack' occurrences in the rebuilt package (the STFS dir entry, the
pack-loc name block, the .big internals = consistent).
### 3. THE REBUILD ORACLES (all green)
morshus_paradise (73,728 B) rebuilt; morshu_00000000.big (19,392 B):
  * stfs_parse.py: LIVE, read_only=1, total=6, ftbc=1, ftbn=0;
    display_name="Morshu's Paradise"; the dir walk = 1 entry
    'morshu_00000000.big' (dir_index=0xFFFF, length=19,392); the hash-chain
    extract = byte-identical.
  * eb_dlc_list.py: 22 entries, the correct paths/folders/sizes/heads.
  * the pack-loc vault: magic 0x39000/3, usize 95, count 2, stride 0x1C,
    pool_off 52, name 'morshu'; the rows sorted:
    (0xAD1C2A6A, 0) → "Morshu's Paradise"; (0xFD742247, 18) → the description.
  * the EB TOC hash1 = djb2(folder+'/'+name): 22/22.
### 4. THE USER STEPS
Copy the rebuilt morshus_paradise over build/dlc/morshus_paradise; DELETE the
stale installed copy (~/.local/share/skate3/0000000000000000/454108E6/00000002/
morshus_paradise — the launcher skips re-install otherwise); relaunch. The
expected probe: 'dlc<digits>:\morshu_00000000.big' SUCCEEDS on our device.
With the PASS-30 db_custom.big swap: the "Morshu's Paradise" challenge in the
University OTS list (600/1000). INSTALL.md updated (the pass-32 section).
### 5. HONESTLY CLOSED (not needed)
The dlc<digits> root names = the guest's mount labels (no offline hash reproduces
them; the host registers any root_name); no vaultlist/registered-token change =
needed (our token WAS registered — the guest probed it); the content-item-name
metadata = unchanged (the probe = the token+suffix, not the display name).

## PASS 33 — the vaultlist.xml gate: our pack-loc vault never loaded because dlc_morshu.vaultlist.xml was all zeros (the dannyway exemplar re-read = the REAL <VaultList> XML); the package rebuilt
### 1. THE dlcN LABEL ORIGIN (objective 1 — the honest resolution)
The TU3 image hunt: 'dlc%u' found at the dump2 0x229544 (VA 0x82229544), followed by
'%s:' and 'XenonFileDeviceD' — the device-name builder = 'dlc%u' + '%s:'. No direct
absolute references exist in the image (the BE u32 scan = empty; the lis/ori pair
scan = empty in all forms) → the string = via the .sdata pool TOC-relative.
The arithmetic oracle: vlt32/djb2 × {morshu, morshus_paradise, Morshu's Paradise,
morshu_00000000, morshu.vlt, dlc_morshu, play, presell, maloof_money_cup,
project10, dway_park, the display names, the outer hashes} × {ascii, utf16le,
utf16be, +NUL, upper, lower} = ALL fail the observed labels {3787485549,
4023287571, 1272125571, 2652619490, 2192241691}. THE LABELS = the guest's own
mount labels (xeXamContentCreate root_name per pack, xam_content.cpp:118; the
host registers whatever root the guest passes, content_manager.cpp:40-57) —
the labels = deterministic per pack (dlc3787485549 = the play pack in BOTH runs)
but not offline-reproducible. NOT the fix target.
### 2. WHY THE PROBE FAILED (objective 2 — the actual mechanism)
The 024 probe matrix re-read: the failures follow a strict triangular pattern
(the i-th mount's label probes the tokens of the packs mounted AFTER it). The
morshu token was probed on the PLAY mount (the label STABLE = the play pack
across runs) and FAILED — the cross-pack content scan = BENIGN (the morshu .big
is not in the play pack). Our own mount = NOT probed with the morshu token
(the .big lives in our mount = found silently). => the pass-32 rename = CORRECT
and WORKING; the .big probes = consistent with the official packs.
THE ACTUAL FAILURE = the pack-loc vault loader:
  * the loc-dump hook captured 5 pack loc-DBs (danny_way_park 190, play 175,
    maloofmc 107, project10 3, presell 3) — OURS ('morshu', count 2) = ABSENT.
  * the vaultlist.xml = THE GATE: the dannyway's
    dlc_danny_way_park.vaultlist.xml (284 B, extracted from the .big) = the REAL
    XML: <VaultList><VaultFile>dlc_danny_way_park.vlt</VaultFile>...
    <VaultFile>dlc_dwgh_local_data_framework.vlt</VaultFile>
    <VaultFile>challengebanks\dlc_dwgh.vlt</VaultFile>
    <VaultFile>progressionbanks\dlc_dwgh.vlt</VaultFile></VaultList>
    The loader = 'dlcbig:data\db\' + '*.vaultlist.xml' (the 0x826BC428 chain) →
    parses <VaultFile> → loads each vault.
  OUR vaultlist.xml = 284 B of ALL ZEROS (the pass-31 'dannyway exemplar' =
    WRONG — I misread the file at the listing offset; the real content = the
    <VaultList> XML) → the loader = an empty list → loads NOTHING from our pack
    → the pack-loc vault never registers → the 'Morshu's Paradise' loc row
    doesn't resolve in-game.
  (The pass-31 'maloof vaultlist = the 16B-record format' = the maloof pack's
  DIFFERENT vaultlist generation — the maloof/sanvanparty vaultlists =
  the record/obfuscated format; the dannyway = the XML. We mirror the dannyway.)
### 3. THE FIX (objective 3)
build_stfs31.py: the vaultlist.xml = the REAL <VaultList> XML (108 B):
    <?xml version="1.0" encoding="utf-8"?>\r\n<VaultList>\r\n
      <VaultFile>dlc_morshu.vlt</VaultFile>\r\n</VaultList>\r\n
(the dannyway format exactly, minus the framework/challengebank/progressionbank
entries we don't carry). The .big = 19,200 B (22 entries); the STFS = 73,728 B.
ORACLES (all green): stfs_parse (LIVE, read_only=1, total=6, ftbc=1, ftbn=0,
display="Morshu's Paradise", the dir walk = morshu_00000000.big, the hash-chain
extract = byte-identical); the vaultlist = the <VaultList> XML with the
dlc_morshu.vlt entry; the pack-loc vault rows resolve ("Morshu's Paradise" /
the description); eb_dlc_list (22 entries, the entry 2 = the vaultlist.xml with
the '<?xml version=' head); the EB TOC hash1 22/22; zero stale copies; the
exemplar shapes intact.
### 4. THE EXPECTED RUNTIME BEHAVIOR
After the stale-install delete + the rebuilt package copy + the boot: the
vaultlist glob finds dlc_morshu.vaultlist.xml in our pack → parses <VaultList> →
loads dlc_morshu.vlt → the pack-loc vault's 2 rows register → the loc-dump hook
should show a 6TH capture ('morshu', count 2) = the pass-33 success marker.
With the PASS-30 db_custom.big swap: the challenge in the University OTS list.
### 5. THE REMAINING RISKS
The challenge-local-data banks (ots_univ_15.bin/.vlt) = the Forge format vs the
pack local-data banks = the 16B-record format (pass 22); the banks = NOT listed
in the vaultlist (matching the dannyway where only the pack vault + the
framework + the challengebank + the progressionbank are listed) — the local-data
load = via the challengebank refs = via the base path (the pass-30 layer). The
vaultlist may need more entries if the loc-DB capture shows ours loads but the
challenge still doesn't appear. The <VaultFile> path separator = backslash in
the dannyway; our entry = root-level (no separator).

## PASS 34 — the vault-gate forensics: the pack-loc vault loader = 5 official packs only (the loc-DB captures = the definitive evidence); sub_82D083D8 instrumented for the next run
### 1. THE LOG SWEEP (objective 1 — all negatives, 5 sessions)
All 5 sessions (skate3_021..025.log) swept for {dlcbig, vaultlist,
challenge_local, challengebanks, data\db}: ZERO matches in every session.
The vault gate = silent-success (the VFS logs only failures) or never-ran.
THE DEFINITIVE EVIDENCE = the loc-dump hook (the pass-27 pattern): the
loc-DB capture count = 5 in EVERY session. The captured loc-DBs = the 5
OFFICIAL pack loc-DBs (danny_way_park 190, play 175, maloofmc 107,
project10 3, presell 3). OUR pack-loc vault ('morshu', count 2) = NEVER
captured. The pack installs (the 025 log :154-157) = the launcher
re-installed our package (the stale-copy delete = done by the user ✓);
the installed copy = VERIFIED as the pass-33 rebuilt package (19,200 B,
the real <VaultList> XML at 0x780 ✓).
### 2. THE SOURCE TRACE (objective 2)
The dlcbig: device alias (skate3_app_common.cpp:1243-1246,
InstallBigDeviceAliases) = mapped to \Device\Harddisk0\Partition1 = the
BASE game's partition — NOT the pack content devices. The guest's vaultlist
glob = 'dlcbig:data\db\' + '*.vaultlist.xml' (the string cluster at the TU3
image dump2 0x174bea: 'cVaultManager::DLCDirectorySearch' + the glob +
'cVaultListLoadCallback' + 'cVaultBinLoadCallback' + '%s\%s'). The glob =
scans the BASE game's data\db\ — the packs' vaultlists (inside the pack
.bigs on the Content devices) = NEVER scanned via dlcbig:.
The pack-loc vault loader = sub_82D083D8 (TU3; retail 0x82CDD390; the
pass-27 verified address), called from the game-load module chain
sub_826D6BC8 (generated/skate3_recomp.28.cpp:55624, the bl 0x82d083d8). The
loader = a dispatcher-registered function (skate3_register.cpp:13653
SetFunction(0x826D6BC8)), called via the guest's function-dispatcher table
(the vault-open id from the runtime global [0x82FEA50C], 259 loads in the
image per the pass-15 note).
The .big probes = the triangular cross-pack scan (the pack-registry
discovery), NOT the vaultlist gate — the .big probes = all consistent with
the official packs' behavior.
### 3. THE VERDICT (objective 3)
The pack-loc vault loader = processed 5 packs (the official) but NOT ours.
The gate = between the pack mount (our pack = mounted ✓ as Content\6) and
the pack-loc vault loader call. The gate = the module registry (the
vaultlist parse per pack = the DLCDirectorySearch). The exact gate = not
statically resolvable this pass.
### 4. THE INSTRUMENTATION (objective 4)
REX_FUNC(sub_82D083D8) added to src/skate3_loc_dump.cpp: the pack-loc vault
loader fire-trace, logging the call count and the r3/r4/r5 args. The cvar
gate = the same skate3_loc_dump (already on). The next user run:
  N = 5 → the loader = only the official packs (the module-registration
  gate = the static pack list);
  N = 6+ → our pack = included (the module = registered but the vault
  open failed — the r3 args = the descriptor).
The hook = read-only, __imp__ passthrough, the pass-27 pattern (sub_82D083D8
= in the generated skate3_init.cpp:34748 + the DECLARE in skate3_init.h:
35087).
### 5. HONEST LIMITS
The DLCDirectorySearch code = not statically traced (the cVaultManager
vtable = runtime-filled; the codegen graph = the jump-table targets at
0x8217xxxx). The module registration = the runtime-built list. The
dlc<digits> root names = the guest's mount labels (the pass-32 closure).
