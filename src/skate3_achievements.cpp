#include "skate3_achievements.h"
#include "generated/skate3_init.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <ctime>
#include <fstream>
#include <functional>


#include <fmt/format.h>
#include <imgui.h>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/system/flags.h>
#include <rex/system/kernel_state.h>
#include <rex/system/util/xdbf_utils.h>
#include <rex/system/xcontent.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/ui/immediate_drawer.h>
#include <SDL3/SDL.h>

#include "skate3_user_settings.h"
#include "skate3_coord_dump.h"
#include "../third_party/rexglue-sdk/thirdparty/tomlplusplus/vendor/json.hpp"

namespace skate3 {

namespace {

std::atomic<uint32_t> g_local_skater_ptr{0};
std::atomic<bool> g_has_real_hud_gate{false};
std::atomic<bool> g_local_gameplay_active{true};
std::atomic<int64_t> g_last_local_gameplay_ns{0};
std::atomic<bool> g_has_local_phys_pos{false};
std::atomic<float> g_local_phys_pos_x{0.0f};
std::atomic<float> g_local_phys_pos_y{0.0f};
std::atomic<float> g_local_phys_pos_z{0.0f};
std::atomic<uint64_t> g_reset_epoch{0};

static inline uint32_t Fnv1a32(std::string_view str) {
  uint32_t hash = 0x811c9dc5u;
  for (char c : str) {
    hash ^= static_cast<uint8_t>(c);
    hash *= 0x01000193u;
  }
  return hash;
}

static const AchievementDef kStaticAchievements[] = {
    // Base Game Offline Achievements (775 G original + 225 G offline replacements = 1000 G)
    {2, "Park Apprentice", "Complete the skate.Park tutorials", "Complete the skate.Park tutorials", "Base Game", 20, false, {}},
    {3, "Takin 'em to the limit", "Get the terrain meter in the red for your own skate.Park", "Get the terrain meter in the red for your own skate.Park", "Base Game", 20, false, {}},
    {7, "Trick Spammer", "30,000 point line", "30,000 point line", "Base Game", 20, true, {}},
    {8, "Blaow!", "10,000 point trick", "10,000 point trick", "Base Game", 20, true, {}},
    {9, "Did Somebody Get a Boo Boo?", "75,000 point bail", "75,000 point bail", "Base Game", 20, true, {}},
    {10, "Objectifier", "3 Moveable objects used in a single sequence", "3 Moveable objects used in a single sequence", "Base Game", 10, false, {}},
    {11, "Extreme Grindage", "Perform a 100 meter or 300 foot grind", "Perform a 100 meter or 300 foot grind", "Base Game", 10, true, {}},
    {12, "Mantastic Voyage", "3 manuals in a single sequence", "3 manuals in a single sequence", "Base Game", 10, false, {}},
    {13, "Don't be so Mayo", "Land a Miracle Whip (Superdude body flip)", "Land a Miracle Whip (Superdude body flip)", "Base Game", 20, false, {}},
    {14, "100% Pure Adrenaline", "Fly Spread Eagle for 10 seconds", "Fly Spread Eagle for 10 seconds", "Base Game", 10, false, {}},
    {15, "Employee #3", "Earn your second career teammate", "Earn your second career teammate", "Base Game", 20, false, {}},
    {16, "Dedicated", "Everything beaten, everything unlocked, everything earned", "Everything beaten, everything unlocked, everything earned", "Base Game", 100, false, {}},
    {17, "Party at the Penthouse", "Win both Maloof Money Cup contests", "Win both Maloof Money Cup contests", "Base Game", 10, false, {}},
    {18, "T-Mobilized", "Win the T-Mobile sponsored contest", "Win the T-Mobile sponsored contest", "Base Game", 10, false, {}},
    {20, "Employee #2", "Earn your first career teammate", "Earn your first career teammate", "Base Game", 10, false, {}},
    {21, "Beast Unleashed", "Win the Monster contest", "Win the Monster contest", "Base Game", 10, false, {}},
    {22, "Deck Peddler", "1 Million boards sold", "1 Million boards sold", "Base Game", 75, false, {}},
    {23, "GVR Champ", "Win the Goofy vs. Regular contest", "Win the Goofy vs. Regular contest", "Base Game", 10, false, {}},
    {24, "Making a Buddy Call", "Call a Skater to come skate with you", "Call a Skater to come skate with you", "Base Game", 10, false, {}},
    {32, "Is that all you got?", "Own all challenges", "Own all challenges", "Base Game", 50, false, {}},
    {33, "Speed Demon", "Win the final deathrace", "Win the final deathrace", "Base Game", 10, false, {}},
    {34, "In Stereo", "Complete Coach Frank's skate.School Tutorials", "Complete Coach Frank's skate.School Tutorials", "Base Game", 20, false, {}},
    {35, "Masochist", "Beat the \"Thorax Crunch\" Hall of Meat Challenge", "Beat the \"Thorax Crunch\" Hall of Meat Challenge", "Base Game", 10, false, {}},
    {36, "Spot Monopolist", "Own all of the spots", "Own all of the spots", "Base Game", 30, false, {}},
    {37, "All your base are belong to you", "Unlock the Team HQ", "Unlock the Team HQ", "Base Game", 10, false, {}},
    {38, "Employee #1", "Start your team", "Start your team", "Base Game", 10, false, {}},
    {39, "Big-Footy", "Beat the film ender", "Beat the film ender", "Base Game", 10, false, {}},
    {40, "A Dynasty Is Born", "Fully staffed your team", "Fully staffed your team", "Base Game", 30, false, {}},
    {41, "Mass Murderer", "Kill 80 challenges", "Kill 80 challenges", "Base Game", 50, false, {}},
    {42, "The Consumate Pro", "Complete all pro challenges", "Complete all pro challenges", "Base Game", 20, false, {}},
    {43, "King of Kings", "Win the street and tranny ender contests", "Win the street and tranny ender contests", "Base Game", 20, false, {}},
    {44, "Lot Monopolist", "Own all of the lots", "Own all of the lots", "Base Game", 30, false, {}},
    {45, "Sexiest Skater Alive", "Get the covers of Skateboard Mag and Thrasher", "Get the covers of Skateboard Mag and Thrasher", "Base Game", 20, false, {}},
    {46, "Overexposed", "Complete all Promotional Films and Photos", "Complete all Promotional Films and Photos", "Base Game", 20, false, {}},
    {47, "Employee #4", "Earn your third career teammate", "Earn your third career teammate", "Base Game", 20, false, {}},
    // Maloof Money Cup NY DLC (100 G)
    {48, "Money Cup Master", "Win the Maloof NY Contest", "Win the Maloof NY Contest", "Maloof Money Cup NY DLC", 40, false, {}},
    {49, "Maloof Street Slayer", "Complete the Maloof NY Street OTL", "Complete the Maloof NY Street OTL", "Maloof Money Cup NY DLC", 20, false, {}},
    {50, "Maloof Tranny Monster", "Complete the Maloof NY Tranny OTL", "Complete the Maloof NY Tranny OTL", "Maloof Money Cup NY DLC", 20, false, {}},
    {51, "Picture Worth a Thousand Curbs", "Complete both Maloof NY Photo Challenges", "Complete both Maloof NY Photo Challenges", "Maloof Money Cup NY DLC", 10, false, {}},
    {52, "Where's My Money?", "Complete both Maloof NY OTS Challenges", "Complete both Maloof NY OTS Challenges", "Maloof Money Cup NY DLC", 10, false, {}},
    // Danny Way's Hawaiian Dream DLC (150 G)
    {53, "Danny's Demo Machine", "Complete both Demos", "Complete both Demos", "Danny Way's Hawaiian Dream DLC", 10, false, {}},
    {54, "Lei it on the Line", "Complete the \"Touch and Go\" and \"Gazebo Film\" Challenges", "Complete the \"Touch and Go\" and \"Gazebo Film\" Challenges", "Danny Way's Hawaiian Dream DLC", 20, false, {}},
    {55, "Street Locals Only", "Complete the Danny Way Street Path", "Complete the Danny Way Street Path", "Danny Way's Hawaiian Dream DLC", 20, false, {}},
    {56, "Argh! Flying Monkies!", "Complete the \"Jungle Step\" and \"Danny's Mega Gaps\" Challenges", "Complete the \"Jungle Step\" and \"Danny's Mega Gaps\" Challenges", "Danny Way's Hawaiian Dream DLC", 20, false, {}},
    {57, "King of the Jungle", "Complete the Danny Way Tranny Path", "Complete the Danny Way Tranny Path", "Danny Way's Hawaiian Dream DLC", 20, false, {}},
    {58, "Master of Mega Gnar", "Beat Danny Way in the Final Contest", "Beat Danny Way in the Final Contest", "Danny Way's Hawaiian Dream DLC", 50, false, {}},
    {59, "The Ownly Way", "Complete both Danny Way OTS Challenges", "Complete both Danny Way OTS Challenges", "Danny Way's Hawaiian Dream DLC", 10, false, {}},
    // 12 New Offline Base Game Challenges (225 G - replaces online Gamerscore)
    {101, "Isaac’s Day Off", "Unlock Isaac Clarke using the deadspacetoo cheat code", "Keep playing to unlock this secret achievement.", "Base Game", 10, true, {}},
    {102, "Hover Conversion", "Hover above the pavement using the mcfly cheat code", "Keep playing to unlock this secret achievement.", "Base Game", 10, true, {}},
    {103, "Night of the Living Skaters", "Turn the streets into a zombie haven using the zombie cheat code", "Keep playing to unlock this secret achievement.", "Base Game", 10, true, {}},
    {104, "Pocket Skater", "Shrink down to size using the miniskaters cheat code", "Keep playing to unlock this secret achievement.", "Base Game", 10, true, {}},
    {105, "The Mega Drop", "Clear the Super Ultra Mega Park roll-in jump without bailing", "Clear the Super Ultra Mega Park roll-in jump without bailing", "Base Game", 20, false, {}, "105", "gap", "", 0.0f, {343.53f, 140.91f, -729.13f}, {356.63f, 132.34f, -704.69f}, 18.0f},
    {106, "Dam Buster", "Drop down the San Van Spillway Dam face and roll away cleanly", "Drop down the San Van Spillway Dam face and roll away cleanly", "Base Game", 20, false, {}, "106", "gap", "", 0.0f, {448.74f, 104.03f, -485.93f}, {381.84f, 91.46f, -487.19f}, 22.0f},
    {107, "Terminal Velocity", "Reach a speed of 60 mph on your skateboard", "Reach a speed of 60 mph on your skateboard", "Base Game", 20, false, {}},
    {108, "Full Body Cast", "Break 6 or more bones in a single bail", "Break 6 or more bones in a single bail", "Base Game", 20, false, {}},
    {109, "Endless Manual", "Maintain a continuous manual for 100 meters (330 feet)", "Maintain a continuous manual for 100 meters (330 feet)", "Base Game", 15, false, {}},
    {110, "Park Architect", "Save a custom skate.Park with at least 25 placed objects", "Save a custom skate.Park with at least 25 placed objects", "Base Game", 15, false, {}},
    {111, "Chain Reaction", "Chain together 10 or more tricks in a single line", "Chain together 10 or more tricks in a single line", "Base Game", 25, false, {}},
    {112, "King of the Streets", "Own or Kill all 16 base Own the Spot (OTS) locations", "Own or Kill all 16 base Own the Spot (OTS) locations", "Base Game", 50, false, {}},
};

static std::string NormalizeAlnumLower(std::string_view sv) {
  std::string out;
  out.reserve(sv.size());
  for (char c : sv) {
    const unsigned char uc = static_cast<unsigned char>(c);
    if (std::isalnum(uc)) {
      out.push_back(static_cast<char>(std::tolower(uc)));
    }
  }
  return out;
}

}  // namespace

std::vector<std::string> GetTrickGroupTricks(std::string_view group_name) {
  const std::string g = NormalizeAlnumLower(group_name);
  if (g == "flips" || g == "fliptricks" || g == "flip") {
    return {
        "Ollie",           "Kickflip",        "Heelflip",        "Pop Shuvit",
        "FS Pop Shuvit",   "Varial Kickflip", "Varial Heelflip", "360 Flip",
        "Laser Flip",      "Hardflip",        "Inward Heelflip", "Late Flip",
        "Underflip",       "No-Comply",
    };
  }
  if (g == "grinds" || g == "grindsslides" || g == "grind" || g == "slides") {
    return {
        "50-50",      "5-0",       "Nosegrind",  "Crooked",    "Overcrook",
        "Smith",      "Feeble",    "Willy",      "Salad",      "Boardslide",
        "Lipslide",   "Noseslide", "Tailslide",  "Bluntslide", "Noseblunt",
        "Darkslide",  "Crail Slide",
    };
  }
  if (g == "grabs" || g == "grabsairs" || g == "grab" || g == "airs") {
    return {
        "Indy",       "Melon",      "Mute",      "Stalefish", "Tailgrab",
        "Nosegrab",   "Method",     "Japan",     "Rocket Air", "Christ Air",
        "Superdude",  "Benihana",   "Airwalk",   "Tailwalk",
    };
  }
  if (g == "other" || g == "manuals" || g == "plants" || g == "footwork" ||
      g == "manualsplantsfootwork") {
    return {
        "Manual",     "Nose Manual", "Handplant", "Boneless",  "Footplant",
        "Hippy Jump", "Powerslide",  "Revert",    "Wallride",  "Fingerflip",
    };
  }
  return {};
}

std::vector<std::string> MapToCanonicalTricks(std::string_view raw_trick_name) {
  const std::string n = NormalizeAlnumLower(raw_trick_name);
  std::vector<std::string> out;
  if (n.empty() || n == "none") {
    return out;
  }

  auto add = [&](const char* canon) {
    if (std::find(out.begin(), out.end(), canon) == out.end()) {
      out.emplace_back(canon);
    }
  };

  // --- 1. Flip Tricks (14) ---
  if (n == "ollie" || n == "nollie" || n == "bonedollie" || n == "bonednollie") {
    add("Ollie");
  }
  if (n == "kickflip" || n == "doublekickflip" || n == "triplekickflip" ||
      n == "quadkickflip") {
    add("Kickflip");
  }
  if (n == "heelflip" || n == "doubleheelflip" || n == "tripleheelflip" ||
      n == "quadheelflip") {
    add("Heelflip");
  }
  if (n == "popshuvit" || n == "360shuvit" || n == "shuvit" || n == "bspopshuvit") {
    add("Pop Shuvit");
  }
  if (n == "fspopshuvit" || n == "fs360shuvit" || n == "fsshuvit") {
    add("FS Pop Shuvit");
  }
  if (n == "varialkickflip" || n == "nightmareflip" || n == "varialflip") {
    add("Varial Kickflip");
  }
  if (n == "varialheelflip" || n == "heelflipvariallien" || n == "varialheel") {
    add("Varial Heelflip");
  }
  if (n == "360flip" || n == "treflip" || n == "360doubleflip" || n == "360kickflip") {
    add("360 Flip");
  }
  if (n == "laserflip" || n == "360heelflip" || n == "360doubleheelflip") {
    add("Laser Flip");
  }
  if (n == "hardflip" || n == "360hardflip") {
    add("Hardflip");
  }
  if (n == "inwardheelflip" || n == "360inwardheelflip") {
    add("Inward Heelflip");
  }
  if (n == "lateflip" || n.starts_with("late")) {
    add("Late Flip");
  }
  if (n == "underflip" || n.ends_with("underflip")) {
    add("Underflip");
  }
  if (n.find("nocomply") != std::string::npos) {
    add("No-Comply");
  }

  // --- 2. Grinds & Slides (17) ---
  if (n == "5050" || n == "fs5050" || n == "bs5050" || n == "5050grind") {
    add("50-50");
  }
  if (n == "50" || n == "fs50" || n == "bs50" || n == "50grind") {
    add("5-0");
  }
  if (n == "nosegrind" || n == "fsnosegrind" || n == "bsnosegrind") {
    add("Nosegrind");
  }
  if (n == "crooked" || n == "fscrooked" || n == "bscrooked" || n == "crookedgrind") {
    add("Crooked");
  }
  if (n == "overcrook" || n == "fsovercrook" || n == "bsovercrook" || n == "overcrooked") {
    add("Overcrook");
  }
  if (n == "smith" || n == "fssmith" || n == "bssmith" || n == "smithgrind") {
    add("Smith");
  }
  if (n == "feeble" || n == "fsfeeble" || n == "bsfeeble" || n == "feeblegrind") {
    add("Feeble");
  }
  if (n == "willy" || n == "fswilly" || n == "bswilly" || n == "willygrind") {
    add("Willy");
  }
  if (n == "salad" || n == "fssalad" || n == "bssalad" || n == "saladgrind" ||
      n == "suski" || n == "fssuski" || n == "bssuski") {
    add("Salad");
  }
  if (n == "boardslide" || n == "fsboardslide" || n == "bsboardslide") {
    add("Boardslide");
  }
  if (n == "lipslide" || n == "fslipslide" || n == "bslipslide") {
    add("Lipslide");
  }
  if (n == "noseslide" || n == "fsnoseslide" || n == "bsnoseslide") {
    add("Noseslide");
  }
  if (n == "tailslide" || n == "fstailslide" || n == "bstailslide") {
    add("Tailslide");
  }
  if (n == "bluntslide" || n == "fsbluntslide" || n == "bsbluntslide" ||
      n == "blunt" || n == "fsblunt" || n == "bsblunt") {
    add("Bluntslide");
  }
  if (n == "nosebluntslide" || n == "fsnosebluntslide" || n == "bsnosebluntslide" ||
      n == "noseblunt" || n == "fsnoseblunt" || n == "bsnoseblunt") {
    add("Noseblunt");
  }
  if (n == "darkslide" || n == "fsdarkslide" || n == "bsdarkslide") {
    add("Darkslide");
  }
  if (n == "crailslide" || n == "fscrailslide" || n == "bscrailslide" ||
      n == "tailblockslide" || n == "fstailblockslide" || n == "bstailblockslide") {
    add("Crail Slide");
  }

  // --- 3. Grabs & Airs (14) ---
  if (n == "indy" || n == "indyb2b" || n == "indybone" || n == "indytweak" ||
      n == "indy2seatbelt" || n == "indygrab") {
    add("Indy");
  }
  if (n == "melon" || n == "melonb2b" || n == "melonbone" || n == "melontweak" ||
      n == "melon2method" || n == "melongrab") {
    add("Melon");
  }
  if (n == "mute" || n == "muteb2b" || n == "mutebone" || n == "mutetweak" ||
      n == "mute2japan" || n == "mutegrab" || n == "weddle") {
    add("Mute");
  }
  if (n == "stalefish" || n == "stalefishb2b" || n == "stalefishbone" ||
      n == "stalefishtweak" || n == "stale2rocket" || n == "stalefishdogpiss") {
    add("Stalefish");
  }
  if (n == "tailgrab" || n == "tailgrabb2b" || n == "tailgrabbone" ||
      n == "tailgrabtweak" || n == "tail2tailwalk" || n == "tail2onefoot" ||
      n == "tail2benihana") {
    add("Tailgrab");
  }
  if (n == "nosegrab" || n == "nosegrabb2b" || n == "nosegrabbone" ||
      n == "nosegrabtweak" || n == "nose2rocket" || n == "nose2dogpiss" ||
      n == "nose2airwalk") {
    add("Nosegrab");
  }
  if (n == "method" || n == "melon2method") {
    add("Method");
  }
  if (n == "japan" || n == "japanair" || n == "mute2japan") {
    add("Japan");
  }
  if (n == "rocketair" || n == "rocket" || n == "nose2rocket" || n == "stale2rocket") {
    add("Rocket Air");
  }
  if (n == "christair" || n == "nohand2christair") {
    add("Christ Air");
  }
  if (n == "superdude" || n == "nohand2superdude" || n == "miraclewhip") {
    add("Superdude");
  }
  if (n == "benihana" || n == "tail2benihana") {
    add("Benihana");
  }
  if (n == "airwalk" || n == "nose2airwalk") {
    add("Airwalk");
  }
  if (n == "tailwalk" || n == "tail2tailwalk") {
    add("Tailwalk");
  }

  // --- 4. Manuals, Plants & Footwork (10) ---
  if (n == "manual" || n == "onefootmanual") {
    add("Manual");
  }
  if (n == "nosemanual" || n == "onefootnosemanual") {
    add("Nose Manual");
  }
  if (n == "handplant" || n == "invert" || n == "andrecht" || n == "millerflip" ||
      n == "sadplant" || n == "fastplant" || n == "gymnastplant" || n == "eggplant" ||
      n == "onefootinvert" || n == "laybackair") {
    add("Handplant");
  }
  if (n == "boneless" || n == "fsboneless" || n == "bsboneless" ||
      n == "beanplant" || n == "fsbeanplant" || n == "bsbeanplant") {
    add("Boneless");
  }
  if (n == "footplant" || n == "judo" || n == "onefoot" || n == "tail2onefoot" ||
      n == "nose2dogpiss" || n == "stalefishdogpiss" || n == "dogpiss") {
    add("Footplant");
  }
  if (n == "hippyjump" || n == "hippyjump180" || n == "hippyjump360") {
    add("Hippy Jump");
  }
  if (n == "powerslide" || n == "fspowerslide" || n == "bspowerslide" ||
      n == "180powerslide" || n == "fs180powerslide" || n == "bs180powerslide") {
    add("Powerslide");
  }
  if (n == "revert" || n == "fsrevert" || n == "bsrevert") {
    add("Revert");
  }
  if (n == "wallride" || n == "fswallride" || n == "bswallride" ||
      n == "wallie" || n == "fswallie" || n == "bswallie") {
    add("Wallride");
  }
  if (n.find("fingerflip") != std::string::npos) {
    add("Fingerflip");
  }

  return out;
}

// ============================================================================
// Achievement & Toast Sound System (Procedural Synthesis + WAV Customization)
// ============================================================================

REXCVAR_DEFINE_BOOL(achievement_sound, true, "Achievements",
                    "Play sound effects on achievement unlocks and progress notifications");

REXCVAR_DEFINE_DOUBLE(achievement_sound_volume, 1.0, "Achievements",
                      "Volume of achievement sound effects (0.0 to 1.0)");

namespace {

constexpr int kSoundSampleRate = 48000;
constexpr float kSoundPi = 3.14159265358979323846f;

static inline float WhiteNoise(uint32_t& seed) {
  seed = seed * 1664525u + 1013904223u;
  return static_cast<float>(static_cast<int32_t>(seed)) / 2147483648.0f;
}

// 1. Skate 3 Big Combo Land & Bank Sound (Achievement Pop)
// Components:
// 1) Warm, punchy skateboard 4-wheel landing impact (sub-bass thump + maple deck resonance + low-mid urethane texture)
// 2) Score HUD banking whoosh / air swell (smooth sweep across low-mids)
// 3) Resonant combo bank chord (warm Rhodes / vibraphone chime in middle register C4-E5, no shrill treble)
// 4) Warm single-pole lowpass filter (< 1.1 kHz) to guarantee zero harshness
std::vector<float> SynthesizeUnlockSound() {
  const int total_samples = static_cast<int>(kSoundSampleRate * 1.30f);
  std::vector<float> pcm(total_samples * 2, 0.0f);
  uint32_t noise_seed = 987654321u;

  const float fc = 1100.0f;
  const float wc = 2.0f * kSoundPi * fc / kSoundSampleRate;
  const float lp_alpha = wc / (1.0f + wc);
  float lp_l = 0.0f;
  float lp_r = 0.0f;

  for (int i = 0; i < total_samples; ++i) {
    const float t = static_cast<float>(i) / kSoundSampleRate;
    float raw_l = 0.0f;
    float raw_r = 0.0f;

    // --- A. Board Landing Impact (t = 0.00s to 0.08s) ---
    if (t < 0.08f) {
      // 1. Deck thump: 105 Hz sweeping down to 42 Hz with fast decay
      const float thump_decay = std::exp(-t / 0.026f);
      const float thump_freq = 42.0f + 63.0f * std::exp(-t / 0.018f);
      const float thump = 0.65f * thump_decay * std::sin(2.0f * kSoundPi * thump_freq * t);

      // 2. Hollow maple deck body resonance (warm 195 Hz)
      const float wood_decay = std::exp(-t / 0.038f);
      const float wood = 0.42f * wood_decay * std::sin(2.0f * kSoundPi * 195.0f * t);

      // 3. Wheel urethane contact texture (filtered low-mid burst around 360 Hz)
      const float wheel_decay = std::exp(-t / 0.022f);
      const float wheel_n = WhiteNoise(noise_seed);
      const float wheel = 0.28f * wheel_decay * wheel_n * std::sin(2.0f * kSoundPi * 360.0f * t);

      raw_l += (thump * 0.95f + wood * 1.0f + wheel * 0.90f);
      raw_r += (thump * 0.95f + wood * 0.90f + wheel * 1.0f);
    }

    // --- B. Score Bank Whoosh / Air Swell (t = 0.025s to 0.16s) ---
    if (t >= 0.025f && t < 0.16f) {
      const float wt = (t - 0.025f) / 0.135f;
      const float whoosh_env = std::sin(wt * kSoundPi);
      const float whoosh_freq = 190.0f + 250.0f * (wt * wt);
      const float whoosh_noise = WhiteNoise(noise_seed) * 0.15f;
      const float whoosh =
          0.26f * whoosh_env * (std::sin(2.0f * kSoundPi * whoosh_freq * t) + whoosh_noise);

      raw_l += whoosh * (1.05f - 0.4f * wt);
      raw_r += whoosh * (0.65f + 0.4f * wt);
    }

    // --- C. Warm Combo Bank Chord (Mellow Rhodes / Vibraphone Chime) ---
    // Rings out warmly in the middle register (C4 to E5: 261 Hz to 659 Hz)
    if (t >= 0.080f) {
      const float bt = t - 0.080f;
      const float attack = std::min(1.0f, bt / 0.014f);

      struct ChimeTone {
        float freq;
        float amp;
        float decay;
        float pan_l;
        float pan_r;
      };

      static const ChimeTone kBankChord[] = {
          {261.63f, 0.42f, 0.95f, 0.95f, 0.90f},  // C4 (Deep warm fundamental)
          {329.63f, 0.38f, 0.85f, 0.85f, 0.95f},  // E4 (Major third body)
          {392.00f, 0.35f, 0.75f, 1.00f, 0.85f},  // G4 (Fifth resonance)
          {523.25f, 0.28f, 0.65f, 0.88f, 0.98f},  // C5 (Warm bell octave)
          {659.25f, 0.18f, 0.50f, 0.95f, 0.92f},  // E5 (Soft mellow shimmer)
      };

      for (const auto& chord : kBankChord) {
        const float env = attack * std::exp(-bt / chord.decay);
        const float phase_l = 2.0f * kSoundPi * (chord.freq - 0.25f) * bt;
        const float phase_r = 2.0f * kSoundPi * (chord.freq + 0.25f) * bt;
        const float tone_l = std::sin(phase_l);
        const float tone_r = std::sin(phase_r);

        const float harm_l = 0.15f * std::sin(2.0f * phase_l);
        const float harm_r = 0.15f * std::sin(2.0f * phase_r);

        const float val_l = env * chord.amp * (tone_l + harm_l);
        const float val_r = env * chord.amp * (tone_r + harm_r);

        raw_l += val_l * chord.pan_l;
        raw_r += val_r * chord.pan_r;
      }
    }

    // Apply warm single-pole lowpass filter
    lp_l += lp_alpha * (raw_l - lp_l);
    lp_r += lp_alpha * (raw_r - lp_r);

    // Warm analog saturation (tanh)
    pcm[i * 2 + 0] = std::tanh(lp_l * 1.05f) * 0.78f;
    pcm[i * 2 + 1] = std::tanh(lp_r * 1.05f) * 0.78f;
  }
  return pcm;
}

// 2. Skate 3 Combo Multiplier / Progress Tick (Fractional Progress Sound)
// A soft, tactile wooden "thoc" / gentle marimba pop (~0.12s). Zero shrill treble.
std::vector<float> SynthesizeProgressSound() {
  const int total_samples = static_cast<int>(kSoundSampleRate * 0.14f);
  std::vector<float> pcm(total_samples * 2, 0.0f);

  const float fc = 950.0f;
  const float wc = 2.0f * kSoundPi * fc / kSoundSampleRate;
  const float lp_alpha = wc / (1.0f + wc);
  float lp_l = 0.0f;
  float lp_r = 0.0f;

  for (int i = 0; i < total_samples; ++i) {
    const float t = static_cast<float>(i) / kSoundSampleRate;
    float raw_l = 0.0f;
    float raw_r = 0.0f;

    // Tactile wood pop: 340 Hz sweeping down to 160 Hz (t = 0 to 0.028s)
    if (t < 0.028f) {
      const float frac = t / 0.028f;
      const float pop_freq = 340.0f - 180.0f * frac;
      const float pop_env = (1.0f - frac);
      const float pop = 0.45f * pop_env * std::sin(2.0f * kSoundPi * pop_freq * t);
      raw_l += pop * 0.95f;
      raw_r += pop * 0.95f;
    }

    // Gentle warm marimba tick (t = 0.006s onwards)
    if (t >= 0.006f) {
      const float bt = t - 0.006f;
      const float attack = std::min(1.0f, bt / 0.004f);
      const float env1 = attack * std::exp(-bt / 0.048f);
      const float env2 = attack * std::exp(-bt / 0.032f);

      // Low-mid warm notes: A4 (440 Hz) and E5 (659 Hz)
      const float wave1 = 0.30f * env1 * std::sin(2.0f * kSoundPi * 440.00f * bt);
      const float wave2 = 0.15f * env2 * std::sin(2.0f * kSoundPi * 659.25f * bt);

      raw_l += (wave1 * 0.95f + wave2 * 0.85f);
      raw_r += (wave1 * 0.85f + wave2 * 0.95f);
    }

    lp_l += lp_alpha * (raw_l - lp_l);
    lp_r += lp_alpha * (raw_r - lp_r);

    pcm[i * 2 + 0] = std::tanh(lp_l * 1.10f) * 0.65f;
    pcm[i * 2 + 1] = std::tanh(lp_r * 1.10f) * 0.65f;
  }
  return pcm;
}

std::vector<float> LoadCustomWavOrSynthesize(
    const std::vector<std::filesystem::path>& candidate_paths,
    const std::function<std::vector<float>()>& synth_fn) {
  for (const auto& p : candidate_paths) {
    std::error_code ec;
    if (std::filesystem::exists(p, ec)) {
      SDL_AudioSpec wav_spec{};
      Uint8* wav_buf = nullptr;
      Uint32 wav_len = 0;
      if (SDL_LoadWAV(p.string().c_str(), &wav_spec, &wav_buf, &wav_len)) {
        SDL_AudioSpec target_spec{};
        target_spec.channels = 2;
        target_spec.format = SDL_AUDIO_F32LE;
        target_spec.freq = kSoundSampleRate;
        Uint8* dst_data = nullptr;
        int dst_len = 0;
        if (SDL_ConvertAudioSamples(&wav_spec, wav_buf, static_cast<int>(wav_len),
                                    &target_spec, &dst_data, &dst_len) &&
            dst_data) {
          std::vector<float> result(dst_len / sizeof(float));
          std::memcpy(result.data(), dst_data, dst_len);
          SDL_free(dst_data);
          SDL_free(wav_buf);
          return result;
        }
        SDL_free(wav_buf);
      }
    }
  }
  return synth_fn();
}

const std::vector<float>& GetUnlockSoundPcm() {
  static const std::vector<float> s_pcm = []() {
    return LoadCustomWavOrSynthesize(
        {"sound/achievement.wav", "sound/achievements/unlock.wav",
         "game/sound/achievement.wav", "mods/sound/achievement.wav"},
        SynthesizeUnlockSound);
  }();
  return s_pcm;
}

const std::vector<float>& GetProgressSoundPcm() {
  static const std::vector<float> s_pcm = []() {
    return LoadCustomWavOrSynthesize(
        {"sound/progress.wav", "sound/achievements/progress.wav",
         "game/sound/progress.wav", "mods/sound/progress.wav"},
        SynthesizeProgressSound);
  }();
  return s_pcm;
}

}  // namespace

// ============================================================================
// AchievementToastDialog
// ============================================================================

AchievementToastDialog::AchievementToastDialog(rex::ui::ImGuiDrawer* drawer)
    : ImGuiDialog(drawer) {}

AchievementToastDialog::~AchievementToastDialog() {
  if (unlock_stream_) {
    SDL_DestroyAudioStream(unlock_stream_);
    unlock_stream_ = nullptr;
  }
  if (progress_stream_) {
    SDL_DestroyAudioStream(progress_stream_);
    progress_stream_ = nullptr;
  }
}

void AchievementToastDialog::EnsureAudioStreams() {
  if (unlock_stream_ && progress_stream_) {
    return;
  }

  if (!SDL_WasInit(SDL_INIT_AUDIO)) {
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
      return;
    }
  }

  SDL_AudioSpec spec{};
  spec.channels = 2;
  spec.format = SDL_AUDIO_F32LE;
  spec.freq = kSoundSampleRate;

  if (!unlock_stream_) {
    unlock_stream_ =
        SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
    if (unlock_stream_) {
      SDL_ResumeAudioStreamDevice(unlock_stream_);
    }
  }

  if (!progress_stream_) {
    progress_stream_ =
        SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
    if (progress_stream_) {
      SDL_ResumeAudioStreamDevice(progress_stream_);
    }
  }
}

void AchievementToastDialog::PlayUnlockSound() {
  if (!REXCVAR_GET(achievement_sound)) {
    return;
  }
  EnsureAudioStreams();
  if (!unlock_stream_) {
    return;
  }
  const float vol = std::clamp(static_cast<float>(REXCVAR_GET(achievement_sound_volume)), 0.0f, 1.0f);
  if (vol <= 0.0f) {
    return;
  }

  const auto& pcm = GetUnlockSoundPcm();
  if (pcm.empty()) {
    return;
  }

  SDL_ClearAudioStream(unlock_stream_);
  if (std::abs(vol - 1.0f) > 0.001f) {
    std::vector<float> scaled(pcm.size());
    for (size_t i = 0; i < pcm.size(); ++i) {
      scaled[i] = pcm[i] * vol;
    }
    SDL_PutAudioStreamData(unlock_stream_, scaled.data(),
                           static_cast<int>(scaled.size() * sizeof(float)));
  } else {
    SDL_PutAudioStreamData(unlock_stream_, pcm.data(),
                           static_cast<int>(pcm.size() * sizeof(float)));
  }
}

void AchievementToastDialog::PlayProgressSound() {
  if (!REXCVAR_GET(achievement_sound)) {
    return;
  }
  EnsureAudioStreams();
  if (!progress_stream_) {
    return;
  }
  const float vol = std::clamp(static_cast<float>(REXCVAR_GET(achievement_sound_volume)), 0.0f, 1.0f);
  if (vol <= 0.0f) {
    return;
  }

  const auto& pcm = GetProgressSoundPcm();
  if (pcm.empty()) {
    return;
  }

  SDL_ClearAudioStream(progress_stream_);
  if (std::abs(vol - 1.0f) > 0.001f) {
    std::vector<float> scaled(pcm.size());
    for (size_t i = 0; i < pcm.size(); ++i) {
      scaled[i] = pcm[i] * vol;
    }
    SDL_PutAudioStreamData(progress_stream_, scaled.data(),
                           static_cast<int>(scaled.size() * sizeof(float)));
  } else {
    SDL_PutAudioStreamData(progress_stream_, pcm.data(),
                           static_cast<int>(pcm.size() * sizeof(float)));
  }
}

void AchievementToastDialog::PostToast(const AchievementToastItem& item) {
  std::lock_guard lock(mutex_);
  queue_.push_back(item);
  if (!is_active_) {
    is_active_ = true;
    current_ = queue_.front();
    queue_.pop_front();
    start_time_ = std::chrono::steady_clock::now();
    PlayUnlockSound();
  }
}

void AchievementToastDialog::PostProgressToast(const AchievementProgressToastItem& item) {
  std::lock_guard lock(mutex_);
  progress_queue_.push_back(item);
  if (!is_progress_active_) {
    is_progress_active_ = true;
    current_progress_ = progress_queue_.front();
    progress_queue_.pop_front();
    progress_start_time_ = std::chrono::steady_clock::now();
    PlayProgressSound();
  }
}

void AchievementToastDialog::Clear() {
  std::lock_guard lock(mutex_);
  queue_.clear();
  current_.reset();
  is_active_ = false;
  progress_queue_.clear();
  current_progress_.reset();
  is_progress_active_ = false;
  if (unlock_stream_) {
    SDL_ClearAudioStream(unlock_stream_);
  }
  if (progress_stream_) {
    SDL_ClearAudioStream(progress_stream_);
  }
}

bool AchievementToastDialog::WantsContinuousRepaint() const {
  std::lock_guard lock(mutex_);
  if (is_active_ || !queue_.empty() || is_progress_active_ || !progress_queue_.empty()) {
    return true;
  }
  if (manager_ && manager_->HasActiveTelemetryTriggers()) {
    return true;
  }
  return false;
}

void AchievementToastDialog::OnDraw(ImGuiIO& io) {
  if (manager_) {
    manager_->Update(io.DeltaTime);
  }

  std::lock_guard lock(mutex_);
  const auto now = std::chrono::steady_clock::now();
  const float s = std::clamp(io.DisplaySize.y / 1080.0f, 0.6f, 2.5f);
  ImDrawList* dl = ImGui::GetForegroundDrawList();
  if (!dl) {
    return;
  }
  ImFont* bold = imgui_drawer()->ui_font_semibold() ? imgui_drawer()->ui_font_semibold() : ImGui::GetFont();
  ImFont* reg = imgui_drawer()->ui_font() ? imgui_drawer()->ui_font() : ImGui::GetFont();

  // ============================================================================
  // 1. Full Achievement Unlock Toast (bottom-center, ~4 seconds)
  // ============================================================================
  if (is_active_ || !queue_.empty()) {
    if (!is_active_ && !queue_.empty()) {
      is_active_ = true;
      current_ = queue_.front();
      queue_.pop_front();
      start_time_ = now;
      PlayUnlockSound();
    }

    constexpr float kEnterDuration = 0.4f;
    constexpr float kHoldDuration = 4.0f;
    constexpr float kExitDuration = 0.6f;
    constexpr float kTotalDuration = kEnterDuration + kHoldDuration + kExitDuration;

    float elapsed = std::chrono::duration<float>(now - start_time_).count();
    if (elapsed >= kTotalDuration) {
      if (!queue_.empty()) {
        current_ = queue_.front();
        queue_.pop_front();
        start_time_ = now;
        elapsed = 0.0f;
        PlayUnlockSound();
      } else {
        is_active_ = false;
        current_.reset();
      }
    }

    if (current_) {

  float alpha = 1.0f;
  float y_offset = 0.0f;

  if (elapsed < kEnterDuration) {
    const float t = elapsed / kEnterDuration;
    const float e = 1.0f - std::pow(1.0f - t, 3.0f);
    alpha = std::clamp(t * 1.5f, 0.0f, 1.0f);
    y_offset = (1.0f - e) * 50.0f;
  } else if (elapsed > kEnterDuration + kHoldDuration) {
    const float t = (elapsed - (kEnterDuration + kHoldDuration)) / kExitDuration;
    const float e = t * t;
    alpha = std::clamp(1.0f - t, 0.0f, 1.0f);
    y_offset = e * 35.0f;
  }

  ImDrawList* dl = ImGui::GetForegroundDrawList();
  if (!dl) {
    return;
  }

  const float s = std::clamp(io.DisplaySize.y / 1080.0f, 0.6f, 2.5f);

  ImFont* bold = imgui_drawer()->ui_font_semibold() ? imgui_drawer()->ui_font_semibold() : ImGui::GetFont();
  ImFont* reg = imgui_drawer()->ui_font() ? imgui_drawer()->ui_font() : ImGui::GetFont();

  const float card_w = 480.0f * s;
  const float card_x0 = (io.DisplaySize.x - card_w) * 0.5f;
  const float card_x1 = card_x0 + card_w;

  // Gamerscore badge (+XX G) metrics on far right
  const std::string score_str = fmt::format("+{} G", current_->gamerscore);
  const float pill_font_size = 15.0f * s;
  const ImVec2 score_extent = bold->CalcTextSizeA(pill_font_size, FLT_MAX, 0.0f, score_str.c_str());
  const float pill_pad_x = 10.0f * s;
  const float pill_w = score_extent.x + pill_pad_x * 2.0f;
  const float pill_h = 24.0f * s;
  const float pill_x1 = card_x1 - 16.0f * s;
  const float pill_x0 = pill_x1 - pill_w;

  const float text_x0 = card_x0 + 72.0f * s;
  const float text_max_w = std::max(80.0f * s, pill_x0 - text_x0 - 12.0f * s);

  // Word-wrap description into up to 2 lines
  const float desc_size = 13.5f * s;
  std::string desc_line1;
  std::string desc_line2;
  if (!current_->description.empty()) {
    if (reg->CalcTextSizeA(desc_size, FLT_MAX, 0.0f, current_->description.c_str()).x <= text_max_w) {
      desc_line1 = current_->description;
    } else {
      size_t split_pos = std::string::npos;
      for (size_t i = 0; i < current_->description.size(); ++i) {
        if (current_->description[i] == ' ') {
          std::string prefix = current_->description.substr(0, i);
          if (reg->CalcTextSizeA(desc_size, FLT_MAX, 0.0f, prefix.c_str()).x <= text_max_w) {
            split_pos = i;
          } else {
            break;
          }
        }
      }
      if (split_pos != std::string::npos) {
        desc_line1 = current_->description.substr(0, split_pos);
        desc_line2 = current_->description.substr(split_pos + 1);
      } else {
        desc_line1 = current_->description;
        while (!desc_line1.empty() &&
               reg->CalcTextSizeA(desc_size, FLT_MAX, 0.0f, desc_line1.c_str()).x > text_max_w) {
          desc_line1.pop_back();
        }
        desc_line2 = current_->description.substr(desc_line1.size());
      }
      if (reg->CalcTextSizeA(desc_size, FLT_MAX, 0.0f, desc_line2.c_str()).x > text_max_w) {
        while (!desc_line2.empty() &&
               reg->CalcTextSizeA(desc_size, FLT_MAX, 0.0f, (desc_line2 + "...").c_str()).x > text_max_w) {
          desc_line2.pop_back();
        }
        desc_line2 += "...";
      }
    }
  }

  const float card_h = (!desc_line2.empty() ? 104.0f : (!desc_line1.empty() ? 88.0f : 76.0f)) * s;
  const float base_y1 = io.DisplaySize.y - 65.0f * s;
  const float card_y1 = base_y1 + y_offset * s;
  const float card_y0 = card_y1 - card_h;
  const float corner_radius = 8.0f * s;

  const ImU32 bg_col = IM_COL32(10, 14, 16, static_cast<int>(238 * alpha));
  const ImU32 border_col = IM_COL32(213, 235, 10, static_cast<int>(220 * alpha));
  const ImU32 shadow_col = IM_COL32(0, 0, 0, static_cast<int>(140 * alpha));

  // Drop shadow
  dl->AddRectFilled(ImVec2(card_x0 - 2.0f * s, card_y0 + 4.0f * s),
                    ImVec2(card_x1 + 2.0f * s, card_y1 + 6.0f * s),
                    shadow_col, corner_radius + 2.0f * s);

  // Card background & border
  dl->AddRectFilled(ImVec2(card_x0, card_y0), ImVec2(card_x1, card_y1),
                    bg_col, corner_radius);
  dl->AddRect(ImVec2(card_x0, card_y0), ImVec2(card_x1, card_y1),
              border_col, corner_radius, 0, 1.5f * s);

  const float badge_cx = card_x0 + 39.0f * s;
  const float badge_cy = (card_y0 + card_y1) * 0.5f;

  // Left icon or Gamerscore badge
  void* icon_tex = manager_ ? manager_->GetAchievementIcon(current_->id) : nullptr;
  if (icon_tex) {
    const float icon_sz = 48.0f * s;
    const float icon_x0 = card_x0 + 15.0f * s;
    const float icon_y0 = badge_cy - icon_sz * 0.5f;
    const float icon_x1 = icon_x0 + icon_sz;
    const float icon_y1 = icon_y0 + icon_sz;
    const float icon_radius = 6.0f * s;

    dl->AddImageRounded(reinterpret_cast<ImTextureID>(icon_tex),
                        ImVec2(icon_x0, icon_y0), ImVec2(icon_x1, icon_y1),
                        ImVec2(0, 0), ImVec2(1, 1),
                        IM_COL32(255, 255, 255, static_cast<int>(255 * alpha)),
                        icon_radius);
    dl->AddRect(ImVec2(icon_x0, icon_y0), ImVec2(icon_x1, icon_y1),
                border_col, icon_radius, 0, 1.5f * s);
  } else {
    // Circular Xbox Gamerscore fallback badge on left
    const float badge_r = 22.0f * s;

    dl->AddCircleFilled(ImVec2(badge_cx, badge_cy), badge_r,
                        IM_COL32(16, 50, 46, static_cast<int>(255 * alpha)), 32);
    dl->AddCircle(ImVec2(badge_cx, badge_cy), badge_r,
                  border_col, 32, 2.0f * s);

    const float g_size = 20.0f * s;
    const char* g_str = "G";
    const ImVec2 g_extent = bold->CalcTextSizeA(g_size, FLT_MAX, 0.0f, g_str);
    dl->AddText(bold, g_size,
                ImVec2(badge_cx - g_extent.x * 0.5f, badge_cy - g_extent.y * 0.5f),
                IM_COL32(255, 255, 255, static_cast<int>(255 * alpha)), g_str);
  }

  // Gamerscore badge (+XX G) on far right
  const float pill_y0 = badge_cy - pill_h * 0.5f;
  const float pill_y1 = badge_cy + pill_h * 0.5f;

  dl->AddRectFilled(ImVec2(pill_x0, pill_y0), ImVec2(pill_x1, pill_y1),
                    IM_COL32(213, 235, 10, static_cast<int>(40 * alpha)), 6.0f * s);
  dl->AddRect(ImVec2(pill_x0, pill_y0), ImVec2(pill_x1, pill_y1),
              IM_COL32(213, 235, 10, static_cast<int>(120 * alpha)), 6.0f * s, 0, 1.0f * s);
  dl->AddText(bold, pill_font_size,
              ImVec2(pill_x0 + pill_pad_x, pill_y0 + (pill_h - score_extent.y) * 0.5f),
              IM_COL32(213, 235, 10, static_cast<int>(255 * alpha)), score_str.c_str());

  // Header: "ACHIEVEMENT UNLOCKED"
  const char* header_str = "ACHIEVEMENT UNLOCKED";
  const float header_size = 12.0f * s;
  dl->AddText(bold, header_size,
              ImVec2(text_x0, card_y0 + 11.0f * s),
              IM_COL32(213, 235, 10, static_cast<int>(255 * alpha)), header_str);

  // Title:
  const float title_size = 17.5f * s;
  std::string truncated_title = current_->name;
  if (bold->CalcTextSizeA(title_size, FLT_MAX, 0.0f, truncated_title.c_str()).x > text_max_w) {
    while (!truncated_title.empty() &&
           bold->CalcTextSizeA(title_size, FLT_MAX, 0.0f, (truncated_title + "...").c_str()).x > text_max_w) {
      truncated_title.pop_back();
    }
    truncated_title += "...";
  }
  dl->AddText(bold, title_size,
              ImVec2(text_x0, card_y0 + 27.0f * s),
              IM_COL32(250, 252, 252, static_cast<int>(255 * alpha)), truncated_title.c_str());

  // Description:
  const ImU32 desc_col = IM_COL32(195, 206, 210, static_cast<int>(245 * alpha));
  if (!desc_line1.empty()) {
    dl->AddText(reg, desc_size,
                ImVec2(text_x0, card_y0 + 50.0f * s),
                desc_col, desc_line1.c_str());
  }
  if (!desc_line2.empty()) {
    dl->AddText(reg, desc_size,
                ImVec2(text_x0, card_y0 + 67.0f * s),
                desc_col, desc_line2.c_str());
  }
}
}

  // ============================================================================
  // 2. Fractional Progress Toast on the Side of the Screen (~2 seconds)
  // ============================================================================
  if (is_progress_active_ || !progress_queue_.empty()) {
    if (!is_progress_active_ && !progress_queue_.empty()) {
      is_progress_active_ = true;
      current_progress_ = progress_queue_.front();
      progress_queue_.pop_front();
      progress_start_time_ = now;
      PlayProgressSound();
    }

    constexpr float kProgEnter = 0.20f;
    constexpr float kProgHold = 2.00f;
    constexpr float kProgExit = 0.30f;
    constexpr float kProgTotal = kProgEnter + kProgHold + kProgExit;

    const float p_elapsed = std::chrono::duration<float>(now - progress_start_time_).count();
    if (p_elapsed >= kProgTotal) {
      if (!progress_queue_.empty()) {
        current_progress_ = progress_queue_.front();
        progress_queue_.pop_front();
        progress_start_time_ = now;
        PlayProgressSound();
      } else {
        is_progress_active_ = false;
        current_progress_.reset();
      }
    } else if (current_progress_) {
      float p_alpha = 1.0f;
      float p_x_offset = 0.0f;

      if (p_elapsed < kProgEnter) {
        const float t = p_elapsed / kProgEnter;
        const float e = 1.0f - std::pow(1.0f - t, 3.0f);
        p_alpha = std::clamp(t * 1.5f, 0.0f, 1.0f);
        p_x_offset = (1.0f - e) * 60.0f * s;  // slide in from right edge
      } else if (p_elapsed > kProgEnter + kProgHold) {
        const float t = (p_elapsed - (kProgEnter + kProgHold)) / kProgExit;
        const float e = t * t;
        p_alpha = std::clamp(1.0f - t, 0.0f, 1.0f);
        p_x_offset = e * 60.0f * s;            // slide out to right
      }

      const float p_card_w = 320.0f * s;
      const float p_card_h = 74.0f * s;
      const float p_card_x1 = io.DisplaySize.x - 24.0f * s + p_x_offset;
      const float p_card_x0 = p_card_x1 - p_card_w;
      const float p_card_y0 = io.DisplaySize.y * 0.44f - p_card_h * 0.5f;
      const float p_card_y1 = p_card_y0 + p_card_h;
      const float p_radius = 8.0f * s;

      const ImU32 p_bg_col = IM_COL32(10, 14, 16, static_cast<int>(238 * p_alpha));
      const ImU32 p_border_col = IM_COL32(213, 235, 10, static_cast<int>(220 * p_alpha));
      const ImU32 p_shadow_col = IM_COL32(0, 0, 0, static_cast<int>(140 * p_alpha));

      // Drop shadow
      dl->AddRectFilled(ImVec2(p_card_x0 - 2.0f * s, p_card_y0 + 3.0f * s),
                        ImVec2(p_card_x1 + 2.0f * s, p_card_y1 + 5.0f * s),
                        p_shadow_col, p_radius + 2.0f * s);

      // Card background & border
      dl->AddRectFilled(ImVec2(p_card_x0, p_card_y0), ImVec2(p_card_x1, p_card_y1),
                        p_bg_col, p_radius);
      dl->AddRect(ImVec2(p_card_x0, p_card_y0), ImVec2(p_card_x1, p_card_y1),
                  p_border_col, p_radius, 0, 1.5f * s);

      // Icon on left (if available)
      void* p_icon_tex = manager_ ? manager_->GetAchievementIcon(current_progress_->achievement_id) : nullptr;
      float p_text_x0 = p_card_x0 + 16.0f * s;
      if (p_icon_tex) {
        const float p_icon_sz = 40.0f * s;
        const float p_icon_x0 = p_card_x0 + 12.0f * s;
        const float p_icon_y0 = (p_card_y0 + p_card_y1) * 0.5f - p_icon_sz * 0.5f;
        dl->AddImageRounded(reinterpret_cast<ImTextureID>(p_icon_tex),
                            ImVec2(p_icon_x0, p_icon_y0),
                            ImVec2(p_icon_x0 + p_icon_sz, p_icon_y0 + p_icon_sz),
                            ImVec2(0, 0), ImVec2(1, 1),
                            IM_COL32(255, 255, 255, static_cast<int>(255 * p_alpha)),
                            6.0f * s);
        dl->AddRect(ImVec2(p_icon_x0, p_icon_y0),
                    ImVec2(p_icon_x0 + p_icon_sz, p_icon_y0 + p_icon_sz),
                    p_border_col, 6.0f * s, 0, 1.0f * s);
        p_text_x0 = p_icon_x0 + p_icon_sz + 12.0f * s;
      }
      const float p_text_x1 = p_card_x1 - 16.0f * s;

      // Line 1: Item name (left) + fraction count (right)
      const float item_font_sz = 14.0f * s;
      const std::string count_str = fmt::format("{} / {}", current_progress_->current_count, current_progress_->total_count);
      const ImVec2 count_extent = bold->CalcTextSizeA(item_font_sz, FLT_MAX, 0.0f, count_str.c_str());
      const float count_x0 = p_text_x1 - count_extent.x;

      dl->AddText(bold, item_font_sz,
                  ImVec2(count_x0, p_card_y0 + 10.0f * s),
                  IM_COL32(213, 235, 10, static_cast<int>(255 * p_alpha)), count_str.c_str());

      std::string trunc_item = current_progress_->item_name;
      const float max_item_w = std::max(40.0f * s, count_x0 - p_text_x0 - 8.0f * s);
      while (!trunc_item.empty() && bold->CalcTextSizeA(item_font_sz, FLT_MAX, 0.0f, trunc_item.c_str()).x > max_item_w) {
        trunc_item.pop_back();
      }
      dl->AddText(bold, item_font_sz,
                  ImVec2(p_text_x0, p_card_y0 + 10.0f * s),
                  IM_COL32(250, 252, 252, static_cast<int>(255 * p_alpha)), trunc_item.c_str());

      // Line 2: Progress bar
      const float bar_y0 = p_card_y0 + 32.0f * s;
      const float bar_y1 = bar_y0 + 6.0f * s;
      const float bar_w = p_text_x1 - p_text_x0;
      const float frac = current_progress_->total_count > 0
                             ? std::clamp(static_cast<float>(current_progress_->current_count) /
                                              static_cast<float>(current_progress_->total_count),
                                          0.0f, 1.0f)
                             : 0.0f;
      // Track
      dl->AddRectFilled(ImVec2(p_text_x0, bar_y0), ImVec2(p_text_x1, bar_y1),
                        IM_COL32(32, 40, 44, static_cast<int>(220 * p_alpha)), 3.0f * s);
      // Fill
      if (frac > 0.0f) {
        dl->AddRectFilled(ImVec2(p_text_x0, bar_y0), ImVec2(p_text_x0 + bar_w * frac, bar_y1),
                          IM_COL32(213, 235, 10, static_cast<int>(255 * p_alpha)), 3.0f * s);
      }

      // Line 3: Parent achievement name
      const float parent_font_sz = 12.0f * s;
      dl->AddText(reg, parent_font_sz,
                  ImVec2(p_text_x0, p_card_y0 + 46.0f * s),
                  IM_COL32(180, 190, 195, static_cast<int>(235 * p_alpha)), current_progress_->parent_name.c_str());
    }
  }
}

// ============================================================================
// AchievementManager
// ============================================================================

AchievementManager::AchievementManager() {
  instance_ = this;
  LoadDefinitions();
}

AchievementManager::~AchievementManager() {
  if (instance_ == this) {
    instance_ = nullptr;
  }
}

void AchievementManager::Initialize(rex::system::KernelState* kernel_state,
                                    const std::filesystem::path& profiles_path,
                                    AchievementToastDialog* toast_dialog,
                                    rex::ui::ImGuiDrawer* imgui_drawer) {
  std::lock_guard lock(mutex_);
  kernel_state_ = kernel_state;
  profiles_path_ = profiles_path;
  toast_dialog_ = toast_dialog;
  imgui_drawer_ = imgui_drawer;

  g_local_skater_ptr.store(0, std::memory_order_relaxed);
  g_has_local_phys_pos.store(false, std::memory_order_relaxed);
  if (kernel_state != nullptr) {
    // In the live game, require the retail HUD local-human gameplay gate
    // (*(uint8_t*)(hud_obj + 215) != 0) before evaluating achievements.
    g_has_real_hud_gate.store(true, std::memory_order_relaxed);
    g_local_gameplay_active.store(false, std::memory_order_relaxed);
  } else {
    g_has_real_hud_gate.store(false, std::memory_order_relaxed);
    g_local_gameplay_active.store(true, std::memory_order_relaxed);
  }

  if (toast_dialog_) {
    toast_dialog_->SetAchievementManager(this);
  }

  LoadTitleUpdateDatabase();
  LoadDefinitions();
  ReloadForActiveProfile();
}

void AchievementManager::LoadTitleUpdateDatabase() {
  spa_db_.reset();
  spa_data_.clear();

  std::vector<std::filesystem::path> search_roots;
  if (!profiles_path_.empty()) {
    std::filesystem::path p = profiles_path_;
    for (int i = 0; i < 4 && p.has_parent_path(); ++i) {
      p = p.parent_path();
      search_roots.push_back(p);
    }
  }

  search_roots.push_back(std::filesystem::current_path() / "game");
  search_roots.push_back(std::filesystem::current_path());

  std::vector<std::filesystem::path> spa_candidates;
  std::error_code ec;

  for (const auto& root : search_roots) {
    if (!std::filesystem::exists(root, ec) || !std::filesystem::is_directory(root, ec)) {
      continue;
    }

    const auto tu_dir = root / "0000000000000000" / "454108E6" / "00000002";
    if (std::filesystem::exists(tu_dir, ec) && std::filesystem::is_directory(tu_dir, ec)) {
      for (const auto& entry : std::filesystem::recursive_directory_iterator(tu_dir, ec)) {
        if (entry.is_regular_file(ec) && entry.path().filename() == "spa.bin") {
          spa_candidates.push_back(entry.path());
        }
      }
    }

    const auto direct_spa = root / "spa.bin";
    if (std::filesystem::is_regular_file(direct_spa, ec)) {
      spa_candidates.push_back(direct_spa);
    }
  }

  if (spa_candidates.empty()) {
    return;
  }

  auto best_it = std::max_element(
      spa_candidates.begin(), spa_candidates.end(),
      [](const std::filesystem::path& a, const std::filesystem::path& b) {
        std::error_code e1, e2;
        return std::filesystem::file_size(a, e1) < std::filesystem::file_size(b, e2);
      });

  if (best_it == spa_candidates.end()) {
    return;
  }

  std::ifstream f(*best_it, std::ios::binary | std::ios::ate);
  if (!f.is_open()) {
    return;
  }

  const std::streamsize size = f.tellg();
  if (size <= 0) {
    return;
  }
  f.seekg(0, std::ios::beg);

  spa_data_.resize(static_cast<size_t>(size));
  if (!f.read(reinterpret_cast<char*>(spa_data_.data()), size)) {
    spa_data_.clear();
    return;
  }

  spa_db_ = std::make_unique<rex::system::util::XdbfWrapper>(spa_data_.data(), spa_data_.size());
  if (spa_db_->is_valid()) {
    REXLOG_INFO("Skate 3 Achievements: Loaded title update database ({} bytes) from {}",
                spa_data_.size(), best_it->string());
  } else {
    spa_db_.reset();
    spa_data_.clear();
  }
}

rex::ui::ImmediateDrawer* AchievementManager::GetImmediateDrawer() const {
  if (imgui_drawer_ && imgui_drawer_->immediate_drawer()) {
    return imgui_drawer_->immediate_drawer();
  }
  if (toast_dialog_ && toast_dialog_->imgui_drawer() &&
      toast_dialog_->imgui_drawer()->immediate_drawer()) {
    return toast_dialog_->imgui_drawer()->immediate_drawer();
  }
  return nullptr;
}

void AchievementManager::LoadDefinitions() {
  definitions_.clear();
  id_to_index_.clear();
  key_to_id_.clear();

  for (auto def : kStaticAchievements) {
    if (def.key.empty()) {
      def.key = std::to_string(def.id);
    }
    key_to_id_[def.key] = def.id;
    key_to_id_[std::to_string(def.id)] = def.id;
    id_to_index_[def.id] = definitions_.size();
    definitions_.push_back(std::move(def));
  }

  const auto lang_id = static_cast<rex::system::XLanguage>(REXCVAR_GET(user_language));

  if (kernel_state_) {
    const rex::system::util::XdbfGameData db = kernel_state_->title_xdbf();
    if (db.is_valid()) {
      const rex::system::XLanguage language = db.GetExistingLanguage(lang_id);
      const auto achievement_list = db.GetAchievements();
      for (const auto& entry : achievement_list) {
        uint32_t id = entry.id;
        auto it = id_to_index_.find(id);
        if (it != id_to_index_.end()) {
          auto& def = definitions_[it->second];
          std::string name = db.GetStringTableEntry(language, entry.label_id);
          if (!name.empty()) def.name = std::move(name);
          std::string desc = db.GetStringTableEntry(language, entry.description_id);
          if (!desc.empty()) def.description = std::move(desc);
          std::string unach = db.GetStringTableEntry(language, entry.unachieved_id);
          if (!unach.empty()) def.unachieved_desc = std::move(unach);
          def.gamerscore = entry.gamerscore;
          def.is_secret = (entry.flags & 0x8) == 0;
        }
      }
    }
  }

  if (spa_db_ && spa_db_->is_valid()) {
    const auto achievement_list = spa_db_->GetAchievements();
    for (const auto& entry : achievement_list) {
      uint32_t id = entry.id;
      auto it = id_to_index_.find(id);
      if (it != id_to_index_.end()) {
        auto& def = definitions_[it->second];
        std::string name = spa_db_->GetStringTableEntry(lang_id, entry.label_id);
        if (!name.empty()) def.name = std::move(name);
        std::string desc = spa_db_->GetStringTableEntry(lang_id, entry.description_id);
        if (!desc.empty()) def.description = std::move(desc);
        std::string unach = spa_db_->GetStringTableEntry(lang_id, entry.unachieved_id);
        if (!unach.empty()) def.unachieved_desc = std::move(unach);
        def.gamerscore = entry.gamerscore;
        def.is_secret = (entry.flags & 0x8) == 0;
      }
    }
  }

  LoadCustomPackAchievements();
}

void AchievementManager::LoadCustomPackAchievements() {
  const auto app_root = rex::filesystem::GetAppRootFolder();
  const auto cwd = std::filesystem::current_path();
  std::vector<std::filesystem::path> candidate_pack_dirs;
  if (!profiles_path_.empty() && std::filesystem::is_directory(profiles_path_.parent_path() / "mods" / "raw")) {
    candidate_pack_dirs.push_back(profiles_path_.parent_path() / "mods" / "raw");
  }
  if (app_root.filename() == "build" || app_root.filename() == "build-win") {
    candidate_pack_dirs.push_back(app_root.parent_path() / "mods" / "raw");
  }
  candidate_pack_dirs.push_back(app_root / "mods" / "raw");
  candidate_pack_dirs.push_back(app_root.parent_path() / "mods" / "raw");
  candidate_pack_dirs.push_back(cwd / "mods" / "raw");
  candidate_pack_dirs.push_back(cwd.parent_path() / "mods" / "raw");
  candidate_pack_dirs.push_back(app_root / "custom_packs");
  candidate_pack_dirs.push_back(app_root.parent_path() / "custom_packs");
  candidate_pack_dirs.push_back(app_root.parent_path() / "build" / "custom_packs");
  candidate_pack_dirs.push_back(app_root / "build" / "custom_packs");

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

  std::error_code ec;
  for (const auto& entry : std::filesystem::directory_iterator(packs_dir, ec)) {
    if (!entry.is_directory()) continue;
    const std::string folder_name = entry.path().filename().string();
    if (folder_name.ends_with(".disabled") || folder_name.ends_with(".off") ||
        folder_name.ends_with(".bak") || folder_name.ends_with(".tmp")) {
      continue;
    }

    std::filesystem::path ach_file = entry.path() / "achievements.json";
    std::filesystem::path pack_file = entry.path() / "pack.json";
    nlohmann::json ach_array = nlohmann::json::array();
    std::string pack_category = folder_name;
    bool pack_enabled = true;

    if (std::filesystem::exists(ach_file)) {
      try {
        std::ifstream f(ach_file);
        auto j = nlohmann::json::parse(f, nullptr, false);
        if (j.is_object()) {
          pack_category = j.value("category", j.value("name", folder_name));
          pack_enabled = j.value("enabled", true);
          if (j.contains("achievements") && j["achievements"].is_array()) {
            ach_array = j["achievements"];
          }
        } else if (j.is_array()) {
          ach_array = j;
        }
      } catch (...) {}
    } else if (std::filesystem::exists(pack_file)) {
      try {
        std::ifstream f(pack_file);
        auto j = nlohmann::json::parse(f, nullptr, false);
        if (j.is_object()) {
          pack_category = j.value("category", j.value("name", folder_name));
          pack_enabled = j.value("enabled", true);
          if (j.contains("achievements") && j["achievements"].is_array()) {
            ach_array = j["achievements"];
          }
        }
      } catch (...) {}
    }

    if (!pack_enabled || ach_array.empty()) {
      continue;
    }

    std::vector<AchievementDef> pack_defs;
    pack_defs.reserve(ach_array.size());

    for (const auto& a : ach_array) {
      if (!a.is_object()) continue;

      std::string local_id;
      if (a.contains("id")) {
        if (a["id"].is_string()) {
          local_id = a["id"].get<std::string>();
        } else if (a["id"].is_number()) {
          local_id = std::to_string(a["id"].get<uint32_t>());
        }
      }
      if (local_id.empty()) {
        local_id = a.value("name", fmt::format("ach_{}", definitions_.size() + pack_defs.size()));
      }

      const std::string full_key = fmt::format("{}:{}", pack_category, local_id);
      uint32_t id = 0x80000000u | (Fnv1a32(full_key) & 0x7FFFFFFFu);
      if (a.contains("retail_id") && a["retail_id"].is_number_unsigned()) {
        const uint32_t retail_id = a["retail_id"].get<uint32_t>();
        if (retail_id > 0 && id_to_index_.find(retail_id) == id_to_index_.end()) {
          id = retail_id;
        }
      }

      AchievementDef def;
      def.id = id;
      def.key = full_key;
      def.name = a.value("name", fmt::format("Mod Achievement ({})", local_id));
      def.description = a.value("description", "");
      def.unachieved_desc = a.value("unachieved_desc", def.description);
      def.category = a.value("category", pack_category);
      const uint32_t raw_score = a.value("gamerscore", 0u);
      // Hard cap on any single achievement at 500 points
      def.gamerscore = std::min(raw_score, 500u);
      def.is_secret = a.value("is_secret", false);
      def.trigger_type = a.value("trigger", "");
      def.trigger_target = a.value("spot", a.value("target", a.value("group", a.value("code", ""))));
      def.trigger_threshold = a.value("threshold", a.value("count", a.value("score", 0.0f)));
      def.trigger_radius = a.value("radius", 5.0f);

      if (a.contains("tricks") && a["tricks"].is_array()) {
        for (const auto& t_item : a["tricks"]) {
          if (t_item.is_string()) {
            const std::string s = t_item.get<std::string>();
            if (!s.empty()) {
              def.required_tricks.push_back(s);
            }
          }
        }
      } else if (def.trigger_type == "trick_group" || def.trigger_type == "trick_checklist") {
        def.required_tricks = GetTrickGroupTricks(def.trigger_target);
      }

      if (a.contains("position") && a["position"].is_array() && a["position"].size() >= 3) {
        def.trigger_pos[0] = a["position"][0].get<float>();
        def.trigger_pos[1] = a["position"][1].get<float>();
        def.trigger_pos[2] = a["position"][2].get<float>();
      }
      if (a.contains("takeoff") && a["takeoff"].is_array() && a["takeoff"].size() >= 3) {
        def.trigger_pos[0] = a["takeoff"][0].get<float>();
        def.trigger_pos[1] = a["takeoff"][1].get<float>();
        def.trigger_pos[2] = a["takeoff"][2].get<float>();
      }
      if (a.contains("landing") && a["landing"].is_array() && a["landing"].size() >= 3) {
        def.trigger_pos2[0] = a["landing"][0].get<float>();
        def.trigger_pos2[1] = a["landing"][1].get<float>();
        def.trigger_pos2[2] = a["landing"][2].get<float>();
      }

      if (a.contains("icon") && a["icon"].is_string()) {
        std::filesystem::path icon_p = entry.path() / a["icon"].get<std::string>();
        if (std::filesystem::exists(icon_p)) {
          def.custom_icon_path = icon_p;
        }
      }

      pack_defs.push_back(std::move(def));
    }

    // Scale pack achievements if total exceeds 1000 points
    uint32_t total_pack_score = 0;
    for (const auto& d : pack_defs) {
      total_pack_score += d.gamerscore;
    }

    if (total_pack_score > 1000) {
      const double factor = 1000.0 / static_cast<double>(total_pack_score);
      uint32_t scaled_total = 0;
      for (auto& d : pack_defs) {
        if (d.gamerscore > 0) {
          d.gamerscore = std::max(1u, static_cast<uint32_t>(std::round(d.gamerscore * factor)));
        }
        scaled_total += d.gamerscore;
      }
      while (scaled_total > 1000) {
        auto max_it = std::max_element(pack_defs.begin(), pack_defs.end(),
            [](const AchievementDef& a, const AchievementDef& b) {
              return a.gamerscore < b.gamerscore;
            });
        if (max_it != pack_defs.end() && max_it->gamerscore > 1) {
          max_it->gamerscore--;
          scaled_total--;
        } else {
          break;
        }
      }
      while (scaled_total < 1000) {
        auto max_it = std::max_element(pack_defs.begin(), pack_defs.end(),
            [](const AchievementDef& a, const AchievementDef& b) {
              return a.gamerscore < b.gamerscore;
            });
        if (max_it != pack_defs.end()) {
          max_it->gamerscore++;
          scaled_total++;
        } else {
          break;
        }
      }
    }

    for (auto& def : pack_defs) {
      const uint32_t id = def.id;
      const std::string full_key = def.key;
      key_to_id_[full_key] = id;
      key_to_id_[std::to_string(id)] = id;

      auto it = id_to_index_.find(id);
      if (it == id_to_index_.end()) {
        id_to_index_[id] = definitions_.size();
        definitions_.push_back(std::move(def));
      } else {
        definitions_[it->second] = std::move(def);
      }
    }
  }
}

std::string AchievementManager::GetCurrentProfileId() const {
  if (profiles_path_.empty()) return "default";
  try {
    const auto store = skate3::LoadProfiles(profiles_path_);
    if (!store.selected_profile.empty()) {
      return store.selected_profile;
    }
  } catch (...) {
  }
  return "default";
}

std::filesystem::path AchievementManager::GetAchievementFilePath(const std::string& profile_id) const {
  if (profiles_path_.empty()) return {};
  const auto base_dir = profiles_path_.parent_path();
  const auto nested_file = base_dir / "profiles" / fmt::format("{}_achievements.json", profile_id);
  if (std::filesystem::exists(nested_file)) {
    return nested_file;
  }
  std::filesystem::path target_dir;
  if (base_dir.filename() == "profiles") {
    target_dir = base_dir;
  } else {
    target_dir = base_dir / "profiles";
  }
  std::error_code ec;
  std::filesystem::create_directories(target_dir, ec);
  return target_dir / fmt::format("{}_achievements.json", profile_id);
}

void AchievementManager::LoadAchievementsForProfile(const std::string& profile_id) {
  records_.clear();
  for (const auto& def : definitions_) {
    records_[def.id] = {false, 0, {}, 0.0f};
  }

  const auto path = GetAchievementFilePath(profile_id);
  if (path.empty() || !std::filesystem::exists(path)) {
    return;
  }

  try {
    std::ifstream f(path);
    const auto j = nlohmann::json::parse(f, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
      return;
    }
    if (j.contains("achievements") && j["achievements"].is_object()) {
      for (const auto& [key, val] : j["achievements"].items()) {
        try {
          uint32_t id = 0;
          auto it_k = key_to_id_.find(key);
          if (it_k != key_to_id_.end()) {
            id = it_k->second;
          } else {
            id = static_cast<uint32_t>(std::stoul(key));
          }
          if (id != 0 && val.is_object()) {
            AchievementRecord rec;
            rec.unlocked = val.value("unlocked", false);
            rec.unlock_time = val.value("unlock_time", uint64_t(0));
            rec.best_stat = val.value("best_stat", 0.0f);
            if (val.contains("completed_tricks") && val["completed_tricks"].is_array()) {
              for (const auto& ct : val["completed_tricks"]) {
                if (ct.is_string()) {
                  rec.completed_tricks.push_back(ct.get<std::string>());
                }
              }
            }
            records_[id] = std::move(rec);
          }
        } catch (...) {
        }
      }
    }
  } catch (...) {
  }
}

void AchievementManager::SaveAchievements() {
  const auto path = GetAchievementFilePath(active_profile_id_);
  if (path.empty()) return;

  try {
    nlohmann::json root;
    nlohmann::json ach_obj = nlohmann::json::object();

    if (std::filesystem::exists(path)) {
      std::ifstream in_f(path);
      auto existing = nlohmann::json::parse(in_f, nullptr, false);
      if (existing.is_object() && existing.contains("achievements") && existing["achievements"].is_object()) {
        ach_obj = existing["achievements"];
      }
    }

    root["profile"] = active_profile_id_;
    for (const auto& def : definitions_) {
      const std::string key = def.key.empty() ? std::to_string(def.id) : def.key;
      auto it = records_.find(def.id);
      if (it != records_.end()) {
        const auto& rec = it->second;
        if (rec.unlocked || !rec.completed_tricks.empty() || rec.best_stat > 0.0f) {
          nlohmann::json item;
          item["unlocked"] = rec.unlocked;
          item["unlock_time"] = rec.unlock_time;
          if (!rec.completed_tricks.empty()) {
            item["completed_tricks"] = rec.completed_tricks;
          }
          if (rec.best_stat > 0.0f) {
            item["best_stat"] = rec.best_stat;
          }
          ach_obj[key] = std::move(item);
        } else {
          ach_obj.erase(key);
          ach_obj.erase(std::to_string(def.id));
        }
      }
    }
    root["achievements"] = ach_obj;

    std::ofstream f(path);
    f << root.dump(2) << std::endl;
  } catch (...) {
  }
}

void AchievementManager::ReloadForActiveProfile() {
  active_profile_id_ = GetCurrentProfileId();
  LoadAchievementsForProfile(active_profile_id_);
}

void AchievementManager::ReloadDefinitions() {
  std::lock_guard lock(mutex_);
  LoadDefinitions();
  active_profile_id_ = GetCurrentProfileId();
  LoadAchievementsForProfile(active_profile_id_);
}

bool AchievementManager::Unlock(uint32_t user_index, std::string_view achievement_key) {
  std::lock_guard lock(mutex_);
  const std::string key_str(achievement_key);
  auto it = key_to_id_.find(key_str);
  if (it != key_to_id_.end()) {
    return UnlockInternal(user_index, it->second);
  }
  try {
    uint32_t id = static_cast<uint32_t>(std::stoul(key_str));
    return UnlockInternal(user_index, id);
  } catch (...) {
  }
  return false;
}

bool AchievementManager::Unlock(uint32_t user_index, uint32_t achievement_id) {
  std::lock_guard lock(mutex_);
  return UnlockInternal(user_index, achievement_id);
}

bool AchievementManager::UnlockInternal(uint32_t user_index, uint32_t achievement_id) {
  if (achievement_id == 0) return false;

  const auto it = id_to_index_.find(achievement_id);
  if (it == id_to_index_.end()) {
    return false;
  }

  const std::string current_profile = GetCurrentProfileId();
  if (current_profile != active_profile_id_) {
    active_profile_id_ = current_profile;
    LoadAchievementsForProfile(active_profile_id_);
  }

  auto& rec = records_[achievement_id];
  if (rec.unlocked) {
    return false;
  }

  rec.unlocked = true;
  rec.unlock_time = static_cast<uint64_t>(std::time(nullptr));
  SaveAchievements();

  const auto& def = definitions_[it->second];
  const std::string& ach_name = def.name;
  const std::string& ach_desc = def.description;
  const uint32_t gamerscore = def.gamerscore;

  REXLOG_INFO("Skate 3 Achievements: Unlocked #{} '{}' ({} G)", achievement_id, ach_name, gamerscore);

  if (toast_dialog_) {
    toast_dialog_->PostToast({achievement_id, ach_name, ach_desc, gamerscore});
  }

  return true;
}

bool AchievementManager::IsUnlocked(uint32_t user_index, std::string_view achievement_key) const {
  std::lock_guard lock(mutex_);
  const std::string key_str(achievement_key);
  auto it = key_to_id_.find(key_str);
  if (it != key_to_id_.end()) {
    auto it_rec = records_.find(it->second);
    if (it_rec != records_.end()) {
      return it_rec->second.unlocked;
    }
  }
  try {
    uint32_t id = static_cast<uint32_t>(std::stoul(key_str));
    auto it_rec = records_.find(id);
    if (it_rec != records_.end()) {
      return it_rec->second.unlocked;
    }
  } catch (...) {
  }
  return false;
}

bool AchievementManager::IsUnlocked(uint32_t user_index, uint32_t achievement_id) const {
  std::lock_guard lock(mutex_);
  const auto it = records_.find(achievement_id);
  if (it != records_.end()) {
    return it->second.unlocked;
  }
  return false;
}

void AchievementManager::Update(float dt) {
  dt = std::clamp(dt, 0.001f, 0.5f);

  float pos[3];
  if (!IsLocalGameplayActive() || !GetActiveLocalSkaterPosition(pos)) {
    std::lock_guard lock(mutex_);
    has_prev_pos_ = false;
    return;
  }

  std::lock_guard lock(mutex_);

  float speed_mph = 0.0f;
  if (has_prev_pos_) {
    const float dx = pos[0] - prev_pos_[0];
    const float dy = pos[1] - prev_pos_[1];
    const float dz = pos[2] - prev_pos_[2];
    const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
    const float speed_mps = dist / dt;
    // Filter out session marker warps and teleports (> 55 m/s = ~123 mph)
    if (speed_mps <= 55.0f) {
      speed_mph = speed_mps * 2.23694f;
    }
  }

  prev_pos_[0] = pos[0];
  prev_pos_[1] = pos[1];
  prev_pos_[2] = pos[2];
  has_prev_pos_ = true;

  const auto now = std::chrono::steady_clock::now();
  const bool bailed_or_off = IsSkaterBailedOrOffBoard();
  const bool airborne = IsSkaterOnBoardAirborne();

  // Check base game 107 (Terminal Velocity - 60 mph)
  if (!bailed_or_off && speed_mph >= 60.0f) {
    UnlockInternal(0, 107);
  }

  // Broad-zone fallback for 105 (The Mega Drop) and 106 (Dam Buster)
  {
    // 105: Super Ultra Mega Park roll-in or Observatory Mega Ramp jump
    auto it105 = records_.find(105);
    if (it105 == records_.end() || !it105->second.unlocked) {
      static bool s_mega_active = false;
      static bool s_mega_airborne = false;
      static float s_mega_takeoff_y = 0.0f;
      static std::chrono::steady_clock::time_point s_mega_time{};
      if (bailed_or_off) {
        s_mega_active = false;
        s_mega_airborne = false;
      } else {
        const bool in_super_mega_top =
            (pos[0] >= 295.0f && pos[0] <= 365.0f && pos[1] >= 134.0f && pos[1] <= 148.0f &&
             pos[2] >= -765.0f && pos[2] <= -715.0f);
        const bool in_obs_mega_top =
            (pos[0] >= -460.0f && pos[0] <= -340.0f && pos[1] >= 210.0f && pos[1] <= 248.0f &&
             pos[2] >= -900.0f && pos[2] <= -790.0f);
        if (in_super_mega_top || in_obs_mega_top) {
          s_mega_active = true;
          s_mega_takeoff_y = std::max(s_mega_takeoff_y, pos[1]);
          s_mega_time = now;
          if (airborne) s_mega_airborne = true;
        } else if (s_mega_active) {
          const float elapsed = std::chrono::duration<float>(now - s_mega_time).count();
          if (elapsed > 10.0f) {
            s_mega_active = false;
            s_mega_airborne = false;
            s_mega_takeoff_y = 0.0f;
          } else {
            if (airborne) s_mega_airborne = true;
            if (s_mega_airborne && !airborne && elapsed >= 0.15f &&
                pos[1] <= s_mega_takeoff_y - 5.0f) {
              UnlockInternal(0, 105);
              s_mega_active = false;
              s_mega_airborne = false;
              s_mega_takeoff_y = 0.0f;
            }
          }
        }
      }
    }

    // 106: San Van Spillway Dam face drop
    auto it106 = records_.find(106);
    if (it106 == records_.end() || !it106->second.unlocked) {
      static bool s_dam_active = false;
      static float s_dam_crest_y = 0.0f;
      static std::chrono::steady_clock::time_point s_dam_time{};
      if (bailed_or_off) {
        s_dam_active = false;
        s_dam_crest_y = 0.0f;
      } else {
        const bool on_dam_crest =
            (pos[0] >= 420.0f && pos[0] <= 575.0f && pos[1] >= 98.0f && pos[1] <= 115.0f &&
             pos[2] >= -515.0f && pos[2] <= -435.0f) ||
            (pos[0] >= 740.0f && pos[0] <= 805.0f && pos[1] >= 134.0f && pos[1] <= 148.0f &&
             pos[2] >= -445.0f && pos[2] <= -375.0f);
        if (on_dam_crest) {
          s_dam_active = true;
          s_dam_crest_y = std::max(s_dam_crest_y, pos[1]);
          s_dam_time = now;
        } else if (s_dam_active) {
          const float elapsed = std::chrono::duration<float>(now - s_dam_time).count();
          if (elapsed > 12.0f) {
            s_dam_active = false;
            s_dam_crest_y = 0.0f;
          } else if (!airborne && elapsed >= 0.15f && pos[1] <= s_dam_crest_y - 8.0f) {
            UnlockInternal(0, 106);
            s_dam_active = false;
            s_dam_crest_y = 0.0f;
          }
        }
      }
    }
  }

  for (const auto& def : definitions_) {
    if (def.trigger_type.empty()) continue;

    auto it_rec = records_.find(def.id);
    if (it_rec != records_.end() && it_rec->second.unlocked) {
      continue;
    }

    if (def.trigger_type == "speed") {
      if (!bailed_or_off && speed_mph >= def.trigger_threshold) {
        UnlockInternal(0, def.id);
      }
    } else if (def.trigger_type == "reach_location") {
      const float dx = pos[0] - def.trigger_pos[0];
      const float dy = pos[1] - def.trigger_pos[1];
      const float dz = pos[2] - def.trigger_pos[2];
      const float dist_sq = dx * dx + dy * dy + dz * dz;
      const float rad = def.trigger_radius > 0.0f ? def.trigger_radius : 10.0f;
      if (dist_sq <= rad * rad) {
        UnlockInternal(0, def.id);
      }
    } else if (def.trigger_type == "gap") {
      const float t_dx = pos[0] - def.trigger_pos[0];
      const float t_dy = pos[1] - def.trigger_pos[1];
      const float t_dz = pos[2] - def.trigger_pos[2];
      const float t_dist_sq = t_dx * t_dx + t_dy * t_dy + t_dz * t_dz;
      const float rad = def.trigger_radius > 0.0f ? def.trigger_radius : 6.0f;

      float l_dx = pos[0] - def.trigger_pos2[0];
      if (std::fabs(def.trigger_pos2[0] - 415.0f) < 1.0f) {
        const float alt_dx = pos[0] - 145.0f;
        if (std::fabs(alt_dx) < std::fabs(l_dx)) {
          l_dx = alt_dx;
        }
      }
      const float l_dy = pos[1] - def.trigger_pos2[1];
      const float l_dz = pos[2] - def.trigger_pos2[2];
      const float l_dist_sq = l_dx * l_dx + l_dy * l_dy + l_dz * l_dz;

      auto& gstate = gap_states_[def.id];
      if (bailed_or_off) {
        gstate.active = false;
        gstate.was_airborne = false;
        continue;
      }

      const bool in_air_or_grind = IsSkaterAirborneOrGrinding() || airborne;

      if (t_dist_sq <= rad * rad) {
        if (in_air_or_grind) {
          gstate.takeoff_time = now;
          gstate.active = true;
          gstate.was_airborne = true;
        }
      } else if (gstate.active) {
        const float elapsed = std::chrono::duration<float>(now - gstate.takeoff_time).count();
        if (elapsed > 12.0f) {
          gstate.active = false;
          gstate.was_airborne = false;
          continue;
        }
        // Between takeoff and landing zones, player must remain off the ground (in air, grinding, or tricking)
        if (l_dist_sq > rad * rad && !in_air_or_grind) {
          gstate.active = false;
          gstate.was_airborne = false;
          continue;
        }
      }

      // Check landing zone: reached end zone while active, elapsed >= 0.15s, cleanly on board
      if (gstate.active && gstate.was_airborne && l_dist_sq <= rad * rad && t_dist_sq > rad * rad) {
        const float elapsed = std::chrono::duration<float>(now - gstate.takeoff_time).count();
        if (elapsed >= 0.15f && elapsed <= 12.0f && !bailed_or_off) {
          UnlockInternal(0, def.id);
          gstate.active = false;
          gstate.was_airborne = false;
        }
      }
    }
  }
}

bool AchievementManager::HasActiveTelemetryTriggers() const {
  std::lock_guard lock(mutex_);
  auto it107 = records_.find(107);
  if (it107 == records_.end() || !it107->second.unlocked) {
    return true;
  }
  for (const auto& def : definitions_) {
    if (def.trigger_type.empty()) continue;
    auto it = records_.find(def.id);
    if (it != records_.end() && it->second.unlocked) continue;
    if (def.trigger_type == "speed" || def.trigger_type == "reach_location" ||
        def.trigger_type == "gap" || def.trigger_type == "air_time") {
      return true;
    }
  }
  return false;
}

void AchievementManager::OnSpotCompleted(const std::string& spot_key, bool killed) {
  std::lock_guard lock(mutex_);
  const std::string norm_spot = NormalizeAlnumLower(spot_key);

  // Track 16 base Own The Spot locations for 112 ("King of the Streets")
  if (!norm_spot.empty() && norm_spot != "any") {
    auto& rec112 = records_[112];
    if (!rec112.unlocked) {
      if (std::find(rec112.completed_tricks.begin(), rec112.completed_tricks.end(), norm_spot) ==
          rec112.completed_tricks.end()) {
        rec112.completed_tricks.push_back(norm_spot);
        SaveAchievements();
        if (rec112.completed_tricks.size() < 16) {
          if (toast_dialog_) {
            toast_dialog_->PostProgressToast(
                {112, killed ? "Spot Killed" : "Spot Owned", "King of the Streets",
                 static_cast<uint32_t>(rec112.completed_tricks.size()), 16});
          }
        }
      }
      if (rec112.completed_tricks.size() >= 16) {
        UnlockInternal(0, 112);
      }
    }
  }

  for (const auto& def : definitions_) {
    if (def.trigger_type.empty()) continue;
    const std::string norm_target = NormalizeAlnumLower(def.trigger_target);
    const bool matches_spot = (def.trigger_target == "any" || def.trigger_target.empty() ||
                               def.trigger_target == spot_key ||
                               (!norm_target.empty() && norm_target == norm_spot));
    if (!matches_spot) continue;

    if (def.trigger_type == "kill_spot") {
      if (killed) {
        UnlockInternal(0, def.id);
      }
    } else if (def.trigger_type == "own_spot" || def.trigger_type == "complete_spot") {
      UnlockInternal(0, def.id);
    }
  }
}

void AchievementManager::OnStatUpdated(const std::string& stat_name, float value) {
  std::lock_guard lock(mutex_);

  // Base game challenges
  if ((stat_name == "line_score" || stat_name == "score") && value >= 30000.0f) {
    UnlockInternal(0, 7);
  }
  if (stat_name == "trick_score" && value >= 10000.0f) {
    UnlockInternal(0, 8);
  }
  if (stat_name == "bail_score" && value >= 75000.0f) {
    UnlockInternal(0, 9);
  }
  if ((stat_name == "moveable_objects" || stat_name == "objects") && value >= 3.0f) {
    UnlockInternal(0, 10);
  }
  if ((stat_name == "grind_distance" || stat_name == "grind") && value >= 100.0f) {
    UnlockInternal(0, 11);
  }
  if ((stat_name == "manuals_chained" || stat_name == "manuals") && value >= 3.0f) {
    UnlockInternal(0, 12);
  }
  if (stat_name == "board_sales" && value >= 1000000.0f) {
    UnlockInternal(0, 22);
  }
  if ((stat_name == "speed" || stat_name == "top_speed") && value >= 60.0f) {
    UnlockInternal(0, 107);
  }
  if ((stat_name == "bones_broken" || stat_name == "bones") && value >= 6.0f) {
    UnlockInternal(0, 108);
  }
  if ((stat_name == "manual_distance" || stat_name == "manual") && value >= 100.0f) {
    UnlockInternal(0, 109);
  }
  if ((stat_name == "park_objects" || stat_name == "placed_objects") && value >= 25.0f) {
    UnlockInternal(0, 110);
  }
  if ((stat_name == "tricks_chained" || stat_name == "combo") && value >= 10.0f) {
    UnlockInternal(0, 111);
  }

  // Custom pack triggers
  for (const auto& def : definitions_) {
    if (def.trigger_type.empty()) continue;

    bool match = false;
    if (def.trigger_type == stat_name) {
      match = true;
    } else if (def.trigger_type == "grind_distance" && stat_name == "grind") {
      match = true;
    } else if (def.trigger_type == "manual_distance" && stat_name == "manual") {
      match = true;
    } else if (def.trigger_type == "line_score" && stat_name == "score") {
      match = true;
    } else if (def.trigger_type == "tricks_chained" && stat_name == "combo") {
      match = true;
    } else if (def.trigger_type == "bones_broken" && stat_name == "bones") {
      match = true;
    } else if (def.trigger_type == "moveable_objects" && stat_name == "objects") {
      match = true;
    } else if (def.trigger_type == "park_objects" && stat_name == "placed_objects") {
      match = true;
    } else if (def.trigger_type == "speed" && (stat_name == "top_speed" || stat_name == "speed")) {
      match = true;
    }

    if (match) {
      auto& rec = records_[def.id];
      if (!rec.unlocked && value > rec.best_stat) {
        rec.best_stat = value;
      }
      if (value >= def.trigger_threshold) {
        UnlockInternal(0, def.id);
      }
    }
  }
}

void AchievementManager::OnTrickLanded(std::string_view trick_name) {
  std::lock_guard lock(mutex_);
  const std::string norm_trick = NormalizeAlnumLower(trick_name);
  if (norm_trick.empty() || norm_trick == "none") {
    return;
  }

  const std::vector<std::string> canonical_tricks = MapToCanonicalTricks(trick_name);

  if (trick_name == "Miracle Whip" || norm_trick == "miraclewhip") {
    UnlockInternal(0, 13);
  }

  bool dirty_progress = false;
  for (const auto& def : definitions_) {
    if (def.trigger_type == "trick_name") {
      if (def.trigger_target == "any" || def.trigger_target.empty()) {
        UnlockInternal(0, def.id);
        continue;
      }
      const std::string norm_target = NormalizeAlnumLower(def.trigger_target);
      if (norm_target.empty()) continue;

      bool match = (norm_trick == norm_target) ||
                   (norm_target == "treflip" && norm_trick == "360flip") ||
                   (norm_target == "360flip" && norm_trick == "treflip");
      if (!match) {
        for (const auto& canon : canonical_tricks) {
          if (NormalizeAlnumLower(canon) == norm_target) {
            match = true;
            break;
          }
        }
      }
      if (!match && canonical_tricks.empty() && norm_trick.find(norm_target) != std::string::npos) {
        match = true;
      }
      if (match) {
        UnlockInternal(0, def.id);
      }
    } else if ((def.trigger_type == "trick_group" || def.trigger_type == "trick_checklist") &&
               !def.required_tricks.empty()) {
      auto& rec = records_[def.id];
      if (rec.unlocked) continue;

      for (const auto& req : def.required_tricks) {
        const std::string norm_req = NormalizeAlnumLower(req);
        if (norm_req.empty()) continue;

        bool matched_req = (norm_trick == norm_req);
        if (!matched_req) {
          for (const auto& canon : canonical_tricks) {
            if (NormalizeAlnumLower(canon) == norm_req) {
              matched_req = true;
              break;
            }
          }
        }

        if (matched_req) {
          const bool already_done = std::any_of(
              rec.completed_tricks.begin(), rec.completed_tricks.end(),
              [&](const std::string& done_str) {
                return NormalizeAlnumLower(done_str) == norm_req;
              });
          if (!already_done) {
            rec.completed_tricks.push_back(req);
            dirty_progress = true;
            if (rec.completed_tricks.size() < def.required_tricks.size()) {
              if (toast_dialog_) {
                toast_dialog_->PostProgressToast(
                    {def.id, req, def.name, static_cast<uint32_t>(rec.completed_tricks.size()),
                     static_cast<uint32_t>(def.required_tricks.size())});
              }
            }
          }
        }
      }

      if (rec.completed_tricks.size() >= def.required_tricks.size()) {
        UnlockInternal(0, def.id);
        dirty_progress = false;  // UnlockInternal already saves
      }
    }
  }

  if (dirty_progress) {
    SaveAchievements();
  }
}

void AchievementManager::OnCheatEntered(std::string_view cheat_code) {
  std::lock_guard lock(mutex_);
  const std::string lower_code = NormalizeAlnumLower(cheat_code);
  if (lower_code.empty()) {
    return;
  }

  // Base game offline cheat code challenges
  if (lower_code == "deadspacetoo" || lower_code == "deadspace") {
    UnlockInternal(0, 101);
  } else if (lower_code == "mcfly") {
    UnlockInternal(0, 102);
  } else if (lower_code == "zombie") {
    UnlockInternal(0, 103);
  } else if (lower_code == "miniskaters") {
    UnlockInternal(0, 104);
  }

  // Custom pack cheat_code triggers
  for (const auto& def : definitions_) {
    if (def.trigger_type != "cheat_code") continue;
    if (def.trigger_target == "any" || def.trigger_target.empty()) {
      UnlockInternal(0, def.id);
    } else {
      const std::string target_lower = NormalizeAlnumLower(def.trigger_target);
      if (lower_code == target_lower) {
        UnlockInternal(0, def.id);
      }
    }
  }
}

void AchievementManager::ResetAchievements() {
  std::lock_guard lock(mutex_);
  for (auto& [id, record] : records_) {
    record.unlocked = false;
    record.unlock_time = 0;
    record.completed_tricks.clear();
    record.best_stat = 0.0f;
  }
  gap_states_.clear();
  has_prev_pos_ = false;
  g_reset_epoch.fetch_add(1, std::memory_order_relaxed);
  if (!g_has_real_hud_gate.load(std::memory_order_relaxed)) {
    g_local_skater_ptr.store(0, std::memory_order_relaxed);
  }
  if (toast_dialog_) {
    toast_dialog_->Clear();
  }

  const auto path = GetAchievementFilePath(active_profile_id_);
  if (!path.empty()) {
    try {
      nlohmann::json root;
      root["profile"] = active_profile_id_;
      root["achievements"] = nlohmann::json::object();
      std::ofstream f(path);
      f << root.dump(2) << std::endl;
    } catch (...) {
    }
  }
  REXLOG_INFO("Skate 3 Achievements: Reset all achievements for profile '{}'", active_profile_id_);
}

void AchievementManager::ResetAchievements(const std::vector<uint32_t>& achievement_ids) {
  std::lock_guard lock(mutex_);
  if (achievement_ids.empty()) {
    return;
  }
  for (uint32_t id : achievement_ids) {
    auto it = records_.find(id);
    if (it != records_.end()) {
      it->second.unlocked = false;
      it->second.unlock_time = 0;
      it->second.completed_tricks.clear();
      it->second.best_stat = 0.0f;
    }
    gap_states_.erase(id);
  }
  has_prev_pos_ = false;
  g_reset_epoch.fetch_add(1, std::memory_order_relaxed);
  SaveAchievements();
  REXLOG_INFO("Skate 3 Achievements: Reset {} filtered achievement(s) for profile '{}'",
              achievement_ids.size(), active_profile_id_);
}

rex::ui::SimpleAchievementData AchievementManager::GetAchievementData() const {
  std::lock_guard lock(mutex_);
  rex::ui::SimpleAchievementData data;
  data.total_count = static_cast<uint32_t>(definitions_.size());

  for (const auto& def : definitions_) {
    data.total_gamerscore += def.gamerscore;
    bool is_unlocked = false;
    uint64_t unlock_time = 0;
    const AchievementRecord* rec_ptr = nullptr;
    const auto it = records_.find(def.id);
    if (it != records_.end()) {
      rec_ptr = &it->second;
      if (it->second.unlocked) {
        is_unlocked = true;
        unlock_time = it->second.unlock_time;
        data.unlocked_count++;
        data.unlocked_gamerscore += def.gamerscore;
      }
    }

    rex::ui::SimpleAchievementItem item;
    item.id = def.id;
    item.name = def.name;
    item.description = def.description;
    item.unachieved_desc = def.unachieved_desc;
    item.category = def.category;
    item.gamerscore = def.gamerscore;
    item.is_secret = def.is_secret;
    item.unlocked = is_unlocked;
    if (is_unlocked && unlock_time != 0) {
      const std::time_t t = static_cast<std::time_t>(unlock_time);
      std::tm tm_buf{};
#if defined(_WIN32)
      localtime_s(&tm_buf, &t);
#else
      localtime_r(&t, &tm_buf);
#endif
      char time_str[64];
      std::strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M", &tm_buf);
      item.unlock_time_str = time_str;
    }

    if (!def.required_tricks.empty()) {
      item.progress_total = static_cast<uint32_t>(def.required_tricks.size());
      item.checklist.reserve(def.required_tricks.size());
      uint32_t done_count = 0;
      for (const auto& req : def.required_tricks) {
        bool done = is_unlocked;
        if (!done && rec_ptr) {
          const std::string norm_req = NormalizeAlnumLower(req);
          done = std::any_of(
              rec_ptr->completed_tricks.begin(), rec_ptr->completed_tricks.end(),
              [&](const std::string& s) { return NormalizeAlnumLower(s) == norm_req; });
        }
        if (done) {
          done_count++;
        }
        item.checklist.emplace_back(req, done);
      }
      item.progress_current = done_count;
    } else if (def.id == 112) {
      item.progress_total = 16;
      item.progress_current = is_unlocked
                                  ? 16u
                                  : (rec_ptr ? std::min<uint32_t>(
                                                   16u, static_cast<uint32_t>(
                                                            rec_ptr->completed_tricks.size()))
                                             : 0u);
    } else if (def.trigger_threshold >= 2.0f && def.trigger_type != "speed") {
      item.progress_total = static_cast<uint32_t>(std::round(def.trigger_threshold));
      item.progress_current =
          is_unlocked
              ? item.progress_total
              : (rec_ptr ? std::min(item.progress_total,
                                    static_cast<uint32_t>(std::floor(rec_ptr->best_stat)))
                         : 0u);
    }

    data.items.push_back(std::move(item));
  }
  return data;
}

void* AchievementManager::GetAchievementIcon(uint32_t achievement_id) {
  std::lock_guard lock(mutex_);
  if (achievement_id == 0) return nullptr;

  auto it = icon_textures_.find(achievement_id);
  if (it != icon_textures_.end()) {
    return it->second.get();
  }

  // 1. Check custom icon file path if defined
  auto it_def = id_to_index_.find(achievement_id);
  if (it_def != id_to_index_.end() && !definitions_[it_def->second].custom_icon_path.empty()) {
    const auto& icon_path = definitions_[it_def->second].custom_icon_path;
    SDL_IOStream* io = SDL_IOFromFile(icon_path.string().c_str(), "rb");
    if (io) {
      SDL_Surface* surface = SDL_LoadPNG_IO(io, true);
      if (surface) {
        SDL_Surface* rgba_surface = SDL_ConvertSurface(surface, SDL_PIXELFORMAT_RGBA32);
        SDL_DestroySurface(surface);
        if (rgba_surface) {
          rex::ui::ImmediateDrawer* imm = GetImmediateDrawer();
          if (imm) {
            auto texture = imm->CreateTexture(
                rgba_surface->w, rgba_surface->h,
                rex::ui::ImmediateTextureFilter::kLinear, false,
                static_cast<const uint8_t*>(rgba_surface->pixels));
            SDL_DestroySurface(rgba_surface);
            if (texture) {
              void* res = texture.get();
              icon_textures_[achievement_id] = std::move(texture);
              return res;
            }
          } else {
            SDL_DestroySurface(rgba_surface);
          }
        }
      }
    }
  }

  // Fallback for new offline challenges to thematic base game achievement icons
  uint32_t lookup_id = achievement_id;
  if (achievement_id >= 101 && achievement_id <= 112) {
    switch (achievement_id) {
      case 101: lookup_id = 38; break; // Isaac's Day Off -> Employee #1
      case 102: lookup_id = 2;  break; // Hover Conversion -> Park Apprentice
      case 103: lookup_id = 19; break; // Night of the Living Skaters -> Gone Viral (Skate Flu)
      case 104: lookup_id = 24; break; // Pocket Skater -> Making a Buddy Call
      case 105: lookup_id = 14; break; // The Mega Drop -> 100% Pure Adrenaline
      case 106: lookup_id = 43; break; // Dam Buster -> King of Kings
      case 107: lookup_id = 33; break; // Terminal Velocity -> Speed Demon
      case 108: lookup_id = 9;  break; // Full Body Cast -> Boo Boo (Bail)
      case 109: lookup_id = 12; break; // Endless Manual -> Mantastic Voyage
      case 110: lookup_id = 3;  break; // Park Architect -> Takin 'em to the limit
      case 111: lookup_id = 7;  break; // Chain Reaction -> Trick Spammer
      case 112: lookup_id = 36; break; // King of the Streets -> Spot Monopolist
      default: break;
    }
  }

  const uint8_t* png_data = nullptr;
  size_t png_size = 0;

  // 2. Check title_xdbf
  if (kernel_state_) {
    const rex::system::util::XdbfGameData db = kernel_state_->title_xdbf();
    if (db.is_valid()) {
      auto block = db.GetEntry(rex::system::util::XdbfSection::kImage, lookup_id);
      if (block.buffer && block.size > 0) {
        png_data = block.buffer;
        png_size = block.size;
      }
    }
  }

  // 3. Check Title Update database
  if (!png_data && spa_db_ && spa_db_->is_valid()) {
    auto block = spa_db_->GetEntry(rex::system::util::XdbfSection::kImage, lookup_id);
    if (block.buffer && block.size > 0) {
      png_data = block.buffer;
      png_size = block.size;
    }
  }

  if (!png_data || png_size == 0) {
    return nullptr;
  }

  // 3. Decode PNG via SDL3
  SDL_IOStream* io = SDL_IOFromConstMem(png_data, png_size);
  if (!io) {
    return nullptr;
  }
  SDL_Surface* surface = SDL_LoadPNG_IO(io, true);
  if (!surface) {
    return nullptr;
  }

  SDL_Surface* rgba_surface = SDL_ConvertSurface(surface, SDL_PIXELFORMAT_RGBA32);
  SDL_DestroySurface(surface);
  if (!rgba_surface) {
    return nullptr;
  }

  rex::ui::ImmediateDrawer* imm = GetImmediateDrawer();
  if (!imm) {
    SDL_DestroySurface(rgba_surface);
    return nullptr;
  }

  auto texture = imm->CreateTexture(
      rgba_surface->w, rgba_surface->h,
      rex::ui::ImmediateTextureFilter::kLinear, false,
      static_cast<const uint8_t*>(rgba_surface->pixels));

  SDL_DestroySurface(rgba_surface);

  if (!texture) {
    return nullptr;
  }

  void* result = texture.get();
  icon_textures_[achievement_id] = std::move(texture);
  return result;
}

void AchievementManager::TestTriggerAchievement(uint32_t achievement_id) {
  if (achievement_id == 0) {
    for (const auto& def : definitions_) {
      if (!IsUnlocked(0, def.id)) {
        achievement_id = def.id;
        break;
      }
    }
    if (achievement_id == 0 && !definitions_.empty()) {
      achievement_id = definitions_[0].id;
    }
  }
  if (achievement_id != 0) {
    // If already unlocked, still trigger the visual toast for testing
    if (!Unlock(0, achievement_id)) {
      std::string ach_name = fmt::format("Achievement #{}", achievement_id);
      std::string ach_desc;
      uint32_t gamerscore = 0;
      const auto it = id_to_index_.find(achievement_id);
      if (it != id_to_index_.end()) {
        const auto& def = definitions_[it->second];
        ach_name = def.name;
        ach_desc = def.description;
        gamerscore = def.gamerscore;
      }
      if (toast_dialog_) {
        toast_dialog_->PostToast({achievement_id, ach_name, ach_desc, gamerscore});
      }
    }
  }
}

uint32_t GetLocalSkaterPtr() {
  return g_local_skater_ptr.load(std::memory_order_relaxed);
}

bool IsLocalGameplayActive() {
  if (!g_local_gameplay_active.load(std::memory_order_relaxed)) {
    return false;
  }
  if (g_has_real_hud_gate.load(std::memory_order_relaxed)) {
    const auto now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now().time_since_epoch())
                            .count();
    const int64_t last_ns = g_last_local_gameplay_ns.load(std::memory_order_relaxed);
    if (last_ns == 0 || (now_ns - last_ns) > 1000000000LL) {
      return false;
    }
  }
  return true;
}

void SetLocalSkaterPhysicsPosition(float x, float y, float z) {
  if (std::isfinite(x) && std::isfinite(y) && std::isfinite(z)) {
    g_local_phys_pos_x.store(x, std::memory_order_relaxed);
    g_local_phys_pos_y.store(y, std::memory_order_relaxed);
    g_local_phys_pos_z.store(z, std::memory_order_relaxed);
    g_has_local_phys_pos.store(true, std::memory_order_relaxed);
  }
}

bool GetActiveLocalSkaterPosition(float out_pos[3]) {
  if (!out_pos) {
    return false;
  }
  if (g_has_local_phys_pos.load(std::memory_order_relaxed)) {
    out_pos[0] = g_local_phys_pos_x.load(std::memory_order_relaxed);
    out_pos[1] = g_local_phys_pos_y.load(std::memory_order_relaxed);
    out_pos[2] = g_local_phys_pos_z.load(std::memory_order_relaxed);
    return true;
  }
  return skate3::coord_dump::GetLastSkaterPosition(out_pos);
}

}  // namespace skate3

// ============================================================================
// Guest Hooks for Retail & Custom Achievement Triggers
// ============================================================================

namespace {

inline bool IsValidGuestAddr(uint32_t addr, uint32_t size = 4) {
  return addr >= 0x10000u && addr <= 0xF0000000u && (addr + size) >= addr &&
         (addr + size) <= 0xF0000000u;
}

inline uint8_t LoadU8(uint8_t* base, uint32_t addr) {
  return *(base + addr);
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

inline uint64_t LoadU64(uint8_t* base, uint32_t addr) {
  uint64_t raw;
  std::memcpy(&raw, base + addr, 8);
  return __builtin_bswap64(raw);
}

inline float LoadF32(uint8_t* base, uint32_t addr) {
  uint32_t raw = LoadU32(base, addr);
  float val;
  std::memcpy(&val, &raw, 4);
  return val;
}

inline std::string ReadGuestCString(uint8_t* base, uint32_t addr, size_t max_len = 64) {
  if (!IsValidGuestAddr(addr, 1)) {
    return {};
  }
  std::string out;
  for (size_t i = 0; i < max_len; ++i) {
    if (!IsValidGuestAddr(addr + static_cast<uint32_t>(i), 1)) {
      break;
    }
    const char c = static_cast<char>(LoadU8(base, addr + static_cast<uint32_t>(i)));
    if (c == '\0') {
      break;
    }
    out.push_back(c);
  }
  return out;
}

// Verifies the retail HUD gate set by sub_82775328:
// - *(uint8_t*)(hud_obj + 215) != 0 when the skater is the local human player and free-skate
//   stat recording is enabled (locks g_local_skater_ptr after pre-load attract mode).
// - *(uint8_t*)(hud_obj + 214) != 0 when the HUD's skater is in an active skating state
//   (remains 1 inside Own The Spot and Career challenges when +215 pauses free-skate stats).
inline bool CheckAndRecordLocalHudGate(uint8_t* base, uint32_t hud_obj, uint32_t skater) {
  if (!IsValidGuestAddr(skater, 128)) {
    return false;
  }
  if (IsValidGuestAddr(hud_obj, 224)) {
    skate3::g_has_real_hud_gate.store(true, std::memory_order_relaxed);
    const bool is_local_free_skate = (LoadU8(base, hud_obj + 215u) != 0);
    const bool is_hud_skating = (LoadU8(base, hud_obj + 214u) != 0);
    const uint32_t cur_local = skate3::g_local_skater_ptr.load(std::memory_order_relaxed);
    if (is_local_free_skate || (is_hud_skating && cur_local != 0 && cur_local == skater)) {
      skate3::g_local_skater_ptr.store(skater, std::memory_order_relaxed);
      skate3::g_local_gameplay_active.store(true, std::memory_order_relaxed);
      const auto now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                              std::chrono::steady_clock::now().time_since_epoch())
                              .count();
      skate3::g_last_local_gameplay_ns.store(now_ns, std::memory_order_relaxed);
      return true;
    } else {
      if (cur_local == 0 || cur_local == skater) {
        skate3::g_local_gameplay_active.store(false, std::memory_order_relaxed);
      }
      return false;
    }
  }
  // Unit-test path where hud_obj == 0:
  if (!skate3::g_has_real_hud_gate.load(std::memory_order_relaxed)) {
    if (!skate3::g_local_gameplay_active.load(std::memory_order_relaxed)) {
      return false;
    }
    const uint32_t cur_local = skate3::g_local_skater_ptr.load(std::memory_order_relaxed);
    if (cur_local == 0) {
      skate3::g_local_skater_ptr.store(skater, std::memory_order_relaxed);
    } else if (cur_local != skater) {
      return false;
    }
    return true;
  }
  return false;
}

inline bool IsLocalActiveSkater(uint32_t skater) {
  if (!skate3::IsLocalGameplayActive()) {
    return false;
  }
  const uint32_t local_skater = skate3::g_local_skater_ptr.load(std::memory_order_relaxed);
  if (skate3::g_has_real_hud_gate.load(std::memory_order_relaxed)) {
    return local_skater != 0 && skater == local_skater;
  }
  if (local_skater != 0 && skater != 0 && skater != local_skater) {
    return false;
  }
  return true;
}

// In ManualTracker, GrindTracker, and AirTracker, *(uint32_t*)(tracker + 4) is the owning skater pointer.
inline bool IsLocalActiveTracker(uint8_t* base, uint32_t tracker) {
  if (!skate3::IsLocalGameplayActive()) {
    return false;
  }
  const uint32_t tracker_skater = LoadU32(base, tracker + 4u);
  const uint32_t local_skater = skate3::g_local_skater_ptr.load(std::memory_order_relaxed);
  if (skate3::g_has_real_hud_gate.load(std::memory_order_relaxed)) {
    return local_skater != 0 && tracker_skater == local_skater;
  }
  if (local_skater != 0 && tracker_skater != 0 && tracker_skater != local_skater) {
    return false;
  }
  return true;
}

inline uint64_t ReadCareerBoardSalesTotal(uint8_t* base, uint32_t sales_struct) {
  if (!IsValidGuestAddr(sales_struct, 24)) {
    return 0;
  }
  return static_cast<uint64_t>(LoadU32(base, sales_struct + 4u)) +
         static_cast<uint64_t>(LoadU32(base, sales_struct + 8u)) +
         static_cast<uint64_t>(LoadU32(base, sales_struct + 12u)) +
         static_cast<uint64_t>(LoadU32(base, sales_struct + 16u));
}

inline void ReportCareerBoardSales(uint8_t* base, uint32_t sales_struct) {
  const uint64_t total_boards = ReadCareerBoardSalesTotal(base, sales_struct);
  if (total_boards > 0 && total_boards < 1000000000ull) {
    if (auto* mgr = skate3::AchievementManager::instance()) {
      mgr->OnStatUpdated("board_sales", static_cast<float>(total_boards));
    }
  }
}

inline std::string ExtractSpotCompletionKey(uint8_t* base, uint32_t challenge_obj) {
  if (IsValidGuestAddr(challenge_obj, 24)) {
    const uint32_t def_ptr = LoadU32(base, challenge_obj + 20u);
    if (IsValidGuestAddr(def_ptr, 8)) {
      const uint32_t inner_ptr = LoadU32(base, def_ptr + 0u);
      if (IsValidGuestAddr(inner_ptr, 32)) {
        const uint64_t cid = LoadU64(base, inner_ptr + 24u);
        if (cid != 0) {
          return fmt::format("spot_{:016x}", cid);
        }
      }
      return fmt::format("spot_def_{:08x}", def_ptr);
    }
    return fmt::format("spot_{:08x}", challenge_obj);
  }
  return "spot_completed";
}

}  // namespace

// 1. Retail Achievement Unlock Dispatchers
// 1a. Hash-based Achievement Unlock Dispatcher (maps achievement hash in r3 to XDBF ID via sub_8274A3F0)
extern "C" void sub_8274A590(PPCContext& ctx, uint8_t* base) {
  const uint64_t ach_hash = ctx.r3.u64;
  const uint64_t saved_lr = ctx.lr;
  __imp__sub_8274A3F0(ctx, base);
  const uint32_t ach_id = ctx.r3.u32;
  if (ach_id > 0 && ach_id < 200) {
    if (auto* mgr = skate3::AchievementManager::instance()) {
      mgr->Unlock(0, ach_id);
      if (ach_id == 36) {
        mgr->Unlock(0, 112);  // Spot Monopolist -> King of the Streets
      }
    }
  }
  ctx.r3.u64 = ach_hash;
  ctx.lr = saved_lr;
  __imp__sub_8274A590(ctx, base);
}

// 1b. Direct ID-based Achievement Unlock Dispatcher (r4 = XDBF achievement ID, used by sub_827422D8)
extern "C" void sub_82740298(PPCContext& ctx, uint8_t* base) {
  const uint32_t ach_id = ctx.r4.u32;
  if (ach_id > 0 && ach_id < 200) {
    if (auto* mgr = skate3::AchievementManager::instance()) {
      mgr->Unlock(0, ach_id);
      if (ach_id == 36) {
        mgr->Unlock(0, 112);  // Spot Monopolist -> King of the Streets
      }
    }
  }
  __imp__sub_82740298(ctx, base);
}

// 2. Manual Distance Tracker End & Tick
// In ManualTracker (size 1104):
// - +4 is the owning skater pointer
// - +1008 (3D meters at +1028, max at +1032) tracks Regular Manuals & Nose Manuals (flags & 0x0C000000)
// - +1056 (3D meters at +1076, max at +1080) tracks One-Foot Manuals (flags & 0x00800000)
extern "C" void sub_82DAA868(PPCContext& ctx, uint8_t* base) {
  const uint32_t manual_tracker = ctx.r3.u32;
  if (IsValidGuestAddr(manual_tracker, 1104) && IsLocalActiveTracker(base, manual_tracker) &&
      !skate3::IsSkaterBailedOrOffBoard()) {
    const float d1 = LoadF32(base, manual_tracker + 1028u);
    const float d2 = LoadF32(base, manual_tracker + 1032u);
    const float dist = std::max(std::isfinite(d1) ? d1 : 0.0f, std::isfinite(d2) ? d2 : 0.0f);
    if (dist > 0.0f && dist < 10000.0f) {
      if (auto* mgr = skate3::AchievementManager::instance()) {
        mgr->OnStatUpdated("manual_distance", dist);
        if (dist >= 1.0f) {
          const uint32_t flags = LoadU32(base, manual_tracker + 104u);
          if ((flags & 0x04000000u) != 0u) {
            mgr->OnTrickLanded("Nose Manual");
          } else {
            mgr->OnTrickLanded("Manual");
          }
        }
      }
    }
  }
  __imp__sub_82DAA868(ctx, base);
}

extern "C" void sub_82DAA8E0(PPCContext& ctx, uint8_t* base) {
  const uint32_t manual_tracker = ctx.r3.u32;
  __imp__sub_82DAA8E0(ctx, base);
  if (IsValidGuestAddr(manual_tracker, 1104) && IsLocalActiveTracker(base, manual_tracker) &&
      !skate3::IsSkaterBailedOrOffBoard()) {
    const float d1 = LoadF32(base, manual_tracker + 1028u);
    const float d2 = LoadF32(base, manual_tracker + 1032u);
    const float dist = std::max(std::isfinite(d1) ? d1 : 0.0f, std::isfinite(d2) ? d2 : 0.0f);
    if (dist >= 5.0f && dist < 10000.0f) {
      if (auto* mgr = skate3::AchievementManager::instance()) {
        mgr->OnStatUpdated("manual_distance", dist);
      }
    }
  }
}

extern "C" void sub_82DAA4C0(PPCContext& ctx, uint8_t* base) {
  const uint32_t manual_tracker = ctx.r3.u32;
  __imp__sub_82DAA4C0(ctx, base);
  if (!IsValidGuestAddr(manual_tracker, 1104) || !IsLocalActiveTracker(base, manual_tracker)) {
    return;
  }

  static bool s_in_manual = false;
  static float s_accum_manual_dist = 0.0f;
  static float s_prev_pos[3] = {0.0f, 0.0f, 0.0f};
  static uint64_t s_seen_reset_epoch = 0;

  const uint64_t cur_epoch = skate3::g_reset_epoch.load(std::memory_order_relaxed);
  if (cur_epoch != s_seen_reset_epoch) {
    s_seen_reset_epoch = cur_epoch;
    s_in_manual = false;
    s_accum_manual_dist = 0.0f;
  }

  if (skate3::IsSkaterBailedOrOffBoard() || skate3::IsSkaterOnBoardAirborne()) {
    s_in_manual = false;
    s_accum_manual_dist = 0.0f;
    return;
  }

  const uint32_t flags = LoadU32(base, manual_tracker + 104u);
  const bool is_manual_state = (flags & 0x0C800000u) != 0u;

  float pos[3] = {0.0f, 0.0f, 0.0f};
  if (is_manual_state && skate3::GetActiveLocalSkaterPosition(pos)) {
    if (s_in_manual) {
      const float dx = pos[0] - s_prev_pos[0];
      const float dy = pos[1] - s_prev_pos[1];
      const float dz = pos[2] - s_prev_pos[2];
      const float step = std::sqrt(dx * dx + dy * dy + dz * dz);
      if (std::isfinite(step) && step > 0.0f && step < 5.0f) {
        s_accum_manual_dist += step;
      }
    } else {
      s_in_manual = true;
      s_accum_manual_dist = 0.0f;
    }
    s_prev_pos[0] = pos[0];
    s_prev_pos[1] = pos[1];
    s_prev_pos[2] = pos[2];
  } else {
    s_in_manual = false;
    s_accum_manual_dist = 0.0f;
  }

  const float d_reg1 = LoadF32(base, manual_tracker + 1028u);
  const float d_reg2 = LoadF32(base, manual_tracker + 1032u);
  const float d_of1 = LoadF32(base, manual_tracker + 1076u);
  const float d_of2 = LoadF32(base, manual_tracker + 1080u);

  float best_dist = s_accum_manual_dist;
  if (std::isfinite(d_reg1) && d_reg1 < 10000.0f) best_dist = std::max(best_dist, d_reg1);
  if (std::isfinite(d_reg2) && d_reg2 < 10000.0f) best_dist = std::max(best_dist, d_reg2);
  if (std::isfinite(d_of1) && d_of1 < 10000.0f) best_dist = std::max(best_dist, d_of1);
  if (std::isfinite(d_of2) && d_of2 < 10000.0f) best_dist = std::max(best_dist, d_of2);

  if (best_dist >= 1.5f && best_dist < 10000.0f) {
    if (auto* mgr = skate3::AchievementManager::instance()) {
      if ((flags & 0x04000000u) != 0u) {
        mgr->OnTrickLanded("Nose Manual");
      } else if ((flags & 0x08800000u) != 0u) {
        mgr->OnTrickLanded("Manual");
      }
      if (best_dist >= 5.0f) {
        mgr->OnStatUpdated("manual_distance", best_dist);
      }
    }
  }
}

// 3. Grind Distance Tracker Tick & Telemetry (GrindTracker size 2400, +4 is skater, 3D meters at +760 and +808)
extern "C" void sub_82DA93D8(PPCContext& ctx, uint8_t* base) {
  const uint32_t grind_tracker = ctx.r3.u32;
  __imp__sub_82DA93D8(ctx, base);
  if (IsValidGuestAddr(grind_tracker, 840) && IsLocalActiveTracker(base, grind_tracker) &&
      !skate3::IsSkaterBailedOrOffBoard()) {
    const float d1 = LoadF32(base, grind_tracker + 760u);
    const float d2 = LoadF32(base, grind_tracker + 808u);
    const float dist = std::max(std::isfinite(d1) ? d1 : 0.0f, std::isfinite(d2) ? d2 : 0.0f);
    if (dist >= 5.0f && dist < 10000.0f) {
      if (auto* mgr = skate3::AchievementManager::instance()) {
        mgr->OnStatUpdated("grind_distance", dist);
      }
    }
  }
}

extern "C" void sub_82DA9920(PPCContext& ctx, uint8_t* base) {
  const uint32_t grind_tracker = ctx.r3.u32;
  if (IsValidGuestAddr(grind_tracker, 840) && IsLocalActiveTracker(base, grind_tracker) &&
      !skate3::IsSkaterBailedOrOffBoard()) {
    const float d1 = LoadF32(base, grind_tracker + 760u);
    const float d2 = LoadF32(base, grind_tracker + 808u);
    const float dist = std::max(std::isfinite(d1) ? d1 : 0.0f, std::isfinite(d2) ? d2 : 0.0f);
    if (dist > 0.0f && dist < 10000.0f) {
      if (auto* mgr = skate3::AchievementManager::instance()) {
        mgr->OnStatUpdated("grind_distance", dist);
      }
    }
  }
  __imp__sub_82DA9920(ctx, base);
}

// 4. Air Time Tracker End (AirTracker +4 is skater, DistanceTracker at +320, duration in seconds at +336)
extern "C" void sub_82DA9B08(PPCContext& ctx, uint8_t* base) {
  const uint32_t air_tracker = ctx.r3.u32;
  const bool landed_cleanly = (ctx.r4.u32 & 0xFFu) != 0;
  if (landed_cleanly && IsValidGuestAddr(air_tracker, 400) &&
      IsLocalActiveTracker(base, air_tracker) && !skate3::IsSkaterBailedOrOffBoard()) {
    const float air_time_sec = LoadF32(base, air_tracker + 336u);
    if (std::isfinite(air_time_sec) && air_time_sec > 0.0f && air_time_sec < 120.0f) {
      if (auto* mgr = skate3::AchievementManager::instance()) {
        mgr->OnStatUpdated("air_time", air_time_sec);
      }
    }
  }
  __imp__sub_82DA9B08(ctx, base);
}

// 5. Skater Line Score, Trick Score, Tricks Chained, Manuals Chained, Moveable Objects
extern "C" void sub_827743A0(PPCContext& ctx, uint8_t* base) {
  const uint32_t hud_obj = ctx.r3.u32;
  const uint32_t skater = ctx.r4.u32;
  if (CheckAndRecordLocalHudGate(base, hud_obj, skater)) {
    const uint32_t telemetry = LoadU32(base, skater + 60u);
    if (IsValidGuestAddr(telemetry, 15000)) {
      const bool bailed = (LoadU8(base, telemetry + 14640u) != 0) ||
                          (LoadU8(base, telemetry + 14630u) != 0);
      const bool in_seq = (LoadU8(base, telemetry + 14636u) != 0) ||
                          (LoadU8(base, telemetry + 14658u) != 0);
      const bool seq_banked = (LoadU8(base, telemetry + 14626u) != 0);

      const int32_t active_tricks = static_cast<int32_t>(
          LoadU32(base, telemetry + 120u) - LoadU32(base, telemetry + 124u));
      const int32_t banked_tricks = static_cast<int32_t>(
          LoadU32(base, telemetry + 84u) - LoadU32(base, telemetry + 88u));

      static int32_t s_max_active_tricks = 0;
      static uint64_t s_seen_reset_epoch = 0;
      const uint64_t cur_epoch = skate3::g_reset_epoch.load(std::memory_order_relaxed);
      if (cur_epoch != s_seen_reset_epoch) {
        s_seen_reset_epoch = cur_epoch;
        s_max_active_tricks = 0;
      }

      if (bailed) {
        s_max_active_tricks = 0;
      } else {
        if (active_tricks > s_max_active_tricks && active_tricks < 500) {
          s_max_active_tricks = active_tricks;
          if (!skate3::IsSkaterOnBoardAirborne() && active_tricks >= 2) {
            if (auto* mgr = skate3::AchievementManager::instance()) {
              mgr->OnStatUpdated("tricks_chained", static_cast<float>(active_tricks));
            }
          }
        }

        if (seq_banked || (!in_seq && s_max_active_tricks > 0)) {
          if (auto* mgr = skate3::AchievementManager::instance()) {
            const int32_t trick_score = static_cast<int32_t>(LoadU32(base, telemetry + 108u));
            const int32_t line_score = std::max(
                trick_score, static_cast<int32_t>(LoadU32(base, telemetry + 0u)));
            const int32_t tricks_chained = std::max(banked_tricks, s_max_active_tricks);
            const int32_t manuals_chained = static_cast<int32_t>(LoadU32(base, telemetry + 112u));
            const float max_grind = LoadF32(base, telemetry + 220u);
            const float man_d1 = LoadF32(base, telemetry + 60u);
            const float man_d2 = LoadF32(base, telemetry + 76u);
            const float max_manual = std::max(
                std::isfinite(man_d1) ? man_d1 : 0.0f,
                std::isfinite(man_d2) ? man_d2 : 0.0f);
            const int32_t moveable_objs =
                static_cast<int32_t>(static_cast<int8_t>(LoadU8(base, telemetry + 5758u)));

            if (trick_score > 0) {
              mgr->OnStatUpdated("trick_score", static_cast<float>(trick_score));
            }
            if (line_score > 0) {
              mgr->OnStatUpdated("line_score", static_cast<float>(line_score));
            }
            if (tricks_chained > 0 && tricks_chained < 500) {
              mgr->OnStatUpdated("tricks_chained", static_cast<float>(tricks_chained));
            }
            if (manuals_chained > 0 && manuals_chained < 500) {
              mgr->OnStatUpdated("manuals_chained", static_cast<float>(manuals_chained));
            }
            if (std::isfinite(max_grind) && max_grind > 0.0f && max_grind < 10000.0f) {
              mgr->OnStatUpdated("grind_distance", max_grind);
            }
            if (max_manual > 0.0f && max_manual < 10000.0f) {
              mgr->OnStatUpdated("manual_distance", max_manual);
            }
            if (moveable_objs > 0 && moveable_objs < 100) {
              mgr->OnStatUpdated("moveable_objects", static_cast<float>(moveable_objs));
            }
          }
          s_max_active_tricks = 0;
        }
      }
    }
  }
  __imp__sub_827743A0(ctx, base);
}

// 6. Skater Trick Queue Evaluator — Landed Trick Names (table at 0x820862A8, stride 24, C-string at +20)
extern "C" void sub_82774080(PPCContext& ctx, uint8_t* base) {
  const uint32_t hud_obj = ctx.r3.u32;
  const uint32_t skater = ctx.r4.u32;
  if (CheckAndRecordLocalHudGate(base, hud_obj, skater)) {
    const uint32_t telemetry = LoadU32(base, skater + 60u);
    if (IsValidGuestAddr(telemetry, 15000)) {
      const bool bailed = (LoadU8(base, telemetry + 14640u) != 0) ||
                          (LoadU8(base, telemetry + 14630u) != 0);
      const bool in_seq = (LoadU8(base, telemetry + 14636u) != 0) ||
                          (LoadU8(base, telemetry + 14658u) != 0);
      const bool seq_banked = (LoadU8(base, telemetry + 14626u) != 0);

      static std::vector<int32_t> s_pending_tricks;
      static bool s_was_in_seq = false;
      static uint64_t s_seen_reset_epoch = 0;
      const uint64_t cur_epoch = skate3::g_reset_epoch.load(std::memory_order_relaxed);
      if (cur_epoch != s_seen_reset_epoch) {
        s_seen_reset_epoch = cur_epoch;
        s_pending_tricks.clear();
        s_was_in_seq = false;
      }

      auto collect_tricks = [&]() {
        const uint32_t queues[2] = {telemetry + 1032u, telemetry + 1840u};
        for (uint32_t q : queues) {
          const uint32_t count = std::min(LoadU32(base, q), 4u);
          for (uint32_t i = 0; i < count; ++i) {
            const uint32_t entry = q + 20u + i * 200u;
            const int32_t trick_id = static_cast<int32_t>(LoadU32(base, entry));
            const uint8_t incomplete = LoadU8(base, entry + 178u);
            if (incomplete == 0 && trick_id > 0 && trick_id < 332) {
              if (std::find(s_pending_tricks.begin(), s_pending_tricks.end(), trick_id) ==
                  s_pending_tricks.end()) {
                s_pending_tricks.push_back(trick_id);
              }
            }
          }
        }
      };

      if (bailed) {
        s_pending_tricks.clear();
        s_was_in_seq = false;
      } else if (in_seq) {
        collect_tricks();
        s_was_in_seq = true;
      } else {
        collect_tricks();
        if (seq_banked || s_was_in_seq ||
            (!s_pending_tricks.empty() && !skate3::IsSkaterOnBoardAirborne())) {
          if (auto* mgr = skate3::AchievementManager::instance()) {
            for (int32_t trick_id : s_pending_tricks) {
              const uint32_t name_ptr =
                  LoadU32(base, 0x820862A8u + static_cast<uint32_t>(trick_id) * 24u + 20u);
              const std::string name = ReadGuestCString(base, name_ptr, 64);
              if (!name.empty()) {
                mgr->OnTrickLanded(name);
              }
              if (trick_id == 252 && LoadU32(base, telemetry + 9844u) != 0) {
                mgr->OnTrickLanded("Miracle Whip");
              }
            }
          }
          s_pending_tricks.clear();
          s_was_in_seq = false;
        }
      }
    }
  }
  __imp__sub_82774080(ctx, base);
}

// 6b. Direct Single-Trick Landing Callback (r3 = trick_id, r4 = failed/incomplete, r5 = skater)
extern "C" void sub_82773D98(PPCContext& ctx, uint8_t* base) {
  const int32_t trick_id = ctx.r3.s32;
  const bool failed = (ctx.r4.u32 != 0);
  const uint32_t skater = ctx.r5.u32;
  if (!failed && trick_id > 0 && trick_id < 332 && IsLocalActiveSkater(skater) &&
      !skate3::IsSkaterBailedOrOffBoard()) {
    if (auto* mgr = skate3::AchievementManager::instance()) {
      const uint32_t name_ptr =
          LoadU32(base, 0x820862A8u + static_cast<uint32_t>(trick_id) * 24u + 20u);
      const std::string name = ReadGuestCString(base, name_ptr, 64);
      if (!name.empty()) {
        mgr->OnTrickLanded(name);
      }
      if (trick_id == 252 && IsValidGuestAddr(skater, 128)) {
        const uint32_t telemetry = LoadU32(base, skater + 60u);
        if (IsValidGuestAddr(telemetry, 15000) && LoadU32(base, telemetry + 9844u) != 0) {
          mgr->OnTrickLanded("Miracle Whip");
        }
      }
    }
  }
  __imp__sub_82773D98(ctx, base);
}

// 7. Skater Hall of Meat Bail Score & Broken Bones Evaluator
extern "C" void sub_82774828(PPCContext& ctx, uint8_t* base) {
  const uint32_t hud_obj = ctx.r3.u32;
  const uint32_t skater = ctx.r4.u32;
  if (CheckAndRecordLocalHudGate(base, hud_obj, skater)) {
    const uint32_t telemetry = LoadU32(base, skater + 60u);
    if (IsValidGuestAddr(telemetry, 15000)) {
      if (LoadU8(base, telemetry + 14647u) != 0 || LoadU8(base, telemetry + 14646u) != 0) {
        const uint32_t hom = telemetry + 12336u;
        const int32_t bail_score = static_cast<int32_t>(LoadU32(base, hom + 1196u));
        const int32_t bones_broken = static_cast<int32_t>(LoadU32(base, hom + 904u));
        if (auto* mgr = skate3::AchievementManager::instance()) {
          if (bail_score > 0 && bail_score < 10000000) {
            mgr->OnStatUpdated("bail_score", static_cast<float>(bail_score));
          }
          if (bones_broken > 0 && bones_broken < 100) {
            mgr->OnStatUpdated("bones_broken", static_cast<float>(bones_broken));
          }
        }
      }
    }
  }
  __imp__sub_82774828(ctx, base);
}

// 8. Career Board Sales & Challenge/Spot Completion Hooks
extern "C" void sub_827CE7B0(PPCContext& ctx, uint8_t* base) {
  const uint32_t career_sub = ctx.r3.u32;
  __imp__sub_827CE7B0(ctx, base);
  if (skate3::IsLocalGameplayActive() && IsValidGuestAddr(career_sub, 80)) {
    const uint32_t sales_struct = LoadU32(base, career_sub + 56u);
    ReportCareerBoardSales(base, sales_struct);
  }
}

extern "C" void sub_827CFD48(PPCContext& ctx, uint8_t* base) {
  const uint32_t career_mgr = ctx.r3.u32;
  __imp__sub_827CFD48(ctx, base);
  static uint32_t s_sales_poll_counter = 0;

  if (!skate3::IsLocalGameplayActive() || !IsValidGuestAddr(career_mgr, 96)) {
    return;
  }
  if ((++s_sales_poll_counter % 60u) != 0u) {
    return;
  }
  const uint32_t sales_struct = LoadU32(base, career_mgr + 72u);
  ReportCareerBoardSales(base, sales_struct);
}

extern "C" void sub_827CDB10(PPCContext& ctx, uint8_t* base) {
  const uint32_t career_mgr = ctx.r3.u32 - 4u;
  const uint32_t challenge_obj = ctx.r4.u32;
  const int32_t status = static_cast<int32_t>(ctx.r5.u32);
  const std::string spot_key = ExtractSpotCompletionKey(base, challenge_obj);
  __imp__sub_827CDB10(ctx, base);

  if (status == 1 || status == 2) {
    if (auto* mgr = skate3::AchievementManager::instance()) {
      mgr->OnSpotCompleted(spot_key, status == 2);
    }
  }
  if (IsValidGuestAddr(career_mgr, 96)) {
    const uint32_t sales_struct = LoadU32(base, career_mgr + 72u);
    ReportCareerBoardSales(base, sales_struct);
  }
}

extern "C" void sub_827CDDD8(PPCContext& ctx, uint8_t* base) {
  const uint32_t career_mgr = ctx.r3.u32;
  const uint32_t challenge_obj = ctx.r4.u32;
  const int32_t status = static_cast<int32_t>(ctx.r5.u32);
  const std::string spot_key = ExtractSpotCompletionKey(base, challenge_obj);
  __imp__sub_827CDDD8(ctx, base);

  if (status == 1 || status == 2) {
    if (auto* mgr = skate3::AchievementManager::instance()) {
      mgr->OnSpotCompleted(spot_key, status == 2);
    }
  }
  if (IsValidGuestAddr(career_mgr, 96)) {
    const uint32_t sales_struct = LoadU32(base, career_mgr + 72u);
    ReportCareerBoardSales(base, sales_struct);
  }
}

// 9. Cheat Code Entry Hooks (UI Keyboard Callback & Cheat Code Verifier)
extern "C" void sub_82639890(PPCContext& ctx, uint8_t* base) {
  const uint32_t str_ptr = ctx.r5.u32;
  const bool cancelled = (ctx.r6.u32 & 0xFFu) != 0;
  if (!cancelled && IsValidGuestAddr(str_ptr, 1)) {
    const std::string code = ReadGuestCString(base, str_ptr, 64);
    if (!code.empty()) {
      if (auto* mgr = skate3::AchievementManager::instance()) {
        mgr->OnCheatEntered(code);
      }
    }
  }
  __imp__sub_82639890(ctx, base);
}

extern "C" void sub_827586F0(PPCContext& ctx, uint8_t* base) {
  const uint32_t wstr_ptr = ctx.r3.u32;
  if (IsValidGuestAddr(wstr_ptr, 2)) {
    std::string code;
    for (uint32_t i = 0; i < 64; ++i) {
      if (!IsValidGuestAddr(wstr_ptr + i * 2u, 2)) {
        break;
      }
      const uint16_t wc = LoadU16(base, wstr_ptr + i * 2u);
      if (wc == 0) {
        break;
      }
      if (wc < 128) {
        code.push_back(static_cast<char>(wc));
      }
    }
    if (!code.empty()) {
      if (auto* mgr = skate3::AchievementManager::instance()) {
        mgr->OnCheatEntered(code);
      }
    }
  }
  __imp__sub_827586F0(ctx, base);
}

// 10. Custom skate.Park Save Hooks for 110 ("Park Architect" - at least 25 placed objects)
extern "C" void sub_8288EA18(PPCContext& ctx, uint8_t* base) {
  const uint32_t stats_out = ctx.r3.u32;
  __imp__sub_8288EA18(ctx, base);
  if (IsValidGuestAddr(stats_out, 24)) {
    const uint32_t prop_count = LoadU32(base, stats_out + 4u);
    const uint32_t total_count = std::max(prop_count, LoadU32(base, stats_out + 16u));
    if (total_count > 0 && total_count < 100000u) {
      if (auto* mgr = skate3::AchievementManager::instance()) {
        mgr->OnStatUpdated("park_objects", static_cast<float>(total_count));
      }
    }
  }
}

extern "C" void sub_825113D8(PPCContext& ctx, uint8_t* base) {
  __imp__sub_825113D8(ctx, base);
  if (IsValidGuestAddr(0x8304FD94u, 4)) {
    const uint32_t global_mgr = LoadU32(base, 0x8304FD94u);
    if (IsValidGuestAddr(global_mgr, 272)) {
      const uint32_t park_obj_mgr = LoadU32(base, global_mgr + 268u);
      if (IsValidGuestAddr(park_obj_mgr, 7520)) {
        const uint32_t total_count =
            LoadU32(base, park_obj_mgr + 7504u) + LoadU32(base, park_obj_mgr + 24u);
        if (total_count > 0 && total_count < 100000u) {
          if (auto* mgr = skate3::AchievementManager::instance()) {
            mgr->OnStatUpdated("park_objects", static_cast<float>(total_count));
          }
        }
      }
    }
  }
}

