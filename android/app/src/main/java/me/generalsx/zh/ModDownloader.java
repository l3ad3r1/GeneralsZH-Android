// GeneralsX @feature android-port 08/04/2026
//
// ModDownloader — fetches mods straight to the tablet, no PC involved.
//
// WHERE THE FILES COME FROM
//
// Not ModDB. ModDB has no public download API, sits behind bot protection
// (an automated request returns HTTP 403), and serves Windows .exe installers
// or .rar archives that Android cannot extract. GenLauncher does not download
// from ModDB either -- its config's ModDBLink fields are just "visit the mod
// page" links. The actual files come from GenLauncher's own MinIO/S3 host, and
// that is what this talks to.
//
// So this depends on infrastructure run by the GenLauncher project
// (https://github.com/CommanderFlan/GenLauncher) rather than on anything we
// control. Credit to them; it is their bandwidth. The host is a constant below
// precisely so it is easy to find, change, or point at a mirror -- and the
// catalogue is data, not hardcoded logic, so adding or repointing an entry is a
// one-line change.
//
// INTEGRITY
//
// The endpoint speaks plain HTTP; there is no TLS on port 9000. Every byte
// therefore arrives unauthenticated and a hostile network could substitute
// content that we would then install and the engine would load. To make that
// meaningfully harder, each object's MD5 is taken from its S3 ETag at listing
// time and verified after download; a mismatch fails the install rather than
// leaving altered files in place. This is not a substitute for TLS -- an
// attacker able to rewrite the download can rewrite the listing too -- but it
// does catch corruption and casual tampering, and it costs nothing.

package me.generalsx.zh;

import android.os.Handler;
import android.os.Looper;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.net.URLEncoder;
import java.security.MessageDigest;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

public final class ModDownloader {

    /** GenLauncher's MinIO host. Plain HTTP -- see the integrity note above. */
    public static final String HOST = "http://gen.insave.ovh:9000";

    /** One downloadable entry, mirroring GenLauncher's catalogue. */
    public static final class Catalog {
        public final String name;      // shown in the picker, and the folder created
        public final String kind;      // Mod / Addon / Patch, for the label only
        public final String bucket;
        public final String folder;    // S3 key prefix
        Catalog(String n, String k, String b, String f) {
            name = n; kind = k; bucket = b; folder = f;
        }
        @Override public String toString() { return name + "  (" + kind + ")"; }
    }

    /**
     * Taken from GenLauncher's own GenLauncherCfg.yaml. Data, not logic: an
     * entry that moves needs its bucket/folder edited here and nothing else.
     */
    public static final Catalog[] CATALOG = {
        new Catalog("Rise of the Reds",            "Mod",   "rotr",    "rotr-individual-files"),
        new Catalog("Hanpatch",                    "Patch", "rotr",    "hanpatch-individual-files"),
        new Catalog("Anti Thesis Patch",           "Patch", "atp",     "atp-files"),
        new Catalog("ATP Boss Addon",              "Addon", "atp",     "boss-addon-atp"),
        new Catalog("ATP ControlBarPro Addon",     "Addon", "atp",     "cbp-addon-atp"),
        new Catalog("Empire Pacific Alliance",     "Patch", "rotrepa", "rotrepaA"),
        new Catalog("EPA No MechFactions",         "Addon", "rotrepa", "CNC_EPAADONB"),
        new Catalog("Unofficial Music Factions",   "Addon", "rotrepa", "rotrepaM"),
        new Catalog("Vanilla Campaigns Remastered","Addon", "vcr",     "Vanilla Campaigns Remastered"),
    };

    public interface Listener {
        void onProgress(String message, int percent);
        void onFinished(boolean ok, String message);
    }

    /** Result of asking the host how big a catalogue entry actually is. */
    public interface SizeListener {
        /** files/bytes are meaningful only when ok is true. */
        void onSize(boolean ok, int files, long bytes, String error);
    }

    /**
     * Look up the real file count and total size of an entry before downloading.
     *
     * GeneralsX @bugfix android-port 08/04/2026 The confirmation dialog used to
     * quote "Rise of the Reds is about 1.5 GB" for every entry, which was wrong
     * for the eight that are not Rise of the Reds -- several are a single file
     * of a few MB. The sizes are already in the S3 listing, so ask instead of
     * guessing.
     */
    public void querySize(final Catalog entry, final SizeListener l) {
        pool.execute(new Runnable() {
            @Override public void run() {
                try {
                    List<S3Object> objects = list(entry.bucket, entry.folder);
                    long total = 0;
                    for (S3Object o : objects) total += o.size;
                    final int n = objects.size();
                    final long bytes = total;
                    if (n == 0) {
                        postSize(l, false, 0, 0,
                            "The host lists no files for \"" + entry.folder + "\" in bucket \""
                            + entry.bucket + "\". GenLauncher may have moved or renamed it.");
                    } else {
                        postSize(l, true, n, bytes, null);
                    }
                } catch (Exception e) {
                    postSize(l, false, 0, 0, String.valueOf(e));
                }
            }
        });
    }

    private void postSize(final SizeListener l, final boolean ok, final int n,
                          final long bytes, final String err) {
        ui.post(new Runnable() {
            @Override public void run() { l.onSize(ok, n, bytes, err); }
        });
    }

    private static final class S3Object {
        final String key;
        final long size;
        final String md5;   // from the ETag; null when the server gave a multipart tag
        S3Object(String k, long s, String m) { key = k; size = s; md5 = m; }
        String fileName() {
            int i = key.lastIndexOf('/');
            return (i < 0) ? key : key.substring(i + 1);
        }
    }

    private final ExecutorService pool = Executors.newSingleThreadExecutor();
    private final Handler ui = new Handler(Looper.getMainLooper());
    private final AtomicBoolean cancelled = new AtomicBoolean(false);

    public void cancel() { cancelled.set(true); }

    public void download(final Catalog entry, final File destDir, final Listener listener) {
        cancelled.set(false);
        pool.execute(new Runnable() {
            @Override public void run() {
                try {
                    post(listener, "Contacting " + HOST + "…", 0);
                    List<S3Object> objects = list(entry.bucket, entry.folder);
                    if (objects.isEmpty()) {
                        finish(listener, false,
                            "Nothing to download for " + entry.name + ".\n\n" +
                            "The catalogue entry points at bucket \"" + entry.bucket +
                            "\", folder \"" + entry.folder + "\", which is empty or gone. " +
                            "GenLauncher may have moved it.");
                        return;
                    }

                    long total = 0;
                    for (S3Object o : objects) total += o.size;
                    post(listener, objects.size() + " files, " + human(total) + " to download", 0);

                    if (!destDir.exists() && !destDir.mkdirs()) {
                        finish(listener, false, "Could not create " + destDir.getAbsolutePath());
                        return;
                    }

                    long done = 0;
                    int got = 0, skipped = 0;
                    for (S3Object o : objects) {
                        if (cancelled.get()) {
                            finish(listener, false, "Download cancelled.");
                            return;
                        }
                        File out = new File(destDir, o.fileName());

                        // Resume-friendly: a file already present at the right
                        // size AND checksum is left alone. Mods here run to
                        // gigabytes, so re-fetching what we have is not free.
                        if (out.isFile() && out.length() == o.size
                                && (o.md5 == null || o.md5.equalsIgnoreCase(md5Of(out)))) {
                            done += o.size;
                            skipped++;
                            post(listener, "Have " + o.fileName(), pct(done, total));
                            continue;
                        }

                        post(listener, "Downloading " + o.fileName()
                                + "  (" + human(o.size) + ")", pct(done, total));
                        String err = fetch(entry.bucket, o, out);
                        if (err != null) {
                            finish(listener, false, "Failed on " + o.fileName() + ":\n" + err);
                            return;
                        }
                        done += o.size;
                        got++;
                    }

                    finish(listener, true, entry.name + " installed.\n\n"
                            + got + " downloaded, " + skipped + " already present ("
                            + human(total) + ")\n" + destDir.getAbsolutePath()
                            + "\n\nSelect it in the Mod list to play.");

                } catch (Exception e) {
                    finish(listener, false, "Download failed: " + e);
                }
            }
        });
    }

    /** Anonymous ListObjectsV2, following continuation tokens. */
    private List<S3Object> list(String bucket, String prefix) throws IOException {
        List<S3Object> out = new ArrayList<>();
        String token = null;
        // <Contents> gives key, size and ETag; parsed with a regex because
        // pulling in an XML parser for three fields is not worth it.
        Pattern p = Pattern.compile(
            "<Contents>.*?<Key>(.*?)</Key>.*?<ETag>&#34;([0-9a-fA-F]+)(?:-\\d+)?&#34;</ETag>"
            + ".*?<Size>(\\d+)</Size>.*?</Contents>", Pattern.DOTALL);

        for (int page = 0; page < 40; page++) {          // hard stop; no runaway paging
            StringBuilder u = new StringBuilder(HOST).append('/').append(bucket)
                    .append("?list-type=2&max-keys=1000&prefix=")
                    .append(URLEncoder.encode(prefix + "/", "UTF-8"));
            if (token != null) {
                u.append("&continuation-token=").append(URLEncoder.encode(token, "UTF-8"));
            }
            String xml = getText(u.toString());

            Matcher m = p.matcher(xml);
            while (m.find()) {
                String key = m.group(1);
                if (key.endsWith("/")) continue;          // directory marker
                out.add(new S3Object(key, Long.parseLong(m.group(3)), m.group(2)));
            }
            if (!xml.contains("<IsTruncated>true</IsTruncated>")) break;
            Matcher t = Pattern.compile("<NextContinuationToken>(.*?)</NextContinuationToken>")
                    .matcher(xml);
            if (!t.find()) break;
            token = t.group(1);
        }
        return out;
    }

    private String getText(String url) throws IOException {
        HttpURLConnection c = open(url);
        try {
            InputStream in = c.getInputStream();
            StringBuilder sb = new StringBuilder();
            byte[] buf = new byte[8192];
            int r;
            while ((r = in.read(buf)) > 0) sb.append(new String(buf, 0, r, "UTF-8"));
            return sb.toString();
        } finally {
            c.disconnect();
        }
    }

    /** Returns null on success, else a message. Verifies MD5 before keeping the file. */
    private String fetch(String bucket, S3Object o, File out) {
        File tmp = new File(out.getAbsolutePath() + ".part");
        HttpURLConnection c = null;
        InputStream in = null;
        OutputStream os = null;
        try {
            StringBuilder u = new StringBuilder(HOST).append('/').append(bucket).append('/');
            for (String seg : o.key.split("/")) {
                u.append(URLEncoder.encode(seg, "UTF-8").replace("+", "%20")).append('/');
            }
            u.setLength(u.length() - 1);

            c = open(u.toString());
            in = c.getInputStream();
            os = new FileOutputStream(tmp);
            MessageDigest md = MessageDigest.getInstance("MD5");
            byte[] buf = new byte[1 << 16];
            int r;
            while ((r = in.read(buf)) > 0) {
                if (cancelled.get()) return "cancelled";
                os.write(buf, 0, r);
                md.update(buf, 0, r);
            }
            os.flush();
            closeQuietly(os); os = null;

            if (o.md5 != null) {
                String got = hex(md.digest());
                if (!got.equalsIgnoreCase(o.md5)) {
                    //noinspection ResultOfMethodCallIgnored
                    tmp.delete();
                    return "checksum mismatch (expected " + o.md5 + ", got " + got + ").\n"
                         + "The file was corrupted or altered in transit; it has been discarded.";
                }
            }
            //noinspection ResultOfMethodCallIgnored
            out.delete();
            if (!tmp.renameTo(out)) return "could not move the finished file into place";
            return null;
        } catch (Exception e) {
            //noinspection ResultOfMethodCallIgnored
            tmp.delete();
            return String.valueOf(e);
        } finally {
            closeQuietly(in);
            closeQuietly(os);
            if (c != null) c.disconnect();
        }
    }

    private HttpURLConnection open(String url) throws IOException {
        HttpURLConnection c = (HttpURLConnection) new URL(url).openConnection();
        c.setConnectTimeout(20000);
        c.setReadTimeout(60000);
        // Identify ourselves honestly rather than impersonating a browser, so the
        // host's operators can see who is using their bandwidth.
        c.setRequestProperty("User-Agent", "GeneralsX-Android-Launcher");
        return c;
    }

    private static String md5Of(File f) {
        InputStream in = null;
        try {
            MessageDigest md = MessageDigest.getInstance("MD5");
            in = new java.io.FileInputStream(f);
            byte[] buf = new byte[1 << 16];
            int r;
            while ((r = in.read(buf)) > 0) md.update(buf, 0, r);
            return hex(md.digest());
        } catch (Exception e) {
            return "";
        } finally {
            closeQuietly(in);
        }
    }

    private static String hex(byte[] b) {
        StringBuilder sb = new StringBuilder();
        for (byte x : b) sb.append(String.format(Locale.US, "%02x", x));
        return sb.toString();
    }

    private static void closeQuietly(java.io.Closeable c) {
        if (c != null) { try { c.close(); } catch (IOException ignored) { } }
    }

    private static int pct(long done, long total) {
        return (total <= 0) ? 0 : (int) Math.min(100, done * 100L / total);
    }

    static String human(long b) {
        if (b < 1024) return b + " B";
        if (b < 1024L * 1024) return String.format(Locale.US, "%.1f KB", b / 1024.0);
        if (b < 1024L * 1024 * 1024) return String.format(Locale.US, "%.1f MB", b / (1024.0 * 1024));
        return String.format(Locale.US, "%.2f GB", b / (1024.0 * 1024 * 1024));
    }

    private void post(final Listener l, final String m, final int p) {
        ui.post(new Runnable() { @Override public void run() { l.onProgress(m, p); } });
    }

    private void finish(final Listener l, final boolean ok, final String m) {
        ui.post(new Runnable() { @Override public void run() { l.onFinished(ok, m); } });
    }
}
