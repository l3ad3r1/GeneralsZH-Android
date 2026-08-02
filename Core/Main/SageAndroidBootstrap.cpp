// GeneralsX @feature android-port 08/02/2026
// See SageAndroidBootstrap.h for why this is shared between both games.

#include "SageAndroidBootstrap.h"

#if defined(__ANDROID__)

#include <SDL3/SDL.h>
#include <android/log.h>
#include <android/asset_manager.h>
#include <android/asset_manager_jni.h>
#include <jni.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/stat.h>

void SageAndroid_ForceAudioBackend()
{
	// GeneralsX @bugfix android-port 07/07/2026 Force OpenAL Soft to use the Oboe
	// backend on Android. By default, OpenAL's init gives the "null" backend
	// (a no-output sink) priority, producing no audio. Setting ALSOFT_DRIVERS=oboe
	// before alcOpenDevice forces it to use Oboe (AAudio/OpenSL ES).
	setenv("ALSOFT_DRIVERS", "oboe", 1);
	setenv("ALSOFT_LOGLEVEL", "3", 1);

	// GeneralsX @tweak android-port 08/01/2026 Deepen the mixer buffer.
	// Audible clicking is bursts of buffer-queue underruns on peak frames --
	// visible as a climbing underrun count in `dumpsys media.audio_flinger`,
	// and far worse once the device is thermally throttled. OpenAL Soft's
	// default queue is ~35ms (512x3), which peak frames punch straight
	// through. 1024x4 is ~85ms at 48kHz: inaudible latency for an RTS, and
	// roughly triple the cushion.
	//
	// Ported from wingear's fork (GPL-3.0), commit a2ae64d1 --
	// https://github.com/wingear/GeneralsZH-Android-OpenGL-ES
	if (!getenv("ALSOFT_PERIOD_SIZE")) {
		setenv("ALSOFT_PERIOD_SIZE", "1024", 1);
		setenv("ALSOFT_PERIODS", "4", 1);
	}
}

void SageAndroid_SetSdlHints()
{
	// All mouse events the game sees on a touch device are synthesized by the
	// gesture translator in SDL3GameEngine.cpp; SDL's automatic touch->mouse
	// synthesis would double-deliver finger 1 and fight the two-finger pan logic.
	SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");

	// ...and the reverse direction too. SDL_HINT_MOUSE_TOUCH_EVENTS defaults to
	// "1" on mobile, so a real USB/Bluetooth mouse ALSO emits SDL_EVENT_FINGER_*
	// alongside its normal mouse events. Those fingers reach the gesture
	// translator, where a drag is a one-finger map pan -- and a pan deliberately
	// moves the camera opposite to the finger so the ground stays under it. The
	// result is a mouse whose movement appears inverted, plus every click
	// delivered twice. Mice must drive mouse input only.
	SDL_SetHint(SDL_HINT_MOUSE_TOUCH_EVENTS, "0");

	// Force landscape at the SDL level. Without this hint, SDLActivity derives
	// its own orientation (observed requestedOrientation=13/FULL_USER) and lets
	// the panel's native portrait mode dictate the Vulkan surface transform. On a
	// portrait-native panel (e.g. Galaxy S24 Ultra) that made every present()
	// come back VK_SUBOPTIMAL_KHR, which DXVK's presenter treated as "recreate
	// the swapchain" -- tens of recreations per second, which lost a free-list
	// race in DxvkResourceAllocationPool and crashed with SIGSEGV.
	SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");

	// Make the right mouse button work. On Android a mouse right-click is
	// delivered to the app as the BACK button, not as SDL_BUTTON_RIGHT, unless
	// BACK is trapped. Without this the engine never sees a right-click at all.
	// Trapping also stops the system BACK gesture from backgrounding the game
	// mid-match; it arrives as SDL_SCANCODE_AC_BACK, which the engine's keyboard
	// translator does not bind, so it is ignored.
	SDL_SetHint(SDL_HINT_ANDROID_TRAP_BACK_BUTTON, "1");
}

void SageAndroid_Bootstrap(int argc, char **argv)
{
	// GeneralsX @feature android-port 06/07/2026 Android working directory + diagnostics.
	//
	// Android does not expose APK assets via the filesystem: a native fopen() on
	// "GameData/*.big" cannot reach them. The packaging step stages GameData into
	// the app's internal storage (<files>/GameData), which IS a real filesystem
	// path the engine can chdir() into and read with stdio — mirroring the iOS
	// "GameData beside the binary" model. SDL3 surfaces that path via
	// SDL_GetAndroidInternalStoragePath(). User data (saves, replays, logs) goes
	// under the same root; DXVK's shader cache lives under the cache dir.
	//
	// Diagnostics: native stderr/stdout already flow to `adb logcat`, but a
	// memory-killed process leaves no tombstone, so we ALSO keep a capped,
	// filtered file log (the iOS port's hard-won lesson). bionic has no funopen(),
	// so a simple ring-buffered write() sink replaces it.
	setenv("DXVK_LOG_LEVEL", "none", 0);
	// The engine's StdBIGFileSystem::init() reads "InstallPath" from the registry
	// to locate the Data/*.big archives. On Android there's no registry — the
	// env-var fallback (CNC_ZH_INSTALLPATH) provides it. Point it at "." so the
	// engine finds Data/ relative to the CWD (set below).
	setenv("CNC_ZH_INSTALLPATH", ".", 0);
	setenv("CNC_GENERALS_INSTALLPATH", ".", 0);
	{
		// GeneralsX @feature android-port 06/07/2026
		// Try EXTERNAL storage first (adb-pushable, no root): the app's external
		// files dir at /sdcard/Android/data/<pkg>/files/GameData. Fall back to
		// internal storage (set by the packaging script).
		const char *extFiles = SDL_GetAndroidExternalStoragePath();
		const char *files = SDL_GetAndroidInternalStoragePath();
		__android_log_print(ANDROID_LOG_INFO, "GeneralsX",
			"storage: external=%s internal=%s",
			extFiles ? extFiles : "(null)", files ? files : "(null)");

		bool chdirOk = false;

		// GeneralsX @feature android-port 08/02/2026 Honour "-datadir <path>",
		// which the launcher passes to select a data profile (base game, or a
		// mod install such as ShockWave / Rise of the Reds). Scanned here rather
		// than in CommandLine.cpp because the working directory has to be set
		// before the engine's file systems come up, long before parseCommandLine
		// runs. Unknown flags are skipped by that parser (`if (!found) arg++`),
		// so it passes through harmlessly there.
		for (int i = 1; i + 1 < argc; ++i) {
			if (argv[i] != nullptr && strcmp(argv[i], "-datadir") == 0 && argv[i + 1] != nullptr) {
				if (access(argv[i + 1], R_OK) == 0 && chdir(argv[i + 1]) == 0) {
					__android_log_print(ANDROID_LOG_INFO, "GeneralsX",
						"CWD -> %s (-datadir)", argv[i + 1]);
					chdirOk = true;
				} else {
					// Fall through to the default search rather than failing: a
					// stale profile path must not make the game unlaunchable.
					__android_log_print(ANDROID_LOG_WARN, "GeneralsX",
						"-datadir '%s' unusable, falling back", argv[i + 1]);
				}
				break;
			}
		}

		if (!chdirOk && extFiles != nullptr) {
			char gameData[1024];
			snprintf(gameData, sizeof(gameData), "%s/GameData", extFiles);
			if (access(gameData, R_OK) == 0 && chdir(gameData) == 0) {
				__android_log_print(ANDROID_LOG_INFO, "GeneralsX", "CWD -> %s (external)", gameData);
				chdirOk = true;
			}
			// GeneralsX @bugfix android-port 07/29/2026 Do NOT SDL_free() this.
			// SDL_GetAndroidExternalStoragePath()/InternalStoragePath()/CachePath()
			// all return a pointer to SDL's own cached static (SDL_strdup'd once,
			// then returned again on every call) — not a fresh allocation. Freeing
			// it here poisoned the same pointer that a later call in this function
			// (or SDL itself) hands back, and the *next* SDL_free() on it aborted
			// with a Scudo "invalid chunk state" crash on the very first launch
			// path (whether or not GameData was found). None of the five
			// SDL_free() calls that used to be in this function were needed.
		}
		if (!chdirOk && files != nullptr) {
			char gameData[1024];
			snprintf(gameData, sizeof(gameData), "%s/GameData", files);
			if (access(gameData, R_OK) == 0 && chdir(gameData) == 0) {
				__android_log_print(ANDROID_LOG_INFO, "GeneralsX", "CWD -> %s (internal)", gameData);
				chdirOk = true;
			}
		}
		if (!chdirOk) {
			__android_log_print(ANDROID_LOG_WARN, "GeneralsX", "no GameData dir found, CWD unchanged");
		}

		// GeneralsX @feature android-port 07/07/2026 Extract bundled fonts from
		// APK assets to the GameData filesystem. Android APK assets are invisible
		// to fopen()/access(), but the engine's FreeType font locator
		// (Locate_Font_FontConfig) probes <CWD>/fonts/<name>.ttf via access().
		// The packaging step bundles Liberation fonts (renamed to Windows names)
		// into assets/fonts/. Extract them once on first launch so the engine can
		// read them via standard stdio.
		{
			char fontsDir[1024];
			const char *extractBase = nullptr;

			// GeneralsX @bugfix android-port 08/02/2026 Extract into <CWD>/fonts,
			// which is what the font locator actually probes. This used to be
			// hardcoded to "<external>/GameData/fonts"; that happens to be the
			// CWD in the default layout, but with the launcher's "-datadir" a
			// profile can live anywhere (a mod install, internal storage), and
			// the fonts would then be written somewhere the engine never looks.
			char cwdBuf[1024];
			if (chdirOk && getcwd(cwdBuf, sizeof(cwdBuf)) != nullptr) {
				snprintf(fontsDir, sizeof(fontsDir), "%s/fonts", cwdBuf);
				extractBase = fontsDir;
			}

			const char *extFiles2 = SDL_GetAndroidExternalStoragePath();
			if (extractBase == nullptr && extFiles2 != nullptr) {
				snprintf(fontsDir, sizeof(fontsDir), "%s/GameData/fonts", extFiles2);
				extractBase = fontsDir;
				// see @bugfix note above: SDL owns this pointer, do not free it.
			}
			if (extractBase == nullptr) {
				const char *intFiles2 = SDL_GetAndroidInternalStoragePath();
				if (intFiles2 != nullptr) {
					snprintf(fontsDir, sizeof(fontsDir), "%s/GameData/fonts", intFiles2);
					extractBase = fontsDir;
					// see @bugfix note above: SDL owns this pointer, do not free it.
				}
			}

			if (extractBase != nullptr) {
				mkdir(extractBase, 0755);

				// Check if fonts already extracted (skip if arial.ttf exists)
				char checkPath[1100];
				snprintf(checkPath, sizeof(checkPath), "%s/arial.ttf", extractBase);
				if (access(checkPath, R_OK) != 0) {
					// Obtain the AAssetManager via JNI (SDL3 doesn't expose it directly)
					AAssetManager *mgr = nullptr;
					JNIEnv *env = (JNIEnv *)SDL_GetAndroidJNIEnv();
					jobject activity = (jobject)SDL_GetAndroidActivity();
					if (env != nullptr && activity != nullptr) {
						jclass cls = env->GetObjectClass(activity);
						jmethodID mid = env->GetMethodID(cls, "getAssets", "()Landroid/content/res/AssetManager;");
						if (mid != nullptr) {
							jobject javaAssetMgr = env->CallObjectMethod(activity, mid);
							if (javaAssetMgr != nullptr) {
								mgr = AAssetManager_fromJava(env, javaAssetMgr);
								env->DeleteLocalRef(javaAssetMgr);
							}
						}
						env->DeleteLocalRef(cls);
					}

					if (mgr != nullptr) {
						static const char * const fontFiles[] = {
							"arial.ttf", "arialbold.ttf",
							"couriernew.ttf", "timesnewroman.ttf"
						};
						__android_log_print(ANDROID_LOG_INFO, "GeneralsX",
							"fonts: extracting from APK assets to %s", extractBase);
						for (int i = 0; i < (int)(sizeof(fontFiles)/sizeof(fontFiles[0])); ++i) {
							char assetPath[256];
							snprintf(assetPath, sizeof(assetPath), "fonts/%s", fontFiles[i]);
							AAsset *asset = AAssetManager_open(mgr, assetPath, AASSET_MODE_STREAMING);
							if (asset == nullptr) {
								__android_log_print(ANDROID_LOG_WARN, "GeneralsX",
									"fonts: asset '%s' not found in APK", assetPath);
								continue;
							}
							char outPath[1100];
							snprintf(outPath, sizeof(outPath), "%s/%s", extractBase, fontFiles[i]);
							int outFd = open(outPath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
							if (outFd < 0) {
								AAsset_close(asset);
								continue;
							}
							char buf[8192];
							int bytesRead;
							while ((bytesRead = AAsset_read(asset, buf, sizeof(buf))) > 0) {
								write(outFd, buf, bytesRead);
							}
							close(outFd);
							AAsset_close(asset);
							__android_log_print(ANDROID_LOG_INFO, "GeneralsX",
								"fonts: extracted %s", fontFiles[i]);
						}
					} else {
						__android_log_print(ANDROID_LOG_WARN, "GeneralsX",
							"fonts: cannot get AAssetManager via JNI");
					}
				} else {
					__android_log_print(ANDROID_LOG_INFO, "GeneralsX",
						"fonts: already present at %s", extractBase);
				}
			}
		}

			if (files != nullptr) {
			// DXVK shader cache in the app cache dir (purgeable under storage pressure).
			const char *cache = SDL_GetAndroidCachePath();
			if (cache != nullptr) {
				setenv("DXVK_STATE_CACHE_PATH", cache, 0);
				// see @bugfix note above: SDL owns this pointer, do not free it.
			}
			// Capped, filtered stderr file sink (post-mortem evidence after a kill).
			// GeneralsX @bugfix android-port 08/01/2026 Write the log to EXTERNAL storage
			// when it is available. Internal storage (/data/data/<pkg>/files) is not
			// readable over adb for a non-debuggable release build, so the engine's whole
			// fprintf diagnostic stream was effectively unreachable on a shipping APK --
			// `adb pull` just returns a stale copy and the real log is invisible. The
			// external app-specific dir (/sdcard/Android/data/<pkg>/files) is pullable
			// without root, is still app-private, and is wiped with the app on uninstall.
			const char *logDir = (extFiles != nullptr) ? extFiles : files;
			char logPath[1100], prevPath[1100];
			snprintf(logPath, sizeof(logPath), "%s/generals-stderr.log", logDir);
			snprintf(prevPath, sizeof(prevPath), "%s/generals-stderr-prev.log", logDir);
			rename(logPath, prevPath);
			static int s_logFd = open(logPath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
			if (s_logFd >= 0) {
				// Redirect the C stderr FILE onto our fd via dup2: portable across
				// bionic/libc, unlike Darwin's funopen(). Line-buffered so a crash
				// still flushes recent lines. (Per-frame spam filtering is left to
				// dxvk.conf + DXVK_LOG_LEVEL=none; a full filter callback would need
				// a custom FILE backend that bionic does not provide.)
				fflush(stderr);
				dup2(s_logFd, STDERR_FILENO);
				setvbuf(stderr, nullptr, _IOLBF, 0);
			}
			// see @bugfix note above: SDL owns this pointer, do not free it.
		}
	}
}

#else   // !__ANDROID__

void SageAndroid_ForceAudioBackend() {}
void SageAndroid_Bootstrap(int, char **) {}
void SageAndroid_SetSdlHints() {}

#endif  // __ANDROID__
