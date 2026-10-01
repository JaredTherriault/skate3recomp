#pragma once

// Player/camera coordinate debug display (PASS 38).
// IMPLEMENTATION: src/skate3_loc_dump.cpp (namespace skate3::coord_dump,
// appended PASS 38; kept in that TU so the CMakeLists source list — and
// therefore the CMake re-configure — stays untouched).
//
// Two outputs, both cvar-gated, default OFF:
//   (A) HUD  — skate3_show_coords (F7 toggles live): a small ImGui window
//       with the live player + camera position (top-left).
//   (B) LOG  — skate3_coord_log: appends timestamped x/y/z lines at ~4 Hz
//       to <app root>/coord_dump/skate3_coords.log (tail -f friendly).
//
// Data sources (existing, instruction-verified hooks in this repo; this
// module adds no new guest overrides, it is only tapped from them):
//   - PLAYER: the SkaterPresEntity per-frame taps in skate3_native_render.cpp
//     (sub_82783038 SkaterPresEntity::DoubleBuffer and sub_82785778, the
//     skater-family UBT+Pack vtable driver). r3 = the skater
//     PresentationEntity; its live world matrix m_MatLtoWTrans (+416,
//     16 floats, tail row (0,0,0,1), translation in component 3 of the
//     affine rows) is validated and its translation recorded. Verified
//     layout/fields per the native-entity identity store
//     (native/skate3_native_entity.cpp ReadWorldRowsChecked).
//   - CAMERA: the ViewCamera::SetViewMatrix override in
//     skate3_native_scene.cpp (sub_82802A00, r3 = ViewCamera, r4 = the
//     incoming view Matrix44). Position derived exactly like the native
//     scene builder does (cam = -t * R^T over the +0x20/+incoming view
//     matrix, row-vector convention). Only the MAIN render camera is
//     recorded (cam == the frame-build-published g_sampler_viewcam), so
//     shadow-cascade/portrait-RTT cameras never pollute the sample.
//
// HONEST LABELS: the "player" sample is the nearest-to-camera tracked
// SkaterPresEntity — NPCs of the same class are also tracked; when several
// are live the HUD/log pick the one closest to the camera (the camera
// follows the player). The camera sample IS the render camera (a few
// meters behind the skater), a navigation proxy, not the skater's own
// transform.
//
// The log write runs on a dedicated thread that only ever reads atomics —
// never on the guest thread (the synchronous-fwrite boot-hang lesson,
// src/skate3_loc_dump.cpp header).
//
// OTS candidate capture (PASS 40): the F11 hotkey (bind
// bind_skate3_ots_capture, armed when skate3_coord_capture or
// skate3_show_coords is on — both are ESC-menu items) snapshots the last
// known skater position (+ staleness flag), the render camera, the world
// district (the guest's own streamed DIST_<Name> chunk token, tracked by
// the VFS layer) and the nearest rendered item the game tracks around the
// skater, then appends ONE JSON line to <app root>/OTS_candidates.json
// (open/append/close per press, host thread). The touched-obstacle
// semantics (the physics contact pair) are NOT resolved — the nearest
// rendered item is a navigation proxy and is labelled as such; the
// touched-object recipe lives in analysis/xex/findings.md PASS 40.

#include <cstdint>

namespace rex::ui {
class ImGuiDrawer;
}

namespace skate3::coord_dump {

// From the ViewCamera::SetViewMatrix override (skate3_native_scene.cpp),
// AFTER the original call: is_main distinguishes the main render camera
// from the other ViewCameras. Reads the (possibly freecam-rewritten)
// incoming matrix the game just consumed.
void OnViewMatrix(uint8_t* base, uint32_t cam, uint32_t mtx, bool is_main);

// From the SkaterPresEntity per-frame hooks (skate3_native_render.cpp),
// with r3 = the skater PresentationEntity. Reads the live m_MatLtoWTrans
// world translation (guarded, structurally validated).
void OnSkaterEntity(uint8_t* base, uint32_t entity);

// HUD lifecycle: creates/toggles the dialog per skate3_show_coords and
// registers the F7 bind. Called once from Skate3BaseApp::OnCreateDialogs.
void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer);

// F11 OTS candidate capture: one JSON line to <app root>/OTS_candidates.json.
void CaptureOtsCandidate();

}  // namespace skate3::coord_dump
