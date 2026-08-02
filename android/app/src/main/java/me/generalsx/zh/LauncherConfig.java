// GeneralsX @feature android-port 08/02/2026
//
// LauncherConfig — the launcher's model layer: where profiles live on disk, and
// how the user's choices become the argv handed to the engine.
//
// A "profile" is one directory containing a full set of .big archives (plus the
// engine's Data/, Maps/, fonts/ ... layout). The stock install is the profile
// named "GameData", which is where every previous build kept its files, so an
// existing install is picked up with nothing to migrate. Additional profiles --
// a second game's data, or a total conversion such as ShockWave or Rise of the
// Reds -- live beside it under Profiles/<name>.
//
//   <external files>/
//     GameData/                 <- stock profile (pre-existing installs)
//     Profiles/ShockWave/       <- additional full data sets
//     Profiles/RiseOfTheReds/
//     Mods/<name>/              <- partial mods, applied over a profile via -mod
//
// The engine is told which one to use with "-datadir <path>" (handled in
// SDL3Main.cpp before the working directory is set). Partial mods are layered on
// top with the engine's own "-mod <path>".

package me.generalsx.zh;

import android.content.Context;
import android.content.SharedPreferences;

import java.io.File;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.List;

public final class LauncherConfig {

    public static final String PREFS = "generalsx_launcher";

    public static final String KEY_PROFILE   = "profile";      // "" = stock GameData
    public static final String KEY_MOD       = "mod";          // "" = none
    public static final String KEY_SKIP_INTRO = "skip_intro";
    public static final String KEY_NO_SHELLMAP = "no_shellmap";
    public static final String KEY_WINDOWED  = "windowed";
    public static final String KEY_EXTRA_ARGS = "extra_args";

    public static final String STOCK_PROFILE = "GameData";
    public static final String PROFILES_DIR  = "Profiles";
    public static final String MODS_DIR      = "Mods";

    private LauncherConfig() {}

    public static SharedPreferences prefs(Context ctx) {
        return ctx.getSharedPreferences(PREFS, Context.MODE_PRIVATE);
    }

    /**
     * Root of the app's external files dir -- the adb-pushable location the
     * engine already searches, and the only place large game data can live
     * without requesting broad storage permissions.
     *
     * Returns null when external storage is unavailable (ejected/unmounted),
     * which callers must handle rather than assume.
     */
    public static File storageRoot(Context ctx) {
        return ctx.getExternalFilesDir(null);
    }

    public static File stockProfileDir(Context ctx) {
        File root = storageRoot(ctx);
        return (root == null) ? null : new File(root, STOCK_PROFILE);
    }

    public static File profilesRoot(Context ctx) {
        File root = storageRoot(ctx);
        return (root == null) ? null : new File(root, PROFILES_DIR);
    }

    public static File modsRoot(Context ctx) {
        File root = storageRoot(ctx);
        return (root == null) ? null : new File(root, MODS_DIR);
    }

    /** Absolute directory for a profile name ("" or "GameData" = the stock one). */
    public static File profileDir(Context ctx, String name) {
        if (name == null || name.isEmpty() || STOCK_PROFILE.equals(name)) {
            return stockProfileDir(ctx);
        }
        File root = profilesRoot(ctx);
        return (root == null) ? null : new File(root, name);
    }

    /**
     * Profiles that actually contain game data, stock first.
     *
     * A directory is only offered if it holds at least one .big archive --
     * listing an empty folder would let the user launch into a black screen and
     * then wonder which of the two things went wrong.
     */
    public static List<String> availableProfiles(Context ctx) {
        List<String> out = new ArrayList<>();
        File stock = stockProfileDir(ctx);
        if (stock != null && hasGameData(stock)) {
            out.add(STOCK_PROFILE);
        }
        File root = profilesRoot(ctx);
        if (root != null) {
            File[] kids = root.listFiles();
            if (kids != null) {
                Arrays.sort(kids);
                for (File k : kids) {
                    if (k.isDirectory() && hasGameData(k)) {
                        out.add(k.getName());
                    }
                }
            }
        }
        return out;
    }

    /** Installed partial mods, applied over a profile with -mod. */
    public static List<String> availableMods(Context ctx) {
        List<String> out = new ArrayList<>();
        File root = modsRoot(ctx);
        if (root != null) {
            File[] kids = root.listFiles();
            if (kids != null) {
                Arrays.sort(kids);
                for (File k : kids) {
                    if (k.isDirectory() && !isEmptyDir(k)) {
                        out.add(k.getName());
                    }
                }
            }
        }
        Collections.sort(out);
        return out;
    }

    /**
     * True if the directory looks like game data: at least one .big, either
     * directly inside or one level down (retail installs keep them beside the
     * executable; some copies nest them in Data/).
     */
    public static boolean hasGameData(File dir) {
        if (dir == null || !dir.isDirectory()) return false;
        File[] kids = dir.listFiles();
        if (kids == null) return false;
        for (File k : kids) {
            if (k.isFile() && k.getName().toLowerCase().endsWith(".big")) return true;
        }
        for (File k : kids) {
            if (k.isDirectory()) {
                File[] sub = k.listFiles();
                if (sub == null) continue;
                for (File s : sub) {
                    if (s.isFile() && s.getName().toLowerCase().endsWith(".big")) return true;
                }
            }
        }
        return false;
    }

    private static boolean isEmptyDir(File dir) {
        String[] kids = dir.list();
        return kids == null || kids.length == 0;
    }

    /** Count of .big archives directly in a profile, for the UI summary. */
    public static int countBigFiles(File dir) {
        if (dir == null || !dir.isDirectory()) return 0;
        File[] kids = dir.listFiles();
        if (kids == null) return 0;
        int n = 0;
        for (File k : kids) {
            if (k.isFile() && k.getName().toLowerCase().endsWith(".big")) n++;
        }
        return n;
    }

    /**
     * Build the engine's argv from the saved settings.
     *
     * argv[0] is the program name: CommandLine.cpp starts parsing at index 1 to
     * match Windows' GetCommandLineA(), so a placeholder must be present or the
     * first real flag is silently dropped.
     */
    public static ArrayList<String> buildArguments(Context ctx) {
        SharedPreferences p = prefs(ctx);
        ArrayList<String> args = new ArrayList<>();
        args.add("generalszh");

        File profile = profileDir(ctx, p.getString(KEY_PROFILE, STOCK_PROFILE));
        if (profile != null && profile.isDirectory()) {
            args.add("-datadir");
            args.add(profile.getAbsolutePath());
        }

        String mod = p.getString(KEY_MOD, "");
        if (mod != null && !mod.isEmpty()) {
            File modsRoot = modsRoot(ctx);
            File modDir = (modsRoot == null) ? null : new File(modsRoot, mod);
            if (modDir != null && modDir.exists()) {
                // The engine accepts a directory or a .big here and prepends the
                // user-data path only for relative names, so pass it absolute.
                args.add("-mod");
                args.add(modDir.getAbsolutePath());
            }
        }

        if (p.getBoolean(KEY_SKIP_INTRO, true)) {
            // Clears m_playIntro / m_playSizzle: no EA logo, no Sizzle reel.
            args.add("-nologo");
        }
        if (p.getBoolean(KEY_NO_SHELLMAP, false)) {
            // The animated 3D menu background; the single biggest menu cost on
            // weak GPUs.
            args.add("-noshellmap");
        }
        if (p.getBoolean(KEY_WINDOWED, false)) {
            args.add("-win");
        }

        String extra = p.getString(KEY_EXTRA_ARGS, "");
        if (extra != null && !extra.trim().isEmpty()) {
            for (String tok : extra.trim().split("\\s+")) {
                if (!tok.isEmpty()) args.add(tok);
            }
        }
        return args;
    }

    /** Human-readable form of the argv, shown in the launcher. */
    public static String describeArguments(Context ctx) {
        List<String> a = buildArguments(ctx);
        StringBuilder sb = new StringBuilder();
        for (int i = 1; i < a.size(); i++) {          // skip argv[0]
            if (sb.length() > 0) sb.append(' ');
            sb.append(a.get(i));
        }
        return sb.length() == 0 ? "(no flags)" : sb.toString();
    }
}
