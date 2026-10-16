#pragma once

#include "generated/skate3_init.h"
#include <rex/cvar.h>
#include <string>

//=============================================================================
// Skate 3 Gameplay Cheat CVars
//=============================================================================
REXCVAR_DECLARE(bool, skate3_cheat_no_bails);
REXCVAR_DECLARE(bool, skate3_cheat_freeze_timer);
REXCVAR_DECLARE(std::string, skate3_cheat_gravity);
REXCVAR_DECLARE(std::string, skate3_cheat_pop_height);
REXCVAR_DECLARE(std::string, skate3_cheat_offboard_jump_height);
REXCVAR_DECLARE(std::string, skate3_cheat_offboard_jump_speed);
REXCVAR_DECLARE(std::string, skate3_cheat_drop_height);
REXCVAR_DECLARE(std::string, skate3_cheat_onboard_speed);
REXCVAR_DECLARE(std::string, skate3_cheat_offboard_speed);

namespace skate3 {
namespace cheats {

float GetGravityMultiplier();
float GetPopHeightMultiplier();
float GetOffBoardJumpHeightMultiplier();
float GetOffBoardJumpSpeedMultiplier();
float GetDropHeightMultiplier();
float GetOnBoardSpeedMultiplier();
float GetOffBoardSpeedMultiplier();

void ResetRuntimeTrackingState();
void SetMockCurrentPlayerState(int32_t state);
int32_t GetCurrentPlayerState();

}  // namespace cheats

bool IsSkaterOnBoardAirborne();
bool IsSkaterBailedOrOffBoard();
bool IsSkaterAirborneOrGrinding();

}  // namespace skate3
