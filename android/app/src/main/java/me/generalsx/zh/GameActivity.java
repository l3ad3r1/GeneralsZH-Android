// GeneralsX @feature android-port 06/07/2026
//
// GameActivity — the Android entry point. Mirrors what iOS gets for free from
// SDL's UIApplicationMain bootstrap: load the native library, hand control to
// SDL_main(). SDL3 ships SDLActivity.java (via the FetchContent'd SDL3 source
// under android-project/app/src/main/java/org/libsdl/app/); the packaging script
// copies it in. We subclass it only to pin the package name + set library hints
// before SDL's nativeInit runs.
//
// On launch: SDLActivity.onCreate -> System.loadLibrary("SDL3") ->
// nativeRunMain("main", "SDL_main", argv) -> dlopen("libmain.so") -> SDL_main().
//
// The native SDL_main lives in GeneralsMD/Code/Main/SDL3Main.cpp (renamed by
// SDL3/SDL_main.h on Android). Everything else — Vulkan surface creation, touch
// routing, lifecycle — is handled by the engine + SDL3, identical to iOS.

package me.generalsx.zh;

import org.libsdl.app.SDLActivity;

import java.util.ArrayList;

public class GameActivity extends SDLActivity {

    /** Intent extra: the argv LauncherActivity built, minus argv[0]'s meaning. */
    public static final String EXTRA_ARGS = "me.generalsx.zh.ARGS";

    // GeneralsX @feature android-port 08/02/2026 Hand the launcher's flags to the
    // engine. SDLActivity passes whatever this returns to nativeRunMain as argv,
    // which reaches main() in SDL3Main.cpp and, through __argv, CommandLine.cpp.
    //
    // Launching the Activity directly (adb am start, or a home-screen shortcut to
    // the old entry point) simply yields no extra, and the engine falls back to
    // its default data directory — so the game still starts without the launcher.
    @Override
    protected String[] getArguments() {
        ArrayList<String> args = null;
        if (getIntent() != null) {
            args = getIntent().getStringArrayListExtra(EXTRA_ARGS);
        }
        if (args == null || args.isEmpty()) {
            // Not launched from the launcher: use the saved settings anyway, so
            // behaviour stays consistent however the game was started.
            args = LauncherConfig.buildArguments(this);
        }
        return args.toArray(new String[0]);
    }
    @Override
    protected String[] getLibraries() {
        // Order matters: the engine (libmain.so) dlopens libdxvk_d3d8.so, which
        // pulls libvulkan.so. SDL3 + SDL3_image are the windowing/asset layer.
        // Preloading them here makes them resolvable inside the app's linker
        // namespace before the engine's dlopen() runs (Android API 24+ namespace
        // isolation requires a lib to be registered via loadLibrary before a
        // bare-name dlopen can find it).
        // GeneralsX @feature android-port 08/02/2026 The LAST entry selects which
        // game runs: SDLActivity.getMainSharedObject() turns it into the .so it
        // dlopens and dlsyms SDL_main from. Both engines ship in the APK --
        // libmain.so (Zero Hour) and libmain_generals.so (Generals) -- so
        // switching games is choosing which one goes last here.
        return new String[]{
            "SDL3",
            "SDL3_image",
            "openal",
            "dxvk_d3d9",
            "dxvk_d3d8",
            LauncherConfig.engine(this)
        };
    }

    @Override
    protected String getMainFunction() {
        // The symbol nativeRunMain dlsyms inside libmain.so.
        return "SDL_main";
    }
}
