// The installer payload builds the app's archive unpacker (gta4_rpf_extractor.cpp,
// gta4_install_io.h) on its own, without the ReXGlue SDK. It's a payload, not
// the app: reads and writes go through the file cache, plainly, the way an FTP
// server's do (the app's ways around the PS5's limits on a title aren't needed).
#pragma once

#define REX_PLATFORM_PS5 0
