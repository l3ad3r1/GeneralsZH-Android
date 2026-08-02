/*
** SageMobileInput.h
**
** GeneralsX @refactor android-port 08/02/2026
** Mobile app-lifecycle handling and the RTS touch gesture translator, shared by
** both engines.
**
** This lived inside GeneralsMD's SDL3GameEngine.cpp, so only Zero Hour had it.
** Generals shipped its own SDL3GameEngine.cpp with zero mobile support, which is
** why the Generals build booted fully and then left the main loop immediately:
** nothing was gating rendering on the app lifecycle, and no touch events were
** translated.
**
** Shared rather than copied, for the same reason as SageAndroidBootstrap: this
** port has twice been bitten by duplicated platform code drifting apart.
**
** The gesture scheme itself is ported from wingear's fork (GPL-3.0),
** https://github.com/wingear/GeneralsZH-Android-OpenGL-ES -- design credit theirs.
**
** Each game compiles this file into its own target, so "SDL3Device/GameClient/
** SDL3Mouse.h" resolves to that game's own SDL3Mouse.
*/

#pragma once

#if (defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE) || defined(__ANDROID__)
#define SAGE_MOBILE 1
#endif

#ifdef SAGE_MOBILE

#include <SDL3/SDL.h>

class SDL3Mouse;

/** True while the app is backgrounded or inactive: do not touch the GPU. */
bool SageMobile_ShouldPauseRendering();

/** SDL event watcher; lifecycle events can fire outside the poll cycle. */
bool SDLCALL SageMobile_LifecycleWatcher(void *userdata, SDL_Event *event);

/** Translate one SDL_EVENT_FINGER_* into the mouse events the engine expects. */
void SageMobile_HandleTouchEvent(SDL3Mouse *mouse, SDL_Window *window, const SDL_Event &event);

/** Pump the long-press timer each frame: a stationary finger emits no events. */
void SageMobile_UpdateTouchLongPress(SDL3Mouse *mouse, SDL_Window *window);

/** Debug multi-touch injector for adb runs; see the source for the format. */
int SageMobile_TouchScriptThread(void *);

/** Append -xres/-yres matching the real window, unless the user set them. */
void SageMobile_ApplyNativeResolution(SDL_Window *window, int &argc, char **&argv);

#endif // SAGE_MOBILE
