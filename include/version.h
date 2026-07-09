#pragma once

// Human-facing firmware version, bumped by hand on every deployed update (user
// convention). Distinct from build_info.h's FW_GIT_HASH/FW_BUILD_TIME, which are
// auto-generated per build. Lives in its own header so both ui.cpp (settings +
// main-screen label) and the OTA module (GitHub release comparison) share one
// definition. Format is "vMAJ.MIN" — the OTA check parses it numerically.
#define FW_VERSION "v0.29"  // bump on every deployed update (user convention)
