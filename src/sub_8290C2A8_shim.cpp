// sub_8290C2A8_shim.cpp — PASS 69: the cvar-gated, key-independent
// per-pack-loc-vault REGISTRATION + ATTACH shim.
//
// THE DECODE (tu3_disasm.sqlite, pass 69; decode_reconstruction.md PASS 69)
//   PAIR-TABLE GATE:
//     count word [0x82FDE294]  = s32, -1 = empty (INIT), per-APPEND = max+1;
//     pairs word [0x830684F4]  = pointer to the PAIR rows, stride 8 =
//                                {key s32, val s32} (INIT = all {-1, 0});
//     gate  word [0x8306DCA4]  = pointer to the ENTRY rows, stride 0x10 =
//                                {word0, word1, word2 key, word3}
//                                (INIT = 100 zero slots, 0x640 B);
//     base-id word [0x83084110]= INIT value 2 (the observed officials'
//                                ENTRY word1).
//   THE WALKERS (sub_82D083D8, sub_82D08330) call sub_8290C2A8(rec, key)
//   with the key INDEPENDENT of the record bytes → byte-conformant pack
//   .vlt/.BIN is NOT required for the registration+ATTACH to succeed.
//   REGISTER/ATTACH sub_8290C2A8: signed find(rec-key) over the pairs →
//     byte-swap rec words 0/1 → dispatcher sub_8290D638(rec, pair_val).
//   DISPATCHER sub_8290D638: rec[0](BE) == 0x00039000 (the loc-DB magic) →
//     DEP-copy into the key's [8+i*0x1C] loc-DB row
//     (DEP.word2 = the row key @+0x10, DEP.word3 = rec[0], DEP.word4 = rec,
//     DEP.word0 = used-count++); ret 1.
//   APPEND sub_8290C548: gate-guarded, cap 0x64, key = max+1, ENTRY at the
//     first slot with word1 == 0, val = the slot idx, dispatch
//     m3(slot+0xC, key*0x1C) = the loc-DB setup fn.
//   REMOVE sub_8290C6C8 / CLEAR sub_8290C7D0: the count-- tail writes the
//     freed pair as {key, 0} (val 0 = the kill marker; INIT pairs are
//     {-1, 0}, never {k, 0} for k >= 0 while count >= 0).
//
// THE WINDOW DETECTOR: an ENTRY slot whose word2 key is (a) >= the INIT base
// id and (b) STILL VISIBLE in the live pair table → the officials'
// registrations; whose word2 key is visible ONLY as the table's TAIL row
// carrying val == 0 → the killed tail slot = the window (and the table)
// closed. No slot qualifies = no-op (honest miss, one info line per
// session max).
//
// THE SHIM: patch ENTRY+8 = the tail key, DEP+0xC = ENTRY, DEP+4 = 1, then
// call the GUEST dispatcher via the plain-C
// sub_8290C2A8_guest_dispatch(rec, ENTRY) (defined in skate3_loc_dump.cpp;
// = __imp__sub_8290D638 on a private guest stack; NO C++ ABI exposure —
// the shim C++ file links it as a plain-C symbol).
//
// PASS 69B ONE-SHOT CONTRACT (run-085 fix): the session one-shot is taken
// only AFTER every pre-flight guard has passed (this fire could actually
// act), and RELEASED on every skip path — so no-guest-base boot fires and
// pre-window misses never disarm the walk burst's fires. The rec-magic-miss
// path already had the release semantics (retry the next fire); every other
// skip now releases too (re spawning later fires as the window opens).
//
// C++ ABI SAFETY: every guest pointer read here goes through the SEH-guarded
// GuestTryCopy (the std::string reads of guest memory are the exception-
// unsafe patterns the shim was designed to avoid); the shim's own state =
// atomics only.
//
// Reentrant-safe: the shim sees only the FIRE trail of the guest's own
// walkers (sub_8290C2A8 hook entries) and the guest-side table mutations
// it observes are the walkers'/drains' own writes (already serialized on
// the guest thread); the dispatch call reuses the guest thread's own
// register context (no cross-thread table writes).
//
#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>

#include <fmt/format.h>
#include <rex/logging.h>
#include <rex/logging/macros.h>
#include <rex/ppc/context.h>

#include "native/skate3_native_guest_read.h"  // GuestTryCopy (SEH-guarded)
#include "skate3_native_scene_state.h"        // g_guest_base

// The C-style guest dispatch shim (the guest side; defined in
// skate3_loc_dump.cpp): the wrapper's entry into the guest register fn's
// dispatcher body WITHOUT crossing a C++ ABI boundary (plain C signature).
// rec = the record guest pointer, entry = the registry ENTRY guest pointer.
extern "C" void sub_8290C2A8_guest_dispatch(uint32_t rec, uint32_t entry);

namespace skate3::loc_dump {
namespace {

using skate3::native_scene::GuestTryCopy;

std::atomic<bool> g_shim_fired{false};  // session one-shot (PASS 69B contract)

constexpr uint32_t kCountWord = 0x82FDE294;   // s32: -1 empty, per-APPEND max+1
constexpr uint32_t kPairsWord = 0x830684F4;   // ptr: stride-8 {key, val} rows
constexpr uint32_t kGateWord = 0x8306DCA4;    // ptr: stride-0x10 ENTRY rows
constexpr uint32_t kBaseIdWord = 0x83084110;  // INIT value 2 (the officials' ENTRY word1)
constexpr uint32_t kMaxEntrySlots = 0x100;    // INIT: 100 x 0x10 = 0x640 B
constexpr uint32_t kMaxPairRows = 0x100;      // INIT: 100 x 8 = 0x320 B
constexpr uint32_t kMinSaneKey = 2;           // >= the INIT base id
constexpr uint32_t kLocDbMagic = 0x00039000;  // BE rec[0]

bool Rd32(uint8_t* gbase, uint32_t addr, uint32_t* out) {
  uint32_t raw = 0;
  if (!GuestTryCopy(&raw, gbase + addr, 4)) {
    // PASS 69B: a transient SEH read failure = the guest base was pulled
    // out from under the shim; release the one-shot so a later fire retries.
    g_shim_fired.store(false, std::memory_order_relaxed);
    return false;
  }
  *out = __builtin_bswap32(raw);
  return true;
}

bool Wr32(uint8_t* gbase, uint32_t addr, uint32_t val) {
  uint32_t raw = __builtin_bswap32(val);
  const bool ok = GuestTryCopy(&raw, gbase + addr, 4);  // GuestTryCopy writes as well as reads
  if (!ok) {
    g_shim_fired.store(false, std::memory_order_relaxed);  // PASS 69B: release; retry on a later fire
  }
  return ok;
}

}  // namespace

// The shim entry: called from the sub_8290C2A8 REX_FUNC hook (skate3_loc_dump.cpp)
// once per fire while the cvar is on. The session one-shot is taken ONLY after
// the pre-flight guards pass (PASS 69B), and released on every skip path so a
// later fire (the walk burst, our only chance to act) retries.
void Skate3RegisterCustomPackShim(uint32_t rec_guest) {
  // Pre-flight 1: the guest base. If not up yet, do NOT take the one-shot.
  uint8_t* gbase = skate3::native_scene::g_guest_base.load(
      std::memory_order_relaxed);
  if (!gbase) {
    REXLOG_INFO("Skate 3 pass69 SHIM: no guest base yet; skipped (retryable)");
    return;
  }

  // Pre-flight 2: the live globals.
  uint32_t count = 0, pairs = 0, gate = 0, base_id = 0;
  if (!Rd32(gbase, kCountWord, &count)) {
    REXLOG_INFO("Skate 3 pass69 SHIM: count word unreadable; skipped (retryable)");
    return;
  }
  if (!Rd32(gbase, kPairsWord, &pairs) || pairs == 0) {
    REXLOG_INFO("Skate 3 pass69 SHIM: pairs word unreadable/zero; skipped (retryable)");
    return;
  }
  if (!Rd32(gbase, kGateWord, &gate) || gate == 0) {
    REXLOG_INFO("Skate 3 pass69 SHIM: gate word unreadable/zero; skipped (retryable)");
    return;
  }

  // All pre-flight guards passed: this fire could actually act — take the
  // session one-shot now.
  if (g_shim_fired.exchange(true, std::memory_order_relaxed)) {
    return;  // already acted once this session (the success path keeps it)
  }
  Rd32(gbase, kBaseIdWord, &base_id);  // optional: the INIT base id (sanity)
  REXLOG_INFO("Skate 3 pass69 SHIM: rec={:#x} count={} pairs={:#x} gate={:#x} "
              "base_id={:#x}",
              rec_guest, static_cast<int32_t>(count), pairs, gate, base_id);

  // Sanity guards (the honest no-op path — release the one-shot too).
  if (static_cast<int32_t>(count) < 0) {
    REXLOG_INFO("Skate 3 pass69 SHIM: pair table empty (count={}); no "
                "window; skipped (retryable)",
                static_cast<int32_t>(count));
    g_shim_fired.store(false, std::memory_order_relaxed);
    return;
  }
  if (rec_guest < 0x10000) {
    REXLOG_INFO("Skate 3 pass69 SHIM: rec pointer {:#x} not sane; skipped (retryable)",
                rec_guest);
    g_shim_fired.store(false, std::memory_order_relaxed);
    return;
  }

  // (a) Find the first ENTRY slot whose word2 key is STILL VISIBLE in the
  //     live pair table (the officials' window still open).
  // (b) Find the killed-tail slot: word2 key visible ONLY as the table's
  //     TAIL row with val == 0 (the REMOVE's {key, 0} marker = the window
  //     closed).
  const uint32_t n_pairs =
      static_cast<uint32_t>(static_cast<int32_t>(count)) + 1;  // live rows
  if (n_pairs == 0 || n_pairs > kMaxPairRows) {
    REXLOG_INFO("Skate 3 pass69 SHIM: pair count {} out of band; skipped (retryable)",
                n_pairs);
    g_shim_fired.store(false, std::memory_order_relaxed);
    return;
  }

  // Snapshot the live pair table (bounded; SEH-guarded).
  struct PairRow {
    uint32_t key, val;
  };
  PairRow rows[kMaxPairRows];
  std::memset(rows, 0, sizeof(rows));
  for (uint32_t i = 0; i < n_pairs; ++i) {
    if (!Rd32(gbase, pairs + i * 8, &rows[i].key) ||
        !Rd32(gbase, pairs + i * 8 + 4, &rows[i].val)) {
      REXLOG_INFO("Skate 3 pass69 SHIM: pair row {} unreadable; skipped (retryable)", i);
      g_shim_fired.store(false, std::memory_order_relaxed);
      return;
    }
  }

  // Walk the ENTRY array (stride 0x10, cap kMaxEntrySlots).
  uint32_t found_idx = 0xFFFFFFFF;      // (a): the officials' live window
  uint32_t found_key = 0;
  uint32_t tail_idx = 0xFFFFFFFF;       // (b): the killed-tail slot
  uint32_t tail_key = 0;
  for (uint32_t i = 0; i < kMaxEntrySlots; ++i) {
    uint32_t w2 = 0;
    if (!Rd32(gbase, gate + i * 0x10 + 8, &w2)) break;  // array end / unreadable
    if (w2 < kMinSaneKey) continue;
    bool visible = false, is_tail = false, tail_val_zero = false;
    for (uint32_t j = 0; j < n_pairs; ++j) {
      if (rows[j].key == w2) {
        visible = true;
        if (j == n_pairs - 1) {
          is_tail = true;
          tail_val_zero = (rows[j].val == 0);
        }
      }
    }
    if (visible) {
      // Prefer a NON-tail visible key (the officials' window still open —
      // the shim must not run before the walk closes).
      if (!is_tail || !tail_val_zero) {
        found_idx = i;
        found_key = w2;
        break;
      }
      // A tail key with val==0 = the killed-tail slot (candidate (b)) —
      // keep scanning (a later slot may still be live); record the last one.
      tail_idx = i;
      tail_key = w2;
    }
  }

  uint32_t shim_idx = 0xFFFFFFFF, shim_key = 0;
  if (found_idx != 0xFFFFFFFF) {
    // (a) The officials' window still open: DON'T run (the officials' own
    //     registrations haven't finished; running now would sit inside the
    //     officials' burst). Report and skip (release the one-shot — a
    //     later fire after the window closes must retry).
    REXLOG_INFO("Skate 3 pass69 SHIM: officials' window still open "
                "(entry {} key {:#x} live); NOT running yet — wait for the "
                "walk to close",
                found_idx, found_key);
    g_shim_fired.store(false, std::memory_order_relaxed);
    return;
  }
  if (tail_idx != 0xFFFFFFFF) {
    shim_idx = tail_idx;
    shim_key = tail_key;
  } else {
    REXLOG_INFO("Skate 3 pass69 SHIM: no qualifying ENTRY slot found; skipped (retryable)");
    g_shim_fired.store(false, std::memory_order_relaxed);
    return;
  }

  // The ENTRY slot's DEP record (INIT-valued: +0xC = 0, +4 = 0).
  const uint32_t entry_addr = gate + shim_idx * 0x10;
  uint32_t dep = 0, dep4 = 0, w0 = 0, w1 = 0;
  if (!Rd32(gbase, entry_addr + 0xC, &dep) || dep == 0) {
    REXLOG_INFO("Skate 3 pass69 SHIM: ENTRY {:#x} +0xC DEP ptr = 0 "
                "(not INIT-valued); skipped (retryable)",
                entry_addr);
    g_shim_fired.store(false, std::memory_order_relaxed);
    return;
  }
  if (!Rd32(gbase, dep + 4, &dep4) || dep4 != 0) {
    REXLOG_INFO("Skate 3 pass69 SHIM: DEP {:#x} +4 = {:#x} (not 0 = the "
                "INIT shape); skipped (retryable)",
                dep, dep4);
    g_shim_fired.store(false, std::memory_order_relaxed);
    return;
  }
  Rd32(gbase, entry_addr + 0, &w0);
  Rd32(gbase, entry_addr + 4, &w1);

  // Patch: ENTRY+8 = the tail key, DEP+0xC = ENTRY, DEP+4 = 1.
  if (!Wr32(gbase, entry_addr + 8, shim_key) ||
      !Wr32(gbase, dep + 0xC, entry_addr) ||
      !Wr32(gbase, dep + 4, 1)) {
    REXLOG_INFO("Skate 3 pass69 SHIM: ENTRY/DEP patch write failed; skipped (retryable)");
    g_shim_fired.store(false, std::memory_order_relaxed);
    return;
  }

  // THE HARD GUARD (the dispatcher's own): rec[0] (BE) must be the loc-DB
  // image magic 0x00039000 — otherwise the ATTACH CANNOT succeed (ret 0).
  // NOTE (PASS 69B): the rec-magic check is done BEFORE the patch from now
  // on-preference; the release below keeps the original retry semantics.
  uint32_t rec_magic = 0;
  Rd32(gbase, rec_guest, &rec_magic);
  if (rec_magic != kLocDbMagic) {
    REXLOG_INFO("Skate 3 pass69 SHIM: rec {:#x} word0 = {:#x} != the loc-DB "
                "magic 0x00039000 — the dispatcher would reject it; skipped "
                "(the NEXT fire's record gets tried)",
                rec_guest, rec_magic);
    g_shim_fired.store(false, std::memory_order_relaxed);  // retry next fire
    return;
  }
  REXLOG_INFO("Skate 3 pass69 SHIM: entry {:#x} (idx {}, key {:#x}, w0={:#x} "
              "w1={:#x}), dep {:#x}, rec {:#x} — dispatching",
              entry_addr, shim_idx, shim_key, w0, w1, dep, rec_guest);

  // The dispatch (the plain-C shim in skate3_loc_dump.cpp): r3 = rec,
  // r4 = ENTRY. This runs the guest dispatcher (sub_8290D638) directly —
  // the ATTACH (no C++ ABI exposure, the reentrant-safe shape). The one-shot
  // stays taken: the dispatch HAPPENED.
  sub_8290C2A8_guest_dispatch(rec_guest, entry_addr);

  // Post-dispatch state (the dispatcher may have modified the rec pointer —
  // read what it left).
  REXLOG_INFO("Skate 3 pass69 SHIM: dispatch returned");
  REXLOG_INFO("Skate 3 pass69 SHIM: next boot = the officials' walk + the "
              "shim-registered record (the count word reads the tail key)");
}

}  // namespace skate3::loc_dump
