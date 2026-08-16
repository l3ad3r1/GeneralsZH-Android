// GeneralsX @feature android-port 16/08/2026
//
// GameFileChecker — a launcher-side pre-flight for the installed game data.
//
// The native engine mmaps and parses every .big/.gib archive at startup; a
// single truncated or wrong-magic archive takes the whole engine down before
// the first frame, and from logcat alone it is hard to tell which file did it.
// This checker walks the selected profile (and the active mod) the same way the
// engine's archive layer does and validates each container, so the culprit is
// named up front instead of inferred from a native crash.
//
// The .big/.gib container is Westwood's BIG format:
//
//     char[4] magic        "BIGF" (Generals/Zero Hour, and community .gib) or
//                          "BIG4" (later EA titles)
//     uint32  archiveSize  total size of the archive
//     uint32  numEntries   number of embedded files
//     uint32  indexSize    bytes of index before the file data
//
// The two length fields' endianness differs across the format's history, so we
// accept a match under either interpretation and only call a file broken when
// BOTH readings still exceed the bytes actually on disk — which is exactly the
// signature of a copy or download that stopped early. That is the failure this
// tool exists to catch; it deliberately does not try to validate archive
// contents, which is the engine's job.

package me.generalsx.zh;

import android.util.Log;

import java.io.File;
import java.io.IOException;
import java.io.RandomAccessFile;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;

final class GameFileChecker {

    static final String TAG = "GameFileCheck";

    private GameFileChecker() {}

    static final class Result {
        final String report;
        final int archivesChecked;
        final int problems;
        Result(String report, int archivesChecked, int problems) {
            this.report = report;
            this.archivesChecked = archivesChecked;
            this.problems = problems;
        }
    }

    /**
     * Validate every archive under a profile and (optionally) a mod directory.
     *
     * Runs synchronously and is cheap — each archive costs one 16-byte header
     * read regardless of size — but callers should still invoke it off the UI
     * thread since a profile can hold dozens of files on slow storage.
     */
    static Result check(String engineLabel, String profileName, File profileDir,
                        String modName, File modDir) {
        StringBuilder sb = new StringBuilder();
        int[] counters = new int[2];   // [0] = archives checked, [1] = problems

        line(sb, "Game file check");
        line(sb, "Engine:  " + engineLabel);
        line(sb, "Profile: " + profileName
                + (profileDir == null ? "" : "  (" + profileDir.getAbsolutePath() + ")"));
        if (modName != null && !modName.isEmpty()) {
            line(sb, "Mod:     " + modName
                    + (modDir == null ? "" : "  (" + modDir.getAbsolutePath() + ")"));
        }
        line(sb, "");

        checkTree(sb, "Game data", profileDir, counters);
        if (modDir != null) {
            line(sb, "");
            checkTree(sb, "Mod: " + modName, modDir, counters);
        }

        line(sb, "");
        line(sb, "Summary: " + counters[0] + " archive" + (counters[0] == 1 ? "" : "s")
                + " checked, " + counters[1] + " problem" + (counters[1] == 1 ? "" : "s")
                + " found.");
        if (counters[1] == 0 && counters[0] > 0) {
            line(sb, "All archives look intact. If the game still fails to start, the");
            line(sb, "cause is not a truncated/mis-typed archive — check logcat (tag");
            line(sb, "'" + TAG + "' and the engine's own output) for the failing stage.");
        } else if (counters[1] > 0) {
            line(sb, "Re-import or re-download the file(s) marked [FAIL] above; a partial");
            line(sb, "copy is the usual cause. Export this report to share the details.");
        }

        return new Result(sb.toString(), counters[0], counters[1]);
    }

    private static void checkTree(StringBuilder sb, String heading, File dir, int[] counters) {
        line(sb, heading + ":");
        if (dir == null || !dir.isDirectory()) {
            line(sb, "  (directory unavailable)");
            return;
        }
        List<File> archives = new ArrayList<>();
        collectArchives(dir, archives);
        if (archives.isEmpty()) {
            line(sb, "  (no .big/.gib archives found)");
            return;
        }
        for (File f : archives) {
            String rel = relativize(dir, f);
            String verdict = inspect(f);
            boolean ok = verdict == null;
            counters[0]++;
            if (!ok) counters[1]++;
            line(sb, "  [" + (ok ? "OK  " : "FAIL") + "] " + rel
                    + "  (" + GameDataImporter.human(f.length()) + ")"
                    + (ok ? "" : " — " + verdict));
        }
    }

    /** Archives directly in {@code dir} and one level down (engine layout). */
    private static void collectArchives(File dir, List<File> out) {
        File[] top = dir.listFiles();
        if (top == null) return;
        Arrays.sort(top);
        for (File f : top) {
            if (f.isFile() && LauncherConfig.isArchive(f.getName())) out.add(f);
        }
        for (File f : top) {
            if (f.isDirectory()) {
                File[] sub = f.listFiles();
                if (sub == null) continue;
                Arrays.sort(sub);
                for (File s : sub) {
                    if (s.isFile() && LauncherConfig.isArchive(s.getName())) out.add(s);
                }
            }
        }
    }

    /**
     * @return null when the archive is intact, otherwise a short reason string.
     */
    private static String inspect(File f) {
        long len = f.length();
        if (!f.canRead()) return "unreadable (permission or I/O error)";
        if (len == 0)     return "empty file (0 bytes)";
        if (len < 16)     return "too small to be a BIG archive (" + len + " bytes)";

        byte[] head = new byte[16];
        try (RandomAccessFile raf = new RandomAccessFile(f, "r")) {
            raf.readFully(head);
        } catch (IOException e) {
            return "could not read header (" + e.getMessage() + ")";
        }

        String magic = new String(head, 0, 4, java.nio.charset.StandardCharsets.US_ASCII);
        if (!"BIGF".equals(magic) && !"BIG4".equals(magic)) {
            return "bad magic '" + printable(magic) + "' — not a BIG archive "
                    + "(wrong file, or an HTML error page saved as .big)";
        }

        long declaredLE = u32le(head, 4);
        long declaredBE = u32be(head, 4);
        boolean matches = declaredLE == len || declaredBE == len;
        if (!matches && len < declaredLE && len < declaredBE) {
            long expected = Math.min(declaredLE, declaredBE);
            return "truncated — header declares " + expected + " bytes but only "
                    + len + " are on disk (" + GameDataImporter.human(len) + " of "
                    + GameDataImporter.human(expected) + ")";
        }
        // Magic is valid and the file is not short of its declared length. A size
        // field that still disagrees is unusual but not a load-blocker on its own,
        // so it is not counted as a failure.
        return null;
    }

    // --- header helpers -----------------------------------------------------

    private static long u32le(byte[] b, int off) {
        return  (b[off]     & 0xFFL)
              | (b[off + 1] & 0xFFL) << 8
              | (b[off + 2] & 0xFFL) << 16
              | (b[off + 3] & 0xFFL) << 24;
    }

    private static long u32be(byte[] b, int off) {
        return  (b[off]     & 0xFFL) << 24
              | (b[off + 1] & 0xFFL) << 16
              | (b[off + 2] & 0xFFL) << 8
              | (b[off + 3] & 0xFFL);
    }

    private static String printable(String s) {
        StringBuilder out = new StringBuilder();
        for (int i = 0; i < s.length(); i++) {
            char c = s.charAt(i);
            out.append(c >= 0x20 && c < 0x7F ? c : '?');
        }
        return out.toString();
    }

    private static String relativize(File base, File f) {
        String bp = base.getAbsolutePath();
        String fp = f.getAbsolutePath();
        return fp.startsWith(bp) ? fp.substring(bp.length()).replaceFirst("^[/\\\\]", "") : f.getName();
    }

    private static void line(StringBuilder sb, String s) {
        sb.append(s).append('\n');
        Log.i(TAG, s);
    }
}
