// GeneralsX @feature android-port 08/02/2026
// SageAndroidBootstrap — the Android startup sequence, shared by both games.
//
// This lived inside GeneralsMD/Code/Main/SDL3Main.cpp, where it was reachable
// only by Zero Hour. Generals needs exactly the same treatment on Android:
// the working directory has to be pointed at the game data, fonts extracted out
// of the APK, and the log teed to a readable file.
//
// It is shared rather than copied on purpose. This port has already been bitten
// twice by duplicated platform code drifting apart -- OpenALAudioManager.cpp and
// OpenALAudioCache.cpp each chose their FFmpeg headers independently, and fixing
// one left the other stubbed, which cost weeks of silent audio. Two engine mains
// each carrying their own copy of this would fail the same way.
//
// Everything here is a no-op off Android; the file compiles to nothing.

#pragma once

// Entry points, called in this order from each game's SDL_main:
//
//   SageAndroid_ForceAudioBackend()   very first, before OpenAL initialises
//   SageAndroid_Bootstrap(argc, argv) before any file access
//   SageAndroid_SetSdlHints()         before SDL_InitSubSystem(SDL_INIT_VIDEO)

void SageAndroid_ForceAudioBackend();
void SageAndroid_Bootstrap(int argc, char **argv);
void SageAndroid_SetSdlHints();
