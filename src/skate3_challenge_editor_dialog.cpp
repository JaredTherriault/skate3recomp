#include "skate3_challenge_editor_dialog.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <memory>
#include <string>
#include <vector>

#include <imgui.h>
#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/filesystem/vfs.h>
#include <rex/logging.h>

#include "../third_party/rexglue-sdk/thirdparty/tomlplusplus/vendor/json.hpp"
#include "skate3_native_scene.h"

using json = nlohmann::json;

namespace skate3 {

namespace {

constexpr float kPi = 3.14159265358979323846f;

const char* const kDistrictNames[] = {
    "university",
    "downtown",
    "industrial",
    "dannyway",
    "maloof",
    "artgallery",
};
constexpr int kDistrictCount = sizeof(kDistrictNames) / sizeof(kDistrictNames[0]);

const char* const kDistrictDisplayNames[] = {
    "University District",
    "Downtown District",
    "Industrial District",
    "Danny Way Hawaiian Dream",
    "Maloof Money Cup",
    "Art Gallery",
};

const char* const kCategoryNames[] = {
    "Street",
    "Tranny",
    "Park",
};
constexpr int kCategoryCount = sizeof(kCategoryNames) / sizeof(kCategoryNames[0]);

float NormalizeStick(int16_t val, int16_t deadzone) {
  if (std::abs(val) <= deadzone) {
    return 0.0f;
  }
  if (val > 0) {
    return float(val - deadzone) / float(32767 - deadzone);
  }
  return float(val + deadzone) / float(32767 - deadzone);
}

// Projects a 3D world position to 2D screen coordinates using row-vector matrix conventions.
bool Project3D(const float pos[3], const float vp[16], const float proj[16],
               float disp_w, float disp_h, ImVec2& out_screen, float* out_depth = nullptr) {
  const float x = pos[0], y = pos[1], z = pos[2];
  const float clip_x = x * vp[0] + y * vp[4] + z * vp[8] + vp[12];
  const float clip_y = x * vp[1] + y * vp[5] + z * vp[9] + vp[13];
  const float clip_z = x * vp[2] + y * vp[6] + z * vp[10] + vp[14];
  const float clip_w = x * vp[3] + y * vp[7] + z * vp[11] + vp[15];

  if (clip_w <= 0.05f || clip_z < 0.0f) {
    return false;
  }

  const float ndc_x = clip_x / clip_w;
  const float ndc_y = clip_y / clip_w;

  // Projection X scale sign handles inverted horizontal axes
  const float sign_x = (proj && proj[0] < 0.0f) ? -1.0f : 1.0f;

  out_screen.x = (0.5f + 0.5f * sign_x * ndc_x) * disp_w;
  out_screen.y = (0.5f - 0.5f * ndc_y) * disp_h;

  if (out_depth) {
    *out_depth = clip_w;
  }
  return true;
}

void Draw3DRing(ImDrawList* dl, const float center[3], float radius, ImU32 color,
                const float vp[16], const float proj[16], float disp_w, float disp_h,
                int segments = 32, float thickness = 2.5f) {
  std::vector<ImVec2> pts;
  pts.reserve(segments);
  for (int i = 0; i < segments; ++i) {
    const float angle = (float(i) / float(segments)) * (2.0f * kPi);
    const float p[3] = {center[0] + radius * std::cos(angle), center[1],
                        center[2] + radius * std::sin(angle)};
    ImVec2 scr;
    if (Project3D(p, vp, proj, disp_w, disp_h, scr)) {
      pts.push_back(scr);
    }
  }
  if (pts.size() >= 3) {
    dl->AddPolyline(pts.data(), int(pts.size()), color, ImDrawFlags_Closed, thickness);
  }
}

void Draw3DBeacon(ImDrawList* dl, const float base_pos[3], float height, ImU32 color,
                  const float vp[16], const float proj[16], float disp_w, float disp_h,
                  const char* label = nullptr) {
  const float top_pos[3] = {base_pos[0], base_pos[1] + height, base_pos[2]};
  ImVec2 p_base, p_top;
  if (!Project3D(base_pos, vp, proj, disp_w, disp_h, p_base) ||
      !Project3D(top_pos, vp, proj, disp_w, disp_h, p_top)) {
    return;
  }

  // Draw vertical beam glow
  const ImU32 glow_col = (color & 0x00FFFFFF) | 0x44000000;
  dl->AddLine(p_base, p_top, glow_col, 6.0f);
  dl->AddLine(p_base, p_top, color, 2.5f);

  // Top diamond cap
  constexpr float kDiamondSize = 7.0f;
  const ImVec2 diamond[4] = {
      ImVec2(p_top.x, p_top.y - kDiamondSize),
      ImVec2(p_top.x + kDiamondSize, p_top.y),
      ImVec2(p_top.x, p_top.y + kDiamondSize),
      ImVec2(p_top.x - kDiamondSize, p_top.y),
  };
  dl->AddConvexPolyFilled(diamond, 4, color);
  dl->AddPolyline(diamond, 4, IM_COL32(255, 255, 255, 220), ImDrawFlags_Closed, 1.5f);

  if (label && label[0] != '\0') {
    const ImVec2 text_size = ImGui::CalcTextSize(label);
    const ImVec2 badge_center(p_top.x, p_top.y - 18.0f);
    const ImVec2 badge_min(badge_center.x - text_size.x * 0.5f - 8.0f,
                           badge_center.y - text_size.y * 0.5f - 4.0f);
    const ImVec2 badge_max(badge_center.x + text_size.x * 0.5f + 8.0f,
                           badge_center.y + text_size.y * 0.5f + 4.0f);

    dl->AddRectFilled(badge_min, badge_max, IM_COL32(20, 20, 25, 220), 4.0f);
    dl->AddRect(badge_min, badge_max, color, 4.0f, 0, 1.5f);
    dl->AddText(ImVec2(badge_min.x + 8.0f, badge_min.y + 4.0f), IM_COL32(255, 255, 255, 255),
                label);
  }
}

}  // namespace

ChallengeEditorDialog::ChallengeEditorDialog(rex::ui::ImGuiDrawer* drawer,
                                             GamepadPollFn gamepad_poll)
    : ImGuiDialog(drawer), gamepad_poll_(std::move(gamepad_poll)) {
  RefreshDiscoveredPacks();
  SetDrawActive(false);
}

ChallengeEditorDialog::~ChallengeEditorDialog() {
  if (visible_) {
    Hide();
  }
}

void ChallengeEditorDialog::Show() {
  if (visible_) {
    return;
  }
  visible_ = true;
  SetDrawActive(true);
  RefreshDiscoveredPacks();

  // Remember if freecam was active, then activate it
  was_freecam_active_before_ = native_scene::IsFreecamActive();
  native_scene::SetFreecamActive(true);

  // Seed district from camera position
  float cam_pos[3], cam_fwd[3];
  if (native_scene::GetFreecamPose(cam_pos, cam_fwd) ||
      native_scene::FreecamGuestPose(cam_pos)) {
    AutoDetectDistrict(cam_pos);
  }

  status_message_ = "Drone Flight active. Place markers with Gamepad or UI.";
  status_timer_ = 4.0f;
  REXLOG_INFO("ChallengeEditorDialog: Opened. Freecam engaged.");
}

void ChallengeEditorDialog::Hide() {
  if (!visible_) {
    return;
  }
  visible_ = false;
  SetDrawActive(false);

  // Restore freecam state to what it was before opening
  native_scene::SetFreecamActive(was_freecam_active_before_);

  // Clear gamepad inputs
  native_scene::FreecamGamepadInput zero_input;
  native_scene::SetFreecamGamepadInput(zero_input);

  REXLOG_INFO("ChallengeEditorDialog: Closed. Freecam restored to {}.",
              was_freecam_active_before_ ? "active" : "inactive");
}

void ChallengeEditorDialog::Toggle() {
  if (visible_) {
    Hide();
  } else {
    Show();
  }
}

void ChallengeEditorDialog::RefreshDiscoveredPacks() {
  discovered_packs_.clear();
  const auto app_root = rex::filesystem::GetAppRootFolder();
  const std::vector<std::filesystem::path> roots = {
      app_root / "mods" / "raw",
      app_root.parent_path() / "mods" / "raw",
      std::filesystem::current_path() / "mods" / "raw",
  };

  for (const auto& root : roots) {
    if (!std::filesystem::exists(root) || !std::filesystem::is_directory(root)) {
      continue;
    }
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(root, ec)) {
      if (entry.is_directory(ec)) {
        const auto manifest = entry.path() / "pack.json";
        if (std::filesystem::exists(manifest, ec)) {
          const std::string name = entry.path().filename().string();
          if (std::find(discovered_packs_.begin(), discovered_packs_.end(), name) ==
              discovered_packs_.end()) {
            discovered_packs_.push_back(name);
          }
        }
      }
    }
  }

  if (discovered_packs_.empty()) {
    discovered_packs_.push_back("west_coast_ots");
  }

  // Sync draft pack_id
  if (selected_pack_index_ >= 0 && selected_pack_index_ < int(discovered_packs_.size())) {
    snprintf(draft_.pack_id, sizeof(draft_.pack_id), "%s",
             discovered_packs_[selected_pack_index_].c_str());
  }
}

void ChallengeEditorDialog::AutoDetectDistrict(const float drone_pos[3]) {
  // First check streamed district token from VFS
  const std::string vfs_dist = rex::filesystem::dist_track::LastDistrict();
  if (!vfs_dist.empty()) {
    std::string lower = vfs_dist;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    for (int i = 0; i < kDistrictCount; ++i) {
      if (lower.find(kDistrictNames[i]) != std::string::npos) {
        draft_.district_index = i;
        return;
      }
    }
  }

  // Coordinate bounding-box heuristics
  const float x = drone_pos[0];
  const float y = drone_pos[1];
  const float z = drone_pos[2];

  if (y < -5.0f && x > 50.0f) {
    draft_.district_index = 3;  // Danny Way
  } else if (x >= -120.0f && x <= 40.0f && y >= -5.0f && y <= 25.0f && z >= -60.0f && z <= 60.0f) {
    draft_.district_index = 4;  // Maloof
  } else if (y > 35.0f && x >= 40.0f && x <= 200.0f && z >= -80.0f && z <= 100.0f) {
    draft_.district_index = 5;  // Art Gallery
  } else if (z < -350.0f) {
    draft_.district_index = 2;  // Industrial
  } else if (x < 150.0f && z > 20.0f) {
    draft_.district_index = 1;  // Downtown
  } else {
    draft_.district_index = 0;  // University fallback
  }
}

void ChallengeEditorDialog::ComputeAimTarget(const float drone_pos[3], const float drone_fwd[3],
                                            float out_target[3]) {
  // Default forward projection
  float t = 10.0f;

  // If drone points downward, project to approximate ground plane
  if (drone_fwd[1] < -0.05f) {
    // Ground plane estimate: 3.5m below current drone height or draft site height
    const float ground_y = draft_.has_site ? draft_.site[1] : (drone_pos[1] - 3.5f);
    t = (ground_y - drone_pos[1]) / drone_fwd[1];
    t = std::clamp(t, 2.0f, 60.0f);
  }

  out_target[0] = drone_pos[0] + drone_fwd[0] * t;
  out_target[1] = drone_pos[1] + drone_fwd[1] * t;
  out_target[2] = drone_pos[2] + drone_fwd[2] * t;
}

void ChallengeEditorDialog::ProcessInput(ImGuiIO& io, const float drone_pos[3],
                                       const float aim_target[3]) {
  uint16_t cur_buttons = 0;
  native_scene::FreecamGamepadInput gp_in;

  if (gamepad_poll_) {
    rex::input::X_INPUT_GAMEPAD pad;
    if (gamepad_poll_(&pad)) {
      cur_buttons = pad.buttons;

      // Sticks
      constexpr int16_t kDeadzoneL = 7849;
      constexpr int16_t kDeadzoneR = 8689;
      gp_in.move_r = NormalizeStick(pad.thumb_lx, kDeadzoneL);
      gp_in.move_f = NormalizeStick(pad.thumb_ly, kDeadzoneL);
      gp_in.look_yaw = NormalizeStick(pad.thumb_rx, kDeadzoneR);
      gp_in.look_pitch = NormalizeStick(pad.thumb_ry, kDeadzoneR);

      // Triggers for vertical elevation
      const float rt = float(pad.right_trigger) / 255.0f;
      const float lt = float(pad.left_trigger) / 255.0f;
      gp_in.move_u = rt - lt;

      // Boost
      if ((pad.buttons & rex::input::X_INPUT_GAMEPAD_LEFT_THUMB) ||
          (pad.buttons & rex::input::X_INPUT_GAMEPAD_LEFT_SHOULDER)) {
        gp_in.speed_mult = 3.5f;
      }
    }
  }

  // Keyboard navigation when not typing in text inputs
  if (!io.WantTextInput) {
    if (ImGui::IsKeyDown(ImGuiKey_W)) gp_in.move_f += 1.0f;
    if (ImGui::IsKeyDown(ImGuiKey_S)) gp_in.move_f -= 1.0f;
    if (ImGui::IsKeyDown(ImGuiKey_D)) gp_in.move_r += 1.0f;
    if (ImGui::IsKeyDown(ImGuiKey_A)) gp_in.move_r -= 1.0f;
    if (ImGui::IsKeyDown(ImGuiKey_E) || ImGui::IsKeyDown(ImGuiKey_Space)) gp_in.move_u += 1.0f;
    if (ImGui::IsKeyDown(ImGuiKey_Q) || ImGui::IsKeyDown(ImGuiKey_C)) gp_in.move_u -= 1.0f;
    if (ImGui::IsKeyDown(ImGuiKey_LeftShift)) gp_in.speed_mult = 3.5f;
  }

  // Forward movement intents to freecam
  native_scene::SetFreecamGamepadInput(gp_in);

  // Edge-triggered gamepad buttons
  const uint16_t pressed = cur_buttons & ~prev_buttons_;
  prev_buttons_ = cur_buttons;

  // (X) = Set Obstacle Site
  if ((pressed & rex::input::X_INPUT_GAMEPAD_X) ||
      (!io.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_1, false))) {
    std::memcpy(draft_.site, aim_target, sizeof(draft_.site));
    draft_.has_site = true;
    status_message_ = "Obstacle (Site) placed!";
    status_timer_ = 3.0f;
  }

  // (Y) = Set Drop-in Start
  if ((pressed & rex::input::X_INPUT_GAMEPAD_Y) ||
      (!io.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_2, false))) {
    std::memcpy(draft_.start, aim_target, sizeof(draft_.start));
    draft_.has_start = true;
    status_message_ = "Drop-in Start placed!";
    status_timer_ = 3.0f;
  }

  // (RB) = Capture Camera Transform
  if ((pressed & rex::input::X_INPUT_GAMEPAD_RIGHT_SHOULDER) ||
      (!io.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_3, false))) {
    std::memcpy(draft_.cam, drone_pos, sizeof(draft_.cam));
    draft_.has_cam = true;
    status_message_ = "Flythrough Camera captured!";
    status_timer_ = 3.0f;
  }

  // (BACK) or C = Clear
  if ((pressed & rex::input::X_INPUT_GAMEPAD_BACK) ||
      (!io.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_C, false))) {
    draft_.has_site = false;
    draft_.has_start = false;
    draft_.has_cam = false;
    status_message_ = "Markers cleared.";
    status_timer_ = 2.0f;
  }

  // (B) or Esc = Exit
  if ((pressed & rex::input::X_INPUT_GAMEPAD_B) ||
      (!io.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_Escape, false))) {
    Hide();
  }
}

void ChallengeEditorDialog::Render3DWorld(ImDrawList* dl, const float drone_pos[3],
                                         const float aim_target[3]) {
  const auto scene = native_scene::LastFrameScene();
  if (!scene) {
    return;
  }

  const ImGuiIO& io = ImGui::GetIO();
  const float dw = io.DisplaySize.x;
  const float dh = io.DisplaySize.y;
  const float* vp = scene->view_proj;
  const float* proj = scene->proj;

  // 1. Aim Reticle on Ground
  {
    const float pulse = 0.5f + 0.5f * std::sin(anim_time_ * 5.0f);
    const float reticle_r = 0.8f + 0.2f * pulse;
    Draw3DRing(dl, aim_target, reticle_r, IM_COL32(0, 220, 255, 230), vp, proj, dw, dh, 32, 2.0f);
    Draw3DRing(dl, aim_target, reticle_r * 0.4f, IM_COL32(255, 255, 255, 180), vp, proj, dw, dh, 16,
               1.5f);

    ImVec2 center_scr;
    if (Project3D(aim_target, vp, proj, dw, dh, center_scr)) {
      dl->AddCircleFilled(center_scr, 3.5f, IM_COL32(0, 255, 255, 255));
    }
  }

  // 2. Obstacle (Site) Marker (RED)
  if (draft_.has_site) {
    Draw3DRing(dl, draft_.site, 1.25f, IM_COL32(255, 40, 40, 240), vp, proj, dw, dh, 32, 3.0f);
    Draw3DBeacon(dl, draft_.site, 6.0f, IM_COL32(255, 50, 50, 255), vp, proj, dw, dh,
                 "OBSTACLE (SITE)");
  }

  // 3. Drop-in Start Marker (GREEN)
  if (draft_.has_start) {
    Draw3DRing(dl, draft_.start, 1.0f, IM_COL32(40, 255, 60, 240), vp, proj, dw, dh, 32, 3.0f);
    Draw3DBeacon(dl, draft_.start, 5.0f, IM_COL32(50, 255, 70, 255), vp, proj, dw, dh,
                 "DROP-IN START");
  }

  // 4. Runway Guide Line & Distance Badge (GOLDEN)
  if (draft_.has_site && draft_.has_start) {
    ImVec2 p_start, p_site;
    if (Project3D(draft_.start, vp, proj, dw, dh, p_start) &&
        Project3D(draft_.site, vp, proj, dw, dh, p_site)) {
      const float dx = draft_.site[0] - draft_.start[0];
      const float dy = draft_.site[1] - draft_.start[1];
      const float dz = draft_.site[2] - draft_.start[2];
      const float dist_m = std::sqrt(dx * dx + dy * dy + dz * dz);

      // Main runway line
      dl->AddLine(p_start, p_site, IM_COL32(255, 215, 0, 100), 7.0f);
      dl->AddLine(p_start, p_site, IM_COL32(255, 235, 50, 240), 2.5f);

      // Distance badge at midpoint
      const float mid[3] = {(draft_.start[0] + draft_.site[0]) * 0.5f,
                            (draft_.start[1] + draft_.site[1]) * 0.5f + 0.8f,
                            (draft_.start[2] + draft_.site[2]) * 0.5f};
      ImVec2 p_mid;
      if (Project3D(mid, vp, proj, dw, dh, p_mid)) {
        char dist_buf[48];
        snprintf(dist_buf, sizeof(dist_buf), "Runway: %.1f m", dist_m);
        const ImVec2 text_sz = ImGui::CalcTextSize(dist_buf);
        const ImVec2 b_min(p_mid.x - text_sz.x * 0.5f - 6.0f, p_mid.y - text_sz.y * 0.5f - 4.0f);
        const ImVec2 b_max(p_mid.x + text_sz.x * 0.5f + 6.0f, p_mid.y + text_sz.y * 0.5f + 4.0f);

        dl->AddRectFilled(b_min, b_max, IM_COL32(25, 25, 30, 220), 4.0f);
        dl->AddRect(b_min, b_max, IM_COL32(255, 215, 0, 240), 4.0f, 0, 1.5f);
        dl->AddText(ImVec2(b_min.x + 6.0f, b_min.y + 4.0f), IM_COL32(255, 235, 50, 255),
                    dist_buf);
      }
    }
  }

  // 5. Intro Flythrough Camera Marker (CYAN)
  if (draft_.has_cam) {
    Draw3DBeacon(dl, draft_.cam, 3.5f, IM_COL32(0, 210, 255, 255), vp, proj, dw, dh,
                 "INTRO CAM");
  }
}

void ChallengeEditorDialog::RenderUIWindow() {
  ImGui::SetNextWindowSize(ImVec2(380, 560), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowPos(ImVec2(24, 24), ImGuiCond_FirstUseEver);

  const ImGuiWindowFlags win_flags = ImGuiWindowFlags_NoCollapse;
  if (ImGui::Begin("Own-The-Spot Challenge Editor", nullptr, win_flags)) {
    // Status banner
    if (status_timer_ > 0.0f && !status_message_.empty()) {
      ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 225, 60, 255));
      ImGui::TextWrapped(">> %s", status_message_.c_str());
      ImGui::PopStyleColor();
      ImGui::Separator();
    }

    // 1. Mod Pack Selector
    if (ImGui::CollapsingHeader("Target Mod Pack", ImGuiTreeNodeFlags_DefaultOpen)) {
      if (ImGui::BeginCombo("Pack", draft_.pack_id)) {
        for (int i = 0; i < int(discovered_packs_.size()); ++i) {
          const bool is_selected = (selected_pack_index_ == i);
          if (ImGui::Selectable(discovered_packs_[i].c_str(), is_selected)) {
            selected_pack_index_ = i;
            snprintf(draft_.pack_id, sizeof(draft_.pack_id), "%s",
                     discovered_packs_[i].c_str());
          }
          if (is_selected) {
            ImGui::SetItemDefaultFocus();
          }
        }
        ImGui::EndCombo();
      }
      ImGui::InputText("Custom Pack ID", draft_.pack_id, sizeof(draft_.pack_id));
    }

    // 2. Challenge Info
    if (ImGui::CollapsingHeader("Challenge Details", ImGuiTreeNodeFlags_DefaultOpen)) {
      ImGui::InputText("Title", draft_.title, sizeof(draft_.title));

      // Auto-generate key if title changes and key is empty
      if (draft_.key[0] == '\0' && draft_.title[0] != '\0') {
        std::string slug = draft_.title;
        std::transform(slug.begin(), slug.end(), slug.begin(), [](char c) {
          return std::isalnum(c) ? char(std::tolower(c)) : '_';
        });
        snprintf(draft_.key, sizeof(draft_.key), "%s", slug.c_str());
      }
      ImGui::InputText("Unique Key", draft_.key, sizeof(draft_.key));
      ImGui::InputTextMultiline("Description", draft_.desc, sizeof(draft_.desc),
                                ImVec2(-1, 55));

      ImGui::Combo("District", &draft_.district_index, kDistrictDisplayNames, kDistrictCount);
      ImGui::Combo("Category", &draft_.category_index, kCategoryNames, kCategoryCount);

      ImGui::DragInt("Own Score", &draft_.own_score, 50, 100, 10000);
      ImGui::DragInt("Kill Score", &draft_.kill_score, 50, 200, 20000);
    }

    // 3. Markers & Placement
    if (ImGui::CollapsingHeader("Spot Markers", ImGuiTreeNodeFlags_DefaultOpen)) {
      // Obstacle Site
      if (draft_.has_site) {
        ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "[SET] Obstacle:");
        ImGui::SameLine();
        ImGui::Text("(%.1f, %.1f, %.1f)", draft_.site[0], draft_.site[1], draft_.site[2]);
      } else {
        ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "[NOT SET] Obstacle (Site)");
      }

      // Drop-in Start
      if (draft_.has_start) {
        ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.4f, 1.0f), "[SET] Drop-in Start:");
        ImGui::SameLine();
        ImGui::Text("(%.1f, %.1f, %.1f)", draft_.start[0], draft_.start[1], draft_.start[2]);
      } else {
        ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "[NOT SET] Drop-in Start");
      }

      // Flythrough Camera
      if (draft_.has_cam) {
        ImGui::TextColored(ImVec4(0.3f, 0.8f, 1.0f, 1.0f), "[SET] Flythrough Cam:");
        ImGui::SameLine();
        ImGui::Text("(%.1f, %.1f, %.1f)", draft_.cam[0], draft_.cam[1], draft_.cam[2]);
      } else {
        ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "[NOT SET] Flythrough Cam");
      }

      if (draft_.has_site && draft_.has_start) {
        const float dx = draft_.site[0] - draft_.start[0];
        const float dy = draft_.site[1] - draft_.start[1];
        const float dz = draft_.site[2] - draft_.start[2];
        const float runway_len = std::sqrt(dx * dx + dy * dy + dz * dz);
        ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.2f, 1.0f), "Calculated Runway: %.1f m",
                           runway_len);
      }
    }

    ImGui::Separator();

    // Actions
    if (ImGui::Button("Save Challenge to Pack", ImVec2(-1, 34))) {
      SaveSpotToPack();
    }

    if (ImGui::Button("Close / Return to Skating (B)", ImVec2(-1, 28))) {
      Hide();
    }
  }
  ImGui::End();
}

void ChallengeEditorDialog::RenderGamepadHUD() {
  const ImGuiIO& io = ImGui::GetIO();
  const float dw = io.DisplaySize.x;
  const float dh = io.DisplaySize.y;

  constexpr float kBarW = 720.0f;
  constexpr float kBarH = 42.0f;
  const ImVec2 bar_min((dw - kBarW) * 0.5f, dh - kBarH - 18.0f);
  const ImVec2 bar_max((dw + kBarW) * 0.5f, dh - 18.0f);

  ImDrawList* dl = ImGui::GetForegroundDrawList();
  dl->AddRectFilled(bar_min, bar_max, IM_COL32(18, 18, 24, 225), 8.0f);
  dl->AddRect(bar_min, bar_max, IM_COL32(255, 255, 255, 45), 8.0f, 0, 1.5f);

  const char* const legend =
      "(L-Stick) Fly Drone  |  (R-Stick) Look  |  (RT/LT) Elevate  |  (X) Set Obstacle  |  "
      "(Y) Set Drop-in  |  (RB) Cam  |  (B) Exit";
  const ImVec2 txt_sz = ImGui::CalcTextSize(legend);
  dl->AddText(ImVec2(bar_min.x + (kBarW - txt_sz.x) * 0.5f, bar_min.y + (kBarH - txt_sz.y) * 0.5f),
              IM_COL32(240, 240, 240, 240), legend);
}

bool ChallengeEditorDialog::SaveSpotToPack() {
  if (draft_.pack_id[0] == '\0') {
    status_message_ = "Error: Mod pack ID cannot be empty.";
    status_timer_ = 4.0f;
    return false;
  }
  if (draft_.key[0] == '\0') {
    status_message_ = "Error: Challenge Key cannot be empty.";
    status_timer_ = 4.0f;
    return false;
  }
  if (draft_.title[0] == '\0') {
    status_message_ = "Error: Challenge Title cannot be empty.";
    status_timer_ = 4.0f;
    return false;
  }
  if (!draft_.has_site) {
    status_message_ = "Error: Please place Obstacle (Site) first!";
    status_timer_ = 4.0f;
    return false;
  }

  // Locate or create pack.json
  const auto app_root = rex::filesystem::GetAppRootFolder();
  std::filesystem::path pack_dir = app_root / "mods" / "raw" / draft_.pack_id;
  if (!std::filesystem::exists(pack_dir)) {
    pack_dir = std::filesystem::current_path() / "mods" / "raw" / draft_.pack_id;
  }

  std::error_code ec;
  std::filesystem::create_directories(pack_dir, ec);
  const auto manifest_path = pack_dir / "pack.json";

  json doc;
  if (std::filesystem::exists(manifest_path)) {
    try {
      std::ifstream in(manifest_path);
      in >> doc;
    } catch (...) {
      doc = json::object();
    }
  }

  // Populate metadata if new pack
  if (!doc.contains("id")) {
    doc["id"] = draft_.pack_id;
    doc["name"] = draft_.pack_id;
    doc["version"] = "1.0.0";
    doc["author"] = "In-Game Creator";
    doc["description"] = "Custom Own-The-Spot challenges created in-game.";
    doc["enabled"] = true;
  }
  if (!doc.contains("spots") || !doc["spots"].is_array()) {
    doc["spots"] = json::array();
  }

  // Construct spot object
  json spot_obj;
  spot_obj["key"] = draft_.key;
  spot_obj["title"] = draft_.title;
  spot_obj["desc"] = draft_.desc;
  spot_obj["district"] = kDistrictNames[draft_.district_index];
  spot_obj["category"] = kCategoryNames[draft_.category_index];

  // Round coordinates to 2 decimal places
  auto round2 = [](float v) { return std::round(v * 100.0f) / 100.0f; };

  spot_obj["site"] = {round2(draft_.site[0]), round2(draft_.site[1]), round2(draft_.site[2])};

  if (draft_.has_start) {
    spot_obj["start"] = {round2(draft_.start[0]), round2(draft_.start[1]),
                         round2(draft_.start[2])};
    const float dx = draft_.site[0] - draft_.start[0];
    const float dy = draft_.site[1] - draft_.start[1];
    const float dz = draft_.site[2] - draft_.start[2];
    spot_obj["runway"] = round2(std::sqrt(dx * dx + dy * dy + dz * dz));
  } else {
    spot_obj["runway"] = 16.0f;
  }

  if (draft_.has_cam) {
    spot_obj["cam"] = {round2(draft_.cam[0]), round2(draft_.cam[1]), round2(draft_.cam[2])};
  } else {
    // Default camera offset 3m back and 1.5m up
    spot_obj["cam"] = {round2(draft_.site[0] - 2.5f), round2(draft_.site[1] + 1.5f),
                       round2(draft_.site[2] - 2.5f)};
  }

  spot_obj["scores"] = {
      {"own", draft_.own_score},
      {"kill", draft_.kill_score},
  };

  // Upsert into spots array
  bool updated = false;
  for (auto& s : doc["spots"]) {
    if (s.is_object() && s.value("key", "") == draft_.key) {
      s = spot_obj;
      updated = true;
      break;
    }
  }
  if (!updated) {
    doc["spots"].push_back(spot_obj);
  }

  // Write to disk
  std::ofstream out(manifest_path);
  if (!out.is_open()) {
    status_message_ = "Error: Failed to write to " + manifest_path.string();
    status_timer_ = 4.0f;
    return false;
  }
  out << doc.dump(2) << std::endl;
  out.close();

  REXLOG_INFO("ChallengeEditorDialog: Saved spot '{}' to {}", draft_.key,
              manifest_path.string());

  status_message_ = std::string("Challenge '") + draft_.title + "' saved to " + draft_.pack_id + "!";
  status_timer_ = 5.0f;

  // Trigger pack compilation in background so game sees it immediately
  const std::string compositor_cmd = "python3 tools/pack_compositor.py > /dev/null 2>&1 &";
  (void)std::system(compositor_cmd.c_str());

  RefreshDiscoveredPacks();
  return true;
}

void ChallengeEditorDialog::OnDraw(ImGuiIO& io) {
  if (!visible_) {
    return;
  }

  anim_time_ += io.DeltaTime;
  if (status_timer_ > 0.0f) {
    status_timer_ -= io.DeltaTime;
  }

  // Query flown drone pose
  float drone_pos[3] = {0.0f, 0.0f, 0.0f};
  float drone_fwd[3] = {0.0f, 0.0f, 1.0f};
  if (!native_scene::GetFreecamPose(drone_pos, drone_fwd)) {
    native_scene::FreecamGuestPose(drone_pos);
  }

  // Compute aim cursor on ground
  float aim_target[3];
  ComputeAimTarget(drone_pos, drone_fwd, aim_target);

  // Process gamepads, keyboard, and hotkeys
  ProcessInput(io, drone_pos, aim_target);

  // Render in-world 3D elements (rings, beacons, runway, aim cursor)
  ImDrawList* bg_dl = ImGui::GetBackgroundDrawList();
  Render3DWorld(bg_dl, drone_pos, aim_target);

  // Render ImGui Editor Window
  RenderUIWindow();

  // Render Gamepad Legend Bar
  RenderGamepadHUD();
}

}  // namespace skate3
