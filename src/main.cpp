// skate3 - ReXGlue Recompiled Project
//
// This file is yours to edit. 'rexglue migrate' will NOT overwrite it.

#include "skate3_app_common.h"
#include <skate3_version.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

#include <rex/cvar.h>
#include <rex/filesystem.h>

#if defined(__linux__) || defined(__APPLE__)
#include <sys/wait.h>
#include <unistd.h>
#endif

REXCVAR_DEFINE_BOOL(run_achievement_tests, false, "Skate 3",
                    "Run the Skate 3 achievement test suite and exit");

namespace {

int RunAchievementTestBinary(const char* argv0) {
  std::error_code ec;
  std::filesystem::path exe_dir;
#if defined(__linux__)
  auto self_exe = std::filesystem::read_symlink("/proc/self/exe", ec);
  if (!ec && !self_exe.empty()) {
    exe_dir = self_exe.parent_path();
  }
#endif
  if (exe_dir.empty() && argv0 && *argv0) {
    exe_dir = std::filesystem::absolute(argv0, ec).parent_path();
  }
  if (exe_dir.empty()) {
    exe_dir = std::filesystem::current_path(ec);
  }
#if defined(_WIN32)
  const auto test_bin = exe_dir / "skate3_achievement_tests.exe";
#else
  const auto test_bin = exe_dir / "skate3_achievement_tests";
#endif
  const std::string cmd = "\"" + test_bin.string() + "\"";
  const int rc = std::system(cmd.c_str());
#if defined(__linux__) || defined(__APPLE__)
  if (rc != -1 && WIFEXITED(rc)) {
    return WEXITSTATUS(rc);
  }
#endif
  return rc == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

#if defined(__linux__)
__attribute__((constructor)) void CheckRunAchievementTestsFlagEarly() {
  std::ifstream cmdline("/proc/self/cmdline", std::ios::binary);
  if (!cmdline) {
    return;
  }
  std::string arg;
  std::string argv0;
  bool first = true;
  while (std::getline(cmdline, arg, '\0')) {
    if (first) {
      argv0 = arg;
      first = false;
    }
    if (arg == "--run-achievement-tests" || arg == "--run_achievement_tests") {
      std::_Exit(RunAchievementTestBinary(argv0.c_str()));
    }
  }
}
#endif

}  // namespace

// NVIDIA Optimus / AMD PowerXpress discrete-GPU opt-in. The drivers only read
// these from the executable's export table; the copies in rexruntime.dll are
// invisible to them, which left hybrid-graphics laptops rendering on the
// integrated GPU. They must live here, in the exe image itself.
#if defined(_WIN32)
extern "C" {
__declspec(dllexport) unsigned long NvOptimusEnablement = 0x00000001;
__declspec(dllexport) unsigned long AmdPowerXpressRequestHighPerformance = 1;
}
#endif

class Skate3PureApp : public Skate3BaseApp {
 public:
  using Skate3BaseApp::Skate3BaseApp;

  std::string_view GetBuildTitle() const override {
    return SKATE3_BUILD_TITLE;
  }

  std::string_view GetBuildStamp() const override {
    return SKATE3_BUILD_STAMP;
  }

  std::string GetWindowTitle() const override {
#if defined(__APPLE__)
    // Match the macOS bundle name so the title bar and the .app agree.
    return "skate3recomp " SKATE3_BUILD_TITLE;
#else
    return "Skate 3 " SKATE3_BUILD_TITLE;
#endif
  }

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    if (REXCVAR_GET(run_achievement_tests)) {
      std::_Exit(RunAchievementTestBinary(nullptr));
    }
    return std::unique_ptr<Skate3PureApp>(
        new Skate3PureApp(ctx, "skate3", skate3_PPCImageConfig));
  }
};

REX_DEFINE_APP(skate3, Skate3PureApp::Create)
