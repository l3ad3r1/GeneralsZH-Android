# Android input: touch gestures and mouse

How touchscreen and mouse input reach a 2003 RTS engine that assumes a
two-button mouse with a cursor that exists even when nothing is pressed.

Everything here was verified on physical hardware (TCL NXTPAPER 9469X,
Mali-G57, Android 14, with a Rapoo Bluetooth mouse). Where a claim is *not*
verified on device it says so.

---

## 1. The gesture scheme

Ported from [wingear's GLES fork](https://github.com/wingear/GeneralsZH-Android-OpenGL-ES)
(GPL-3.0, commits "touch v2/v3/v4"). **The design credit is theirs.** It is
renderer-independent, so it applies unchanged to our DXVK build. Our copy
translates the comments to English and restores the `SAGE_USE_OPENAL` include
guard their build does not use.

| Gesture | Action |
|---|---|
| Tap (world) | Left click, sent **immediately** |
| Tap (UI/menu) | Left click immediately — buttons must not lag |
| Drag one finger (world) | Pan the map; the ground tracks your finger 1:1 |
| Drag one finger (over UI) | Ordinary left-drag, for scrollbars and lists |
| Double tap on a unit | Select every unit of that type on screen |
| Double tap on ground | Right click — deselect |
| Double tap, then hold and drag | Selection box; the first click is swallowed |
| Long press (600 ms), finger still | Right click |
| Two-finger drag | Pan; **pinch** zooms 1:1 |
| Placement mode | One finger positions the building, a second rotates it toward that point; lift both to place |

Implementation: `GeneralsMD/Code/GameEngineDevice/Source/SDL3GameEngine.cpp`,
the `TouchState` machine and `handleTouchEvent()`.

Details that matter:

- **Taps fire instantly.** A double tap is detected *on top of* an
  already-delivered first click, exactly as on desktop. Deferring the first
  click to wait out a double-tap window adds perceptible lag to every order.
- **Panning moves the camera opposite to the finger** on purpose, so the world
  point under your finger stays under it. Both finger positions are projected
  onto the terrain (`screenToTerrain`) and the camera moves by the difference,
  which stays exact at any zoom.
- **Pinch zoom uses `getHeightAboveGround()`**, not `m_zoom`. `m_zoom` is a
  *desired* value on a different scale; the mismatch made zoom jerk on the first
  movement of the fingers.
- **Panning is gated** on being in a real match (`isInGame() &&
  !isInShellGame()`) and on the gesture not starting over a UI window. Shell
  menus drive the camera from scripts.

---

## 2. Mouse support, and four traps

A mouse is not simply "touch with better precision" on Android. Every item
below was a real regression introduced by landing the gesture layer, because
that layer was written and tested on a touch-only phone — so everything it
assumed about mice was untested.

### 2.1 A real mouse also generates finger events

`SDL_HINT_MOUSE_TOUCH_EVENTS` defaults to **`"1"` on mobile**. A USB or
Bluetooth mouse therefore emits `SDL_EVENT_FINGER_*` *in addition to* its normal
mouse events. Those fingers reach the gesture translator, where a drag is a
one-finger map pan — and a pan deliberately moves the camera opposite to the
finger. The result is a mouse that appears to move **backwards**, plus every
click delivered twice.

Fixed in two places (`SDL3Main.cpp`, `SDL3GameEngine.cpp`):

```c
SDL_SetHint(SDL_HINT_MOUSE_TOUCH_EVENTS, "0");   // mouse must not synthesise touch
...
if (event.tfinger.touchID == SDL_MOUSE_TOUCHID)  // belt-and-braces guard
    return;
```

Note this is the mirror of `SDL_HINT_TOUCH_MOUSE_EVENTS`, which we set to `"0"`
so that touch does not synthesise mouse events either. **Both directions must be
off.** The gesture translator owns every touch-to-mouse conversion.

### 2.2 `SDL_HasMouse()` lies on Android

Measured on device with a Rapoo Bluetooth mouse connected *and actively moving*:

```
physical mouse latched: which=0 type=1024 SDL_HasMouse=0
```

`SDL_HasMouse()` returns **0**. Anything gated on it silently disables itself
for mouse users. Edge-of-screen camera scrolling was gated on it and was dead.

### 2.3 A real mouse reports `which == 0`

Also visible above: the genuine mouse carries `SDL_MouseID` **0** — the same
value a zero-initialised synthetic event carries. So "nonzero device id means
real hardware" does not work either; it was the first fix attempted and it
failed.

The working discrimination is the other way round. The gesture translator tags
its own output as `SDL_TOUCH_MOUSEID` (honest — those events *are*
touch-derived), and `SDL3Mouse::addSDLEvent()` treats anything **not** so tagged
as real hardware:

```c
if (which != SDL_TOUCH_MOUSEID)
    m_sawPhysicalMouse = true;
```

`isPhysicalMousePresent()` then returns `SDL_HasMouse() || m_sawPhysicalMouse`.
It **latches** deliberately: a mouse that goes idle for a moment must not make
the camera stop scrolling mid-drag.

Only the SDL event loop discards `SDL_TOUCH_MOUSEID` events, and synthetic
events are handed straight to `addSDLEvent()`, so tagging them cannot cause them
to be dropped.

### 2.4 Right-click arrives as the BACK button

On Android a mouse right-click is delivered to the app as `BACK`, not as
`SDL_BUTTON_RIGHT`, unless BACK is trapped. Without the trap the engine never
sees a right click at all: in the default control scheme that silently kills
deselect, and with **Alternate Mouse Setup** enabled — where orders live on the
right button — it removes the ability to give orders entirely.

```c
SDL_SetHint(SDL_HINT_ANDROID_TRAP_BACK_BUTTON, "1");
```

SDL documents this hint as exactly this fix. Trapping also stops the system BACK
gesture from backgrounding the game mid-match; it arrives as
`SDL_SCANCODE_AC_BACK`, which the engine's keyboard translator does not bind, so
it is ignored.

### 2.5 Edge scrolling is gated on a real mouse

On a touch device the cursor "sticks" wherever the last tap landed. A tap near
the screen edge therefore left the camera scrolling forever. `LookAtXlat.cpp`
gates `SCROLL_SCREENEDGE` on `TheMouse->isPhysicalMousePresent()` — which is why
§2.2 and §2.3 had to be correct before edge scrolling worked at all.

---

## 3. Verified on device

| Behaviour | Status |
|---|---|
| Touch gestures (tap, pan, pinch, double-tap, placement) | ✅ confirmed in gameplay |
| Tap to skip intro / cutscene | ✅ measured: menu at 80 s untouched, 19 s when tapped at 15 s |
| Mouse camera control | ✅ confirmed |
| Mouse left click | ✅ confirmed |
| Mouse edge scrolling | ✅ confirmed |
| Mouse right click (BACK trap) | ⚠️ implemented and reasoned from SDL's own documentation; not explicitly confirmed in gameplay |

---

## 4. If you touch this code

- Never assume a hint's default is the same on desktop and mobile. Both
  touch↔mouse synthesis hints default *on* for mobile.
- Never gate a feature on `SDL_HasMouse()` on Android.
- Never use `SDL_MouseID == 0` to mean "synthetic".
- Test with a mouse attached. Four separate regressions here reached a shipped
  build because touch was tested and mouse was not.
