# Handover: Music/Voice audio + loading-screen video not working on Android

**For:** Gemini (implementation agent)
**From:** Claude (diagnosis session, 2026-07-31)
**Repo:** https://github.com/l3ad3r1/GeneralsZH-Android
**Working dir on PC:** `E:\claude-projects\GeneralsZH-Android-Debug`
**Context:** the TCL NXTPAPER Mali port (`v0.4-mali`, see `HANDOVER_TCL_NXTPAPER.md` and `docs/WORKDIR/phases/PHASE07_TCL_SWIFTSHADER_PORT.md`) is verified stable — full missions play at 30 FPS. This is the next bug: **music, voice/EVA lines, and loading-screen video are all non-functional**, while short SFX and UI click sounds work correctly. Confirmed on the TCL; almost certainly also affects the S24 Ultra build (this is pre-existing, not TCL/Mali-specific — see `docs/DEV_BLOG` history, audio has been a known gap since the first Android build).

## 0. START HERE

**Symptom, precisely:** on real hardware, with the user actually playing —
- ✅ UI clicks and menu sounds play
- ✅ SFX (explosions, gunfire, unit acknowledgment blips) play
- ❌ Background/menu music is silent
- ❌ Unit voice lines and EVA announcer are silent
- ❌ Mission-briefing / loading-screen video does not animate (may show a static frame — see §5)

**The single most important fact I found:** this build has `DEBUG_LOGGING` compiled out
entirely. I confirmed this by checking the actual bytes of the shipped `libmain.so` —
every diagnostic string used by the relevant code (`"Received audio frame"`,
`"Failed to open file:"`, `"Playing 3D sample"`, etc.) is **absent from the binary**,
not just silent at runtime. This means **no black-box, non-rebuild technique can get
further evidence.** I spent a large amount of effort trying (see §2) and it was a
real, hard wall. **Your first and most valuable move is enabling `DEBUG_LOGGING` for a
diagnostic rebuild** (see §4) — one test run with real logs will very likely settle
this in minutes where I spent hours black-box.

## 1. Why this is one bug, not several

Music, voice, and video-cutscene audio all reach the speaker through the **exact
same class**, `OpenALAudioStream` (`Core/GameEngineDevice/Source/OpenALAudioDevice/OpenALAudioStream.cpp`),
via two different entry points:
- `OpenALAudioManager::playStream()` → for `AT_Music` and `AT_Streaming` (long voice lines) sound-type events
- `TheAudio->getHandleForBink()`, consumed directly by `FFmpegVideoPlayer.cpp` → for video cutscene audio

Short SFX and UI sounds use a **completely different, working path**:
`OpenALAudioFileCache::decodeFFmpeg()` (full-file decode into a static buffer) +
`OpenALAudioManager::playSample()`/`playSample3D()`.

So the working/broken split lines up exactly with "goes through `OpenALAudioStream`"
vs. "doesn't." That's the bug's location, even without knowing the exact line yet.

## 2. What I ruled out, with hard evidence (do not re-derive these)

I could not get live debug output (see above), so everything below was proven via
static source reading and targeted native test harnesses run directly on the TCL
(build/push/run tools are all documented working on this PC — see
`HANDOVER_TCL_NXTPAPER.md` §6).

1. **Missing ffmpeg decoder.** Extracted the actual `libavcodec.so` from the shipped
   APK and found the literal `ffmpeg configure` command string baked into the binary:
   `--enable-decoder='pcm_s16le,pcm_s8,pcm_u8,pcm_alaw,pcm_mulaw,adpcm_ima_wav,
   adpcm_ms,mp3float,mp3'`. Both MP3 (music's real codec — verified via `ffprobe` on
   the actual extracted `MusicZH.big` track files) and PCM (speech/SFX's codec) are
   compiled in. **Ruled out.**
2. **OpenAL/Oboe can't handle stereo.** Speech is stereo PCM, music is stereo MP3,
   working SFX is mono PCM — a channel-count bug was the obvious first suspect. I
   built and ran two escalating native tests directly against the bundled
   `libopenal.so`:
   - A bare `adb shell` executable (`alprobe.c`, in repo root as reference) — reported
     `AL_NO_ERROR`/`AL_PLAYING` for both mono and stereo test tones, but **nothing was
     audible for either** — this turned out to be a dead end: Android's audio policy
     appears to silently drop output from bare shell-UID processes with no foreground
     Activity, regardless of API-level success. Don't trust bare-shell OpenAL tests on
     this device.
   - A proper minimal installed APK (`me.generalsx.audiotest`, built by hand with
     `javac`+`d8`+`aapt2`+`zipalign`+`apksigner` — no Gradle needed, see
     `HANDOVER_TCL_NXTPAPER.md` §6 for why Gradle isn't available on this PC) with a
     real `Activity` and JNI native code linking the same `libopenal.so`. **The user
     directly confirmed hearing BOTH the mono and the stereo test tone, clearly and
     equally**, when this ran as a real foregrounded app. **Ruled out, with the
     strongest evidence in this whole investigation — stereo audio is fully
     functional on this device's OpenAL/Oboe stack.**
3. **`FFmpegFile` decode/open has a channel-specific bug.** Read
   `Core/GameEngineDevice/Source/VideoDevice/FFmpeg/FFmpegFile.cpp::open()` in full —
   it's generic `avformat_open_input`/`avcodec_open2` setup with no branching on
   channel count or codec type anywhere. **Ruled out** (doesn't mean decode never
   fails for other reasons — just not a channel-count-specific bug in this function).
4. **Wrong archive path construction (case-sensitivity etc.).** Extracted
   `AudioSettings.ini` from the real device's `INIZH.big` and confirmed
   `AudioRoot = Data\Audio`, `MusicFolder = Tracks`, `StreamingFolder = Speech`,
   `SoundsFolder = Sounds`. Built the exact same concatenation
   `AudioEventRTS::generateFilenamePrefix()` does
   (`Core/GameEngine/Source/Common/Audio/AudioEventRTS.cpp:762`) and it is
   **byte-for-byte identical** to the real internal archive paths I extracted
   directly from the `.big` files (`Data\Audio\Tracks\CHI_10.mp3`,
   `Data\Audio\Speech\ccruise01.wav`). Same generic logic is used for SFX too.
   **Ruled out.**
5. **Missing/corrupt archive files on device.** `ls -la` on the device confirmed
   `Music.big`, `MusicZH.big`, `Speech.big`, `SpeechZH.big`, `SpeechEnglishZH.big` all
   present at their correct original sizes with correct `777` permissions (same as
   every other working archive). **Ruled out.**

## 3. What I could NOT rule out — the two live candidates

### 3a. Streaming buffer-queue priming race in `OpenALAudioStream`
`OpenALAudioManager::playStream()` calls `stream->play()` (i.e. `alSourcePlay`)
**immediately** on a freshly-created stream, **before any buffer has ever been
queued** (buffer queueing only happens later, driven by `m_requireDataCallback`,
which is first invoked from `OpenALAudioStream::update()` — and `update()` is only
called once per frame from the main `OpenALAudioManager` update loop, i.e. one frame
*after* `playStream()` already ran).

On paper this self-heals: the restart-guard at the top of `update()`
(`if ((sourceState == AL_STOPPED || AL_INITIAL || AL_PAUSED) && num_queued > 0 && ...)  play();`)
should call `play()` again once buffers exist, one frame later. But I could not verify
this actually happens correctly on this device/Oboe backend — it's exactly the kind
of one-frame-early-state read that's fragile in practice even when it looks correct
on paper, and the class has many dated bugfix comments already fighting closely
related race conditions (`OpenALAudioStream.cpp`, look for "GeneralsX @bugfix" dated
22/04, 04/07, 14/06, 11/03/2026 — this code has a real history of subtle bugs exactly
like this).

**Where to look:** `Core/GameEngineDevice/Source/OpenALAudioDevice/OpenALAudioStream.cpp::update()`
and the setup call site in `Core/GameEngineDevice/Source/OpenALAudioDevice/OpenALAudioManager.cpp`
around line 810 (`stream = new OpenALAudioStream; stream->setRequireDataCallback(...); ...`)
through line 875 (`playStream(event, stream);`).

### 3b. Volume/gain never gets its *first* correct value
`OpenALAudioManager::adjustPlayingVolume()` (line 1339) correctly computes and applies
`m_musicVolume`/`m_speechVolume * desiredVolume` via `alSourcef(..., AL_GAIN, ...)` —
but it's only called from the per-frame stream loop **gated behind
`if (m_volumeHasChanged)`** (line ~2531). The `OpenALAudioStream` constructor sets a
reasonable default (`AL_GAIN = 1.0`), so this alone shouldn't cause total silence —
but I could not verify `m_musicVolume`/`m_speechVolume` are sane, non-zero values at
the point a stream is *first* created, nor find anywhere that explicitly applies the
correct initial gain to a brand-new stream source before the first
`m_volumeHasChanged`-gated update. Lower-confidence than 3a, but check it while you're
in this code.

## 4. Recommended first move: enable `DEBUG_LOGGING` and rebuild

This is a compile-time-only flag — no logic changes needed to get real diagnostics.
From `Core/GameEngine/Include/Common/Debug.h`:

```c
#define NO_RELEASE_DEBUG_LOGGING
#ifdef RELEASE_DEBUG_LOGGING  // <-- define this to flip DEBUG_LOGGING on
    #define ALLOW_DEBUG_UTILS 1
    #define DEBUG_LOGGING 1
    ...
#endif
```

Add `target_compile_definitions(... PRIVATE RELEASE_DEBUG_LOGGING)` to the Android
build target (or pass `-DRELEASE_DEBUG_LOGGING` via the CMake preset used for the
Mali/TCL build), rebuild `libmain.so`, repackage exactly as the Mali build already
does (signing/`chmod`/GameData rules are identical — see `HANDOVER_TCL_NXTPAPER.md`
§5 and §8.2, they still apply), install, and capture
`/sdcard/Android/data/me.generalsx.zh/files/generals-stderr.log` (pull with
`adb pull`, **not** `adb shell cat` — it's app-owned, `cat` via shell gets
"Permission denied"; `pull` works fine) during three scenarios:

1. Sit at the main menu 20+ seconds (music should be playing) — grep the log for
   `Failed to open file`, `Failed to open FFmpeg file`, `Received audio frame`,
   `Playing 3D sample`, `Having N buffers queued`, `buffers have been processed`.
2. Play far enough into a mission/skirmish to trigger a unit voice acknowledgment or
   an EVA line.
3. Trigger a mission-briefing/loading screen (per the user, video does not animate
   here either — capture whatever `FFmpegVideoPlayer.cpp` logs, and screenshot to
   confirm whether it's frozen-on-first-frame vs. genuinely not decoding at all).

One clean log from scenario 1 alone will almost certainly show either
`"Failed to open file: ..."` (→ investigate further, since I proved the path
construction *should* be correct — look for something upstream I didn't check, e.g.
whether the event is even reaching `getBufferForFile`/`playStream` at all) or a
`"Received audio frame"` flood with no corresponding sound (→ confirms candidate 3a,
the buffer-queue timing race).

## 5. The video symptom — same subsystem, check together

The user confirmed a mission-briefing video (a "BNN Exclusive" broadcast cutscene)
does not animate during loading — this may be a static first frame rather than truly
frozen; screenshot evidence exists in this session's chat log but wasn't saved to
disk. `FFmpegVideoPlayer.cpp` shares `OpenALAudioStream` for its audio track (see §1)
and presumably has its own `decodePacket()`-driven frame-advance loop analogous to
the audio one. Given the shared class, **fixing 3a may fix all three symptoms at
once** (music, voice, and video-audio) — but the video not *animating* (not just
being silent) suggests its frame-decode pump might have a related-but-separate issue
worth checking once the audio side is understood.

## 6. Tools available to you (all already proven working on this PC)

- `alprobe.c` / the `me.generalsx.audiotest` APK build recipe (steps documented in
  §2 item 2 above) — reusable pattern for any future "does OpenAL work at all"
  question; remember the bare-shell-process gotcha.
- `vkprobe.c`, `vkprobe_sw.c`, `vkprobe_feat.c` (repo root) — Vulkan-side probes from
  the Mali investigation, not directly relevant here but demonstrate the same
  build/push/run pattern for native test harnesses if useful.
- `scripts/build/android/*` and the full Mali build pipeline are proven working (you
  built it yourself for `v0.4-mali`) — reuse that exact toolchain/signing setup.

## 7. Safety rules (same as always — see `HANDOVER_TCL_NXTPAPER.md` §5)

- Sign with the certificate already on the TCL (`b82277491a6e25094ab2521b32e7728f4e9eb165cb950f544badfcf4564a5374`).
  A signature mismatch must be fixed by signing correctly — **never** `adb uninstall`
  to work around it; that deletes the device's GameData.
- After any `adb push` into `files/GameData`, re-apply
  `adb shell chmod -R 777 /sdcard/Android/data/me.generalsx.zh/files/GameData` or the
  app cannot read what you just pushed (this bit us hard during the Mali work).
- Every binary patch must be length-preserving (pad with NUL, never grow a file) —
  see the corrupted-`libmain.so` incident in `HANDOVER_TCL_NXTPAPER.md` §11 if you
  end up doing any binary-level work instead of a full rebuild.

---

## 8. ⛔ BUILD GUARD — read before producing any APK (added 2026-07-31)

**A rebuild attempt on 2026-07-31 produced an APK that crashed on launch and was
installed to the user's tablet, replacing the working `v0.4-mali` build.** The source
changes were fine; **the packaging was the failure.** Do not repeat it.

### 8.1 What went wrong
A stock `./gradlew assembleDebug` was used. That does **not** reproduce the Mali build.
Comparing the broken APK against the known-good `v0.4-mali` release:

| library | `v0.4-mali` (works) | broken debug build |
|---|---|---|
| `libdxvk_d3d9.so` | **2,639,136** (DXVK Native 1.9.2b) | **37,704,720** (DXVK **2.6**) |
| `libdxvk_d3d8.so` | 1,089,640 (d3d8to9) | 6,217,104 (DXVK 2.6) |
| `libavcodec/avformat/avutil/swresample/swscale.so` | **all 5 present** | **all 5 MISSING** |
| `libmain.so` | 13,661,680 (release) | 21,632,168 (debug) |
| `libfreetype.so` | `libfreetype.so` | `libfreetyped.so` (debug variant) |

Two fatal consequences:
1. **DXVK 2.6 requires Vulkan 1.3. The Mali-G57 driver only offers 1.1.** This is the
   original blocker this entire port exists to solve (§`HANDOVER_TCL_NXTPAPER.md` §4).
   Shipping DXVK 2.6 to this device guarantees the renderer cannot initialise.
2. **All five FFmpeg libraries were dropped** — which are precisely the libraries
   responsible for decoding the music/speech/video this document is about.

Observed crash: `SIGSEGV` at `StringClass::str()+12` called from
`DX8Wrapper::Set_Render_Device+380` — consistent with D3D device creation returning
null/empty after DXVK failed to find a usable Vulkan 1.3 device.

### 8.2 Misdiagnosis to NOT repeat
The crash was attributed to the engine being unable to find `GameData`, and
`patch_lib_cwd.py` was run to inject a `/sdcard/GZH` path into `libmain.so`. **Both
premises are false:**
- **`/sdcard/GZH` does not exist on this device.** Verified: `ls /sdcard/GZH` →
  `No such file or directory`. It is a dead path from an abandoned experiment weeks ago.
- **GameData resolution was never broken.** The crash log from that very build shows
  `CWD -> /storage/emulated/0/Android/data/me.generalsx.zh/files/GameData (external)`
  succeeding, followed by archives loading and the engine running its full init
  sequence. The crash happened *much later*, in renderer setup.

The real GameData path is, and remains,
`/sdcard/Android/data/me.generalsx.zh/files/GameData` (80 entries). **Never run
`patch_lib_cwd.py` / `patch_lib.py` / `patch_zip.py`** — those are dead scripts from
the first debugging session and will corrupt a working binary.

### 8.3 Mandatory pre-install checklist
Before `adb install` of any build, verify **all** of these. Any mismatch = do not install.

```bash
# 1. libmain.so must be release-sized, not debug-sized
#    (13,661,680 for the current release lineage; debug builds land ~21 MB)
# 2. DXVK must be the Native 1.9.2b pair, NOT DXVK 2.6
#    libdxvk_d3d9.so ~2.6 MB   (NOT ~37 MB)
#    libdxvk_d3d8.so ~1.1 MB   (NOT ~6.2 MB)
# 3. All five FFmpeg libs must be present:
#    libavcodec.so libavformat.so libavutil.so libswresample.so libswscale.so
# 4. Signing cert must match the device:
#    b82277491a6e25094ab2521b32e7728f4e9eb165cb950f544badfcf4564a5374
```

One-shot verification (adjust the APK path):

```bash
python -c "
import zipfile,sys
z=zipfile.ZipFile(sys.argv[1]); n=set(z.namelist())
need=['libavcodec.so','libavformat.so','libavutil.so','libswresample.so','libswscale.so']
miss=[x for x in need if 'lib/arm64-v8a/'+x not in n]
d9=z.getinfo('lib/arm64-v8a/libdxvk_d3d9.so').file_size
main=z.getinfo('lib/arm64-v8a/libmain.so').file_size
print('MISSING ffmpeg libs:', miss or 'none  OK')
print('libdxvk_d3d9.so:', d9, '->', 'OK (Native 1.9.2b)' if d9 < 5_000_000 else 'FAIL: this is DXVK 2.6, cannot work on Mali')
print('libmain.so:', main, '->', 'OK (release)' if main < 16_000_000 else 'WARN: debug-sized build')
" YOUR.apk
```

Then confirm on-device after install (this build logs its DXVK version at startup):

```
adb logcat -d | grep -a 'DXVK: v'      # MUST read: DXVK: v1.9.2
```

### 8.4 The correct packaging path
`v0.4-mali` was produced by staging the pre-built Mali runtime libraries into
`android/app/src/main/jniLibs/arm64-v8a/` (DXVK Native 1.9.2b + d3d8to9 + the five
FFmpeg libs), *then* packaging — see `scripts/build/android/repack-mali-apk.py` and
`docs/WORKDIR/phases/PHASE06_ANDROID_PORT.md` (TCL Mali section). Gradle's own
`externalNativeBuild` does not produce or stage these. If you rebuild `libmain.so`
from source, you must **keep the existing Mali runtime `.so` set alongside it**, not
regenerate it.

### 8.5 Recovery (already performed)
The working build was restored by reinstalling the verified `v0.4-mali` APK
(`GeneralsZH-TCL-Mali.apk`, SHA-256
`b2d5de434ae9eea751210dcf0cc1c5451aa74f86f76d860196d0d7816e37a2e4`) with
`adb install -r`. Certificates matched, so **no uninstall was needed and GameData was
never at risk** (verified 80/80 entries before and after). Confirmed working
afterwards: `DXVK: v1.9.2`, swapchain created, empty crash buffer, live skirmish at a
locked 30 FPS.

---

## 9. Status of §2/§3 findings as of 2026-07-31

**Nothing in §2 or §3 has been solved.** No diagnostic build ever reached the device
(the only attempt is the broken one described in §8), so none of the candidates could
be tested. Everything ruled out in §2 stays ruled out; both live candidates in §3
remain open and untested.

### 9.1 Genuinely useful progress that DOES exist (keep this work)
The uncommitted working tree contains real progress toward the diagnostic build
recommended in §4 — this is worth keeping, it just was never successfully packaged:
- `CMakeLists.txt` — `add_compile_definitions(RELEASE_DEBUG_LOGGING)` added. This is
  exactly the flag §4 called for, so **`DEBUG_LOG` output should now compile in**.
- `cmake/compilers.cmake` — `-Wsuggest-override` disabled (it breaks this NDK).
- `android/app/build.gradle` — `ndkVersion` bumped `27.1.12297006` → `28.2.13676358`
  to match the NDK actually installed on this PC.
- Several one-line fixes to `DEBUG_LOG` call sites that only fail to compile once
  logging is enabled (`StdLocalFileSystem.cpp`, `udp.cpp`, `GameResultsThread.cpp`,
  `AIPathfind.cpp`, `ScriptEngine.cpp`, `textureloader.cpp`, `GameClient.cpp`).
- `Core/GameEngineDevice/Source/OpenALAudioDevice/OpenALAudioManager.cpp` — an
  `fprintf(stderr, ...)` trace at the top of `init()`. Note this uses `fprintf`
  rather than `DEBUG_LOG`, so it survives regardless of the logging flag.
- `GeneralsMD/Code/Main/SDL3Main.cpp` — `setenv("ALSOFT_LOGLEVEL", "3", 1)`, which
  raises openal-soft's own log verbosity. Useful (see §9.2).

**None of it hardcodes `/sdcard/GZH`** — verified by grepping every tracked diff. The
source tree is clean; only the packaging was wrong.

### 9.2 NEW finding: OpenAL is leaking in-use buffers (not previously recorded)
Running the restored `v0.4-mali` build during live gameplay, logcat shows a continuous
flood of openal-soft errors:

```
W openal  : [ALSOFT] (WW) Error generated on context 0x…, code 0xa004,
            "Deleting in-use buffer 662"
```

`0xa004` is `AL_INVALID_OPERATION`. Buffer IDs climb steadily (660→679 in ~10 s), so
this happens many times per second throughout normal play.

**Practical tip for whoever picks this up:** the log tag is lowercase **`openal`**.
Grepping for `OpenAL` (as the earlier investigation did) misses all of it — which is
why this was not found sooner. Use `adb logcat -d | grep -a openal`.

**Mechanism**, from `OpenALAudioCache.cpp:228 releaseOpenAudioFile()`:
```cpp
if (fileToRelease->m_openCount > 0) {
    TheAudio->closeAnySamplesUsingFile(...);   // meant to stop sources first
}
...
if (fileToRelease->m_buffer) {
    alDeleteBuffers(1, &fileToRelease->m_buffer);   // <-- rejected: still in use
}
fileToRelease->m_buffer = 0;                        // <-- handle dropped anyway
```
`alDeleteBuffers` fails (source still bound), but the handle is zeroed and the caller
decrements `m_currentlyUsedSize` regardless. So the buffer **leaks inside OpenAL**
while the cache's own accounting believes it was freed. `closeAnySamplesUsingFile()`
is evidently not reliably detaching/stopping the source first.

**Honest caveat:** this is a real, directly-observed defect, but it is **not proven**
to be the cause of the silent music/voice — short SFX still play correctly despite it.
It is worth fixing on its own merits, and it may well be contributory (a cache whose
size accounting drifts will make wrong eviction decisions, and
`getBufferForFile()` returns `0` — i.e. silence — whenever
`freeEnoughSpaceForSample()` fails). Investigate alongside §3.

### 9.3 Video: partially contradicts the §5 symptom
§5 recorded, from the user, that loading-screen video does not animate. On the
restored build I captured two screenshots 2 s apart during an in-engine cinematic and
they **differ**, i.e. that scene animates fine. That scene may be an in-engine 3D
sequence rather than an FFmpeg-decoded video, so this does **not** clear §5 — but the
"video is frozen" symptom should be re-confirmed against a specific, known `.bik`/
FFmpeg-backed cutscene before time is spent on it, and the two cases distinguished.
