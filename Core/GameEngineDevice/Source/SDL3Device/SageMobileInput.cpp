/*
** SageMobileInput.cpp -- see SageMobileInput.h.
**
** GeneralsX @refactor android-port 08/02/2026 Moved out of GeneralsMD's
** SDL3GameEngine.cpp so the Generals engine gets the identical implementation.
*/

#include "SDL3Device/SageMobileInput.h"

#ifdef SAGE_MOBILE

#include "SDL3Device/GameClient/SDL3Mouse.h"
#include "GameClient/Mouse.h"
#include "GameClient/View.h"
#include "GameClient/Display.h"
#include "GameClient/InGameUI.h"
#include "GameClient/GameWindowManager.h"
#include "GameLogic/GameLogic.h"
#include <SDL3/SDL.h>
#include <cstdio>
#include <cstring>

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

bool SageMobile_ShouldPauseRendering()
{
	return s_appBackgrounded.load() || s_appInactive.load();
}

bool SDLCALL SageMobile_LifecycleWatcher(void *userdata, SDL_Event *event)
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
// (file-local helpers below; entry points have external linkage)
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

void SageMobile_HandleTouchEvent(SDL3Mouse *mouse, SDL_Window *window, const SDL_Event &event)
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
void SageMobile_UpdateTouchLongPress(SDL3Mouse *mouse, SDL_Window *window)
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

int SageMobile_TouchScriptThread(void *)
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



/**
 * Match the game's internal resolution to the screen.
 *
 * Without this the engine runs its 4:3 default inside a wide mobile display:
 * pillarboxed picture and a skewed window->game coordinate mapping. Applied by
 * appending -xres/-yres to argv so the normal command-line path handles it;
 * flags the user passed explicitly still win, so nothing is added then.
 *
 * GeneralsX @refactor android-port 08/02/2026 Moved out of GeneralsMD's
 * SDL3Main.cpp, where only Zero Hour could reach it -- Generals letterboxed.
 */
void SageMobile_ApplyNativeResolution(SDL_Window *window, int &argc, char **&argv)
{
	if (window == nullptr) {
		return;
	}
	for (int i = 1; i < argc; ++i) {
		if (argv[i] != nullptr &&
		    (strcmp(argv[i], "-xres") == 0 || strcmp(argv[i], "-yres") == 0)) {
			return;   // user chose a resolution; leave it alone
		}
	}

	// Pixel size of the high-density drawable: the game renders 1:1 into the
	// native swapchain and the UI rescales via the engine's font scaling.
	int winW = 0, winH = 0;
	SDL_GetWindowSizeInPixels(window, &winW, &winH);
	if (winW <= 0 || winH <= 0 || winW <= winH) {
		return;
	}

	static char xresVal[16], yresVal[16];
	static char xresFlag[] = "-xres";
	static char yresFlag[] = "-yres";
	const int yres = winH;
	int xres = winW;
	xres &= ~1;   // keep it even
	snprintf(xresVal, sizeof(xresVal), "%d", xres);
	snprintf(yresVal, sizeof(yresVal), "%d", yres);

	static char *newArgv[64];
	int n = 0;
	for (int i = 0; i < argc && n < 59; ++i) {
		newArgv[n++] = argv[i];
	}
	newArgv[n++] = xresFlag;
	newArgv[n++] = xresVal;
	newArgv[n++] = yresFlag;
	newArgv[n++] = yresVal;
	newArgv[n] = nullptr;
	argv = newArgv;
	argc = n;
	fprintf(stderr, "INFO: Mobile internal resolution set to %sx%s (window %dx%d)\n",
	        xresVal, yresVal, winW, winH);
}

#endif // SAGE_MOBILE
