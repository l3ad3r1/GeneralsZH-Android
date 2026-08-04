// GeneralsX @feature android-port 08/02/2026
//
// GameDataImporter — copies a game install (or a mod) from anywhere the user can
// reach into the app's external files dir, where the engine can actually open it.
//
// Why the Storage Access Framework rather than plain File I/O: from Android 11
// an app cannot read arbitrary paths under /sdcard without MANAGE_EXTERNAL_STORAGE,
// which is a Play-policy-restricted permission and overkill here. Asking the user
// to pick the folder once with ACTION_OPEN_DOCUMENT_TREE grants access to exactly
// that tree and nothing else, needs no manifest permission, and works on every
// version from API 24 up.
//
// The copy runs on a background thread and reports progress; a full Zero Hour
// install is ~2GB and would hold the UI thread for minutes.

package me.generalsx.zh;

import android.content.Context;
import android.net.Uri;
import android.os.Handler;
import android.os.Looper;

import androidx.documentfile.provider.DocumentFile;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.atomic.AtomicBoolean;

public final class GameDataImporter {

    public interface Listener {
        void onProgress(String message, int percent);
        /** ok=false means nothing usable was imported; message explains why. */
        void onFinished(boolean ok, String message);
    }

    private final ExecutorService pool = Executors.newSingleThreadExecutor();
    private final Handler ui = new Handler(Looper.getMainLooper());
    private final AtomicBoolean cancelled = new AtomicBoolean(false);

    public void cancel() { cancelled.set(true); }

    /**
     * Copy every game file found under {@code treeUri} into {@code destDir}.
     *
     * Only the file types the engine actually reads are copied. A retail install
     * directory also holds the Windows executables, DirectX redistributables and
     * save games -- copying those would waste a lot of time and storage on a
     * tablet for files that can never be used.
     */
    public void importFrom(final Context ctx, final Uri treeUri, final File destDir,
                           final Listener listener) {
        cancelled.set(false);
        pool.execute(new Runnable() {
            @Override public void run() {
                try {
                    DocumentFile root = DocumentFile.fromTreeUri(ctx, treeUri);
                    if (root == null || !root.isDirectory()) {
                        finish(listener, false, "Could not open the selected folder.");
                        return;
                    }

                    post(listener, "Scanning folder…", 0);
                    List<Entry> files = new ArrayList<>();
                    collect(root, "", files, 0);

                    if (files.isEmpty()) {
                        finish(listener, false,
                            "No game files found there.\n\n" +
                            "Pick the folder that contains the .big archives — for a Steam " +
                            "install that is:\n" +
                            "steamapps/common/Command and Conquer Generals Zero Hour");
                        return;
                    }

                    long total = 0;
                    for (Entry e : files) total += Math.max(0, e.doc.length());

                    if (!destDir.exists() && !destDir.mkdirs()) {
                        finish(listener, false, "Could not create " + destDir.getAbsolutePath());
                        return;
                    }

                    long done = 0;
                    int copied = 0;
                    for (Entry e : files) {
                        if (cancelled.get()) {
                            finish(listener, false, "Import cancelled.");
                            return;
                        }
                        File out = new File(destDir, e.relativePath);
                        File parent = out.getParentFile();
                        if (parent != null && !parent.exists() && !parent.mkdirs()) {
                            finish(listener, false, "Could not create " + parent.getAbsolutePath());
                            return;
                        }

                        // Skip files already present at the same size: re-running
                        // an import after a failure part-way through should not
                        // re-copy gigabytes it already has.
                        long srcLen = e.doc.length();
                        if (out.isFile() && out.length() == srcLen && srcLen > 0) {
                            done += srcLen;
                            post(listener, "Skipping " + e.doc.getName() + " (already copied)",
                                 pct(done, total));
                            continue;
                        }

                        post(listener, "Copying " + e.doc.getName()
                                + "  (" + human(srcLen) + ")", pct(done, total));
                        done += copyOne(ctx, e.doc.getUri(), out);
                        copied++;
                    }

                    finish(listener, true, "Imported " + copied + " file"
                            + (copied == 1 ? "" : "s") + " (" + human(done) + ") to\n"
                            + destDir.getAbsolutePath());

                } catch (IOException io) {
                    finish(listener, false, "Copy failed: " + io.getMessage());
                } catch (Exception ex) {
                    finish(listener, false, "Import failed: " + ex);
                }
            }
        });
    }

    /** One source document plus the path it should take under the destination. */
    private static final class Entry {
        final DocumentFile doc;
        final String relativePath;
        Entry(DocumentFile d, String p) { doc = d; relativePath = p; }
    }

    /**
     * Walk the tree collecting engine-readable files, preserving relative paths.
     *
     * Depth is capped because a user could hand us a whole SD card; the engine's
     * layout never nests more than a few levels.
     */
    private void collect(DocumentFile dir, String prefix, List<Entry> out, int depth) {
        if (depth > 6 || cancelled.get()) return;
        DocumentFile[] kids = dir.listFiles();
        if (kids == null) return;
        for (DocumentFile k : kids) {
            if (cancelled.get()) return;
            String name = k.getName();
            if (name == null) continue;
            if (k.isDirectory()) {
                if (shouldSkipDir(name)) continue;
                collect(k, prefix + name + "/", out, depth + 1);
            } else if (isWanted(name)) {
                out.add(new Entry(k, prefix + name));
            }
        }
    }

    private static boolean shouldSkipDir(String name) {
        String n = name.toLowerCase(Locale.US);
        // Save games, replays and the Windows redistributables are dead weight.
        return n.equals("save") || n.equals("replays") || n.equals("directx")
            || n.equals("_directx") || n.startsWith(".");
    }

    private static boolean isWanted(String name) {
        String n = name.toLowerCase(Locale.US);
        return n.endsWith(".big")     // archives: the bulk of the game
            // GeneralsX @bugfix android-port 08/02/2026 .gib is the same BIGF
            // container under a different extension. Community mods ship it
            // exclusively -- Rise of the Reds is 17 .gib files and not one .big --
            // the extension chosen so the retail game does NOT auto-load them.
            // Without this, importing a mod folder copied zero files and then
            // reported "No game files found there", which looks like a broken
            // import rather than an unsupported format.
            || n.endsWith(".gib")
            || n.endsWith(".ini")     // loose config (mods ship these unpacked)
            || n.endsWith(".bik")     // videos
            || n.endsWith(".wav") || n.endsWith(".mp3")
            || n.endsWith(".tga") || n.endsWith(".dds")
            || n.endsWith(".w3d")     // models
            || n.endsWith(".map")
            || n.endsWith(".str")     // localised text
            || n.endsWith(".csf")
            || n.endsWith(".ttf");
    }

    private long copyOne(Context ctx, Uri src, File dst) throws IOException {
        InputStream in = null;
        OutputStream os = null;
        try {
            in = ctx.getContentResolver().openInputStream(src);
            if (in == null) throw new IOException("cannot read " + src);
            os = new FileOutputStream(dst);
            byte[] buf = new byte[1 << 16];
            long n = 0;
            int r;
            while ((r = in.read(buf)) > 0) {
                if (cancelled.get()) break;
                os.write(buf, 0, r);
                n += r;
            }
            os.flush();
            return n;
        } finally {
            closeQuietly(in);
            closeQuietly(os);
        }
    }

    private static void closeQuietly(java.io.Closeable c) {
        if (c != null) {
            try { c.close(); } catch (IOException ignored) { }
        }
    }

    private static int pct(long done, long total) {
        if (total <= 0) return 0;
        return (int) Math.min(100, (done * 100L) / total);
    }

    static String human(long bytes) {
        if (bytes < 1024) return bytes + " B";
        if (bytes < 1024L * 1024) return String.format(Locale.US, "%.1f KB", bytes / 1024.0);
        if (bytes < 1024L * 1024 * 1024) return String.format(Locale.US, "%.1f MB", bytes / (1024.0 * 1024));
        return String.format(Locale.US, "%.2f GB", bytes / (1024.0 * 1024 * 1024));
    }

    private void post(final Listener l, final String msg, final int pctDone) {
        ui.post(new Runnable() { @Override public void run() { l.onProgress(msg, pctDone); } });
    }

    private void finish(final Listener l, final boolean ok, final String msg) {
        ui.post(new Runnable() { @Override public void run() { l.onFinished(ok, msg); } });
    }
}
