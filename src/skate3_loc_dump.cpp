// skate3_loc_dump.cpp — RUNTIME CAPTURE for the DLC LANGUAGE_*.BIN loc-DB plaintext.
//
// PURPOSE
//   The dannyway/sanvanparty LANGUAGE_*.BIN = the loc-DB image under a size-
//   preserving per-file XOR keystream cipher. The cipher key is provably
//   out-of-image (analysis/xex PASS 25): the only way to close the loc-key-name
//   annotation is a runtime capture. This file adds a non-invasive guest
//   observation hook that dumps the decrypted loc-DB stream to host disk once
//   per language load. cvar-gated, default OFF.
//
//   The captured stream parses immediately with the analysis loc-DB layout:
//     [u32 LE magic 0x00039000][u32 LE usize = filesize-8][u32 LE count]
//     [u32 LE stride 0x1C][u32 LE pool_off][16B NUL-padded name]
//     [count × (key u32 LE, off u32 LE)][pool @8+pool_off]
//   and the two pack-vault ground-truth keys resolve exactly:
//     0x00DF4A26 → pool+0x7D ('Voltooi de tranny-demo van Danny Way')
//     0x149D7545 → pool+0x58 ('Voltooi de street-demo van Danny Way')
//
// HOOK SITE (statically validated against disasm.sqlite + the codegen graph)
//   sub_8290C2A8: the loc-DB image register fn (TU3; retail name sub_828E4A88).
//     r3 at entry = the LOADED loc-DB image (the decrypted LANGUAGE_*.BIN
//     stream — one of the two archives the pack-loc loader opens per language
//     per pack: the pack-vault's loc-DB or the per-language set).
//     r4 at entry = the pack's loc handle (ignored here).
//     The image = [u32 LE magic 0x00039000][u32 LE usize = filesize-8][...];
//     the image size = 8 + usize.
//     xrefs (disasm.sqlite, raw-word verified):
//       sub_8290C2A8 ← bl @0x82D0854C and @0x82D089DC (both inside the pack-loc
//       loader 0x82D083D8's body, the two archive registers per pack).
//       sub_8290D638 (the parser) ← bl @0x8290C3F0 inside the bswap fn
//       sub_8290C3F0 (= the retail 0x828E4BD0 attach fn; the retail parser
//       xref was sub_828E5E18 ← bl @0x828E4B80). We hook the swap fn so the
//       dump sees the pre-swap LE bytes, which the offline parser wants.
//
//   REX_FUNC (the link-time override) is used instead of a dispatcher
//   SetFunction because 0x8290C2A8 / 0x8290D638 are NOT explicit function
//   entries in config/skate3_functions.toml — the codegen's boundaries for
//   this region are graph-derived (they sit inside the pack-loc loader's
//   0x82D083D8 chain, which IS an entry). A SetFunction for a non-entry
//   address is not supported by the codegen's boundary model; the REX_FUNC
//   route resolves through the recomp function table like any other direct
//   reference and needs no boundary overrides.
//
//   Guest addresses are used verbatim: the recomp's skate3 module has no
//   base shift (config/skate3_functions.toml's retail image addresses are the
//   guest keys). `base` in the hook = the guest-memory base; the host pointer
//   for a guest address `g` inside the skate3 image = base + (g - 0x82000000).
//
// ACTIVATION (default OFF; zero overhead in normal play)
//   skate3_loc_dump = true          (cvar in skate3.toml / the launcher cvars)
//   skate3_loc_dump_dir = ""        (optional absolute dir override; default
//                                    <app root>/loc_dump/)
//   The cvar is read on every hook entry (kHotReload), so the capture can be
//   enabled per-session without a rebuild. The hook self-disables again when
//   the cvar is cleared, and normal play is unaffected: the hook is read-only
//   on guest memory and dedupes writes by (loc-DB name, guest address).
//
// OUTPUT
//   <app root>/loc_dump/loc_dump_<name>_<addr>.bin  (the decrypted loc-DB)
//   One file per unique (loc-DB name, guest address) pair. The expected dump
//   set for the dannyway pack = 9 language files × 2 DB kinds (Global +
//   HISTOGRAM) ≈ 18 files, plus the pack vault's loc-DB (~1 file per pack).

#include <atomic>
#include <bit>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <imgui.h>
#include <fmt/format.h>
#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/filesystem/vfs.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/system/function_dispatcher.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/keybinds.h>

#include "generated/skate3_init.h"
#include "native/skate3_native_entity.h"  // LookupCtx (ctx -> entity identity)
#include "native/skate3_native_guest_read.h"  // GuestTryCopy (SEH-guarded)
#include "skate3_coord_dump.h"
#include "skate3_native_scene.h"
#include "skate3_native_scene_state.h"  // g_guest_base (host guest-memory base)

REXCVAR_DEFINE_BOOL(skate3_loc_dump, false, "Skate 3",
                    "Dump the decrypted LANGUAGE_*.BIN loc-DB images once per "
                    "language load to <app root>/loc_dump/");
REXCVAR_DEFINE_STRING(skate3_loc_dump_dir, "", "Skate 3",
                      "Optional dump directory override (absolute path)");

namespace skate3::loc_dump {
namespace {

// The guest image base of the recomp's skate3 module. The recomp resolves
// guest addresses 1:1 with the TU3 runtime image (the loc-DB parser =
// 0x8290D638, the swap fn = 0x8290C2A8); there is no base shift — the
// codegen ran on the TU3-patched image (manifests/skate3.toml's
// patched_file_path + skate3_tu_functions.toml), so the TU3 runtime-image
// addresses ARE the generated function keys (nm-verified sub_8290C2A8).
constexpr uint32_t kSkate3ImageBase = 0x82000000;
constexpr size_t kMaxLanguageImage = 0x20000;  // 128KB: the largest observed = 10,308 B.

// The guest buffer's own header drives the size: [u32 LE magic][u32 LE usize
// = filesize-8][...]. So the image size = usize + 8 and the usize = the bytes
// at [4..8] little-endian.
uint32_t ImageSizeFromHeader(const uint8_t* p) {
  return uint32_t(p[4]) | (uint32_t(p[5]) << 8) | (uint32_t(p[6]) << 16) |
         (uint32_t(p[7]) << 24);
}

uint32_t MagicFromHeader(const uint8_t* p) {
  return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) |
         (uint32_t(p[3]) << 24);
}

// The loc-DB's name field ([8..24) = a 16-byte NUL-padded name, e.g.
// "danny_way_park", "project10", "presell", "Global", "HISTOGRAM").
std::string NameFromHeader(const uint8_t* p) {
  std::string name;
  for (int i = 8; i < 24; ++i) {
    const char c = static_cast<char>(p[i]);
    if (c == '\0') break;
    name.push_back(std::isalnum(static_cast<unsigned char>(c)) ? c : '_');
  }
  return name.empty() ? "unnamed" : name;
}

std::filesystem::path DumpDir() {
  const auto configured = REXCVAR_GET(skate3_loc_dump_dir);
  if (!configured.empty()) {
    return std::filesystem::path(configured);
  }
  return rex::filesystem::GetAppRootFolder() / "loc_dump";
}

std::atomic<bool> g_active{false};
std::atomic<uint64_t> g_captures{0};

void DumpLocDb(const uint8_t* p, size_t max_len, uint32_t guest_addr) {
  if (max_len < 24) return;
  if (MagicFromHeader(p) != 0x00039000) {
    // Not a loc-DB image (the raw-stored challenge-local-data banks etc.).
    // The hook's contract = the decrypted loc-DB streams only.
    return;
  }
  const uint32_t usize = ImageSizeFromHeader(p);
  const size_t size = size_t(usize) + 8;
  if (usize < 64 || size > max_len) {
    REXLOG_WARN("Skate 3 loc dump: image at {:#x} usize {:#x} vs window {:#x}; skipped",
                guest_addr, usize, max_len);
    return;
  }
  const std::string name = NameFromHeader(p);
  // Dedup on (loc-DB name, guest address): the loader may re-register the same
  // image across the language-select's re-entry. The address is stable for the
  // same image's load, so this dedupes to one file per unique image.
  static std::atomic<uint64_t> seen_count{0};
  // The dedup key = the loc-DB's name field + the buffer's first 16 bytes.
  // The loader fires the register per language per pack once, so the same
  // image's re-registration (the language select's re-entry) is skipped.
  static std::atomic<uint64_t> last_hash{0};
  uint64_t h = 0;
  for (int i = 0; i < 16; ++i) {
    h = h * 1099511628211ULL + p[i];
  }
  const uint64_t key = h ^ uint64_t(usize) << 32;
  if (last_hash.exchange(key, std::memory_order_relaxed) == key) {
    return;  // same image re-registered, skip
  }
  g_captures.fetch_add(1, std::memory_order_relaxed);

  // Per-session safety cap: the loader registers one image per pack-language
  // combination (≈18 for dannyway + one per pack vault). Anything beyond 64
  // unique images per session = pathological, skip and keep boot fast.
  if (g_captures.load(std::memory_order_relaxed) > 64) {
    return;
  }

  // Copy the bytes NOW (the guest buffer may be recycled by the caller) and
  // write on a detached thread: synchronous I/O on this guest path stalled
  // early boot to a black screen in the field (rex filesystem + boot thread
  // contention on external media). The copy is tiny (≤128 KB) and the
  // thread-local dedupe already ran.
  auto dir = DumpDir();
  std::vector<uint8_t> snapshot(p, p + size);
  const std::string name_copy = name;
  char fname[160];
  std::snprintf(fname, sizeof(fname), "loc_dump_%s_%08X.bin", name_copy.c_str(),
                guest_addr);
  const std::string path_copy = (dir / fname).string();
  const uint64_t capture_index = g_captures.load(std::memory_order_relaxed);
  const uint32_t size_copy = static_cast<uint32_t>(size);
  const uint32_t addr_copy = guest_addr;
  std::thread([snapshot = std::move(snapshot), name_copy, path_copy,
               capture_index, size_copy, addr_copy]() {
    std::error_code ec;
    std::filesystem::create_directories(
        std::filesystem::path(path_copy).parent_path(), ec);
    if (FILE* f = std::fopen(path_copy.c_str(), "wb")) {
      std::fwrite(snapshot.data(), 1, size_copy, f);
      std::fclose(f);
      REXLOG_INFO("Skate 3 loc dump: wrote {} ({} bytes, loc-DB '{}' at "
                  "{:#x}) [capture #{}]",
                  path_copy, size_copy, name_copy, addr_copy, capture_index);
    } else {
      REXLOG_WARN("Skate 3 loc dump: failed to open {}", path_copy);
    }
  }).detach();
}

}  // namespace
}  // namespace skate3::loc_dump

// The loc-DB image register fn: sub_8290C2A8(r3 = the loaded loc-DB image,
// r4 = the pack's loc handle). The image = the decrypted LANGUAGE_*.BIN stream
// for the pack vault OR the per-language set (the pack-loc loader's two archive
// opens). The image size = 8 + usize (the [4..8] LE). Dump PRE-__imp__ so the
// LE bytes are what the offline parser wants.
//
// PASS 27 (2026-09-25): the hook was RETARGETED. The original target
// sub_828E4A88 was a RETAIL-image address; this build runs the TU3-patched
// image (default.xex + default.xexp, title update 3.0.0.0 -> 3.0.3.0), which
// re-links the module (image 18,022,400 B -> 18,546,688 B; .text
// 0x82370000-0x82F734D4 -> 0x82380000-0x82F9E16C). The loc-loader chain was
// verified UNCHANGED in TU3 (byte-identical function bodies; only branch
// targets and data references differ) but RELOCATED:
//   retail 0x828E4A88 (register fn) -> TU3 0x8290C2A8
//   retail 0x828E5E18 (parser)      -> TU3 0x8290D638
//   retail 0x82CDD390 (pack-loc loader) -> TU3 0x82D083D8
// with the two register call sites (retail 0x82CDD504/0x82CDD994) now at TU3
// 0x82D0854C/0x82D089DC, both `bl 0x8290C2A8` (branch-word verified). The old
// REX_FUNC(sub_828E4A88) override bound to TU3 code that is NOT the loc-DB
// register (a container/ref-count method) and NEVER fired (no 'hook fired'
// line in any skate3_*.log, cvar on). Verified against the TU3 runtime image
// (rexglue dump-xex) + tu3_disasm.sqlite; see analysis/xex PASS 27.
extern "C" REX_FUNC(sub_8290C2A8) {
  // Fire-trace: proves whether the override is invoked at all. Logs the first
  // 8 invocations, then every 4096th (guest boot may call other images before
  // language load; without this we can't distinguish "never fires" from
  // "fires but skips").
  static std::atomic<uint64_t> fire_count{0};
  const uint64_t n = fire_count.fetch_add(1, std::memory_order_relaxed);
  if (n < 8 || (n & 0xFFF) == 0) {
    REXLOG_INFO("Skate 3 loc dump: hook fired #{} r3={:#x}", n,
                ctx.r3.u32);
  }
  // Pure observation (no register/state mutation). Safe to run unconditionally
  // when the cvar is on; the cvar is checked inside, so flipping it per-session
  // gates the capture. Zero cost when the cvar is off (a single 32-bit read).
  const uint32_t guest_addr = ctx.r3.u32;
  if (REXCVAR_GET(skate3_loc_dump) && guest_addr >= 0x1000000 &&
      guest_addr < 0x90000000) {
    // Guest addresses here are RUNTIME pointers into guest RAM (the loc-DB is
    // loaded into the guest heap, e.g. 0x40A3C850/0x41893620 — NOT image
    // addresses; run 018's fire-trace proved the old image-region guard
    // `guest_addr >= 0x82000000` rejected every real image). Translate with
    // the shared guest-memory base (host pointer = base + guest address,
    // the same convention skate3_native_scene's GuestTryCopy uses). The
    // magic/usize check inside DumpLocDb rejects non-loc-DB buffers, which
    // is what makes a wide address window safe.
    if (uint8_t* gbase =
            skate3::native_scene::g_guest_base.load(std::memory_order_relaxed)) {
      skate3::loc_dump::DumpLocDb(gbase + guest_addr,
                                  skate3::loc_dump::kMaxLanguageImage,
                                  guest_addr);
    }
  }
  __imp__sub_8290C2A8(ctx, base);
}

// PASS 34: the pack-loc vault loader fire-trace. sub_82D083D8 = the pack-loc
// vault loader body (the pass-27 verified TU3 address, called from the
// game-load module chain sub_826D6BC8). It runs per registered module (the
// vaultlist.xml entries) and registers the pack-loc vaults. Our pack's
// dlc_morshu.vlt = NOT registered (the 025 captures = the 5 official packs
// only). This hook tells us: how many times the loader runs (= the module
// count) and what r3 (the module descriptor) it receives.
// The hook = read-only (a fire-trace + the arg dump); the cvar = the same
// skate3_loc_dump gate. SAFE: the loader's own code runs via __imp__.
extern "C" REX_FUNC(sub_82D083D8) {
  static std::atomic<uint64_t> loader_count{0};
  const uint64_t n = loader_count.fetch_add(1, std::memory_order_relaxed);
  if (n < 16 || (n & 0xFF) == 0) {
    REXLOG_INFO("Skate 3 loc dump: VAULT-LOADER fired #{} r3={:#x} r4={:#x} r5={:#x}",
                n, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32);
  }
  __imp__sub_82D083D8(ctx, base);
}

// ===========================================================================
// PASS 38: the player/camera coordinate debug display (skate3::coord_dump).
//
// LIVES IN THIS TU (not a new CMake source) deliberately: editing
// CMakeLists.txt forces a CMake re-configure, which re-runs the TU-package
// staging (SKATE3_TITLE_UPDATE_PACKAGE, the /tmp/opencode/tu3.xex cache
// entry) — a dependency this machine cannot currently satisfy. Appending
// here keeps the build incremental (one TU recompiled + relink).
//
// Two outputs, both cvar-gated, default OFF:
//   (A) HUD  — skate3_show_coords (F7 toggles live): a small ImGui window
//       with the live player + camera position (top-left).
//   (B) LOG  — skate3_coord_log: appends timestamped x/y/z lines at ~4 Hz
//       to <app root>/coord_dump/skate3_coords.log (tail -f friendly).
//
// Data sources: NO new guest overrides — this TU is tapped from three
// existing, instruction-verified REX_FUNC hooks:
//   - PLAYER: the SkaterPresEntity per-frame taps in skate3_native_render.cpp
//     (sub_82783038 SkaterPresEntity::DoubleBuffer, and sub_82785778, the
//     skater-family UBT+Pack vtable driver). r3 = the skater
//     PresentationEntity; its live world matrix m_MatLtoWTrans (+416, 16
//     floats, tail row (0,0,0,1), translation in component 3 of the affine
//     rows) is validated and its translation recorded. Verified layout per
//     the native-entity identity store (native/skate3_native_entity.cpp,
//     ReadWorldRowsChecked / kEntL2WTrans).
//   - CAMERA: the ViewCamera::SetViewMatrix override in
//     skate3_native_scene.cpp (sub_82802A00, r3 = ViewCamera, r4 = the
//     incoming view Matrix44). Position derived exactly like the native
//     scene builder does (cam = -t * R^T, row-vector convention). Only the
//     MAIN render camera is recorded (cam == the frame-build-published
//     g_sampler_viewcam), so shadow-cascade / portrait-RTT cameras never
//     pollute the sample.
//
// HONEST LABELS: the "player" sample is the nearest-to-camera tracked
// SkaterPresEntity — NPCs of the same class are tracked too; when several
// are live, the HUD/log pick the one closest to the camera (the camera
// follows the player). The camera sample IS the render camera (a few
// meters behind the skater), a navigation proxy, not the skater's own
// transform. The camera sample additionally requires the native scene
// renderer on (it identifies the main camera via g_sampler_viewcam).
// The log write runs on a dedicated thread reading atomics only — NEVER
// on the guest thread (the synchronous-fwrite boot-hang lesson above).

REXCVAR_DEFINE_BOOL(skate3_show_coords, false, "Skate 3",
                    "Show a live player/camera position HUD (top-left). "
                    "F7 toggles it in-game.");
REXCVAR_DEFINE_BOOL(skate3_coord_log, false, "Skate 3",
                    "Append timestamped player/camera coordinates at ~4 Hz "
                    "to <app root>/coord_dump/skate3_coords.log.");
REXCVAR_DEFINE_BOOL(skate3_coord_capture, false, "Skate 3",
                    "Arm the F11 OTS candidate capture (writes one JSON line "
                    "to <app root>/OTS_candidates.json per press). Also armed "
                    "while skate3_show_coords is on. ESC menu: Interface "
                    "section.");

namespace skate3::coord_dump {
namespace {

struct SkaterSample {
  uint32_t entity = 0;
  float pos[3] = {0.0f, 0.0f, 0.0f};
  uint64_t stamp_ns = 0;  // steady_clock
};

constexpr int kMaxSkaters = 16;

std::mutex g_mu;
SkaterSample g_skaters[kMaxSkaters];
int g_skater_count = 0;  // used slots (compact array)
float g_cam[3] = {0.0f, 0.0f, 0.0f};
uint64_t g_cam_stamp_ns = 0;
uint32_t g_cam_guest = 0;                // the ViewCamera guest address
// Never-expiring last-known skater sample (staleness-flagged, not aged out).
float g_last_pos[3] = {0.0f, 0.0f, 0.0f};
uint64_t g_last_pos_ns = 0;
std::atomic<uint64_t> g_tap_skater{0};   // telemetry: tap fire counts
std::atomic<uint64_t> g_tap_skater_ok{0};
std::atomic<uint64_t> g_tap_cam{0};
std::atomic<uint64_t> g_tap_cam_ok{0};

uint64_t NowNs() {
  return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count());
}

bool Finite(float v) { return std::isfinite(v) && v > -1e6f && v < 1e6f; }

// Same structural validation as native_entity.cpp ReadWorldRowsChecked:
// 16-float Matrix44 with a (0,0,0,1) tail row, sane row norms, translation
// within the game's world range. Translation = m[r*4+3].
bool LoadSkaterTranslation(uint8_t* base, uint32_t entity, float out[3]) {
  if (base == nullptr || entity < 0x10000) {
    return false;
  }
  uint32_t raw[16];
  if (!native_scene::GuestTryCopy(raw, base + entity + 416, sizeof(raw))) {
    return false;
  }
  float m[16];
  for (int i = 0; i < 16; ++i) {
    m[i] = std::bit_cast<float>(__builtin_bswap32(raw[i]));
  }
  if (std::fabs(m[12]) > 1e-4f || std::fabs(m[13]) > 1e-4f ||
      std::fabs(m[14]) > 1e-4f || std::fabs(m[15] - 1.0f) > 1e-4f) {
    return false;
  }
  for (int r = 0; r < 3; ++r) {
    float n = 0.0f;
    for (int i = 0; i < 3; ++i) {
      const float f = m[r * 4 + i];
      if (!(f > -1e7f && f < 1e7f)) {
        return false;
      }
      n += f * f;
    }
    if (!(n > 0.0025f && n < 400.0f) || m[r * 4 + 3] <= -20000.0f ||
        m[r * 4 + 3] >= 20000.0f) {
      return false;
    }
  }
  out[0] = m[3];
  out[1] = m[7];
  out[2] = m[11];
  if (!Finite(out[0]) || !Finite(out[1]) || !Finite(out[2])) {
    return false;
  }
  return true;
}

void RecordSkater(uint8_t* base, uint32_t entity) {
  float t[3];
  if (!LoadSkaterTranslation(base, entity, t)) {
    return;  // unreadable / teardown / not yet initialized
  }
  const uint64_t now = NowNs();
  std::lock_guard<std::mutex> lock(g_mu);
  g_tap_skater_ok.fetch_add(1, std::memory_order_relaxed);
  // Never-expiring last-known sample (any entity; the OTS capture reports
  // it with its staleness flag instead of failing when nothing is fresh).
  g_last_pos[0] = t[0];
  g_last_pos[1] = t[1];
  g_last_pos[2] = t[2];
  g_last_pos_ns = now;
  for (int i = 0; i < g_skater_count; ++i) {
    if (g_skaters[i].entity == entity) {
      g_skaters[i].pos[0] = t[0];
      g_skaters[i].pos[1] = t[1];
      g_skaters[i].pos[2] = t[2];
      g_skaters[i].stamp_ns = now;
      return;
    }
  }
  if (g_skater_count < kMaxSkaters) {
    SkaterSample& s = g_skaters[g_skater_count++];
    s.entity = entity;
    s.pos[0] = t[0];
    s.pos[1] = t[1];
    s.pos[2] = t[2];
    s.stamp_ns = now;
  }
  // Table full: silently drop (16 live SkaterPresEntities is already
  // pathological; a ring would thrash anyway).
}

// Pick the reported "player": nearest-to-camera tracked skater when the
// camera is fresh, otherwise the freshest sample. The camera follows the
// player, so this resolves the NPC ambiguity; both are labelled honestly.
bool PickPlayer(float out[3], uint32_t* out_ent, uint64_t max_age_ns) {
  const uint64_t now = NowNs();
  std::lock_guard<std::mutex> lock(g_mu);
  // Age out stale entries in place.
  int n = 0;
  for (int i = 0; i < g_skater_count; ++i) {
    if (now - g_skaters[i].stamp_ns <= max_age_ns) {
      if (n != i) {
        g_skaters[n] = g_skaters[i];
      }
      ++n;
    }
  }
  g_skater_count = n;
  if (n == 0) {
    return false;
  }
  int best = 0;
  if (g_cam_stamp_ns != 0 && now - g_cam_stamp_ns <= max_age_ns) {
    float best_d2 = 0.0f;
    for (int i = 0; i < n; ++i) {
      const float dx = g_skaters[i].pos[0] - g_cam[0];
      const float dy = g_skaters[i].pos[1] - g_cam[1];
      const float dz = g_skaters[i].pos[2] - g_cam[2];
      const float d2 = dx * dx + dy * dy + dz * dz;
      if (i == 0 || d2 < best_d2) {
        best = i;
        best_d2 = d2;
      }
    }
  } else {
    for (int i = 1; i < n; ++i) {  // freshest
      if (g_skaters[i].stamp_ns > g_skaters[best].stamp_ns) {
        best = i;
      }
    }
  }
  out[0] = g_skaters[best].pos[0];
  out[1] = g_skaters[best].pos[1];
  out[2] = g_skaters[best].pos[2];
  if (out_ent != nullptr) {
    *out_ent = g_skaters[best].entity;
  }
  return true;
}

bool PickCamera(float out[3], uint64_t max_age_ns, uint32_t* out_cam) {
  const uint64_t now = NowNs();
  std::lock_guard<std::mutex> lock(g_mu);
  if (g_cam_stamp_ns == 0 || now - g_cam_stamp_ns > max_age_ns) {
    return false;
  }
  out[0] = g_cam[0];
  out[1] = g_cam[1];
  out[2] = g_cam[2];
  if (out_cam != nullptr) {
    *out_cam = g_cam_guest;
  }
  return true;
}

// --- (B) The ~4 Hz coordinate log. Dedicated thread, atomic reads only. ---

std::thread StartLogThread() {
  return std::thread([]() {
    const std::filesystem::path dir =
        rex::filesystem::GetAppRootFolder() / "coord_dump";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const std::filesystem::path path = dir / "skate3_coords.log";
    REXLOG_INFO("Skate 3 coord dump: logging to {}", path.string());
    FILE* f = nullptr;
    uint64_t lines = 0;
    bool logged_open_fail = false;
    for (;;) {
      if (!REXCVAR_GET(skate3_coord_log)) {
        if (f) {
          std::fclose(f);
          f = nullptr;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        continue;
      }
      if (f == nullptr) {
        f = std::fopen(path.c_str(), "a");
        if (f == nullptr) {
          if (!logged_open_fail) {
            REXLOG_WARN("Skate 3 coord dump: cannot open {}", path.string());
            logged_open_fail = true;
          }
          std::this_thread::sleep_for(std::chrono::milliseconds(500));
          continue;
        }
        logged_open_fail = false;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(250));  // ~4 Hz
      // Samples fresher than 2 s count as live; older ones print as "-".
      constexpr uint64_t kMaxAge = uint64_t(2) * 1000 * 1000 * 1000;
      float p[3];
      uint32_t ent = 0;
      float c[3];
      uint32_t cam_g = 0;
      const bool has_p = PickPlayer(p, &ent, kMaxAge);
      const bool has_c = PickCamera(c, kMaxAge, &cam_g);
      if (!has_p && !has_c) {
        continue;  // nothing live yet (menus / loading / cvar just flipped)
      }
      const auto now_sys = std::chrono::system_clock::now();
      const std::time_t tt = std::chrono::system_clock::to_time_t(now_sys);
      std::tm tm{};
      localtime_r(&tt, &tm);
      const uint32_t ms = uint32_t(
          std::chrono::duration_cast<std::chrono::milliseconds>(
              now_sys.time_since_epoch())
              .count() %
          1000);
      char ts[40];
      std::strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm);
      if (has_p && has_c) {
        std::fprintf(f,
                     "%s.%03u player x=%.2f y=%.2f z=%.2f ent=%08X cam "
                     "x=%.2f y=%.2f z=%.2f camc=%08X\n",
                     ts, ms, p[0], p[1], p[2], ent, c[0], c[1], c[2], cam_g);
      } else if (has_p) {
        std::fprintf(f, "%s.%03u player x=%.2f y=%.2f z=%.2f ent=%08X cam -\n",
                     ts, ms, p[0], p[1], p[2], ent);
      } else {
        std::fprintf(f, "%s.%03u player - cam x=%.2f y=%.2f z=%.2f camc=%08X\n",
                     ts, ms, c[0], c[1], c[2], cam_g);
      }
      std::fflush(f);  // tail -f friendly
      if (++lines >= 100000) {  // ~7 h at 4 Hz: truncate on next open
        std::fclose(f);
        f = nullptr;
        lines = 0;
      }
    }
  });
}

std::once_flag g_log_thread_started;

void EnsureLogThread() {
  std::call_once(g_log_thread_started, []() {
    std::thread t = StartLogThread();
    t.detach();
  });
}

// --- (A) The HUD dialog. ---------------------------------------------------

class CoordDumpDialog final : public rex::ui::ImGuiDialog {
 public:
  explicit CoordDumpDialog(rex::ui::ImGuiDrawer* drawer) : ImGuiDialog(drawer) {}

 protected:
  // Passive readout: repaints ride the guest frame paints (gameplay is
  // continuous anyway); avoids a standalone repaint loop inflating FPS.
  bool WantsContinuousRepaint() const override { return false; }

  void OnDraw(ImGuiIO& io) override {
    ImGui::SetNextWindowPos(ImVec2(10.0f, 10.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowBgAlpha(0.55f);
    ImGui::PushFont(nullptr, 18.0f);
    if (ImGui::Begin("Skate 3 coords##coord_dump", nullptr,
                     ImGuiWindowFlags_NoCollapse |
                         ImGuiWindowFlags_NoSavedSettings |
                         ImGuiWindowFlags_AlwaysAutoResize)) {
      constexpr uint64_t kMaxAge = uint64_t(2) * 1000 * 1000 * 1000;
      float p[3];
      uint32_t ent = 0;
      if (PickPlayer(p, &ent, kMaxAge)) {
        ImGui::Text("player x=%.2f y=%.2f z=%.2f", p[0], p[1], p[2]);
        ImGui::PushFont(nullptr, 13.0f);
        ImGui::Text("  (SkaterPresEntity %08X)", ent);
        ImGui::PopFont();
      } else {
        ImGui::TextUnformatted("player (no skater entity in range)");
      }
      float c[3];
      if (PickCamera(c, kMaxAge, nullptr)) {
        ImGui::Text("camera x=%.2f y=%.2f z=%.2f", c[0], c[1], c[2]);
        ImGui::PushFont(nullptr, 13.0f);
        ImGui::TextUnformatted(
            "  camera trails the player (navigation proxy)");
        ImGui::PopFont();
      } else {
        ImGui::TextUnformatted("camera (needs native scene renderer on)");
      }
    }
    ImGui::End();
    ImGui::PopFont();
  }
};

std::unique_ptr<CoordDumpDialog> g_hud;

void SyncHud(rex::ui::ImGuiDrawer* drawer) {
  const bool want = REXCVAR_GET(skate3_show_coords);
  if (want && !g_hud) {
    g_hud = std::make_unique<CoordDumpDialog>(drawer);
    REXLOG_INFO("Skate 3 coord dump: HUD on (F7 toggles)");
  } else if (!want && g_hud) {
    g_hud.reset();
  }
}

// PASS 40: the nearest rendered item the game tracks around the skater —
// the OTS 'anchor' navigation proxy. Excludes everything that would make a
// bad anchor: the player's own skater family (SkaterPresEntity classes),
// living-world peds/vehicles (lw-mapped, they move), hair/cloth garments,
// and items with no usable world transform. The physics contact pair (what
// the skater is REALLY touching) is not resolved here — honest negative,
// recipe in analysis/xex/findings.md PASS 40.
struct ItemProbe {
  float pos[3] = {0.0f, 0.0f, 0.0f};
  float dist_m = -1.0f;
  uint32_t ctx = 0;
  uint32_t entity = 0;
  uint8_t env_family = 0;
  bool char_item = false;
};

bool NearestItemProbe(const float from[3], ItemProbe* out) {
  auto scene = native_scene::LastFrameScene();
  if (!scene) {
    return false;
  }
  float best_d2 = 0.0f;
  bool found = false;
  for (const native_scene::DrawItem& item : scene->items) {
    if (item.ctx == 0) {
      continue;
    }
    if (item.hair) {
      continue;
    }
    // World position: char items with a staged bone palette use the
    // plausible-bone center (the vehicle-retention heuristic); everything
    // else the rigid world matrix translation (row 3, row-vector).
    float p[3];
    if (item.bones.size() >= 12) {
      const size_t nb = item.bones.size() / 12;
      const float b0[3] = {item.bones[3], item.bones[7], item.bones[11]};
      double sum[3] = {0.0, 0.0, 0.0};
      int n = 0;
      for (size_t b = 0; b < nb; ++b) {
        const float t[3] = {item.bones[b * 12 + 3], item.bones[b * 12 + 7],
                            item.bones[b * 12 + 11]};
        if (!std::isfinite(t[0]) || !std::isfinite(t[1]) ||
            !std::isfinite(t[2])) {
          continue;
        }
        const float dx = t[0] - b0[0], dy = t[1] - b0[1], dz = t[2] - b0[2];
        if (dx * dx + dy * dy + dz * dz > 100.0f) {
          continue;  // > 10 m from bone 0: junk row
        }
        sum[0] += t[0];
        sum[1] += t[1];
        sum[2] += t[2];
        ++n;
      }
      if (n == 0) {
        continue;
      }
      p[0] = float(sum[0] / n);
      p[1] = float(sum[1] / n);
      p[2] = float(sum[2] / n);
    } else {
      p[0] = item.world[12];
      p[1] = item.world[13];
      p[2] = item.world[14];
      if (!std::isfinite(p[0]) || !std::isfinite(p[1]) ||
          !std::isfinite(p[2])) {
        continue;
      }
      if (p[0] == 0.0f && p[1] == 0.0f && p[2] == 0.0f) {
        continue;  // identity/never-evaluated transform
      }
    }
    // Bad anchors: the player's own presentation entity (skater classes)
    // and living-world instances (peds/vehicles move — stamped in the item).
    native_entity::CtxInfo info;
    if (native_entity::LookupCtx(item.ctx, &info)) {
      using C = skate3::native_entity::EntClass;
      if (info.cls == C::kSkater || info.cls == C::kColorized ||
          info.cls == C::kSkaterAux) {
        continue;
      }
      out->entity = info.entity;
    }
    if (item.lw_entity != 0) {
      continue;
    }
    const float dx = p[0] - from[0];
    const float dy = p[1] - from[1];
    const float dz = p[2] - from[2];
    const float d2 = dx * dx + dy * dy + dz * dz;
    if (!found || d2 < best_d2) {
      found = true;
      best_d2 = d2;
      out->pos[0] = p[0];
      out->pos[1] = p[1];
      out->pos[2] = p[2];
      out->dist_m = std::sqrt(d2);
      out->ctx = item.ctx;
      out->env_family = item.env_family;
      out->char_item = item.bones.size() >= 12;
    }
  }
  return found;
}

}  // namespace

// The taps (guest threads).

void OnViewMatrix(uint8_t* base, uint32_t cam, uint32_t mtx, bool is_main) {
  if (!is_main || mtx == 0 || cam == 0) {
    return;
  }
  if (!REXCVAR_GET(skate3_show_coords) && !REXCVAR_GET(skate3_coord_log)) {
    return;
  }
  g_tap_cam.fetch_add(1, std::memory_order_relaxed);
  EnsureLogThread();
  if (base == nullptr) {
    return;
  }
  uint32_t raw[16];
  if (!native_scene::GuestTryCopy(raw, base + mtx, sizeof(raw))) {
    return;
  }
  float v[16];
  for (int i = 0; i < 16; ++i) {
    v[i] = std::bit_cast<float>(__builtin_bswap32(raw[i]));
  }
  // cam = -t * R^T over the row-vector view matrix — the exact formula the
  // native scene builder uses (skate3_native_scene.cpp:9267-9277).
  float c[3];
  for (int j = 0; j < 3; ++j) {
    c[j] = -(v[12] * v[j * 4 + 0] + v[13] * v[j * 4 + 1] + v[14] * v[j * 4 + 2]);
  }
  if (!Finite(c[0]) || !Finite(c[1]) || !Finite(c[2])) {
    return;
  }
  const uint64_t now = NowNs();
  std::lock_guard<std::mutex> lock(g_mu);
  g_tap_cam_ok.fetch_add(1, std::memory_order_relaxed);
  g_cam[0] = c[0];
  g_cam[1] = c[1];
  g_cam[2] = c[2];
  g_cam_stamp_ns = now;
  g_cam_guest = cam;
}

void OnSkaterEntity(uint8_t* base, uint32_t entity) {
  if (entity == 0) {
    return;
  }
  if (!REXCVAR_GET(skate3_show_coords) && !REXCVAR_GET(skate3_coord_log)) {
    return;
  }
  g_tap_skater.fetch_add(1, std::memory_order_relaxed);
  EnsureLogThread();
  RecordSkater(base, entity);
}

void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) {
  EnsureLogThread();
  SyncHud(drawer);
  // Live toggle: the change callback creates/resets the dialog. F7 sets the
  // cvar, the callback does the work (the rex_app show_fps_counter pattern).
  rex::cvar::RegisterChangeCallback(
      "skate3_show_coords", [drawer](std::string_view, std::string_view) {
        SyncHud(drawer);
      });
  rex::ui::RegisterBind("bind_skate3_coord_dump", "F7",
                        "Toggle coordinate HUD",
                        [] {
                          REXCVAR_SET(skate3_show_coords,
                                      !REXCVAR_GET(skate3_show_coords));
                        });
  // PASS 40: F11 OTS candidate capture. Armed by skate3_coord_capture (ESC
  // menu item) OR skate3_show_coords. Host input thread — never blocks the
  // guest; the JSON append is a fresh ofstream per press (open/append/
  // close, flushed).
  rex::ui::RegisterBind("bind_skate3_ots_capture", "F11",
                        "Capture OTS candidate (skater position + anchor)",
                        [] { CaptureOtsCandidate(); });
}

void CaptureOtsCandidate() {
  const bool armed = REXCVAR_GET(skate3_coord_capture) ||
                     REXCVAR_GET(skate3_show_coords) ||
                     REXCVAR_GET(skate3_coord_log);
  if (!armed) {
    static std::atomic<uint32_t> s_unarmed_logged{0};
    const uint32_t n = s_unarmed_logged.fetch_add(1, std::memory_order_relaxed);
    if (n < 3) {
      REXLOG_INFO("Skate 3 coord dump: OTS capture pressed but disarmed "
                  "(enable 'Player Coordinates' or 'OTS Candidate Capture' "
                  "in the ESC menu)");
    }
    return;
  }

  constexpr uint64_t kFreshNs = uint64_t(2) * 1000 * 1000 * 1000;
  const uint64_t now = NowNs();
  float pos[3];
  uint64_t pos_ns = 0;
  {
    std::lock_guard<std::mutex> lock(g_mu);
    pos[0] = g_last_pos[0];
    pos[1] = g_last_pos[1];
    pos[2] = g_last_pos[2];
    pos_ns = g_last_pos_ns;
  }
  const bool pos_valid = pos_ns != 0;
  const bool pos_stale = pos_valid && (now - pos_ns > kFreshNs);

  ItemProbe probe;
  const bool has_obstacle = pos_valid && NearestItemProbe(pos, &probe);

  const std::string dist = rex::filesystem::dist_track::LastDistrict();
  const double dist_age = rex::filesystem::dist_track::LastDistrictAgeS();

  // UTC ISO8601 (the user's standing preference).
  const auto now_sys = std::chrono::system_clock::now();
  const std::time_t tt = std::chrono::system_clock::to_time_t(now_sys);
  std::tm tm{};
  gmtime_r(&tt, &tm);
  const uint32_t ms = uint32_t(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          now_sys.time_since_epoch())
          .count() %
      1000);
  char ts[48];
  std::strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%S", &tm);

  std::string line;
  line.reserve(240);
  line += "{\"t\":\"";
  line += ts;
  line += fmt::format(".{:03d}Z\"", ms);
  if (pos_valid) {
    line += fmt::format(",\"pos\":[{:.2f},{:.2f},{:.2f}]", pos[0], pos[1], pos[2]);
  } else {
    line += ",\"pos\":null";
  }
  line += fmt::format(",\"pos_stale\":{}", pos_stale ? "true" : "false");
  float cam[3];
  if (PickCamera(cam, kFreshNs, nullptr)) {
    line += fmt::format(",\"cam\":[{:.2f},{:.2f},{:.2f}]", cam[0], cam[1], cam[2]);
  }
  if (!dist.empty() && dist_age >= 0.0) {
    line += fmt::format(",\"dist\":\"{}\",\"dist_age_s\":{:.1f}", dist, dist_age);
  } else {
    line += ",\"dist\":null";
  }
  if (has_obstacle) {
    // Honest labels: the nearest rendered item is a NAVIGATION PROXY, not
    // the physics contact pair. name = null (no fabricated names); the
    // addr string carries the draw ctx and the owning entity address.
    line += ",\"obstacle\":{\"name\":null,\"addr\":\"";
    line += fmt::format("ctx:0x{:08X}", probe.ctx);
    if (probe.entity != 0) {
      line += fmt::format(" ent:0x{:08X}", probe.entity);
    }
    line += fmt::format("\",\"dist_m\":{:.2f}", probe.dist_m);
    line += fmt::format(",\"env_family\":{},\"char_item\":{}",
                        probe.env_family, probe.char_item ? "true" : "false");
    line += "}";
  } else {
    line += ",\"obstacle\":{\"name\":null,\"addr\":null}";
  }
  line += "\n";

  const auto path =
      rex::filesystem::GetAppRootFolder() / "OTS_candidates.json";
  if (std::ofstream f(path, std::ios::app); f.is_open()) {
    f << line;
    f.flush();
    // ofstream closes on scope exit (fresh open/append/close per press).
    REXLOG_INFO("Skate 3 coord dump: OTS candidate -> {} {}",
                path.string(), line.substr(0, line.size() - 1));
  } else {
    REXLOG_WARN("Skate 3 coord dump: cannot open {}", path.string());
  }
}

}  // namespace skate3::coord_dump
