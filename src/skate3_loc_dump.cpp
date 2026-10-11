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
// PASS 69: the per-pack-loc-vault REGISTRATION shim, default OFF (harmless
// when off = the negative-control contract). With it on, each hook entry
// tries (once per session, guest-side) the key-independent
// register + ATTACH path (sub_8290C2A8 ENTRY-walk → dispatcher
// sub_8290D638) so OUR pack's loc-DB rows register like the officials'.
REXCVAR_DEFINE_BOOL(skate3_register_custom_packs, false, "Skate 3",
                    "Pass-69 shim: register custom DLC packs' loc-DBs "
                    "key-independently (once per session, guest-side).");

namespace skate3::loc_dump {
namespace {

// The guest image base of the recomp's skate3 module. The recomp resolves
// guest addresses 1:1 with the TU3 runtime image (the loc-DB parser =
// 0x8290D638, the swap fn = 0x8290C2A8); there is no base shift — the
// codegen ran on the TU3-patched image (manifests/skate3.toml's
// patched_file_path + skate3_tu_functions.toml), so the TU3 runtime-image
// addresses ARE the generated function keys (nm-verified sub_8290C2A8).
constexpr uint32_t kSkate3ImageBase = 0x82000000;
constexpr size_t kMaxLanguageImage = 0x100000;  // 1 MB: run-073 measured the base English Global loc-DB at 531,155 B — the old 0x20000 window skipped it.

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

// The PASS 69 shim entry (defined in sub_8290C2A8_shim.cpp):
// the cvar-gated, key-independent per-pack-loc-vault REGISTRATION + ATTACH.
namespace skate3::loc_dump {
void Skate3RegisterCustomPackShim(uint32_t rec_guest);
}  // namespace skate3::loc_dump

extern "C" void sub_8290C2A8_guest_dispatch(uint32_t rec, uint32_t entry);

extern "C" void sub_8290C2A8_guest_dispatch(uint32_t rec, uint32_t entry) {
  // A private guest stack (r1) far from any live guest frame; the guest
  // dispatcher's body (sub_8290D638) runs via its __imp__ form (no C++ ABI
  // exposure, reentrant-safe: the shim sees only the FIRE trail of the
  // guest's own walkers).
  PPCContext ctx{};
  ctx.r1.u32 = 0x80000000;
  ctx.r3.u32 = rec;
  ctx.r4.u32 = entry;
  uint8_t* base = skate3::native_scene::g_guest_base.load(
      std::memory_order_relaxed);
  if (!base) {
    REXLOG_WARN("Skate 3 pass69: guest dispatch: no guest base");
    return;
  }
  __imp__sub_8290D638(ctx, base);
}

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
  // PASS 69 (SHIM): the gate-pin trace — one INFO line per fire with the
  // RAW byteswapped r3 argument. The pass-68A arbitration showed ENTRY
  // count=7 with NO morshu-named capture: either our .vlt's +0x10 key word
  // never reaches the walkers' registry match, or the walker-side
  // key-independent shim path (added this pass) is what carries us. This
  // line names every recorded argument so the next run's grep decides.
  static std::atomic<uint64_t> fire_count{0};
  const uint64_t n = fire_count.fetch_add(1, std::memory_order_relaxed);
  if (n < 8 || (n & 0xFFF) == 0) {
    REXLOG_INFO("Skate 3 loc dump: hook fired #{} r3={:#x} r4={:#x}", n,
                ctx.r3.u32, ctx.r4.u32);
  }
  // Pure observation (no register/state mutation). Safe to run unconditionally
  // when the cvar is on; the cvar is checked inside, so flipping it per-session
  // gates the capture. Zero cost when the cvar is off (a single 32-bit read).
  const uint32_t guest_addr = ctx.r3.u32;
  // PASS 67 (FIX-A): the ENTRY trace, trigger 1 — the first 32 fires (was 8),
  // each stamped with a brief argument hex: the loc-DB image head's first
  // 8 bytes (BE words: magic + usize). Read PRE-__imp__ because the caller
  // may recycle the buffer after the callee runs. Honest failure markers,
  // never guessed values.
  if (n < 32 && guest_addr >= 0x1000000 && guest_addr < 0x90000000) {
    if (uint8_t* gbase = skate3::native_scene::g_guest_base.load(
            std::memory_order_relaxed)) {
      uint32_t w0 = 0, w1 = 0;
      const bool ok0 = skate3::native_scene::GuestTryCopy(&w0, gbase + guest_addr, 4);
      const bool ok1 = skate3::native_scene::GuestTryCopy(&w1, gbase + guest_addr + 4, 4);
      if (ok0 && ok1) {
        REXLOG_INFO("Skate 3 pass67: ENTRY #{} arg={:#x} head={:08X} {:08X}",
                    n, guest_addr, __builtin_bswap32(w0), __builtin_bswap32(w1));
      } else {
        REXLOG_INFO("Skate 3 pass67: ENTRY #{} arg={:#x} head=read=FAIL",
                    n, guest_addr);
      }
    } else {
      REXLOG_INFO("Skate 3 pass67: ENTRY #{} arg={:#x} head=NO_GUEST_BASE",
                  n, guest_addr);
    }
  }
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
  // =========================================================================
  // PASS 69 (SHIM, skate3_register_custom_packs, default OFF): the
  // key-independent per-pack-loc-vault REGISTRATION + ATTACH path.
  //
  // Static shape (this pass's full decode, recorded in
  // decode_reconstruction.md PASS 69): sub_8290C2A8 = find(rec-key, r4-key)
  // over the stride-8 pair table + the record's words 0/1 byte-swap +
  // dispatch sub_8290D638(rec, pair_val). The dispatcher's guards:
  //   (1) rec[0] (BE) == 0x00039000 = the loc-DB image magic,
  //   (2) r4 = a registry ENTRY: ENTRY[+8] = the pair-table key,
  //       ENTRY[+0x18] = the first DEP record of the key's [8+i*0x1C]
  //       loc-DB row, DEP[+4] = its key-count,
  // and its body: DEP.word2 = the row key @+0x10, DEP.word3 = rec[0]
  // (the .BIG pool ptr), DEP.word4 = rec, DEP.word0 = used-count++.
  //
  // THE GATE-PIN: the walkers call sub_8290C2A8 with the key INDEPENDENT
  // of the record bytes — a byte-conformant pack .vlt/.BIN is NOT required
  // for the registration+ATTACH to succeed. So the shim:
  //   1. rec = the record this fire carries (BE r3, saved pre-__imp__);
  //   2. ENTRY-WALK: scan the ENTRY array ([0x8306DCA4], stride 0x10,
  //      INIT = 100 slots) for the slot whose word2 (its pair-table key)
  //      is STILL VISIBLE in the pair table ([0x830684F4] rows, stride 8,
  //      count [0x82FDE294] ≥ 0; a {key,0} row = the killed tail slot) —
  //      the officials' registrations happen per walk and stay visible
  //      within it; a killed-tail hit = the window (and the table) closed.
  //      No slot qualifies = no-op (honest miss).
  //   3. patch ENTRY+8 = the table's tail key (kill marker's row),
  //      DEP+0xC = ENTRY, DEP+4 = 1;
  //   4. call the GUEST dispatcher via the plain-C shim
  //      sub_8290C2A8_guest_dispatch(r3 = rec, r4 = ENTRY) = __imp__
  //      sub_8290D638 (the real pair-table append path's second half —
  //      the ATTACH; no C++ ABI exposure, reentrant-safe: the shim sees
  //      only the FIRE trail of the guest's own walkers).
  // The pass-67/68A hooks, rates and the capture guard stay untouched;
  // this block is cvar-gated and reads guest memory SEH-guarded only.
  if (REXCVAR_GET(skate3_register_custom_packs) &&
      guest_addr >= 0x1000000 && guest_addr < 0x90000000) {
    skate3::loc_dump::Skate3RegisterCustomPackShim(guest_addr);
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
  // PASS 58 registration-window probe: fires exactly when the pack-loc
  // loader chain is active (each sub_82D083D8 fire brackets the
  // 8290C548 alloc + the 8290C2A8 register for ONE module; VAULT-LOADER
  // fire #1 = the game-load module loop). Sampled AFTER the body so the
  // just-registered context reads back. Decides pass-34 branch (a1)
  // empirically: an '8290C548 returned' line WITHOUT an '8290C2A8'
  // line = the alloc-only path taken (the module never registered);
  // an '8290C2A8' line per module = the register side live per module.
  if (REXCVAR_GET(skate3_loc_dump)) {
    uint8_t* gbase = skate3::native_scene::g_guest_base.load(
        std::memory_order_relaxed);
    if (gbase) {
      uint32_t raw = 0, gidx = 0, cnt = 0, last = 0;
      if (skate3::native_scene::GuestTryCopy(&raw, gbase + 0x830C2288, 4)) {
        gidx = __builtin_bswap32(raw);
      }
      if (skate3::native_scene::GuestTryCopy(&raw, gbase + 0x830BE4F4, 4)) {
        cnt = __builtin_bswap32(raw);
      }
      if (cnt != 0 && skate3::native_scene::GuestTryCopy(
                          &raw, gbase + 0x830BEE40 + (cnt - 1) * 8, 4)) {
        last = __builtin_bswap32(raw);
      }
      REXLOG_INFO("Skate 3 pass58: REG-WINDOW #{} gidx={:#x} "
                  "alloc_count={} last_slot={:#x}",
                  n, gidx, cnt, last);
    }
  }
}

// ===========================================================================
// PASS 43 (hooks) / PASS 43b (find-context layout corrected): the
// challenge-list / vault-register chain fire-trace.
//
// Chain mapped from the TU3 disasm:
//   sub_826DAB98  = the DLCDirectorySearch DRIVER: mgr ([r3]) +0x1450 =
//     the search handle (a find-context object), +0x1458 = the search
//     functor (virtual call, bctrl, with the debug tag
//     'cVaultManager::DLCDirectorySearch'), per found file calls
//     sub_828D9378, then FindNextFile 0x8298f430 + FindClose 0x8298fff8.
//   sub_828D9378  = the per-file VAULTLIST PROCESSOR. PASS 43b layout
//     correction (run-048 dumped the glob spec + garbage, not names —
//     handle+8 was the wrong offset): the find-context is a
//     WIN32_FIND_DATA-like record the guest's string helper 0x82a884d0
//     concatenates:
//       handle+0x14  = the search DIRECTORY string ('...dlcbig:data\db\'),
//       handle+0x140 = the CURRENT MATCH NAME (the found file!),
//     and the dir+name output goes to the caller's r1+0x50 buffer.
//   sub_8247F9E8  = the load-callback STRING-ATTACH setter (obj{ptr,len}
//     <- r4 string, disasm-verified): fires per queued vault load WITH
//     the full path — the cleanest submit-side trace (which exact paths
//     the game turns into vault loads).
//   sub_828D9510  = the cVaultBinLoadCallback-side creator (HOT ~120/s
//     even at menu — not chain-discriminative, kept for parity).
// Everything read-only, SEH-guarded via GuestTryCopy, cvar skate3_loc_dump.
//
// PASS 43b: hex-dump fallbacks added (bounded, SEH-guarded) so the next
// run prints the found names even if a layout guess misses again.
namespace {

// SEH-guarded hex dump: appends up to `words` BE u32s at addr into out.
void HexDumpWords(uint8_t* gbase, uint32_t addr, int words, std::string* out) {
  char buf[16];
  for (int i = 0; i < words; ++i) {
    uint32_t raw = 0;
    if (!skate3::native_scene::GuestTryCopy(&raw, gbase + addr + i * 4, 4)) {
      out->append("...");
      return;  // unreadable: honest truncation
    }
    raw = __builtin_bswap32(raw);
    std::snprintf(buf, sizeof(buf), "%08X", raw);
    out->append(buf);
    if (i + 1 < words) out->append(" ");
  }
}

// ===========================================================================
// PASS 69 (the PAIR-TABLE slots fully re-derived from tu3_disasm.sqlite —
// supersedes the pass-67 comment block below, whose word ADDRESSES all
// re-verify; the LAYOUT/semantics half now closes):
//   count word [0x82FDE294]: s32; -1 = empty; per-APPEND = max_key+1.
//     All 14 binary accesses pin to it; 0x82FE9294 (the old suspect) = ZERO.
//   pairs word [0x830684F4]: pointer to the PAIR rows, stride 8 =
//     {key s32, val s32} (INIT fills key=-1, val=0; find/append/removal all
//     stride 8; 0x830684E4/-0x10 variants = ZERO accesses — the pass-67
//     "stride 0x10" comment half was wrong).
//   gate  word [0x8306DCA4]: pointer to the ENTRY rows, stride 0x10 =
//     {word0 in-use, word1 [0x83084110]-snapshot, word2 key, word3 0}.
//     (INIT allocs 100×0x10 = 0x640 B + 100×8 = 0x320 B pairs via the
//     registry sub-object's virtual m3, count=-1, [0x83084110]=2.)
//   CHAIN: INIT sub_8290BEB8 (per-mount, from 0x82D08090; the only caller)
//     → WALKER sub_82D083D8 (per module): PAIRFIND-APPEND = sub_8290C548
//     (gate≠0 + count+1<0x64; key = max+1; ENTRY at first slot+4==0;
//     dispatch m3(slot+0xC, ENTRY_key*0x1C) = the loc-DB setup fn)
//     → REGISTER/ATTACH = sub_8290C2A8 (signed find over the stride-8
//     pairs; byte-swaps the record's words 0/1; then dispatcher
//     sub_8290D638(rec, pair_val): rec[0]==0x00039000 (the loc-DB magic,
//     NOT a DEP table) → DEP copy into the key's [8+idx*0x1C] loc-DB row,
//     DEP.word2 = the row key @+0x10, DEP.word3 = the .BIG pool ptr,
//     DEP.word4 = the loc-DB image ptr, DEP word0 = used-count++)
//     → TEARDOWN sub_82D08ED0 → REMOVE = sub_8290C6C8 (find + shift +
//     count-- + freed pair = {-1, 0}) → sub_8290C7D0 = full clear.
//   The walkers (0x82D0854C key match / 0x82D089DC queue drain) call
//     sub_8290C2A8 with the key INDEPENDENT of the record bytes →
//     byte-conformant pack .vlt/.BIN content is NOT required for the
//     registration+ATTACH to succeed → the wrapper-side shim below.
//   Semantics: rec word1 = the name-key (word0 = the .BIG pool ptr), and
//     r4 @sub_82D083D8+0x2C = the registry handle whose vtable+8 the
//     walkers call per found file (the pass-53 functor triple's site).
// All reads SEH-guarded (GuestTryCopy); one-shot per session, fired once
// the pass59 found-record burst has progressed (its 16th+ fire) or ~2 s
// of no new found lines elapse, whichever first. The dump is read-only
// observation; the SHIM (registration) lives in the sub_8290C2A8 hook
// above, cvar-gated (skate3_register_custom_packs, default OFF).
void DumpPairTable(uint64_t at_fire) {
  uint8_t* gbase = skate3::native_scene::g_guest_base.load(
      std::memory_order_relaxed);
  if (!gbase) {
    REXLOG_WARN("Skate 3 pass67: pair-table dump: no guest base yet");
    return;
  }
  uint32_t raw = 0, count = 0, rows = 0, clone = 0;
  if (skate3::native_scene::GuestTryCopy(&raw, gbase + 0x82FDE294, 4)) {
    count = __builtin_bswap32(raw);
  }
  if (skate3::native_scene::GuestTryCopy(&raw, gbase + 0x830684F4, 4)) {
    rows = __builtin_bswap32(raw);
  }
  if (skate3::native_scene::GuestTryCopy(&raw, gbase + 0x8306DCA4, 4)) {
    clone = __builtin_bswap32(raw);
  }
  REXLOG_INFO("Skate 3 pass67: PAIRTABLE #fire={} count={:#x} rows={:#x} "
              "clone={:#x}", at_fire, count, rows, clone);
  if (rows != 0) {
    std::string row0, last10, last8;
    HexDumpWords(gbase, rows, 4, &row0);
    REXLOG_INFO("Skate 3 pass67: PAIRTABLE row0={}", row0);
    if (count >= 1 && count < 100) {
      // stride-0x10 reading (the fresh-body row-obj layout): the last row's
      // first 8 words cover the row itself + its neighbor.
      HexDumpWords(gbase, rows + (count - 1) * 0x10, 8, &last10);
      REXLOG_INFO("Skate 3 pass67: PAIRTABLE lastrow@ret*10 ={}", last10);
      // stride-8 reading (the max-scan / dedup cmp stride): the same slot's
      // word pair, cross-check for the offline FIX-B dep-pair derivation.
      HexDumpWords(gbase, rows + (count - 1) * 8, 4, &last8);
      REXLOG_INFO("Skate 3 pass67: PAIRTABLE lastrow@ret*8 ={}", last8);
    }
  }
}

// PASS 67: the one-shot dump gate — both triggers (the 16th found line and
// the 2 s silence fallback) funnel through one flag so the dump runs once.
std::atomic<bool> g_pair_dump_done{false};
std::atomic<int64_t> g_found_first_ns{0};  // steady_clock ns of the first found line

void MaybeDumpPairTable(uint64_t at_fire) {
  if (g_pair_dump_done.exchange(true, std::memory_order_relaxed) == false) {
    DumpPairTable(at_fire);
  }
}

int64_t SteadyNsNow() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

}  // namespace

extern "C" REX_FUNC(sub_826DAB98) {
  static std::atomic<uint64_t> count{0};
  const uint64_t n = count.fetch_add(1, std::memory_order_relaxed);
  if (REXCVAR_GET(skate3_loc_dump) && (n < 8 || (n & 0xFF) == 0)) {
    const uint32_t mgr = ctx.r3.u32;
    std::string hex;
    std::string method;
    if (uint8_t* gbase =
            skate3::native_scene::g_guest_base.load(std::memory_order_relaxed)) {
      HexDumpWords(gbase, mgr + 0x1450, 4, &hex);
      // PASS 46: capture the FUNCTOR's search method: mgr+0x1458 -> functor
      // object; [functor] = vtable; vtable+8 = the method that drives the
      // candidate enumeration (the pass-43b wait loop's process target).
      // Its VA printed here = the static hook target for the next pass
      // (verified codegen-entry before use).
      uint32_t functor = 0, raw = 0;
      const char* f_note = "";
      if (!skate3::native_scene::GuestTryCopy(&raw, gbase + mgr + 0x1458, 4)) {
        f_note = " functor-read=FAIL";
      } else {
        functor = __builtin_bswap32(raw);
        uint32_t vtable = 0;
        if (!skate3::native_scene::GuestTryCopy(&raw, gbase + functor, 4)) {
          f_note = " vtable-read=FAIL";
        } else {
          vtable = __builtin_bswap32(raw);
          uint32_t slot8 = 0;
          if (!skate3::native_scene::GuestTryCopy(&raw, gbase + vtable + 8, 4)) {
            f_note = " method8-read=FAIL";
          } else {
            slot8 = __builtin_bswap32(raw);
            if (slot8 != 0) {
              method = fmt::format("functor={:#x} vtable={:#x} method8={:#x}",
                                   functor, vtable, slot8);
            }
          }
        }
      }
      if (!method.empty()) method += f_note;
      else if (f_note[0] != 0) method = f_note + 1;
    }
    REXLOG_INFO("Skate 3 pass46: SEARCH-DRIVER #{} mgr={:#x} mgr1450..={} {}",
                n, mgr, hex, method);
  }
  __imp__sub_826DAB98(ctx, base);
}

extern "C" REX_FUNC(sub_828D9378) {
  static std::atomic<uint64_t> count{0};
  const uint64_t n = count.fetch_add(1, std::memory_order_relaxed);
  // PASS 58: the per-fire identity print is DEBUG-only now (at INFO it
  // printed 2 lines per record in 070/071 — a duplicate of the same
  // functor/vtable/method8 triple); the bracket stays at INFO.
  if (n == 0) {
    REXLOG_INFO("Skate 3 pass58: VAULTLIST-FOUND window OPEN (per-fire "
                "identity + name reads at DEBUG)");
  }
  if (REXCVAR_GET(skate3_loc_dump)) {
    const uint32_t mgr = ctx.r3.u32;
    std::string dir, name, hex_head, hex_tail;
    uint32_t handle = 0, functor53 = 0, vtable53 = 0, slot8_53 = 0;
    if (uint8_t* gbase =
            skate3::native_scene::g_guest_base.load(std::memory_order_relaxed)) {
      uint32_t raw = 0;
      if (skate3::native_scene::GuestTryCopy(&raw, gbase + mgr + 0x1450, 4)) {
        handle = __builtin_bswap32(raw);
      }
      // PASS 53: capture the functor chain while the records are live.
      if (skate3::native_scene::GuestTryCopy(&raw, gbase + mgr + 0x1458, 4)) {
        functor53 = __builtin_bswap32(raw);
      }
      if (functor53 != 0 && skate3::native_scene::GuestTryCopy(
                                &raw, gbase + functor53, 4)) {
        vtable53 = __builtin_bswap32(raw);
      }
      if (vtable53 != 0 && skate3::native_scene::GuestTryCopy(
                                &raw, gbase + vtable53 + 8, 4)) {
        slot8_53 = __builtin_bswap32(raw);
      }
      // PASS 59 RESTORE (the pass-43b record reads): the pass-56/58 hook
      // rewrites dropped these reads — dir/name/hex_head were declared but
      // never filled, so every fire printed dir='' name='' head= (runs
      // 070-074) while the records were live and readable (the identity
      // chain read through fine in the same session). Restore:
      //   dir  @handle+0x14   = the search directory spec
      //   name @handle+0x140  = the CURRENT MATCH filename
      //   head fallback       = 6 words @handle (raw bytes even when the
      //                         string reads fail)
      if (handle != 0) {
        char buf[128] = {};
        if (skate3::native_scene::GuestTryCopy(buf, gbase + handle + 0x14,
                                               sizeof(buf) - 1)) {
          dir = buf;
        }
        std::memset(buf, 0, sizeof(buf));
        if (skate3::native_scene::GuestTryCopy(buf, gbase + handle + 0x140,
                                               sizeof(buf) - 1)) {
          name = buf;
        }
        HexDumpWords(gbase, handle, 6, &hex_head);
      }
    }
    if (n < 16) {
      REXLOG_DEBUG("Skate 3 pass58: VAULTLIST-FOUND #{} identity: "
                   "functor={:#x} vtable={:#x} method8={:#x}",
                   n, functor53, vtable53, slot8_53);
    }
    // PASS 58: the per-fire name read moved to DEBUG (log_level=debug in
    // skate3.toml to see it; INFO runs stay quiet — this hook fires per
    // found file in a hot loop and the identity pair above already
    // carries the walk state at INFO).
    REXLOG_DEBUG(
        "Skate 3 pass58: VAULTLIST-FOUND #{} mgr={:#x} dir='{}' name='{}' "
        "head={}", n, mgr, dir, name, hex_head);
    // PASS 59: the found NAME is the decisive record — promote it to INFO
    // (visible at any log level) when non-empty so no run loses it to a
    // DEBUG filter; plus the bounded tail fallback (8 words @handle+0x130).
    if (!name.empty()) {
      if (uint8_t* gbase = skate3::native_scene::g_guest_base.load(
              std::memory_order_relaxed)) {
        HexDumpWords(gbase, handle + 0x130, 8, &hex_tail);
      }
      REXLOG_INFO("Skate 3 pass59: VAULTLIST-FOUND #{} dir='{}' name='{}' "
                  "tail={}", n, dir, name, hex_tail);
    }
    // PASS 67 (FIX-A): the pair-table live dump, one-shot per session —
    // trigger 1: at the 16th+ found line (the burst completed ~1.5 s in
    // run-081, so the 16th fire sits just after the window closes and the
    // dump correlates with the pass59 timestamps by construction).
    // Trigger 2 (the timed fallback): MaybeDumpPairTable is also armed from
    // the hot VAULTBIN-CB site below, firing on ~2 s of no new found
    // lines, so a shorter burst still dumps exactly once. Both funnel
    // through g_pair_dump_done.
    {
      int64_t expected = 0;
      g_found_first_ns.compare_exchange_strong(expected, SteadyNsNow(),
                                               std::memory_order_relaxed);
    }
    if (n >= 16) {
      MaybeDumpPairTable(n);
    }
  }
  __imp__sub_828D9378(ctx, base);
}

extern "C" REX_FUNC(sub_8247F9E8) {
  static std::atomic<uint64_t> count{0};
  const uint64_t n = count.fetch_add(1, std::memory_order_relaxed);
  if (REXCVAR_GET(skate3_loc_dump) && (n < 16 || (n & 0xFF) == 0)) {
    const uint32_t obj = ctx.r3.u32;
    const uint32_t path = ctx.r4.u32;
    std::string s;
    uint8_t* gbase = path != 0 ? skate3::native_scene::g_guest_base.load(
                                     std::memory_order_relaxed)
                               : nullptr;
    if (gbase) {
      char buf[128] = {};
      if (skate3::native_scene::GuestTryCopy(buf, gbase + path,
                                             sizeof(buf) - 1)) {
        s = buf;
      }
    }
    REXLOG_INFO("Skate 3 pass43: VAULTLOAD-ATTACH #{} obj={:#x} path='{}'",
                n, obj, s);
  }
  __imp__sub_8247F9E8(ctx, base);
}

// ===========================================================================
// PASS 46: the ENUMERATION-SOURCE trace. Pass-43b mapped the record shape
// (find record: +0x14 dir spec, +0x140 match name) but the SOURCE of the
// candidates was still open. This pass, static mapping of the find-context
// creator and the cVaultManager ctor:
//   sub_828AAE90(size, device_key, tag, flags) = the find-context CREATOR:
//     it walks a global list ([glob]-style, stride 0x6c entries), indexes
//     it by the device_key arg (mulli r10, r29(arg2), 0x6c — arg2 = the
//     per-search DEVICE KEY/INDEX), and the entry's vtable+8 method
//     builds the find record. The (device_key, tag) pair per search =
//     THE enumeration-source answer.
//   sub_828D8B58(mgr?, dev, functor, flags) = the manager REGISTER
//     (stores r4->+0x1454, r5->+0x1458 = the functor, r6->+0x145c,
//     zeroes +0x1450 = the current find record, clears the atomic at
//     +0x1460) — its single caller sub_826D65A4 passes the functor
//     created by 0x828AAE90 with the device key [global 0x830C621C].
// Hooks: dump r3/r4/r5 (+ the tag string read for r5/r4-string form),
// cvar-gated, SEH-guarded.
extern "C" REX_FUNC(sub_828AAE90) {
  static std::atomic<uint64_t> count{0};
  const uint64_t n = count.fetch_add(1, std::memory_order_relaxed);
  if (REXCVAR_GET(skate3_loc_dump) && (n < 16 || (n & 0xFF) == 0)) {
    const uint32_t size = ctx.r3.u32;
    const uint32_t key = ctx.r4.u32;
    const uint32_t tag = ctx.r5.u32;
    std::string s;
    uint8_t* gbase = (tag >= 0x80000000 && (tag & 3) == 0)
                         ? skate3::native_scene::g_guest_base.load(
                               std::memory_order_relaxed)
                         : nullptr;
    if (gbase) {
      char buf[64] = {};
      if (skate3::native_scene::GuestTryCopy(buf, gbase + tag,
                                             sizeof(buf) - 1)) {
        s = buf;
      }
    }
    REXLOG_INFO("Skate 3 pass46: FIND-CREATE #{} size={:#x} key={:#x} tag='{}'",
                n, size, key, s);
  }
  __imp__sub_828AAE90(ctx, base);
}

// PASS 54: the 0x6C-stride registry instrumentation.
// sub_8298E020 = the WALK'S per-slot iterator (pass-53: the walk method8
// 0x8298E4E8 tail-branches here). Args per its disasm: r3 = the pool base,
// r4 = the DEVICE INDEX (slot selector into [pool+0xC1B4] + idx*0x6C),
// r5 = a rule object, r6 = search flag bits, r7/r8 = buffers. It resolves
// the slot record then hands the entry's sub-object to sub_8298E2E8.
// This hook prints the resolved slot's identity words (SEH-guarded: the
// sub-object pointers at slot+0x30/+0x34 and the record head 8 words)
// so one boot shows the per-device REGISTRY contents — our pack's slot
// (index unknown) vs the official slots side by side.
extern "C" REX_FUNC(sub_8298E020) {
  static std::atomic<uint64_t> count{0};
  const uint64_t n = count.fetch_add(1, std::memory_order_relaxed);
  if (REXCVAR_GET(skate3_loc_dump) && (n < 32 || (n & 0x3FF) == 0)) {
    const uint32_t pool = ctx.r3.u32;
    const uint32_t idx = ctx.r4.u32;
    std::string head, sub;
    if (uint8_t* gbase =
            skate3::native_scene::g_guest_base.load(std::memory_order_relaxed)) {
      uint32_t raw = 0, table = 0, slot = 0;
      if (skate3::native_scene::GuestTryCopy(
              &raw, gbase + pool + 0xC1B4, 4)) {
        table = __builtin_bswap32(raw);
      }
      if (table != 0) {
        slot = table + idx * 0x6C;
        HexDumpWords(gbase, slot, 8, &head);
        uint32_t subo0 = 0;
        if (skate3::native_scene::GuestTryCopy(
                &raw, gbase + slot + 0x30, 4)) {
          subo0 = __builtin_bswap32(raw);
        }
        uint32_t subo1 = 0;
        if (skate3::native_scene::GuestTryCopy(
                &raw, gbase + slot + 0x34, 4)) {
          subo1 = __builtin_bswap32(raw);
        }
        sub = fmt::format(" sub30={:#x} sub34={:#x}", subo0, subo1);
      }
    }
    REXLOG_INFO(
        "Skate 3 pass54: WALK-SLOT #{} pool={:#x} idx={} slot..={}{}",
        n, pool, idx, head, sub);
  }
  __imp__sub_8298E020(ctx, base);
}

// PASS 56: REG-POP — the registration-site hook at sub_8298DE78 (the
// find-context/population writer per pass-54's static close-out; single
// caller sub_8298D97C). Static-arg decode from the callee's disasm:
//   r28(arg1?) = the pool self; r30 = a device/lk pointer (arg2, used as
//   r24 = addis r4,1 -0x290 with -0x15bc deref for the slot count);
//   r31(arg3) = the register record: [r31+0] and +4 = entry sub-objects
//   (r29/r27 stored at slot+0x30/+0x34 via r27... it reconstructs:
//     lwz r29, 0(r31); lwz r27, 4(r31); lwz r26, 0x14(r31); lwz r25, 0x18(r31)
//   then record stores at slot+0x30..+0x38), [r31+8] and [r31+0xc...] =
//   counts, [r31+0x10] = length, [r31+0x14]/[r31+0x18] = a ptr+len pair.
// This hook prints all of it per fire (before the store), one line per
// REGISTRATION event; the pack's find-context creations arrive here in
// a boot independent of the walk variance.
extern "C" REX_FUNC(sub_8298DE78) {
  static std::atomic<uint64_t> count{0};
  const uint64_t n = count.fetch_add(1, std::memory_order_relaxed);
  if (REXCVAR_GET(skate3_loc_dump) && (n < 64 || (n & 0xFF) == 0)) {
    const uint32_t pool_self = ctx.r3.u32;
    const uint32_t dev = ctx.r4.u32;
    const uint32_t rec = ctx.r5.u32;
    uint32_t e0 = 0, e1 = 0, cnt8 = 0, cntc = 0, len10 = 0, p14 = 0, l18 = 0;
    uint32_t raw = 0;
    std::string p14s;
    if (uint8_t* gbase =
            skate3::native_scene::g_guest_base.load(std::memory_order_relaxed)) {
      auto rd = [&](uint32_t addr, uint32_t* out) {
        uint32_t r = 0;
        if (skate3::native_scene::GuestTryCopy(&r, gbase + addr, 4)) {
          *out = __builtin_bswap32(r);
        }
      };
      if (rec != 0) {
        rd(rec + 0, &e0);
        rd(rec + 4, &e1);
        rd(rec + 8, &cnt8);
        rd(rec + 0xc, &cntc);
        rd(rec + 0x10, &len10);
        rd(rec + 0x14, &p14);
        rd(rec + 0x18, &l18);
        if (p14 != 0 && l18 != 0 && l18 < 128) {
          char buf[128] = {};
          if (skate3::native_scene::GuestTryCopy(buf, gbase + p14,
                                                 sizeof(buf) - 1)) {
            p14s = buf;
          }
        }
      }
    }
    REXLOG_INFO(
        "Skate 3 pass56: REG-POP #{} self={:#x} dev={:#x} rec={:#x} "
        "e0={:#x} e1={:#x} c8={:#x} cc={:#x} len={:#x} p14={:#x} l18={:#x} "
        "str='{}'", n, pool_self, dev, rec, e0, e1, cnt8, cntc, len10, p14,
        l18, p14s);
  }
  __imp__sub_8298DE78(ctx, base);
  // PASS 56b: post-call sample — ctx.r3 = the constructed find-context
  // (the registered slot's identity!). One line per registration with
  // the produced object; its vtable says WHICH kind of find context got
  // built (the official packs' vs ours).
  {
    static std::atomic<uint64_t> post{0};
    const uint64_t m = post.fetch_add(1, std::memory_order_relaxed);
    if (REXCVAR_GET(skate3_loc_dump) && (m < 64 || (m & 0xFF) == 0)) {
      const uint32_t made = ctx.r3.u32;
      std::string vinfo;
      if (uint8_t* gbase = skate3::native_scene::g_guest_base.load(
              std::memory_order_relaxed)) {
        uint32_t raw = 0, vt = 0, m08 = 0;
        if (skate3::native_scene::GuestTryCopy(&raw, gbase + made, 4)) {
          vt = __builtin_bswap32(raw);
        }
        if (vt != 0 && skate3::native_scene::GuestTryCopy(
                           &raw, gbase + vt + 8, 4)) {
          m08 = __builtin_bswap32(raw);
        }
        vinfo = fmt::format(" made={:#x} vtbl={:#x} m08={:#x}", made, vt, m08);
      }
      REXLOG_INFO("Skate 3 pass56: REG-POP-MADE #{}{}", m, vinfo);
    }
  }
}

extern "C" REX_FUNC(sub_828D8B58) {
  static std::atomic<uint64_t> count{0};
  const uint64_t n = count.fetch_add(1, std::memory_order_relaxed);
  if (REXCVAR_GET(skate3_loc_dump) && (n < 8 || (n & 0xFF) == 0)) {
    const uint32_t a3 = ctx.r3.u32;
    const uint32_t dev = ctx.r4.u32;
    const uint32_t functor = ctx.r5.u32;
    std::string hex;
    if (uint8_t* gbase =
            skate3::native_scene::g_guest_base.load(std::memory_order_relaxed)) {
      HexDumpWords(gbase, functor, 4, &hex);
    }
    REXLOG_INFO(
        "Skate 3 pass46: MGR-REGISTER #{} mgr={:#x} dev={:#x} functor={:#x} "
        "functor..={}", n, a3, dev, functor, hex);
  }
  __imp__sub_828D8B58(ctx, base);
}

extern "C" REX_FUNC(sub_828D9510) {
  // PASS 67 (FIX-A): the pair-table dump's timed fallback — trigger 2.
  // This site sweeps ~120/s even at the menu, so ~2 s after the first
  // VAULTLIST-FOUND line the dump fires exactly once even when the found
  // burst ended before its 16th fire. Gated on the same
  // g_pair_dump_done flag; two atomic loads when idle.
  if (REXCVAR_GET(skate3_loc_dump) && !g_pair_dump_done.load(
          std::memory_order_relaxed)) {
    const int64_t t0 = g_found_first_ns.load(std::memory_order_relaxed);
    if (t0 != 0 && SteadyNsNow() - t0 > int64_t(2) * 1000 * 1000 * 1000) {
      static std::atomic<uint64_t> cb_fallback_count{0};
      const uint64_t m = cb_fallback_count.fetch_add(1,
                                                     std::memory_order_relaxed);
      MaybeDumpPairTable(m);
    }
  }
  static std::atomic<uint64_t> count{0};
  const uint64_t n = count.fetch_add(1, std::memory_order_relaxed);
  if (REXCVAR_GET(skate3_loc_dump) && (n < 16 || (n & 0x3FF) == 0)) {
    REXLOG_INFO("Skate 3 pass43: VAULTBIN-CB fired #{} r3={:#x} r4={:#x}",
                n, ctx.r3.u32, ctx.r4.u32);
  }
  __imp__sub_828D9510(ctx, base);
}

// ===========================================================================
// PASS 67 (FIX-A): the PAIR-TABLE FIND exit hook at sub_8290C548.
// Statically decoded this pass (tu3_disasm.sqlite, raw-word): the callee =
// the pair-table's grow/find primitive —
//   the count word [0x82FDE294] with the cap count+1 >= 0x64 (bail =
//   return -1 at 0x8290C6BC);
//   the first-empty walk over the deref'd pairs base [0x82FE84F4]
//   (stride-0x10 rows; the row-obj identity per the fresh-body decode =
//   {count, ptr 0x830F092C@4, fn 0x82E50D30@8, 0@0xC});
//   the match = the linear stride-8 cmp against the compared key.
// Hook contract: __imp__ FIRST (the callee writes its own r3), then
// sample ctx.r3 = the RETURN (the row-obj idx, or 0xFFFFFFFF on any bail).
// DECISIVE per the pass-66 plan: a −1 printed while our row object passes
// through = the membership gate PROVEN (→ FIX-B); an idx ≥ 0 whose
// register-fn triple shows the parse-fail exit = the parse gate (→
// FIX-C). Read-only, rate-capped (first 64 fires, then every 256th),
// UTC-stamped by the logging sink (skate3_loc_dump-gated, kHotReload).
// ===========================================================================
// PASS 67 (FIX-A): the PAIR-TABLE FIND exit hook at sub_8290C548.
// Statically decoded (tu3_disasm.sqlite, raw-word): the callee =
// the pair-table's grow/find primitive —
//   the count word [0x82FDE294] with the cap count+1 >= 0x64 (bail =
//   return -1 at 0x8290C6BC);
//   the first-empty walk over the deref'd rows base = the word at
//   [0x830684F4] (stride-0x10 rows; the row-obj identity per the
//   fresh-batch decode = {count, ptr 0x830F092C@4, fn 0x82E50D30@8,
//   0@0xC});
//   the match = the linear stride-8 cmp against the compared key.
// Hook contract: __imp__ FIRST (the callee writes its own r3), then
// sample ctx.r3 = the RESULT (the row-obj idx, or 0xFFFFFFFF on any
// bail). EVERY negative result is logged (rate-capped at the first 64
// misses, then every 256th miss) — a −1 while our row object passes
// through = the membership gate PROVEN (→ the FIX-B dep-pair path);
// non-negative results are sampled (first 32, then every 256th), and a
// run whose only misses come BEFORE the walk window (the boot burst) is
// distinguishable from one DURING it by the prompt's timestamps.
// Read-only, UTC-stamped by the logging sink (skate3_loc_dump-gated,
// kHotReload).
extern "C" REX_FUNC(sub_8290C548) {
  __imp__sub_8290C548(ctx, base);
  if (!REXCVAR_GET(skate3_loc_dump)) return;
  static std::atomic<uint64_t> count{0};
  static std::atomic<uint64_t> miss_count{0};
  const int32_t ret = static_cast<int32_t>(ctx.r3.u32);
  if (ret < 0) {
    const uint64_t m = miss_count.fetch_add(1, std::memory_order_relaxed);
    if (m < 64 || (m & 0xFF) == 0) {
      REXLOG_INFO("Skate 3 pass67: PAIRFIND #{} ret={:#x} (MISS — "
                  "candidate row misses the pair table)",
                  m, ctx.r3.u32);
    }
  } else {
    const uint64_t n = count.fetch_add(1, std::memory_order_relaxed);
    if (n < 32 || (n & 0xFF) == 0) {
      REXLOG_INFO("Skate 3 pass67: PAIRFIND #{} ret={:#x}", n, ctx.r3.u32);
    }
  }
}

// ===========================================================================
// PASS 38: the player/camera coordinate debug display (skate3::coord_dump).
//
// LIVES IN THIS TU (not a new CMake source) deliberately: editing
// CMakeLists.txt forces a CMake re-configure, which re-runs the TU-package
// staging (SKATE3_TITLE_UPDATE_PACKAGE, the title update cache
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
                    "to <app root>/OTS_candidates.json per press). Standalone "
                    "— the skater samples stamp with or without the HUD; "
                    "also armed while skate3_show_coords is on. ESC menu: "
                    "Interface section.");

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
    FILE* f = nullptr;
    uint64_t lines = 0;
    bool logged_open_fail = false;
    bool logged_start = false;  // announces the path on the first ACTUAL write
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
        if (!logged_start) {
          REXLOG_INFO("Skate 3 coord dump: logging to {}", path.string());
          logged_start = true;
        }
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

// Any armed consumer keeps the sample-stamping taps running (PASS 41): the
// OTS capture arm alone (skate3_coord_capture, without the HUD or the log)
// must still produce fresh samples for the F11 line — the bug report was
// captures with null pos when the coords HUD was off. Only the HUD DRAWING
// stays behind skate3_show_coords.
bool AnyConsumerArmed() {
  return REXCVAR_GET(skate3_show_coords) || REXCVAR_GET(skate3_coord_log) ||
         REXCVAR_GET(skate3_coord_capture);
}

void OnViewMatrix(uint8_t* base, uint32_t cam, uint32_t mtx, bool is_main) {
  if (!is_main || mtx == 0 || cam == 0) {
    return;
  }
  if (!AnyConsumerArmed()) {
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
  if (!AnyConsumerArmed()) {
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
  if (!AnyConsumerArmed()) {
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
