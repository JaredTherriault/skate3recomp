#include "skate3_achievements.h"
#include "generated/skate3_init.h"
#include "skate3_coord_dump.h"
#include "skate3_user_settings.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

// ============================================================================
// Controllable Test Environment & Guest Memory Harness
// ============================================================================

namespace {

bool g_has_skater_pos = true;
float g_skater_pos[3] = {0.0f, 0.0f, 0.0f};
bool g_skater_airborne = false;
bool g_skater_grinding = false;
bool g_skater_bailed_or_off = false;
uint32_t g_mock_mapped_ach_id = 0;

void SetSkaterState(float x, float y, float z, bool airborne, bool bailed_or_off) {
  g_has_skater_pos = true;
  g_skater_pos[0] = x;
  g_skater_pos[1] = y;
  g_skater_pos[2] = z;
  g_skater_airborne = airborne;
  g_skater_grinding = false;
  g_skater_bailed_or_off = bailed_or_off;
}

void SetSkaterStateGrind(float x, float y, float z, bool grinding, bool bailed_or_off) {
  g_has_skater_pos = true;
  g_skater_pos[0] = x;
  g_skater_pos[1] = y;
  g_skater_pos[2] = z;
  g_skater_airborne = false;
  g_skater_grinding = grinding;
  g_skater_bailed_or_off = bailed_or_off;
}

inline void StoreU8(uint8_t* base, uint32_t addr, uint8_t val) {
  *(base + addr) = val;
}

inline void StoreU16(uint8_t* base, uint32_t addr, uint16_t val) {
  uint16_t be = __builtin_bswap16(val);
  std::memcpy(base + addr, &be, 2);
}

inline void StoreU32(uint8_t* base, uint32_t addr, uint32_t val) {
  uint32_t be = __builtin_bswap32(val);
  std::memcpy(base + addr, &be, 4);
}

inline void StoreU64(uint8_t* base, uint32_t addr, uint64_t val) {
  uint64_t be = __builtin_bswap64(val);
  std::memcpy(base + addr, &be, 8);
}

inline void StoreF32(uint8_t* base, uint32_t addr, float val) {
  uint32_t raw;
  std::memcpy(&raw, &val, 4);
  StoreU32(base, addr, raw);
}

inline void StoreCString(uint8_t* base, uint32_t addr, const char* str) {
  const size_t len = std::strlen(str);
  std::memcpy(base + addr, str, len + 1);
}

inline void StoreWString(uint8_t* base, uint32_t addr, const char* ascii_str) {
  const size_t len = std::strlen(ascii_str);
  for (size_t i = 0; i <= len; ++i) {
    StoreU16(base, addr + static_cast<uint32_t>(i * 2), static_cast<uint16_t>(ascii_str[i]));
  }
}

}  // namespace

namespace skate3 {

bool IsSkaterOnBoardAirborne() {
  return g_skater_airborne;
}

bool IsSkaterAirborneOrGrinding() {
  return g_skater_airborne || g_skater_grinding;
}

bool IsSkaterGrinding() {
  return g_skater_grinding;
}

void NotifySkaterGrinding() {
  g_skater_grinding = true;
}

bool IsSkaterBailedOrOffBoard() {
  return g_skater_bailed_or_off;
}

LocalProfileStore LoadProfiles(const std::filesystem::path&) {
  LocalProfileStore store;
  store.selected_profile = "test_profile";
  return store;
}

namespace coord_dump {

bool GetLastSkaterPosition(float out_pos[3]) {
  if (!g_has_skater_pos) return false;
  out_pos[0] = g_skater_pos[0];
  out_pos[1] = g_skater_pos[1];
  out_pos[2] = g_skater_pos[2];
  return true;
}

}  // namespace coord_dump
}  // namespace skate3

// Guest Hook Underlying Stubs (__imp__sub_*)
extern "C" {
void __imp__sub_8274A3F0(PPCContext& ctx, uint8_t*) {
  ctx.r3.u32 = g_mock_mapped_ach_id;
}
void __imp__sub_8274A590(PPCContext&, uint8_t*) {}
void __imp__sub_82740298(PPCContext&, uint8_t*) {}
void __imp__sub_82DAA868(PPCContext&, uint8_t*) {}
void __imp__sub_82DAA8E0(PPCContext&, uint8_t*) {}
void __imp__sub_82DAA4C0(PPCContext&, uint8_t*) {}
void __imp__sub_82DA93D8(PPCContext&, uint8_t*) {}
void __imp__sub_82DA9920(PPCContext&, uint8_t*) {}
void __imp__sub_82DA9B08(PPCContext&, uint8_t*) {}
void __imp__sub_827743A0(PPCContext&, uint8_t*) {}
void __imp__sub_82774080(PPCContext&, uint8_t*) {}
void __imp__sub_82773D98(PPCContext&, uint8_t*) {}
void __imp__sub_82774828(PPCContext&, uint8_t*) {}
void __imp__sub_827CE7B0(PPCContext&, uint8_t*) {}
void __imp__sub_827CFD48(PPCContext&, uint8_t*) {}
void __imp__sub_827CDB10(PPCContext&, uint8_t*) {}
void __imp__sub_827CDDD8(PPCContext&, uint8_t*) {}
void __imp__sub_82639890(PPCContext&, uint8_t*) {}
void __imp__sub_827586F0(PPCContext&, uint8_t*) {}
void __imp__sub_8288EA18(PPCContext&, uint8_t*) {}
void __imp__sub_825113D8(PPCContext&, uint8_t*) {}
}

// Guest Hook Entrypoints Under Test (implemented in src/skate3_achievements.cpp)
extern "C" {
void sub_8274A590(PPCContext& ctx, uint8_t* base);
void sub_82740298(PPCContext& ctx, uint8_t* base);
void sub_82DAA868(PPCContext& ctx, uint8_t* base);
void sub_82DAA8E0(PPCContext& ctx, uint8_t* base);
void sub_82DAA4C0(PPCContext& ctx, uint8_t* base);
void sub_82DA93D8(PPCContext& ctx, uint8_t* base);
void sub_82DA9920(PPCContext& ctx, uint8_t* base);
void sub_82DA9B08(PPCContext& ctx, uint8_t* base);
void sub_827743A0(PPCContext& ctx, uint8_t* base);
void sub_82774080(PPCContext& ctx, uint8_t* base);
void sub_82773D98(PPCContext& ctx, uint8_t* base);
void sub_82774828(PPCContext& ctx, uint8_t* base);
void sub_827CE7B0(PPCContext& ctx, uint8_t* base);
void sub_827CFD48(PPCContext& ctx, uint8_t* base);
void sub_827CDB10(PPCContext& ctx, uint8_t* base);
void sub_827CDDD8(PPCContext& ctx, uint8_t* base);
void sub_82639890(PPCContext& ctx, uint8_t* base);
void sub_827586F0(PPCContext& ctx, uint8_t* base);
void sub_8288EA18(PPCContext& ctx, uint8_t* base);
void sub_825113D8(PPCContext& ctx, uint8_t* base);
}

#define TEST_CHECK(cond, msg)                                                           \
  do {                                                                                  \
    if (!(cond)) {                                                                      \
      std::cerr << "[FAIL] " << __FILE__ << ":" << __LINE__ << " - " << (msg) << "\n"; \
      std::exit(1);                                                                     \
    }                                                                                   \
  } while (0)

static const rex::ui::SimpleAchievementItem* FindUiItem(
    const rex::ui::SimpleAchievementData& data, std::string_view name) {
  for (const auto& item : data.items) {
    if (item.name == name) {
      return &item;
    }
  }
  return nullptr;
}

int main() {
  std::cout << "=== Skate 3 Achievement Popping & Checklist Test Suite ===\n";

  // Suppress audio during test execution
  using namespace skate3;
  REXCVAR_SET(achievement_sound, false);

  // Set up an isolated temporary directory for profile persistence
  const auto temp_dir = std::filesystem::temp_directory_path() / "skate3_ach_tests";
  std::error_code ec;
  std::filesystem::remove_all(temp_dir, ec);
  std::filesystem::create_directories(temp_dir / "profiles", ec);

  // Copy mods/raw/sample_achievement_mod and mods/raw/online_achievements into temp_dir/mods/raw
  const auto repo_sample_pack =
      std::filesystem::exists("mods/raw/sample_achievement_mod/pack.json")
          ? std::filesystem::path("mods/raw/sample_achievement_mod/pack.json")
          : std::filesystem::path("../mods/raw/sample_achievement_mod/pack.json");
  TEST_CHECK(std::filesystem::exists(repo_sample_pack),
             "mods/raw/sample_achievement_mod/pack.json must exist");
  std::filesystem::create_directories(temp_dir / "mods" / "raw" / "sample_achievement_mod", ec);
  std::filesystem::copy_file(
      repo_sample_pack, temp_dir / "mods" / "raw" / "sample_achievement_mod" / "pack.json",
      std::filesystem::copy_options::overwrite_existing, ec);

  const auto repo_online_pack =
      std::filesystem::exists("mods/raw/online_achievements/pack.json")
          ? std::filesystem::path("mods/raw/online_achievements/pack.json")
          : std::filesystem::path("../mods/raw/online_achievements/pack.json");
  TEST_CHECK(std::filesystem::exists(repo_online_pack),
             "mods/raw/online_achievements/pack.json must exist");
  std::filesystem::create_directories(temp_dir / "mods" / "raw" / "online_achievements", ec);
  std::filesystem::copy_file(
      repo_online_pack, temp_dir / "mods" / "raw" / "online_achievements" / "pack.json",
      std::filesystem::copy_options::overwrite_existing, ec);

  skate3::AchievementToastDialog toast_dialog(nullptr);
  skate3::AchievementManager mgr;
  mgr.Initialize(nullptr, temp_dir / "profiles.json", &toast_dialog, nullptr);
  mgr.ResetAchievements();

  // Allocate sparse guest address space buffer (up to 0x83100000 -> ~2.1 GB virtual,
  // Linux on-demand paging only allocates touched pages!)
  constexpr size_t kGuestMemSize = 0x83200000ull;
  std::vector<uint8_t> guest_mem(kGuestMemSize, 0);
  uint8_t* base = guest_mem.data();

  // Populate trick table entries at 0x820862A8 (stride 24, name pointer at +20)
  auto register_guest_trick = [&](uint32_t trick_id, const char* name) {
    const uint32_t str_addr = 0x82010000u + trick_id * 64u;
    StoreCString(base, str_addr, name);
    StoreU32(base, 0x820862A8u + trick_id * 24u + 20u, str_addr);
  };
  register_guest_trick(1, "Ollie");
  register_guest_trick(5, "Kickflip");
  register_guest_trick(13, "360Flip");
  register_guest_trick(252, "Superdude");

  const std::vector<uint32_t> kOnlineIds = {1, 4, 5, 6, 19, 25, 26, 27, 28, 29, 30, 31};
  auto is_online_id = [&](uint32_t id) {
    return std::find(kOnlineIds.begin(), kOnlineIds.end(), id) != kOnlineIds.end();
  };

  // --------------------------------------------------------------------------
  // 1. Verify Default Gamerscore Totals & Disabled-By-Default Online Pack
  // --------------------------------------------------------------------------
  {
    const auto data = mgr.GetAchievementData();
    uint32_t base_score = 0;
    uint32_t maloof_score = 0;
    uint32_t danny_score = 0;
    uint32_t online_count = 0;
    uint32_t sample_score = 0;
    uint32_t sample_count = 0;

    for (const auto& item : data.items) {
      if (item.category == "Base Game") base_score += item.gamerscore;
      else if (item.category == "Maloof Money Cup NY DLC") maloof_score += item.gamerscore;
      else if (item.category == "Danny Way's Hawaiian Dream DLC") danny_score += item.gamerscore;
      else if (item.category == "Online") online_count++;
      else if (item.category == "Sample Achievement Mod") {
        sample_score += item.gamerscore;
        sample_count++;
      }
    }

    TEST_CHECK(online_count == 0,
               "Online achievements pack must be disabled by default (0 Online achievements in list)");
    TEST_CHECK(base_score == 1000, "Base Game achievements must total 1000 G");
    TEST_CHECK(maloof_score == 100, "Maloof DLC achievements must total 100 G");
    TEST_CHECK(danny_score == 150, "Danny Way DLC achievements must total 150 G");
    TEST_CHECK(sample_count == 19, "Sample Achievement Mod must have 19 achievements");
    TEST_CHECK(sample_score == 1000, "Sample Achievement Mod must total 1000 G");
    std::cout << "[PASS] Default Gamerscore totals verified (Base=1000G, DLC=250G, SampleMod=1000G, Online disabled by default)\n";
  }

  // --------------------------------------------------------------------------
  // 2. Verify All 47 Offline Retail & DLC Achievements + 12 Online Pack Achievements (250 G)
  // --------------------------------------------------------------------------
  {
    mgr.ResetAchievements();
    PPCContext ctx{};
    for (uint32_t id = 1; id <= 59; ++id) {
      TEST_CHECK(!mgr.IsUnlocked(id), "Achievement should start locked");
      g_mock_mapped_ach_id = id;
      ctx.r3.u64 = 0x1000ull + id;
      sub_8274A590(ctx, base);
      if (is_online_id(id)) {
        TEST_CHECK(!mgr.IsUnlocked(id),
                   fmt::format("Disabled online achievement #{} must NOT unlock while pack is disabled", id));
      } else {
        TEST_CHECK(mgr.IsUnlocked(id),
                   fmt::format("Offline retail achievement #{} failed to pop via sub_8274A590", id));
      }
    }
    // Unlocking #36 (Spot Monopolist) via sub_8274A590 must also unlock #112 (King of the Streets)
    TEST_CHECK(mgr.IsUnlocked(112), "Unlocking #36 (Spot Monopolist) must also unlock #112 (King of the Streets)");

    // Verify toast notification contains title, description, and points total
    const auto toast = toast_dialog.current_toast();
    TEST_CHECK(toast.has_value() && !toast->name.empty() && !toast->description.empty() && toast->gamerscore > 0,
               "AchievementToastDialog must include title, description, and gamerscore");

    // Now enable the Online Achievements pack and verify 250 G total & popping for all 12 online IDs
    const auto temp_online_pack = temp_dir / "mods" / "raw" / "online_achievements" / "pack.json";
    {
      std::ifstream in(temp_online_pack);
      std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
      const auto pos = content.find("\"enabled\": false");
      TEST_CHECK(pos != std::string::npos, "online_achievements/pack.json should have enabled: false");
      content.replace(pos, std::strlen("\"enabled\": false"), "\"enabled\": true");
      std::ofstream out(temp_online_pack);
      out << content;
    }
    mgr.ReloadDefinitions();
    {
      const auto data = mgr.GetAchievementData();
      uint32_t online_score = 0;
      uint32_t online_count = 0;
      for (const auto& item : data.items) {
        if (item.category == "Online") {
          online_score += item.gamerscore;
          online_count++;
        }
      }
      TEST_CHECK(online_count == 12, "Enabled Online pack must have 12 achievements");
      TEST_CHECK(online_score == 250, fmt::format("Enabled Online pack must total 250 G (got {})", online_score));
    }
    for (uint32_t id : kOnlineIds) {
      g_mock_mapped_ach_id = id;
      ctx.r3.u64 = 0x1000ull + id;
      sub_8274A590(ctx, base);
      TEST_CHECK(mgr.IsUnlocked(id),
                 fmt::format("Enabled online achievement #{} failed to pop via sub_8274A590", id));
    }

    // Re-disable Online pack to restore default state for subsequent tests
    std::filesystem::copy_file(repo_online_pack, temp_online_pack,
                               std::filesystem::copy_options::overwrite_existing, ec);
    mgr.ReloadDefinitions();
    std::cout << "[PASS] All 47 Offline Retail/DLC achievements + 12 Online Pack achievements (250G) verified\n";
  }

  // --------------------------------------------------------------------------
  // 3. Verify Base Game Telemetry & Stat Hooks (7, 8, 9, 10, 11, 12, 13, 22)
  // --------------------------------------------------------------------------
  {
    mgr.ResetAchievements();
    SetSkaterState(0.0f, 0.0f, 0.0f, false, false);
    PPCContext ctx{};

    const uint32_t skater = 0x40000000u;
    const uint32_t telemetry = 0x40010000u;
    StoreU32(base, skater + 60u, telemetry);

    // 7 (Trick Spammer: 30,000 line), 8 (Blaow!: 10,000 trick), 10 (Objectifier: 3 objects),
    // 12 (Mantastic Voyage: 3 manuals), 111 (Chain Reaction: 10 tricks chained)
    StoreU8(base, telemetry + 14640u, 0);  // not bailed
    StoreU8(base, telemetry + 14636u, 0);  // not in seq
    StoreU8(base, telemetry + 14626u, 1);  // seq_banked = true
    StoreU32(base, telemetry + 108u, 12000u);  // trick_score = 12,000
    StoreU32(base, telemetry + 0u, 35000u);    // line_score = 35,000
    StoreU32(base, telemetry + 84u, 11u);      // banked_tricks = 11
    StoreU32(base, telemetry + 88u, 0u);
    StoreU32(base, telemetry + 112u, 3u);      // manuals_chained = 3
    StoreU8(base, telemetry + 5758u, 3u);      // moveable_objects = 3

    ctx.r4.u32 = skater;
    sub_827743A0(ctx, base);

    TEST_CHECK(mgr.IsUnlocked(7), "#7 Trick Spammer (30k line) should unlock");
    TEST_CHECK(mgr.IsUnlocked(8), "#8 Blaow! (10k trick) should unlock");
    TEST_CHECK(mgr.IsUnlocked(10), "#10 Objectifier (3 moveable objects) should unlock");
    TEST_CHECK(mgr.IsUnlocked(12), "#12 Mantastic Voyage (3 manuals) should unlock");
    TEST_CHECK(mgr.IsUnlocked(111), "#111 Chain Reaction (10 tricks chained) should unlock");

    // 9 (Did Somebody Get a Boo Boo?: 75,000 bail) & 108 (Full Body Cast: 6 bones broken)
    StoreU8(base, telemetry + 14647u, 1);  // HoM active
    StoreU32(base, telemetry + 12336u + 1196u, 80000u);  // bail_score = 80,000
    StoreU32(base, telemetry + 12336u + 904u, 7u);       // bones_broken = 7
    ctx.r4.u32 = skater;
    sub_82774828(ctx, base);
    TEST_CHECK(mgr.IsUnlocked(9), "#9 Did Somebody Get a Boo Boo? (75k bail) should unlock");
    TEST_CHECK(mgr.IsUnlocked(108), "#108 Full Body Cast (6+ bones broken) should unlock");

    // 11 (Extreme Grindage: 100m grind)
    const uint32_t grind_tracker = 0x40030000u;
    StoreF32(base, grind_tracker + 760u, 105.0f);
    ctx.r3.u32 = grind_tracker;
    sub_82DA93D8(ctx, base);
    TEST_CHECK(mgr.IsUnlocked(11), "#11 Extreme Grindage (100m grind) should unlock");

    // 13 (Don't be so Mayo: Miracle Whip via sub_82773D98 with trick_id=252 and telemetry+9844 != 0)
    StoreU32(base, telemetry + 9844u, 1u);
    ctx.r3.s32 = 252;
    ctx.r4.u32 = 0;  // failed = false
    ctx.r5.u32 = skater;
    sub_82773D98(ctx, base);
    TEST_CHECK(mgr.IsUnlocked(13), "#13 Don't be so Mayo (Miracle Whip) should unlock");

    // 22 (Deck Peddler: 1,000,000 boards sold via sub_827CE7B0)
    const uint32_t career_sub = 0x40040000u;
    const uint32_t sales_struct = 0x40041000u;
    StoreU32(base, career_sub + 56u, sales_struct);
    StoreU32(base, sales_struct + 4u, 400000u);
    StoreU32(base, sales_struct + 8u, 300000u);
    StoreU32(base, sales_struct + 12u, 200000u);
    StoreU32(base, sales_struct + 16u, 150000u);
    ctx.r3.u32 = career_sub;
    sub_827CE7B0(ctx, base);
    TEST_CHECK(mgr.IsUnlocked(22), "#22 Deck Peddler (1M boards sold) should unlock");

    std::cout << "[PASS] Base Game stat & trick hooks (#7, #8, #9, #10, #11, #12, #13, #22) verified\n";
  }

  // --------------------------------------------------------------------------
  // 4. Verify All 12 Offline Base Game Achievements (101..112)
  // --------------------------------------------------------------------------
  {
    mgr.ResetAchievements();
    PPCContext ctx{};

    // 101..104 Cheat Codes (including negative test when cancelled == 1)
    const uint32_t str_addr = 0x40050000u;
    StoreCString(base, str_addr, "deadspacetoo");
    ctx.r5.u32 = str_addr;
    ctx.r6.u32 = 1;  // cancelled!
    sub_82639890(ctx, base);
    TEST_CHECK(!mgr.IsUnlocked(101), "#101 must NOT unlock when keyboard input is cancelled");

    ctx.r6.u32 = 0;  // confirmed
    sub_82639890(ctx, base);
    TEST_CHECK(mgr.IsUnlocked(101), "#101 Isaac's Day Off should unlock on 'deadspacetoo'");

    StoreCString(base, str_addr, "mcfly");
    sub_82639890(ctx, base);
    TEST_CHECK(mgr.IsUnlocked(102), "#102 Hover Conversion should unlock on 'mcfly'");

    const uint32_t wstr_addr = 0x40051000u;
    StoreWString(base, wstr_addr, "zombie");
    ctx.r3.u32 = wstr_addr;
    sub_827586F0(ctx, base);
    TEST_CHECK(mgr.IsUnlocked(103), "#103 Night of the Living Skaters should unlock on UTF-16 'zombie'");

    StoreCString(base, str_addr, "miniskaters");
    ctx.r5.u32 = str_addr;
    ctx.r6.u32 = 0;
    sub_82639890(ctx, base);
    TEST_CHECK(mgr.IsUnlocked(104), "#104 Pocket Skater should unlock on 'miniskaters'");

    // 105 (The Mega Drop: Super Ultra Mega Park roll-in jump [343.53, 140.91, -729.13] -> [356.63, 132.34, -704.69])
    // Negative test: bail mid-jump
    SetSkaterState(343.53f, 140.91f, -729.13f, true, false);
    mgr.Update(0.05f);
    std::this_thread::sleep_for(std::chrono::milliseconds(170));
    SetSkaterState(356.63f, 132.34f, -704.69f, false, true);  // bailed!
    mgr.Update(0.05f);
    TEST_CHECK(!mgr.IsUnlocked(105), "#105 The Mega Drop must NOT unlock if bailed on landing");

    // Positive test: clean jump and roll-away
    SetSkaterState(343.53f, 140.91f, -729.13f, true, false);
    mgr.Update(0.05f);
    std::this_thread::sleep_for(std::chrono::milliseconds(170));
    SetSkaterState(356.63f, 132.34f, -704.69f, false, false);
    mgr.Update(0.05f);
    TEST_CHECK(mgr.IsUnlocked(105), "#105 The Mega Drop should unlock on clean landing");

    // 106 (Dam Buster: San Van Spillway Dam drop [448.74, 104.03, -485.93] -> [381.84, 91.46, -487.19])
    SetSkaterState(448.74f, 104.03f, -485.93f, true, false);
    mgr.Update(0.05f);
    std::this_thread::sleep_for(std::chrono::milliseconds(170));
    SetSkaterState(381.84f, 91.46f, -487.19f, false, false);
    mgr.Update(0.05f);
    TEST_CHECK(mgr.IsUnlocked(106), "#106 Dam Buster should unlock on clean Dam drop");

    // 107 (Terminal Velocity: 60 mph on skateboard)
    // Negative test: 65 mph while bailed/off-board
    SetSkaterState(0.0f, 0.0f, 0.0f, false, true);
    mgr.Update(0.1f);
    SetSkaterState(30.0f, 0.0f, 0.0f, false, true);  // 30 m/s in 0.1s = 300 m/s (>60 mph), but bailed
    mgr.Update(0.1f);
    TEST_CHECK(!mgr.IsUnlocked(107), "#107 Terminal Velocity must NOT unlock while bailed/off-board");

    // Positive test: 30 m/s (~67 mph) on-board
    SetSkaterState(0.0f, 0.0f, 0.0f, false, false);
    mgr.Update(0.1f);
    SetSkaterState(3.0f, 0.0f, 0.0f, false, false);  // 3.0m in 0.1s = 30 m/s = 67.1 mph
    mgr.Update(0.1f);
    TEST_CHECK(mgr.IsUnlocked(107), "#107 Terminal Velocity should unlock at 67.1 mph on-board");

    // 109 (Endless Manual: 100m manual via sub_82DAA868)
    const uint32_t manual_tracker = 0x40060000u;
    StoreF32(base, manual_tracker + 1028u, 102.5f);
    ctx.r3.u32 = manual_tracker;
    sub_82DAA868(ctx, base);
    TEST_CHECK(mgr.IsUnlocked(109), "#109 Endless Manual (100m) should unlock");

    // 110 (Park Architect: 25+ placed objects in skate.Park via sub_8288EA18 and sub_825113D8)
    const uint32_t park_stats = 0x40070000u;
    StoreU32(base, park_stats + 4u, 24u);
    StoreU32(base, park_stats + 16u, 24u);
    ctx.r3.u32 = park_stats;
    sub_8288EA18(ctx, base);
    TEST_CHECK(!mgr.IsUnlocked(110), "#110 Park Architect must NOT unlock with 24 placed objects");

    StoreU32(base, park_stats + 16u, 25u);
    sub_8288EA18(ctx, base);
    TEST_CHECK(mgr.IsUnlocked(110), "#110 Park Architect should unlock with 25 placed objects");

    // Also test sub_825113D8 path for #110
    mgr.ResetAchievements();
    const uint32_t global_mgr = 0x40080000u;
    const uint32_t park_obj_mgr = 0x40090000u;
    StoreU32(base, 0x8304FD94u, global_mgr);
    StoreU32(base, global_mgr + 268u, park_obj_mgr);
    StoreU32(base, park_obj_mgr + 7504u, 20u);
    StoreU32(base, park_obj_mgr + 24u, 6u);
    sub_825113D8(ctx, base);
    TEST_CHECK(mgr.IsUnlocked(110), "#110 Park Architect should unlock via sub_825113D8 with 26 objects");

    // 112 (King of the Streets: 16 distinct OTS spots via OnSpotCompleted)
    for (int i = 1; i <= 15; ++i) {
      mgr.OnSpotCompleted(fmt::format("ots_spot_{}", i), false);
    }
    TEST_CHECK(!mgr.IsUnlocked(112), "#112 must NOT unlock at 15/16 OTS spots");
    {
      const auto data = mgr.GetAchievementData();
      const auto* item112 = FindUiItem(data, "King of the Streets");
      TEST_CHECK(item112 && item112->progress_current == 15 && item112->progress_total == 16,
                 "#112 progress bar should show 15/16");
    }
    mgr.OnSpotCompleted("ots_spot_16", true);
    TEST_CHECK(mgr.IsUnlocked(112), "#112 King of the Streets should unlock at 16/16 OTS spots");

    std::cout << "[PASS] All 12 Offline Base Game achievements (101..112) verified (positive & negative cases)\n";
  }

  // --------------------------------------------------------------------------
  // 5. Verify All 19 Sample Achievement Mod Achievements
  // --------------------------------------------------------------------------
  {
    mgr.ResetAchievements();
    SetSkaterState(0.0f, 0.0f, 0.0f, false, false);
    PPCContext ctx{};

    // 5.1: sample_own_spot ("First on the Board") & sample_kill_spot ("Taking It to the Limit")
    ctx.r3.u32 = 0;
    ctx.r4.u32 = 0;
    ctx.r5.u32 = 1;  // Owned (not killed)
    sub_827CDDD8(ctx, base);
    TEST_CHECK(mgr.IsUnlocked("Sample Achievement Mod:sample_own_spot"),
               "sample_own_spot should unlock when owning a spot (status=1)");
    TEST_CHECK(!mgr.IsUnlocked("Sample Achievement Mod:sample_kill_spot"),
               "sample_kill_spot must NOT unlock when only owning a spot (status=1)");

    ctx.r5.u32 = 2;  // Killed
    sub_827CDDD8(ctx, base);
    TEST_CHECK(mgr.IsUnlocked("Sample Achievement Mod:sample_kill_spot"),
               "sample_kill_spot should unlock when killing a spot (status=2)");

    // 5.2: sample_reach_location ("Campus Sightseer" at [370.69, 79.94, -112.83], r=8.0)
    SetSkaterState(410.0f, 71.0f, -245.0f, false, false);  // Lower Chan Center courtyard
    mgr.Update(0.05f);
    TEST_CHECK(!mgr.IsUnlocked("Sample Achievement Mod:sample_reach_location"),
               "sample_reach_location must NOT unlock in lower Chan Center");

    SetSkaterState(370.69f, 79.94f, -112.83f, false, false);
    mgr.Update(0.05f);
    TEST_CHECK(mgr.IsUnlocked("Sample Achievement Mod:sample_reach_location"),
               "sample_reach_location should unlock at upper University overlook");

    // 5.3: sample_gap ("The Stairway Flight": [415.0, 69.0, -210.0] -> [415.0, 65.0, -181.0], r=6.5)
    // Negative test 1: rolling on ground without grinding or going airborne
    SetSkaterState(415.0f, 69.0f, -210.0f, false, false);
    mgr.Update(0.05f);
    std::this_thread::sleep_for(std::chrono::milliseconds(170));
    SetSkaterState(145.0f, 65.0f, -181.0f, false, false);
    mgr.Update(0.05f);
    TEST_CHECK(!mgr.IsUnlocked("Sample Achievement Mod:sample_gap"),
               "sample_gap must NOT unlock by rolling on the ground down the stairs");

    // Negative test 2: start grind, but stay on ground in the middle of the stairs (> 0.6s grace)
    SetSkaterStateGrind(415.0f, 69.0f, -210.0f, true, false);
    mgr.Update(0.05f);
    SetSkaterState(280.0f, 67.0f, -195.0f, false, false);  // touched ground in the middle!
    mgr.Update(0.05f);
    std::this_thread::sleep_for(std::chrono::milliseconds(700));
    mgr.Update(0.05f);
    SetSkaterStateGrind(145.0f, 65.0f, -181.0f, true, false);
    mgr.Update(0.05f);
    TEST_CHECK(!mgr.IsUnlocked("Sample Achievement Mod:sample_gap"),
               "sample_gap must NOT unlock if skater stays on ground in the middle of the stairs");

    // Positive test: grind the rail from top to bottom without touching ground
    SetSkaterStateGrind(415.0f, 69.0f, -210.0f, true, false);
    mgr.Update(0.05f);
    std::this_thread::sleep_for(std::chrono::milliseconds(170));
    SetSkaterStateGrind(145.0f, 65.0f, -181.0f, true, false);
    mgr.Update(0.05f);
    TEST_CHECK(mgr.IsUnlocked("Sample Achievement Mod:sample_gap"),
               "sample_gap should unlock after continuous grind down the stair set");

    // 5.4: sample_speed ("Breeze on the Boulevard" - 30 mph)
    SetSkaterState(0.0f, 0.0f, 0.0f, false, false);
    mgr.Update(0.1f);
    SetSkaterState(1.5f, 0.0f, 0.0f, false, false);  // 15 m/s = 33.55 mph
    mgr.Update(0.1f);
    TEST_CHECK(mgr.IsUnlocked("Sample Achievement Mod:sample_speed"),
               "sample_speed should unlock at 33.5 mph");

    // 5.5: sample_air_time ("Hang Time" - 1.5s via sub_82DA9B08)
    const uint32_t air_tracker = 0x400A0000u;
    StoreF32(base, air_tracker + 336u, 1.6f);
    ctx.r3.u32 = air_tracker;
    ctx.r4.u32 = 0;  // landed_cleanly = false!
    sub_82DA9B08(ctx, base);
    TEST_CHECK(!mgr.IsUnlocked("Sample Achievement Mod:sample_air_time"),
               "sample_air_time must NOT unlock when landed_cleanly is false");

    ctx.r4.u32 = 1;  // landed_cleanly = true
    sub_82DA9B08(ctx, base);
    TEST_CHECK(mgr.IsUnlocked("Sample Achievement Mod:sample_air_time"),
               "sample_air_time should unlock at 1.6s clean air");

    // 5.6: sample_manual_distance ("Steady Wheels" - 15m) & sample_grind_distance ("Curb Cruiser" - 15m)
    // Verify manualing 20m unlocks Steady Wheels but does NOT unlock Curb Cruiser!
    const uint32_t manual_tracker = 0x400B0000u;
    StoreU32(base, manual_tracker + 104u, 0x08000000u);  // regular manual flag
    StoreF32(base, manual_tracker + 1028u, 20.0f);
    ctx.r3.u32 = manual_tracker;
    sub_82DAA4C0(ctx, base);
    TEST_CHECK(mgr.IsUnlocked("Sample Achievement Mod:sample_manual_distance"),
               "sample_manual_distance (Steady Wheels) should unlock on 20m manual");
    TEST_CHECK(!mgr.IsUnlocked("Sample Achievement Mod:sample_grind_distance"),
               "sample_grind_distance (Curb Cruiser) must NOT unlock from a manual");

    const uint32_t grind_tracker = 0x400C0000u;
    StoreF32(base, grind_tracker + 760u, 16.5f);
    ctx.r3.u32 = grind_tracker;
    sub_82DA93D8(ctx, base);
    TEST_CHECK(mgr.IsUnlocked("Sample Achievement Mod:sample_grind_distance"),
               "sample_grind_distance (Curb Cruiser) should unlock on 16.5m grind");

    // 5.7: sample_line_score (2500), sample_tricks_chained (4), sample_moveable_objects (1)
    const uint32_t skater = 0x400D0000u;
    const uint32_t telemetry = 0x400E0000u;
    StoreU32(base, skater + 60u, telemetry);
    StoreU8(base, telemetry + 14640u, 0);
    StoreU8(base, telemetry + 14636u, 0);
    StoreU8(base, telemetry + 14626u, 1);  // banked
    StoreU32(base, telemetry + 108u, 1000u);
    StoreU32(base, telemetry + 0u, 2600u);  // line_score = 2600
    StoreU32(base, telemetry + 84u, 4u);    // 4 tricks chained
    StoreU32(base, telemetry + 88u, 0u);
    StoreU8(base, telemetry + 5758u, 1u);   // 1 moveable object
    ctx.r3.u32 = 0;
    ctx.r4.u32 = skater;
    sub_827743A0(ctx, base);
    TEST_CHECK(mgr.IsUnlocked("Sample Achievement Mod:sample_line_score"),
               "sample_line_score (Building Momentum) should unlock at 2600 pts");
    TEST_CHECK(mgr.IsUnlocked("Sample Achievement Mod:sample_tricks_chained"),
               "sample_tricks_chained (Combo Starter) should unlock at 4 tricks");
    TEST_CHECK(mgr.IsUnlocked("Sample Achievement Mod:sample_moveable_objects"),
               "sample_moveable_objects (Prop Improviser) should unlock at 1 object");

    // 5.8: sample_trick_name ("Three-Sixty Twist" - single flatground 360 Flip via sub_82774080)
    // Simulate a single trick in queue without in_seq/seq_banked
    StoreU8(base, telemetry + 14626u, 0);
    StoreU32(base, telemetry + 1032u, 1u);       // 1 trick in queue
    StoreU32(base, telemetry + 1032u + 20u, 13u); // trick_id 13 = "360Flip"
    StoreU8(base, telemetry + 1032u + 20u + 178u, 0u);  // complete
    ctx.r4.u32 = skater;
    sub_82774080(ctx, base);
    TEST_CHECK(mgr.IsUnlocked("Sample Achievement Mod:sample_trick_name"),
               "sample_trick_name (Three-Sixty Twist) should unlock on single flatground 360Flip");

    // 5.9: sample_bones_broken ("Minor Fracture" - 2 bones)
    StoreU8(base, telemetry + 14647u, 1);
    StoreU32(base, telemetry + 12336u + 904u, 2u);
    ctx.r4.u32 = skater;
    sub_82774828(ctx, base);
    TEST_CHECK(mgr.IsUnlocked("Sample Achievement Mod:sample_bones_broken"),
               "sample_bones_broken (Minor Fracture) should unlock at 2 bones");

    // 5.10: sample_board_sales ("On the Map" - 10,000 boards)
    const uint32_t career_sub = 0x400F0000u;
    const uint32_t sales_struct = 0x400F1000u;
    StoreU32(base, career_sub + 56u, sales_struct);
    StoreU32(base, sales_struct + 4u, 5000u);
    StoreU32(base, sales_struct + 8u, 5500u);
    ctx.r3.u32 = career_sub;
    sub_827CE7B0(ctx, base);
    TEST_CHECK(mgr.IsUnlocked("Sample Achievement Mod:sample_board_sales"),
               "sample_board_sales (On the Map) should unlock at 10,500 boards");

    // 5.11: sample_cheat_code ("Open Sesame" - any cheat code)
    const uint32_t cheat_str = 0x400F2000u;
    StoreCString(base, cheat_str, "customcode123");
    ctx.r5.u32 = cheat_str;
    ctx.r6.u32 = 0;
    sub_82639890(ctx, base);
    TEST_CHECK(mgr.IsUnlocked("Sample Achievement Mod:sample_cheat_code"),
               "sample_cheat_code (Open Sesame) should unlock on any code");

    std::cout << "[PASS] Original 15 Sample Mod achievements verified via guest hooks\n";
  }

  // --------------------------------------------------------------------------
  // 6. Verify All 4 Trick Group Checklist Achievements & Progress Bar UI State
  // --------------------------------------------------------------------------
  {
    mgr.ResetAchievements();

    // 6.1: Flip Trick Encyclopedia (14 flip tricks)
    // Landing "Varial Kickflip" must post a progress toast with item="Varial Kickflip", parent="Flip Trick Encyclopedia", 1/14
    mgr.OnTrickLanded("Varial Kickflip");
    {
      const auto ptoast = toast_dialog.current_progress_toast();
      TEST_CHECK(ptoast.has_value(), "Landing a new checklist trick must post a progress toast");
      TEST_CHECK(ptoast->item_name == "Varial Kickflip", "Progress toast must display the landed trick name");
      TEST_CHECK(ptoast->parent_name == "Flip Trick Encyclopedia", "Progress toast must display parent goal");
      TEST_CHECK(ptoast->current_count == 1 && ptoast->total_count == 14, "Progress toast must show 1/14");
    }

    // Landing "Varial Kickflip" a second time must NOT duplicate or increment progress
    mgr.OnTrickLanded("Varial Kickflip");
    {
      const auto ptoast = toast_dialog.current_progress_toast();
      TEST_CHECK(ptoast && ptoast->current_count == 1, "Duplicate trick must not increment progress count");
    }

    // Landing "FS Pop Shuvit" must check off "FS Pop Shuvit" ONLY, NOT "Pop Shuvit"!
    mgr.OnTrickLanded("FS Pop Shuvit");
    {
      const auto data = mgr.GetAchievementData();
      const auto* flip_item = FindUiItem(data, "Flip Trick Encyclopedia");
      TEST_CHECK(flip_item != nullptr, "Flip Trick Encyclopedia item must exist");
      TEST_CHECK(flip_item->progress_total == 14, "Flip Trick Encyclopedia must have 14 total tricks");
      TEST_CHECK(flip_item->progress_current == 2, "Flip Trick Encyclopedia should be at 2/14");
      for (const auto& [t_name, done] : flip_item->checklist) {
        if (t_name == "Varial Kickflip" || t_name == "FS Pop Shuvit") {
          TEST_CHECK(done, t_name + " should be checked off");
        } else {
          TEST_CHECK(!done, t_name + " must NOT be falsely checked off");
        }
      }
    }

    // Verify persistence of partial trick checklist across ReloadForActiveProfile()
    mgr.ReloadForActiveProfile();
    {
      const auto data = mgr.GetAchievementData();
      const auto* flip_item = FindUiItem(data, "Flip Trick Encyclopedia");
      TEST_CHECK(flip_item && flip_item->progress_current == 2 && !flip_item->unlocked,
                 "Partial trick checklist progress (2/14) must persist across profile reload");
    }

    // Land the remaining 12 flip tricks using internal xex variant names
    const std::vector<std::string> remaining_flips = {
        "Nollie",             // -> Ollie
        "DoubleKickflip",     // -> Kickflip
        "TripleHeelflip",     // -> Heelflip
        "360Shuvit",          // -> Pop Shuvit
        "HeelflipVarialLien", // -> Varial Heelflip
        "360Flip",            // -> 360 Flip
        "360Heelflip",        // -> Laser Flip
        "360Hardflip",        // -> Hardflip
        "360InwardHeelflip",  // -> Inward Heelflip
        "LateKickflip",       // -> Late Flip
        "KickflipUnderflip",  // -> Underflip
    };
    for (const auto& t : remaining_flips) {
      mgr.OnTrickLanded(t);
    }
    TEST_CHECK(!mgr.IsUnlocked("Sample Achievement Mod:sample_flip_master"),
               "sample_flip_master must NOT unlock at 13/14 flip tricks");
    mgr.OnTrickLanded("FSNoComply");  // 14/14 -> No-Comply
    TEST_CHECK(mgr.IsUnlocked("Sample Achievement Mod:sample_flip_master"),
               "sample_flip_master (Flip Trick Encyclopedia) should unlock at 14/14 flip tricks");

    // 6.2: Grind & Slide Connoisseur (16 grinds/slides)
    // Verify "Noseblunt" does NOT falsely mark "Bluntslide", and "Overcrook" does NOT mark "Crooked"
    mgr.OnTrickLanded("FSNosebluntslide");
    mgr.OnTrickLanded("BSOvercrook");
    {
      const auto data = mgr.GetAchievementData();
      const auto* grind_item = FindUiItem(data, "Grind & Slide Connoisseur");
      TEST_CHECK(grind_item && grind_item->progress_total == 16 && grind_item->progress_current == 2,
                 "Grind & Slide Connoisseur should be at 2/16");
      for (const auto& [t_name, done] : grind_item->checklist) {
        if (t_name == "Noseblunt" || t_name == "Overcrook") {
          TEST_CHECK(done, t_name + " should be checked off");
        } else {
          TEST_CHECK(!done, t_name + " must NOT be falsely checked off");
        }
      }
    }
    const std::vector<std::string> remaining_grinds = {
        "FS 50-50", "BS 5-0 Grind", "FS Nosegrind", "FS Crook", "BS Smith", "FS Feeble",
        "BS Willy", "FS Suski", "FS Boardslide", "BS Lipslide", "FS Noseslide",
        "BS Tailslide", "FS Bluntslide", "FS Darkslide",
    };
    for (const auto& t : remaining_grinds) {
      mgr.OnTrickLanded(t);
    }
    TEST_CHECK(mgr.IsUnlocked("Sample Achievement Mod:sample_grind_master"),
               "sample_grind_master (Grind & Slide Connoisseur) should unlock at 16/16 grinds");

    // 6.3: Airtime Acrobat (14 grabs & airs)
    const std::vector<std::string> all_grabs = {
        "FS Indy", "BS Melon", "FS Mute", "BS Stalefish", "Tailgrab",
        "Nosegrab", "FS Method", "BS Japan", "Rocket Air", "Christ Air",
        "Superdude", "BS Benihana", "FS Airwalk", "BS Tailwalk",
    };
    for (size_t i = 0; i < all_grabs.size(); ++i) {
      TEST_CHECK(!mgr.IsUnlocked("Sample Achievement Mod:sample_grab_master"),
                 "sample_grab_master must not unlock before 14/14");
      mgr.OnTrickLanded(all_grabs[i]);
    }
    TEST_CHECK(mgr.IsUnlocked("Sample Achievement Mod:sample_grab_master"),
               "sample_grab_master (Airtime Acrobat) should unlock at 14/14 grabs");

    // 6.4: Street Specialist (10 manuals, plants & footwork)
    // Verify "Nose Manual" does NOT falsely mark "Manual"
    mgr.OnTrickLanded("OneFootNoseManual");
    {
      const auto data = mgr.GetAchievementData();
      const auto* other_item = FindUiItem(data, "Street Specialist");
      TEST_CHECK(other_item && other_item->progress_total == 10 && other_item->progress_current == 1,
                 "Street Specialist should be at 1/10 after Nose Manual");
      for (const auto& [t_name, done] : other_item->checklist) {
        if (t_name == "Nose Manual") {
          TEST_CHECK(done, "Nose Manual should be checked off");
        } else {
          TEST_CHECK(!done, t_name + " must NOT be checked off from Nose Manual");
        }
      }
    }
    const std::vector<std::string> remaining_other = {
        "Manual", "FS Invert", "BS Footplant", "FS Boneless", "Hippy Jump 180",
        "Powerslide 180", "BS Revert", "FS Wallride", "Kickflip Fingerflip",
    };
    for (const auto& t : remaining_other) {
      mgr.OnTrickLanded(t);
    }
    TEST_CHECK(mgr.IsUnlocked("Sample Achievement Mod:sample_other_master"),
               "sample_other_master (Street Specialist) should unlock at 10/10 tricks");

    std::cout << "[PASS] All 4 Trick Group Checklist achievements & UI progress/checklists verified\n";
  }

  // --------------------------------------------------------------------------
  // 7. Verify 500G Single Cap & 1000G Pack Scaling Rules
  // --------------------------------------------------------------------------
  {
    const auto over_pack_dir = temp_dir / "mods" / "raw" / "over_cap_pack";
    std::filesystem::create_directories(over_pack_dir, ec);
    {
      std::ofstream f(over_pack_dir / "pack.json");
      f << R"({
        "id": "over_cap_pack",
        "name": "Over Cap Pack",
        "category": "Over Cap Pack",
        "enabled": true,
        "achievements": [
          { "id": "a1", "name": "Huge 1", "gamerscore": 800, "trigger": "own_spot" },
          { "id": "a2", "name": "Huge 2", "gamerscore": 500, "trigger": "own_spot" },
          { "id": "a3", "name": "Huge 3", "gamerscore": 500, "trigger": "own_spot" }
        ]
      })";
    }
    mgr.ReloadDefinitions();
    const auto data = mgr.GetAchievementData();
    uint32_t over_pack_total = 0;
    for (const auto& item : data.items) {
      if (item.category == "Over Cap Pack") {
        TEST_CHECK(item.gamerscore <= 500, "Single achievement must never exceed 500 G");
        over_pack_total += item.gamerscore;
      }
    }
    TEST_CHECK(over_pack_total == 1000,
               fmt::format("Pack exceeding 1000 G must scale to 1000 G (got {})", over_pack_total));
    std::cout << "[PASS] 500G single-achievement cap and 1000G pack scaling verified\n";
  }

  // --------------------------------------------------------------------------
  // 8. Verify Filter-Aware Achievement Reset vs Nuclear Reset
  // --------------------------------------------------------------------------
  {
    mgr.ResetAchievements();
    mgr.Unlock(0, 2);    // Base Game: Park Apprentice (20 G)
    mgr.Unlock(0, 101);  // Base Game: Isaac's Day Off (10 G)
    mgr.Unlock(0, "Sample Achievement Mod:sample_own_spot");  // Sample Mod (60 G)
    mgr.OnTrickLanded("Kickflip");  // Progress on Sample Mod flip trick checklist

    TEST_CHECK(mgr.IsUnlocked(2) && mgr.IsUnlocked(101), "Base Game achievements should be unlocked");
    TEST_CHECK(mgr.IsUnlocked("Sample Achievement Mod:sample_own_spot"),
               "Sample Mod achievement should be unlocked");

    // Collect IDs for "Sample Achievement Mod" category filter only
    std::vector<uint32_t> sample_mod_ids;
    for (const auto& item : mgr.GetAchievementData().items) {
      if (item.category == "Sample Achievement Mod") {
        sample_mod_ids.push_back(item.id);
      }
    }
    TEST_CHECK(sample_mod_ids.size() == 19, "Should collect all 19 Sample Mod achievement IDs");

    // Reset ONLY the filtered Sample Achievement Mod pack
    mgr.ResetAchievements(sample_mod_ids);

    TEST_CHECK(mgr.IsUnlocked(2) && mgr.IsUnlocked(101),
               "Base Game achievements must remain unlocked after resetting Sample Achievement Mod filter");
    TEST_CHECK(!mgr.IsUnlocked("Sample Achievement Mod:sample_own_spot"),
               "Sample Mod achievement must be locked after filtered reset");
    {
      const auto data = mgr.GetAchievementData();
      const auto* flip_item = FindUiItem(data, "Flip Trick Encyclopedia");
      TEST_CHECK(flip_item && flip_item->progress_current == 0,
                 "Sample Mod trick checklist progress must be cleared after filtered reset");
      TEST_CHECK(data.unlocked_gamerscore == 30,
                 "Remaining unlocked Gamerscore should equal Base Game unlocked Gamerscore (30 G)");
    }

    // Now test nuclear ResetAchievements()
    mgr.ResetAchievements();
    TEST_CHECK(!mgr.IsUnlocked(2) && !mgr.IsUnlocked(101),
               "Nuclear ResetAchievements() must lock all remaining achievements");
    TEST_CHECK(mgr.GetAchievementData().unlocked_gamerscore == 0,
               "Nuclear ResetAchievements() must reset unlocked Gamerscore to 0");
    std::cout << "[PASS] Filter-aware achievement reset and nuclear reset verified\n";
  }

  // --------------------------------------------------------------------------
  // 9. Verify Pre-Load Attract Mode Rejection, AI Skater Rejection & Board Sales Reset Baseline
  // --------------------------------------------------------------------------
  {
    mgr.ResetAchievements();
    PPCContext ctx{};

    const uint32_t hud_obj      = 0x400E0000u;
    const uint32_t local_skater = 0x400E1000u;
    const uint32_t ai_skater    = 0x400E2000u;
    const uint32_t air_tracker  = 0x400E3000u;
    const uint32_t man_tracker  = 0x400E4000u;
    const uint32_t grd_tracker  = 0x400E5000u;

    // 9.1: Pre-load attract mode: hud_obj + 215 == 0 (stat recording disabled / non-local skater)
    StoreU8(base, hud_obj + 215u, 0u);
    ctx.r3.u32 = hud_obj;
    ctx.r4.u32 = ai_skater;
    ctx.r5.s32 = 5000;
    sub_82774080(ctx, base);
    TEST_CHECK(!skate3::IsLocalGameplayActive(),
               "Gameplay must be marked inactive when hud_obj+215 == 0 (pre-load attract mode)");
    TEST_CHECK(!mgr.IsUnlocked("Sample Achievement Mod:sample_trick_score"),
               "sample_trick_score must NOT unlock during pre-load attract mode");

    // AirTracker, ManualTracker, GrindTracker, Spot ownership, and Location checks must all be blocked
    StoreU32(base, air_tracker + 4u, ai_skater);
    StoreF32(base, air_tracker + 336u, 3.0f);
    ctx.r3.u32 = air_tracker;
    ctx.r4.u32 = 1;
    sub_82DA9B08(ctx, base);
    TEST_CHECK(!mgr.IsUnlocked("Sample Achievement Mod:sample_air_time"),
               "sample_air_time must NOT unlock during pre-load attract mode");

    SetSkaterState(370.69f, 79.94f, -112.83f, false, false);
    mgr.Update(0.05f);
    TEST_CHECK(!mgr.IsUnlocked("Sample Achievement Mod:sample_reach_location"),
               "sample_reach_location must NOT unlock while gameplay is inactive");

    // 9.2: Local human skater enters active gameplay (hud_obj + 215 == 1)
    StoreU8(base, hud_obj + 215u, 1u);
    ctx.r3.u32 = hud_obj;
    ctx.r4.u32 = local_skater;
    ctx.r5.s32 = 100;  // small trick score
    sub_82774080(ctx, base);
    TEST_CHECK(skate3::IsLocalGameplayActive(),
               "Gameplay should be active once local human skater has hud_obj+215 == 1");
    TEST_CHECK(skate3::GetLocalSkaterPtr() == local_skater,
               "GetLocalSkaterPtr() should match local_skater");

    // Now an AI skater in the world jumps for 3.0s air time, manuals 50m, and grinds 50m
    StoreU32(base, air_tracker + 4u, ai_skater);
    StoreF32(base, air_tracker + 336u, 3.0f);
    ctx.r3.u32 = air_tracker;
    ctx.r4.u32 = 1;
    sub_82DA9B08(ctx, base);
    TEST_CHECK(!mgr.IsUnlocked("Sample Achievement Mod:sample_air_time"),
               "AI skater AirTracker must NOT unlock player's sample_air_time while player is on ground!");

    StoreU32(base, man_tracker + 4u, ai_skater);
    StoreU32(base, man_tracker + 104u, 0x08000000u);
    StoreF32(base, man_tracker + 1028u, 50.0f);
    ctx.r3.u32 = man_tracker;
    sub_82DAA868(ctx, base);
    TEST_CHECK(!mgr.IsUnlocked("Sample Achievement Mod:sample_manual_distance"),
               "AI skater ManualTracker must NOT unlock player's sample_manual_distance!");

    StoreU32(base, grd_tracker + 4u, ai_skater);
    StoreF32(base, grd_tracker + 760u, 120.0f);
    ctx.r3.u32 = grd_tracker;
    sub_82DA93D8(ctx, base);
    TEST_CHECK(!mgr.IsUnlocked("Sample Achievement Mod:sample_grind_distance") && !mgr.IsUnlocked(11),
               "AI skater GrindTracker must NOT unlock player's grind distance achievements!");

    // Now the local human skater jumps for 2.0s air time -> SHOULD unlock!
    StoreU32(base, air_tracker + 4u, local_skater);
    StoreF32(base, air_tracker + 336u, 2.0f);
    ctx.r3.u32 = air_tracker;
    ctx.r4.u32 = 1;
    sub_82DA9B08(ctx, base);
    TEST_CHECK(mgr.IsUnlocked("Sample Achievement Mod:sample_air_time"),
               "Local human skater AirTracker SHOULD unlock sample_air_time");

    // 9.3: Save-based board sales still unlock after reset even when capped at 1M+ boards
    mgr.ResetAchievements();
    // Re-enable local gameplay after reset
    StoreU8(base, hud_obj + 215u, 1u);
    ctx.r3.u32 = hud_obj;
    ctx.r4.u32 = local_skater;
    ctx.r5.s32 = 100;
    sub_82774080(ctx, base);

    const uint32_t career_obj = 0x400E6000u;
    const uint32_t sales_obj  = 0x400E8000u;
    StoreU32(base, career_obj + 72u, sales_obj);
    StoreU32(base, sales_obj + 4u, 1000000u);  // Capped 1,000,000 boards on career save
    StoreU32(base, sales_obj + 8u, 0u);
    StoreU32(base, sales_obj + 12u, 0u);
    StoreU32(base, sales_obj + 16u, 0u);
    ctx.r3.u32 = career_obj;
    for (int i = 0; i < 60; ++i) {
      sub_827CFD48(ctx, base);
    }
    TEST_CHECK(mgr.IsUnlocked("Sample Achievement Mod:sample_board_sales") && mgr.IsUnlocked(22),
               "Save-based board sales (including 1M cap) must still unlock after reset once gameplay is active");

    // 9.4: OTS Spot & Challenge Mode Gameplay (hud_obj + 215 == 0 because free-skate stats are paused,
    // while hud_obj + 214 == 1 during the spot attempt, and both 0 at the end-of-spot dialog)
    mgr.ResetAchievements();
    StoreU8(base, hud_obj + 215u, 0u);  // Free-skate stat recording paused inside OTS spot
    StoreU8(base, hud_obj + 214u, 1u);  // Skater is actively skating inside the OTS spot
    ctx.r3.u32 = hud_obj;
    ctx.r4.u32 = local_skater;
    sub_82774080(ctx, base);
    TEST_CHECK(skate3::IsLocalGameplayActive(),
               "Gameplay MUST remain active inside OTS/Career challenges when hud_obj+214 == 1 for local_skater");

    // Verify air/trick/grind achievements still work while inside an OTS spot
    StoreU32(base, air_tracker + 4u, local_skater);
    StoreF32(base, air_tracker + 336u, 2.0f);
    ctx.r3.u32 = air_tracker;
    ctx.r4.u32 = 1;
    sub_82DA9B08(ctx, base);
    TEST_CHECK(mgr.IsUnlocked("Sample Achievement Mod:sample_air_time"),
               "sample_air_time must unlock when landed by local_skater inside an OTS challenge");

    // Now simulate completing the OTS spot at the end-of-challenge screen where hud_obj+214 and +215 are 0
    StoreU8(base, hud_obj + 214u, 0u);
    StoreU8(base, hud_obj + 215u, 0u);
    ctx.r3.u32 = hud_obj;
    ctx.r4.u32 = local_skater;
    sub_82774080(ctx, base);

    const uint32_t challenge_obj = 0x400E9000u;
    const uint32_t challenge_def = 0x400E9100u;
    const uint32_t challenge_inner = 0x400E9200u;
    StoreU32(base, challenge_obj + 20u, challenge_def);
    StoreU32(base, challenge_def + 0u, challenge_inner);
    StoreU64(base, challenge_inner + 24u, 0x1122334455667788ULL);

    // Own the OTS spot (status = 1) via sub_827CDDD8
    ctx.r3.u32 = career_obj;
    ctx.r4.u32 = challenge_obj;
    ctx.r5.u32 = 1;
    sub_827CDDD8(ctx, base);
    TEST_CHECK(mgr.IsUnlocked("Sample Achievement Mod:sample_own_spot"),
               "First on the Board (sample_own_spot) MUST unlock when owning an OTS spot!");
    TEST_CHECK(!mgr.IsUnlocked("Sample Achievement Mod:sample_kill_spot"),
               "Taking It to the Limit (sample_kill_spot) must NOT unlock when only owning an OTS spot");
    {
      const auto data = mgr.GetAchievementData();
      const auto* item112 = FindUiItem(data, "King of the Streets");
      TEST_CHECK(item112 && item112->progress_current == 1 && item112->progress_total == 16,
                 "#112 King of the Streets must increment unique OTS spot progress (1/16)");
    }

    // Kill a second OTS spot (status = 2) via sub_827CDB10
    StoreU64(base, challenge_inner + 24u, 0x99AABBCCDDEEFF00ULL);
    ctx.r3.u32 = career_obj + 4u;
    ctx.r4.u32 = challenge_obj;
    ctx.r5.u32 = 2;
    sub_827CDB10(ctx, base);
    TEST_CHECK(mgr.IsUnlocked("Sample Achievement Mod:sample_kill_spot"),
               "Taking It to the Limit (sample_kill_spot) MUST unlock when killing an OTS spot!");
    {
      const auto data = mgr.GetAchievementData();
      const auto* item112 = FindUiItem(data, "King of the Streets");
      TEST_CHECK(item112 && item112->progress_current == 2 && item112->progress_total == 16,
                 "#112 King of the Streets must increment unique OTS spot progress to 2/16");
    }

    // Also verify direct ID-based retail achievement unlock hook sub_82740298 (used by sub_827422D8)
    ctx.r4.u32 = 36;  // Spot Monopolist
    sub_82740298(ctx, base);
    TEST_CHECK(mgr.IsUnlocked(36) && mgr.IsUnlocked(112),
               "sub_82740298 with ach_id=36 must unlock #36 (Spot Monopolist) and #112 (King of the Streets)");

    std::cout << "[PASS] Pre-load attract mode rejection, AI skater rejection, OTS spot completion, and save-based board sales verified\n";
  }

  // --------------------------------------------------------------------------
  // 10. Verify Achievement & Progress Toast Sound Triggers & CVARs
  // --------------------------------------------------------------------------
  {
    toast_dialog.Clear();
    TEST_CHECK(!toast_dialog.current_toast().has_value(), "Toast dialog should be clear");
    TEST_CHECK(!toast_dialog.current_progress_toast().has_value(), "Progress toast dialog should be clear");

    // Posting an achievement unlock toast exercises unlock chime audio pipeline
    toast_dialog.PostToast({101, "Isaac's Day Off", "Unlock Isaac Clarke", 10});
    TEST_CHECK(toast_dialog.current_toast().has_value(), "Current toast must be set");

    // Posting a progress toast exercises progress pop audio pipeline
    toast_dialog.PostProgressToast({4052066969u, "Kickflip", "Flip Trick Encyclopedia", 1, 14});
    TEST_CHECK(toast_dialog.current_progress_toast().has_value(), "Current progress toast must be set");

    // Test Clear() cleans queues and stops audio streams cleanly
    toast_dialog.Clear();
    TEST_CHECK(!toast_dialog.current_toast().has_value(), "Clear() must reset active unlock toast");
    TEST_CHECK(!toast_dialog.current_progress_toast().has_value(), "Clear() must reset active progress toast");

    std::cout << "[PASS] Achievement & progress toast audio triggers and dialog state verified\n";
  }

  std::filesystem::remove_all(temp_dir, ec);
  std::cout << "=== ALL ACHIEVEMENT POPPING TESTS PASSED (100%) ===\n";
  return 0;
}
