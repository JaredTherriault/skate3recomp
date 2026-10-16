#pragma once

#include <chrono>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <rex/ui/imgui_dialog.h>
#include <rex/ui/overlay/simple_settings_overlay.h>

namespace rex::system {
class KernelState;
}

namespace rex::system::util {
class XdbfWrapper;
}

namespace rex::ui {
class ImGuiDrawer;
class ImmediateDrawer;
class ImmediateTexture;
}

namespace skate3 {

class AchievementManager;

struct AchievementDef {
  uint32_t id = 0;
  std::string name;
  std::string description;
  std::string unachieved_desc;
  std::string category;
  uint32_t gamerscore = 0;
  bool is_secret = false;
  std::filesystem::path custom_icon_path;
  std::string key;
  std::string trigger_type;
  std::string trigger_target;
  float trigger_threshold = 0.0f;
  float trigger_pos[3] = {0.0f, 0.0f, 0.0f};
  float trigger_pos2[3] = {0.0f, 0.0f, 0.0f};
  float trigger_radius = 5.0f;
  std::vector<std::string> required_tricks;
};

struct AchievementRecord {
  bool unlocked = false;
  uint64_t unlock_time = 0;
  std::vector<std::string> completed_tricks;
  float best_stat = 0.0f;
};

struct AchievementToastItem {
  uint32_t id = 0;
  std::string name;
  std::string description;
  uint32_t gamerscore = 0;
};

struct AchievementProgressToastItem {
  uint32_t achievement_id = 0;
  std::string item_name;      // e.g. "Kickflip"
  std::string parent_name;    // e.g. "Flip Trick Encyclopedia"
  uint32_t current_count = 0; // e.g. 1
  uint32_t total_count = 0;   // e.g. 14
};

class AchievementToastDialog final : public rex::ui::ImGuiDialog {
 public:
  explicit AchievementToastDialog(rex::ui::ImGuiDrawer* drawer);
  ~AchievementToastDialog() override = default;

  void PostToast(const AchievementToastItem& item);
  void PostProgressToast(const AchievementProgressToastItem& item);
  void Clear();
  void SetAchievementManager(AchievementManager* manager) { manager_ = manager; }
  rex::ui::ImGuiDrawer* imgui_drawer() const { return rex::ui::ImGuiDialog::imgui_drawer(); }
  std::optional<AchievementToastItem> current_toast() const {
    std::lock_guard lock(mutex_);
    return current_;
  }
  std::optional<AchievementProgressToastItem> current_progress_toast() const {
    std::lock_guard lock(mutex_);
    return current_progress_;
  }

 protected:
  bool WantsContinuousRepaint() const override;
  void OnDraw(ImGuiIO& io) override;

 private:
  mutable std::mutex mutex_;
  std::deque<AchievementToastItem> queue_;
  std::optional<AchievementToastItem> current_;
  std::chrono::steady_clock::time_point start_time_;
  bool is_active_ = false;

  // Separate, shorter queue for fraction progress notifications
  std::deque<AchievementProgressToastItem> progress_queue_;
  std::optional<AchievementProgressToastItem> current_progress_;
  std::chrono::steady_clock::time_point progress_start_time_;
  bool is_progress_active_ = false;

  AchievementManager* manager_ = nullptr;
};

class AchievementManager {
 public:
  AchievementManager();
  ~AchievementManager();

  static AchievementManager* instance() { return instance_; }

  void Initialize(rex::system::KernelState* kernel_state,
                  const std::filesystem::path& profiles_path,
                  AchievementToastDialog* toast_dialog,
                  rex::ui::ImGuiDrawer* imgui_drawer = nullptr);

  bool Unlock(uint32_t user_index, uint32_t achievement_id);
  bool Unlock(uint32_t user_index, std::string_view achievement_key);
  bool Unlock(std::string_view achievement_key) { return Unlock(0, achievement_key); }
  bool Unlock(uint32_t achievement_id) { return Unlock(0, achievement_id); }

  bool IsUnlocked(uint32_t user_index, uint32_t achievement_id) const;
  bool IsUnlocked(uint32_t user_index, std::string_view achievement_key) const;
  bool IsUnlocked(std::string_view achievement_key) const { return IsUnlocked(0, achievement_key); }
  bool IsUnlocked(uint32_t achievement_id) const { return IsUnlocked(0, achievement_id); }

  void ResetAchievements();
  void ResetAchievements(const std::vector<uint32_t>& achievement_ids);

  void Update(float dt);
  bool HasActiveTelemetryTriggers() const;
  void OnSpotCompleted(const std::string& spot_key, bool killed);
  void OnStatUpdated(const std::string& stat_name, float value);
  void OnTrickLanded(std::string_view trick_name);
  void OnCheatEntered(std::string_view cheat_code);

  rex::ui::SimpleAchievementData GetAchievementData() const;
  void* GetAchievementIcon(uint32_t achievement_id);

  void ReloadForActiveProfile();
  void ReloadDefinitions();
  void TestTriggerAchievement(uint32_t achievement_id = 0);

  const std::vector<AchievementDef>& definitions() const { return definitions_; }

 private:
  bool UnlockInternal(uint32_t user_index, uint32_t achievement_id);
  void LoadDefinitions();
  void LoadCustomPackAchievements();
  void LoadTitleUpdateDatabase();
  rex::ui::ImmediateDrawer* GetImmediateDrawer() const;
  std::filesystem::path GetAchievementFilePath(const std::string& profile_id) const;
  void LoadAchievementsForProfile(const std::string& profile_id);
  void SaveAchievements();
  std::string GetCurrentProfileId() const;

  mutable std::mutex mutex_;
  rex::system::KernelState* kernel_state_ = nullptr;
  std::filesystem::path profiles_path_;
  AchievementToastDialog* toast_dialog_ = nullptr;
  rex::ui::ImGuiDrawer* imgui_drawer_ = nullptr;
  std::string active_profile_id_ = "default";

  bool has_prev_pos_ = false;
  float prev_pos_[3] = {0.0f, 0.0f, 0.0f};
  struct GapTriggerState {
    std::chrono::steady_clock::time_point takeoff_time{};
    bool active = false;
    bool was_airborne = false;
  };
  std::unordered_map<uint32_t, GapTriggerState> gap_states_;

  std::vector<uint8_t> spa_data_;
  std::unique_ptr<rex::system::util::XdbfWrapper> spa_db_;
  mutable std::unordered_map<uint32_t, std::unique_ptr<rex::ui::ImmediateTexture>> icon_textures_;

  std::vector<AchievementDef> definitions_;
  std::unordered_map<uint32_t, size_t> id_to_index_;
  std::unordered_map<std::string, uint32_t> key_to_id_;
  std::unordered_map<uint32_t, AchievementRecord> records_;

  static inline AchievementManager* instance_ = nullptr;
};

bool IsSkaterOnBoardAirborne();
bool IsSkaterBailedOrOffBoard();
bool IsSkaterAirborneOrGrinding();

uint32_t GetLocalSkaterPtr();
bool IsLocalGameplayActive();
void SetLocalSkaterPhysicsPosition(float x, float y, float z);
bool GetActiveLocalSkaterPosition(float out_pos[3]);

std::vector<std::string> GetTrickGroupTricks(std::string_view group_name);
std::vector<std::string> MapToCanonicalTricks(std::string_view raw_trick_name);

}  // namespace skate3


