// GTA IV PS5 - the TV's refresh rate (the hub's Refresh Rate) and how many
// vblanks each frame stays on screen.
//
// The 120 Hz mode is VideoOut's mode 15, on the handle RADV opened
// (wsi_common_videoout.c). RADV asks for it itself when it opens the TV, once,
// before the hub, if param.json declares 120 Hz output (attribute3 0x40, with
// 0x40000 as retail titles with VRR do: ps5_package.sh). The link wraps three
// of its VideoOut calls (ps5_package.sh and CMakeLists.txt, --wrap):
//  - IsOutputSupported: notes the handle; RADV's ask for the 120 Hz mode only
//    goes through when the hub's Refresh Rate is 120.
//  - ConfigureOutput: notes whether the 120 Hz mode was taken.
//  - SetFlipRate: notes the handle; RADV sets 0 (a frame shows from the next
//    vblank) for each swapchain, and the rate the frame limit wants is kept
//    instead.
// RADV asks only for attribute3 0x80040 (with 0x80000, "VRR at 120 Hz"),
// which ours (0x40040: 120 Hz, VRR supported) isn't, so the switch is the
// hub's: at its first frame when the Refresh Rate is 120, and when it's chosen
// (SetHighRefresh), both from its own thread, while no new frame is flipped.
//
// With VRR on in the PS5's settings and a TV that has it, the 120 Hz output
// becomes a VRR one: until frames come, the TV refreshes at its VRR floor
// (48 Hz on most, a vblank every ~20.8 ms), which the first check here (and
// RADV's) took for a TV stuck at 60 Hz, putting it back (PS5_Mesa issue 4; a
// TCL C6K tester's "can't switch to 120 Hz"). A TV at a fixed 60 Hz keeps
// every vblank ~16.7 ms apart; one faster than that, or a period past the 60
// Hz one, is a 120 Hz or a VRR output, kept.
#pragma once

namespace gta4ps5::display {

// Whether the TV refreshes at 119.88 Hz (else 59.94).
bool HighRefresh();
// Whether that 120 Hz output is a VRR one (the PS5's VRR setting on, a TV
// with it): frames are then shown as soon as they're ready (the frame limit
// spaces them), the TV refreshing with them.
bool Vrr();
// Whether the TV is open (a switch can be made).
bool Ready();

// Switches the TV to 119.88 Hz (true) or back to 59.94 Hz now. False when
// it stays where it was: no TV open yet, or (for 120) the TV or the PS5's
// video output settings don't take it. The screen goes dark for a moment.
bool SetHighRefresh(bool high);

// From the game's start (the hub keeps every vblank): each frame stays on
// screen for as many vblanks as the frame limit takes. At 120 Hz: 40 FPS
// three, 60 FPS two; at 60 Hz: 30 FPS two; with VRR, 60 and 120 FPS none (as
// soon as it's ready). Follows later frame limit changes (the pause menu's
// Graphics page).
void StartGamePacing();

}  // namespace gta4ps5::display
