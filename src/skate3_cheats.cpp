/**
 * @file        src/skate3_cheats.cpp
 * @brief       Runtime gameplay cheats for Skate 3 (live-toggled via Meta Menu)
 *
 * Implements live-adjustable cheats without requiring a game restart:
 *   - skate3_cheat_no_bails             (bool)   : Disable bails / wipeouts
 *   - skate3_cheat_freeze_timer         (bool)   : Freeze challenge countdown timers
 *   - skate3_cheat_gravity              (string) : Gravity preset ("Moon", "Low", "Normal", "High", "Super High")
 *   - skate3_cheat_pop_height           (string) : Ollie / pop height multiplier ("Normal", "1.5x", "2x", "3x", "5x")
 *   - skate3_cheat_offboard_jump_height (string) : Off-board jump height multiplier, independent of gravity
 *   - skate3_cheat_offboard_jump_speed  (string) : Off-board jump forward speed multiplier, independent of gravity
 *   - skate3_cheat_drop_height          (string) : Drop height bail threshold ("Normal", "2x", "5x", "10x", "Unlimited")
 *   - skate3_cheat_onboard_speed        (string) : On-board speed multiplier ("Slow (0.5x)", "Normal", "Fast (1.5x)", "2x", "3x")
 *   - skate3_cheat_offboard_speed       (string) : Off-board run/walk speed multiplier ("Slow (0.5x)", "Normal", "Fast (1.5x)", "2x", "3x")
 */

#include "skate3_cheats.h"
#include "skate3_achievements.h"
#include "generated/skate3_init.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

#include <rex/cvar.h>
#include <rex/logging.h>

//=============================================================================
// Cheat CVars (Live-updated from the Meta Menu Cheats tab)
//=============================================================================
REXCVAR_DEFINE_BOOL(skate3_cheat_no_bails, false, "Skate 3 Cheats",
                    "Prevent the skater from bailing on crashes, slams, or manual bail inputs.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(skate3_cheat_freeze_timer, false, "Skate 3 Cheats",
                    "Freeze challenge and callout countdown timers.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_STRING(skate3_cheat_gravity, "Normal", "Skate 3 Cheats",
                      "Gravity preset applied to skater and board physics.")
    .allowed({"Moon", "Low", "Normal", "High", "Super High"})
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_STRING(skate3_cheat_pop_height, "Normal", "Skate 3 Cheats",
                      "On-board ollie and nollie pop height multiplier.")
    .allowed({"Normal", "1.5x", "2x", "3x", "5x"})
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_STRING(skate3_cheat_offboard_jump_height, "Normal", "Skate 3 Cheats",
                      "Off-board jump height multiplier (independent of gravity).")
    .allowed({"Normal", "1.5x", "2x", "3x", "5x"})
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_STRING(skate3_cheat_offboard_jump_speed, "Normal", "Skate 3 Cheats",
                      "Off-board jump forward speed multiplier (independent of gravity).")
    .allowed({"Slow (0.5x)", "Normal", "Fast (1.5x)", "2x", "3x"})
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_STRING(skate3_cheat_drop_height, "Normal", "Skate 3 Cheats",
                      "Maximum drop height before bailing (on-board and off-board).")
    .allowed({"Normal", "2x", "5x", "10x", "Unlimited"})
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_STRING(skate3_cheat_onboard_speed, "Normal", "Skate 3 Cheats",
                      "On-board pushing and rolling speed multiplier.")
    .allowed({"Slow (0.5x)", "Normal", "Fast (1.5x)", "2x", "3x"})
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_STRING(skate3_cheat_offboard_speed, "Normal", "Skate 3 Cheats",
                      "Off-board walking, jogging, and sprinting speed multiplier.")
    .allowed({"Slow (0.5x)", "Normal", "Fast (1.5x)", "2x", "3x"})
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace {

//=============================================================================
// Guest Memory Helpers & Constants
//=============================================================================

constexpr uint32_t kGuestRamStart = 0x80000000u;
constexpr uint32_t kGuestRamEnd   = 0x90000000u;

constexpr float    kDefaultPlayerGravityY = -9.8f;
constexpr float    kDefaultWorldGravityY  = -9.81f;

// Normal max survivable drop height in Skate 3 is ~5.5 meters (~18 ft).
constexpr float kNormalSafeDropMeters = 5.5f;

// In Xbox 360 memory space, heap and stack allocations reside in 0x00010000..0x7FFFFFFF
// while module data resides in 0x82000000..0x8FFFFFFF.
inline bool IsValidGuestAddr(uint32_t addr, uint32_t size = 4) {
    return addr >= 0x10000u && addr <= 0xF0000000u && (addr + size) >= addr && (addr + size) <= 0xF0000000u;
}

inline bool PlausibleGuestAddr(uint32_t addr) {
    return addr >= 0x10000u && addr < 0xF0000000u;
}

inline uint8_t LoadU8(uint8_t* base, uint32_t addr) {
    return *(base + addr);
}

inline void StoreU8(uint8_t* base, uint32_t addr, uint8_t val) {
    *(base + addr) = val;
}

inline uint16_t LoadU16(uint8_t* base, uint32_t addr) {
    uint16_t raw;
    std::memcpy(&raw, base + addr, 2);
    return __builtin_bswap16(raw);
}

inline uint32_t LoadU32(uint8_t* base, uint32_t addr) {
    uint32_t raw;
    std::memcpy(&raw, base + addr, 4);
    return __builtin_bswap32(raw);
}

inline void StoreU32(uint8_t* base, uint32_t addr, uint32_t val) {
    const uint32_t raw = __builtin_bswap32(val);
    std::memcpy(base + addr, &raw, 4);
}

inline float LoadF32(uint8_t* base, uint32_t addr) {
    uint32_t raw = LoadU32(base, addr);
    float val;
    std::memcpy(&val, &raw, 4);
    return val;
}

inline void StoreF32(uint8_t* base, uint32_t addr, float val) {
    uint32_t raw;
    std::memcpy(&raw, &val, 4);
    StoreU32(base, addr, raw);
}

//=============================================================================
// CVar Preset Parsers
//=============================================================================

float GetGravityMultiplier() {
    const std::string& s = REXCVAR_GET(skate3_cheat_gravity);
    if (s == "Moon") return 0.25f;
    if (s == "Low") return 0.5f;
    if (s == "High") return 1.5f;
    if (s == "Super High") return 2.5f;
    return 1.0f;
}

float GetHeightPresetMultiplier(const std::string& s) {
    if (s == "1.5x") return 1.5f;
    if (s == "2x") return 2.0f;
    if (s == "3x") return 3.0f;
    if (s == "5x") return 5.0f;
    return 1.0f;
}

float GetPopHeightMultiplier() {
    return GetHeightPresetMultiplier(REXCVAR_GET(skate3_cheat_pop_height));
}

float GetOffBoardJumpHeightMultiplier() {
    return GetHeightPresetMultiplier(REXCVAR_GET(skate3_cheat_offboard_jump_height));
}

float GetSpeedMultiplier(const std::string& s) {
    if (s == "Slow (0.5x)") return 0.5f;
    if (s == "Fast (1.5x)") return 1.5f;
    if (s == "2x") return 2.0f;
    if (s == "3x") return 3.0f;
    return 1.0f;
}

float GetOffBoardJumpSpeedMultiplier() {
    return GetSpeedMultiplier(REXCVAR_GET(skate3_cheat_offboard_jump_speed));
}

float GetDropHeightMultiplier() {
    const std::string& s = REXCVAR_GET(skate3_cheat_drop_height);
    if (s == "2x") return 2.0f;
    if (s == "5x") return 5.0f;
    if (s == "10x") return 10.0f;
    if (s == "Unlimited") return 10000.0f;
    return 1.0f;
}

float GetOnBoardSpeedMultiplier() {
    return GetSpeedMultiplier(REXCVAR_GET(skate3_cheat_onboard_speed));
}

float GetOffBoardSpeedMultiplier() {
    return GetSpeedMultiplier(REXCVAR_GET(skate3_cheat_offboard_speed));
}

//=============================================================================
// Runtime Tracking State
//=============================================================================

static uint32_t g_local_player_ptr = 0;
static uint32_t g_local_skateboard_ptr = 0;
static int32_t  g_current_player_state = 0;

static bool  g_onboard_in_air = false;
static float g_onboard_peak_y = 0.0f;
static float g_onboard_drop_meters = 0.0f;
static int   g_onboard_landing_grace_frames = 0;

static bool  g_offboard_in_air = false;
static float g_offboard_peak_y = 0.0f;
static float g_offboard_drop_meters = 0.0f;
static int   g_offboard_landing_grace_frames = 0;

static bool  g_pending_custom_jump = false;
static float g_custom_jump_vx = 0.0f;
static float g_custom_jump_vy = 0.0f;
static float g_custom_jump_vz = 0.0f;

// Challenge timer tracking: distinguish active HUD/callout gameplay timers from
// the 3-2-1 start countdown (LR 0x826649D8 / 0x82664AE4) and internal script delay timers.
static uint32_t g_timer_frame_counter = 1;
static uint32_t g_start_countdown_timer_id = 0xFFFFFFFFu;

struct ActiveGameplayTimerEntry {
    uint32_t timer_id = 0xFFFFFFFFu;
    uint32_t last_seen_frame = 0;
};
static ActiveGameplayTimerEntry g_active_gameplay_timers[16] = {};

inline void RecordActiveGameplayTimer(uint32_t timer_id) {
    if (timer_id == 0xFFFFFFFFu || timer_id == g_start_countdown_timer_id) {
        return;
    }
    size_t oldest_idx = 0;
    uint32_t oldest_frame = 0xFFFFFFFFu;
    for (size_t i = 0; i < 16; ++i) {
        if (g_active_gameplay_timers[i].timer_id == timer_id) {
            g_active_gameplay_timers[i].last_seen_frame = g_timer_frame_counter;
            return;
        }
        if (g_active_gameplay_timers[i].last_seen_frame < oldest_frame) {
            oldest_frame = g_active_gameplay_timers[i].last_seen_frame;
            oldest_idx = i;
        }
    }
    g_active_gameplay_timers[oldest_idx].timer_id = timer_id;
    g_active_gameplay_timers[oldest_idx].last_seen_frame = g_timer_frame_counter;
}

inline bool IsActiveGameplayTimer(uint32_t timer_id) {
    if (timer_id == 0xFFFFFFFFu || timer_id == g_start_countdown_timer_id) {
        return false;
    }
    for (size_t i = 0; i < 16; ++i) {
        if (g_active_gameplay_timers[i].timer_id == timer_id &&
            (g_timer_frame_counter - g_active_gameplay_timers[i].last_seen_frame) <= 30u) {
            return true;
        }
    }
    return false;
}

inline bool IsOnBoardDropProtected() {
    if (REXCVAR_GET(skate3_cheat_no_bails)) {
        return true;
    }
    const float drop_mult = GetDropHeightMultiplier();
    if (drop_mult > 1.001f) {
        if (g_onboard_in_air || g_onboard_landing_grace_frames > 0) {
            return g_onboard_drop_meters <= (kNormalSafeDropMeters * drop_mult);
        }
    }
    return false;
}

inline bool IsOffBoardDropProtected() {
    if (REXCVAR_GET(skate3_cheat_no_bails)) {
        return true;
    }
    const float drop_mult = GetDropHeightMultiplier();
    const float jump_h = GetOffBoardJumpHeightMultiplier();
    const float jump_spd = GetOffBoardJumpSpeedMultiplier();
    if (drop_mult > 1.001f || jump_h > 1.001f || jump_spd > 1.001f) {
        if (g_offboard_in_air || g_offboard_landing_grace_frames > 0) {
            const float effective_mult = std::max({drop_mult, jump_h * 2.0f, jump_spd * 1.5f});
            return g_offboard_drop_meters <= (kNormalSafeDropMeters * effective_mult);
        }
    }
    return false;
}

inline void ApplyGravityCheat(uint8_t* base) {
    (void)base;
    // Note: Do NOT write to 0x830D0850! That address is an animation deltaTime divisor in sub_824C97B8.
    // Overwriting it caused character hair lagging/jumping and distorted bone transforms on vert ramps.
}

// Clear all 34 wipeout condition bytes in WipeoutConditionMgr (cond_mgr = *(player + 1816))
inline void ClearAllWipeoutConditions(uint8_t* base, uint32_t cond_mgr) {
    if (!IsValidGuestAddr(cond_mgr, 212)) {
        return;
    }
    std::memset(base + cond_mgr + 20u, 0, 34u);
    StoreU32(base, cond_mgr + 200u, 0u);
}

// Clear drop/landing-impact wipeout conditions in WipeoutConditionMgr while preserving
// horizontal wall/object crash conditions (offsets 23, 24, 32..37) when only drop_height is active.
inline void ClearDropHeightWipeoutConditions(uint8_t* base, uint32_t cond_mgr) {
    if (!IsValidGuestAddr(cond_mgr, 212)) {
        return;
    }
    static constexpr uint32_t kDropOffsets[] = {
        20, 21, 22, 25, 26, 27, 28, 31,
        38, 39, 40, 41, 42, 43, 46, 50, 51, 52, 53
    };
    for (uint32_t offset : kDropOffsets) {
        StoreU8(base, cond_mgr + offset, 0);
    }
}

// Clear air-flail / wipeout request bits in ProcessedPhysIn (phys_in = *(player + 1804))
inline void SanitizePhysInWipeoutBits(uint8_t* base, uint32_t phys_in, bool clear_onboard_bails, bool clear_offboard_bails) {
    if (!IsValidGuestAddr(phys_in, 2960)) {
        return;
    }
    if (clear_onboard_bails) {
        // Clear wipeout trigger bits in +2468 (0x2 = wipeout, 0x4 = bail, 0x40000 = forced bail)
        const uint32_t f2468 = LoadU32(base, phys_in + 2468u);
        StoreU32(base, phys_in + 2468u, f2468 & ~(0x2u | 0x4u | 0x40000u));

        // Clear air flail / panic bits in +2472 (0x40 = bail, 0x8000 = air flail mode 7, 0x40000 = drop flail)
        const uint32_t f2472 = LoadU32(base, phys_in + 2472u);
        StoreU32(base, phys_in + 2472u, f2472 & ~(0x40u | 0x8000u | 0x40000u));
    }
    if (clear_onboard_bails || clear_offboard_bails) {
        // Clear off-board air flail bit (0x2) and off-board bail bit (0x100000) in +2484
        const uint32_t f2484 = LoadU32(base, phys_in + 2484u);
        StoreU32(base, phys_in + 2484u, f2484 & ~(0x2u | 0x100000u));
    }
}

// Locate global skateboard tuning table from global pointer 0x830CFDA4 or ground_phys + 2212
inline uint32_t GetSkateboardTuningAddr(uint8_t* base, uint32_t ground_phys = 0) {
    if (IsValidGuestAddr(ground_phys, 2220)) {
        const uint32_t tuning = LoadU32(base, ground_phys + 2212u);
        if (IsValidGuestAddr(tuning, 600)) {
            return tuning;
        }
    }
    const uint32_t p1 = LoadU32(base, 0x830CFDA4u);
    if (IsValidGuestAddr(p1, 240)) {
        const uint32_t p2 = LoadU32(base, p1 + 232u);
        if (IsValidGuestAddr(p2, 16)) {
            const uint32_t tuning = LoadU32(base, p2 + 4u);
            if (IsValidGuestAddr(tuning, 600)) {
                return tuning;
            }
        }
    }
    return 0;
}

}  // namespace

//=============================================================================
// 1. Challenge Timer Freeze Hooks
//=============================================================================
//
// ChallengeTimer (self) stores all timers in an EASTL RB-tree rooted at self + 4292
// (with sentinel mAnchor at self + 4284, mpNodeRight at node + 0, mpNodeLeft at node + 4).
// This tree includes BOTH the 3-2-1 start countdown / script wait timers AND the active
// gameplay challenge/callout countdown timers.
// By hooking ChallengeTimer::GetRemainingSeconds (sub_82D57DE0), we identify which timer IDs
// are actively displayed by the gameplay challenge/callout HUD vs the 3-2-1 start countdown,
// so starting and completing challenges works seamlessly even with Freeze Timer enabled!

extern "C" void sub_82D57DE0(PPCContext& ctx, uint8_t* base) {
    const uint32_t timer_id = ctx.r4.u32;
    const uint32_t caller_lr = static_cast<uint32_t>(ctx.lr);

    // 0x826649D8 & 0x82664AE4 are the 3-2-1 Challenge Start Countdown HUD callers.
    if (caller_lr == 0x826649D8u || caller_lr == 0x82664AE4u) {
        g_start_countdown_timer_id = timer_id;
    }
    // Active gameplay HUD & callout timer callers:
    else if (caller_lr == 0x825F1120u || caller_lr == 0x82663998u ||
             caller_lr == 0x82668E00u || caller_lr == 0x827A3350u ||
             caller_lr == 0x8256042Cu || caller_lr == 0x825606E4u) {
        RecordActiveGameplayTimer(timer_id);
    }

    __imp__sub_82D57DE0(ctx, base);
}

// ChallengeTimer::Update (iterates the timer RB-tree and ticks countdown timers at node+20
// unless node+29 isPaused is non-zero).
extern "C" void sub_82D57960(PPCContext& ctx, uint8_t* base) {
    ++g_timer_frame_counter;

    if (!REXCVAR_GET(skate3_cheat_freeze_timer)) {
        __imp__sub_82D57960(ctx, base);
        return;
    }

    // Only freeze timer during active gameplay (state 100..599). Never during loading screens, menus, or cutscenes.
    if (g_current_player_state < 100 || g_current_player_state >= 600) {
        __imp__sub_82D57960(ctx, base);
        return;
    }

    const uint32_t self = ctx.r3.u32;
    uint32_t paused_nodes[64];
    size_t paused_count = 0;

    if (PlausibleGuestAddr(self)) {
        const uint32_t sentinel = self + 4284u;
        const uint32_t root = LoadU32(base, self + 4292u);
        if (PlausibleGuestAddr(root) && root != sentinel) {
            uint32_t stack[64];
            size_t sp = 0;
            stack[sp++] = root;
            size_t visited = 0;
            while (sp > 0 && visited < 256) {
                const uint32_t node = stack[--sp];
                ++visited;
                if (!PlausibleGuestAddr(node) || node == sentinel) {
                    continue;
                }
                const uint32_t timer_id   = LoadU32(base, node + 16u);
                const int32_t  ticks      = static_cast<int32_t>(LoadU32(base, node + 20u));
                const uint8_t  is_removed = LoadU8(base, node + 28u);
                const uint8_t  is_paused  = LoadU8(base, node + 29u);
                const uint32_t direction  = LoadU32(base, node + 32u);

                // Only freeze active gameplay countdown timers (direction == 1) that have > 0.5s
                // remaining and are not the 3-2-1 start countdown.
                if (is_removed == 0 && is_paused == 0 && direction == 1u &&
                    ticks > 30 && IsActiveGameplayTimer(timer_id) && paused_count < 64) {
                    StoreU8(base, node + 29u, 1u);
                    paused_nodes[paused_count++] = node;
                }

                const uint32_t right = LoadU32(base, node + 0u);
                const uint32_t left  = LoadU32(base, node + 4u);
                if (PlausibleGuestAddr(right) && right != sentinel && sp < 64) {
                    stack[sp++] = right;
                }
                if (PlausibleGuestAddr(left) && left != sentinel && sp < 64) {
                    stack[sp++] = left;
                }
            }
        }
    }

    __imp__sub_82D57960(ctx, base);

    for (size_t i = 0; i < paused_count; ++i) {
        StoreU8(base, paused_nodes[i] + 29u, 0u);
    }
}

//=============================================================================
// 2. PhysicalPlayer Master Frame Update, No Bails & Drop Height Hooks
//=============================================================================

// PhysicalPlayer::PreUpdate(player) — runs right before PhysicalPlayerStateChanger::GetNextState
extern "C" void sub_82DB4048(PPCContext& ctx, uint8_t* base) {
    ApplyGravityCheat(base);

    const uint32_t player = ctx.r3.u32;
    if (IsValidGuestAddr(player, 1936)) {
        const uint32_t player_skater = LoadU32(base, player + 1928u);
        const uint32_t local_skater  = skate3::GetLocalSkaterPtr();
        // If we know the local human skater and this PhysicalPlayer belongs to an AI skater,
        // do not overwrite g_local_player_ptr, g_current_player_state, or apply local cheats.
        if (local_skater != 0 && player_skater != 0 && player_skater != local_skater) {
            __imp__sub_82DB4048(ctx, base);
            return;
        }

        g_local_player_ptr = player;
        const uint32_t skateboard = LoadU32(base, player + 1880u);
        if (IsValidGuestAddr(skateboard, 64)) {
            g_local_skateboard_ptr = skateboard;
        }

        const uint32_t phys_in  = LoadU32(base, player + 1804u);
        const uint32_t cond_mgr = LoadU32(base, player + 1816u);
        if (IsValidGuestAddr(phys_in, 2960)) {
            const int32_t state = static_cast<int32_t>(LoadU32(base, phys_in + 2500u));
            g_current_player_state = state;

            // Read current player XYZ position from ProcessedPhysIn (+192 is transform, +240 is pos.xyz)
            const float player_x = LoadF32(base, phys_in + 240u);
            const float player_y = LoadF32(base, phys_in + 244u);
            const float player_z = LoadF32(base, phys_in + 248u);
            if (local_skater != 0 && player_skater == local_skater) {
                skate3::SetLocalSkaterPhysicsPosition(player_x, player_y, player_z);
            }

            // Track on-board air drop distance (states 200..299 air, 400..403 jump tricks)
            const bool onboard_air = (state >= 200 && state < 300) || (state >= 400 && state <= 403);
            if (onboard_air) {
                if (!g_onboard_in_air) {
                    g_onboard_in_air = true;
                    g_onboard_peak_y = player_y;
                    g_onboard_drop_meters = 0.0f;
                } else {
                    if (player_y > g_onboard_peak_y) {
                        g_onboard_peak_y = player_y;
                    }
                    g_onboard_drop_meters = std::max(0.0f, g_onboard_peak_y - player_y);
                }
                g_onboard_landing_grace_frames = 20;
            } else {
                g_onboard_in_air = false;
                if (g_onboard_landing_grace_frames > 0) {
                    --g_onboard_landing_grace_frames;
                } else {
                    g_onboard_drop_meters = 0.0f;
                }
            }

            // Track off-board air drop distance
            const bool offboard_air = (state == 501);
            if (offboard_air) {
                if (!g_offboard_in_air) {
                    g_offboard_in_air = true;
                    g_offboard_peak_y = player_y;
                    g_offboard_drop_meters = 0.0f;
                } else {
                    if (player_y > g_offboard_peak_y) {
                        g_offboard_peak_y = player_y;
                    }
                    g_offboard_drop_meters = std::max(0.0f, g_offboard_peak_y - player_y);
                }
                g_offboard_landing_grace_frames = 20;
            } else {
                g_offboard_in_air = false;
                if (g_offboard_landing_grace_frames > 0) {
                    --g_offboard_landing_grace_frames;
                } else {
                    g_offboard_drop_meters = 0.0f;
                }
            }

            // Apply real-time gravity delta to on-board airborne physics
            const float grav_mult = GetGravityMultiplier();
            if (std::fabs(grav_mult - 1.0f) > 0.001f && g_onboard_in_air) {
                const float dt = 1.0f / 60.0f;
                const float delta_vy = (1.0f - grav_mult) * 9.80665f * dt;
                const float cur_pvy = LoadF32(base, phys_in + 2884u);
                StoreF32(base, phys_in + 2884u, cur_pvy + delta_vy);

                if (IsValidGuestAddr(g_local_skateboard_ptr, 310)) {
                    const uint32_t sb_comp = LoadU32(base, g_local_skateboard_ptr + 304u);
                    if (IsValidGuestAddr(sb_comp, 600)) {
                        const float cur_sb_vy = LoadF32(base, sb_comp + 548u);
                        StoreF32(base, sb_comp + 548u, cur_sb_vy + delta_vy);
                    }
                }
            }

            // On-board speed multiplier (boost push acceleration in state 101)
            const float onboard_spd_mult = GetOnBoardSpeedMultiplier();
            if (std::fabs(onboard_spd_mult - 1.0f) > 0.001f && state == 101) {
                if (IsValidGuestAddr(g_local_skateboard_ptr, 310)) {
                    const uint32_t sb_comp = LoadU32(base, g_local_skateboard_ptr + 304u);
                    if (IsValidGuestAddr(sb_comp, 2800)) {
                        const float fx = LoadF32(base, sb_comp + 464u);
                        const float fz = LoadF32(base, sb_comp + 472u);
                        const float flen = std::sqrt(fx * fx + fz * fz);
                        if (flen > 0.01f) {
                            const float push_accel = (onboard_spd_mult - 1.0f) * 0.25f;
                            const float cur_vx = LoadF32(base, sb_comp + 544u);
                            const float cur_vz = LoadF32(base, sb_comp + 552u);
                            const float new_vx = cur_vx + (fx / flen) * push_accel;
                            const float new_vz = cur_vz + (fz / flen) * push_accel;
                            StoreF32(base, sb_comp + 544u, new_vx);
                            StoreF32(base, sb_comp + 552u, new_vz);
                            StoreF32(base, sb_comp + 2752u, std::sqrt(new_vx * new_vx + new_vz * new_vz));
                        }
                    }
                }
            }

            // Off-board speed multiplier (walk / run acceleration in state 500)
            const float offboard_spd_mult = GetOffBoardSpeedMultiplier();
            if (std::fabs(offboard_spd_mult - 1.0f) > 0.001f && state == 500) {
                const float vx = LoadF32(base, phys_in + 2880u);
                const float vz = LoadF32(base, phys_in + 2888u);
                const float cur_spd = std::sqrt(vx * vx + vz * vz);
                if (cur_spd > 0.2f) {
                    const float blend = 0.15f;
                    const float target_vx = vx * offboard_spd_mult;
                    const float target_vz = vz * offboard_spd_mult;
                    StoreF32(base, phys_in + 2880u, vx + (target_vx - vx) * blend);
                    StoreF32(base, phys_in + 2888u, vz + (target_vz - vz) * blend);
                }
            }

            const bool no_bails         = REXCVAR_GET(skate3_cheat_no_bails);
            const bool protect_onboard  = IsOnBoardDropProtected();
            const bool protect_offboard = IsOffBoardDropProtected();

            if (no_bails) {
                ClearAllWipeoutConditions(base, cond_mgr);
            } else if (protect_onboard || protect_offboard) {
                ClearDropHeightWipeoutConditions(base, cond_mgr);
            }
            SanitizePhysInWipeoutBits(base, phys_in, protect_onboard, protect_offboard);
        }
    }

    __imp__sub_82DB4048(ctx, base);
}

// PhysicalPlayer::Update(player)
extern "C" void sub_82DB5588(PPCContext& ctx, uint8_t* base) {
    ApplyGravityCheat(base);

    const uint32_t player = ctx.r3.u32;
    if (IsValidGuestAddr(player, 1920)) {
        const uint32_t phys_in  = LoadU32(base, player + 1804u);
        const uint32_t cond_mgr = LoadU32(base, player + 1816u);
        const bool no_bails         = REXCVAR_GET(skate3_cheat_no_bails);
        const bool protect_onboard  = IsOnBoardDropProtected();
        const bool protect_offboard = IsOffBoardDropProtected();

        if (no_bails) {
            ClearAllWipeoutConditions(base, cond_mgr);
        } else if (protect_onboard || protect_offboard) {
            ClearDropHeightWipeoutConditions(base, cond_mgr);
        }
        SanitizePhysInWipeoutBits(base, phys_in, protect_onboard, protect_offboard);
    }

    __imp__sub_82DB5588(ctx, base);
}

// PhysicalPlayer::SyncWipeoutFlags(player)
extern "C" void sub_82DB5CF8(PPCContext& ctx, uint8_t* base) {
    const uint32_t player = ctx.r3.u32;
    __imp__sub_82DB5CF8(ctx, base);

    if (!IsValidGuestAddr(player, 1920)) {
        return;
    }

    const uint32_t phys_in  = LoadU32(base, player + 1804u);
    const uint32_t cond_mgr = LoadU32(base, player + 1816u);

    const bool no_bails         = REXCVAR_GET(skate3_cheat_no_bails);
    const bool protect_onboard  = IsOnBoardDropProtected();
    const bool protect_offboard = IsOffBoardDropProtected();

    if (no_bails) {
        ClearAllWipeoutConditions(base, cond_mgr);
        SanitizePhysInWipeoutBits(base, phys_in, true, true);
    } else if (protect_onboard || protect_offboard) {
        ClearDropHeightWipeoutConditions(base, cond_mgr);
        SanitizePhysInWipeoutBits(base, phys_in, protect_onboard, protect_offboard);
    }
}

// Physics::Wipeout::UpdateAndCheck - evaluates all 34 bail reason flags
// (this+20..this+53) and returns true when a bail should trigger.
extern "C" void sub_82D91098(PPCContext& ctx, uint8_t* base) {
    const uint32_t wipeout = ctx.r3.u32;
    if (REXCVAR_GET(skate3_cheat_no_bails)) {
        ClearAllWipeoutConditions(base, wipeout);
        ctx.r3.u64 = 0;
        return;
    }
    if (IsOnBoardDropProtected() || IsOffBoardDropProtected()) {
        ClearDropHeightWipeoutConditions(base, wipeout);
    }
    __imp__sub_82D91098(ctx, base);
}

// SkaterPhysics::ShouldBail - secondary check for pending wipeout conditions.
extern "C" void sub_82DB9100(PPCContext& ctx, uint8_t* base) {
    if (REXCVAR_GET(skate3_cheat_no_bails) || IsOnBoardDropProtected() || IsOffBoardDropProtected()) {
        ctx.r3.u64 = 0;
        return;
    }
    __imp__sub_82DB9100(ctx, base);
}

// PhysicalPlayerStateChanger::CheckForceBail - checks force-bail triggers.
extern "C" void sub_82D8BD50(PPCContext& ctx, uint8_t* base) {
    if (REXCVAR_GET(skate3_cheat_no_bails) || IsOnBoardDropProtected() || IsOffBoardDropProtected()) {
        ctx.r3.u64 = 0;
        return;
    }
    __imp__sub_82D8BD50(ctx, base);
}

// Wipeout::CheckVerticalDropVelocity (sets wipeout+20, +22)
extern "C" void sub_82D90AB8(PPCContext& ctx, uint8_t* base) {
    const uint32_t wipeout = ctx.r3.u32;
    __imp__sub_82D90AB8(ctx, base);

    if (REXCVAR_GET(skate3_cheat_no_bails) || IsOnBoardDropProtected() || IsOffBoardDropProtected()) {
        if (IsValidGuestAddr(wipeout, 212)) {
            StoreU8(base, wipeout + 20u, 0u);
            StoreU8(base, wipeout + 22u, 0u);
        }
    }
}

// Wipeout::CheckAirDropHeight (sets wipeout+27)
extern "C" void sub_82D90C98(PPCContext& ctx, uint8_t* base) {
    if (REXCVAR_GET(skate3_cheat_no_bails) || IsOnBoardDropProtected() || IsOffBoardDropProtected()) {
        return;
    }
    __imp__sub_82D90C98(ctx, base);
}

// Wipeout::CheckLandingImpulseAccumulator (sets wipeout+23, +31)
extern "C" void sub_82D907B0(PPCContext& ctx, uint8_t* base) {
    const uint32_t wipeout = ctx.r3.u32;
    __imp__sub_82D907B0(ctx, base);

    if (REXCVAR_GET(skate3_cheat_no_bails) || IsOnBoardDropProtected() || IsOffBoardDropProtected()) {
        if (IsValidGuestAddr(wipeout, 212)) {
            StoreU8(base, wipeout + 23u, 0u);
            StoreU8(base, wipeout + 31u, 0u);
            StoreF32(base, wipeout + 204u, 0.0f);
        }
    }
}

// Wipeout::CheckAirToGroundTransitionVelocity (sets wipeout+25, +26, +28)
// Computes ground/ramp contact alignment projection. MUST NOT be bypassed on vert ramps!
extern "C" void sub_82D90D08(PPCContext& ctx, uint8_t* base) {
    const uint32_t wipeout = ctx.r3.u32;
    __imp__sub_82D90D08(ctx, base);

    if (REXCVAR_GET(skate3_cheat_no_bails) || IsOnBoardDropProtected() || IsOffBoardDropProtected()) {
        if (IsValidGuestAddr(wipeout, 212)) {
            StoreU8(base, wipeout + 25u, 0u);
            StoreU8(base, wipeout + 26u, 0u);
            StoreU8(base, wipeout + 28u, 0u);
        }
    }
}

// Wipeout::CheckGround (sets wipeout+20, +21, +38)
extern "C" void sub_82D8F9E0(PPCContext& ctx, uint8_t* base) {
    const uint32_t wipeout = ctx.r3.u32;
    if (REXCVAR_GET(skate3_cheat_no_bails)) {
        ClearAllWipeoutConditions(base, wipeout);
        return;
    }
    __imp__sub_82D8F9E0(ctx, base);
    if (IsOnBoardDropProtected() || IsOffBoardDropProtected()) {
        ClearDropHeightWipeoutConditions(base, wipeout);
    }
}

// Wipeout::CheckAir
extern "C" void sub_82D90358(PPCContext& ctx, uint8_t* base) {
    const uint32_t wipeout = ctx.r3.u32;
    if (REXCVAR_GET(skate3_cheat_no_bails)) {
        ClearAllWipeoutConditions(base, wipeout);
        return;
    }
    __imp__sub_82D90358(ctx, base);
    if (IsOnBoardDropProtected() || IsOffBoardDropProtected()) {
        ClearDropHeightWipeoutConditions(base, wipeout);
    }
}

// PhysicsRigModeCtrl::SetMode(rig_ctrl, mode)
// Mode 7 is the looping air-flail / panic animation! Suppress it when bails off or drop height protected.
extern "C" void sub_82D911A8(PPCContext& ctx, uint8_t* base) {
    const uint32_t rig_ctrl = ctx.r3.u32;
    const uint32_t requested_mode = ctx.r4.u32;
    const bool jump_augmented = (GetOffBoardJumpHeightMultiplier() > 1.001f || GetOffBoardJumpSpeedMultiplier() > 1.001f);

    if (requested_mode == 7u && (REXCVAR_GET(skate3_cheat_no_bails) || IsOnBoardDropProtected() || IsOffBoardDropProtected() || jump_augmented)) {
        uint32_t safe_mode = (g_current_player_state >= 500) ? 4u : 6u;
        if (IsValidGuestAddr(rig_ctrl, 32)) {
            const uint8_t cur_mode = LoadU8(base, rig_ctrl + 16u);
            if (cur_mode == 4u || cur_mode == 6u) {
                safe_mode = cur_mode;
            }
        }
        ctx.r4.u64 = safe_mode;
    }

    __imp__sub_82D911A8(ctx, base);
}

// PhysicalPlayerStateChanger::GetNextState(state_changer)
// State families:
//   100..105: On-board ground
//   200..202: On-board air (200 = normal air, 202 = wipeout air flail)
//   300..301: Wipeout / ragdoll bail
//   400..405: Ollie / Trick / Grind / Manual
//   500..505: Off-board (500 = Biped_Ground, 501 = Biped_Air)
extern "C" void sub_82D8ADE8(PPCContext& ctx, uint8_t* base) {
    const uint32_t state_changer = ctx.r3.u32;
    uint32_t phys_in = 0;
    uint32_t cond_mgr = 0;
    int32_t cur_state = 100;

    if (IsValidGuestAddr(state_changer, 24)) {
        phys_in  = LoadU32(base, state_changer + 4u);
        cond_mgr = LoadU32(base, state_changer + 8u);
        if (IsValidGuestAddr(phys_in, 2960)) {
            cur_state = static_cast<int32_t>(LoadU32(base, phys_in + 2500u));
        }
    }

    const bool no_bails         = REXCVAR_GET(skate3_cheat_no_bails);
    const bool protect_onboard  = IsOnBoardDropProtected();
    const bool protect_offboard = IsOffBoardDropProtected();

    if (no_bails) {
        ClearAllWipeoutConditions(base, cond_mgr);
        SanitizePhysInWipeoutBits(base, phys_in, true, true);
    } else if (protect_onboard || protect_offboard) {
        ClearDropHeightWipeoutConditions(base, cond_mgr);
        SanitizePhysInWipeoutBits(base, phys_in, protect_onboard, protect_offboard);
    }

    __imp__sub_82D8ADE8(ctx, base);

    const int32_t next_state = static_cast<int32_t>(ctx.r3.u32);

    // Block transitions into Wipeout (300/301) or WipeoutAir (202)
    if (next_state == 300 || next_state == 301 || next_state == 202) {
        if (cur_state >= 500) {
            if (no_bails || protect_offboard) {
                ctx.r3.u64 = 500u;
            }
        } else {
            if (no_bails || protect_onboard) {
                if (next_state == 202 || (cur_state >= 200 && cur_state < 300)) {
                    ctx.r3.u64 = 200u;
                } else if (cur_state >= 400 && cur_state < 500) {
                    ctx.r3.u64 = static_cast<uint64_t>(cur_state);
                } else {
                    ctx.r3.u64 = 100u;
                }
            }
        }
    }
}

// Grind/Slide Alignment Adjustment (sub_82D40580)
// When grinding or sliding on rails/edges, sub_82D40890 calls sub_82D40580 to adjust
// the contact transform matrix via an indirect virtual call on the obstacle object:
//   r28 = *( *(r4 + 4) + 20 )
//   r10 = *(r28 + 0)
//   r9  = *(r10 + 36)
// If the obstacle or edge does not implement this interface (r9 == 0 or invalid address),
// the indirect call triggers InvalidFunctionTrap and crashes.
// When r9 is invalid, we safely bypass the virtual call and copy in_mat (r5) to out_mat (r3).
extern "C" void sub_82D40580(PPCContext& ctx, uint8_t* base) {
    const uint32_t out_mat   = ctx.r3.u32;
    const uint32_t grind_obj = ctx.r4.u32;
    const uint32_t in_mat    = ctx.r5.u32;

    bool can_call = false;
    if (IsValidGuestAddr(grind_obj, 8)) {
        const uint32_t r11 = LoadU32(base, grind_obj + 4u);
        if (IsValidGuestAddr(r11, 24)) {
            const uint32_t r28 = LoadU32(base, r11 + 20u);
            if (IsValidGuestAddr(r28, 4)) {
                const uint32_t r10 = LoadU32(base, r28 + 0u);
                if (IsValidGuestAddr(r10, 40)) {
                    const uint32_t r9 = LoadU32(base, r10 + 36u);
                    if (r9 >= REX_CODE_BASE && r9 < (REX_CODE_BASE + REX_CODE_SIZE)) {
                        can_call = true;
                    }
                }
            }
        }
    }

    if (can_call) {
        __imp__sub_82D40580(ctx, base);
    } else {
        if (IsValidGuestAddr(out_mat, 64) && IsValidGuestAddr(in_mat, 64)) {
            std::memcpy(base + out_mat, base + in_mat, 64);
        }
        ctx.r3.u64 = out_mat;
    }
}

//=============================================================================
// 4. On-Board Pop Height Hooks (SkateboardAirTrajMgr & SkateboardGroundPhysics::Pop)
//=============================================================================
//
// How on-board ollie / nollie pop velocity is computed:
//   1. sub_82D67848 (SkateboardAirTrajMgr::StartOllie): copies pop_params (+256..+264) into
//      ground_phys (+2176..+2184), then calls sub_82D67D08 and sub_82D682E8.
//   2. sub_82D67D08 (SkateboardAirTrajMgr::ComputePopImpulse): computes the physical board pop
//      impulse vector at ground_phys + 2048 (vertical Y at +2052).
//   3. sub_82D67320 (SkateboardAirTrajMgr::BuildOllieCandidateVelocities): multiplies pop power
//      at ground_phys + 2180, clamps it to [tuning + 440, tuning + 444], and writes N candidate
//      launch velocity vectors at ground_phys + 2192 + i * 64.
//   4. sub_82D68698 (SkateboardAirTrajMgr::ComputeFlightTime): computes trajectory flight time
//      clamped to *(float*)(tuning + 428).
//   5. sub_82D68800 (OllieMgr::Update) & sub_82D613B0 (SkateboardGroundPhysics::Pop): read
//      *(float*)(tuning + 564) and *(float*)(tuning + 428).

// SkateboardAirTrajMgr::StartOllie(ground_phys, pop_params)
extern "C" void sub_82D67848(PPCContext& ctx, uint8_t* base) {
    ApplyGravityCheat(base);

    const uint32_t pop_params = ctx.r4.u32;
    const float pop_mult = GetPopHeightMultiplier();
    float orig_param_pop = 0.0f;
    bool patched = false;

    if (std::fabs(pop_mult - 1.0f) > 0.001f && IsValidGuestAddr(pop_params, 272)) {
        orig_param_pop = LoadF32(base, pop_params + 260u);
        StoreF32(base, pop_params + 260u, orig_param_pop * pop_mult);
        patched = true;
    }

    __imp__sub_82D67848(ctx, base);

    if (patched) {
        StoreF32(base, pop_params + 260u, orig_param_pop);
    }
}

// SkateboardAirTrajMgr::ComputePopImpulse(ground_phys)
extern "C" void sub_82D67D08(PPCContext& ctx, uint8_t* base) {
    const uint32_t ground_phys = ctx.r3.u32;
    __imp__sub_82D67D08(ctx, base);

    const float pop_mult = GetPopHeightMultiplier();
    if (std::fabs(pop_mult - 1.0f) > 0.001f && IsValidGuestAddr(ground_phys, 2100)) {
        const float vy = LoadF32(base, ground_phys + 2052u);
        if (vy > 0.0f) {
            StoreF32(base, ground_phys + 2052u, vy * pop_mult);
        }
    }
}

// SkateboardAirTrajMgr::BuildOllieCandidateVelocities(ground_phys)
extern "C" void sub_82D67320(PPCContext& ctx, uint8_t* base) {
    const uint32_t ground_phys = ctx.r3.u32;
    const float pop_mult = GetPopHeightMultiplier();
    const uint32_t tuning = GetSkateboardTuningAddr(base, ground_phys);

    float orig_min_pop = 0.0f;
    float orig_max_pop = 0.0f;
    float orig_gp_pop  = 0.0f;
    bool patched_tuning = false;
    bool patched_gp     = false;

    if (std::fabs(pop_mult - 1.0f) > 0.001f) {
        if (tuning != 0) {
            orig_min_pop = LoadF32(base, tuning + 440u);
            orig_max_pop = LoadF32(base, tuning + 444u);
            StoreF32(base, tuning + 440u, orig_min_pop * pop_mult);
            StoreF32(base, tuning + 444u, orig_max_pop * pop_mult);
            patched_tuning = true;
        }
        if (IsValidGuestAddr(ground_phys, 2200)) {
            orig_gp_pop = LoadF32(base, ground_phys + 2180u);
            StoreF32(base, ground_phys + 2180u, orig_gp_pop * pop_mult);
            patched_gp = true;
        }
    }

    __imp__sub_82D67320(ctx, base);

    if (patched_tuning) {
        StoreF32(base, tuning + 440u, orig_min_pop);
        StoreF32(base, tuning + 444u, orig_max_pop);
    }
    if (patched_gp) {
        StoreF32(base, ground_phys + 2180u, orig_gp_pop);
    }
}

// SkateboardAirTrajMgr::ComputeFlightTime(vel_ptr)
extern "C" void sub_82D68698(PPCContext& ctx, uint8_t* base) {
    const float pop_mult  = GetPopHeightMultiplier();
    const float grav_mult = GetGravityMultiplier();
    const float time_scale = std::max(1.0f, pop_mult / std::sqrt( std::max(0.1f, grav_mult) ));
    const uint32_t tuning = GetSkateboardTuningAddr(base);

    float orig_max_time = 0.0f;
    bool patched = false;
    if (tuning != 0 && time_scale > 1.001f) {
        orig_max_time = LoadF32(base, tuning + 428u);
        StoreF32(base, tuning + 428u, orig_max_time * time_scale);
        patched = true;
    }

    __imp__sub_82D68698(ctx, base);

    if (patched) {
        StoreF32(base, tuning + 428u, orig_max_time);
    }

    if (grav_mult < 0.999f) {
        ctx.f1.f64 = ctx.f1.f64 / std::sqrt(std::max(0.1f, grav_mult));
    }
}

// OllieMgr::Update(ground_phys)
extern "C" void sub_82D68800(PPCContext& ctx, uint8_t* base) {
    const uint32_t ground_phys = ctx.r3.u32;
    ApplyGravityCheat(base);

    const float pop_mult = GetPopHeightMultiplier();
    const uint32_t tuning = GetSkateboardTuningAddr(base, ground_phys);

    float orig_pop_vel = 0.0f;
    float orig_pop_max = 0.0f;
    float orig_min_clamp = 0.0f;
    float orig_max_clamp = 0.0f;
    bool patched = false;

    if (tuning != 0 && std::fabs(pop_mult - 1.0f) > 0.001f) {
        orig_pop_vel   = LoadF32(base, tuning + 564u);
        orig_pop_max   = LoadF32(base, tuning + 428u);
        orig_min_clamp = LoadF32(base, tuning + 440u);
        orig_max_clamp = LoadF32(base, tuning + 444u);
        StoreF32(base, tuning + 564u, orig_pop_vel * pop_mult);
        StoreF32(base, tuning + 428u, orig_pop_max * pop_mult);
        StoreF32(base, tuning + 440u, orig_min_clamp * pop_mult);
        StoreF32(base, tuning + 444u, orig_max_clamp * pop_mult);
        patched = true;
    }

    __imp__sub_82D68800(ctx, base);

    if (patched) {
        StoreF32(base, tuning + 564u, orig_pop_vel);
        StoreF32(base, tuning + 428u, orig_pop_max);
        StoreF32(base, tuning + 440u, orig_min_clamp);
        StoreF32(base, tuning + 444u, orig_max_clamp);
    }
}

// OllieMgr::StartOllieTrajectory(ground_phys)
extern "C" void sub_82D682E8(PPCContext& ctx, uint8_t* base) {
    ApplyGravityCheat(base);
    __imp__sub_82D682E8(ctx, base);
}

// SkateboardGroundPhysics::Pop(ground_phys)
extern "C" void sub_82D613B0(PPCContext& ctx, uint8_t* base) {
    const uint32_t ground_phys = ctx.r3.u32;
    const float pop_mult = GetPopHeightMultiplier();
    const uint32_t tuning = GetSkateboardTuningAddr(base, ground_phys);

    float orig_pop_vel = 0.0f;
    float orig_pop_max = 0.0f;
    bool patched = false;

    if (tuning != 0 && std::fabs(pop_mult - 1.0f) > 0.001f) {
        orig_pop_vel = LoadF32(base, tuning + 564u);
        orig_pop_max = LoadF32(base, tuning + 428u);
        StoreF32(base, tuning + 564u, orig_pop_vel * pop_mult);
        StoreF32(base, tuning + 428u, orig_pop_max * pop_mult);
        patched = true;
    }

    __imp__sub_82D613B0(ctx, base);

    if (patched) {
        StoreF32(base, tuning + 564u, orig_pop_vel);
        StoreF32(base, tuning + 428u, orig_pop_max);
    }
}

//=============================================================================
// 5. On-Board Speed Hook (SkateboardGroundPhysics::FinishJob / sub_82D633B0)
//=============================================================================
//
// In PhysState_PhysicsGround::FinishJob (sub_82D37F38), sub_82D633B0(ground_phys, skateboard_ctrl)
// evaluates pushing impulse, top speed cap, and rolling friction from SkateboardTuning:
//   - tuning + 520: push force impulse
//   - tuning + 528: push max speed
//   - tuning + 556: rolling friction / drag
//   - tuning + 560: max push velocity
// Modifying these parameters allows the Havok vehicle integrator to naturally accelerate the board
// along the exact surface normal (flat, bank, or vert ramp) without artificial coordinate misalignment!

extern "C" void sub_82D633B0(PPCContext& ctx, uint8_t* base) {
    const uint32_t ground_phys = ctx.r3.u32;
    const float speed_mult = GetOnBoardSpeedMultiplier();

    if (!IsValidGuestAddr(ground_phys, 3000) || std::fabs(speed_mult - 1.0f) <= 0.001f) {
        __imp__sub_82D633B0(ctx, base);
        return;
    }

    const uint32_t tuning = GetSkateboardTuningAddr(base, ground_phys);
    float orig_force = 0.0f;
    float orig_speed = 0.0f;
    float orig_max   = 0.0f;
    float orig_drag  = 0.0f;
    bool patched = false;

    if (tuning != 0) {
        orig_force = LoadF32(base, tuning + 520u);
        orig_speed = LoadF32(base, tuning + 528u);
        orig_max   = LoadF32(base, tuning + 560u);
        orig_drag  = LoadF32(base, tuning + 556u);

        StoreF32(base, tuning + 520u, orig_force * speed_mult);
        StoreF32(base, tuning + 528u, orig_speed * speed_mult);
        StoreF32(base, tuning + 560u, orig_max * speed_mult);
        if (speed_mult > 1.0f) {
            StoreF32(base, tuning + 556u, orig_drag / speed_mult);
        } else {
            StoreF32(base, tuning + 556u, orig_drag * (1.0f / std::max(0.2f, speed_mult)));
        }
        patched = true;
    }

    __imp__sub_82D633B0(ctx, base);

    if (patched) {
        StoreF32(base, tuning + 520u, orig_force);
        StoreF32(base, tuning + 528u, orig_speed);
        StoreF32(base, tuning + 560u, orig_max);
        StoreF32(base, tuning + 556u, orig_drag);
    }
}

//=============================================================================
// 6. Off-Board Run/Walk Speed Hooks (PhysState_Biped_Ground)
//=============================================================================
//
// Off-board ground movement uses root-motion trajectory blending in BipedGroundTrajMgr.
// Scaling target walk speed (phys_in + 2688) and run speed (phys_in + 2692) in sub_82D310F8
// allows the character controller and animation tree to execute the higher speed naturally!

extern "C" void sub_82D310F8(PPCContext& ctx, uint8_t* base) {
    const uint32_t biped_ground = ctx.r3.u32;
    const float mult = GetOffBoardSpeedMultiplier();

    float orig_walk = 0.0f;
    float orig_run  = 0.0f;
    bool patched = false;
    uint32_t phys_in = 0;

    if (std::fabs(mult - 1.0f) > 0.001f && IsValidGuestAddr(biped_ground, 32)) {
        phys_in = LoadU32(base, biped_ground + 16u);
        if (IsValidGuestAddr(phys_in, 2700)) {
            orig_walk = LoadF32(base, phys_in + 2688u);
            orig_run  = LoadF32(base, phys_in + 2692u);
            StoreF32(base, phys_in + 2688u, orig_walk * mult);
            StoreF32(base, phys_in + 2692u, orig_run * mult);
            patched = true;
        }
    }

    __imp__sub_82D310F8(ctx, base);

    if (patched) {
        StoreF32(base, phys_in + 2688u, orig_walk);
        StoreF32(base, phys_in + 2692u, orig_run);
    }
}

extern "C" void sub_82D32338(PPCContext& ctx, uint8_t* base) {
    __imp__sub_82D32338(ctx, base);
}

//=============================================================================
// 7. Off-Board Jump Height, Jump Speed & High-Drop Landing Hooks (Biped_Air)
//=============================================================================

// BipedAirTrajMgr::EvaluateJumpCandidates(traj_mgr, jump_params)
extern "C" void sub_82D6CA58(PPCContext& ctx, uint8_t* base) {
    const uint32_t traj_mgr    = ctx.r3.u32;
    const uint32_t jump_params = ctx.r4.u32;

    const float jump_h   = GetOffBoardJumpHeightMultiplier();
    const float jump_spd = GetOffBoardJumpSpeedMultiplier();
    const bool  custom_jump = (std::fabs(jump_h - 1.0f) > 0.001f) || (std::fabs(jump_spd - 1.0f) > 0.001f);

    g_pending_custom_jump = false;

    if (custom_jump && IsValidGuestAddr(traj_mgr, 8520) && IsValidGuestAddr(jump_params, 64)) {
        const float vx = LoadF32(base, jump_params + 0u);
        const float vy = LoadF32(base, jump_params + 4u);
        const float vz = LoadF32(base, jump_params + 8u);

        float new_vx = vx * jump_spd;
        float new_vz = vz * jump_spd;

        // If jumping from standstill with jump_speed > 1, apply forward velocity along facing dir
        const float horiz_spd = std::sqrt(vx * vx + vz * vz);
        if (horiz_spd < 0.5f && jump_spd > 1.001f) {
            const float dir_x = LoadF32(base, jump_params + 16u);
            const float dir_z = LoadF32(base, jump_params + 24u);
            const float dir_len = std::sqrt(dir_x * dir_x + dir_z * dir_z);
            if (dir_len > 0.001f) {
                const float boost_spd = 3.0f * (jump_spd - 1.0f);
                new_vx += (dir_x / dir_len) * boost_spd;
                new_vz += (dir_z / dir_len) * boost_spd;
            }
        }

        // Clamp horizontal jump velocity to safe bounds (< 15 m/s) to prevent clipping through vertical walls
        const float new_horiz_spd = std::sqrt(new_vx * new_vx + new_vz * new_vz);
        if (new_horiz_spd > 15.0f) {
            const float scale = 15.0f / new_horiz_spd;
            new_vx *= scale;
            new_vz *= scale;
        }

        float new_vy = vy;
        if (vy > 0.0f) {
            // Cap initial vertical jump impulse to 11.5 m/s (~6.75 meters / 22 feet high!)
            // to keep trajectory within swept collision resolution limits
            new_vy = std::min(vy * jump_h, 11.5f);
        }

        StoreF32(base, jump_params + 0u, new_vx);
        StoreF32(base, jump_params + 4u, new_vy);
        StoreF32(base, jump_params + 8u, new_vz);

        g_pending_custom_jump = true;
        g_custom_jump_vx = new_vx;
        g_custom_jump_vy = new_vy;
        g_custom_jump_vz = new_vz;
    }

    __imp__sub_82D6CA58(ctx, base);

    g_pending_custom_jump = false;
}

// BipedAirTrajMgr::BuildCandidates(traj_mgr)
// Populates N = *(u32*)(traj_mgr + 8472) candidate trajectories at traj_mgr + 4032 + i * 128.
// Candidate i has:
//   +0  (4032 + i*128): pos0 (vec4)
//   +16 (4048 + i*128): vel0 (vec4)
//   +32 (4064 + i*128): grav (vec4, Y at 4068 + i*128)
extern "C" void sub_82D6BF90(PPCContext& ctx, uint8_t* base) {
    const uint32_t traj_mgr = ctx.r3.u32;
    __imp__sub_82D6BF90(ctx, base);

    if (!IsValidGuestAddr(traj_mgr, 8520)) {
        return;
    }

    const float grav_mult = GetGravityMultiplier();
    const uint32_t count = std::min(LoadU32(base, traj_mgr + 8472u), 16u);

    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t cand = traj_mgr + 4032u + i * 128u;
        if (g_pending_custom_jump) {
            StoreF32(base, cand + 16u, g_custom_jump_vx);
            StoreF32(base, cand + 20u, g_custom_jump_vy);
            StoreF32(base, cand + 24u, g_custom_jump_vz);
        }
        if (std::fabs(grav_mult - 1.0f) > 0.001f) {
            const float gy = LoadF32(base, cand + 36u);
            if (gy < -0.1f) {
                StoreF32(base, cand + 36u, gy * grav_mult);
            }
        }
    }
}

// PhysState_Biped_Air::Update(biped_air)
extern "C" void sub_82D2EF38(PPCContext& ctx, uint8_t* base) {
    const uint32_t biped_air = ctx.r3.u32;
    const bool jump_augmented = (GetOffBoardJumpHeightMultiplier() > 1.001f || GetOffBoardJumpSpeedMultiplier() > 1.001f);
    const bool protect_offboard = IsOffBoardDropProtected() || jump_augmented;

    if (IsValidGuestAddr(biped_air, 560)) {
        const uint32_t phys_in  = LoadU32(base, biped_air + 16u);
        const uint32_t cond_mgr = LoadU32(base, biped_air + 20u);
        const uint32_t traj_mgr = LoadU32(base, biped_air + 52u);

        // Clamp downward fall velocity to safe terminal velocity (-11.0 m/s)
        // At -11 m/s, displacement per 60Hz frame is < 0.19m, strictly within Havok capsule radius,
        // completely preventing high-fall ground tunneling/clipping through the floor!
        if (IsValidGuestAddr(phys_in, 2960)) {
            const float vy = LoadF32(base, phys_in + 2884u);
            if (vy < -11.0f) {
                StoreF32(base, phys_in + 2884u, -11.0f);
            }
        }

        if (protect_offboard) {
            SanitizePhysInWipeoutBits(base, phys_in, REXCVAR_GET(skate3_cheat_no_bails), true);
            if (IsValidGuestAddr(cond_mgr, 64)) {
                StoreU8(base, cond_mgr + 52u, 0);
            }
            if (IsValidGuestAddr(traj_mgr, 8520) && LoadU8(base, biped_air + 404u) == 0 && LoadU32(base, biped_air + 452u) > 2u) {
                StoreU8(base, traj_mgr + 8493u, 1u);
            }
        }
    }

    __imp__sub_82D2EF38(ctx, base);

    if (protect_offboard && IsValidGuestAddr(biped_air, 560)) {
        const uint32_t phys_in  = LoadU32(base, biped_air + 16u);
        const uint32_t cond_mgr = LoadU32(base, biped_air + 20u);
        SanitizePhysInWipeoutBits(base, phys_in, REXCVAR_GET(skate3_cheat_no_bails), true);
        if (IsValidGuestAddr(cond_mgr, 64)) {
            StoreU8(base, cond_mgr + 52u, 0);
        }
    }
}

// PhysState_Biped_Air::ReEvaluateLanding(biped_air)
extern "C" void sub_82D2F748(PPCContext& ctx, uint8_t* base) {
    const uint32_t biped_air = ctx.r3.u32;
    __imp__sub_82D2F748(ctx, base);

    if (IsOffBoardDropProtected() && IsValidGuestAddr(biped_air, 560)) {
        const uint32_t cond_mgr = LoadU32(base, biped_air + 20u);
        if (IsValidGuestAddr(cond_mgr, 64)) {
            // Prevent unlandable-trajectory check from triggering an off-board air wipeout
            StoreU8(base, cond_mgr + 52u, 0u);
        }
    }
}

// PhysState_Biped_Air::CheckWipeout(biped_air)
extern "C" void sub_82D2FFF8(PPCContext& ctx, uint8_t* base) {
    if (IsOffBoardDropProtected()) {
        const uint32_t biped_air = ctx.r3.u32;
        if (IsValidGuestAddr(biped_air, 560)) {
            const uint32_t cond_mgr = LoadU32(base, biped_air + 20u);
            if (IsValidGuestAddr(cond_mgr, 64)) {
                StoreU8(base, cond_mgr + 52u, 0u);
            }
        }
        return;
    }
    __imp__sub_82D2FFF8(ctx, base);
}

namespace skate3 {

bool IsSkaterOnBoardAirborne() {
    return g_onboard_in_air;
}

bool IsSkaterBailedOrOffBoard() {
    return g_current_player_state >= 500;
}

static std::atomic<uint64_t> g_last_grind_time_ms{0};

void NotifySkaterGrinding() {
    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    g_last_grind_time_ms.store(now_ms, std::memory_order_relaxed);
}

bool IsSkaterGrinding() {
    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    auto last = g_last_grind_time_ms.load(std::memory_order_relaxed);
    return (now_ms - last) < 350; // Active if GrindTracker ticked within 350ms
}

bool IsSkaterAirborneOrGrinding() {
    const int32_t state = g_current_player_state;
    return (state >= 200 && state < 500) || g_onboard_in_air || IsSkaterGrinding();
}

namespace cheats {

float GetGravityMultiplier() {
    return ::GetGravityMultiplier();
}
float GetPopHeightMultiplier() {
    return ::GetPopHeightMultiplier();
}
float GetOffBoardJumpHeightMultiplier() {
    return ::GetOffBoardJumpHeightMultiplier();
}
float GetOffBoardJumpSpeedMultiplier() {
    return ::GetOffBoardJumpSpeedMultiplier();
}
float GetDropHeightMultiplier() {
    return ::GetDropHeightMultiplier();
}
float GetOnBoardSpeedMultiplier() {
    return ::GetOnBoardSpeedMultiplier();
}
float GetOffBoardSpeedMultiplier() {
    return ::GetOffBoardSpeedMultiplier();
}

void ResetRuntimeTrackingState() {
    g_local_player_ptr = 0;
    g_local_skateboard_ptr = 0;
    g_current_player_state = 0;
    g_onboard_in_air = false;
    g_onboard_peak_y = 0.0f;
    g_onboard_drop_meters = 0.0f;
    g_onboard_landing_grace_frames = 0;
    g_offboard_in_air = false;
    g_offboard_peak_y = 0.0f;
    g_offboard_drop_meters = 0.0f;
    g_offboard_landing_grace_frames = 0;
    g_pending_custom_jump = false;
    g_timer_frame_counter = 1;
    g_start_countdown_timer_id = 0xFFFFFFFFu;
    for (size_t i = 0; i < 16; ++i) {
        g_active_gameplay_timers[i] = {};
    }
}

void SetMockCurrentPlayerState(int32_t state) {
    g_current_player_state = state;
}

int32_t GetCurrentPlayerState() {
    return g_current_player_state;
}

}  // namespace cheats
}  // namespace skate3

