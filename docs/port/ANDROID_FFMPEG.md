# Android FFmpeg: video and audio decoding

The single highest-cost bug in this port. It shipped in **four** consecutive
builds without producing a warning, an error, or a failed build. Read §1 before
touching anything audio- or video-related.

---

## 1. Two independent stub layers

The engine has **two** separate places that fall back to stub FFmpeg on Android.
Fixing one does not fix the other, and each fails silently.

### Layer 1 — the CMake source switch

`Core/GameEngineDevice/CMakeLists.txt` picks between the real decoder sources
and stubs based on `SAGE_ANDROID_FFMPEG_DIR`:

```cmake
if(ANDROID AND SAGE_ANDROID_FFMPEG_DIR)
    ...                                     # real FFmpegFile.cpp + Bink player
    target_compile_definitions(corei_gameenginedevice_public INTERFACE RTS_HAS_FFMPEG)
elseif(ANDROID)
    # No FFmpeg — use the stub (audio will be silent)
    target_sources(... FFmpegFileStub.cpp)  # + BinkVideoPlayerStub.cpp
endif()
```

If the variable is not passed, CMake takes the stub branch **quietly**. The
build reports SUCCESS. The APK still contains all five FFmpeg `.so` files,
because those are copied in by the packaging step, which knows nothing about how
the engine was compiled. The game runs. It simply has no video and no audio
decoding whatsoever.

It is passed from `android/app/build.gradle`:

```groovy
"-DSAGE_ANDROID_FFMPEG_DIR=" + file("${rootDir}/../ffmpeg-android").absolutePath,
```

### Layer 2 — hardcoded `#include` guards

`OpenALAudioManager.cpp` and `OpenALAudioCache.cpp` each choose their FFmpeg
headers independently of the above:

```c
#if defined(__ANDROID__) && !defined(RTS_HAS_FFMPEG)
#include "VideoDevice/FFmpeg/FFmpegAndroidStub.h"
#else
extern "C" { #include <libavcodec/avcodec.h> ... }
#endif
```

`OpenALAudioManager.cpp` was missing the `&& !defined(RTS_HAS_FFMPEG)` half. So
even after Layer 1 was fixed and the binary genuinely linked libavcodec, that
one file still compiled against the stub.

**`FFmpegAndroidStub.h` is not a passive placeholder.** It actively corrupts a
build that has real FFmpeg linked:

- `av_samples_get_buffer_size()` returns **0**, so the audio frame callback
  computes a zero-byte frame and queues nothing.
- It declares its **own `struct AVFrame`**, whose layout does not match
  libavutil's. Real `AVFrame*` pointers handed over by `FFmpegFile.cpp` are read
  through the wrong offsets — `format`, `nb_samples` and `ch_layout` are all
  garbage.

This is why video worked while music and speech stayed silent: video decodes
*inside* `FFmpegFile.cpp`, which always used the real headers. Engine audio
crossed the boundary into the stubbed translation unit and died there.

---

## 2. How to tell, in one command

Do not trust "BUILD SUCCESSFUL", and do not trust the presence of `libav*.so` in
the APK. Ask the binary what it actually links:

```bash
llvm-readelf -d libmain.so | grep -E "NEEDED.*(avcodec|avformat|avutil|swresample|swscale)"
llvm-strings  libmain.so | grep -c "Linux stub (video playback not available)"   # must be 0
```

Per translation unit, an object file compiled against the stub has **zero**
undefined FFmpeg symbols, because every stub function is `inline`:

```bash
llvm-nm -u .../OpenALAudioManager.cpp.o | grep -E "\bav_|\bswr_"
# empty  => compiled against the stub
# av_*   => compiled against real FFmpeg
```

`scripts/build/android/package-mali-apk.py` now **refuses to emit an APK** that
fails these checks. That guard is the only reason this cannot ship again; it was
verified to flag the three known-bad builds and pass the fixed one.

---

## 3. Building the FFmpeg SDK

`scripts/build/android/build-ffmpeg-android.sh` produces `ffmpeg-android/`
(headers + `lib/*.so`). Two things bite:

**Bink needs the *config* name, not the codec name.** The video decoder's
configure switch is `bink`, not `binkvideo`. Passing `--enable-decoder=binkvideo`
configures cleanly and yields working Bink *audio* with no video. Confirm names
with `./configure --list-decoders`. The full set:

```
--enable-decoder=bink,binkaudio_dct,binkaudio_rdft --enable-demuxer=bink
```

**Android rejects versioned sonames.** FFmpeg emits `libavcodec.so.62`; Android
needs `libavcodec.so`. Fix after building:

```bash
patchelf --set-soname libavcodec.so libavcodec.so
patchelf --replace-needed libavutil.so.60 libavutil.so libavcodec.so
```

---

## 4. Unrelated but adjacent: `RELEASE_DEBUG_LOGGING`

Do not enable it on Android. It compiles in global `LogClass` instances whose
constructors run during `call_constructors()` — before the engine's custom
`operator new` pool exists. Scudo (Android 14+) detects the resulting heap
corruption and aborts at startup:

```
#06 std::string::append
#07 LogClass::LogClass
#09 __dl__ZN6soinfo17call_constructorsEv
```

Use targeted `fprintf`/`__android_log_print` instead. See the comment in the
root `CMakeLists.txt`.
