#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include <imgui.h>
#include <rex/input/input.h>
#include <rex/ui/imgui_dialog.h>

namespace rex::input {
class InputSystem;
}

namespace skate3 {

class ChallengeEditorDialog final : public rex::ui::ImGuiDialog {
 public:
  using GamepadPollFn = std::function<bool(rex::input::X_INPUT_GAMEPAD*)>;
  using RestartFn = std::function<void()>;

  explicit ChallengeEditorDialog(rex::ui::ImGuiDrawer* drawer,
                                 GamepadPollFn gamepad_poll = nullptr,
                                 RestartFn restart_fn = nullptr);
  ~ChallengeEditorDialog();

  void Show();
  void Hide();
  void Toggle();
  bool visible() const { return visible_; }

 protected:
  void OnDraw(ImGuiIO& io) override;

 private:
  struct SpotDraft {
    char pack_id[64] = "west_coast_ots";
    char key[64] = "";
    char title[64] = "";
    char desc[256] = "Lay down your best lines here to own this spot.";
    int district_index = 0;  // 0: University, 1: Downtown, 2: Industrial, 3: Danny Way, 4: Maloof, 5: Art Gallery
    int category_index = 0;  // 0: Street, 1: Tranny, 2: Park
    int own_score = 600;
    int kill_score = 1000;

    bool has_site = false;
    float site[3] = {0.0f, 0.0f, 0.0f};

    bool has_start = false;
    float start[3] = {0.0f, 0.0f, 0.0f};

    bool has_cam = false;
    float cam[3] = {0.0f, 0.0f, 0.0f};
  };

  void RefreshDiscoveredPacks();
  void AutoDetectDistrict(const float drone_pos[3]);
  void ComputeAimTarget(const float drone_pos[3], const float drone_fwd[3], float out_target[3]);
  void ProcessInput(ImGuiIO& io, const float drone_pos[3], const float aim_target[3]);
  void Render3DWorld(ImDrawList* dl, const float drone_pos[3], const float aim_target[3]);
  void RenderUIWindow(const float drone_pos[3], const float aim_target[3]);
  void RenderGamepadHUD();
  bool SaveSpotToPack(bool and_restart = false);

  GamepadPollFn gamepad_poll_;
  RestartFn restart_fn_;
  bool visible_ = false;
  bool was_freecam_active_before_ = false;

  SpotDraft draft_;
  std::vector<std::string> discovered_packs_;
  int selected_pack_index_ = 0;

  // Previous button mask for edge detection (rising edge)
  uint16_t prev_buttons_ = 0;
  float anim_time_ = 0.0f;
  std::string status_message_;
  float status_timer_ = 0.0f;
  bool aim_hit_surface_ = false;
};

}  // namespace skate3
