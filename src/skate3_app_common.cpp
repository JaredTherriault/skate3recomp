#include "skate3_app_common.h"

#include "skate3_demo_path.h"
#include "skate3_fov.h"
#include "skate3_coord_dump.h"
#include "skate3_iso_installer.h"
#include "skate3_native_render.h"
#include "skate3_native_scene.h"
#include "skate3_screenshot.h"
#include "skate3_shader_disasm.h"
#include "skate3_win_icon.h"
#include "skate3_title_update_installer.h"
#include "skate3_user_settings.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iomanip>
#include "../third_party/rexglue-sdk/thirdparty/tomlplusplus/vendor/json.hpp"
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <cmath>

// MSVC CRT lacks C23 roundeven / roundevenf (halfway cases rounded to even).
// LLVM lowering of __builtin_roundevenf emits calls to roundevenf when targeting MSVC.
extern "C" {
float roundevenf(float v) {
  float rounded = std::round(v);
  float diff = rounded - v;
  if (std::fabs(diff) == 0.5f && (static_cast<int64_t>(rounded) & 1)) {
    rounded = v - diff;
  }
  return rounded;
}

double roundeven(double v) {
  double rounded = std::round(v);
  double diff = rounded - v;
  if (std::fabs(diff) == 0.5 && (static_cast<int64_t>(rounded) & 1)) {
    rounded = v - diff;
  }
  return rounded;
}
}
#elif defined(__linux__) || defined(__APPLE__)
#include <spawn.h>
#if defined(__APPLE__)
#include <crt_externs.h>
#endif
#endif

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/filesystem/devices/stfs_container_device.h>
#include <rex/filesystem/devices/host_path_device.h>
#include <rex/filesystem/vfs.h>
#include <rex/graphics/flags.h>
#include <rex/graphics/native_guest_renderer.h>
#include <rex/input/input_system.h>
#include <rex/kernel/xam/module.h>
#include <rex/logging.h>
#include <rex/platform.h>
#include <rex/perf/counter.h>
#include <rex/ppc/context.h>
#include <rex/system/function_dispatcher.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xam/content_device.h>
#include <rex/system/xam/content_manager.h>
#include <rex/system/xam/user_profile.h>
#include <rex/system.h>
#include <rex/ui/flags.h>
#include <rex/ui/keybinds.h>
#include <rex/ui/window.h>
#include <rex/ui/overlay/simple_settings_overlay.h>

#include <imgui.h>
#include <toml++/toml.hpp>

#if defined(__linux__) || defined(__APPLE__)
extern char** environ;
#endif

extern const rex::PPCImageInfo eawebkit_PPCImageConfig;

// Register multi-entry-function alternate entries that rex's analyzer can't
// lift via config because they overlap with auto-discovered parent functions.
extern "C" REX_FUNC(__restgprlr_19);

// Defined in skate3_native_scene.cpp; toggled by the showcase hotkey below.
REXCVAR_DECLARE(bool, skate3_native_render_capture_hotkeys);
REXCVAR_DECLARE(bool, skate3_native_render_scene_showcase);
// Defined in skate3_native_scene.cpp; the freecam hotkey below toggles it,
// and while it captures input the guest input system is gated off.
REXCVAR_DECLARE(bool, skate3_native_render_scene_freecam);
REXCVAR_DECLARE(bool, skate3_native_render_scene_freecam_capture_input);

REXCVAR_DEFINE_STRING(skate3_dlc_root, "", "Skate 3",
                      "Directory containing Skate 3 DLC package files");
REXCVAR_DEFINE_BOOL(skate3_auto_install_dlc, true, "Skate 3",
                    "Install DLC package files found in configured DLC folders");
REXCVAR_DEFINE_BOOL(skate3_mods, true, "Skate 3",
                    "Serve override mod files from skate3_mods_dir through "
                    "VFS symlinks, without modifying the game tree.");
REXCVAR_DEFINE_STRING(skate3_mods_dir, "", "Skate 3",
                      "Directory holding compiled mod files (.mod) "
                      "(empty = <app root>/mods/compiled).");
REXCVAR_DEFINE_BOOL(skate3_custom_bigs, true, "Skate 3",
                    "Legacy alias for skate3_mods.");
REXCVAR_DEFINE_STRING(skate3_custom_bigs_dir, "", "Skate 3",
                      "Legacy alias for skate3_mods_dir.");
REXCVAR_DEFINE_BOOL(skate3_ultrawide, false, "Skate 3",
                    "Ultrawide display support: the native renderer draws true widescreen "
                    "frames at the host display aspect (requires the native renderer; the "
                    "emulated fallback presents 16:9 pillarboxed)")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_DOUBLE(skate3_field_of_view, 60.0, "Skate 3",
                      "Gameplay camera field of view in degrees")
    .range(40.0, 120.0);
REXCVAR_DEFINE_BOOL(skate3_ultrawide_widen_game_frustum, true, "Skate 3",
                    "Widen the game-side main-world cull frustum to match the ultrawide view");
REXCVAR_DEFINE_DOUBLE(skate3_ultrawide_target_aspect, 0.0, "Skate 3",
                      "Ultrawide display aspect (0 = derive from the host display at boot)")
    .range(0.0, 8.0);

namespace {

void ApplyDemoPathProfileOverride() {
  if (!rex::cvar::Query<bool>("skate3_demo_path") &&
      !rex::cvar::Query<bool>("skate3_demo_path_probe")) {
    return;
  }
  if (rex::cvar::Query<bool>("skate3_demo_path_signed_in")) {
    // Closer-to-real-use runs: keep the user's profile and save so the boot
    // flow resumes their career instead of a signed-out session.
    return;
  }

  rex::cvar::SetFlagByName("user_profile_signed_in", "false");
  rex::cvar::SetFlagByName("user_live_signed_in", "false");
}

#if defined(__linux__) || defined(__APPLE__)
std::vector<std::string> CurrentProcessArgumentsForRestart(
    const std::filesystem::path& executable_path) {
  std::vector<std::string> args;
#if defined(__APPLE__)
  int argc = *_NSGetArgc();
  char** argv = *_NSGetArgv();
  args.reserve(static_cast<size_t>(argc > 0 ? argc : 1));
  for (int i = 0; i < argc; ++i) {
    args.emplace_back(argv[i] ? argv[i] : "");
  }
#else
  std::ifstream cmdline("/proc/self/cmdline", std::ios::binary);
  std::string arg;
  char ch = 0;
  while (cmdline.get(ch)) {
    if (ch == '\0') {
      args.push_back(std::move(arg));
      arg.clear();
    } else {
      arg.push_back(ch);
    }
  }
  if (!arg.empty()) {
    args.push_back(std::move(arg));
  }
#endif

  const std::string executable = rex::path_to_utf8(executable_path);
  if (args.empty()) {
    args.push_back(executable);
  } else {
    args[0] = executable;
  }
  return args;
}

void SetRestartArgument(std::vector<std::string>& args, std::string name, std::string value) {
  const std::string option = "--" + name;
  const std::string option_with_equals = option + "=";
  for (size_t i = 1; i < args.size(); ++i) {
    if (args[i] == option) {
      if (i + 1 < args.size()) {
        args[i + 1] = std::move(value);
      } else {
        args.push_back(std::move(value));
      }
      return;
    }
    if (args[i].starts_with(option_with_equals)) {
      args[i] = option_with_equals + value;
      return;
    }
  }

  args.push_back(std::move(option));
  args.push_back(std::move(value));
}
#endif

constexpr std::string_view kUserDirectoryName = "skate3";
constexpr std::string_view kSettingsFilename = "settings.toml";
constexpr std::string_view kDlcDirectoryName = "dlc";
constexpr std::string_view kSavesDirectoryName = "saves";
constexpr double kSixteenNineAspect = 16.0 / 9.0;
constexpr double kUltrawideAspectEpsilon = 0.01;

struct DisplaySize {
  int32_t width = 0;
  int32_t height = 0;
};

#if defined(_WIN32)
BOOL CALLBACK CollectMonitorCallback(HMONITOR monitor_handle, HDC, LPRECT, LPARAM data) {
  auto* monitors = reinterpret_cast<std::vector<HMONITOR>*>(data);
  monitors->push_back(monitor_handle);
  return TRUE;
}

HMONITOR GetConfiguredMonitorHandle() {
  const int32_t monitor_index = REXCVAR_GET(monitor);
  if (monitor_index > 0) {
    std::vector<HMONITOR> monitors;
    EnumDisplayMonitors(nullptr, nullptr, CollectMonitorCallback,
                        reinterpret_cast<LPARAM>(&monitors));
    if (monitor_index <= static_cast<int32_t>(monitors.size())) {
      return monitors[monitor_index - 1];
    }
  }

  POINT origin{0, 0};
  return MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY);
}

std::optional<DisplaySize> QueryFullscreenMonitorSize() {
  HMONITOR monitor_handle = GetConfiguredMonitorHandle();
  if (!monitor_handle) {
    return std::nullopt;
  }

  MONITORINFO monitor_info{};
  monitor_info.cbSize = sizeof(monitor_info);
  if (!GetMonitorInfo(monitor_handle, &monitor_info)) {
    return std::nullopt;
  }

  const int32_t width = monitor_info.rcMonitor.right - monitor_info.rcMonitor.left;
  const int32_t height = monitor_info.rcMonitor.bottom - monitor_info.rcMonitor.top;
  if (width <= 0 || height <= 0) {
    return std::nullopt;
  }
  return DisplaySize{width, height};
}
#else
std::optional<DisplaySize> QueryFullscreenMonitorSize() {
  return std::nullopt;
}
#endif

std::optional<DisplaySize> ResolveUltrawideTargetDisplaySize() {
  if (rex::cvar::HasNonDefaultValue("resolution")) {
    return std::nullopt;
  }

  if (REXCVAR_GET(fullscreen)) {
    return QueryFullscreenMonitorSize();
  }

  const int32_t configured_window_width = REXCVAR_GET(window_width);
  const int32_t configured_window_height = REXCVAR_GET(window_height);
  if (rex::cvar::HasNonDefaultValue("window_width") &&
      rex::cvar::HasNonDefaultValue("window_height") && configured_window_width > 0 &&
      configured_window_height > 0) {
    return DisplaySize{configured_window_width, configured_window_height};
  }

  return std::nullopt;
}

void ApplyUltrawideVideoDefaults() {
  if (!REXCVAR_GET(skate3_ultrawide)) {
    return;
  }

  // Explicitly configured aspect wins; otherwise derive it from the
  // fullscreen monitor (or a configured window size) once at boot.
  double target_aspect = rex::cvar::HasNonDefaultValue("skate3_ultrawide_target_aspect")
                             ? REXCVAR_GET(skate3_ultrawide_target_aspect)
                             : 0.0;
  if (target_aspect <= kSixteenNineAspect + kUltrawideAspectEpsilon) {
    const std::optional<DisplaySize> target_size = ResolveUltrawideTargetDisplaySize();
    if (!target_size || target_size->width <= 0 || target_size->height <= 0) {
      return;
    }
    target_aspect =
        static_cast<double>(target_size->width) / static_cast<double>(target_size->height);
    if (target_aspect <= kSixteenNineAspect + kUltrawideAspectEpsilon) {
      return;
    }
    rex::cvar::SetFlagByName("skate3_ultrawide_target_aspect", std::to_string(target_aspect));
  }

  // The native renderer draws true wide frames at this aspect (wide guest
  // output + Hor+ projection + centered 2D band). Emulated fallback frames
  // keep the 16:9 guest output and present pillarboxed via the letterbox
  // default below.
  rex::graphics::SetNativeGuestOutputWideAspect(
      std::clamp(target_aspect, kSixteenNineAspect, 8.0));

  if (!rex::cvar::HasNonDefaultValue("present_letterbox")) {
    rex::cvar::SetFlagByName("present_letterbox", "true");
  }
}

void DisableActiveDebugDiagnostics() {
  constexpr std::string_view kFalseFlags[] = {
      "perf_draw_fingerprints",
      "perf_keep_heavyweight_draw_diagnostics",
      "trace_gpu_stream",
  };
  for (std::string_view flag : kFalseFlags) {
    rex::cvar::SetFlagByName(std::string(flag), "false");
  }

  constexpr std::string_view kZeroFlags[] = {
      "vulkan_debug_log_frame_summaries_remaining",
      "vulkan_debug_log_resolve_decisions_remaining",
      "vulkan_debug_log_team_profile_background_candidates_remaining",
      "vulkan_debug_log_team_profile_background_bindings_remaining",
      "filesystem_debug_log_fe_asset_ops_remaining",
      "filesystem_debug_log_team_profile_background_remaining",
  };
  for (std::string_view flag : kZeroFlags) {
    rex::cvar::SetFlagByName(std::string(flag), "0");
  }
}

std::filesystem::path DefaultDocumentsUserRoot() {
  return rex::filesystem::GetUserFolder() / std::string(kUserDirectoryName);
}

std::filesystem::path DefaultRoamingUserRoot() {
#if defined(_WIN32)
  char* appdata = nullptr;
  size_t appdata_size = 0;
  if (_dupenv_s(&appdata, &appdata_size, "APPDATA") == 0 && appdata && *appdata) {
    std::filesystem::path result = std::filesystem::path(appdata) / std::string(kUserDirectoryName);
    std::free(appdata);
    return result;
  }
  std::free(appdata);
#endif
  return DefaultDocumentsUserRoot();
}

std::filesystem::path ResolveSkate3UserRoot(const rex::PathConfig& paths) {
  const auto executable_root = rex::filesystem::GetAppRootFolder();
  if (std::filesystem::exists(executable_root / "portable.txt")) {
    return executable_root;
  }

  const auto old_default = DefaultDocumentsUserRoot();
  if (!paths.user_data_root.empty() && paths.user_data_root != old_default) {
    return paths.user_data_root;
  }

  return DefaultRoamingUserRoot();
}

void ConfigureSkate3UserPaths(rex::PathConfig& paths, std::filesystem::path& settings_path,
                              std::filesystem::path& profiles_path) {
  const auto old_user_root = DefaultDocumentsUserRoot();
  const auto old_cache_root = old_user_root / "cache";
  const auto original_cache_root = paths.cache_root;
  const auto resolved_user_root = ResolveSkate3UserRoot(paths);

  paths.user_data_root = resolved_user_root;
  if (original_cache_root.empty() || original_cache_root == old_cache_root) {
    paths.cache_root = resolved_user_root / "cache";
  }
  settings_path = resolved_user_root / std::string(kSettingsFilename);
  profiles_path = skate3::ProfilesFilePath(resolved_user_root);
}

bool ConfigContainsAnyKey(const toml::table& table, std::initializer_list<std::string_view> keys) {
  for (std::string_view key : keys) {
    if (table.contains(key)) {
      return true;
    }
  }
  return false;
}

bool ConfigFileContainsAnyKey(const std::filesystem::path& config_path,
                              std::initializer_list<std::string_view> keys) {
  if (config_path.empty() || !std::filesystem::exists(config_path)) {
    return false;
  }
  try {
    auto config = toml::parse_file(config_path.string());
    return ConfigContainsAnyKey(config, keys);
  } catch (const toml::parse_error&) {
    return false;
  }
}

bool DeveloperConfigHasResolutionScaleOverride(const std::filesystem::path& config_path) {
  return ConfigFileContainsAnyKey(config_path, {"resolution_scale", "draw_resolution_scale_x",
                                                "draw_resolution_scale_y"});
}

constexpr int kDefaultResolutionScale = 2;

void ApplyFirstRunVideoDefaults(const std::filesystem::path& settings_path,
                                const std::filesystem::path& developer_config_path) {
  if (std::filesystem::exists(settings_path) ||
      DeveloperConfigHasResolutionScaleOverride(developer_config_path) ||
      rex::cvar::HasNonDefaultValue("resolution_scale") ||
      rex::cvar::HasNonDefaultValue("draw_resolution_scale_x") ||
      rex::cvar::HasNonDefaultValue("draw_resolution_scale_y")) {
    return;
  }

  const auto scale = std::to_string(kDefaultResolutionScale);
  rex::cvar::SetFlagByName("resolution_scale", scale);
  rex::cvar::SetFlagByName("draw_resolution_scale_x", scale);
  rex::cvar::SetFlagByName("draw_resolution_scale_y", scale);
}

void LoadAndNormalizeSimpleSettings(const std::filesystem::path& settings_path,
                                    const std::filesystem::path& developer_config_path) {
  if (std::filesystem::exists(settings_path)) {
    rex::cvar::LoadConfig(settings_path);
  } else {
    ApplyFirstRunVideoDefaults(settings_path, developer_config_path);
  }
  rex::ui::EnsureSimpleSettingsConfig(settings_path);
}

std::filesystem::path ResolveRuntimeGameDataRoot(const rex::PathConfig& paths) {
  if (!paths.game_data_root.empty()) {
    return paths.game_data_root;
  }

  const auto working_directory_game = std::filesystem::current_path() / "game";
  if (skate3::IsGameInstalled(working_directory_game)) {
    return working_directory_game;
  }

  return paths.config_path.parent_path() / "game";
}

const char* FirstExistingFontPath(std::initializer_list<const char*> paths) {
  for (const char* path : paths) {
    if (std::filesystem::exists(path)) {
      return path;
    }
  }
  return nullptr;
}

std::string Hex8(uint32_t value) {
  std::ostringstream stream;
  stream << std::uppercase << std::hex << std::setw(8) << std::setfill('0') << value;
  return stream.str();
}

std::filesystem::path InstalledMarketplaceContentPath(const std::filesystem::path& content_root,
                                                      uint32_t title_id,
                                                      const std::filesystem::path& package_path) {
  return content_root / "0000000000000000" / Hex8(title_id) / "00000002" /
         package_path.filename();
}

std::filesystem::path InstalledMarketplaceHeaderPath(const std::filesystem::path& content_root,
                                                     uint32_t title_id,
                                                     const std::filesystem::path& package_path) {
  return content_root / "0000000000000000" / Hex8(title_id) / "Headers" / "00000002" /
         (package_path.filename().string() + ".header");
}

bool IsInstalledMarketplaceContent(const std::filesystem::path& content_root, uint32_t title_id,
                                   const std::filesystem::path& package_path) {
  return std::filesystem::is_directory(
             InstalledMarketplaceContentPath(content_root, title_id, package_path)) &&
         std::filesystem::is_regular_file(
             InstalledMarketplaceHeaderPath(content_root, title_id, package_path));
}

std::vector<std::filesystem::path> DiscoverDlcSourceDirectories(
    const std::filesystem::path& executable_root, const std::filesystem::path& game_data_root,
    const std::filesystem::path& user_data_root) {
  std::vector<std::filesystem::path> dirs;
  auto add_dir = [&](std::filesystem::path dir) {
    if (dir.empty()) {
      return;
    }
    std::error_code ec;
    dir = std::filesystem::absolute(dir, ec);
    if (ec) {
      return;
    }
    for (const auto& existing : dirs) {
      if (std::filesystem::equivalent(existing, dir, ec)) {
        return;
      }
      ec.clear();
    }
    dirs.push_back(std::move(dir));
  };

  const std::string configured_root = REXCVAR_GET(skate3_dlc_root);
  if (!configured_root.empty()) {
    add_dir(configured_root);
  }
  add_dir(executable_root / std::string(kDlcDirectoryName));
  add_dir(game_data_root / std::string(kDlcDirectoryName));
  add_dir(user_data_root / std::string(kDlcDirectoryName));
  return dirs;
}

}  // namespace

#if !SKATE3_HAS_TITLE_UPDATE
// Retail sub_82EBAE4C: alternate entry into sub_82EBAE34 (37 vtable refs in .data).
// Body: lwz r3, 0x164(r31); addi r1, r31, 0x140; b __restgprlr_19
static void Sub82EBAE4CImpl(PPCContext& ctx, uint8_t* base) {
  ctx.r3.u64 = REX_LOAD_U32(ctx.r31.u32 + 0x164);
  ctx.r1.s64 = ctx.r31.s64 + 0x140;
  __restgprlr_19(ctx, base);
}
#endif

Skate3BaseApp::~Skate3BaseApp() = default;

void Skate3BaseApp::OnConfigurePaths(rex::PathConfig& paths) {
  ConfigureSkate3UserPaths(paths, user_settings_path_, profiles_path_);
  config_path_ = paths.config_path;
  LoadAndNormalizeSimpleSettings(user_settings_path_, config_path_);
  Skate3InitializeFieldOfViewOverride();
  ApplyUltrawideVideoDefaults();
  DisableActiveDebugDiagnostics();
}

void Skate3BaseApp::OnConfigureFonts(ImFontAtlas* atlas) {
  if (!atlas) {
    return;
  }

  atlas->Clear();
  ImFontConfig font_config;
  font_config.OversampleH = 2;
  font_config.OversampleV = 2;
  font_config.PixelSnapH = false;

  const char* base_font_path = FirstExistingFontPath({
#if defined(_WIN32)
      "C:\\Windows\\Fonts\\Helvetica.ttf",
      "C:\\Windows\\Fonts\\helvetica.ttf",
      "C:\\Windows\\Fonts\\HelveticaNeue.ttf",
      "C:\\Windows\\Fonts\\arial.ttf",
#elif defined(__APPLE__)
      "/System/Library/Fonts/SFNS.ttf",
      "/System/Library/Fonts/SFCompact.ttf",
      "/System/Library/Fonts/HelveticaNeue.ttc",
      "/System/Library/Fonts/LucidaGrande.ttc",
      "/System/Library/Fonts/Supplemental/Arial.ttf",
#else
      "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
      "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
      "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
      "/usr/local/share/fonts/NotoSans-Regular.ttf",
      "/usr/local/share/fonts/LiberationSans-Regular.ttf",
#endif
  });

  if (base_font_path) {
    atlas->AddFontFromFileTTF(base_font_path, 16.0f, &font_config,
                              atlas->GetGlyphRangesDefault());
  } else {
    atlas->AddFontDefault();
  }

#if defined(_WIN32)
  const char* jp_font_path = "C:\\Windows\\Fonts\\msgothic.ttc";
  if (std::filesystem::exists(jp_font_path)) {
    ImFontConfig jp_font_config;
    jp_font_config.MergeMode = true;
    jp_font_config.OversampleH = jp_font_config.OversampleV = 1;
    jp_font_config.PixelSnapH = true;
    jp_font_config.FontNo = 0;
    atlas->AddFontFromFileTTF(jp_font_path, 16.0f, &jp_font_config,
                              atlas->GetGlyphRangesJapanese());
  }
#endif
}

std::optional<rex::PathConfig> Skate3BaseApp::OnFinalizePaths(
    const rex::PathConfig& defaults, std::function<void(rex::PathConfig)> resume) {
  config_path_ = defaults.config_path;
  user_settings_path_ = defaults.user_data_root / std::string(kSettingsFilename);
  profiles_path_ = skate3::ProfilesFilePath(defaults.user_data_root);

  auto profiles = skate3::LoadProfiles(profiles_path_);
  const bool has_profiles_file = std::filesystem::exists(profiles_path_);
  skate3::EnsureUsableProfileStore(profiles, "Player");
  if (auto* profile = skate3::FindSelectedProfile(profiles)) {
    skate3::ApplyProfileCvars(*profile);
    ApplyDemoPathProfileOverride();
  }

  const bool has_config_file = std::filesystem::exists(defaults.config_path);
  const bool has_game_path = std::filesystem::is_directory(defaults.game_data_root);
  if (!has_profiles_file && has_config_file && has_game_path) {
    skate3::SaveProfiles(profiles_path_, profiles);
  }
  auto runtime_paths = defaults;
  runtime_paths.game_data_root = ResolveRuntimeGameDataRoot(runtime_paths);
  runtime_paths.config_path.clear();
  if (!skate3::IsGameInstalled(runtime_paths.game_data_root)) {
    REXLOG_INFO("Game files not found at {}; launching rexglue ISO installer",
                runtime_paths.game_data_root.string());
#if defined(__APPLE__)
    if (const char* automated_iso = std::getenv("SKATE3_INSTALL_ISO");
        automated_iso == nullptr || *automated_iso == '\0') {
#if SKATE3_HAS_TITLE_UPDATE
      // Chain the title update wizard after the ISO install completes.
      auto resume_after_title_update =
          [this, resume = std::move(resume)](rex::PathConfig paths) mutable {
            if (!skate3::IsTitleUpdateInstalled(paths.game_data_root)) {
              skate3::ShowTitleUpdateInstallWizard(imgui_drawer(), std::move(paths),
                                                   std::move(resume));
              return;
            }
            resume(std::move(paths));
          };
      skate3::ShowRexglueIsoInstallWizard(imgui_drawer(), std::move(runtime_paths),
                                          std::move(resume_after_title_update));
#else
      skate3::ShowRexglueIsoInstallWizard(imgui_drawer(), std::move(runtime_paths),
                                          std::move(resume));
#endif
      return std::nullopt;
    }
#endif
    rex::PathConfig installed_paths;
    const bool installed = skate3::RunRexglueIsoInstallWizardBlocking(
        app_context(), window(), imgui_drawer(), runtime_paths, installed_paths);
    if (!installed) {
      app_context().QuitFromUIThread();
      return std::nullopt;
    }
    runtime_paths = std::move(installed_paths);
  }

#if SKATE3_HAS_TITLE_UPDATE
  // This build executes Title Update 3 code; the game cannot boot without the
  // TU payloads staged next to the installed game files. Existing installs
  // from releases that predate TU support land here with the game present but
  // the title update missing.
  if (!skate3::IsTitleUpdateInstalled(runtime_paths.game_data_root)) {
    REXLOG_INFO("Skate 3 Title Update 3 not staged at {}; launching title update installer",
                runtime_paths.game_data_root.string());
#if defined(__APPLE__)
    if (const char* automated_tu = std::getenv("SKATE3_INSTALL_TU");
        automated_tu == nullptr || *automated_tu == '\0') {
      skate3::ShowTitleUpdateInstallWizard(imgui_drawer(), std::move(runtime_paths),
                                           std::move(resume));
      return std::nullopt;
    }
#endif
    rex::PathConfig tu_paths;
    const bool tu_installed = skate3::RunTitleUpdateInstallWizardBlocking(
        app_context(), window(), imgui_drawer(), runtime_paths, tu_paths);
    if (!tu_installed) {
      app_context().QuitFromUIThread();
      return std::nullopt;
    }
    runtime_paths = std::move(tu_paths);
  }
#endif
#if defined(_WIN32)
  // Window/taskbar + Explorer icon sourced from the user's OWN game art at
  // runtime (game/nxeart); the shipped exe and the repo carry no EA
  // artwork (see skate3_win_icon.h). Game files are guaranteed installed by
  // this point on Windows (the install wizards above run blocking).
  skate3::ApplyGameIconFromGameData(
      runtime_paths.game_data_root,
      window() ? window()->GetNativeWindowHandle() : nullptr);
#endif
  return runtime_paths;
}

void Skate3BaseApp::OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) {
  // PASS 38: the coordinate debug HUD (skate3_show_coords, F7 toggles;
  // self-contained in skate3::coord_dump, cvar change-callback driven).
  skate3::coord_dump::OnCreateDialogs(drawer);
  // Native/emulated corner readout (top right; off by default, cvar
  // skate3_native_render_mode_indicator shows it live). Input-transparent,
  // so it never affects cursor or focus handling.
  render_mode_indicator_ = std::make_unique<skate3::RenderModeIndicator>(drawer);
  rex::ui::RegisterBind("bind_skate3_menu", "Escape", "Skate 3 settings", [this] {
    // Escape backs out of the settings screen level by level (rows ->
    // category rail -> closed); when the screen is closed it opens it.
    if (simple_settings_dialog_ && simple_settings_dialog_->visible()) {
      simple_settings_dialog_->NavigateBack();
    } else {
      ToggleSimpleSettings();
    }
  });
  rex::ui::RegisterBind("bind_skate3_menu_alt", "F1", "Skate 3 settings alternate", [this] {
    ToggleSimpleSettings();
  });
  // Remembered handle: the F11 paired A/B parity capture (native + emulated
  // screenshots + gsnap, sequenced from the guest frame loop in
  // skate3_native_render.cpp) needs the window without an app pointer.
  skate3::screenshot::RememberWindow(window() ? window()->GetNativeWindowHandle()
                                              : nullptr);
  rex::ui::RegisterBind("bind_skate3_screenshot", "F6",
                        "Save screenshot to screenshots/", [this] {
                          skate3::screenshot::CaptureWindow(
                              window() ? window()->GetNativeWindowHandle() : nullptr);
                        });
  rex::ui::RegisterBind("bind_skate3_native_render_toggle", "F5",
                        "Toggle native/emulated renderer", [] {
                          skate3::native_scene::ToggleSceneEnabled();
                        });
  rex::ui::RegisterBind("bind_skate3_save_draw_fingerprints", "F8",
                        "Save draw fingerprint log", [this] {
                          SaveDrawFingerprintLog();
                        });
  rex::ui::RegisterBind("bind_skate3_log_debug_marker", "F9",
                        "Write debug marker to log", [this] {
                          LogDebugMarker();
                        });
  rex::ui::RegisterBind("bind_skate3_log_user_marker", "F10",
                        "Write marker to log", [this] {
                          LogUserMarker();
                        });
  rex::ui::RegisterBind("bind_skate3_native_debug", "F12",
                        "Native render debug menu", [this] {
                          ToggleNativeDebug();
                        });
  rex::ui::RegisterBind("bind_skate3_showcase", "Ctrl+Shift+B",
                        "Graphics build-up showcase", [] {
                          // A capture/recording tool, not a player feature:
                          // like the capture hotkeys, the bind is inert
                          // unless the diagnostics cvar opts in (the F12
                          // Showcase Setup window still starts runs).
                          if (!REXCVAR_GET(skate3_native_render_capture_hotkeys)) {
                            return;
                          }
                          REXCVAR_SET(
                              skate3_native_render_scene_showcase,
                              !REXCVAR_GET(skate3_native_render_scene_showcase));
                        });
  rex::ui::RegisterBind("bind_skate3_freecam", "End",
                        "Drone camera (free fly)", [] {
                          REXCVAR_SET(
                              skate3_native_render_scene_freecam,
                              !REXCVAR_GET(skate3_native_render_scene_freecam));
                        });
}

void Skate3BaseApp::OnPostSetup() {
  skate3::shader_disasm::RunIfRequested();
  ApplySelectedProfileToRuntime();
  ApplyGameplayCursorMode();

  if (auto* input_system = static_cast<rex::input::InputSystem*>(runtime()->input_system())) {
    input_system->SetActiveCallback([this]() {
      const bool settings_visible = simple_settings_dialog_ && simple_settings_dialog_->visible();
      const bool xam_ui_active = rex::kernel::xam::xeXamIsUIActive();
      // The drone cam owns the keyboard while flying: keep guest input off
      // so the fly keys don't also steer the skater.
      const bool freecam_captures =
          REXCVAR_GET(skate3_native_render_scene_freecam) &&
          REXCVAR_GET(skate3_native_render_scene_freecam_capture_input);
      return !settings_visible && !xam_ui_active && !freecam_captures;
    });
    input_system->SetMenuChordCallback([this]() {
      app_context().CallInUIThreadDeferred([this]() { ToggleSimpleSettings(); });
    });
  }

  if (std::getenv("SKATE3_DISABLE_BIG_ALIASES") == nullptr) {
    InstallBigDeviceAliases();
  }
  // Portable saves: a "saves" folder next to the executable takes over
  // saved-game content when it exists (launcher-managed installs). Layout is
  // flattened to saves/<xuid>/<save>; nothing is migrated from the user
  // content tree, and without the folder the default location is unchanged.
  {
    const auto portable_saves =
        rex::filesystem::GetAppRootFolder() / std::string(kSavesDirectoryName);
    if (std::filesystem::is_directory(portable_saves) && runtime()->kernel_state() &&
        runtime()->kernel_state()->content_manager()) {
      runtime()->kernel_state()->content_manager()->SetContentTypeRoot(
          rex::system::XContentType::kSavedGame, portable_saves);
      REXLOG_INFO("Skate 3 saves: using portable folder {}", portable_saves.string());
    }
  }
  // Scan and install DLC packages before custom big overrides so that stock base
  // files are available for delta overlays.
  InstallDlcPackages();
  // Custom mod overrides: serve modified db/miscboot/missions/DLC mods from a
  // host directory via VFS symlinks (the game tree stays untouched). Must run
  // before the guest opens the base bigs; the registration is one-shot at init.
  InstallModOverrides();
  InstallRecipeOverlay();

  // Register retail multi-entry-function alternate entries.
#if !SKATE3_HAS_TITLE_UPDATE
  runtime()->function_dispatcher()->SetFunction(0x82EBAE4C, &Sub82EBAE4CImpl);
#endif

  auto* dispatcher = runtime()->function_dispatcher();
  skate3::native_render::Install();
  skate3::demo_path::InstallHooks(dispatcher);
  // User-facing intro-movie skip (independent of the demo path): the movie
  // completion override polls the merged UI pad state through this provider.
  skate3::demo_path::SetUiInputProvider([this]() {
    return static_cast<rex::input::InputSystem*>(runtime()->input_system());
  });
  if (dispatcher->InitializeFunctionTable(eawebkit_PPCImageConfig.code_base,
                                          eawebkit_PPCImageConfig.code_size,
                                          eawebkit_PPCImageConfig.image_base,
                                          eawebkit_PPCImageConfig.image_size)) {
    for (int i = 0; eawebkit_PPCImageConfig.func_mappings[i].guest != 0; ++i) {
      auto* host = eawebkit_PPCImageConfig.func_mappings[i].host;
      if (host) {
        dispatcher->SetFunction(
            static_cast<uint32_t>(eawebkit_PPCImageConfig.func_mappings[i].guest),
            host);
      }
    }
  }
}

void Skate3BaseApp::OnShutdown() {
  rex::ui::UnregisterBind("bind_skate3_menu");
  rex::ui::UnregisterBind("bind_skate3_menu_alt");
  rex::ui::UnregisterBind("bind_skate3_save_draw_fingerprints");
  rex::ui::UnregisterBind("bind_skate3_log_debug_marker");
  rex::ui::UnregisterBind("bind_skate3_log_user_marker");
  rex::ui::UnregisterBind("bind_skate3_native_debug");
  ApplyGameplayCursorMode();
  skate3::native_scene::SetSettingsMenuBlur(false);
  simple_settings_dialog_.reset();
  native_debug_dialog_.reset();
  render_mode_indicator_.reset();
}

void Skate3BaseApp::ToggleSimpleSettings() {
  if (simple_settings_dialog_) {
    if (simple_settings_dialog_->visible()) {
      simple_settings_dialog_->Hide();
    } else {
      ApplySettingsCursorMode();
      skate3::native_scene::SetSettingsMenuBlur(true);
      simple_settings_dialog_->Show();
    }
    return;
  }

  auto load_profiles = [this]() {
    rex::ui::SimpleProfileState state;
    auto store = skate3::LoadProfiles(profiles_path_);
    skate3::EnsureUsableProfileStore(store, "Player");
    state.selected_index = 0;
    for (int i = 0; i < static_cast<int>(store.profiles.size()); ++i) {
      const auto& profile = store.profiles[i];
      state.profiles.push_back({profile.id, profile.gamertag, profile.signed_in});
      if (profile.id == store.selected_profile) {
        state.selected_index = i;
      }
    }
    return state;
  };
  auto save_profile = [this](int selected_index, std::string gamertag, bool signed_in) {
    auto store = skate3::LoadProfiles(profiles_path_);
    skate3::EnsureUsableProfileStore(store, "Player");
    if (store.profiles.empty()) {
      return;
    }
    selected_index = std::clamp(selected_index, 0, static_cast<int>(store.profiles.size()) - 1);
    auto& profile = store.profiles[selected_index];
    if (!gamertag.empty()) {
      profile.gamertag = std::move(gamertag);
    }
    profile.signed_in = signed_in;
    if (!profile.signed_in) {
      profile.live_signed_in = false;
    }
    store.selected_profile = profile.id;
    skate3::SaveProfiles(profiles_path_, store);
    skate3::ApplyProfileCvars(profile);
    ApplyDemoPathProfileOverride();
    ApplySelectedProfileToRuntime();
  };
  // Fires on every Hide (B/Esc, Close Settings, Close Game), the one spot
  // that reliably sees the menu close regardless of who initiated it.
  auto close_settings = [this]() {
    skate3::native_scene::SetSettingsMenuBlur(false);
    ApplyGameplayCursorMode();
  };
  auto close_game = [this]() {
#if REX_PLATFORM_MAC || REX_PLATFORM_LINUX
    std::thread([]() {
      std::this_thread::sleep_for(std::chrono::seconds(10));
      REXLOG_WARN("Close Game watchdog exiting process after shutdown timeout");
      std::_Exit(EXIT_SUCCESS);
    }).detach();
#endif
    app_context().CallInUIThreadDeferred([this]() {
#if REX_PLATFORM_MAC
      app_context().QuitFromUIThread();
#else
      if (window()) {
        window()->RequestClose();
      } else {
        app_context().QuitFromUIThread();
      }
#endif
    });
  };
  auto restart_game = [this]() { RestartGame(); };
  // Raw pad poll for menu navigation: bypasses the is_active gate that
  // zeroes guest-facing input while the settings screen is open.
  auto poll_gamepad = [this]() {
    rex::ui::SimpleSettingsGamepad pad;
    // The dialog can be painted (and thus poll) during a frame that races
    // runtime teardown at close time, when runtime() is already gone.
    auto* rt = runtime();
    auto* input_system =
        rt ? static_cast<rex::input::InputSystem*>(rt->input_system()) : nullptr;
    if (input_system) {
      rex::input::X_INPUT_GAMEPAD state;
      if (input_system->GetUiGamepadState(&state)) {
        pad.connected = true;
        pad.buttons = state.buttons;
        pad.thumb_lx = state.thumb_lx;
        pad.thumb_ly = state.thumb_ly;
      }
    }
    return pad;
  };

  auto load_mods = [this]() -> std::vector<rex::ui::SimpleModInfo> {
    std::vector<rex::ui::SimpleModInfo> result;
    const auto app_root = rex::filesystem::GetAppRootFolder();
    const std::vector<std::filesystem::path> candidate_pack_dirs = {
        app_root / "mods" / "raw",
        app_root.parent_path() / "mods" / "raw",
        app_root / "custom_packs",
        app_root.parent_path() / "custom_packs",
        app_root.parent_path() / "build" / "custom_packs",
        app_root / "build" / "custom_packs",
    };
    std::filesystem::path packs_dir;
    for (const auto& cpd : candidate_pack_dirs) {
      if (std::filesystem::is_directory(cpd)) {
        packs_dir = cpd;
        break;
      }
    }
    if (packs_dir.empty() || !std::filesystem::is_directory(packs_dir)) {
      return result;
    }
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(packs_dir, ec)) {
      if (!entry.is_directory()) continue;
      const std::string folder_name = entry.path().filename().string();
      const bool is_disabled_folder =
          folder_name.ends_with(".disabled") || folder_name.ends_with(".off") ||
          folder_name.ends_with(".bak") || folder_name.ends_with(".tmp");
      const auto manifest_path = entry.path() / "pack.json";
      if (!std::filesystem::exists(manifest_path)) continue;
      try {
        std::ifstream f(manifest_path);
        if (!f.is_open()) continue;
        auto j = nlohmann::json::parse(f, nullptr, false);
        if (j.is_discarded() || !j.is_object()) continue;
        rex::ui::SimpleModInfo info;
        info.id = j.value("id", entry.path().stem().string());
        info.name = j.value("name", info.id);
        info.description = j.value("description", "Custom challenge mod.");
        info.version = j.value("version", "1.0.0");
        const bool manifest_enabled = j.value("enabled", true);
        info.enabled = manifest_enabled && !is_disabled_folder;
        result.push_back(std::move(info));
      } catch (...) {
      }
    }
    std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) {
      return a.name < b.name;
    });
    return result;
  };

  auto save_mods = [this](const std::vector<rex::ui::SimpleModInfo>& mods) {
    const auto app_root = rex::filesystem::GetAppRootFolder();
    const std::vector<std::filesystem::path> candidate_pack_dirs = {
        app_root / "mods" / "raw",
        app_root.parent_path() / "mods" / "raw",
        app_root / "custom_packs",
        app_root.parent_path() / "custom_packs",
        app_root.parent_path() / "build" / "custom_packs",
        app_root / "build" / "custom_packs",
    };
    std::filesystem::path packs_dir;
    for (const auto& cpd : candidate_pack_dirs) {
      if (std::filesystem::is_directory(cpd)) {
        packs_dir = cpd;
        break;
      }
    }
    if (packs_dir.empty() || !std::filesystem::is_directory(packs_dir)) {
      return;
    }
    for (const auto& mod : mods) {
      std::error_code ec;
      for (const auto& entry : std::filesystem::directory_iterator(packs_dir, ec)) {
        if (!entry.is_directory()) continue;
        auto manifest_path = entry.path() / "pack.json";
        if (!std::filesystem::exists(manifest_path)) continue;
        try {
          nlohmann::json j;
          {
            std::ifstream f(manifest_path);
            j = nlohmann::json::parse(f, nullptr, false);
          }
          if (j.is_discarded() || !j.is_object()) continue;
          const std::string current_id = j.value("id", entry.path().stem().string());
          if (current_id == mod.id) {
            j["enabled"] = mod.enabled;
            auto current_path = entry.path();
            if (mod.enabled && (current_path.filename().string().ends_with(".disabled") ||
                                current_path.filename().string().ends_with(".off"))) {
              std::string clean = current_path.filename().string();
              if (clean.ends_with(".disabled")) clean.resize(clean.size() - 9);
              else if (clean.ends_with(".off")) clean.resize(clean.size() - 4);
              auto target_path = current_path.parent_path() / clean;
              std::error_code ren_ec;
              std::filesystem::rename(current_path, target_path, ren_ec);
              if (!ren_ec) {
                current_path = target_path;
                manifest_path = current_path / "pack.json";
              }
            }
            std::ofstream out(manifest_path);
            out << j.dump(2) << std::endl;
            REXLOG_INFO("Skate 3 mods: updated '{}' ({}) enabled = {}", mod.name, mod.id, mod.enabled);
            break;
          }
        } catch (...) {
        }
      }
    }
  };

  simple_settings_dialog_ =
      std::make_unique<rex::ui::SimpleSettingsDialog>(
          imgui_drawer(), user_settings_path_, std::move(load_profiles), std::move(save_profile),
          std::move(close_settings), std::move(close_game), std::move(restart_game),
          std::move(poll_gamepad), std::move(load_mods), std::move(save_mods));
  ApplySettingsCursorMode();
  skate3::native_scene::SetSettingsMenuBlur(true);
  simple_settings_dialog_->Show();
}

void Skate3BaseApp::ToggleNativeDebug() {
  if (!native_debug_dialog_) {
    native_debug_dialog_ = std::make_unique<skate3::NativeDebugDialog>(imgui_drawer());
  }
  if (native_debug_dialog_->visible()) {
    native_debug_dialog_->Hide();
    ApplyGameplayCursorMode();
  } else {
    ApplySettingsCursorMode();
    native_debug_dialog_->Show();
  }
}

void Skate3BaseApp::ApplySettingsCursorMode() {
  if (window()) {
    window()->SetCursorVisibility(rex::ui::Window::CursorVisibility::kVisible);
  }
}

void Skate3BaseApp::ApplyGameplayCursorMode() {
  if (window()) {
    window()->SetCursorVisibility(rex::ui::Window::CursorVisibility::kAutoHidden);
  }
}

void Skate3BaseApp::RestartGame() {
  app_context().CallInUIThreadDeferred([this]() {
#if defined(_WIN32)
    wchar_t executable_path[MAX_PATH] = {};
    if (!GetModuleFileNameW(nullptr, executable_path, MAX_PATH)) {
      REXLOG_WARN("Restart requested, but the executable path could not be resolved");
      return;
    }

    std::wstring command_line = GetCommandLineW();
    STARTUPINFOW startup_info{};
    startup_info.cb = sizeof(startup_info);
    PROCESS_INFORMATION process_info{};
    if (!CreateProcessW(executable_path, command_line.data(), nullptr, nullptr, FALSE, 0, nullptr,
                        nullptr, &startup_info, &process_info)) {
      REXLOG_WARN("Restart requested, but launching a new process failed");
      return;
    }
    CloseHandle(process_info.hThread);
    CloseHandle(process_info.hProcess);
#elif defined(__linux__) || defined(__APPLE__)
    const auto executable_path = rex::filesystem::GetExecutablePath();
    if (executable_path.empty()) {
      REXLOG_WARN("Restart requested, but the executable path could not be resolved");
      return;
    }

    auto args = CurrentProcessArgumentsForRestart(executable_path);
    std::filesystem::path restart_game_data_root;
    if (runtime()) {
      restart_game_data_root = runtime()->game_data_root();
    }
    if (restart_game_data_root.empty()) {
      restart_game_data_root = game_data_root();
    }
    if (!restart_game_data_root.empty()) {
      SetRestartArgument(args, "game_data_root", rex::path_to_utf8(restart_game_data_root));
    }
    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (auto& arg : args) {
      argv.push_back(arg.data());
    }
    argv.push_back(nullptr);

    const std::string executable = rex::path_to_utf8(executable_path);
    pid_t child_pid = 0;
    const int spawn_result =
        posix_spawn(&child_pid, executable.c_str(), nullptr, nullptr, argv.data(), environ);
    if (spawn_result != 0) {
      REXLOG_WARN("Restart requested, but launching a new process failed: {}",
                  std::strerror(spawn_result));
      return;
    }
#else
    REXLOG_WARN("Restart requested, but automatic restart is not implemented on this platform");
    return;
#endif

  #if REX_PLATFORM_MAC
    app_context().QuitFromUIThread();
  #else
    if (window()) {
      window()->RequestClose();
    } else {
      app_context().QuitFromUIThread();
    }
  #endif
  });
}

void Skate3BaseApp::SaveDrawFingerprintLog() {
#ifndef REXGLUE_ENABLE_PERF_COUNTERS
  REXLOG_WARN("Perf capture is unavailable because perf counters are disabled in this build");
  return;
#else
  auto now = std::chrono::system_clock::now();
  std::time_t now_time = std::chrono::system_clock::to_time_t(now);
  std::tm local_time{};
#if defined(_WIN32)
  localtime_s(&local_time, &now_time);
#else
  localtime_r(&now_time, &local_time);
#endif

  std::ostringstream counters_filename;
  counters_filename << "perf_capture_counters_" << std::put_time(&local_time, "%Y%m%d_%H%M%S")
                    << ".csv";
  std::ostringstream filename;
  filename << "perf_capture_draw_fingerprints_" << std::put_time(&local_time, "%Y%m%d_%H%M%S")
           << ".csv";

  std::error_code ec;
  std::filesystem::create_directories(cache_root(), ec);
  const auto counters_path = cache_root() / counters_filename.str();
  const auto path = cache_root() / filename.str();
  if (rex::perf::StartCapture(counters_path, path)) {
    REXLOG_INFO("Started perf capture; counter log will be saved to {}",
                counters_path.string());
    REXLOG_INFO("Started perf capture; draw fingerprint log will be saved to {}",
                path.string());
  } else {
    REXLOG_WARN("Perf capture is already running");
  }
#endif
}

void Skate3BaseApp::LogUserMarker() {
  const uint32_t marker = debug_marker_count_.fetch_add(1, std::memory_order_relaxed) + 1;
  REXLOG_WARN("USER LOG MARKER #{}: F10 pressed", marker);
}

void Skate3BaseApp::LogDebugMarker() {
  const uint32_t marker = debug_marker_count_.fetch_add(1, std::memory_order_relaxed) + 1;
  DisableActiveDebugDiagnostics();
  REXLOG_WARN("USER DEBUG MARKER #{}: F9 pressed; active diagnostics disabled", marker);
}

void Skate3BaseApp::ApplySelectedProfileToRuntime() {
  if (!runtime() || !runtime()->kernel_state() || !runtime()->kernel_state()->user_profile()) {
    return;
  }
  auto profiles = skate3::LoadProfiles(profiles_path_);
  if (auto* profile = skate3::FindSelectedProfile(profiles)) {
    skate3::ApplyProfileCvars(*profile);
    ApplyDemoPathProfileOverride();
    runtime()->kernel_state()->user_profile()->SetIdentity(profile->xuid, profile->gamertag);
  }
}

bool Skate3BaseApp::IsRecipeNameChar(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
         (c >= '0' && c <= '9') || c == '_' || c == '-';
}

std::set<std::string> Skate3BaseApp::DiscoverRecipeAliases(
    const std::filesystem::path& content_root) {
  static constexpr size_t kScanBytes = 8 * 1024 * 1024;
  static constexpr std::string_view kPrefix = "data/content/recipe/";

  std::set<std::string> aliases;
  std::error_code ec;
  for (const auto& entry : std::filesystem::directory_iterator(content_root, ec)) {
    if (ec || !entry.is_regular_file() || entry.path().extension() != ".big") {
      continue;
    }

    std::ifstream stream(entry.path(), std::ios::binary);
    if (!stream) {
      continue;
    }

    std::string buffer;
    buffer.resize(kScanBytes);
    stream.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    buffer.resize(static_cast<size_t>(stream.gcount()));

    size_t pos = 0;
    while ((pos = buffer.find(kPrefix, pos)) != std::string::npos) {
      size_t name_start = pos + kPrefix.size();
      size_t name_end = name_start;
      while (name_end < buffer.size() && IsRecipeNameChar(buffer[name_end])) {
        ++name_end;
      }
      if (name_end > name_start) {
        aliases.insert(buffer.substr(name_start, name_end - name_start));
      }
      pos = name_end;
    }
  }

  return aliases;
}

bool Skate3BaseApp::CreateOverlayDirectory(
    const std::filesystem::path& overlay_root, std::string_view guest_path) {
  std::filesystem::path path = overlay_root;
  size_t segment_start = 0;
  while (segment_start < guest_path.size()) {
    const size_t slash = guest_path.find('/', segment_start);
    const size_t segment_end = slash == std::string_view::npos ? guest_path.size() : slash;
    if (segment_end > segment_start) {
      path /= std::string(guest_path.substr(segment_start, segment_end - segment_start));
    }
    if (slash == std::string_view::npos) {
      break;
    }
    segment_start = slash + 1;
  }

  std::error_code ec;
  std::filesystem::create_directories(path, ec);
  return !ec;
}

void Skate3BaseApp::InstallRecipeOverlay() {
  if (recipe_overlay_installed_ || !runtime() || !runtime()->file_system()) {
    return;
  }

  const auto content_root = runtime()->game_data_root() / "data" / "content";
  if (!std::filesystem::exists(content_root)) {
    REXLOG_WARN("Skipping Skate 3 recipe VFS overlay; content root not found: {}",
                content_root.string());
    return;
  }

  const auto aliases = DiscoverRecipeAliases(content_root);
  if (aliases.empty()) {
    REXLOG_WARN("Skipping Skate 3 recipe VFS overlay; no recipe aliases found in {}",
                content_root.string());
    return;
  }

  const auto overlay_root = cache_root() / "vfs_big_directory_aliases";
  std::error_code ec;
  std::filesystem::create_directories(overlay_root, ec);
  if (ec) {
    REXLOG_WARN("Skipping Skate 3 BIG-directory VFS overlay; failed to create {}: {}",
                overlay_root.string(), ec.message());
    return;
  }

  size_t created = 0;
  for (const auto& alias : aliases) {
    const std::string guest_path = std::string("data/content/recipe/") + alias;
    if (CreateOverlayDirectory(overlay_root, guest_path)) {
      ++created;
    }
  }

  static constexpr std::string_view kBigDirectoryAliases[] = {
      "data/scene",
      "data/scene/anim",
      "data/scene/trickguide",
      "data/livingworld/PluginDescriptor",
      "data/state/livingworldentities/pedestrian/plugin",
  };
  for (std::string_view alias : kBigDirectoryAliases) {
    if (CreateOverlayDirectory(overlay_root, alias)) {
      ++created;
    }
  }

  if (!created) {
    REXLOG_WARN("Skipping Skate 3 BIG-directory VFS overlay; failed to create alias directories in {}",
                overlay_root.string());
    return;
  }

  auto device = std::make_unique<rex::filesystem::HostPathDevice>(
      "skate3bigdirs:", overlay_root, true);
  if (!device->Initialize()) {
    REXLOG_WARN("Skipping Skate 3 BIG-directory VFS overlay; failed to initialize host device for {}",
                overlay_root.string());
    return;
  }

  runtime()->file_system()->RegisterDevice(std::move(device));
  runtime()->file_system()->RegisterSymbolicLink(
      "\\Device\\Harddisk0\\Partition1\\data\\content\\recipe",
      "skate3bigdirs:\\data\\content\\recipe");
  runtime()->file_system()->RegisterSymbolicLink(
      "\\Device\\Harddisk0\\Partition1\\data\\scene",
      "skate3bigdirs:\\data\\scene");
  runtime()->file_system()->RegisterSymbolicLink(
      "\\Device\\Harddisk0\\Partition1\\data\\livingworld\\PluginDescriptor",
      "skate3bigdirs:\\data\\livingworld\\PluginDescriptor");
  runtime()->file_system()->RegisterSymbolicLink(
      "\\Device\\Harddisk0\\Partition1\\data\\state\\livingworldentities\\pedestrian\\plugin",
      "skate3bigdirs:\\data\\state\\livingworldentities\\pedestrian\\plugin");
  recipe_overlay_installed_ = true;
  REXLOG_INFO("Installed Skate 3 BIG-directory VFS overlay with {} aliases from {}",
              created, content_root.string());
}

void Skate3BaseApp::InstallBigDeviceAliases() {
  if (big_device_aliases_installed_ || !runtime() || !runtime()->file_system()) {
    return;
  }

  // Skate 3 uses these file-server style schemes for world content probes.
  // Route them to the normal title mount so existing disc files resolve.
  runtime()->file_system()->RegisterSymbolicLink(
      "big:", "\\Device\\Harddisk0\\Partition1");
  runtime()->file_system()->RegisterSymbolicLink(
      "dlcbig:", "\\Device\\Harddisk0\\Partition1");
  big_device_aliases_installed_ = true;
}

namespace {

using rex::X_STATUS;

#pragma pack(push, 1)
struct ModFileHeader {
  uint32_t magic;         // 'S3DL' (0x5333444C)
  uint32_t version;       // 1
  uint64_t virtual_size;
  uint64_t base_size;
  uint32_t chunk_count;
  uint32_t reserved;
  char base_name[64];
};

struct ModChunkHeader {
  uint64_t offset;
  uint32_t length;
  uint32_t reserved;
};
#pragma pack(pop)

struct ModChunk {
  uint64_t offset = 0;
  uint32_t length = 0;
  std::vector<uint8_t> data;
};

struct ModOverlayData {
  std::string base_name;
  uint64_t virtual_size = 0;
  uint64_t base_size = 0;
  std::vector<ModChunk> chunks;
  std::filesystem::path stock_path;
};

std::shared_ptr<ModOverlayData> LoadModOverlay(const std::filesystem::path& mod_path) {
  std::ifstream file(mod_path, std::ios::binary);
  if (!file) {
    REXLOG_ERROR("Skate 3 mods: cannot open {}", mod_path.string());
    return nullptr;
  }
  ModFileHeader hdr{};
  file.read(reinterpret_cast<char*>(&hdr), sizeof(hdr));
  if (file.gcount() != sizeof(hdr)) {
    REXLOG_ERROR("Skate 3 mods: header read failed {}", mod_path.string());
    return nullptr;
  }
  if (hdr.magic != 0x5333444C) {  // 'S3DL'
    REXLOG_ERROR("Skate 3 mods: bad magic {:08X} in {}", hdr.magic, mod_path.string());
    return nullptr;
  }

  auto data = std::make_shared<ModOverlayData>();
  hdr.base_name[sizeof(hdr.base_name) - 1] = '\0';
  data->base_name = hdr.base_name;
  data->virtual_size = hdr.virtual_size;
  data->base_size = hdr.base_size;

  for (uint32_t i = 0; i < hdr.chunk_count; ++i) {
    ModChunkHeader chdr{};
    file.read(reinterpret_cast<char*>(&chdr), sizeof(chdr));
    if (file.gcount() != sizeof(chdr)) {
      REXLOG_ERROR("Skate 3 mods: chunk {} header read failed in {}", i, mod_path.string());
      return nullptr;
    }
    ModChunk chunk;
    chunk.offset = chdr.offset;
    chunk.length = chdr.length;
    chunk.data.resize(chdr.length);
    if (chdr.length > 0) {
      file.read(reinterpret_cast<char*>(chunk.data.data()), chdr.length);
      if (static_cast<uint32_t>(file.gcount()) != chdr.length) {
        REXLOG_ERROR("Skate 3 mods: chunk {} payload read failed in {}", i, mod_path.string());
        return nullptr;
      }
    }
    data->chunks.push_back(std::move(chunk));
  }
  return data;
}

class ModOverlayFile : public rex::filesystem::File {
 public:
  ModOverlayFile(uint32_t file_access, rex::filesystem::Entry* entry,
                 std::shared_ptr<ModOverlayData> mod_data,
                 std::unique_ptr<rex::filesystem::FileHandle> base_file)
      : rex::filesystem::File(file_access, entry),
        mod_data_(std::move(mod_data)),
        base_file_(std::move(base_file)) {}

  ~ModOverlayFile() override = default;

  void Destroy() override {
    delete this;
  }

  X_STATUS ReadSync(std::span<uint8_t> buffer, size_t byte_offset,
                    size_t* out_bytes_read) override {
    if (!(file_access_ & (rex::filesystem::FileAccess::kGenericRead |
                          rex::filesystem::FileAccess::kFileReadData))) {
      return X_STATUS_ACCESS_DENIED;
    }
    if (byte_offset >= mod_data_->virtual_size) {
      if (out_bytes_read) *out_bytes_read = 0;
      return X_STATUS_END_OF_FILE;
    }

    size_t to_read = buffer.size();
    if (byte_offset + to_read > mod_data_->virtual_size) {
      to_read = static_cast<size_t>(mod_data_->virtual_size - byte_offset);
    }

    // 1. Read base data if within base_size and base_file is available
    if (byte_offset < mod_data_->base_size && base_file_) {
      const size_t base_read_len =
          std::min(to_read, static_cast<size_t>(mod_data_->base_size - byte_offset));
      size_t bytes_read = 0;
      if (!base_file_->Read(byte_offset, buffer.data(), base_read_len, &bytes_read)) {
        std::memset(buffer.data(), 0, base_read_len);
      }
      if (to_read > base_read_len) {
        std::memset(buffer.data() + base_read_len, 0, to_read - base_read_len);
      }
    } else {
      std::memset(buffer.data(), 0, to_read);
    }

    // 2. Overlay chunks
    const uint64_t req_start = byte_offset;
    const uint64_t req_end = byte_offset + to_read;

    for (const auto& chunk : mod_data_->chunks) {
      const uint64_t chunk_start = chunk.offset;
      const uint64_t chunk_end = chunk.offset + chunk.length;
      if (chunk_start < req_end && chunk_end > req_start) {
        const uint64_t ov_start = std::max(req_start, chunk_start);
        const uint64_t ov_end = std::min(req_end, chunk_end);
        const size_t dest_off = static_cast<size_t>(ov_start - req_start);
        const size_t src_off = static_cast<size_t>(ov_start - chunk_start);
        const size_t ov_len = static_cast<size_t>(ov_end - ov_start);
        std::memcpy(buffer.data() + dest_off, chunk.data.data() + src_off, ov_len);
      }
    }

    if (out_bytes_read) {
      *out_bytes_read = to_read;
    }
    return X_STATUS_SUCCESS;
  }

  X_STATUS WriteSync(std::span<const uint8_t> buffer, size_t byte_offset,
                     size_t* out_bytes_written) override {
    return X_STATUS_ACCESS_DENIED;
  }

  X_STATUS SetLength(size_t length) override {
    return X_STATUS_ACCESS_DENIED;
  }

 private:
  std::shared_ptr<ModOverlayData> mod_data_;
  std::unique_ptr<rex::filesystem::FileHandle> base_file_;
};

class ModOverlayEntry : public rex::filesystem::Entry {
 public:
  ModOverlayEntry(rex::filesystem::Device* device, rex::filesystem::Entry* parent,
                  const std::string_view path,
                  std::shared_ptr<ModOverlayData> mod_data)
      : rex::filesystem::Entry(device, parent, path),
        mod_data_(std::move(mod_data)) {
    attributes_ = rex::filesystem::kFileAttributeNormal | rex::filesystem::kFileAttributeReadOnly;
    size_ = static_cast<size_t>(mod_data_->virtual_size);
    allocation_size_ = (size_ + 0xFFF) & ~static_cast<size_t>(0xFFF);
  }

  X_STATUS Open(uint32_t desired_access, rex::filesystem::File** out_file) override {
    std::unique_ptr<rex::filesystem::FileHandle> base_handle;
    if (!mod_data_->stock_path.empty() && std::filesystem::exists(mod_data_->stock_path)) {
      base_handle = rex::filesystem::FileHandle::OpenExisting(
          mod_data_->stock_path, rex::filesystem::FileAccess::kFileReadData);
    }
    if (!base_handle && mod_data_->base_size > 0) {
      REXLOG_ERROR("Skate 3 mod overlay: failed to open stock base file {}",
                   mod_data_->stock_path.string());
      return X_STATUS_NO_SUCH_FILE;
    }
    *out_file = new ModOverlayFile(desired_access, this, mod_data_, std::move(base_handle));
    return X_STATUS_SUCCESS;
  }

 private:
  std::shared_ptr<ModOverlayData> mod_data_;
};

class LoggingOverrideDevice : public rex::filesystem::HostPathDevice {
 public:
  LoggingOverrideDevice(const std::string_view mount_path,
                        const std::filesystem::path& host_path)
      : rex::filesystem::HostPathDevice(mount_path, host_path, false) {}

  void RegisterModEntry(const std::string& name, std::shared_ptr<ModOverlayData> mod_data) {
    auto entry = std::make_unique<ModOverlayEntry>(this, nullptr, "\\" + name, std::move(mod_data));
    mod_entries_[name] = std::move(entry);
  }

  rex::filesystem::Entry* ResolvePath(const std::string_view path) override {
    std::string normalized(path);
    while (!normalized.empty() && (normalized.front() == '\\' || normalized.front() == '/')) {
      normalized.erase(normalized.begin());
    }
    auto it = mod_entries_.find(normalized);
    if (it != mod_entries_.end()) {
      REXLOG_INFO("Skate 3 mods: RESOLVE (MOD OVERLAY) '{}' -> FOUND", path);
      return it->second.get();
    }
    auto* entry = rex::filesystem::HostPathDevice::ResolvePath(path);
    REXLOG_INFO("Skate 3 mods: RESOLVE '{}' -> {}",
                path, entry ? "FOUND" : "NOT-FOUND");
    return entry;
  }

 private:
  std::unordered_map<std::string, std::unique_ptr<ModOverlayEntry>> mod_entries_;
};

}  // namespace

void Skate3BaseApp::InstallModOverrides() {
  if (mods_installed_ || !runtime() || !runtime()->file_system()) {
    return;
  }
  if (!REXCVAR_GET(skate3_mods) || !REXCVAR_GET(skate3_custom_bigs)) {
    REXLOG_INFO("Skate 3 mods: disabled via cvar");
    return;
  }

  const auto app_root = rex::filesystem::GetAppRootFolder();
  auto custom_root = std::string(REXCVAR_GET(skate3_mods_dir));
  if (custom_root.empty()) {
    custom_root = std::string(REXCVAR_GET(skate3_custom_bigs_dir));
  }
  std::filesystem::path custom_dir;
  if (!custom_root.empty()) {
    custom_dir = std::filesystem::path(custom_root);
  } else {
    const std::vector<std::filesystem::path> search_paths = {
        app_root / "mods" / "compiled",
        app_root.parent_path() / "mods" / "compiled",
        runtime()->game_data_root() / "mods" / "compiled",
        runtime()->game_data_root().parent_path() / "mods" / "compiled",
        app_root / "custom_bigs",
        app_root.parent_path() / "custom_bigs",
        app_root.parent_path() / "build" / "custom_bigs",
        app_root / "build" / "custom_bigs",
    };
    for (const auto& sp : search_paths) {
      if (std::filesystem::exists(sp)) {
        custom_dir = sp;
        break;
      }
    }
    if (custom_dir.empty()) {
      custom_dir = app_root / "mods" / "compiled";
    }
  }
  std::error_code dir_ec;
  std::filesystem::create_directories(custom_dir, dir_ec);

  // Raw Mod Packages
  const std::vector<std::filesystem::path> candidate_pack_dirs = {
      app_root / "mods" / "raw",
      app_root.parent_path() / "mods" / "raw",
      runtime()->game_data_root() / "mods" / "raw",
      runtime()->game_data_root().parent_path() / "mods" / "raw",
      app_root / "custom_packs",
      app_root.parent_path() / "custom_packs",
      app_root.parent_path() / "build" / "custom_packs",
      app_root / "build" / "custom_packs",
  };
  std::filesystem::path packs_dir;
  for (const auto& cpd : candidate_pack_dirs) {
    if (std::filesystem::is_directory(cpd)) {
      packs_dir = cpd;
      break;
    }
  }
  if (packs_dir.empty()) {
    packs_dir = app_root / "mods" / "raw";
  }

  std::filesystem::path compositor_script = app_root.parent_path() / "tools" / "pack_compositor.py";
  if (!std::filesystem::exists(compositor_script)) {
    compositor_script = app_root / "tools" / "pack_compositor.py";
  }
  if (!std::filesystem::exists(compositor_script)) {
    compositor_script = runtime()->game_data_root() / "tools" / "pack_compositor.py";
  }
  if (!std::filesystem::exists(compositor_script)) {
    compositor_script = runtime()->game_data_root().parent_path() / "tools" / "pack_compositor.py";
  }

  const bool has_packs_dir = std::filesystem::is_directory(packs_dir);
  if (has_packs_dir && std::filesystem::exists(compositor_script)) {
    // Only recompile if compiled mods are missing or if a raw pack manifest is newer than .active
    bool needs_recompile = false;
    const auto active_marker = custom_dir / ".active";
    std::error_code active_ec;
    if (!std::filesystem::exists(active_marker, active_ec) ||
        !std::filesystem::exists(custom_dir / "db_custom.mod", active_ec) ||
        !std::filesystem::exists(custom_dir / "missions_custom.mod", active_ec) ||
        !std::filesystem::exists(custom_dir / "global_locators_univ.rx2", active_ec)) {
      needs_recompile = true;
    } else {
      const auto active_time = std::filesystem::last_write_time(active_marker, active_ec);
      if (!active_ec) {
        try {
          for (const auto& entry : std::filesystem::recursive_directory_iterator(
                   packs_dir, std::filesystem::directory_options::skip_permission_denied, active_ec)) {
            if (active_ec) break;
            if (entry.is_regular_file(active_ec) && entry.path().extension() == ".json") {
              const auto mod_time = entry.last_write_time(active_ec);
              if (!active_ec && mod_time > active_time) {
                needs_recompile = true;
                break;
              }
            }
          }
        } catch (...) {
          // Ignore iterator errors
        }
      }
    }

    if (needs_recompile) {
      std::string py_bin;
#ifdef _WIN32
      const std::vector<std::filesystem::path> local_pythons = {
          app_root / "tools" / "python" / "python.exe",
          app_root.parent_path() / "tools" / "python" / "python.exe",
          runtime()->game_data_root() / "tools" / "python" / "python.exe",
          runtime()->game_data_root().parent_path() / "tools" / "python" / "python.exe",
          app_root / "mods" / ".python" / "python.exe",
      };
      for (const auto& lp : local_pythons) {
        if (std::filesystem::exists(lp, active_ec)) {
          py_bin = "\"" + lp.string() + "\"";
          break;
        }
      }
      if (py_bin.empty() && std::system("python --version >nul 2>&1") == 0) {
        py_bin = "python";
      }
#else
      if (std::system("python3 --version >/dev/null 2>&1") == 0) {
        py_bin = "python3";
      } else if (std::system("python --version >/dev/null 2>&1") == 0) {
        py_bin = "python";
      }
#endif

      if (!py_bin.empty()) {
        const std::string sync_cmd = py_bin + " \"" + compositor_script.string() + "\" --packs-dir \"" +
                                     packs_dir.string() + "\" --out-dir \"" + custom_dir.string() + "\"";
        REXLOG_INFO("Skate 3 mods: syncing raw packs in {}...", packs_dir.string());
        const int status = std::system(sync_cmd.c_str());
        if (status != 0) {
          REXLOG_WARN("Skate 3 mods: compositor sync returned status {}", status);
        }
      } else {
        REXLOG_INFO("Skate 3 mods: Python not available to recompile mods; using existing compiled assets.");
      }
    }
  }

  // If modular custom packs are in use, only mount overrides when at least one pack is active.
  if (has_packs_dir && !std::filesystem::exists(custom_dir / ".active")) {
    REXLOG_INFO("Skate 3 mods: no active challenge packs, using stock retail data");
    return;
  }

  if (!std::filesystem::is_directory(custom_dir)) {
    REXLOG_WARN("Skate 3 mods: directory not found, override disabled: {}",
                custom_dir.string());
    return;
  }

  struct OverrideMapping {
    const char* guest_subpath;  // under the partition root, backslash form
    const char* host_file;      // inside custom_dir
    bool is_dlc_big = false;
  };
  static constexpr OverrideMapping kMappings[] = {
      {"data\\big\\db.big", "db_custom.big", false},
      {"data\\big\\miscboot.big", "miscboot_custom.big", false},
      {"data\\content\\missions.big", "missions_custom.big", false},
      // Custom OTS map pins: District global locators (University, Downtown, Industrial)
      {"data\\content\\global_locators\\BAM\\DIST_University\\"
       "DIST_Univers_[0x0f7a42a9282864ac].rx2",
       "global_locators_univ.rx2", false},
      {"data\\content\\global_locators\\BAM\\DIST_DownTown\\"
       "DIST_DownTow_[0x71ff8817a4de4033].rx2",
       "global_locators_dt.rx2", false},
      {"data\\content\\global_locators\\BAM\\DIST_Industrial\\"
       "DIST_Industr_[0xa5cc938d0194a85d].rx2",
       "global_locators_ind.rx2", false},
      // Official DLC Big Overrides:
      // Danny Way's Hawaiian Dream
      {"dway_park_00000000.big", "dway_park_custom.big", true},
      // San Van Party Pack (Art Gallery & Zen Plaza / Marraffa's)
      {"play_00000000.big", "play_custom.big", true},
      // Maloof Money Cup 2010 NYC
      {"maloof_money_cup_00000000.big", "maloof_custom.big", true},
      // Black Box Park
      {"presell_00000000.big", "presell_custom.big", true},
  };

  auto device = std::make_unique<LoggingOverrideDevice>(
      "skate3mods:", custom_dir);
  if (!device->Initialize()) {
    REXLOG_WARN("Skate 3 mods: failed to initialize host device for {}",
                custom_dir.string());
    return;
  }
  auto legacy_device = std::make_unique<LoggingOverrideDevice>(
      "skate3custombigs:", custom_dir);
  const bool has_legacy_device = legacy_device->Initialize();

  auto find_stock_file = [&](const std::string_view guest_subpath, bool is_dlc_big,
                             const std::string& base_hint) -> std::filesystem::path {
    const std::string filename = !base_hint.empty()
        ? base_hint
        : std::filesystem::path(guest_subpath).filename().string();

    std::error_code ec;

    // 1. Direct candidate paths relative to game_data_root and app_root
    std::string fwd_subpath(guest_subpath);
    std::replace(fwd_subpath.begin(), fwd_subpath.end(), '\\', '/');

    const std::vector<std::filesystem::path> direct_candidates = {
        runtime()->game_data_root() / guest_subpath,
        runtime()->game_data_root() / fwd_subpath,
        runtime()->game_data_root() / "data" / "big" / filename,
        runtime()->game_data_root() / "data" / "content" / filename,
        runtime()->game_data_root() / "content" / filename,
        runtime()->game_data_root() / filename,
        runtime()->game_data_root() / "dlc" / filename,
        app_root / "dlc" / filename,
        app_root / "stock_bigs" / filename,
        app_root.parent_path() / "build" / "stock_bigs" / filename,
        app_root / "build" / "stock_bigs" / filename,
        app_root.parent_path() / "stock_bigs" / filename,
        runtime()->user_data_root() / std::string(kDlcDirectoryName) / filename,
    };

    for (const auto& p : direct_candidates) {
      if (std::filesystem::is_regular_file(p, ec) && std::filesystem::file_size(p, ec) > 0) {
        return p;
      }
      ec.clear();
    }

    // 2. Safe shallow scan of user DLC drop folder
    try {
      const auto user_dlc = runtime()->user_data_root() / std::string(kDlcDirectoryName);
      if (std::filesystem::is_directory(user_dlc, ec)) {
        for (const auto& entry : std::filesystem::directory_iterator(
                 user_dlc, std::filesystem::directory_options::skip_permission_denied, ec)) {
          if (ec) break;
          if (entry.is_regular_file(ec) && entry.path().filename() == filename &&
              std::filesystem::file_size(entry.path(), ec) > 0) {
            return entry.path();
          }
        }
      }
    } catch (...) {
      // Ignore any filesystem errors during scanning
    }

    return {};
  };

  size_t installed = 0;
  auto* fs = runtime()->file_system();
  std::vector<std::pair<std::string, std::string>> active_dlc_mappings;
  for (const auto& mapping : kMappings) {
    std::filesystem::path mod_path = custom_dir / mapping.host_file;
    mod_path.replace_extension(".mod");
    std::filesystem::path delta_path = custom_dir / mapping.host_file;
    delta_path.replace_extension(".delta");
    const auto direct_path = custom_dir / mapping.host_file;

    const bool use_mod = std::filesystem::exists(mod_path);
    const bool use_delta = !use_mod && std::filesystem::exists(delta_path);
    const bool use_direct = !use_mod && !use_delta && std::filesystem::exists(direct_path);

    if (!use_mod && !use_delta && !use_direct) {
      REXLOG_WARN("Skate 3 mods: {} not found (.mod, .delta, or direct file), skipped",
                  mapping.host_file);
      continue;
    }

    if (use_mod || use_delta) {
      const auto overlay_path = use_mod ? mod_path : delta_path;
      auto mod_data = LoadModOverlay(overlay_path);
      if (mod_data) {
        mod_data->stock_path =
            find_stock_file(mapping.guest_subpath, mapping.is_dlc_big, mod_data->base_name);
        if (mod_data->stock_path.empty() && mod_data->base_size > 0) {
          REXLOG_ERROR("Skate 3 mods: stock base file not found for overlay: {}",
                       overlay_path.string());
          continue;
        }
        REXLOG_INFO("Skate 3 mods: MOD OVERLAY active: {} (base={})",
                    overlay_path.filename().string(), mod_data->stock_path.string());
        device->RegisterModEntry(mapping.host_file, mod_data);
        device->RegisterModEntry(overlay_path.filename().string(), mod_data);
        if (has_legacy_device) {
          legacy_device->RegisterModEntry(mapping.host_file, mod_data);
          legacy_device->RegisterModEntry(overlay_path.filename().string(), mod_data);
        }
      } else {
        REXLOG_ERROR("Skate 3 mods: failed to load mod overlay: {}", overlay_path.string());
        continue;
      }
    }

    const std::string target =
        std::string("skate3mods:\\") + mapping.host_file;
    const std::string legacy_target =
        std::string("skate3custombigs:\\") + mapping.host_file;

    if (!mapping.is_dlc_big) {
      const std::string guest_bs =
          std::string("\\Device\\Harddisk0\\Partition1\\") + mapping.guest_subpath;
      std::string d_scheme = std::string("d:\\") + mapping.guest_subpath;
      std::string big_scheme = std::string("big:") + mapping.guest_subpath;
      // Forward-slash variants of the same paths.
      std::string guest_bs_fwd = guest_bs;
      std::string d_scheme_fwd = d_scheme;
      std::string big_scheme_fwd = big_scheme;
      auto slashes = [](std::string& s) {
        std::replace(s.begin(), s.end(), '\\', '/');
      };
      slashes(guest_bs_fwd);
      slashes(d_scheme_fwd);
      slashes(big_scheme_fwd);
      fs->RegisterSymbolicLink(guest_bs, target);
      fs->RegisterSymbolicLink(guest_bs_fwd, target);
      fs->RegisterSymbolicLink(d_scheme, target);
      fs->RegisterSymbolicLink(d_scheme_fwd, target);
      fs->RegisterSymbolicLink(big_scheme, target);
      fs->RegisterSymbolicLink(big_scheme_fwd, target);
    } else {
      active_dlc_mappings.emplace_back(std::string(mapping.guest_subpath), mapping.host_file);
      const std::string dlcbig_bs = std::string("dlcbig:\\") + mapping.guest_subpath;
      const std::string dlcbig_fwd = std::string("dlcbig:/") + mapping.guest_subpath;
      fs->RegisterSymbolicLink(dlcbig_bs, target);
      fs->RegisterSymbolicLink(dlcbig_fwd, target);
    }

    ++installed;
    REXLOG_INFO("Skate 3 mods: {} -> {} ({})", mapping.guest_subpath, target,
                use_mod ? "MOD OVERLAY" : (use_delta ? "DELTA OVERLAY" : "HOST FILE"));
  }

  if (!active_dlc_mappings.empty() && runtime()->kernel_state() &&
      runtime()->kernel_state()->content_manager()) {
    runtime()->kernel_state()->content_manager()->SetPackageMountCallbacks(
        [fs, active_dlc_mappings](const std::string_view root_name,
                                  const std::string_view device_path,
                                  const std::filesystem::path& package_path) {
          for (const auto& [guest_subpath, host_file] : active_dlc_mappings) {
            const auto target_file = package_path / guest_subpath;
            if (std::filesystem::exists(target_file)) {
              const std::string target = std::string("skate3mods:\\") + host_file;
              const std::string legacy_target = std::string("skate3custombigs:\\") + host_file;
              std::string dev_root(device_path);
              if (!dev_root.empty() && dev_root.back() != '\\' && dev_root.back() != '/') {
                dev_root += '\\';
              }
              std::string fwd_subpath = guest_subpath;
              std::replace(fwd_subpath.begin(), fwd_subpath.end(), '\\', '/');

              const std::string content_bs = dev_root + guest_subpath;
              const std::string content_bs_double = dev_root + "\\" + guest_subpath;
              const std::string content_fwd = dev_root + fwd_subpath;
              const std::string content_fwd_double = dev_root + "/" + fwd_subpath;
              const std::string root_bs = std::string(root_name) + ":\\" + guest_subpath;
              const std::string root_fwd = std::string(root_name) + ":/" + fwd_subpath;

              fs->RegisterSymbolicLink(content_bs, target);
              fs->RegisterSymbolicLink(content_bs_double, target);
              fs->RegisterSymbolicLink(content_fwd, target);
              fs->RegisterSymbolicLink(content_fwd_double, target);
              fs->RegisterSymbolicLink(root_bs, target);
              fs->RegisterSymbolicLink(root_fwd, target);
              REXLOG_INFO("Skate 3 DLC mod mounted: {} -> {} for device {} (package {})",
                          guest_subpath, target, device_path, root_name);
            }
          }
        },
        [fs, active_dlc_mappings](const std::string_view root_name,
                                  const std::string_view device_path) {
          for (const auto& [guest_subpath, host_file] : active_dlc_mappings) {
            std::string dev_root(device_path);
            if (!dev_root.empty() && dev_root.back() != '\\' && dev_root.back() != '/') {
              dev_root += '\\';
            }
            std::string fwd_subpath = guest_subpath;
            std::replace(fwd_subpath.begin(), fwd_subpath.end(), '\\', '/');

            fs->UnregisterSymbolicLink(dev_root + guest_subpath);
            fs->UnregisterSymbolicLink(dev_root + "\\" + guest_subpath);
            fs->UnregisterSymbolicLink(dev_root + fwd_subpath);
            fs->UnregisterSymbolicLink(dev_root + "/" + fwd_subpath);
            fs->UnregisterSymbolicLink(std::string(root_name) + ":\\" + guest_subpath);
            fs->UnregisterSymbolicLink(std::string(root_name) + ":/" + fwd_subpath);
          }
        });
  }

  runtime()->file_system()->RegisterDevice(std::move(device));
  if (has_legacy_device) {
    runtime()->file_system()->RegisterDevice(std::move(legacy_device));
  }

  if (installed) {
    mods_installed_ = true;
    REXLOG_INFO("Installed Skate 3 mod overrides ({} files) from {}", installed,
                custom_dir.string());
  }
}

void Skate3BaseApp::InstallDlcPackages() {
  if (!REXCVAR_GET(skate3_auto_install_dlc) || !runtime() || !runtime()->kernel_state() ||
      !runtime()->kernel_state()->content_manager()) {
    return;
  }

  const uint32_t title_id = runtime()->kernel_state()->title_id();
  if (title_id == 0) {
    REXLOG_WARN("Skipping Skate 3 DLC install; title ID is not available");
    return;
  }

  const auto user_dlc_root = runtime()->user_data_root() / std::string(kDlcDirectoryName);
  std::error_code create_ec;
  std::filesystem::create_directories(user_dlc_root, create_ec);
  if (create_ec) {
    REXLOG_WARN("Could not create Skate 3 DLC drop folder {}: {}", user_dlc_root.string(),
                create_ec.message());
  }

  // Purge legacy/prototype DLC packages from the installed marketplace directory.
  // Custom challenge packs are handled entirely via the modular mods system;
  // legacy STFS prototype packages with non-standard vault structures cause the
  // guest vault manager to spin infinitely during DLC scanning.
  const auto installed_marketplace_root =
      runtime()->user_data_root() / "0000000000000000" /
      fmt::format("{:08X}", title_id) / "00000002";
  const auto installed_headers_root =
      runtime()->user_data_root() / "0000000000000000" /
      fmt::format("{:08X}", title_id) / "Headers" / "00000002";

  if (std::filesystem::is_directory(installed_marketplace_root)) {
    std::error_code dir_ec;
    for (const auto& entry : std::filesystem::directory_iterator(installed_marketplace_root, dir_ec)) {
      if (dir_ec) break;
      std::string name = entry.path().filename().string();
      std::string name_lower = name;
      std::transform(name_lower.begin(), name_lower.end(), name_lower.begin(), ::tolower);
      if (name_lower.find("morshu") != std::string::npos) {
        REXLOG_INFO("Purging obsolete legacy prototype DLC: {}", entry.path().string());
        std::error_code rm_ec;
        std::filesystem::remove_all(entry.path(), rm_ec);
        auto header_path = installed_headers_root / (name + ".header");
        if (std::filesystem::exists(header_path)) {
          std::filesystem::remove(header_path, rm_ec);
        }
      }
    }
  }

  const auto source_dirs =
      DiscoverDlcSourceDirectories(rex::filesystem::GetAppRootFolder(),
                                   runtime()->game_data_root(), runtime()->user_data_root());
  std::unordered_set<std::string> seen_packages;
  size_t installed_count = 0;
  size_t skipped_count = 0;

  for (const auto& source_dir : source_dirs) {
    if (!std::filesystem::is_directory(source_dir)) {
      continue;
    }

    std::error_code iter_ec;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(source_dir, iter_ec)) {
      if (iter_ec) {
        REXLOG_WARN("Could not scan Skate 3 DLC folder {}: {}", source_dir.string(),
                    iter_ec.message());
        break;
      }
      if (!entry.is_regular_file()) {
        continue;
      }

      const auto package_path = entry.path();
      const auto package_filename = package_path.filename().string();
      std::string filename_lower = package_filename;
      std::transform(filename_lower.begin(), filename_lower.end(), filename_lower.begin(), ::tolower);
      if (filename_lower.find("morshu") != std::string::npos) {
        REXLOG_INFO("Skipping obsolete prototype DLC package: {}", package_filename);
        ++skipped_count;
        continue;
      }

      std::error_code canonical_ec;
      auto package_key = std::filesystem::weakly_canonical(package_path, canonical_ec).string();
      if (canonical_ec) {
        package_key = std::filesystem::absolute(package_path).string();
      }
      if (!seen_packages.insert(package_key).second) {
        continue;
      }

      const auto header = rex::filesystem::StfsContainerDevice::ReadPackageHeader(package_path);
      if (!header) {
        REXLOG_WARN("Skipping DLC candidate with invalid STFS header: {}", package_path.string());
        ++skipped_count;
        continue;
      }

      const auto content_type =
          static_cast<rex::system::XContentType>(header->metadata.content_type);
      if (content_type != rex::system::XContentType::kMarketplaceContent) {
        REXLOG_WARN("Skipping non-DLC content package {} with type {:08X}",
                    package_path.filename().string(), static_cast<uint32_t>(content_type));
        ++skipped_count;
        continue;
      }

      const uint32_t package_title_id = header->metadata.execution_info.title_id;
      if (package_title_id != 0 && package_title_id != title_id) {
        REXLOG_WARN("Skipping DLC package {} for title {:08X}; running title is {:08X}",
                    package_path.filename().string(), package_title_id, title_id);
        ++skipped_count;
        continue;
      }

      if (IsInstalledMarketplaceContent(runtime()->user_data_root(), title_id, package_path)) {
        REXLOG_INFO("DLC package already installed: {}", package_path.filename().string());
        ++skipped_count;
        continue;
      }

      REXLOG_INFO("Installing Skate 3 DLC package: {}", package_path.string());
      const auto result = runtime()->kernel_state()->content_manager()->InstallContent(package_path);
      if (result == 0) {
        ++installed_count;
        REXLOG_INFO("Installed Skate 3 DLC package: {}", package_path.filename().string());
      } else {
        ++skipped_count;
        REXLOG_WARN("Failed to install Skate 3 DLC package {}: {:08X}",
                    package_path.filename().string(), static_cast<uint32_t>(result));
      }
    }
  }

  if (installed_count || skipped_count) {
    REXLOG_INFO("Skate 3 DLC scan complete: {} installed, {} skipped", installed_count,
                skipped_count);
  } else {
    REXLOG_INFO("No Skate 3 DLC packages found. Drop legally obtained DLC package files in {}",
                user_dlc_root.string());
  }
}
