/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2025 Electronic Arts Inc.
**
**	This program is free software: you can redistribute it and/or modify
**	it under the terms of the GNU General Public License as published by
**	the Free Software Foundation, either version 3 of the License, or
**	(at your option) any later version.
**
**	This program is distributed in the hope that it will be useful,
**	but WITHOUT ANY WARRANTY; without even the implied warranty of
**	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
**	GNU General Public License for more details.
**
**	You should have received a copy of the GNU General Public License
**	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

/*
** SDL3GameEngine.cpp
**
** Linux implementation of GameEngine using SDL3 for windowing/input.
**
** TheSuperHackers @feature CnC_Generals_Linux 07/02/2026
** Provides SDL3-based input and window management for Linux builds.
** Based on fighter19 reference implementation.
*/

#ifndef _WIN32

#include "SDL3GameEngine.h"
#ifdef SAGE_USE_OPENAL
#include "OpenALAudioDevice/OpenALAudioManager.h"
#else
#include "OpenALAudioManager.h"
#endif
#include "SDL3Device/GameClient/SDL3Mouse.h"
#include "SDL3Device/GameClient/SDL3Keyboard.h"
#include "GameClient/Mouse.h"
#include "GameClient/Keyboard.h"
#include "GameClient/GameWindow.h"
#include "GameClient/GameWindowManager.h"
#include "GameClient/Gadget.h"
#include "W3DDevice/GameLogic/W3DGameLogic.h"
#include "W3DDevice/GameClient/W3DGameClient.h"
#include "W3DDevice/Common/W3DModuleFactory.h"
#include "W3DDevice/Common/W3DThingFactory.h"
#include "W3DDevice/Common/W3DFunctionLexicon.h"
#include "W3DDevice/Common/W3DRadar.h"
#include "W3DDevice/GameClient/W3DParticleSys.h"
#include "W3DDevice/GameClient/W3DWebBrowser.h"
#include "StdDevice/Common/StdLocalFileSystem.h"
#include "StdDevice/Common/StdBIGFileSystem.h"
#include "Common/GlobalData.h"
#include "GameClient/View.h"
#include "GameClient/Display.h"
#include "GameClient/InGameUI.h"
#include "GameLogic/GameLogic.h"
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

// GeneralsX @feature android-port 06/07/2026
// Touch input, app-lifecycle render-gating, and the gesture state machine are
// shared by every mobile target. SAGE_MOBILE covers iOS and Android; the iOS
// branch names (iosShouldPauseRendering, iosLifecycleWatcher) are kept for
// history but now compile for Android too — Android destroys the drawing
// surface (ANativeWindow) on background, so halting render+sim is even more
// mandatory there than on iOS.
#if (defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE) || defined(__ANDROID__)
#define SAGE_MOBILE 1
#endif

// Extern globals for input devices (set by GameClient)
extern Mouse *TheMouse;
extern Keyboard *TheKeyboard;
extern GameWindowManager *TheWindowManager;

#ifdef SAGE_MOBILE
#include <atomic>

// ---------------------------------------------------------------------------
// Mobile app lifecycle
//
// Mobile OSes suspend the process when the app leaves the foreground. Any GPU
// work submitted around suspension is hazardous: on iOS it stalls on drawable
// acquisition (MoltenVK waits out a timeout per present); on Android the
// ANativeWindow backing the Vulkan surface is DESTROYED, so a present call
// fails hard with VK_ERROR_SURFACE_LOST_KHR. SDL warns that lifecycle events
// can arrive outside the normal poll cycle, so they are captured in an event
// watcher that fires immediately on the delivering thread; the engine update
// loop checks the flag and skips simulation + rendering while backgrounded.
// ---------------------------------------------------------------------------
// Two independent reasons to halt the render/sim loop:
//  - BACKGROUNDED (home / switched away): the process is about to be suspended.
//  - INACTIVE (multitasking switcher open, Control Center / Recents, a
//    notification banner): the OS snapshots the window and owns the drawing
//    surface during this window — and crucially, opening the app switcher
//    fires resign-active WITHOUT a full background transition.
// Acquiring a drawable during EITHER state fights the OS for the surface; across
// repeated suspend/switcher cycles the Vulkan/MoltenVK surface is driven into an
// unrecoverable state and the app crashes (the reported "crashes after
// backgrounding / multitasking a few times"). Pause whenever either is set.
static std::atomic<bool> s_appBackgrounded{false};
static std::atomic<bool> s_appInactive{false};

static inline bool iosShouldPauseRendering()
{
	return s_appBackgrounded.load() || s_appInactive.load();
}

static bool SDLCALL iosLifecycleWatcher(void *userdata, SDL_Event *event)
{
	switch (event->type) {
		case SDL_EVENT_WILL_ENTER_BACKGROUND:
		case SDL_EVENT_DID_ENTER_BACKGROUND:
			s_appBackgrounded.store(true);
			break;
		case SDL_EVENT_DID_ENTER_FOREGROUND:
			s_appBackgrounded.store(false);
			break;
		// Resign/become active. On iOS, SDL maps applicationWillResignActive ->
		// window focus lost and applicationDidBecomeActive -> window focus gained.
		// On Android, SDL maps onPause->FOCUS_LOST/MINIMIZED and onResume->
		// FOCUS_GAINED/RESTORED. Stay paused until fully active again (focus
		// regained), which arrives after DID_ENTER_FOREGROUND.
		case SDL_EVENT_WINDOW_FOCUS_LOST:
		case SDL_EVENT_WINDOW_MINIMIZED:
			s_appInactive.store(true);
			break;
		case SDL_EVENT_WINDOW_FOCUS_GAINED:
		case SDL_EVENT_WINDOW_RESTORED:
			s_appInactive.store(false);
			break;
		default:
			break;
	}
	return true;
}

// ---------------------------------------------------------------------------
// Mobile touch -> mouse gesture translation
//
// GeneralsX @feature android-port 08/01/2026 The RTS gesture scheme below --
// instant-tap clicks with double-tap detected on top, one-finger 1:1 map
// panning, pinch zoom anchored to the real camera height, and the two-finger
// building placement/rotation flow -- is ported from wingear's fork:
//
//   https://github.com/wingear/GeneralsZH-Android-OpenGL-ES
//   (GPL-3.0, same licence as this tree; commits "touch v2/v3/v4")
//
// The design credit is theirs. It is renderer-independent, so it applies
// unchanged to our DXVK build; only the comments were translated from Russian
// and the include guard restored for our SAGE_USE_OPENAL build option.
//
// SDL's automatic touch-mouse synthesis is disabled on mobile (SDL3Main.cpp
// sets SDL_HINT_TOUCH_MOUSE_EVENTS=0); every mouse event the game sees on a
// touch device is synthesized here, through the same SDL3Mouse::addSDLEvent
// path real mice use. The same SDL_EVENT_FINGER_* events fire on iOS (UIKit)
// and Android (MotionEvent), so this state machine is shared verbatim.
//
// Gestures (mobile scheme -- "the map lives under your finger"):
//   1 finger tap (world)   -> left click, sent immediately; a double tap
//                             is detected on top of it, exactly like desktop
//   1 finger tap (UI/menu) -> left click immediately (buttons must not lag)
//   1 finger drag (world)  -> PAN the map; the map tracks the finger 1:1
//   1 finger drag (over UI)-> ordinary LMB drag (scrollbars, lists)
//   double tap (units)     -> double LMB click (all units of that type)
//   double tap (ground)    -> RMB click = deselect
//   double tap + drag/hold -> LMB selection box (first click NOT sent)
//   1 finger long-press    -> right click, if the finger stays put
//   2 fingers drag         -> pan; pinch -> 1:1 zoom
//   placement mode: finger1 positions the building, a second finger rotates
//                   it toward that point (LMB anchor + drag), lift = place
// ---------------------------------------------------------------------------
namespace {

struct TouchState {
	enum Phase {
		IDLE,           // no fingers tracked
		PENDING,        // finger1 down, gesture identity not yet known, nothing sent
		SELECT_PENDING, // second tap held: pure double-click or selection box
		UI_DRAG,        // LMB drag over the UI (scrollbars/lists), LMB held
		PANNING,        // one-finger map pan (no mouse buttons)
		SELECTING,      // selection box, LMB held
		LONGPRESSED,    // long-press fired (RMB click sent), swallow until lift
		PAN2,           // two-finger pan + pinch zoom (no mouse buttons)
		PLACE_MOVE,     // placement: finger1 moves the building (cursor), no buttons
		PLACE_ROTATE    // placement: LMB anchored, finger2 sets the angle
	};

	// (TAP_WAIT removed: a tap's click is sent instantly, double tap builds on top)
	Phase phase = IDLE;
	SDL_FingerID finger1 = 0;
	SDL_FingerID finger2 = 0;
	float downX = 0.0f, downY = 0.0f;   // finger1 down position (window points)
	float lastX = 0.0f, lastY = 0.0f;   // finger1 latest position
	float panX = 0.0f, panY = 0.0f;     // pan centroid (PAN2)
	float pinchDist = 0.0f;
	Uint64 downTicks = 0;
	float f1x = 0.0f, f1y = 0.0f, f2x = 0.0f, f2y = 0.0f; // normalized per finger
	// last clean tap (double-tap detection; its click already went out)
	float lastTapX = -10000.0f, lastTapY = -10000.0f;
	Uint64 lastTapUpTicks = 0;
	// pinch zoom 1:1 through the ACTUAL camera height
	float heightStart = 0.0f, distStart = 0.0f;
	// last position of finger2 (PLACE_ROTATE)
	float last2X = 0.0f, last2Y = 0.0f;
};

TouchState s_touch;

// Gesture trace, so multi-touch can be verified from logcat on adb runs.
#if defined(__ANDROID__)
#include <android/log.h>
#define GX_TOUCH_LOG(...) __android_log_print(ANDROID_LOG_INFO, "GX-TOUCH", __VA_ARGS__)
#else
#define GX_TOUCH_LOG(...) ((void)0)
#endif

const Uint64 LONG_PRESS_MS = 600;
const Uint64 DOUBLE_TAP_MS = 350;      // second-tap window
const Uint64 SELECT_HOLD_MS = 250;     // holding the second tap = selection box
const float DOUBLE_TAP_RADIUS_PX = 64.0f;
const float TAP_DEAD_ZONE_PX = 8.0f;   // jitter below this keeps a tap a tap

// Panning is allowed only in a real match (not the shell menu, where scripts
// drive the camera), and only if the gesture did NOT start over a UI window.
bool gxWorldPanAllowed()
{
	return TheTacticalView != nullptr && TheGameLogic != nullptr &&
	       TheGameLogic->isInGame() && !TheGameLogic->isInShellGame();
}

// Is building-placement mode active (something picked in the build panel).
bool gxPlacementActive()
{
	return TheInGameUI != nullptr && TheInGameUI->getPendingPlaceType() != nullptr;
}

// Is there a unit/object under this point (window pixels) -- double-tap branch.
bool gxDrawableUnder(float px, float py, int winW, int winH)
{
	if (TheTacticalView == nullptr || TheDisplay == nullptr || winW <= 0 || winH <= 0)
		return false;
	ICoord2D s;
	s.x = (Int)(px * (float)TheDisplay->getWidth() / (float)winW);
	s.y = (Int)(py * (float)TheDisplay->getHeight() / (float)winH);
	return TheTacticalView->pickDrawable(&s, FALSE, (PickType)PICK_TYPE_ALL_DRAWABLES) != nullptr;
}

bool gxPointOverUI(float px, float py, int winW, int winH)
{
	if (TheWindowManager == nullptr || TheDisplay == nullptr || winW <= 0 || winH <= 0)
		return true;   // GUI not ready yet -- behave conservatively (treat it as UI)
	const Int gx = (Int)(px * (float)TheDisplay->getWidth() / (float)winW);
	const Int gy = (Int)(py * (float)TheDisplay->getHeight() / (float)winH);
	return TheWindowManager->getWindowUnderCursor(gx, gy) != nullptr;
}

// Pan so the world point stays under the finger: both finger positions are
// projected onto the terrain and the camera moves by the difference. Exact 1:1.
void gxPanBetween(float fromXpx, float fromYpx, float toXpx, float toYpx, int winW, int winH)
{
	if (TheTacticalView == nullptr || TheDisplay == nullptr || winW <= 0 || winH <= 0)
		return;
	const float sx = (float)TheDisplay->getWidth() / (float)winW;
	const float sy = (float)TheDisplay->getHeight() / (float)winH;
	ICoord2D sFrom, sTo;
	sFrom.x = (Int)(fromXpx * sx);
	sFrom.y = (Int)(fromYpx * sy);
	sTo.x = (Int)(toXpx * sx);
	sTo.y = (Int)(toYpx * sy);
	Coord3D wFrom, wTo;
	TheTacticalView->screenToTerrain(&sFrom, &wFrom);
	TheTacticalView->screenToTerrain(&sTo, &wTo);
	const Coord3D &cur = TheTacticalView->getPosition();
	Coord3D p;
	p.x = cur.x + (wFrom.x - wTo.x);
	p.y = cur.y + (wFrom.y - wTo.y);
	p.z = 0.0f;   // z on the ground -> lookAt takes the simple path (setPosition + recalc)
	TheTacticalView->lookAt(&p);
}

void sendSyntheticMouse(SDL3Mouse *mouse, SDL_Window *window, Uint32 type,
                        float x, float y, Uint8 button = 0, float wheelY = 0.0f)
{
	// The windowID must be valid: SDL3Mouse::scaleMouseCoordinates() looks the
	// window up by id to map window points into the game's internal resolution,
	// and silently skips scaling when the lookup fails.
	const SDL_WindowID windowID = SDL_GetWindowID(window);

	SDL_Event ev;
	SDL_zero(ev);
	ev.type = type;
	// GeneralsX @bugfix android-port 08/01/2026 Tag these as touch-derived.
	// They ARE touch-derived, and saying so lets SDL3Mouse tell a real mouse
	// apart from this translator's output. Leaving the id at 0 made that
	// impossible: SDL on Android reports which==0 for the actual mouse too, so
	// "nonzero means hardware" never fired and edge-scrolling stayed disabled
	// with a mouse attached. Only the SDL event loop drops SDL_TOUCH_MOUSEID
	// events, and these are handed straight to addSDLEvent(), so tagging them
	// cannot cause them to be discarded.
	switch (type) {
		case SDL_EVENT_MOUSE_MOTION:
			ev.motion.windowID = windowID;
			ev.motion.which = SDL_TOUCH_MOUSEID;
			ev.motion.x = x;
			ev.motion.y = y;
			break;
		case SDL_EVENT_MOUSE_BUTTON_DOWN:
		case SDL_EVENT_MOUSE_BUTTON_UP:
			ev.button.windowID = windowID;
			ev.button.which = SDL_TOUCH_MOUSEID;
			ev.button.button = button;
			ev.button.down = (type == SDL_EVENT_MOUSE_BUTTON_DOWN);
			ev.button.clicks = 1;
			ev.button.x = x;
			ev.button.y = y;
			break;
		case SDL_EVENT_MOUSE_WHEEL:
			ev.wheel.windowID = windowID;
			ev.wheel.which = SDL_TOUCH_MOUSEID;
			ev.wheel.x = 0.0f;
			ev.wheel.y = wheelY;
			ev.wheel.mouse_x = x;
			ev.wheel.mouse_y = y;
			break;
	}
	mouse->addSDLEvent(&ev);
}

void beginPan2(int winW, int winH)
{
	// Two-finger gesture: pan by centroid + 1:1 pinch zoom. No mouse buttons.
	s_touch.panX = (s_touch.f1x + s_touch.f2x) * 0.5f * (float)winW;
	s_touch.panY = (s_touch.f1y + s_touch.f2y) * 0.5f * (float)winH;
	const float dx = (s_touch.f1x - s_touch.f2x) * (float)winW;
	const float dy = (s_touch.f1y - s_touch.f2y) * (float)winH;
	s_touch.pinchDist = SDL_sqrtf(dx * dx + dy * dy);
	s_touch.distStart = s_touch.pinchDist;
	// ACTUAL camera height (not m_zoom: that is a "desired" value on a different
	// scale, and the mismatch made the zoom jerk on the first finger movement).
	s_touch.heightStart = TheTacticalView ? TheTacticalView->getHeightAboveGround() : 0.0f;
	s_touch.phase = TouchState::PAN2;
}

void handleTouchEvent(SDL3Mouse *mouse, SDL_Window *window, const SDL_Event &event)
{
	// GeneralsX @bugfix android-port 08/01/2026 Never let a real mouse drive the
	// gesture machine. SDL synthesises finger events from mouse input on mobile
	// (SDL_HINT_MOUSE_TOUCH_EVENTS defaults to "1" there); those arrive tagged
	// with SDL_MOUSE_TOUCHID. Left unfiltered, a mouse drag becomes a one-finger
	// map pan -- which moves the camera opposite to the drag on purpose -- so the
	// mouse reads as inverted, and each click is also delivered twice. The hint
	// is turned off in SDL3Main.cpp; this is the belt-and-braces guard, because
	// the hint is documented as settable at any time and a synthesised finger is
	// never something this translator should act on.
	if (event.tfinger.touchID == SDL_MOUSE_TOUCHID) {
		return;
	}

	int winW = 0, winH = 0;
	SDL_GetWindowSize(window, &winW, &winH);
	const float px = event.tfinger.x * (float)winW;
	const float py = event.tfinger.y * (float)winH;

	switch (event.type) {
	case SDL_EVENT_FINGER_DOWN:
		if (s_touch.phase == TouchState::IDLE) {
			s_touch.finger1 = event.tfinger.fingerID;
			s_touch.downX = s_touch.lastX = px;
			s_touch.downY = s_touch.lastY = py;
			s_touch.f1x = event.tfinger.x;
			s_touch.f1y = event.tfinger.y;
			s_touch.downTicks = SDL_GetTicks();

			// A second touch nearby, soon after a clean world tap -> double-tap gesture
			// (the first click already went out instantly, exactly as on desktop).
			const bool doubleTap = gxWorldPanAllowed() &&
				(SDL_GetTicks() - s_touch.lastTapUpTicks) <= DOUBLE_TAP_MS &&
				SDL_fabsf(px - s_touch.lastTapX) + SDL_fabsf(py - s_touch.lastTapY) <= DOUBLE_TAP_RADIUS_PX;
			if (doubleTap) {
				s_touch.phase = TouchState::SELECT_PENDING;   // double-click or selection box -- decided below
				s_touch.lastTapUpTicks = 0;   // a triple tap must not re-enter the gesture
				break;
			}
			// Defer all BUTTON output: a finger landing could become a tap, a
			// pan, a long-press, or the first finger of a two-finger gesture.
			s_touch.phase = TouchState::PENDING;
			// Hover-highlight widgets before the tap commits (e.g. Challenge buttons).
			sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_MOTION, px, py);
		}
		else if (s_touch.phase == TouchState::PENDING || s_touch.phase == TouchState::PANNING ||
		         s_touch.phase == TouchState::SELECT_PENDING || s_touch.phase == TouchState::PLACE_MOVE) {
			s_touch.finger2 = event.tfinger.fingerID;
			s_touch.f2x = event.tfinger.x;
			s_touch.f2y = event.tfinger.y;
			s_touch.last2X = px;
			s_touch.last2Y = py;
			if (gxPlacementActive() &&
			    (s_touch.phase == TouchState::PENDING || s_touch.phase == TouchState::PLACE_MOVE)) {
				// Placement: anchor LMB at the building position (finger1); the second finger
				// sets the direction -- the engine rotates the building toward the "cursor".
				sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_MOTION, s_touch.lastX, s_touch.lastY);
				sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_BUTTON_DOWN,
				                   s_touch.lastX, s_touch.lastY, SDL_BUTTON_LEFT);
				sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_MOTION, px, py);
				s_touch.phase = TouchState::PLACE_ROTATE;
				GX_TOUCH_LOG("PLACE_ROTATE enter: anchor=(%.0f,%.0f) f2=(%.0f,%.0f) anchored=%d",
				             s_touch.lastX, s_touch.lastY, px, py,
				             TheInGameUI ? (int)TheInGameUI->isPlacementAnchored() : -1);
			} else {
				beginPan2(winW, winH);
			}
		}
		else if (s_touch.phase == TouchState::UI_DRAG || s_touch.phase == TouchState::SELECTING) {
			// Second finger while LMB is held: end the drag/box and switch to panning.
			s_touch.finger2 = event.tfinger.fingerID;
			s_touch.f2x = event.tfinger.x;
			s_touch.f2y = event.tfinger.y;
			sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_BUTTON_UP,
			                   s_touch.lastX, s_touch.lastY, SDL_BUTTON_LEFT);
			beginPan2(winW, winH);
		}
		// LONGPRESSED / PAN2 / PLACE_ROTATE with extra fingers: ignored
		break;

	case SDL_EVENT_FINGER_MOTION: {
		const float prevX = s_touch.lastX, prevY = s_touch.lastY;
		if (event.tfinger.fingerID == s_touch.finger1) {
			s_touch.f1x = event.tfinger.x;
			s_touch.f1y = event.tfinger.y;
			s_touch.lastX = px;
			s_touch.lastY = py;
		} else if ((s_touch.phase == TouchState::PAN2 || s_touch.phase == TouchState::PLACE_ROTATE) &&
		           event.tfinger.fingerID == s_touch.finger2) {
			s_touch.f2x = event.tfinger.x;
			s_touch.f2y = event.tfinger.y;
			s_touch.last2X = px;
			s_touch.last2Y = py;
		} else {
			break;
		}

		if (s_touch.phase == TouchState::PENDING && event.tfinger.fingerID == s_touch.finger1) {
			const float moved = SDL_fabsf(px - s_touch.downX) + SDL_fabsf(py - s_touch.downY);
			if (moved >= TAP_DEAD_ZONE_PX) {
				// Commit the gesture: placement -> the building follows the finger; world ->
				// pan; UI/menu -> the ordinary LMB drag (scrollbars, lists).
				if (gxPlacementActive() && !gxPointOverUI(s_touch.downX, s_touch.downY, winW, winH)) {
					s_touch.phase = TouchState::PLACE_MOVE;
					GX_TOUCH_LOG("PLACE_MOVE enter: (%.0f,%.0f)", px, py);
					sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_MOTION, px, py);
				} else if (gxWorldPanAllowed() && !gxPointOverUI(s_touch.downX, s_touch.downY, winW, winH)) {
					s_touch.phase = TouchState::PANNING;
					gxPanBetween(s_touch.downX, s_touch.downY, px, py, winW, winH);
				} else {
					sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_MOTION, s_touch.downX, s_touch.downY);
					sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_BUTTON_DOWN,
					                   s_touch.downX, s_touch.downY, SDL_BUTTON_LEFT);
					sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_MOTION, px, py);
					s_touch.phase = TouchState::UI_DRAG;
				}
			}
		}
		else if (s_touch.phase == TouchState::SELECT_PENDING && event.tfinger.fingerID == s_touch.finger1) {
			const float moved = SDL_fabsf(px - s_touch.downX) + SDL_fabsf(py - s_touch.downY);
			if (moved >= TAP_DEAD_ZONE_PX) {
				// The double tap became a selection box: the first click is not sent at all.
				sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_MOTION, s_touch.downX, s_touch.downY);
				sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_BUTTON_DOWN,
				                   s_touch.downX, s_touch.downY, SDL_BUTTON_LEFT);
				sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_MOTION, px, py);
				s_touch.phase = TouchState::SELECTING;
			}
		}
		else if (s_touch.phase == TouchState::PANNING && event.tfinger.fingerID == s_touch.finger1) {
			// Do not move the cursor: a finger at the edge must not start edge-scroll.
			gxPanBetween(prevX, prevY, px, py, winW, winH);
		}
		else if (s_touch.phase == TouchState::PLACE_MOVE && event.tfinger.fingerID == s_touch.finger1) {
			sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_MOTION, px, py);   // building follows the finger
		}
		else if (s_touch.phase == TouchState::PLACE_ROTATE && event.tfinger.fingerID == s_touch.finger2) {
			sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_MOTION, px, py);   // angle follows finger2
		}
		else if ((s_touch.phase == TouchState::UI_DRAG || s_touch.phase == TouchState::SELECTING) &&
		         event.tfinger.fingerID == s_touch.finger1) {
			sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_MOTION, px, py);
		}
		else if (s_touch.phase == TouchState::PAN2) {
			const float cx = (s_touch.f1x + s_touch.f2x) * 0.5f * (float)winW;
			const float cy = (s_touch.f1y + s_touch.f2y) * 0.5f * (float)winH;
			if (gxWorldPanAllowed())
				gxPanBetween(s_touch.panX, s_touch.panY, cx, cy, winW, winH);
			s_touch.panX = cx;
			s_touch.panY = cy;

			// 1:1 pinch zoom: camera height is inversely proportional to the finger
			// distance (the engine clamps it itself when m_zoomLimited)
			const float dx = (s_touch.f1x - s_touch.f2x) * (float)winW;
			const float dy = (s_touch.f1y - s_touch.f2y) * (float)winH;
			const float dist = SDL_sqrtf(dx * dx + dy * dy);
			if (s_touch.distStart > 1.0f && dist > 1.0f && s_touch.heightStart > 0.0f &&
			    TheTacticalView != nullptr && gxWorldPanAllowed()) {
				TheTacticalView->setHeightAboveGround(s_touch.heightStart * (s_touch.distStart / dist));
			}
		}
		break;
	}

	case SDL_EVENT_FINGER_UP:
	case SDL_EVENT_FINGER_CANCELED:
		if (event.tfinger.fingerID != s_touch.finger1 &&
		    !((s_touch.phase == TouchState::PAN2 || s_touch.phase == TouchState::PLACE_ROTATE) &&
		      event.tfinger.fingerID == s_touch.finger2)) {
			break;
		}
		switch (s_touch.phase) {
			case TouchState::PENDING:
				// A CANCELED touch (incoming call, notification shade, palm
				// rejection) must not become a committed tap — that would be a
				// phantom select/command/rally-point click at the cancel point.
				if (event.type == SDL_EVENT_FINGER_CANCELED) {
					break;
				}
				// Clean tap: the click goes out IMMEDIATELY (orders without delay); a double
				// tap is built ON TOP of that already-sent first click, as on desktop.
				sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_MOTION, s_touch.downX, s_touch.downY);
				sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_BUTTON_DOWN,
				                   s_touch.downX, s_touch.downY, SDL_BUTTON_LEFT);
				sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_BUTTON_UP,
				                   s_touch.downX, s_touch.downY, SDL_BUTTON_LEFT);
				s_touch.lastTapX = s_touch.downX;
				s_touch.lastTapY = s_touch.downY;
				s_touch.lastTapUpTicks = SDL_GetTicks();
				break;
			case TouchState::SELECT_PENDING:
				if (event.type == SDL_EVENT_FINGER_CANCELED) {
					break;
				}
				// Clean double tap (no movement, no hold):
				// on units -> one more LMB click (the first already went out, so the engine
				// sees a double click = all units of that type); on ground -> RMB (deselect).
				sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_MOTION, s_touch.downX, s_touch.downY);
				if (gxDrawableUnder(s_touch.downX, s_touch.downY, winW, winH)) {
					sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_BUTTON_DOWN,
					                   s_touch.downX, s_touch.downY, SDL_BUTTON_LEFT);
					sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_BUTTON_UP,
					                   s_touch.downX, s_touch.downY, SDL_BUTTON_LEFT);
				} else {
					sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_BUTTON_DOWN,
					                   s_touch.downX, s_touch.downY, SDL_BUTTON_RIGHT);
					sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_BUTTON_UP,
					                   s_touch.downX, s_touch.downY, SDL_BUTTON_RIGHT);
				}
				break;
			case TouchState::UI_DRAG:
			case TouchState::SELECTING:
				sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_BUTTON_UP, px, py, SDL_BUTTON_LEFT);
				break;
			case TouchState::PLACE_MOVE:
				// Lifting the finger in placement mode = place the building here.
				if (event.type == SDL_EVENT_FINGER_CANCELED) {
					break;
				}
				GX_TOUCH_LOG("PLACE commit: (%.0f,%.0f)", s_touch.lastX, s_touch.lastY);
				sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_MOTION, s_touch.lastX, s_touch.lastY);
				sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_BUTTON_DOWN,
				                   s_touch.lastX, s_touch.lastY, SDL_BUTTON_LEFT);
				sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_BUTTON_UP,
				                   s_touch.lastX, s_touch.lastY, SDL_BUTTON_LEFT);
				break;
			case TouchState::PLACE_ROTATE:
				// Lifting the rotate finger only locks the angle: release the anchor DIRECTLY
				// (the angle lives on the ghost -- m_placeIcon->getOrientation() -- and
				// survives this), and do NOT send LMB-up: without the anchor that click would
				// leak down the translator as an order. The remaining finger MOVES the
				// building again (PLACE_MOVE).
				if (TheInGameUI != nullptr)
					TheInGameUI->setPlacementStart(nullptr);
				if (event.tfinger.fingerID == s_touch.finger1 && s_touch.finger2 != 0) {
					// the holding finger was lifted -- the rotator becomes the holder
					s_touch.finger1 = s_touch.finger2;
					s_touch.f1x = s_touch.f2x;
					s_touch.f1y = s_touch.f2y;
					s_touch.lastX = s_touch.last2X;
					s_touch.lastY = s_touch.last2Y;
				}
				s_touch.finger2 = 0;
				s_touch.phase = TouchState::PLACE_MOVE;
				GX_TOUCH_LOG("PLACE_ROTATE exit: unanchored=%d holder=(%.0f,%.0f)",
				             TheInGameUI ? (int)!TheInGameUI->isPlacementAnchored() : -1,
				             s_touch.lastX, s_touch.lastY);
				return;   // do not reset to IDLE -- placement continues
			case TouchState::PAN2:
				// One finger lifted -- keep panning with the remaining one.
				if (event.tfinger.fingerID == s_touch.finger1) {
					s_touch.finger1 = s_touch.finger2;
					s_touch.f1x = s_touch.f2x;
					s_touch.f1y = s_touch.f2y;
				}
				s_touch.lastX = s_touch.f1x * (float)winW;
				s_touch.lastY = s_touch.f1y * (float)winH;
				s_touch.finger2 = 0;
				s_touch.phase = TouchState::PANNING;
				return;   // do not reset to IDLE
			default:
				break;
		}
		s_touch.phase = TouchState::IDLE;
		break;
	}
}

// Called once per engine frame (not just per touch event): a perfectly
// stationary finger produces no SDL events, so timers must be polled from the
// frame loop or they would never fire.
void updateTouchLongPress(SDL3Mouse *mouse, SDL_Window *window)
{
	const Uint64 now = SDL_GetTicks();

	if (s_touch.phase == TouchState::PENDING && (now - s_touch.downTicks) >= LONG_PRESS_MS) {
		// No LMB was sent yet (deferred), so this is a pure right-click.
		sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_MOTION, s_touch.downX, s_touch.downY);
		sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_BUTTON_DOWN,
		                   s_touch.downX, s_touch.downY, SDL_BUTTON_RIGHT);
		sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_BUTTON_UP,
		                   s_touch.downX, s_touch.downY, SDL_BUTTON_RIGHT);
		s_touch.phase = TouchState::LONGPRESSED;
	}
	else if (s_touch.phase == TouchState::SELECT_PENDING && (now - s_touch.downTicks) >= SELECT_HOLD_MS) {
		// The second tap is held in place -- switch to a selection box.
		sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_MOTION, s_touch.downX, s_touch.downY);
		sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_BUTTON_DOWN,
		                   s_touch.downX, s_touch.downY, SDL_BUTTON_LEFT);
		s_touch.phase = TouchState::SELECTING;
	}
}

#if defined(__ANDROID__)
// Debug multi-touch gesture injector for adb runs: /dev/input needs root and
// `adb shell input` cannot do two fingers. Twice a second this thread looks
// for <files>/gx_touch_script.txt (reachable via run-as on a debuggable APK),
// executes it line by line and deletes it. Format (window pixels):
//   d <fingerID> <x> <y>   finger down
//   m <fingerID> <x> <y>   finger moves
//   u <fingerID> <x> <y>   finger up
//   s <ms>                 pause
// Events go out via SDL_PushEvent by the same route as real ones.
void gxPushFinger(Uint32 type, Sint64 fingerId, float xPx, float yPx)
{
	SDL_Window *const *wins = SDL_GetWindows(nullptr);
	SDL_Window *win = wins != nullptr ? wins[0] : nullptr;
	if (win == nullptr)
		return;
	int w = 0, h = 0;
	SDL_GetWindowSize(win, &w, &h);
	if (w <= 0 || h <= 0)
		return;
	SDL_Event e;
	SDL_zero(e);
	e.tfinger.type = (SDL_EventType)type;
	e.tfinger.timestamp = SDL_GetTicksNS();
	e.tfinger.touchID = (SDL_TouchID)0x6758;   // 'gX' -- synthetic touchscreen
	e.tfinger.fingerID = (SDL_FingerID)fingerId;
	e.tfinger.x = xPx / (float)w;
	e.tfinger.y = yPx / (float)h;
	e.tfinger.pressure = 1.0f;
	e.tfinger.windowID = SDL_GetWindowID(win);
	SDL_PushEvent(&e);
}

int gxTouchScriptThread(void *)
{
	const char *base = SDL_GetAndroidInternalStoragePath();  // SDL-owned
	if (base == nullptr)
		return 0;
	char path[1024];
	snprintf(path, sizeof(path), "%s/gx_touch_script.txt", base);
	for (;;) {
		SDL_Delay(500);
		FILE *f = fopen(path, "r");
		if (f == nullptr)
			continue;
		GX_TOUCH_LOG("script: executing %s", path);
		char line[128];
		while (fgets(line, sizeof(line), f) != nullptr) {
			long long id = 0;
			float x = 0.0f, y = 0.0f;
			int ms = 0;
			if (sscanf(line, "d %lld %f %f", &id, &x, &y) == 3)
				gxPushFinger(SDL_EVENT_FINGER_DOWN, id, x, y);
			else if (sscanf(line, "m %lld %f %f", &id, &x, &y) == 3)
				gxPushFinger(SDL_EVENT_FINGER_MOTION, id, x, y);
			else if (sscanf(line, "u %lld %f %f", &id, &x, &y) == 3)
				gxPushFinger(SDL_EVENT_FINGER_UP, id, x, y);
			else if (sscanf(line, "s %d", &ms) == 1)
				SDL_Delay((Uint32)ms);
		}
		fclose(f);
		remove(path);
		GX_TOUCH_LOG("script: done");
	}
}
#endif // __ANDROID__

} // anonymous namespace
#endif // SAGE_MOBILE

namespace {

Bool DecodeNextUtf8Codepoint(const char* text, size_t length, size_t& offset, UnsignedInt& outCodepoint)
{
	outCodepoint = 0;
	if (!text || offset >= length) {
		return false;
	}

	const unsigned char first = static_cast<unsigned char>(text[offset]);
	if (first == 0) {
		return false;
	}

	if (first < 0x80) {
		outCodepoint = first;
		offset += 1;
		return true;
	}

	if ((first & 0xE0) == 0xC0 && offset + 1 < length) {
		const unsigned char second = static_cast<unsigned char>(text[offset + 1]);
		if ((second & 0xC0) == 0x80) {
			outCodepoint = ((first & 0x1F) << 6) | (second & 0x3F);
			offset += 2;
			return true;
		}
	}

	if ((first & 0xF0) == 0xE0 && offset + 2 < length) {
		const unsigned char second = static_cast<unsigned char>(text[offset + 1]);
		const unsigned char third = static_cast<unsigned char>(text[offset + 2]);
		if ((second & 0xC0) == 0x80 && (third & 0xC0) == 0x80) {
			outCodepoint = ((first & 0x0F) << 12) | ((second & 0x3F) << 6) | (third & 0x3F);
			offset += 3;
			return true;
		}
	}

	if ((first & 0xF8) == 0xF0 && offset + 3 < length) {
		const unsigned char second = static_cast<unsigned char>(text[offset + 1]);
		const unsigned char third = static_cast<unsigned char>(text[offset + 2]);
		const unsigned char fourth = static_cast<unsigned char>(text[offset + 3]);
		if ((second & 0xC0) == 0x80 && (third & 0xC0) == 0x80 && (fourth & 0xC0) == 0x80) {
			outCodepoint = ((first & 0x07) << 18) | ((second & 0x3F) << 12) | ((third & 0x3F) << 6) | (fourth & 0x3F);
			offset += 4;
			return true;
		}
	}

	// Invalid UTF-8 sequence: skip one byte and keep processing.
	offset += 1;
	return false;
}

}

/**
 * Constructor: Initialize SDL3 game engine state
 */
SDL3GameEngine::SDL3GameEngine()
	: GameEngine(),
	  m_SDLWindow(nullptr),
	  m_IsInitialized(false),
	  m_IsActive(false),
	  m_IsTextInputActive(false),
	  m_TextInputFocusWindow(nullptr)
{
	fprintf(stderr, "DEBUG: SDL3GameEngine::SDL3GameEngine() created\n");
}

/**
 * Destructor: Cleanup SDL3 resources
 */
SDL3GameEngine::~SDL3GameEngine()
{
	if (m_SDLWindow && m_IsTextInputActive) {
		SDL_StopTextInput(m_SDLWindow);
		m_IsTextInputActive = false;
		m_TextInputFocusWindow = nullptr;
	}

	if (m_IsInitialized) {
		// Window cleanup is done in reset/shutdown
	}
	fprintf(stderr, "DEBUG: SDL3GameEngine::~SDL3GameEngine() destroyed\n");
}

/**
 * From GameEngine: init() - initialize subsystems
 * 
 * GeneralsX @bugfix felipebraz 16/02/2026
 * Simplified to follow fighter19 pattern - SDL3/Vulkan initialized in SDL3Main.cpp
 * before GameEngine is created. This init() only delegates to parent GameEngine::init().
 * ApplicationHWnd and TheSDL3Window are already set by main() before this is called.
 */
void SDL3GameEngine::init(void)
{
	fprintf(stderr, "INFO: SDL3GameEngine::init() starting\n");

	if (TheGlobalData && TheGlobalData->m_headless) {
		// GeneralsX @bugfix Copilot 17/05/2026 Allow headless replay path to initialize engine subsystems without an SDL window.
		fprintf(stderr, "INFO: SDL3GameEngine::init() headless mode - skipping SDL window binding\n");
		m_SDLWindow = nullptr;
		m_IsInitialized = true;
		m_IsActive = true;
		GameEngine::init();
		return;
	}

	// Verify window was created by SDL3Main.cpp
	extern SDL_Window* TheSDL3Window;
	extern HWND ApplicationHWnd;
	
	if (!TheSDL3Window || !ApplicationHWnd) {
		fprintf(stderr, "FATAL: SDL3 window not initialized before GameEngine::init()\n");
		fprintf(stderr, "FATAL: TheSDL3Window=%p, ApplicationHWnd=%p\n", TheSDL3Window, ApplicationHWnd);
		return;
	}

	// Store window reference locally
	m_SDLWindow = TheSDL3Window;
	m_IsInitialized = true;
	m_IsActive = true;

#ifdef SAGE_MOBILE
	// Lifecycle events can fire outside the poll cycle on mobile; catch them
	// immediately so rendering halts before the process is suspended.
	SDL_AddEventWatch(iosLifecycleWatcher, nullptr);
#endif

	fprintf(stderr, "INFO: SDL3GameEngine using pre-initialized window\n");

	// Call parent init to initialize game subsystems
	GameEngine::init();
}

/**
 * From GameEngine: reset() - reset system to starting state
 */
void SDL3GameEngine::reset(void)
{
	fprintf(stderr, "DEBUG: SDL3GameEngine::reset()\n");
	if (m_SDLWindow && m_IsTextInputActive) {
		SDL_StopTextInput(m_SDLWindow);
		m_IsTextInputActive = false;
		m_TextInputFocusWindow = nullptr;
	}
	GameEngine::reset();
}

/**
 * From GameEngine: update() - per-frame update
 */
void SDL3GameEngine::update(void)
{
	pollSDL3Events();
#ifdef SAGE_MOBILE
	// Pause sim + render while backgrounded OR inactive (see iosLifecycleWatcher).
	// Acquiring a drawable in these windows fights the OS for the surface: on iOS
	// it stalls on the Metal layer and, across repeated suspend/switcher cycles,
	// crashes MoltenVK; on Android the ANativeWindow is destroyed and a present
	// call fails hard. Keep polling so we still catch the resume events; just
	// don't touch the GPU.
	if (iosShouldPauseRendering()) {
		SDL_Delay(50);
		return;
	}
#endif
	GameEngine::update();
}

/**
 * From GameEngine: execute() - main game loop
 */
void SDL3GameEngine::execute(void)
{
	fprintf(stderr, "INFO: SDL3GameEngine::execute() - entering main loop\n");
	GameEngine::execute();
	fprintf(stderr, "INFO: SDL3GameEngine::execute() - exited main loop\n");
}

/**
 * From GameEngine: serviceWindowsOS() - native OS service
 * On Linux, process SDL3 events
 */
void SDL3GameEngine::serviceWindowsOS(void)
{
#if defined(__ANDROID__) && defined(SAGE_MOBILE)
	// Debug gesture injector (see gxTouchScriptThread): started after SDL init,
	// once per process.
	static bool s_touchScriptStarted = false;
	if (!s_touchScriptStarted) {
		s_touchScriptStarted = true;
		SDL_DetachThread(SDL_CreateThread(gxTouchScriptThread, "gx-touch-script", nullptr));
	}
#endif
	pollSDL3Events();
}

/**
 * Check if game has OS focus
 */
Bool SDL3GameEngine::isActive(void)
{
	return m_IsActive;
}

/**
 * Set OS focus status
 */
void SDL3GameEngine::setIsActive(Bool isActive)
{
	m_IsActive = isActive;
}

/**
 * Poll and process SDL3 events
 * Handles keyboard, mouse, window, and quit events
 */
void SDL3GameEngine::pollSDL3Events(void)
{
	if (!m_SDLWindow) {
		return;
	}

	updateTextInputState();

	SDL_Event event;
	while (SDL_PollEvent(&event)) {
		switch (event.type) {
			case SDL_EVENT_QUIT:
				m_quitting = true;
				break;

			case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
				m_quitting = true;
				break;

			case SDL_EVENT_WINDOW_FOCUS_GAINED:
				m_IsActive = true;
				if (TheMouse) {
					TheMouse->regainFocus();
					TheMouse->refreshCursorCapture();
				}
				break;

			case SDL_EVENT_WINDOW_FOCUS_LOST:
				m_IsActive = false;
				if (m_IsTextInputActive) {
					SDL_StopTextInput(m_SDLWindow);
					m_IsTextInputActive = false;
					m_TextInputFocusWindow = nullptr;
				}
				if (TheMouse) {
					TheMouse->loseFocus();
				}
				break;

#ifdef SAGE_MOBILE
			// App suspension/resume: mirror the desktop focus handling so audio
			// and mouse state pause cleanly (the render gate lives in update()).
			case SDL_EVENT_DID_ENTER_BACKGROUND:
				m_IsActive = false;
				if (TheMouse) {
					TheMouse->loseFocus();
				}
				break;

			case SDL_EVENT_DID_ENTER_FOREGROUND:
				m_IsActive = true;
				if (TheMouse) {
					TheMouse->regainFocus();
					TheMouse->refreshCursorCapture();
				}
				break;
#endif

			case SDL_EVENT_WINDOW_MOUSE_ENTER:
				if (TheMouse) {
					TheMouse->onCursorMovedInside();
				}
				break;

			case SDL_EVENT_WINDOW_MOUSE_LEAVE:
				if (TheMouse) {
					TheMouse->onCursorMovedOutside();
				}
				break;

			case SDL_EVENT_KEY_DOWN:
			case SDL_EVENT_KEY_UP:
				// Fighter19 pattern: direct addSDLEvent() call
				// GeneralsX @refactor felipebraz 16/02/2026 Simplified event routing
				if (TheKeyboard) {
					SDL3Keyboard* keyboard = dynamic_cast<SDL3Keyboard*>(TheKeyboard);
					if (keyboard) {
						keyboard->addSDLEvent(&event);
					}
				}
				break;

			case SDL_EVENT_TEXT_INPUT:
				forwardTextInputEvent(event.text.text);
				break;

			case SDL_EVENT_MOUSE_MOTION:
			case SDL_EVENT_MOUSE_BUTTON_DOWN:
			case SDL_EVENT_MOUSE_BUTTON_UP:
			case SDL_EVENT_MOUSE_WHEEL:
#ifdef SAGE_MOBILE
				// Belt-and-braces: drop SDL's own touch-synthesized mouse events.
				// The gesture translator owns all touch->mouse conversion; double
				// delivery would produce phantom second clicks.
				if (event.motion.which == SDL_TOUCH_MOUSEID) {
					break;
				}
#endif
				// Fighter19 pattern: direct addSDLEvent() call with raw SDL_Event
				// GeneralsX @refactor felipebraz 16/02/2026 Simplified event routing
				if (TheMouse) {
					SDL3Mouse* mouse = dynamic_cast<SDL3Mouse*>(TheMouse);
					if (mouse) {
						mouse->addSDLEvent(&event);
					}
				}
				break;

#ifdef SAGE_MOBILE
			case SDL_EVENT_FINGER_DOWN:
			case SDL_EVENT_FINGER_MOTION:
			case SDL_EVENT_FINGER_UP:
			case SDL_EVENT_FINGER_CANCELED:
				if (TheMouse && m_SDLWindow) {
					SDL3Mouse* mouse = dynamic_cast<SDL3Mouse*>(TheMouse);
					if (mouse) {
						handleTouchEvent(mouse, m_SDLWindow, event);
					}
				}
				break;
#endif

			case SDL_EVENT_WINDOW_RESIZED:
				handleWindowEvent(event.window);
				break;

			default:
				// Ignore other events for now
				break;
		}

		updateTextInputState();
	}

#ifdef SAGE_MOBILE
	// Poll the long-press timer every frame; a stationary finger emits no events.
	if (TheMouse && m_SDLWindow) {
		SDL3Mouse* touchMouse = dynamic_cast<SDL3Mouse*>(TheMouse);
		if (touchMouse) {
			updateTouchLongPress(touchMouse, m_SDLWindow);
		}
	}
#endif
}

// GeneralsX @bugfix felipebraz 01/04/2026 Enable SDL text input only while an entry gadget owns focus.
void SDL3GameEngine::updateTextInputState(void)
{
	if (!m_SDLWindow || !TheWindowManager) {
		return;
	}

	GameWindow* focusedWindow = TheWindowManager->winGetFocus();
	const Bool wantsTextInput =
		focusedWindow != nullptr && BitIsSet(focusedWindow->winGetStyle(), GWS_ENTRY_FIELD);

	if (wantsTextInput) {
		if (!m_IsTextInputActive) {
			if (SDL_StartTextInput(m_SDLWindow)) {
				m_IsTextInputActive = true;
			}
		}
		m_TextInputFocusWindow = focusedWindow;
	} else {
		if (m_IsTextInputActive) {
			SDL_StopTextInput(m_SDLWindow);
			m_IsTextInputActive = false;
		}
		m_TextInputFocusWindow = nullptr;
	}
}

// GeneralsX @bugfix felipebraz 01/04/2026 Forward SDL UTF-8 text input through existing GWM_IME_CHAR path.
void SDL3GameEngine::forwardTextInputEvent(const char* utf8Text)
{
	if (!utf8Text || !TheWindowManager) {
		return;
	}

	// GeneralsX @bugfix felipebraz 01/04/2026 Use tracked text-input focus window to keep SDL text delivery stable.
	GameWindow* targetWindow = m_TextInputFocusWindow;
	if (!targetWindow || !BitIsSet(targetWindow->winGetStyle(), GWS_ENTRY_FIELD)) {
		return;
	}

	const size_t textLength = strlen(utf8Text);
	size_t offset = 0;
	while (offset < textLength) {
		UnsignedInt codepoint = 0;
		if (!DecodeNextUtf8Codepoint(utf8Text, textLength, offset, codepoint)) {
			continue;
		}

		// GeneralsX @bugfix felipebraz 01/04/2026 Clamp IME char forwarding to BMP and reject UTF-16 surrogate range.
		if (codepoint == 0 || codepoint > 0x10FFFFU) {
			continue;
		}

		if (codepoint >= 0xD800U && codepoint <= 0xDFFFU) {
			continue;
		}

		if (codepoint > 0xFFFFU) {
			continue;
		}

		const WideChar wideCharacter = static_cast<WideChar>(codepoint);
		TheWindowManager->winSendInputMsg(targetWindow, GWM_IME_CHAR, static_cast<WindowMsgData>(wideCharacter), 0);
	}
}

/**
 * Handle keyboard event -dispatch to Keyboard manager
 * TheSuperHackers @build 10/02/2026 BenderAI - Phase 1.5 event wiring
 */
void SDL3GameEngine::handleKeyboardEvent(const SDL_KeyboardEvent& event)
{
	// Dispatch to SDL3Keyboard if available
	if (TheKeyboard) {
		SDL3Keyboard* sdlKeyboard = dynamic_cast<SDL3Keyboard*>(TheKeyboard);
		if (sdlKeyboard) {
			sdlKeyboard->addSDL3KeyEvent(event);
		}
	}
}

/**
 * Handle mouse motion event - dispatch to Mouse manager
 * TheSuperHackers @build 10/02/2026 BenderAI - Phase 1.5 event wiring
 */
void SDL3GameEngine::handleMouseMotionEvent(const SDL_MouseMotionEvent& event)
{
	// Dispatch to SDL3Mouse if available
	if (TheMouse) {
		SDL3Mouse* sdlMouse = dynamic_cast<SDL3Mouse*>(TheMouse);
		if (sdlMouse) {
			sdlMouse->addSDL3MouseMotionEvent(event);
		}
	}
}

/**
 * Handle mouse button event - dispatch to Mouse manager
 * TheSuperHackers @build 10/02/2026 BenderAI - Phase 1.5 event wiring
 */
void SDL3GameEngine::handleMouseButtonEvent(const SDL_MouseButtonEvent& event)
{
	// Dispatch to SDL3Mouse if available
	if (TheMouse) {
		SDL3Mouse* sdlMouse = dynamic_cast<SDL3Mouse*>(TheMouse);
		if (sdlMouse) {
			sdlMouse->addSDL3MouseButtonEvent(event);
		}
	}
}

/**
 * Handle mouse wheel event - dispatch to Mouse manager
 * TheSuperHackers @build 10/02/2026 BenderAI - Phase 1.5 event wiring
 */
void SDL3GameEngine::handleMouseWheelEvent(const SDL_MouseWheelEvent& event)
{
	// Dispatch to SDL3Mouse if available
	if (TheMouse) {
		SDL3Mouse* sdlMouse = dynamic_cast<SDL3Mouse*>(TheMouse);
		if (sdlMouse) {
			sdlMouse->addSDL3MouseWheelEvent(event);
		}
	}
}

/**
 * Handle window event (resize, etc.)
 */
void SDL3GameEngine::handleWindowEvent(const SDL_WindowEvent& event)
{
	// TODO: Phase 2 - Handle window resize, notify graphics subsystem
	// fprintf(stderr, "DEBUG: Window event (type=%d)\n", event.type);
}

/**
 * Factory Methods for GameEngine subsystems
 * TheSuperHackers @build felipebraz 13/02/2026
 * Implementations in .cpp to provide complete type definitions and avoid circular includes
 */

LocalFileSystem *SDL3GameEngine::createLocalFileSystem(void)
{
	fprintf(stderr, "INFO: SDL3GameEngine::createLocalFileSystem() -> StdLocalFileSystem\n");
	return NEW StdLocalFileSystem;
}

ArchiveFileSystem *SDL3GameEngine::createArchiveFileSystem(void)
{
	fprintf(stderr, "INFO: SDL3GameEngine::createArchiveFileSystem() -> StdBIGFileSystem\n");
	return NEW StdBIGFileSystem;
}

GameLogic *SDL3GameEngine::createGameLogic(void)
{
	fprintf(stderr, "INFO: SDL3GameEngine::createGameLogic() -> W3DGameLogic\n");
	return NEW W3DGameLogic;
}

GameClient *SDL3GameEngine::createGameClient(void)
{
	fprintf(stderr, "INFO: SDL3GameEngine::createGameClient() -> W3DGameClient\n");
	return NEW W3DGameClient;
}

ModuleFactory *SDL3GameEngine::createModuleFactory(void)
{
	fprintf(stderr, "INFO: SDL3GameEngine::createModuleFactory() -> W3DModuleFactory\n");
	return NEW W3DModuleFactory;
}

ThingFactory *SDL3GameEngine::createThingFactory(void)
{
	fprintf(stderr, "INFO: SDL3GameEngine::createThingFactory() -> W3DThingFactory\n");
	return NEW W3DThingFactory;
}

FunctionLexicon *SDL3GameEngine::createFunctionLexicon(void)
{
	fprintf(stderr, "INFO: SDL3GameEngine::createFunctionLexicon() -> W3DFunctionLexicon\n");
	return NEW W3DFunctionLexicon;
}

// GeneralsX @bugfix Copilot 15/04/2026 Match upstream GameEngine pure-virtual signature after sync.
Radar *SDL3GameEngine::createRadar(Bool dummy)
{
	// GeneralsX @bugfix fbraz 04/05/2026 Respect headless mode and create dummy radar.
	// Upstream reference: Win32GameEngine headless factory behavior, TheSuperHackers/GeneralsGameCode
	// https://github.com/TheSuperHackers/GeneralsGameCode
	if (dummy) {
		fprintf(stderr, "INFO: SDL3GameEngine::createRadar() -> RadarDummy (headless)\n");
		return NEW RadarDummy;
	}
	fprintf(stderr, "INFO: SDL3GameEngine::createRadar() -> W3DRadar\n");
	return NEW W3DRadar;
}

// GeneralsX @bugfix Copilot 24/03/2026 Match upstream GameEngine pure-virtual signature after sync.
ParticleSystemManager* SDL3GameEngine::createParticleSystemManager(Bool dummy)
{
	// GeneralsX @bugfix fbraz 04/05/2026 Respect headless mode and create dummy particle manager.
	if (dummy) {
		fprintf(stderr, "INFO: SDL3GameEngine::createParticleSystemManager() -> ParticleSystemManagerDummy (headless)\n");
		return NEW ParticleSystemManagerDummy;
	}
	fprintf(stderr, "INFO: SDL3GameEngine::createParticleSystemManager() -> W3DParticleSystemManager\n");
	return NEW W3DParticleSystemManager;
}

WebBrowser *SDL3GameEngine::createWebBrowser(void)
{
	// WebBrowser uses Windows COM (CComObject<W3DWebBrowser>)
	// Not available on Linux - return nullptr
	fprintf(stderr, "WARNING: WebBrowser not available on Linux platform\n");
	return nullptr;
}

/**
 * Factory method: AudioManager
 * Select audio backend based on compile flags
 * GeneralsX @bugfix Copilot 15/04/2026 Match upstream GameEngine pure-virtual signature after sync.
 */
AudioManager *SDL3GameEngine::createAudioManager(Bool dummy)
{
	(void)dummy;
	fprintf(stderr, "INFO: SDL3GameEngine::createAudioManager()\n");

#ifdef SAGE_USE_OPENAL
	fprintf(stderr, "INFO: Creating OpenAL audio backend\n");
	return new OpenALAudioManager();
#else
	fprintf(stderr, "INFO: Audio backend not available (SAGE_USE_OPENAL not defined)\n");
	fprintf(stderr, "WARNING: Falls back to parent implementation or silent mode\n");
	return GameEngine::createAudioManager();  // Call parent (may return stub)
#endif
}

#endif // !_WIN32

