// GeneralsX @feature android-port 08/02/2026
//
// LauncherActivity — the app's entry point, in the spirit of the desktop
// "RSP Launcher": choose which data set to run, toggle the startup flags, import
// game files, manage mods, then launch.
//
// It exists because everything it does previously required a PC and adb: copying
// .big archives in, switching between installs, and passing command-line flags.
// The engine already understood -mod / -nologo / -noshellmap / -win; the only new
// engine-side support needed was -datadir (SDL3Main.cpp), which points the
// working directory at a chosen profile.
//
// The UI is built in code rather than XML on purpose: this module has no
// AppCompat theme of its own and only a handful of controls, so a layout file
// plus styles would be more moving parts than the screen is worth.

package me.generalsx.zh;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.graphics.Color;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.text.TextUtils;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.AdapterView;
import android.widget.ArrayAdapter;
import android.widget.Button;
import android.widget.CheckBox;
import android.widget.CompoundButton;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.ScrollView;
import android.widget.Spinner;
import android.widget.TextView;
import android.widget.Toast;

import java.io.File;
import java.util.ArrayList;
import java.util.List;

public class LauncherActivity extends Activity {

    private static final int REQ_IMPORT_GAME   = 1001;
    private static final int REQ_IMPORT_MOD    = 1002;
    private static final int REQ_EXPORT_REPORT = 1003;
    private static final int REQ_EXPORT_LOG    = 1004;

    private static final int BG     = 0xFF12161C;
    private static final int ACCENT = 0xFF4FA3FF;
    private static final int TEXT   = 0xFFE6EDF5;
    private static final int MUTED  = 0xFF93A1B2;

    private Spinner engineSpinner;
    private Spinner profileSpinner;
    private Spinner modSpinner;
    private CheckBox skipIntro, noShellMap, windowed, noShadowVolumes;
    private TextView statusText, argsText, titleText;
    private ProgressBar progress;
    private Button playButton;

    private final GameDataImporter importer = new GameDataImporter();
    private final ModDownloader downloader = new ModDownloader();
    private List<String> profiles = new ArrayList<>();
    private List<String> mods = new ArrayList<>();

    // GeneralsX @feature android-port 16/08/2026 Last game-file-check report, held
    // so "Export report…" can write it out after the SAF create-document dialog
    // returns on a later turn of the event loop.
    private String lastReport;

    // Engine log chosen in the export picker, held for the same reason.
    private File pendingLogFile;

    @Override
    protected void onCreate(Bundle saved) {
        super.onCreate(saved);
        setContentView(buildUi());
        refresh();
    }

    @Override
    protected void onResume() {
        super.onResume();
        refresh();   // data may have changed while the game or a file manager ran
    }

    // ---------------------------------------------------------------- UI ----

    private View buildUi() {
        ScrollView scroller = new ScrollView(this);
        scroller.setBackgroundColor(BG);
        scroller.setFillViewport(true);

        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setPadding(dp(20), dp(20), dp(20), dp(20));
        scroller.addView(root, new ScrollView.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));

        TextView title = new TextView(this);
        title.setText("COMMAND & CONQUER");
        title.setTextColor(MUTED);
        title.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12f);
        title.setLetterSpacing(0.25f);
        root.addView(title);

        // GeneralsX @bugfix android-port 08/04/2026 The heading was a hardcoded
        // "GENERALS ZERO:HOUR" -- wrong on two counts: the game is "Zero Hour",
        // and the heading became inaccurate once the APK shipped
        // both engines: selecting Generals still showed the Zero Hour title.
        // It now follows the selected game.
        titleText = new TextView(this);
        titleText.setTextColor(TEXT);
        titleText.setTextSize(TypedValue.COMPLEX_UNIT_SP, 26f);
        titleText.setTypeface(null, android.graphics.Typeface.BOLD);
        root.addView(titleText);

        TextView sub = new TextView(this);
        sub.setText("Android launcher");
        sub.setTextColor(ACCENT);
        sub.setTextSize(TypedValue.COMPLEX_UNIT_SP, 13f);
        sub.setPadding(0, dp(2), 0, dp(16));
        root.addView(sub);

        // ---- game (engine) ----
        root.addView(sectionLabel("Game"));
        engineSpinner = new Spinner(this);
        engineSpinner.setBackgroundColor(0xFF1B2230);
        root.addView(engineSpinner, rowParams());
        engineSpinner.setAdapter(adapter(java.util.Arrays.asList(
                "Command & Conquer: Generals — Zero Hour",
                "Command & Conquer: Generals (original)")));
        engineSpinner.setSelection(
                LauncherConfig.ENGINE_GENERALS.equals(
                        prefs().getString(LauncherConfig.KEY_ENGINE,
                                          LauncherConfig.ENGINE_ZEROHOUR)) ? 1 : 0);
        engineSpinner.setOnItemSelectedListener(new AdapterView.OnItemSelectedListener() {
            @Override public void onItemSelected(AdapterView<?> p, View v, int pos, long id) {
                String want = (pos == 1) ? LauncherConfig.ENGINE_GENERALS
                                         : LauncherConfig.ENGINE_ZEROHOUR;
                if (!want.equals(LauncherConfig.engine(LauncherActivity.this))) {
                    prefs().edit().putString(LauncherConfig.KEY_ENGINE, want).apply();
                    // Each engine remembers its own data set, so re-read them.
                    refresh();
                }
            }
            @Override public void onNothingSelected(AdapterView<?> p) { }
        });

        // ---- game / profile ----
        root.addView(sectionLabel("Game data"));
        profileSpinner = new Spinner(this);
        profileSpinner.setBackgroundColor(0xFF1B2230);
        root.addView(profileSpinner, rowParams());
        profileSpinner.setOnItemSelectedListener(new AdapterView.OnItemSelectedListener() {
            @Override public void onItemSelected(AdapterView<?> p, View v, int pos, long id) {
                if (pos >= 0 && pos < profiles.size()) {
                    LauncherConfig.setSelectedProfile(LauncherActivity.this, profiles.get(pos));
                    updateSummary();
                }
            }
            @Override public void onNothingSelected(AdapterView<?> p) { }
        });

        LinearLayout dataRow = new LinearLayout(this);
        dataRow.setOrientation(LinearLayout.HORIZONTAL);
        root.addView(dataRow, rowParams());
        dataRow.addView(smallButton("Import game files…", new View.OnClickListener() {
            @Override public void onClick(View v) { pickFolder(REQ_IMPORT_GAME); }
        }), weight1());
        dataRow.addView(smallButton("Storage info", new View.OnClickListener() {
            @Override public void onClick(View v) { showStorageInfo(); }
        }), weight1());

        // GeneralsX @feature android-port 16/08/2026 Diagnostics, kept on their own
        // row rather than crowded into the data row: the two together are what you
        // reach for when the game will not start.
        //   - Verify game files pre-flights the archives before the native engine
        //     sees them, so a truncated .big is named here instead of taking the
        //     engine down at load.
        //   - Export engine log lifts the engine's own stderr off the device. That
        //     log is the only place startup failures are explained (a DXVK/Vulkan
        //     mismatch, for one, prints there and nowhere else), and until now it
        //     needed adb to retrieve -- which is exactly what a player reporting a
        //     bug does not have.
        LinearLayout diagRow = new LinearLayout(this);
        diagRow.setOrientation(LinearLayout.HORIZONTAL);
        root.addView(diagRow, rowParams());
        diagRow.addView(smallButton("Verify game files", new View.OnClickListener() {
            @Override public void onClick(View v) { verifyGameFiles(); }
        }), weight1());
        diagRow.addView(smallButton("Export engine log…", new View.OnClickListener() {
            @Override public void onClick(View v) { exportEngineLog(); }
        }), weight1());

        // ---- mods ----
        root.addView(sectionLabel("Mod (applied over the selected game data)"));
        modSpinner = new Spinner(this);
        modSpinner.setBackgroundColor(0xFF1B2230);
        root.addView(modSpinner, rowParams());
        modSpinner.setOnItemSelectedListener(new AdapterView.OnItemSelectedListener() {
            @Override public void onItemSelected(AdapterView<?> p, View v, int pos, long id) {
                String value = (pos <= 0) ? "" : mods.get(pos - 1);
                prefs().edit().putString(LauncherConfig.KEY_MOD, value).apply();
                updateSummary();
            }
            @Override public void onNothingSelected(AdapterView<?> p) { }
        });

        LinearLayout modRow = new LinearLayout(this);
        modRow.setOrientation(LinearLayout.HORIZONTAL);
        root.addView(modRow, rowParams());
        modRow.addView(smallButton("Install mod…", new View.OnClickListener() {
            @Override public void onClick(View v) { pickFolder(REQ_IMPORT_MOD); }
        }), weight1());
        modRow.addView(smallButton("Remove mod", new View.OnClickListener() {
            @Override public void onClick(View v) { confirmRemoveMod(); }
        }), weight1());

        LinearLayout dlRow = new LinearLayout(this);
        dlRow.setOrientation(LinearLayout.HORIZONTAL);
        root.addView(dlRow, rowParams());
        dlRow.addView(smallButton("Download mod…", new View.OnClickListener() {
            @Override public void onClick(View v) { showDownloadPicker(); }
        }), weight1());

        // ---- options ----
        root.addView(sectionLabel("Startup options"));
        skipIntro  = check("Skip intro videos", LauncherConfig.KEY_SKIP_INTRO, true);
        noShellMap = check("Disable animated menu background (faster on weak GPUs)",
                           LauncherConfig.KEY_NO_SHELLMAP, false);
        windowed   = check("Windowed", LauncherConfig.KEY_WINDOWED, false);
        noShadowVolumes = check("Disable shadow volumes (fixes black buildings/shadows)",
                                LauncherConfig.KEY_NO_SHADOW_VOLUMES, true);
        root.addView(skipIntro);
        root.addView(noShellMap);
        root.addView(windowed);
        root.addView(noShadowVolumes);

        // ---- extra arguments ----
        // GeneralsX @feature android-port 08/02/2026 Free-form engine flags. The
        // engine understands far more than the checkboxes above (-noshroud,
        // -lowDetail, -xres/-yres, ...), and being able to try one without a
        // rebuild is the difference between diagnosing a rendering problem in
        // minutes and in a build cycle.
        root.addView(sectionLabel("Extra arguments (advanced, optional)"));
        final android.widget.EditText extraArgs = new android.widget.EditText(this);
        extraArgs.setHint("e.g. -noshroud -lowDetail");
        extraArgs.setTextColor(TEXT);
        extraArgs.setHintTextColor(0xFF6F8098);
        extraArgs.setTextSize(TypedValue.COMPLEX_UNIT_SP, 13f);
        extraArgs.setSingleLine(true);
        extraArgs.setText(prefs().getString(LauncherConfig.KEY_EXTRA_ARGS, ""));
        extraArgs.addTextChangedListener(new android.text.TextWatcher() {
            @Override public void beforeTextChanged(CharSequence c, int a, int b, int d) { }
            @Override public void onTextChanged(CharSequence c, int a, int b, int d) { }
            @Override public void afterTextChanged(android.text.Editable e) {
                prefs().edit().putString(LauncherConfig.KEY_EXTRA_ARGS, e.toString()).apply();
                updateSummary();
            }
        });
        root.addView(extraArgs, rowParams());

        // ---- status ----
        statusText = new TextView(this);
        statusText.setTextColor(MUTED);
        statusText.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12f);
        statusText.setPadding(0, dp(14), 0, dp(4));
        root.addView(statusText);

        argsText = new TextView(this);
        argsText.setTextColor(0xFF6F8098);
        argsText.setTextSize(TypedValue.COMPLEX_UNIT_SP, 11f);
        argsText.setTypeface(android.graphics.Typeface.MONOSPACE);
        argsText.setPadding(0, 0, 0, dp(10));
        root.addView(argsText);

        progress = new ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal);
        progress.setMax(100);
        progress.setVisibility(View.GONE);
        root.addView(progress, rowParams());

        // ---- play ----
        playButton = new Button(this);
        playButton.setText("PLAY GAME");
        playButton.setAllCaps(true);
        playButton.setTextSize(TypedValue.COMPLEX_UNIT_SP, 18f);
        playButton.setTextColor(Color.WHITE);
        playButton.setBackgroundColor(0xFF2D6FB8);
        playButton.setPadding(dp(12), dp(14), dp(12), dp(14));
        playButton.setOnClickListener(new View.OnClickListener() {
            @Override public void onClick(View v) { launchGame(); }
        });
        LinearLayout.LayoutParams pp = rowParams();
        pp.topMargin = dp(10);
        root.addView(playButton, pp);

        return scroller;
    }

    private TextView sectionLabel(String s) {
        TextView t = new TextView(this);
        t.setText(s);
        t.setTextColor(ACCENT);
        t.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12f);
        t.setPadding(0, dp(14), 0, dp(4));
        return t;
    }

    private CheckBox check(final String label, final String key, final boolean def) {
        CheckBox c = new CheckBox(this);
        c.setText(label);
        c.setTextColor(TEXT);
        c.setTextSize(TypedValue.COMPLEX_UNIT_SP, 14f);
        c.setChecked(prefs().getBoolean(key, def));
        c.setOnCheckedChangeListener(new CompoundButton.OnCheckedChangeListener() {
            @Override public void onCheckedChanged(CompoundButton b, boolean on) {
                prefs().edit().putBoolean(key, on).apply();
                updateSummary();
            }
        });
        return c;
    }

    private Button smallButton(String label, View.OnClickListener l) {
        Button b = new Button(this);
        b.setText(label);
        b.setAllCaps(false);
        b.setTextSize(TypedValue.COMPLEX_UNIT_SP, 13f);
        b.setTextColor(TEXT);
        b.setBackgroundColor(0xFF243040);
        b.setOnClickListener(l);
        return b;
    }

    private LinearLayout.LayoutParams rowParams() {
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        lp.bottomMargin = dp(6);
        return lp;
    }

    private LinearLayout.LayoutParams weight1() {
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(
                0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f);
        lp.rightMargin = dp(6);
        return lp;
    }

    private int dp(int v) {
        return Math.round(getResources().getDisplayMetrics().density * v);
    }

    private SharedPreferences prefs() { return LauncherConfig.prefs(this); }

    // ------------------------------------------------------------ state ----

    private void refresh() {
        profiles = LauncherConfig.availableProfiles(this);
        mods = LauncherConfig.availableMods(this);

        List<String> profileLabels = new ArrayList<>();
        for (String p : profiles) {
            File dir = LauncherConfig.profileDir(this, p);
            profileLabels.add(p + "  (" + LauncherConfig.countBigFiles(dir) + " archives)");
        }
        if (profileLabels.isEmpty()) {
            profileLabels.add("No game data installed");
        }
        profileSpinner.setAdapter(adapter(profileLabels));
        String savedProfile = LauncherConfig.getSelectedProfile(this);
        int idx = profiles.indexOf(savedProfile);
        if (idx >= 0) profileSpinner.setSelection(idx);

        List<String> modLabels = new ArrayList<>();
        modLabels.add("None");
        modLabels.addAll(mods);
        modSpinner.setAdapter(adapter(modLabels));
        String savedMod = prefs().getString(LauncherConfig.KEY_MOD, "");
        int midx = mods.indexOf(savedMod);
        modSpinner.setSelection(midx >= 0 ? midx + 1 : 0);

        updateSummary();
    }

    private ArrayAdapter<String> adapter(List<String> items) {
        ArrayAdapter<String> a = new ArrayAdapter<String>(
                this, android.R.layout.simple_spinner_item, items) {
            @Override public View getView(int p, View c, ViewGroup parent) {
                TextView t = (TextView) super.getView(p, c, parent);
                t.setTextColor(TEXT);
                return t;
            }
        };
        a.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item);
        return a;
    }

    private void updateSummary() {
        boolean ready = !profiles.isEmpty();
        playButton.setEnabled(ready);
        playButton.setBackgroundColor(ready ? 0xFF2D6FB8 : 0xFF33404F);

        File root = LauncherConfig.storageRoot(this);
        if (root == null) {
            statusText.setText("External storage is unavailable.");
        } else if (!ready) {
            statusText.setText(
                "No game data found.\n\n" +
                "Tap the Import game files button and pick the folder holding your game "
                + "archives (.big for a game install, .gib for a mod), or push them to:\n"
                + new File(root, LauncherConfig.STOCK_PROFILE).getAbsolutePath());
        } else {
            File p = LauncherConfig.profileDir(this, LauncherConfig.getSelectedProfile(this));
            statusText.setText("Ready — " + (p == null ? "?" : p.getAbsolutePath()));
        }
        if (titleText != null) {
            titleText.setText(LauncherConfig.isGenerals(this)
                    ? "GENERALS" : "GENERALS ZERO HOUR");
        }
        argsText.setText(LauncherConfig.describeArguments(this));
    }

    // ----------------------------------------------------------- actions ----

    private void launchGame() {
        if (profiles.isEmpty()) {
            toast("No game data installed yet.");
            return;
        }
        ArrayList<String> args = LauncherConfig.buildArguments(this);
        Intent i = new Intent(this, GameActivity.class);
        i.putStringArrayListExtra(GameActivity.EXTRA_ARGS, args);
        startActivity(i);
    }

    private void pickFolder(int requestCode) {
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
        i.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
        try {
            startActivityForResult(i, requestCode);
        } catch (Exception e) {
            toast("No file picker available on this device.");
        }
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (resultCode != RESULT_OK || data == null || data.getData() == null) return;
        Uri tree = data.getData();

        if (requestCode == REQ_IMPORT_GAME) {
            File dest = LauncherConfig.stockProfileDir(this);
            if (dest == null) { toast("External storage unavailable."); return; }
            askProfileNameThenImport(tree, dest);
        } else if (requestCode == REQ_IMPORT_MOD) {
            promptModName(tree);
        } else if (requestCode == REQ_EXPORT_REPORT) {
            writeReport(tree);
        } else if (requestCode == REQ_EXPORT_LOG) {
            writeLog(tree);
        }
    }

    /**
     * Importing a second data set must not overwrite the first, so when a stock
     * install already exists the user is asked where this one should go.
     */
    private void askProfileNameThenImport(final Uri tree, final File stockDir) {
        boolean stockPopulated = LauncherConfig.hasGameData(stockDir);
        if (!stockPopulated) {
            startImport(tree, stockDir, "game data");
            return;
        }
        final android.widget.EditText input = new android.widget.EditText(this);
        input.setHint("e.g. ShockWave");
        new AlertDialog.Builder(this)
            .setTitle("Import as a new profile")
            .setMessage("Game data is already installed. Give this one a name so both are kept, "
                      + "or choose Replace to overwrite the existing install.")
            .setView(input)
            .setPositiveButton("Import as new", new android.content.DialogInterface.OnClickListener() {
                @Override public void onClick(android.content.DialogInterface d, int w) {
                    String name = sanitize(input.getText().toString());
                    if (name.isEmpty()) { toast("Enter a name."); return; }
                    File root = LauncherConfig.profilesRoot(LauncherActivity.this);
                    if (root == null) { toast("External storage unavailable."); return; }
                    startImport(tree, new File(root, name), name);
                }
            })
            .setNeutralButton("Replace", new android.content.DialogInterface.OnClickListener() {
                @Override public void onClick(android.content.DialogInterface d, int w) {
                    startImport(tree, stockDir, "game data");
                }
            })
            .setNegativeButton("Cancel", null)
            .show();
    }

    private void promptModName(final Uri tree) {
        final android.widget.EditText input = new android.widget.EditText(this);
        input.setHint("e.g. ShockWave, Rise of the Reds");
        new AlertDialog.Builder(this)
            .setTitle("Install mod")
            .setMessage("Name this mod. It will be applied over the selected game data with the "
                      + "engine's -mod flag.")
            .setView(input)
            .setPositiveButton("Install", new android.content.DialogInterface.OnClickListener() {
                @Override public void onClick(android.content.DialogInterface d, int w) {
                    String name = sanitize(input.getText().toString());
                    if (name.isEmpty()) { toast("Enter a name."); return; }
                    File root = LauncherConfig.modsRoot(LauncherActivity.this);
                    if (root == null) { toast("External storage unavailable."); return; }
                    startImport(tree, new File(root, name), name);
                }
            })
            .setNegativeButton("Cancel", null)
            .show();
    }

    /** Strip path separators so a typed name cannot escape the intended directory. */
    private static String sanitize(String s) {
        if (s == null) return "";
        return s.trim().replaceAll("[^A-Za-z0-9 _.\\-]", "").trim();
    }

    private void startImport(Uri tree, final File dest, final String what) {
        setBusy(true);
        statusText.setText("Preparing to import " + what + "…");
        importer.importFrom(this, tree, dest, new GameDataImporter.Listener() {
            @Override public void onProgress(String message, int percent) {
                statusText.setText(message);
                progress.setProgress(percent);
            }
            @Override public void onFinished(boolean ok, String message) {
                setBusy(false);
                refresh();
                new AlertDialog.Builder(LauncherActivity.this)
                    .setTitle(ok ? "Import complete" : "Import failed")
                    .setMessage(message)
                    .setPositiveButton("OK", null)
                    .show();
            }
        });
    }

    private void setBusy(boolean busy) {
        progress.setVisibility(busy ? View.VISIBLE : View.GONE);
        progress.setProgress(0);
        playButton.setEnabled(!busy && !profiles.isEmpty());
    }

    /**
     * Pick a mod to download.
     *
     * Files come from the GenLauncher project's own host, not from ModDB --
     * ModDB has no download API, blocks automated clients, and serves Windows
     * installers. See ModDownloader for the details and the integrity caveat.
     */
    private void showDownloadPicker() {
        final File modsRoot = LauncherConfig.modsRoot(this);
        if (modsRoot == null) { toast("External storage unavailable."); return; }

        final ModDownloader.Catalog[] cat = ModDownloader.CATALOG;
        final String[] labels = new String[cat.length];
        for (int i = 0; i < cat.length; i++) labels[i] = cat[i].toString();

        new AlertDialog.Builder(this)
            .setTitle("Download a mod")
            .setItems(labels, new android.content.DialogInterface.OnClickListener() {
                @Override public void onClick(android.content.DialogInterface d, int which) {
                    askSizeThenConfirm(cat[which], new File(modsRoot, cat[which].name));
                }
            })
            .setNegativeButton("Cancel", null)
            .show();
    }

    /**
     * Ask the host how big this entry really is, then confirm.
     *
     * GeneralsX @bugfix android-port 08/04/2026 The confirmation dialog used to
     * say "Rise of the Reds is about 1.5 GB" whichever entry you picked, which
     * was simply false for the other eight -- some are a single file of a few MB.
     * A download prompt that misstates the size is worse than no size at all,
     * since it is exactly the number someone checks before using mobile data.
     */
    private void askSizeThenConfirm(final ModDownloader.Catalog entry, final File dest) {
        setBusy(true);
        statusText.setText("Checking size of " + entry.name + "…");
        downloader.querySize(entry, new ModDownloader.SizeListener() {
            @Override public void onSize(boolean ok, int files, long bytes, String error) {
                setBusy(false);
                if (!ok) {
                    statusText.setText("Could not reach the mod host.");
                    new AlertDialog.Builder(LauncherActivity.this)
                        .setTitle("Cannot reach the mod host")
                        .setMessage(error)
                        .setPositiveButton("OK", null)
                        .show();
                    return;
                }
                updateSummary();
                confirmDownload(entry, dest, files, bytes);
            }
        });
    }

    private void confirmDownload(final ModDownloader.Catalog entry, final File dest,
                                 int files, long bytes) {
        String size = ModDownloader.human(bytes);
        new AlertDialog.Builder(this)
            .setTitle("Download " + entry.name + "?")
            .setMessage(files + " file" + (files == 1 ? "" : "s") + ", " + size
                      + " total.\n\n"
                      + "These files are hosted by the GenLauncher project, not by this app "
                      + "and not by ModDB.\n\n"
                      + "The connection is plain HTTP, so every file is checked against its "
                      + "published MD5 after download and discarded if it does not match.\n\n"
                      + "Use Wi-Fi if you are paying for data.")
            .setPositiveButton("Download " + size,
                new android.content.DialogInterface.OnClickListener() {
                    @Override public void onClick(android.content.DialogInterface d, int w) {
                        startDownload(entry, dest);
                    }
                })
            .setNegativeButton("Cancel", null)
            .show();
    }

    private void startDownload(ModDownloader.Catalog entry, File dest) {
        setBusy(true);
        statusText.setText("Preparing to download " + entry.name + "…");
        downloader.download(entry, dest, new ModDownloader.Listener() {
            @Override public void onProgress(String message, int percent) {
                statusText.setText(message);
                progress.setProgress(percent);
            }
            @Override public void onFinished(boolean ok, String message) {
                setBusy(false);
                refresh();
                new AlertDialog.Builder(LauncherActivity.this)
                    .setTitle(ok ? "Download complete" : "Download failed")
                    .setMessage(message)
                    .setPositiveButton("OK", null)
                    .show();
            }
        });
    }

    private void confirmRemoveMod() {
        final String mod = prefs().getString(LauncherConfig.KEY_MOD, "");
        if (TextUtils.isEmpty(mod)) { toast("No mod selected."); return; }
        final File root = LauncherConfig.modsRoot(this);
        if (root == null) return;
        final File dir = new File(root, mod);
        new AlertDialog.Builder(this)
            .setTitle("Remove mod")
            .setMessage("Delete \"" + mod + "\" and its files?\n\n" + dir.getAbsolutePath()
                      + "\n\nYour game data is not touched.")
            .setPositiveButton("Delete", new android.content.DialogInterface.OnClickListener() {
                @Override public void onClick(android.content.DialogInterface d, int w) {
                    deleteRecursive(dir);
                    prefs().edit().putString(LauncherConfig.KEY_MOD, "").apply();
                    refresh();
                    toast("Removed " + mod);
                }
            })
            .setNegativeButton("Cancel", null)
            .show();
    }

    private static void deleteRecursive(File f) {
        if (f == null || !f.exists()) return;
        if (f.isDirectory()) {
            File[] kids = f.listFiles();
            if (kids != null) for (File k : kids) deleteRecursive(k);
        }
        // Best effort: a file we cannot delete should not abort the rest.
        //noinspection ResultOfMethodCallIgnored
        f.delete();
    }

    private void showStorageInfo() {
        File root = LauncherConfig.storageRoot(this);
        StringBuilder sb = new StringBuilder();
        if (root == null) {
            sb.append("External storage unavailable.");
        } else {
            sb.append("Data location:\n").append(root.getAbsolutePath()).append("\n\n");
            long free = root.getUsableSpace();
            sb.append("Free space: ").append(GameDataImporter.human(free)).append("\n\n");
            sb.append("Installed game data:\n");
            List<String> ps = LauncherConfig.availableProfiles(this);
            if (ps.isEmpty()) {
                sb.append("  (none)\n");
            } else {
                for (String p : ps) {
                    File d = LauncherConfig.profileDir(this, p);
                    sb.append("  • ").append(p).append(" — ")
                      .append(LauncherConfig.countBigFiles(d)).append(" archives\n");
                }
            }
            List<String> ms = LauncherConfig.availableMods(this);
            sb.append("\nInstalled mods:\n");
            if (ms.isEmpty()) sb.append("  (none)\n");
            else for (String m : ms) sb.append("  • ").append(m).append('\n');

            sb.append("\nTo copy files with adb instead:\n")
              .append("adb push *.big *.gib \"")
              .append(new File(root, LauncherConfig.STOCK_PROFILE).getAbsolutePath())
              .append("/\"");
        }
        new AlertDialog.Builder(this)
            .setTitle("Storage")
            .setMessage(sb.toString())
            .setPositiveButton("OK", null)
            .show();
    }

    // ------------------------------------------------ game file checker ----

    /**
     * Validate the selected profile's archives (and the active mod's) before the
     * engine ever sees them. The header read is cheap, but a profile can hold
     * dozens of files, so it runs off the UI thread.
     */
    private void verifyGameFiles() {
        if (profiles.isEmpty()) { toast("No game data installed yet."); return; }

        final String engineLabel = LauncherConfig.engineLabel(LauncherConfig.engine(this));
        final String profileName = LauncherConfig.getSelectedProfile(this);
        final File profileDir = LauncherConfig.profileDir(this, profileName);
        final String modName = prefs().getString(LauncherConfig.KEY_MOD, "");
        final File modsRoot = LauncherConfig.modsRoot(this);
        final File modDir = (modName == null || modName.isEmpty() || modsRoot == null)
                ? null : new File(modsRoot, modName);

        setBusy(true);
        statusText.setText("Checking game files…");
        new Thread(new Runnable() {
            @Override public void run() {
                final GameFileChecker.Result r = GameFileChecker.check(
                        engineLabel, profileName, profileDir, modName, modDir);
                runOnUiThread(new Runnable() {
                    @Override public void run() {
                        setBusy(false);
                        updateSummary();
                        lastReport = r.report;
                        showCheckResult(r);
                    }
                });
            }
        }).start();
    }

    private void showCheckResult(GameFileChecker.Result r) {
        TextView tv = new TextView(this);
        tv.setText(r.report);
        tv.setTextColor(TEXT);
        tv.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12f);
        tv.setTypeface(android.graphics.Typeface.MONOSPACE);
        tv.setPadding(dp(16), dp(8), dp(16), dp(8));
        tv.setTextIsSelectable(true);
        ScrollView sv = new ScrollView(this);
        sv.addView(tv);

        String title = r.problems == 0
                ? "Game files OK (" + r.archivesChecked + " checked)"
                : r.problems + " problem" + (r.problems == 1 ? "" : "s") + " found";
        new AlertDialog.Builder(this)
            .setTitle(title)
            .setView(sv)
            .setPositiveButton("Close", null)
            .setNeutralButton("Export report…", new android.content.DialogInterface.OnClickListener() {
                @Override public void onClick(android.content.DialogInterface d, int w) { exportReport(); }
            })
            .show();
    }

    /** Let the user save the last report anywhere, via the system file picker. */
    private void exportReport() {
        if (lastReport == null) { toast("Run a check first."); return; }
        String name = "generalsx-filecheck-"
                + LauncherConfig.engine(this) + "-"
                + sanitize(LauncherConfig.getSelectedProfile(this)).replace(' ', '_')
                + ".txt";
        Intent i = new Intent(Intent.ACTION_CREATE_DOCUMENT);
        i.addCategory(Intent.CATEGORY_OPENABLE);
        i.setType("text/plain");
        i.putExtra(Intent.EXTRA_TITLE, name);
        try {
            startActivityForResult(i, REQ_EXPORT_REPORT);
        } catch (Exception e) {
            toast("No file picker available to save the report.");
        }
    }

    private void writeReport(Uri dest) {
        String report = lastReport;
        if (report == null) { toast("Nothing to export."); return; }
        try (java.io.OutputStream os = getContentResolver().openOutputStream(dest)) {
            if (os == null) { toast("Could not open the chosen file."); return; }
            os.write(report.getBytes(java.nio.charset.StandardCharsets.UTF_8));
            os.flush();
            toast("Report saved.");
        } catch (Exception e) {
            toast("Export failed: " + e.getMessage());
        }
    }

    // ------------------------------------------------- engine log export ----

    /** The engine's own stderr, written by SageAndroid_Bootstrap into this dir. */
    static final String LOG_CURRENT  = "generals-stderr.log";
    static final String LOG_PREVIOUS = "generals-stderr-prev.log";

    /**
     * Offer the engine logs for export.
     *
     * Both sessions are offered, not just the latest: the engine rotates its log
     * on every launch, so after a crash the run that actually failed is already
     * the *previous* one by the time the launcher is back on screen. Exporting
     * only the current log would hand over a fresh, empty-ish file and lose the
     * evidence.
     */
    private void exportEngineLog() {
        File root = LauncherConfig.storageRoot(this);
        if (root == null) { toast("External storage unavailable."); return; }

        File cur  = new File(root, LOG_CURRENT);
        File prev = new File(root, LOG_PREVIOUS);

        final List<File> logs = new ArrayList<>();
        List<String> labels = new ArrayList<>();
        if (cur.isFile() && cur.length() > 0) {
            logs.add(cur);
            labels.add("Latest session  (" + GameDataImporter.human(cur.length()) + ")");
        }
        if (prev.isFile() && prev.length() > 0) {
            logs.add(prev);
            labels.add("Previous session  (" + GameDataImporter.human(prev.length()) + ")"
                     + "\nusually the one that crashed");
        }
        if (logs.isEmpty()) {
            new AlertDialog.Builder(this)
                .setTitle("No engine log yet")
                .setMessage("The engine writes its log when the game starts, so run the game "
                          + "once and come back.\n\nExpected at:\n"
                          + new File(root, LOG_CURRENT).getAbsolutePath())
                .setPositiveButton("OK", null)
                .show();
            return;
        }

        new AlertDialog.Builder(this)
            .setTitle("Export engine log")
            .setItems(labels.toArray(new String[0]),
                new android.content.DialogInterface.OnClickListener() {
                    @Override public void onClick(android.content.DialogInterface d, int which) {
                        askWhereToSaveLog(logs.get(which));
                    }
                })
            .setNegativeButton("Cancel", null)
            .show();
    }

    private void askWhereToSaveLog(File log) {
        pendingLogFile = log;
        boolean prev = LOG_PREVIOUS.equals(log.getName());
        // Ends in .txt deliberately. The picker appends an extension matching the
        // MIME type, so a name ending in .log comes back out as ".log.txt"; the
        // log is plain text anyway, and .txt is what opens on a phone.
        String name = "generalsx-engine-" + (prev ? "prev" : "latest") + "-"
                + LauncherConfig.engine(this) + ".txt";
        Intent i = new Intent(Intent.ACTION_CREATE_DOCUMENT);
        i.addCategory(Intent.CATEGORY_OPENABLE);
        i.setType("text/plain");
        i.putExtra(Intent.EXTRA_TITLE, name);
        try {
            startActivityForResult(i, REQ_EXPORT_LOG);
        } catch (Exception e) {
            pendingLogFile = null;
            toast("No file picker available to save the log.");
        }
    }

    /**
     * Copy the chosen log to the user's destination.
     *
     * Off the UI thread and streamed: the engine caps the log at 8 MB, which is
     * more than enough to stall the main thread on slow storage.
     */
    private void writeLog(final Uri dest) {
        final File src = pendingLogFile;
        pendingLogFile = null;
        if (src == null) { toast("Nothing to export."); return; }

        setBusy(true);
        statusText.setText("Exporting " + src.getName() + "…");
        new Thread(new Runnable() {
            @Override public void run() {
                String error = null;
                try (java.io.InputStream in = new java.io.FileInputStream(src);
                     java.io.OutputStream out = getContentResolver().openOutputStream(dest)) {
                    if (out == null) {
                        error = "could not open the chosen file";
                    } else {
                        byte[] buf = new byte[64 * 1024];
                        int n;
                        while ((n = in.read(buf)) > 0) out.write(buf, 0, n);
                        out.flush();
                    }
                } catch (Exception e) {
                    error = String.valueOf(e.getMessage());
                }
                final String err = error;
                runOnUiThread(new Runnable() {
                    @Override public void run() {
                        setBusy(false);
                        updateSummary();
                        toast(err == null
                                ? "Engine log saved (" + GameDataImporter.human(src.length()) + ")."
                                : "Export failed: " + err);
                    }
                });
            }
        }).start();
    }

    private void toast(String s) {
        Toast.makeText(this, s, Toast.LENGTH_LONG).show();
    }
}
