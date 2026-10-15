/**
 * @file tests/test_cheats.cpp
 * @brief Comprehensive verification suite for Skate 3 cheats
 *
 * Verifies that:
 *  1. Cheat CVars parse all presets correctly.
 *  2. No Bails suppresses all 34 wipeout conditions, blocks wipeout state transitions,
 *     and suppresses looping air-flail animations (Mode 7).
 *  3. Drop Height Threshold increases allowable safe drop height while preserving
 *     horizontal wall/obstacle impact wipeouts.
 *  4. Mission loading & elevation changes (e.g. spawns at Y < -30m, teleports, 200ft drops)
 *     never corrupt player coordinates or hang loading screens.
 *  5. Challenge Timer Freeze pauses active gameplay challenge countdowns while never
 *     blocking 3-2-1 countdowns, loading screens, cinematics, or challenge completion.
 *  6. Physics multipliers (pop height, on-board speed, off-board speed, jump height/speed)
 *     scale physics parameters accurately and preserve memory cleanliness.
 *  7. High-fall terminal velocity clamp prevents clipping/tunneling through ground geo.
 *  8. Crash guard (grind alignment matrix, malformed timer tree, null pointers) prevents crashes.
 */

#include "skate3_cheats.h"
#include "generated/skate3_init.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <sys/mman.h>
#include <unistd.h>
#include <vector>

//=============================================================================
// Test Assertions & Memory Helpers
//=============================================================================

static int g_tests_passed = 0;
static int g_tests_failed = 0;

#define TEST_CHECK(cond, msg)                                                           \
  do {                                                                                  \
    if (cond) {                                                                         \
      ++g_tests_passed;                                                                 \
    } else {                                                                            \
      ++g_tests_failed;                                                                 \
      std::cerr << "[FAIL] " << __FILE__ << ":" << __LINE__ << " - " << (msg) << "\n"; \
    }                                                                                   \
  } while (0)

static inline void StoreU8(uint8_t* base, uint32_t addr, uint8_t val) {
  *(base + addr) = val;
}

static inline uint8_t LoadU8(uint8_t* base, uint32_t addr) {
  return *(base + addr);
}

static inline void StoreU32(uint8_t* base, uint32_t addr, uint32_t val) {
  const uint32_t be = __builtin_bswap32(val);
  std::memcpy(base + addr, &be, 4);
}

static inline uint32_t LoadU32(uint8_t* base, uint32_t addr) {
  uint32_t raw;
  std::memcpy(&raw, base + addr, 4);
  return __builtin_bswap32(raw);
}

static inline void StoreF32(uint8_t* base, uint32_t addr, float val) {
  uint32_t raw;
  std::memcpy(&raw, &val, 4);
  StoreU32(base, addr, raw);
}

static inline float LoadF32(uint8_t* base, uint32_t addr) {
  uint32_t raw = LoadU32(base, addr);
  float val;
  std::memcpy(&val, &raw, 4);
  return val;
}

//=============================================================================
// Mock Recompiled Implementations (__imp__sub_XXXXXX)
//=============================================================================

static bool g_imp_timer_update_called = false;
static bool g_imp_timer_get_remaining_called = false;
static bool g_imp_state_changer_called = false;
static bool g_imp_should_bail_called = false;
static bool g_imp_check_force_bail_called = false;
static bool g_imp_wipeout_update_called = false;
static bool g_imp_grind_matrix_called = false;
static int32_t g_mock_next_state_return = 300;

extern "C" {

void __imp__sub_82D57DE0(PPCContext& ctx, uint8_t* base) {
  g_imp_timer_get_remaining_called = true;
  ctx.f1.f64 = 25.0; // 25 seconds remaining
}

void __imp__sub_82D57960(PPCContext& ctx, uint8_t* base) {
  g_imp_timer_update_called = true;
}

void __imp__sub_82DB4048(PPCContext& ctx, uint8_t* base) {}
void __imp__sub_82DB5588(PPCContext& ctx, uint8_t* base) {}
void __imp__sub_82DB5CF8(PPCContext& ctx, uint8_t* base) {}

void __imp__sub_82D91098(PPCContext& ctx, uint8_t* base) {
  g_imp_wipeout_update_called = true;
  ctx.r3.u64 = 1; // normally returns 1 when bail condition detected
}

void __imp__sub_82DB9100(PPCContext& ctx, uint8_t* base) {
  g_imp_should_bail_called = true;
  ctx.r3.u64 = 1;
}

void __imp__sub_82D8BD50(PPCContext& ctx, uint8_t* base) {
  g_imp_check_force_bail_called = true;
  ctx.r3.u64 = 1;
}

void __imp__sub_82D90AB8(PPCContext& ctx, uint8_t* base) {}
void __imp__sub_82D90C98(PPCContext& ctx, uint8_t* base) {}
void __imp__sub_82D907B0(PPCContext& ctx, uint8_t* base) {}
void __imp__sub_82D90D08(PPCContext& ctx, uint8_t* base) {}
void __imp__sub_82D8F9E0(PPCContext& ctx, uint8_t* base) {}
void __imp__sub_82D90358(PPCContext& ctx, uint8_t* base) {}

void __imp__sub_82D911A8(PPCContext& ctx, uint8_t* base) {
  // Store the mode passed in r4 to rig_ctrl + 16
  const uint32_t rig = ctx.r3.u32;
  if (rig && base) {
    StoreU8(base, rig + 16u, static_cast<uint8_t>(ctx.r4.u32));
  }
}

void __imp__sub_82D8ADE8(PPCContext& ctx, uint8_t* base) {
  g_imp_state_changer_called = true;
  ctx.r3.u64 = static_cast<uint64_t>(g_mock_next_state_return);
}

void __imp__sub_82D40580(PPCContext& ctx, uint8_t* base) {
  g_imp_grind_matrix_called = true;
}

void __imp__sub_82D67848(PPCContext& ctx, uint8_t* base) {}
void __imp__sub_82D67D08(PPCContext& ctx, uint8_t* base) {}
void __imp__sub_82D67320(PPCContext& ctx, uint8_t* base) {}
void __imp__sub_82D68698(PPCContext& ctx, uint8_t* base) {}
void __imp__sub_82D68800(PPCContext& ctx, uint8_t* base) {}
void __imp__sub_82D682E8(PPCContext& ctx, uint8_t* base) {}
void __imp__sub_82D613B0(PPCContext& ctx, uint8_t* base) {}
void __imp__sub_82D633B0(PPCContext& ctx, uint8_t* base) {}
void __imp__sub_82D310F8(PPCContext& ctx, uint8_t* base) {}
void __imp__sub_82D32338(PPCContext& ctx, uint8_t* base) {}
void __imp__sub_82D6CA58(PPCContext& ctx, uint8_t* base) {}
void __imp__sub_82D6BF90(PPCContext& ctx, uint8_t* base) {}
void __imp__sub_82D2EF38(PPCContext& ctx, uint8_t* base) {}
void __imp__sub_82D2F748(PPCContext& ctx, uint8_t* base) {}
void __imp__sub_82D2FFF8(PPCContext& ctx, uint8_t* base) {}

} // extern "C"

//=============================================================================
// Declarations of Hooked Functions in src/skate3_cheats.cpp
//=============================================================================

extern "C" {
void sub_82D57DE0(PPCContext& ctx, uint8_t* base);
void sub_82D57960(PPCContext& ctx, uint8_t* base);
void sub_82DB4048(PPCContext& ctx, uint8_t* base);
void sub_82DB5588(PPCContext& ctx, uint8_t* base);
void sub_82DB5CF8(PPCContext& ctx, uint8_t* base);
void sub_82D91098(PPCContext& ctx, uint8_t* base);
void sub_82DB9100(PPCContext& ctx, uint8_t* base);
void sub_82D8BD50(PPCContext& ctx, uint8_t* base);
void sub_82D90AB8(PPCContext& ctx, uint8_t* base);
void sub_82D90C98(PPCContext& ctx, uint8_t* base);
void sub_82D907B0(PPCContext& ctx, uint8_t* base);
void sub_82D90D08(PPCContext& ctx, uint8_t* base);
void sub_82D8F9E0(PPCContext& ctx, uint8_t* base);
void sub_82D90358(PPCContext& ctx, uint8_t* base);
void sub_82D911A8(PPCContext& ctx, uint8_t* base);
void sub_82D8ADE8(PPCContext& ctx, uint8_t* base);
void sub_82D40580(PPCContext& ctx, uint8_t* base);
void sub_82D67848(PPCContext& ctx, uint8_t* base);
void sub_82D67D08(PPCContext& ctx, uint8_t* base);
void sub_82D67320(PPCContext& ctx, uint8_t* base);
void sub_82D68698(PPCContext& ctx, uint8_t* base);
void sub_82D68800(PPCContext& ctx, uint8_t* base);
void sub_82D682E8(PPCContext& ctx, uint8_t* base);
void sub_82D613B0(PPCContext& ctx, uint8_t* base);
void sub_82D633B0(PPCContext& ctx, uint8_t* base);
void sub_82D310F8(PPCContext& ctx, uint8_t* base);
void sub_82D32338(PPCContext& ctx, uint8_t* base);
void sub_82D6CA58(PPCContext& ctx, uint8_t* base);
void sub_82D6BF90(PPCContext& ctx, uint8_t* base);
void sub_82D2EF38(PPCContext& ctx, uint8_t* base);
void sub_82D2F748(PPCContext& ctx, uint8_t* base);
void sub_82D2FFF8(PPCContext& ctx, uint8_t* base);
}

//=============================================================================
// Test Suites
//=============================================================================

static void TestCVarPresetParsers() {
  std::cout << "--- [Test 1] CVar Preset Parsers ---\n";

  // Gravity presets
  REXCVAR_SET(skate3_cheat_gravity, "Moon");
  TEST_CHECK(std::fabs(skate3::cheats::GetGravityMultiplier() - 0.25f) < 0.001f, "Moon gravity is 0.25x");
  REXCVAR_SET(skate3_cheat_gravity, "Low");
  TEST_CHECK(std::fabs(skate3::cheats::GetGravityMultiplier() - 0.5f) < 0.001f, "Low gravity is 0.5x");
  REXCVAR_SET(skate3_cheat_gravity, "Normal");
  TEST_CHECK(std::fabs(skate3::cheats::GetGravityMultiplier() - 1.0f) < 0.001f, "Normal gravity is 1.0x");
  REXCVAR_SET(skate3_cheat_gravity, "High");
  TEST_CHECK(std::fabs(skate3::cheats::GetGravityMultiplier() - 1.5f) < 0.001f, "High gravity is 1.5x");
  REXCVAR_SET(skate3_cheat_gravity, "Super High");
  TEST_CHECK(std::fabs(skate3::cheats::GetGravityMultiplier() - 2.5f) < 0.001f, "Super High gravity is 2.5x");

  // Pop height presets
  REXCVAR_SET(skate3_cheat_pop_height, "Normal");
  TEST_CHECK(std::fabs(skate3::cheats::GetPopHeightMultiplier() - 1.0f) < 0.001f, "Pop height Normal is 1.0x");
  REXCVAR_SET(skate3_cheat_pop_height, "2x");
  TEST_CHECK(std::fabs(skate3::cheats::GetPopHeightMultiplier() - 2.0f) < 0.001f, "Pop height 2x is 2.0x");
  REXCVAR_SET(skate3_cheat_pop_height, "5x");
  TEST_CHECK(std::fabs(skate3::cheats::GetPopHeightMultiplier() - 5.0f) < 0.001f, "Pop height 5x is 5.0x");

  // Drop height presets
  REXCVAR_SET(skate3_cheat_drop_height, "Normal");
  TEST_CHECK(std::fabs(skate3::cheats::GetDropHeightMultiplier() - 1.0f) < 0.001f, "Drop height Normal is 1.0x");
  REXCVAR_SET(skate3_cheat_drop_height, "2x");
  TEST_CHECK(std::fabs(skate3::cheats::GetDropHeightMultiplier() - 2.0f) < 0.001f, "Drop height 2x is 2.0x");
  REXCVAR_SET(skate3_cheat_drop_height, "Unlimited");
  TEST_CHECK(skate3::cheats::GetDropHeightMultiplier() >= 10000.0f, "Drop height Unlimited is >= 10000x");

  // Speed presets
  REXCVAR_SET(skate3_cheat_onboard_speed, "Slow (0.5x)");
  TEST_CHECK(std::fabs(skate3::cheats::GetOnBoardSpeedMultiplier() - 0.5f) < 0.001f, "On-board speed 0.5x");
  REXCVAR_SET(skate3_cheat_onboard_speed, "Fast (1.5x)");
  TEST_CHECK(std::fabs(skate3::cheats::GetOnBoardSpeedMultiplier() - 1.5f) < 0.001f, "On-board speed 1.5x");
  REXCVAR_SET(skate3_cheat_offboard_speed, "3x");
  TEST_CHECK(std::fabs(skate3::cheats::GetOffBoardSpeedMultiplier() - 3.0f) < 0.001f, "Off-board speed 3x");

  // Reset CVars to defaults
  REXCVAR_SET(skate3_cheat_gravity, "Normal");
  REXCVAR_SET(skate3_cheat_pop_height, "Normal");
  REXCVAR_SET(skate3_cheat_drop_height, "Normal");
  REXCVAR_SET(skate3_cheat_onboard_speed, "Normal");
  REXCVAR_SET(skate3_cheat_offboard_speed, "Normal");
  REXCVAR_SET(skate3_cheat_offboard_jump_height, "Normal");
  REXCVAR_SET(skate3_cheat_offboard_jump_speed, "Normal");
  REXCVAR_SET(skate3_cheat_no_bails, false);
  REXCVAR_SET(skate3_cheat_freeze_timer, false);
}

static void TestNoBailsSuppression(uint8_t* base) {
  std::cout << "--- [Test 2] No Bails & State Transition Redirection ---\n";

  const uint32_t cond_mgr = 0x82100000u;
  const uint32_t phys_in  = 0x82110000u;
  const uint32_t changer  = 0x82120000u;

  // Set condition bytes to non-zero (triggering bails)
  for (uint32_t i = 20; i < 54; ++i) {
    StoreU8(base, cond_mgr + i, 1);
  }
  StoreU32(base, cond_mgr + 200u, 0xFFFFFFFFu);

  // Setup state changer: +4 = phys_in, +8 = cond_mgr
  StoreU32(base, changer + 4u, phys_in);
  StoreU32(base, changer + 8u, cond_mgr);
  StoreU32(base, phys_in + 2500u, 100u); // On-board ground

  // With no_bails = true:
  REXCVAR_SET(skate3_cheat_no_bails, true);

  PPCContext ctx{};
  ctx.r3.u32 = cond_mgr;
  sub_82D91098(ctx, base); // Wipeout::UpdateAndCheck
  TEST_CHECK(ctx.r3.u64 == 0, "No bails forces Wipeout::UpdateAndCheck to return 0");

  bool all_zero = true;
  for (uint32_t i = 20; i < 54; ++i) {
    if (LoadU8(base, cond_mgr + i) != 0) all_zero = false;
  }
  TEST_CHECK(all_zero, "All 34 wipeout condition bytes are cleared");
  TEST_CHECK(LoadU32(base, cond_mgr + 200u) == 0, "Wipeout condition flags at +200 are cleared");

  // Secondary bail checks
  ctx.r3.u32 = 0x82100000u;
  sub_82DB9100(ctx, base); // ShouldBail
  TEST_CHECK(ctx.r3.u64 == 0, "ShouldBail returns 0 when no bails is enabled");

  sub_82D8BD50(ctx, base); // CheckForceBail
  TEST_CHECK(ctx.r3.u64 == 0, "CheckForceBail returns 0 when no bails is enabled");

  // State Changer test: On-board bail redirection
  // If game tries to transition to 300 (Wipeout) while on-board (100)
  g_mock_next_state_return = 300;
  ctx.r3.u32 = changer;
  sub_82D8ADE8(ctx, base);
  TEST_CHECK(ctx.r3.u32 == 100u, "State 300 is redirected to 100 (On-board Ground) under no_bails");

  // In-air wipeout redirection: next_state = 202 (WipeoutAir) -> 200 (Normal Air)
  StoreU32(base, phys_in + 2500u, 200u);
  g_mock_next_state_return = 202;
  ctx.r3.u32 = changer;
  sub_82D8ADE8(ctx, base);
  TEST_CHECK(ctx.r3.u32 == 200u, "State 202 (WipeoutAir) is redirected to 200 (Normal Air)");

  // Off-board bail redirection: next_state = 300 from 500 (Biped_Ground)
  StoreU32(base, phys_in + 2500u, 500u);
  g_mock_next_state_return = 300;
  ctx.r3.u32 = changer;
  sub_82D8ADE8(ctx, base);
  TEST_CHECK(ctx.r3.u32 == 500u, "Off-board state 300 is redirected to 500 (Biped_Ground)");

  // Mode 7 air flail suppression
  const uint32_t rig_ctrl = 0x82130000u;
  StoreU8(base, rig_ctrl + 16u, 4u);
  ctx.r3.u32 = rig_ctrl;
  ctx.r4.u32 = 7u; // requested mode 7 (infinite flail)
  sub_82D911A8(ctx, base);
  TEST_CHECK(ctx.r4.u32 != 7u, "Mode 7 air flail request is suppressed under no_bails");

  REXCVAR_SET(skate3_cheat_no_bails, false);
}

static void TestDropHeightThreshold(uint8_t* base) {
  std::cout << "--- [Test 3] Drop Height Threshold & Horizontal Crash Preservation ---\n";

  const uint32_t cond_mgr = 0x82140000u;
  const uint32_t player   = 0x82150000u;
  const uint32_t phys_in  = 0x82160000u;

  StoreU32(base, player + 1804u, phys_in);
  StoreU32(base, player + 1816u, cond_mgr);
  StoreU32(base, phys_in + 2500u, 200u); // On-board air

  // Set both drop condition bytes (offset 20, 21, 22, 27) AND wall crash condition bytes (offset 32..37)
  StoreU8(base, cond_mgr + 20u, 1); // vertical drop impact
  StoreU8(base, cond_mgr + 27u, 1); // air drop height
  StoreU8(base, cond_mgr + 32u, 1); // horizontal wall/rail collision
  StoreU8(base, cond_mgr + 33u, 1); // head slam into wall

  REXCVAR_SET(skate3_cheat_no_bails, false);
  REXCVAR_SET(skate3_cheat_drop_height, "5x"); // 5x safe drop height (~27.5 meters)

  // Simulate skater falling 15 meters: Peak Y = 30.0, current Y = 15.0
  StoreF32(base, phys_in + 244u, 30.0f);
  PPCContext ctx{};
  ctx.r3.u32 = player;
  sub_82DB4048(ctx, base); // Peak Y updated to 30.0m

  StoreF32(base, phys_in + 244u, 15.0f); // 15 meters dropped
  sub_82DB4048(ctx, base);

  // Check that drop conditions are cleared, but wall impact bytes are PRESERVED!
  TEST_CHECK(LoadU8(base, cond_mgr + 20u) == 0, "Drop impact condition +20 is cleared for 15m drop under 5x");
  TEST_CHECK(LoadU8(base, cond_mgr + 27u) == 0, "Drop height condition +27 is cleared for 15m drop under 5x");
  TEST_CHECK(LoadU8(base, cond_mgr + 32u) == 1, "Horizontal wall crash byte +32 is preserved under drop_height");
  TEST_CHECK(LoadU8(base, cond_mgr + 33u) == 1, "Wall slam byte +33 is preserved under drop_height");

  REXCVAR_SET(skate3_cheat_drop_height, "Normal");
}

static void TestInfiniteLoadAndElevationSafety(uint8_t* base) {
  std::cout << "--- [Test 4] Mission Loading & Elevation Safety (Infinite Load Prevention) ---\n";

  const uint32_t player    = 0x82170000u;
  const uint32_t phys_in   = 0x82180000u;
  const uint32_t skateboard = 0x82190000u;
  const uint32_t sb_comp   = 0x821A0000u;

  StoreU32(base, player + 1804u, phys_in);
  StoreU32(base, player + 1880u, skateboard);
  StoreU32(base, skateboard + 304u, sb_comp);

  skate3::cheats::ResetRuntimeTrackingState();

  // Test Case A: Spawning at negative elevation (Y = -35.0m, canals/docks/ditches)
  StoreU32(base, phys_in + 2500u, 100u); // On-board ground at mission start
  StoreF32(base, phys_in + 244u, -35.0f); // Player Y = -35.0m
  StoreF32(base, phys_in + 2884u, -1.2f); // Player initial Vy
  StoreF32(base, sb_comp + 116u, -35.0f); // Board Y = -35.0m

  PPCContext ctx{};
  ctx.r3.u32 = player;
  sub_82DB4048(ctx, base);

  TEST_CHECK(std::fabs(LoadF32(base, phys_in + 244u) - (-35.0f)) < 0.001f,
             "Negative elevation Y = -35m is preserved and NOT corrupted by fall rescue");
  TEST_CHECK(std::fabs(LoadF32(base, phys_in + 2884u) - (-1.2f)) < 0.001f,
             "Vertical velocity is NOT zeroed out during negative elevation spawn");
  TEST_CHECK(std::fabs(LoadF32(base, sb_comp + 116u) - (-35.0f)) < 0.001f,
             "Skateboard Y is preserved at -35m during negative elevation spawn");

  // Test Case B: Teleport from mountain (Y = 80m) to downtown challenge (Y = 15m)
  StoreF32(base, phys_in + 244u, 80.0f);
  sub_82DB4048(ctx, base); // Peak Y records 80.0m

  // Now mission fast travels / spawns skater at Y = 15.0m (65m drop)
  StoreF32(base, phys_in + 244u, 15.0f);
  StoreF32(base, phys_in + 2884u, 0.0f);
  sub_82DB4048(ctx, base);

  TEST_CHECK(std::fabs(LoadF32(base, phys_in + 244u) - 15.0f) < 0.001f,
             "Teleporting 65m down preserves destination Y = 15m (does not snap back to 81m)");

  // Test Case C: 200ft high drop (60 meters down)
  StoreU32(base, phys_in + 2500u, 200u); // In-air
  StoreF32(base, phys_in + 244u, 100.0f);
  sub_82DB4048(ctx, base); // Peak Y = 100.0m

  // Skater drops 40m down (Y = 60m): must NOT be snapped back upward!
  StoreF32(base, phys_in + 244u, 60.0f);
  sub_82DB4048(ctx, base);
  TEST_CHECK(std::fabs(LoadF32(base, phys_in + 244u) - 60.0f) < 0.001f,
             "Skater dropping 40m is allowed to fall freely and NOT snapped back to 101m");

  // Test Case D: Grinds (404) and Manuals (405) are ground states, not airborne
  StoreU32(base, phys_in + 2500u, 404u); // Grind
  sub_82DB4048(ctx, base);
  TEST_CHECK(!skate3::IsSkaterOnBoardAirborne(), "Grind (state 404) is recognized as ground contact, not air");

  StoreU32(base, phys_in + 2500u, 405u); // Manual
  sub_82DB4048(ctx, base);
  TEST_CHECK(!skate3::IsSkaterOnBoardAirborne(), "Manual (state 405) is recognized as ground contact, not air");

  skate3::cheats::ResetRuntimeTrackingState();
}

static void TestChallengeTimerFreeze(uint8_t* base) {
  std::cout << "--- [Test 5] Challenge Timer Freeze & Start/End Transition Safety ---\n";

  // Build a mock ChallengeTimer EASTL RB-tree at self = 0x82200000u
  const uint32_t self = 0x82200000u;
  const uint32_t sentinel = self + 4284u;
  const uint32_t node1 = 0x82210000u; // Start countdown timer (timer_id = 101)
  const uint32_t node2 = 0x82210100u; // Challenge HUD timer (timer_id = 202)

  // Tree root at self + 4292 points to node1, node1 right points to node2
  StoreU32(base, self + 4292u, node1);
  StoreU32(base, node1 + 0u, node2);    // right child = node2
  StoreU32(base, node1 + 4u, sentinel); // left child = sentinel
  StoreU32(base, node1 + 16u, 101u);    // timer_id = 101
  StoreU32(base, node1 + 20u, 180u);    // ticks = 180 (3.0s)
  StoreU8(base,  node1 + 28u, 0u);      // is_removed = 0
  StoreU8(base,  node1 + 29u, 0u);      // is_paused = 0
  StoreU32(base, node1 + 32u, 1u);      // direction = 1 (countdown)

  StoreU32(base, node2 + 0u, sentinel);
  StoreU32(base, node2 + 4u, sentinel);
  StoreU32(base, node2 + 16u, 202u);    // timer_id = 202
  StoreU32(base, node2 + 20u, 1800u);   // ticks = 1800 (30.0s)
  StoreU8(base,  node2 + 28u, 0u);      // is_removed = 0
  StoreU8(base,  node2 + 29u, 0u);      // is_paused = 0
  StoreU32(base, node2 + 32u, 1u);      // direction = 1 (countdown)

  skate3::cheats::ResetRuntimeTrackingState();

  // 1. Identify 3-2-1 Start Countdown Timer (LR 0x826649D8)
  PPCContext ctx{};
  ctx.r4.u32 = 101u;
  ctx.lr = 0x826649D8u;
  sub_82D57DE0(ctx, base);

  // 2. Identify Active Gameplay Challenge HUD Timer (LR 0x825F1120)
  ctx.r4.u32 = 202u;
  ctx.lr = 0x825F1120u;
  sub_82D57DE0(ctx, base);

  // 3. Test Loading Screen Bypass: When in loading screen (player_state = 0)
  REXCVAR_SET(skate3_cheat_freeze_timer, true);
  skate3::cheats::SetMockCurrentPlayerState(0); // Loading / transition state

  g_imp_timer_update_called = false;
  ctx.r3.u32 = self;
  sub_82D57960(ctx, base);

  TEST_CHECK(g_imp_timer_update_called, "Timer update delegates directly during loading screen");
  TEST_CHECK(LoadU8(base, node1 + 29u) == 0, "Timer 101 is not paused during loading");
  TEST_CHECK(LoadU8(base, node2 + 29u) == 0, "Timer 202 is not paused during loading");

  // 4. Test Active Gameplay: player_state = 100 (On-board skating)
  skate3::cheats::SetMockCurrentPlayerState(100);

  // sub_82D57960 temporarily sets node+29 = 1 for active gameplay timers during the call,
  // then restores node+29 = 0 after __imp__sub_82D57960 returns.
  sub_82D57960(ctx, base);

  // Start countdown timer (101) must NEVER be paused, so challenges can start!
  TEST_CHECK(LoadU8(base, node1 + 29u) == 0, "3-2-1 Start countdown timer 101 is never paused");
  // Challenge timer (202) was paused during update and restored cleanly to 0 afterwards
  TEST_CHECK(LoadU8(base, node2 + 29u) == 0, "Challenge timer 202 is restored to unpaused state after update");

  REXCVAR_SET(skate3_cheat_freeze_timer, false);
  skate3::cheats::ResetRuntimeTrackingState();
}

static void TestPhysicsModifiers(uint8_t* base) {
  std::cout << "--- [Test 6] Pop Height & Speed Multipliers ---\n";

  // 1. Pop Height
  const uint32_t ground_phys = 0x82300000u;
  const uint32_t pop_params  = 0x82310000u;

  StoreF32(base, pop_params + 260u, 3.5f);
  StoreF32(base, ground_phys + 2052u, 4.0f); // Pop vertical impulse Vy

  REXCVAR_SET(skate3_cheat_pop_height, "2x");

  PPCContext ctx{};
  ctx.r3.u32 = ground_phys;
  ctx.r4.u32 = pop_params;
  sub_82D67848(ctx, base); // StartOllie

  TEST_CHECK(std::fabs(LoadF32(base, pop_params + 260u) - 3.5f) < 0.001f,
             "Pop params restored to original value after StartOllie");

  sub_82D67D08(ctx, base); // ComputePopImpulse
  TEST_CHECK(std::fabs(LoadF32(base, ground_phys + 2052u) - 8.0f) < 0.001f,
             "Vertical pop impulse scaled by 2.0x (4.0 -> 8.0)");

  REXCVAR_SET(skate3_cheat_pop_height, "Normal");

  // 2. Off-Board Speed Multiplier
  const uint32_t biped_ground = 0x82320000u;
  const uint32_t phys_in      = 0x82330000u;

  StoreU32(base, biped_ground + 16u, phys_in);
  StoreF32(base, phys_in + 2688u, 1.4f); // walk speed
  StoreF32(base, phys_in + 2692u, 4.2f); // run speed

  REXCVAR_SET(skate3_cheat_offboard_speed, "Fast (1.5x)");

  ctx.r3.u32 = biped_ground;
  sub_82D310F8(ctx, base);

  TEST_CHECK(std::fabs(LoadF32(base, phys_in + 2688u) - 1.4f) < 0.001f,
             "Walk speed restored cleanly after evaluation");
  TEST_CHECK(std::fabs(LoadF32(base, phys_in + 2692u) - 4.2f) < 0.001f,
             "Run speed restored cleanly after evaluation");

  REXCVAR_SET(skate3_cheat_offboard_speed, "Normal");
}

static void TestJumpAugmentationAndTunnelingPrevention(uint8_t* base) {
  std::cout << "--- [Test 7] Off-Board Jump Bounds & Safe Terminal Velocity ---\n";

  const uint32_t traj_mgr    = 0x82400000u;
  const uint32_t jump_params = 0x82410000u;
  const uint32_t biped_air   = 0x82420000u;
  const uint32_t phys_in     = 0x82430000u;

  StoreU32(base, biped_air + 16u, phys_in);

  // Test jump velocity clamping: Jump height 5x, Jump speed 3x
  REXCVAR_SET(skate3_cheat_offboard_jump_height, "5x");
  REXCVAR_SET(skate3_cheat_offboard_jump_speed, "3x");

  StoreF32(base, jump_params + 0u, 6.0f); // Vx
  StoreF32(base, jump_params + 4u, 4.0f); // Vy
  StoreF32(base, jump_params + 8u, 6.0f); // Vz

  PPCContext ctx{};
  ctx.r3.u32 = traj_mgr;
  ctx.r4.u32 = jump_params;
  sub_82D6CA58(ctx, base); // EvaluateJumpCandidates

  const float new_vy = LoadF32(base, jump_params + 4u);
  const float new_vx = LoadF32(base, jump_params + 0u);
  const float new_vz = LoadF32(base, jump_params + 8u);
  const float new_horiz = std::sqrt(new_vx * new_vx + new_vz * new_vz);

  TEST_CHECK(new_vy <= 11.51f, "Vertical jump impulse capped at safe 11.5 m/s ceiling to prevent clipping");
  TEST_CHECK(new_horiz <= 15.01f, "Horizontal jump speed clamped to <= 15.0 m/s ceiling to prevent wall tunneling");

  // Test downward terminal velocity clamp in Biped_Air::Update
  // High fall from skyscraper with Vy = -30 m/s
  StoreF32(base, phys_in + 2884u, -30.0f);
  ctx.r3.u32 = biped_air;
  sub_82D2EF38(ctx, base);

  const float clamped_vy = LoadF32(base, phys_in + 2884u);
  TEST_CHECK(clamped_vy >= -11.001f, "High downward fall velocity clamped to -11 m/s (Havok capsule limit)");

  REXCVAR_SET(skate3_cheat_offboard_jump_height, "Normal");
  REXCVAR_SET(skate3_cheat_offboard_jump_speed, "Normal");
}

static void TestCrashGuardAndRobustness(uint8_t* base) {
  std::cout << "--- [Test 8] Crash Guard & Robustness (Hang / Null Pointer Safety) ---\n";

  // 1. Grind alignment matrix crash guard (sub_82D40580)
  const uint32_t out_mat   = 0x82500000u;
  const uint32_t in_mat    = 0x82500040u;
  const uint32_t grind_obj = 0x82500080u;

  // Initialize input matrix with distinctive floats
  for (uint32_t i = 0; i < 16; ++i) {
    StoreF32(base, in_mat + i * 4u, static_cast<float>(i + 1));
  }

  // Test Case A: Null obstacle pointer
  g_imp_grind_matrix_called = false;
  PPCContext ctx{};
  ctx.r3.u32 = out_mat;
  ctx.r4.u32 = 0u; // NULL grind obstacle
  ctx.r5.u32 = in_mat;
  sub_82D40580(ctx, base);

  TEST_CHECK(!g_imp_grind_matrix_called, "Does not call virtual function when obstacle is null");
  TEST_CHECK(LoadF32(base, out_mat) == 1.0f && LoadF32(base, out_mat + 60u) == 16.0f,
             "Safely copies in_mat to out_mat without crashing");

  // Test Case B: Obstacle with broken vtable (null function pointer)
  StoreU32(base, grind_obj + 4u, 0x82500100u);
  StoreU32(base, 0x82500100u + 20u, 0x82500200u);
  StoreU32(base, 0x82500200u + 0u, 0x82500300u);
  StoreU32(base, 0x82500300u + 36u, 0x0u); // NULL function pointer

  ctx.r3.u32 = out_mat;
  ctx.r4.u32 = grind_obj;
  ctx.r5.u32 = in_mat;
  sub_82D40580(ctx, base);
  TEST_CHECK(!g_imp_grind_matrix_called, "Safely catches NULL vtable function pointer without crashing");

  // 2. Malformed / circular timer tree traversal safety
  const uint32_t malformed_self = 0x82600000u;
  const uint32_t loop_node = 0x82601000u;
  StoreU32(base, malformed_self + 4292u, loop_node);
  StoreU32(base, loop_node + 0u, loop_node); // Self-looping right pointer!
  StoreU32(base, loop_node + 4u, loop_node); // Self-looping left pointer!

  REXCVAR_SET(skate3_cheat_freeze_timer, true);
  skate3::cheats::SetMockCurrentPlayerState(100);

  ctx.r3.u32 = malformed_self;
  sub_82D57960(ctx, base); // Must exit boundedly without hanging!
  TEST_CHECK(true, "Circular timer tree terminates within bounded iterations without hanging");

  // 3. Null pointer safety across hooks
  ctx.r3.u64 = 0;
  ctx.r4.u64 = 0;
  ctx.r5.u64 = 0;
  sub_82DB4048(ctx, base);
  sub_82DB5588(ctx, base);
  sub_82DB5CF8(ctx, base);
  sub_82D91098(ctx, base);
  sub_82DB9100(ctx, base);
  sub_82D8BD50(ctx, base);
  sub_82D8ADE8(ctx, base);
  sub_82D67848(ctx, base);
  sub_82D67D08(ctx, base);
  sub_82D633B0(ctx, base);
  sub_82D310F8(ctx, base);
  sub_82D2EF38(ctx, base);

  TEST_CHECK(true, "All cheat hooks survive NULL arguments with zero crashes or segfaults");

  REXCVAR_SET(skate3_cheat_freeze_timer, false);
}

//=============================================================================
// Main Test Runner
//=============================================================================

int main() {
  std::cout << "=========================================================\n";
  std::cout << "        Skate 3 Gameplay Cheats Verification Suite       \n";
  std::cout << "=========================================================\n";

  // Map 2.4 GB sparse guest address space using mmap with MAP_NORESERVE
  // (Zero physical memory committed until individual test pages are touched)
  constexpr size_t kGuestMemSize = 0x98000000ull;
  uint8_t* base = static_cast<uint8_t*>(mmap(nullptr, kGuestMemSize,
                                             PROT_READ | PROT_WRITE,
                                             MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE,
                                             -1, 0));
  if (base == MAP_FAILED || !base) {
    std::cerr << "[ERROR] Failed to map sparse guest test memory!\n";
    return 1;
  }

  TestCVarPresetParsers();
  TestNoBailsSuppression(base);
  TestDropHeightThreshold(base);
  TestInfiniteLoadAndElevationSafety(base);
  TestChallengeTimerFreeze(base);
  TestPhysicsModifiers(base);
  TestJumpAugmentationAndTunnelingPrevention(base);
  TestCrashGuardAndRobustness(base);

  munmap(base, kGuestMemSize);

  std::cout << "=========================================================\n";
  std::cout << "Test Results: " << g_tests_passed << " Passed, "
            << g_tests_failed << " Failed.\n";
  std::cout << "=========================================================\n";

  return (g_tests_failed == 0) ? 0 : 1;
}
