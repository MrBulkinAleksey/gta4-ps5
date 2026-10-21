// GTA IV PS5 - the trophy chime (hub_sfx.cpp): made in code at 48 kHz,
// stereo interleaved. (The hub's menu sounds are recordings: hub/sfx,
// ps5_hub.cpp.)
#pragma once

#include <vector>

namespace gta4ps5::hub {

// A trophy's chime (the popup in the game); `platinum`: a fuller one.
std::vector<float> TrophyChime(bool platinum);

}  // namespace gta4ps5::hub
