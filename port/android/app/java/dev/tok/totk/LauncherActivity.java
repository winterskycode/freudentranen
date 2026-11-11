package dev.tok.totk;

import android.app.Activity;
import android.app.ActivityManager;
import android.app.ActivityOptions;
import android.content.ActivityNotFoundException;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.RectF;
import android.graphics.Typeface;
import android.database.Cursor;
import android.net.Uri;
import android.os.Bundle;
import android.os.Environment;
import android.provider.DocumentsContract;
import android.provider.OpenableColumns;
import android.provider.Settings;
import android.text.Layout;
import android.text.StaticLayout;
import android.text.TextPaint;
import android.util.TypedValue;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;
import java.io.BufferedReader;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.FileReader;
import java.io.InputStream;
import java.io.OutputStream;
import java.io.IOException;
import java.io.RandomAccessFile;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;
import java.util.Locale;

/** Startup screen: the game's last frame as the backdrop with the menu on its left, the settings of assets/settings.tsv
    (written to files/launch.env for the native side; the in-game menu shows the same table), the field controls
    (files/actions.txt, ActionLayout) and a Tools page with the last run's log, the Saves page (Saves.java) and the Game
    files page (Setup.java), which a new player sees first, and the Mods page (Mods.java). Controller: the d-pad / stick
    moves, left / right change a value, A confirms, B goes back, X restores a default (or removes / deletes, pressed
    twice), Y moves a mod, L / R switch tabs. */
public class LauncherActivity extends Activity {
    private static final String DATA = Mods.DATA;
    private static final int PICK_MOD = 41;
    private static final int PICK_DRIVER = 42;
    private static final int PICK_LSFG = 43;
    private static final int PICK_SAVE_ZIP = 44, PICK_SAVE_FOLDER = 45, MAKE_SAVE_ZIP = 46;
    private static final int PICK_KEYS = 47, PICK_GAME_FOLDER = 48, PICK_GAME_FILES = 49;

    private SharedPreferences prefs;
    private SettingsTable table;
    private ActionLayout layout;
    private FrameLayout page;
    private String current = "home", controlsBack = "home";
    private int tab;
    private SettingsTable.Setting detail;
    private boolean ready;
    // the page's default row: confirm acts on it when nothing has focus (touch mode)
    private View preferred;
    private final List<Row> padRows = new ArrayList<>();
    private int padRow;
    private View detailView, padView, controlsDeco;
    private String crashNotice;
    private List<Mods.Mod> mods = new ArrayList<>();
    // the mod the right panel shows; X pressed once on it
    private Mods.Mod modDetail, modArmed;
    // the mod to focus when the page is rebuilt; a last-start problem no mod can be named for
    private String modFocus, modGeneral;
    private boolean modMoving;
    // launch() for the "Prepare shaders" row
    private boolean preparing;
    // this run made, or failed to make, the icon mod (IconMod)
    private boolean iconsTried; private String iconsProblem;
    // installing / merging progress; the Continue row while mods are prepared; why they could not be
    private volatile String modStatus = "", launchStatus, launchError;
    // mods found in the inbox / Downloads to install
    private List<File> modCandidates = new ArrayList<>();
    private File modCandidate;
    private volatile boolean modBusy;

    @Override protected void onCreate(Bundle state) {
        super.onCreate(state);
        Ui.init(this);
        // the settings store from before the shared table (keyed by label): not carried over
        deleteSharedPreferences("launch");
        prefs = getSharedPreferences("settings", MODE_PRIVATE);
        table = new SettingsTable(this);
        saves = new Saves(getFilesDir(), this::gameRunning);
        if (Game.SETUP) setup = new SetupFiles(this);
        Mods.context(this); ReportStore.context(this);
        page = new FrameLayout(this);
        page.setBackgroundColor(Ui.INK);
        setContentView(page);
        immersive();
    }

    /** Full screen: no status or navigation bar (they come back with a swipe). */
    private void immersive() {
        WindowInsetsController c = getWindow().getDecorView().getWindowInsetsController();
        if (c == null) return;
        c.hide(WindowInsets.Type.systemBars());
        c.setSystemBarsBehavior(WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
    }

    @Override public void onWindowFocusChanged(boolean focus) { super.onWindowFocusChanged(focus); if (focus) immersive(); }

    @Override protected void onResume() {
        super.onResume();
        // before the game can start: finish a swap of the save that was cut off
        if (Game.SAVES && !Saves.busy() && saveStaged == null) saves.recover();
        // a picked file is gone (GameActivity came back), or there is nothing to play yet: the Game files page first
        if (Game.SETUP) {
            boolean gone = setup.state.takeReported();
            if (gone || (current.equals("home") && !setupOffered && !gamePresent())) { setupOffered = true; setupBack = "home"; setupFocus = -1; current = "setup"; }
        }
        importOverlaySettings();
        // the button layout from before actions: not converted
        new File(getFilesDir(), "controls.txt").delete();
        ActionLayout.load(this);
        // the in-game menu may have changed it
        layout = new ActionLayout(new File(getFilesDir(), "actions.txt"));
        lsfg = Lsfg.ready(getFilesDir());
        // left by a translation that did not end
        if (!lsfgBusy) lsfgCopy().delete();
        File frame = new File(getFilesDir(), "backdrop.ppm");
        new Thread(() -> {
            Backdrop.load(frame);
            File crash = ReportStore.crashReport(this, prefs);
            if (crash != null) crashNotice = crash.getName();
            // a driver from a zip did not start: say so where the player looks
            if (crash != null && driverChoice().startsWith(Drivers.ZIP) && ReportStore.driverFailed(this)) {
                String name = driverLabel(driverChoice());
                runOnUiThread(() -> { launchError = name + " failed to start. Try another driver."; invalidateAll(page); });
            }
            List<Mods.Mod> list = Mods.list(); String general = Mods.lastRun(new File(getFilesDir(), "profile/totk.log"), list);
            runOnUiThread(() -> { mods = list; modGeneral = general; if (page != null && (current.equals("mods") || current.equals("home"))) show(current); });
            runOnUiThread(() -> { if (page != null) { if (crash != null) show(current); else invalidateAll(page); } });
            boolean changed = false;
            for (File r : ReportStore.list()) if (ReportStore.pending(r)) { ReportStore.finish(r); changed = true; }
            if (changed) runOnUiThread(() -> { if (current.equals("tools")) show("tools"); });
        }).start();
        show(current);
    }

    private static void invalidateAll(View v) {
        v.invalidate();
        if (v instanceof ViewGroup) for (int i = 0; i < ((ViewGroup) v).getChildCount(); i++) invalidateAll(((ViewGroup) v).getChildAt(i));
    }

    /** The in-game menu writes the values changed there to files/overlay.env; take them into the settings, then remove it. */
    private void importOverlaySettings() {
        File f = new File(getFilesDir(), "overlay.env");
        if (!f.exists()) return;
        SharedPreferences.Editor e = prefs.edit();
        try (BufferedReader in = new BufferedReader(new FileReader(f))) {
            for (String line; (line = in.readLine()) != null; ) {
                int eq = line.indexOf('=');
                if (line.startsWith("#") || eq < 0) continue;
                SettingsTable.Setting s = table.find(line.substring(0, eq));
                String v = line.substring(eq + 1).trim();
                if (s != null && s.index(v) >= 0) e.putString(s.key, v);
            }
        } catch (IOException ignored) { return; }
        e.apply();
        f.delete();
    }

    // ---- the settings ----

    private boolean turnip() { return new File(getApplicationInfo().nativeLibraryDir, "libvulkan_freedreno.so").exists(); }

    private boolean shown(SettingsTable.Setting s) {
        if (s.has('D') && !turnip()) return false;
        if (s.needsKey == null) return true;
        String v = value(s.needsKey); for (String w : s.needsValues) if (w.equals(v)) return true;
        return false;
    }

    /** A value this device can use now: LSFG's need its shaders. A saved one that cannot be used counts as the default, on
        this screen and in launch.env, and comes back with the shaders. */
    private boolean usable(SettingsTable.Setting s, int i) { return lsfg || !s.values[i].startsWith("lsfg"); }
    private int index(SettingsTable.Setting s) { int i = s.index(prefs.getString(s.key, s.values[s.def])); return i < 0 || !usable(s, i) ? s.def : i; }
    private String value(String key) { SettingsTable.Setting s = table.find(key); return s == null ? "" : s.values[index(s)]; }
    private String name(String key) { SettingsTable.Setting s = table.find(key); return s == null ? "" : s.names[index(s)]; }
    private void set(SettingsTable.Setting s, int i) { int n = s.values.length; prefs.edit().putString(s.key, s.values[((i % n) + n) % n]).apply(); }

    // ---- pages ----

    private void show(String name) {
        current = name;
        page.removeAllViews();
        padRows.clear(); detailView = padView = controlsDeco = null;
        switch (name) {
            case "settings": settingsPage(); break;
            case "controls": controlsPage(); break;
            case "tools": toolsPage(); break;
            case "saves": savesPage(); break;
            case "saveask": saveAskPage(); break;
            case "setup": setupPage(); break;
            case "mods": modsPage(); break;
            case "addmod": addModPage(); break;
            case "drivers": driversPage(); break;
            case "licences": licencesPage(); break;
            default: homePage(); break;
        }
        linkLoops(page);
    }

    /** Every list of rows wraps: up from its first row, down from its last. */
    private void linkLoops(ViewGroup g) {
        Row first = null, last = null;
        for (int i = 0; i < g.getChildCount(); i++) {
            View v = g.getChildAt(i);
            if (v instanceof ViewGroup) linkLoops((ViewGroup) v);
            else if (v instanceof Row && v.getVisibility() == View.VISIBLE) { if (first == null) first = (Row) v; last = (Row) v; }
        }
        if (first != null && first != last) { first.loopUp = last; last.loopDown = first; }
    }

    /** Design units of this screen (the canvas is 1280 x 720; a wider screen gets more width). */
    private float W() { return page.getWidth() > 0 ? page.getWidth() / Ui.U : getResources().getDisplayMetrics().widthPixels / Ui.U; }
    private float H() { return page.getHeight() > 0 ? page.getHeight() / Ui.U : getResources().getDisplayMetrics().heightPixels / Ui.U; }

    private void place(View v, float x, float y, float w, float h) {
        FrameLayout.LayoutParams p = new FrameLayout.LayoutParams(w < 0 ? ViewGroup.LayoutParams.MATCH_PARENT : Ui.ui(w), h < 0 ? ViewGroup.LayoutParams.MATCH_PARENT : Ui.ui(h));
        p.leftMargin = Ui.ui(x); p.topMargin = Ui.ui(y);
        page.addView(v, p);
    }

    private static final class Drawn extends View {
        interface Painter { void paint(Canvas c, float w, float h); }
        private final Painter painter;
        Drawn(Context c, Painter p) { super(c); painter = p; }
        @Override protected void onDraw(Canvas c) { painter.paint(c, getWidth(), getHeight()); }
    }

    private Row addRow(LinearLayout list, float h, Row.Painter painter, View.OnClickListener click) {
        Row r = new Row(this, painter);
        if (click != null) r.setOnClickListener(click);
        LinearLayout.LayoutParams p = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, Ui.ui(h));
        p.bottomMargin = Ui.ui(2);
        list.addView(r, p);
        return r;
    }

    private void prefer(Row r) { preferred = r; r.preferred = true; r.requestFocus(); }

    private LinearLayout column() { LinearLayout l = new LinearLayout(this); l.setOrientation(LinearLayout.VERTICAL); return l; }

    // ---- the startup screen ----

    /** The data made on a PC and pushed to /sdcard/tok (tools/push_data.sh): what the game plays from with nothing picked. */
    private boolean preparedData() {
        return new File(DATA + "/romfs/romfs.bin").length() == Game.ROMFS_SIZE && new File(DATA + "/romfs/runtime-index.bin").exists() && new File(DATA + "/rewritten/main.patched.bin").exists();
    }

    private boolean picked() { return Game.SETUP && setup.state.complete(); }
    private boolean gamePresent() { return picked() || preparedData(); }

    private void homePage() {
        // only prepared data under /sdcard/tok needs it
        boolean allFiles = Environment.isExternalStorageManager(), access = allFiles || Game.SETUP;
        File romfs = new File(DATA + "/romfs/romfs.bin");
        boolean picked = picked(), files = gamePresent();
        int shaders = count(shaderCache(driverChoice()), ".spv");
        ready = access && files;
        String problem = !access ? "Storage access is needed. Press A to grant it." : files ? null
            : Game.SETUP && !romfs.exists() ? "The game files are not set up. Press A to set them up."
            : !romfs.exists() ? "The game files (RomFS) are missing from " + DATA + "." : romfs.length() != Game.ROMFS_SIZE ? "The game files (RomFS) are not version " + Game.VERSION + "." : !files ? "Part of the prepared game data is missing: the game code or the file index." : null;
        String summary = driverLabel(driverChoice()) + " · " + value("TOTK_FPS_CAP") + " fps · " + name("TOTK_UPSCALER")
            + (value("TOTK_AA").equals("off") ? "" : " · " + name("TOTK_AA")) + (value("TOTK_FG").equals("off") ? "" : " · " + name("TOTK_FG"));

        page.addView(new Backdrop(this, true), new FrameLayout.LayoutParams(-1, -1));
        // written by the game's prepare mode (vk/nvn_vulkan_prepare.inc)
        boolean prepared = new File(getFilesDir(), "prepared.txt").exists();
        String shaderText = prepared ? String.format("%,d shaders prepared", shaders) : shaders > 0 ? String.format("%,d shaders ready", shaders) : "Shaders are built on first start";
        place(new Drawn(this, (c, w, h) -> {
            Paint ring = Ui.stroke(Ui.JADE, 1.5f);
            float gx = Ui.u(72 + 17), gy = Ui.u(64 + 17);
            c.drawCircle(gx, gy, Ui.u(15), ring); c.drawCircle(gx, gy, Ui.u(9), ring); c.drawCircle(gx, gy, Ui.u(2.5f), Ui.fill(Ui.JADE));
            c.drawLine(gx, gy - Ui.u(15), gx, gy - Ui.u(10), ring); c.drawLine(gx, gy + Ui.u(10), gx, gy + Ui.u(15), ring);
            c.drawLine(gx - Ui.u(15), gy, gx - Ui.u(10), gy, ring); c.drawLine(gx + Ui.u(10), gy, gx + Ui.u(15), gy, ring);
            Paint cap = Ui.text(Ui.body, 13, Ui.MUTED); cap.setLetterSpacing(0.22f);
            Ui.centred(c, "NATIVE ANDROID PORT · " + Game.VERSION, Ui.u(72 + 46), gy, cap);
            Paint title = Ui.text(Ui.title, 50, Ui.CREAM);
            c.drawText(Game.TITLE, Ui.u(72), Ui.u(64 + 34 + 14 + 44), title);
            Ui.ornament(c, Ui.u(72), Ui.u(64 + 34 + 14 + 62), Ui.u(360));
            float x = w - Ui.u(56);
            String[][] chips = Game.SETUP ? new String[][]{{shaderText, prepared ? "ok" : "wait"}, {files ? "Game files " + Game.VERSION : "Game files missing", files ? "ok" : "bad"}}
                : new String[][]{{shaderText, prepared ? "ok" : "wait"}, {access ? "Storage access" : "No storage access", access ? "ok" : "bad"}, {files ? "Game files " + Game.VERSION : "Game files missing", files ? "ok" : "bad"}};
            for (String[] chip : chips) {
                Paint t = Ui.text(Ui.body, 13, Ui.CREAM); float cw = t.measureText(chip[0]) + Ui.u(38);
                RectF r = new RectF(x - cw, Ui.u(56), x, Ui.u(84));
                c.drawRoundRect(r, Ui.u(14), Ui.u(14), Ui.fill(0xB80A1114)); c.drawRoundRect(r, Ui.u(14), Ui.u(14), Ui.stroke(0x29EEE8D8, 1));
                c.drawCircle(r.left + Ui.u(15), r.centerY(), Ui.u(3), Ui.fill(chip[1].equals("ok") ? Ui.JADE : chip[1].equals("bad") ? Ui.OCHRE : Ui.MUTED));
                Ui.centred(c, chip[0], r.left + Ui.u(26), r.centerY(), t);
                x = r.left - Ui.u(8);
            }
            float fy = h - Ui.u(28) - Ui.u(24), fx = Ui.hint(c, Ui.u(72), fy, "A", "Select"); Ui.hint(c, fx, fy, "B", "Quit");
            String note = Backdrop.frame != null ? "Background: the last frame of your previous session" : "The last frame of your game will appear here after you play";
            Paint n = Ui.text(Ui.body, 13, Ui.SOFT); n.setTextAlign(Paint.Align.RIGHT); Ui.centred(c, note, w - Ui.u(56), fy + Ui.u(12), n);
        }), 0, 0, -1, -1);

        LinearLayout nav = column();
        place(nav, 72, 268, 420, H() - 268 - 70);
        Row start = addRow(nav, 74, (c, r, on) -> {
            Paint label = Ui.text(Ui.title, 26, !ready ? Ui.MUTED : on ? Ui.GOLD_TEXT : Ui.CREAM);
            c.drawText("Continue", Ui.u(39), Ui.u(36), label);
            String note = problem != null ? problem : launchStatus != null ? launchStatus : launchError != null ? launchError : summary;
            Paint sub = Ui.text(Ui.body, 13, problem != null || (launchStatus == null && launchError != null) ? Ui.OCHRE_TEXT : Ui.MUTED);
            c.drawText(note, Ui.u(39), Ui.u(57), sub);
            if (on && ready) {
                float cx = r.getWidth() - Ui.u(18 + 13), cy = r.getHeight() / 2f;
                c.drawCircle(cx, cy, Ui.u(13), Ui.stroke(Ui.GOLD, 1));
                Paint a = Ui.text(Ui.semibold, 12, Ui.GOLD_TEXT); a.setTextAlign(Paint.Align.CENTER); Ui.centred(c, "A", cx, cy, a);
            }
        }, v -> {
            if (ready) launch();
            else if (Game.SETUP) { setupBack = "home"; setupFocus = -1; show("setup"); }
            else if (!Environment.isExternalStorageManager()) startActivity(new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION, Uri.parse("package:" + getPackageName())));
        });
        if (ready && !prepared) menuRow(nav, "Prepare shaders", "once, a few minutes", Ui.GOLD_TEXT, v -> { preparing = true; launch(); preparing = false; });
        int changes = layout.changes();
        menuRow(nav, "Settings", "graphics, frame rate, system", Ui.CREAM, v -> { tab = 0; show("settings"); });
        menuRow(nav, "Controls", changes > 0 ? changes + (changes == 1 ? " action" : " actions") + " moved" : "game defaults", Ui.CREAM, v -> { controlsBack = "home"; padRow = 0; show("controls"); });
        int on = Mods.enabled(mods).size(); boolean modProblem = modGeneral != null || mods.stream().anyMatch(m -> m.problem != null);
        if (Game.MODS) menuRow(nav, "Mods", mods.isEmpty() ? "none installed" : modProblem ? "a problem at the last start" : !modsOn() ? "turned off" : on + " of " + mods.size() + " on",
            modProblem ? Ui.OCHRE_TEXT : Ui.CREAM, v -> { modFocus = null; show("mods"); });
        int nreports = ReportStore.list().size();
        menuRow(nav, "Tools", crashNotice != null ? "crash report saved" : nreports > 0 ? nreports + (nreports == 1 ? " bug report" : " bug reports") + ", log" : "last run's log",
            crashNotice != null ? Ui.OCHRE_TEXT : Ui.CREAM, v -> { toolsLsfg = toolsSaves = toolsSetup = false; show("tools"); });
        menuRow(nav, "Quit", "", Ui.MUTED, v -> finish());
        prefer(start);
    }

    private void menuRow(LinearLayout nav, String label, String caption, int color, View.OnClickListener click) {
        addRow(nav, 50, (c, r, on) -> {
            Ui.centred(c, label, Ui.u(39), r.getHeight() / 2f, Ui.text(Ui.title, 21, on ? Ui.GOLD_TEXT : color));
            Paint cap = Ui.text(Ui.body, 13, Ui.MUTED); cap.setTextAlign(Paint.Align.RIGHT);
            Ui.centred(c, caption, r.getWidth() - Ui.u(18), r.getHeight() / 2f, cap);
        }, click);
    }

    // ---- settings ----

    private void header(Canvas c, float w, String title, String trail) {
        c.drawText(title, Ui.u(56), Ui.u(70), Ui.text(Ui.title, 30, Ui.CREAM));
        if (trail != null) {
            Paint t = Ui.text(Ui.body, 15, Ui.MUTED); c.drawText(trail, Ui.u(56) + Ui.text(Ui.title, 30, 0).measureText(title) + Ui.u(16), Ui.u(68), t);
        }
        c.drawLine(Ui.u(56), Ui.u(96), w - Ui.u(56), Ui.u(96), Ui.stroke(Ui.LINE, 1));
    }

    private float chip(Canvas c, float x, String s) {
        Paint t = Ui.text(Ui.semibold, 12, Ui.CREAM); float cw = t.measureText(s) + Ui.u(16);
        c.drawRoundRect(new RectF(x, Ui.u(48), x + cw, Ui.u(72)), Ui.u(6), Ui.u(6), Ui.stroke(0x59EEE8D8, 1));
        Ui.centred(c, s, x + Ui.u(8), Ui.u(60), t);
        return x + cw + Ui.u(18);
    }

    private void settingsPage() {
        page.addView(new Backdrop(this, false), new FrameLayout.LayoutParams(-1, -1));
        if (table.tabs.isEmpty()) { place(new Drawn(this, (c, w, h) -> header(c, w, "Settings", "This build has no settings table.")), 0, 0, -1, -1); return; }
        tab = Math.min(tab, table.tabs.size() - 1);
        float[] tabX = new float[table.tabs.size() + 1];
        View deco = new Drawn(this, (c, w, h) -> {
            header(c, w, "Settings", null);
            float x = chip(c, Ui.u(56) + Ui.text(Ui.title, 30, 0).measureText("Settings") + Ui.u(36), "L");
            for (int t = 0; t < table.tabs.size(); t++) {
                String n = table.tabs.get(t); Paint p = Ui.text(Ui.title, 18, t == tab ? Ui.GOLD_TEXT : Ui.MUTED);
                tabX[t] = x - Ui.u(8);
                c.drawText(n, x, Ui.u(64), p);
                if (t == tab) Ui.diamond(c, x + p.measureText(n) / 2, Ui.u(80), Ui.u(3.5f), Ui.fill(Ui.GOLD));
                x += p.measureText(n) + Ui.u(28);
            }
            tabX[table.tabs.size()] = x - Ui.u(8);
            chip(c, x - Ui.u(10), "R");
            float fy = h - Ui.u(24) - Ui.u(24), fx = Ui.hint(c, Ui.u(56), fy, "‹ ›", "Change");
            fx = Ui.hint(c, fx, fy, "X", "Default"); Ui.hint(c, fx, fy, "B", "Back");
            Paint n = Ui.text(Ui.body, 14, Ui.SOFT); n.setTextAlign(Paint.Align.RIGHT);
            Ui.centred(c, "Changes are saved as you make them and used when the game starts.", w - Ui.u(56), fy + Ui.u(12), n);
        });
        deco.setOnTouchListener((v, e) -> {
            if (e.getAction() != MotionEvent.ACTION_UP || e.getY() > Ui.u(96)) return e.getY() <= Ui.u(96);
            for (int t = 0; t < table.tabs.size(); t++) if (e.getX() >= tabX[t] && e.getX() < tabX[t + 1]) { tab = t; show("settings"); }
            return true;
        });
        place(deco, 0, 0, -1, -1);

        float listW = W() - 56 - 400 - 40 - 56;
        ScrollView scroll = new ScrollView(this); scroll.setVerticalScrollBarEnabled(false);
        LinearLayout list = column(); scroll.addView(list);
        place(scroll, 56, 120, listW, H() - 120 - 76);
        Row first = null, again = null;
        for (SettingsTable.Setting s : table.rows) {
            if (!s.tab.equals(table.tabs.get(tab)) || !shown(s)) continue;
            if (s.heading()) {
                LinearLayout.LayoutParams hp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, Ui.ui(first == null ? 30 : 44));
                list.addView(new Drawn(this, (c, w, h) -> Ui.centred(c, s.label, Ui.u(14), h - Ui.u(14), Ui.text(Ui.title, 15, Ui.GOLD_TEXT))), hp);
                continue;
            }
            Row r = addRow(list, 52, (c, row, on) -> {
                float mid = row.getHeight() / 2f;
                Ui.centred(c, s.label, Ui.u(36), mid, Ui.text(Ui.body, 18, on ? Ui.GOLD_TEXT : Ui.CREAM));
                float left;
                if (s.page()) {
                    int n = layout.changes(); String v = s.key.equals("@drivers") ? driverLabel(driverChoice()) : n > 0 ? n + " changed" : "Game defaults";
                    Paint chev = Ui.text(Ui.body, 17, Ui.MUTED); float cw = chev.measureText("›");
                    Ui.centred(c, "›", row.getWidth() - Ui.u(16) - cw, mid, chev);
                    Paint val = Ui.text(Ui.medium, 17, Ui.JADE); left = row.getWidth() - Ui.u(28) - cw - val.measureText(v);
                    Ui.centred(c, v, left, mid, val);
                } else left = Ui.value(c, row.getWidth() - Ui.u(16), mid, s.names[index(s)], Ui.JADE);
                if (s.has('X')) Ui.tag(c, left - Ui.u(18), mid, "EXPERIMENTAL", 0x6EEEE8D8, Ui.SOFT);
                else if ((!s.live() && !s.page()) || s.key.equals("@drivers")) Ui.tag(c, left - Ui.u(18), mid, "RESTART", Ui.OCHRE, Ui.OCHRE_TEXT);
            }, s.key.equals("@drivers") ? v -> { driverFocus = null; show("drivers"); }
               : s.page() ? v -> { controlsBack = "settings"; padRow = 0; show("controls"); } : v -> { step(s, 1); v.invalidate(); });
            r.setTag(s);
            if (!s.page()) r.stepper = by -> step(s, by);
            r.onFocus = row -> { detail = s; detailView.invalidate(); };
            // focusing the first row scrolls up to show its heading too
            if (first == null) { first = r; r.onFocus = row -> { detail = s; detailView.invalidate(); scroll.post(() -> scroll.scrollTo(0, 0)); }; }
            if (s.key.equals(settingAgain)) again = r;
        }
        detail = first != null ? (SettingsTable.Setting) first.getTag() : null;
        detailView = new Drawn(this, (c, w, h) -> {
            Ui.panel(c, Ui.u(1), Ui.u(1), w - Ui.u(1), h - Ui.u(1));
            SettingsTable.Setting s = detail; if (s == null) return;
            float x = Ui.u(28), pw = w - Ui.u(56), y = Ui.u(28);
            y = wrap(c, s.label, Ui.title, 24, Ui.GOLD_TEXT, x, y, pw) + Ui.u(14);
            y = wrap(c, s.help, Ui.body, 15, Ui.HELP, x, y, pw) + Ui.u(16);
            int cur = s.page() ? -1 : index(s), choices = 0;
            for (int i = 0; i < s.names.length; i++) if (usable(s, i)) choices++;
            // the choices, closer together when many would run past the footer line (Render resolution has nine)
            float step = choices == 0 ? Ui.u(34) : Math.max(Ui.u(24), Math.min(Ui.u(34), (h - Ui.u(64) - y) / choices));
            for (int i = 0; i < s.names.length; i++) {
                if (!usable(s, i)) continue;
                boolean on = i == cur; float cy = y + step / 2;
                Ui.diamond(c, x + Ui.u(15), cy, Ui.u(3.5f), on ? Ui.fill(Ui.JADE) : Ui.stroke(Ui.SOFT, 1));
                Paint t = Ui.text(Ui.body, 15, on ? Ui.JADE : Ui.SOFT); Ui.centred(c, s.names[i], x + Ui.u(30), cy, t);
                if (i == s.def && !on) Ui.centred(c, "default", x + Ui.u(40) + t.measureText(s.names[i]), cy, Ui.text(Ui.body, 13, Ui.MUTED));
                y += step;
            }
            c.drawLine(x, h - Ui.u(56), w - Ui.u(28), h - Ui.u(56), Ui.stroke(Ui.LINE, 1));
            boolean restart = (!s.live() && !s.page()) || s.key.equals("@drivers");
            Ui.centred(c, s.page() ? "Takes effect immediately, in game too." : restart ? "Saved. Takes effect the next time the game starts." : "Can also be changed in game.",
                x, h - Ui.u(32), Ui.text(Ui.body, 14, restart ? Ui.OCHRE_TEXT : Ui.MUTED));
        });
        place(detailView, W() - 56 - 400, 120, 400, H() - 120 - 76);
        if (first != null) prefer(first);
        if (again != null) { again.requestFocus(); detail = (SettingsTable.Setting) again.getTag(); }
        settingAgain = null;
    }

    // the row to focus when the list is rebuilt
    private String settingAgain;
    /** A step of a setting's value; other rows may show or hide with it. */
    private void step(SettingsTable.Setting s, int by) {
        int n = s.values.length, i = index(s);
        do i = (i + by + n) % n; while (!usable(s, i));
        set(s, i);
        if (table.governs(s)) { settingAgain = s.key; show("settings"); } else if (detailView != null) detailView.invalidate();
    }

    /** Wrapped text from (x, y), width w; returns the y below it. */
    private float wrap(Canvas c, String s, Typeface face, float size, int color, float x, float y, float w) {
        TextPaint p = new TextPaint(Ui.text(face, size, color));
        StaticLayout l = StaticLayout.Builder.obtain(s, 0, s.length(), p, Math.max(1, (int) w)).setAlignment(Layout.Alignment.ALIGN_NORMAL).setLineSpacing(0, 1.12f).build();
        c.save(); c.translate(x, y); l.draw(c); c.restore();
        return y + l.getHeight();
    }

    // ---- the field controls (actions) ----

    private boolean byLabel() { return value("TOTK_FACE_BUTTONS").equals("label"); }

    /** The drawing of a handheld (design units in its 470 x 300 panel): x, y, w, h, corner radius; label */
    private static final float[][] PAD = {
        {362, 150, 28, 28, 14}, {392, 120, 28, 28, 14}, {332, 120, 28, 28, 14}, {362, 90, 28, 28, 14},
        {66, 96, 52, 52, 26}, {300, 190, 46, 46, 23}, {44, 34, 76, 20, 10}, {350, 34, 76, 20, 10},
        {44, 10, 76, 20, 10}, {350, 10, 76, 20, 10}, {246, 98, 30, 22, 6}, {194, 98, 30, 22, 6},
        {112, 196, 22, 22, 4}, {134, 174, 22, 22, 4}, {156, 196, 22, 22, 4}, {134, 218, 22, 22, 4}, {0, 0, 0, 0, 0}};
    private static final String[] PAD_TEXT = {"", "", "", "", "L3", "R3", "L1", "R1", "L2", "R2", "+", "−", "", "", "", "", ""};
    // where each face row's button sits on the drawing, by label
    private static final int[] LABEL_POSITION = {1, 0, 3, 2};

    private int padAt(int i) { return byLabel() && i < 4 ? LABEL_POSITION[i] : i; }

    private void controlsPage() {
        page.addView(new Backdrop(this, false), new FrameLayout.LayoutParams(-1, -1));
        float listX = 566, listW = W() - 56 - listX;
        controlsDeco = new Drawn(this, (c, w, h) -> {
            header(c, w, "Controls", controlsBack.equals("settings") ? "Settings › Controls" : null);
            int n = layout.changes(); Paint st = Ui.text(Ui.body, 14, Ui.JADE); st.setTextAlign(Paint.Align.RIGHT);
            c.drawText(n > 0 ? n + " changed from the game's defaults" : "Using the game's default controls", w - Ui.u(56), Ui.u(68), st);
            Paint col = Ui.text(Ui.semibold, 12, Ui.MUTED); col.setLetterSpacing(0.14f); float cw = (Ui.u(listW) - Ui.u(52)) / 2;
            String[] cols = {"ACTION", "BUTTON"};
            for (int k = 0; k < 2; k++) c.drawText(cols[k], Ui.u(listX + 36) + cw * k, Ui.u(132), col);
            float l = Ui.u(56), t = Ui.u(444), r = Ui.u(526), b = h - Ui.u(76);
            c.drawRect(l, t, r, b, Ui.fill(Ui.PANEL)); c.drawRect(l, t, r, b, Ui.stroke(Ui.LINE, 1));
            int a = padRow;
            String head = ActionLayout.LABELS[a] + " → " + ActionLayout.buttonName(layout.button[a], byLabel());
            float y = wrap(c, head, Ui.title, 21, Ui.GOLD_TEXT, l + Ui.u(22), t + Ui.u(18), r - l - Ui.u(44)) + Ui.u(10);
            wrap(c, ActionLayout.HELP[a] + " This does not affect menus, which confirm with " + ActionLayout.buttonName(1, byLabel())
                + " and go back with " + ActionLayout.buttonName(0, byLabel()) + ".", Ui.body, 15, Ui.HELP, l + Ui.u(22), y, r - l - Ui.u(44));
            float fy = h - Ui.u(24) - Ui.u(24), fx = Ui.hint(c, Ui.u(56), fy, "‹ ›", "Button");
            fx = Ui.hint(c, fx, fy, "X", "Default"); Ui.hint(c, fx, fy, "B", "Back");
            Paint note = Ui.text(Ui.body, 14, Ui.SOFT); note.setTextAlign(Paint.Align.RIGHT);
            Ui.centred(c, "You can also press L1, R1, L2, R2 or a stick button to assign the action to it.", w - Ui.u(56), fy + Ui.u(12), note);
        });
        place(controlsDeco, 0, 0, -1, -1);

        padView = new Drawn(this, (c, w, h) -> {
            Ui.panel(c, Ui.u(1), Ui.u(1), w - Ui.u(1), h - Ui.u(1));
            c.drawRoundRect(new RectF(Ui.u(30), Ui.u(62), w - Ui.u(30), h - Ui.u(34)), Ui.u(40), Ui.u(40), Ui.stroke(0x48EEE8D8, 1.5f));
            int sel = layout.button[padRow];
            for (int i = 0; i < ActionLayout.PHYSICAL.length; i++) {
                float[] b = PAD[padAt(i)]; if (b[2] <= 0) continue;
                boolean on = i == sel; RectF box = new RectF(Ui.u(b[0]), Ui.u(b[1]), Ui.u(b[0] + b[2]), Ui.u(b[1] + b[3]));
                if (on) c.drawRoundRect(box, Ui.u(b[4]), Ui.u(b[4]), Ui.fill(0x5AD8B45C));
                c.drawRoundRect(box, Ui.u(b[4]), Ui.u(b[4]), Ui.stroke(on ? Ui.GOLD : 0x73EEE8D8, 1.5f));
                String s = i < 4 && byLabel() ? ActionLayout.buttonName(i, true) : PAD_TEXT[padAt(i)];
                if (!s.isEmpty()) { Paint t = Ui.text(Ui.semibold, 11, on ? Ui.GOLD_TEXT : Ui.SOFT); t.setTextAlign(Paint.Align.CENTER); Ui.centred(c, s, box.centerX(), box.centerY(), t); }
            }
        });
        place(padView, 56, 124, 470, 300);

        ScrollView scroll = new ScrollView(this); scroll.setVerticalScrollBarEnabled(false);
        LinearLayout list = column(); scroll.addView(list);
        place(scroll, listX, 140, listW, H() - 140 - 76);
        for (int k = 0; k < ActionLayout.ACTIONS; k++) {
            final int a = k;
            Row r = addRow(list, 30, (c, row, on) -> {
                float mid = row.getHeight() / 2f, cw = (row.getWidth() - Ui.u(52)) / 2;
                Ui.centred(c, ActionLayout.LABELS[a], Ui.u(36), mid, Ui.text(Ui.body, 15, on ? Ui.GOLD_TEXT : Ui.CREAM));
                String v = "‹ " + ActionLayout.buttonName(layout.button[a], byLabel()) + " ›";
                Paint jade = Ui.text(Ui.body, 15, Ui.JADE); Ui.centred(c, v, Ui.u(36) + cw, mid, jade);
                if (layout.changed(a)) c.drawCircle(Ui.u(36) + cw + jade.measureText(v) + Ui.u(10), mid, Ui.u(3), Ui.fill(Ui.JADE));
            }, v -> { layout.step(a, 1); layout.write(); refreshControls(); });
            r.stepper = by -> { layout.step(a, by); layout.write(); refreshControls(); };
            r.onFocus = row -> { padRow = a; refreshControls(); };
            padRows.add(r);
        }
        Row restore = addRow(list, 36, (c, row, on) -> Ui.centred(c, "Restore the game's default controls", Ui.u(36), row.getHeight() / 2f, Ui.text(Ui.title, 18, on ? Ui.GOLD_TEXT : Ui.SOFT)),
            v -> { layout.reset(); layout.write(); refreshControls(); });
        ((LinearLayout.LayoutParams) restore.getLayoutParams()).topMargin = Ui.ui(8);
        padRow = Math.min(padRow, ActionLayout.ACTIONS - 1);
        prefer(padRows.get(padRow));
    }

    private void refreshControls() {
        if (controlsDeco != null) controlsDeco.invalidate();
        if (padView != null) padView.invalidate();
        for (Row r : padRows) r.invalidate();
    }

    // ---- Tools: the log and the bug reports ----

    // the report the right panel shows (null: the log)
    private File toolsReport;
    // X pressed once on it: a second X deletes it
    private File reportArmed;
    private String reportStatus = "";
    // the right panel shows the Lossless Scaling row
    private boolean toolsLsfg;
    // back from the Saves page: its row keeps the focus
    private boolean toolsSaves;
    // back from the Game files page: its row keeps the focus
    private boolean toolsSetup;

    private void toolsPage() {
        page.addView(new Backdrop(this, false), new FrameLayout.LayoutParams(-1, -1));
        List<File> reports = ReportStore.list();
        float px = 56 + 420 + 40, pw = W() - 56 - px;
        place(new Drawn(this, (c, w, h) -> {
            header(c, w, "Tools", null);
            float fy = h - Ui.u(24) - Ui.u(24), fx = Ui.hint(c, Ui.u(56), fy, "A", toolsReport != null ? "Share" : "Select");
            if (toolsReport != null) fx = Ui.hint(c, fx, fy, "X", reportArmed == toolsReport ? "Press again to delete" : "Delete");
            Ui.hint(c, fx, fy, "B", "Back");
            Paint n = Ui.text(Ui.body, 14, Ui.JADE); n.setTextAlign(Paint.Align.RIGHT); Ui.centred(c, reportStatus, w - Ui.u(56), fy + Ui.u(12), n);
        }), 0, 0, -1, -1);

        TextView log = new TextView(this);
        log.setText(tail(new File(getFilesDir(), "profile/totk.log"), 8000));
        log.setTypeface(Typeface.MONOSPACE); log.setTextColor(Ui.SOFT); log.setTextSize(TypedValue.COMPLEX_UNIT_PX, Ui.u(11));
        ScrollView logScroll = new ScrollView(this); logScroll.addView(log); logScroll.setFocusable(false);
        View detail = new Drawn(this, (c, w, h) -> {
            Ui.panel(c, Ui.u(1), Ui.u(1), w - Ui.u(1), h - Ui.u(1));
            float x = Ui.u(24), iw = w - Ui.u(48), y = Ui.u(24);
            if (toolsLsfg) { lsfgPanel(c, x, y, iw); return; }
            File r = toolsReport; if (r == null) return;
            File png = new File(r, "frame.png");
            android.graphics.Bitmap b = png.exists() ? android.graphics.BitmapFactory.decodeFile(png.getPath()) : null;
            if (b != null) { float ih = iw * b.getHeight() / b.getWidth(); c.drawBitmap(b, null, new RectF(x, y, x + iw, y + ih), new Paint(Paint.FILTER_BITMAP_FLAG)); y += ih + Ui.u(16); }
            String video = new File(r, "replay.mp4").exists() ? "includes a video of the last 30 seconds" : ReportStore.pending(r) ? "the video is still being prepared" : "no video, because recording was off";
            y = wrap(c, r.getName() + " \u00b7 " + String.format(Locale.ROOT, "%.1f MB", ReportStore.size(r) / 1048576.0) + " \u00b7 " + video, Ui.body, 15, Ui.GOLD_TEXT, x, y, iw) + Ui.u(10);
            String device = ReportStore.head(new File(r, "device.txt"), 700);
            wrap(c, device, Ui.body, 13, Ui.HELP, x, y, iw);
        });
        place(detail, px, 120, pw, H() - 120 - 76);
        place(logScroll, px + 20, 120 + 16, pw - 40, H() - 120 - 76 - 32);
        logScroll.post(() -> logScroll.fullScroll(View.FOCUS_DOWN));

        ScrollView navScroll = new ScrollView(this); navScroll.setVerticalScrollBarEnabled(false);
        LinearLayout nav = column(); navScroll.addView(nav);
        place(navScroll, 56, 120, 420, H() - 120 - 76);
        Row access = addRow(nav, 50, (c, r, on) -> Ui.centred(c, "Storage access", Ui.u(39), r.getHeight() / 2f, Ui.text(Ui.title, 21, on ? Ui.GOLD_TEXT : Ui.CREAM)),
            v -> startActivity(new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION, Uri.parse("package:" + getPackageName()))));
        // the player's files come through the picker
        if (Game.SETUP) access.setVisibility(View.GONE);
        Row logRow = addRow(nav, 50, (c, r, on) -> Ui.centred(c, "Last run's log", Ui.u(39), r.getHeight() / 2f, Ui.text(Ui.title, 21, on ? Ui.GOLD_TEXT : Ui.CREAM)), null);
        String gameFiles = !Game.SETUP ? "" : picked() ? "Your keys and game files" : preparedData() ? "Using the prepared data. You can pick your own files" : "Not set up";
        Row gameRow = !Game.SETUP ? null : addRow(nav, 64, (c, r, on) -> {
            c.drawText("Game files", Ui.u(39), Ui.u(29), Ui.text(Ui.title, 21, on ? Ui.GOLD_TEXT : Ui.CREAM));
            c.drawText(gameFiles, Ui.u(39), Ui.u(49), Ui.text(Ui.body, 13, Ui.MUTED));
        }, v -> { setupBack = "tools"; setupFocus = -1; show("setup"); });
        Row savesRow = !Game.SAVES ? null : addRow(nav, 64, (c, r, on) -> {
            c.drawText("Saves", Ui.u(39), Ui.u(29), Ui.text(Ui.title, 21, on ? Ui.GOLD_TEXT : Ui.CREAM));
            c.drawText(SAVE_WARNING, Ui.u(39), Ui.u(49), Ui.text(Ui.body, 13, Ui.MUTED));
        }, v -> { saveNote = saveError = null; show("saves"); });
        Row lossless = addRow(nav, 64, (c, r, on) -> {
            c.drawText("Lossless Scaling", Ui.u(39), Ui.u(29), Ui.text(Ui.title, 21, on ? Ui.GOLD_TEXT : Ui.CREAM));
            c.drawText(lsfgState(), Ui.u(39), Ui.u(49), Ui.text(Ui.body, 13, lsfgError != null ? Ui.OCHRE_TEXT : Ui.MUTED));
        }, v -> pickLsfg());
        Row licences = addRow(nav, 50, (c, r, on) -> Ui.centred(c, "Licences", Ui.u(39), r.getHeight() / 2f, Ui.text(Ui.title, 21, on ? Ui.GOLD_TEXT : Ui.CREAM)), v -> show("licences"));
        Row.Focus showLog = row -> { toolsReport = null; toolsLsfg = false; logScroll.setVisibility(View.VISIBLE); detail.invalidate(); };
        access.onFocus = showLog; logRow.onFocus = showLog; licences.onFocus = showLog;
        if (savesRow != null) savesRow.onFocus = showLog;
        if (gameRow != null) gameRow.onFocus = showLog;
        lossless.onFocus = row -> { toolsReport = null; toolsLsfg = true; logScroll.setVisibility(View.GONE); detail.invalidate(); };
        // touch mode gives rows no focus: a tap brings the log back
        logRow.setOnClickListener(v -> showLog.focused(logRow));
        View label = new Drawn(this, (c, w, h) -> c.drawText(reports.isEmpty() ? "NO BUG REPORTS YET" : "BUG REPORTS", Ui.u(39), h - Ui.u(10), spaced(Ui.text(Ui.semibold, 12, Ui.MUTED))));
        nav.addView(label, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, Ui.ui(40)));
        Row firstReport = null;
        for (File r : reports) {
            boolean crash = r.getName().endsWith("-crash");
            Row row = addRow(nav, 46, (c, rr, on) -> {
                String n = r.getName(); String when = n.length() >= 15 ? n.substring(6, 8) + "/" + n.substring(4, 6) + " " + n.substring(9, 11) + ":" + n.substring(11, 13) : n;
                Ui.centred(c, when, Ui.u(39), rr.getHeight() / 2f, Ui.text(Ui.body, 17, on ? Ui.GOLD_TEXT : Ui.CREAM));
                if (crash) Ui.tag(c, rr.getWidth() - Ui.u(16), rr.getHeight() / 2f, "CRASH", Ui.OCHRE, Ui.OCHRE_TEXT);
                else if (reportArmed == r) Ui.tag(c, rr.getWidth() - Ui.u(16), rr.getHeight() / 2f, "PRESS X AGAIN", Ui.OCHRE, Ui.OCHRE_TEXT);
            }, v -> shareReport(r));
            row.setTag(r);
            row.onFocus = rr -> { toolsReport = r; toolsLsfg = false; reportArmed = null; logScroll.setVisibility(View.GONE); detail.invalidate(); invalidateAll(page); };
            if (firstReport == null) firstReport = row;
        }
        if (toolsSetup && gameRow != null) { toolsSetup = false; prefer(gameRow); }
        else if (toolsSaves && savesRow != null) { toolsSaves = false; prefer(savesRow); }
        // back from the file picker, which touch mode reaches without focus
        else if (toolsLsfg) { logScroll.setVisibility(View.GONE); prefer(lossless); }
        else prefer(firstReport != null ? firstReport : Game.SETUP ? logRow : access);
    }

    private static Paint spaced(Paint p) { p.setLetterSpacing(0.14f); return p; }

    /** The report zipped (after its video and frame are converted) and handed to the share sheet. */
    private void shareReport(File r) {
        reportStatus = "Preparing " + r.getName() + "\u2026"; invalidateAll(page);
        new Thread(() -> {
            try {
                ReportStore.finish(r);
                File zip = ReportStore.zip(r);
                runOnUiThread(() -> {
                    reportStatus = String.format(Locale.ROOT, "%s.zip, %.1f MB", r.getName(), zip.length() / 1048576.0); invalidateAll(page);
                    Intent send = new Intent(Intent.ACTION_SEND).setType("application/zip").putExtra(Intent.EXTRA_STREAM, ReportProvider.uri(zip))
                        .putExtra(Intent.EXTRA_SUBJECT, "TotK bug report " + r.getName()).addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
                    send.setClipData(android.content.ClipData.newRawUri(r.getName(), ReportProvider.uri(zip)));
                    startActivity(Intent.createChooser(send, "Share bug report"));
                });
            } catch (IOException e) { runOnUiThread(() -> { reportStatus = "Could not package " + r.getName() + " for sharing."; invalidateAll(page); }); }
        }).start();
    }

    // ---- Lossless Scaling: LSFG's shaders from the player's own Lossless.dll (Lsfg.java) ----

    // the shaders are there, so the settings offer LSFG
    private boolean lsfg;
    private volatile boolean lsfgBusy;
    private volatile String lsfgProgress;
    private String lsfgError;

    /** The app's copy of the chosen file, there only while its shaders are made. */
    private File lsfgCopy() { return new File(getCacheDir(), "import/Lossless.dll"); }

    private String lsfgState() {
        return lsfgBusy ? lsfgProgress : lsfgError != null ? "That file could not be used" : lsfg ? "Ready: LSFG is in the frame generation settings" : "Not set up";
    }

    private void lsfgPanel(Canvas c, float x, float y, float w) {
        y = wrap(c, "Lossless Scaling", Ui.title, 22, Ui.GOLD_TEXT, x, y, w) + Ui.u(12);
        y = wrap(c, "LSFG frame generation runs on the shaders in your own copy of Lossless Scaling. Press A and choose its Lossless.dll. The app translates the shaders "
            + "for this device and keeps only those, not the file.", Ui.body, 15, Ui.HELP, x, y, w) + Ui.u(14);
        if (lsfgBusy) { wrap(c, lsfgProgress, Ui.body, 15, Ui.JADE, x, y, w); return; }
        if (lsfgError != null) y = wrap(c, lsfgError, Ui.body, 15, Ui.OCHRE_TEXT, x, y, w) + Ui.u(10);
        if (lsfg) wrap(c, lsfgError != null ? "The shaders you already had are still in place." : "The shaders are ready. Frame generation in Settings now offers LSFG. "
            + "Choose the file again to replace them.", Ui.body, 15, lsfgError != null ? Ui.SOFT : Ui.JADE, x, y, w);
    }

    /** The system's file picker, with no filter: file providers disagree on the type of a .dll, and some would grey it out. */
    private void pickLsfg() {
        if (lsfgBusy) return;
        toolsLsfg = true;
        startActivityForResult(new Intent(Intent.ACTION_OPEN_DOCUMENT).addCategory(Intent.CATEGORY_OPENABLE).setType("*/*"), PICK_LSFG);
    }

    /** Copies the picked file, makes the shaders from it and deletes the copy. */
    private void installLsfg(Uri uri) {
        if (lsfgBusy) return; lsfgBusy = true;
        lsfgError = null; lsfgProgress = "Reading the file…"; current = "tools"; invalidateAll(page);
        new Thread(() -> {
            File copy = lsfgCopy(); copy.getParentFile().mkdirs();
            String failed = null;
            try {
                try (InputStream in = getContentResolver().openInputStream(uri); OutputStream out = new FileOutputStream(copy)) {
                    if (in == null) throw new IOException();
                    byte[] buf = new byte[1 << 16]; long size = 0;
                    for (int n; (n = in.read(buf)) > 0; ) { out.write(buf, 0, n); if ((size += n) > Lsfg.MAX_SIZE) { failed = Lsfg.NOT_DLL; break; } }
                } catch (IOException | RuntimeException e) { failed = "The file could not be read."; }
                if (failed == null) Lsfg.install(copy, getFilesDir(), (done, total) -> runOnUiThread(() -> { lsfgProgress = "Translating shaders: " + done + " of " + total; invalidateAll(page); }));
            } catch (IOException | RuntimeException e) { failed = e.getMessage(); }
            finally { copy.delete(); }
            String f = failed; boolean ok = Lsfg.ready(getFilesDir());
            runOnUiThread(() -> { lsfgBusy = false; lsfgError = f; lsfg = ok; if (current.equals("tools") || current.equals("settings")) show(current); else invalidateAll(page); });
        }).start();
    }

    // ---- Saves: export, import and backups (Saves.java); a page of its own under Tools ----

    private static final String SAVE_WARNING = "Uninstalling the app deletes saves. Export them first.";
    private interface SaveWork { void run() throws IOException; }

    private Saves saves;
    // what a task is doing; how the last one ended
    private volatile String saveProgress, saveNote, saveError;
    // a save read and checked, waiting for the player's answer
    private volatile Saves.Staged saveStaged;
    // the question page: what it says, its rows (the last one is Cancel), and B
    private String askTitle, askText;
    private String[] askRows;
    private Runnable[] askDo;
    private Runnable askBack;

    /** The game runs in the process :game, which stays until GameActivity is destroyed (it may outlive Quit by some seconds). */
    private boolean gameRunning() {
        List<ActivityManager.RunningAppProcessInfo> all = getSystemService(ActivityManager.class).getRunningAppProcesses();
        if (all != null) for (ActivityManager.RunningAppProcessInfo p : all) if (p.processName.equals(getPackageName() + ":game")) return true;
        return false;
    }

    /** Nothing to do while a task runs, and the game must be closed: said now, before the player picks anything. */
    private boolean saveBlocked() {
        if (Saves.busy()) return true;
        saveNote = null; saveError = gameRunning() ? Saves.RUNNING : null;
        invalidateAll(page);
        return saveError != null;
    }

    private static String saveSize(Saves.Info i) {
        return (i.slots.size() == 1 ? "1 saved game" : i.slots.size() + " saved games") + String.format(Locale.ROOT, " · %.1f MB", i.bytes / 1048576.0);
    }

    /** A backup's name is its time (yyyyMMdd-HHmmss-...), shown as the bug reports' are. */
    private static String backupTime(String n) {
        return n.length() >= 15 ? n.substring(6, 8) + "/" + n.substring(4, 6) + " " + n.substring(9, 11) + ":" + n.substring(11, 13) : n;
    }

    private void savesPage() {
        page.addView(new Backdrop(this, false), new FrameLayout.LayoutParams(-1, -1));
        float px = W() - 56 - 440, listW = px - 40 - 56;
        Saves.Info now = saves.current();
        place(new Drawn(this, (c, w, h) -> {
            header(c, w, "Saves", "Tools › Saves");
            float fy = h - Ui.u(24) - Ui.u(24), fx = Ui.hint(c, Ui.u(56), fy, "A", "Select"); Ui.hint(c, fx, fy, "B", "Back");
        }), 0, 0, -1, -1);
        place(new Drawn(this, (c, w, h) -> {
            Ui.panel(c, Ui.u(1), Ui.u(1), w - Ui.u(1), h - Ui.u(1));
            float x = Ui.u(26), pw = w - Ui.u(52), y = Ui.u(24);
            y = wrap(c, "Your save", Ui.title, 22, Ui.GOLD_TEXT, x, y, pw) + Ui.u(10);
            y = wrap(c, now != null ? saveSize(now) : "No save yet. You can import one now, without starting the game first.", Ui.body, 15, Ui.HELP, x, y, pw) + Ui.u(14);
            y = wrap(c, "Export makes a .zip you can keep. Import takes a .zip or a save folder and replaces your save. Your current save is backed up first, and the newest "
                + Saves.KEEP + " backups are kept on this device.", Ui.body, 15, Ui.HELP, x, y, pw) + Ui.u(14);
            y = wrap(c, SAVE_WARNING, Ui.body, 15, Ui.OCHRE_TEXT, x, y, pw) + Ui.u(14);
            String p = saveProgress, n = saveNote, e = saveError;
            if (p != null) wrap(c, p, Ui.body, 15, Ui.JADE, x, y, pw);
            else if (e != null) wrap(c, e, Ui.body, 15, Ui.OCHRE_TEXT, x, y, pw);
            else if (n != null) wrap(c, n, Ui.body, 15, Ui.JADE, x, y, pw);
        }), px, 120, 440, H() - 120 - 76);
        ScrollView scroll = new ScrollView(this); scroll.setVerticalScrollBarEnabled(false);
        LinearLayout list = column(); scroll.addView(list);
        place(scroll, 56, 120, listW, H() - 120 - 76);
        Row export = saveRow(list, "Export save…", v -> {
            if (saveBlocked()) return;
            if (now == null || now.slots.isEmpty()) { saveError = Saves.NO_SAVE; invalidateAll(page); return; }
            savePicker(new Intent(Intent.ACTION_CREATE_DOCUMENT).addCategory(Intent.CATEGORY_OPENABLE).setType("application/zip")
                .putExtra(Intent.EXTRA_TITLE, Saves.exportName(Game.TITLE, new java.util.Date())), MAKE_SAVE_ZIP);
        });
        saveRow(list, "Import a .zip…", v -> {
            if (!saveBlocked()) savePicker(new Intent(Intent.ACTION_OPEN_DOCUMENT).addCategory(Intent.CATEGORY_OPENABLE).setType("*/*")
                .putExtra(Intent.EXTRA_MIME_TYPES, new String[]{"application/zip", "application/x-zip-compressed", "application/octet-stream"}), PICK_SAVE_ZIP);
        });
        saveRow(list, "Import a folder…", v -> { if (!saveBlocked()) savePicker(new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE), PICK_SAVE_FOLDER); });
        List<Saves.Backup> backups = saves.backups();
        View label = new Drawn(this, (c, w, h) -> c.drawText(backups.isEmpty() ? "NO BACKUPS YET" : "BACKUPS", Ui.u(36), h - Ui.u(10), spaced(Ui.text(Ui.semibold, 12, Ui.MUTED))));
        list.addView(label, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, Ui.ui(40)));
        for (Saves.Backup b : backups) {
            addRow(list, 52, (c, r, on) -> {
                float mid = r.getHeight() / 2f;
                Ui.centred(c, backupTime(b.name), Ui.u(36), mid, Ui.text(Ui.body, 17, on ? Ui.GOLD_TEXT : Ui.CREAM));
                Paint cap = Ui.text(Ui.body, 13, Ui.MUTED); cap.setTextAlign(Paint.Align.RIGHT);
                Ui.centred(c, saveSize(b.info), r.getWidth() - Ui.u(16), mid, cap);
            }, v -> {
                if (!saveBlocked()) ask("Backup of " + backupTime(b.name), saveSize(b.info), new String[]{"Restore this backup", "Delete this backup", "Cancel"},
                    new Runnable[]{() -> askRestore(b), () -> askDelete(b), () -> show("saves")}, () -> show("saves"));
            });
        }
        prefer(export);
    }

    private Row saveRow(LinearLayout list, String label, View.OnClickListener click) {
        return addRow(list, 52, (c, r, on) -> Ui.centred(c, label, Ui.u(36), r.getHeight() / 2f, Ui.text(Ui.title, 19, on ? Ui.GOLD_TEXT : Ui.CREAM)), click);
    }

    private void savePicker(Intent picker, int request) {
        try { startActivityForResult(picker, request); }
        catch (ActivityNotFoundException e) { saveError = "This device has no file picker for this."; invalidateAll(page); }
    }

    /** A question with its answers as rows, so touch and the controller both reach them. The focus starts on Cancel. */
    private void ask(String title, String text, String[] rows, Runnable[] actions, Runnable back) {
        askTitle = title; askText = text; askRows = rows; askDo = actions; askBack = back;
        show("saveask");
    }

    private void saveAskPage() {
        page.addView(new Backdrop(this, false), new FrameLayout.LayoutParams(-1, -1));
        place(new Drawn(this, (c, w, h) -> {
            header(c, w, "Saves", "Tools › Saves");
            float fy = h - Ui.u(24) - Ui.u(24), fx = Ui.hint(c, Ui.u(56), fy, "A", "Select"); Ui.hint(c, fx, fy, "B", "Cancel");
        }), 0, 0, -1, -1);
        place(new Drawn(this, (c, w, h) -> {
            Ui.panel(c, Ui.u(1), Ui.u(1), w - Ui.u(1), h - Ui.u(1));
            float x = Ui.u(26), pw = w - Ui.u(52), y = Ui.u(24);
            y = wrap(c, askTitle, Ui.title, 22, Ui.GOLD_TEXT, x, y, pw) + Ui.u(12);
            wrap(c, askText, Ui.body, 15, Ui.HELP, x, y, pw);
        }), 56, 120, W() - 112, 180);
        LinearLayout list = column();
        place(list, 56, 316, 560, 52 * 3 + 12);
        Row last = null;
        for (int i = 0; i < askRows.length; i++) { Runnable action = askDo[i]; last = saveRow(list, askRows[i], v -> action.run()); }
        prefer(last);
    }

    /** The question before a save is replaced. Whose save it is matters only with several users. */
    private String replaceText() {
        Saves.Info now = saves.current();
        String who = "";
        try { Saves.Account a = saves.account(); if (a.users > 1) who = "This goes to the user " + a.name + ". "; } catch (IOException e) { /* install says what is wrong */ }
        return who + (now == null ? "You have no save yet, so nothing is replaced." : "This replaces your current save (" + saveSize(now) + "). It is backed up first, and you can restore it from this page.");
    }

    private void askImport() {
        Saves.Staged s = saveStaged;
        Runnable cancel = () -> { saveStaged = null; saves.discard(); show("saves"); };
        ask("Import this save?", "Found " + saveSize(s.info) + ".\n\n" + replaceText(), new String[]{saves.current() == null ? "Import this save" : "Replace my save", "Cancel"}, new Runnable[]{() -> {
            saveStaged = null;
            saveTask("Importing…", () -> { saves.install(s, null, saveStep()); saveNote = "Imported. " + saveSize(s.info) + "."; });
        }, cancel}, cancel);
    }

    private void askRestore(Saves.Backup b) {
        ask("Restore this backup?", "Backup of " + backupTime(b.name) + ": " + saveSize(b.info) + ".\n\n" + replaceText(), new String[]{"Replace my save", "Cancel"}, new Runnable[]{
            () -> saveTask("Restoring…", () -> { saves.restore(b.name, saveStep()); saveNote = "Restored the backup of " + backupTime(b.name) + "."; }),
            () -> show("saves")}, () -> show("saves"));
    }

    private void askDelete(Saves.Backup b) {
        ask("Delete this backup?", "Backup of " + backupTime(b.name) + ": " + saveSize(b.info) + ".\n\nThis cannot be undone.", new String[]{"Delete this backup", "Cancel"}, new Runnable[]{
            () -> saveTask("Deleting…", () -> { saves.deleteBackup(b.name); saveNote = "Deleted the backup of " + backupTime(b.name) + "."; }),
            () -> show("saves")}, () -> show("saves"));
    }

    private Saves.Progress saveStep() { return what -> runOnUiThread(() -> { saveProgress = what; invalidateAll(page); }); }

    /** Runs a task of Saves.java off the UI thread, one at a time; the game cannot be started until it ends. Then the page shows
        how it went, or the question about the save it read. */
    private void saveTask(String first, SaveWork work) {
        if (!Saves.begin()) return;
        saveNote = saveError = null; saveProgress = first;
        show("saves");
        new Thread(() -> {
            String failed = null;
            try { work.run(); }
            catch (Saves.Problem e) { failed = e.getMessage(); }
            catch (IOException | RuntimeException e) { failed = Saves.NOT_READ; }
            finally { Saves.end(); }
            String f = failed;
            runOnUiThread(() -> {
                saveProgress = null; saveError = f;
                if (f == null && saveStaged != null) askImport();
                else if (current.equals("saves")) show("saves");
            });
        }).start();
    }

    private void importSaveZip(Uri uri) {
        saveTask("Reading the file…", () -> {
            try (InputStream in = getContentResolver().openInputStream(uri)) {
                if (in == null) throw new Saves.Problem(Saves.NOT_READ);
                saves.receive(in);
            }
            saveStaged = saves.stagePicked(saveStep());
        });
    }

    private void importSaveFolder(Uri uri) {
        saveTask("Reading the folder…", () -> saveStaged = saves.stage(new SaveDocuments(getContentResolver(), uri), true, saveStep()));
    }

    /** The zip is made and checked in private storage, then copied to the place the player chose and read back from there. A
        failure removes the file the picker made, so a broken zip is never left to be trusted later. */
    private void exportSave(Uri uri) {
        saveTask("Packing your save…", () -> {
            File zip = saves.exportFile();
            try {
                saves.exportZip(zip, saveStep());
                String made, there;
                try (InputStream in = new FileInputStream(zip); OutputStream out = getContentResolver().openOutputStream(uri, "wt")) {
                    if (out == null) throw new IOException();
                    made = Saves.pump(in, out, Long.MAX_VALUE);
                }
                try (InputStream in = getContentResolver().openInputStream(uri)) {
                    if (in == null) throw new IOException();
                    there = Saves.sha256(in);
                }
                if (!made.equals(there)) throw new IOException();
                saveNote = "Exported your save.";
            } catch (IOException | RuntimeException e) {
                try { DocumentsContract.deleteDocument(getContentResolver(), uri); } catch (IOException | RuntimeException gone) { /* it may not be there */ }
                if (e instanceof Saves.Problem) throw e;
                throw new Saves.Problem("The zip could not be written there. Your save was not changed.");
            } finally { zip.delete(); }
        });
    }

    // ---- Game files: prod.keys, the base game and the update (Setup.java, SetupFiles.java); first on a new install, then under Tools ----

    private static final int SETUP_KEYS = 0, SETUP_FOLDER = 1, SETUP_BASE = 2, SETUP_UPDATE = 3;
    private static final String[] SETUP_TITLES = {"Keys", "Find the game in a folder…", "Base game", "Update " + Game.VERSION};
    private static final String[] SETUP_HELP = {
        "Press A and pick your prod.keys file. The app keeps a private copy and reads it at each start, so you can delete your original. "
            + "Your keys stay in the app: they are not part of bug reports.",
        "Press A and pick the folder that holds the base game and the update. Subfolders count too, and the file names do not matter: the app looks inside the files. "
            + "Android does not offer every folder, such as Download itself. If yours is not offered, use a folder inside it or pick the two files below.",
        "Press A and pick the base game, an NSP or XCI file. You can select the update with it. The game is read from where the file is at every start, "
            + "and nothing is copied. If you move or delete the file, the app asks for it again.",
        "Press A and pick the " + Game.VERSION + " update, an NSP file. You can select the base game with it. The update is read from where the file is at every start, "
            + "and nothing is copied. If you move or delete the file, the app asks for it again."};

    private SetupFiles setup;
    // where B goes, and a first setup when it is done
    private String setupBack = "home";
    // the page opens by itself once: B then reaches the startup screen
    private boolean setupOffered;
    // the row to focus when the page is rebuilt; -1: the first that needs something
    private int setupFocus = -1;
    // the file row that opened the picker
    private int setupSlot;
    // no file picker on this device
    private String setupError;
    private volatile String setupProgress;

    private String setupNote(int row) {
        Setup s = setup.state;
        if (row == SETUP_KEYS) return s.keysProblem() != null ? s.keysProblem() : s.hasKeys() ? "Saved in the app's private storage" : "Pick your prod.keys file";
        if (row == SETUP_FOLDER) return "Finds the base game and the update by looking inside the files";
        int slot = row == SETUP_BASE ? Setup.BASE : Setup.UPDATE;
        Setup.Doc d = s.doc(slot);
        if (s.problem(slot) != null) return s.problem(slot);
        if (d == null) return "Not picked yet";
        return d.name + (d.size > 0 ? String.format(Locale.ROOT, " · %.1f GB", d.size / 1073741824.0) : "");
    }

    private boolean setupBad(int row) {
        Setup s = setup.state;
        return row == SETUP_KEYS ? s.keysProblem() != null : row != SETUP_FOLDER && s.problem(row == SETUP_BASE ? Setup.BASE : Setup.UPDATE) != null;
    }

    private boolean setupTick(int row) {
        Setup s = setup.state;
        return row == SETUP_KEYS ? s.keysOk() : row != SETUP_FOLDER && s.ok(row == SETUP_BASE ? Setup.BASE : Setup.UPDATE);
    }

    private void setupPage() {
        Setup s = setup.state;
        page.addView(new Backdrop(this, false), new FrameLayout.LayoutParams(-1, -1));
        float px = W() - 56 - 440, listW = px - 40 - 56;
        place(new Drawn(this, (c, w, h) -> {
            header(c, w, "Game files", setupBack.equals("tools") ? "Tools › Game files" : null);
            float fy = h - Ui.u(24) - Ui.u(24), fx = Ui.hint(c, Ui.u(56), fy, "A", "Select"); Ui.hint(c, fx, fy, "B", setupProgress != null ? "Stop" : "Back");
        }), 0, 0, -1, -1);
        View panel = new Drawn(this, (c, w, h) -> {
            Ui.panel(c, Ui.u(1), Ui.u(1), w - Ui.u(1), h - Ui.u(1));
            float x = Ui.u(26), pw = w - Ui.u(52), y = Ui.u(24); int row = Math.max(setupFocus, 0);
            y = wrap(c, SETUP_TITLES[row], Ui.title, 22, Ui.GOLD_TEXT, x, y, pw) + Ui.u(12);
            y = wrap(c, SETUP_HELP[row], Ui.body, 15, Ui.HELP, x, y, pw) + Ui.u(14);
            String progress = setupProgress;
            if (progress != null) wrap(c, progress, Ui.body, 15, Ui.JADE, x, y, pw);
            else if (setupError != null) wrap(c, setupError, Ui.body, 15, Ui.OCHRE_TEXT, x, y, pw);
            else if (setupBad(row)) wrap(c, setupNote(row), Ui.body, 15, Ui.OCHRE_TEXT, x, y, pw);
            else if (s.complete()) wrap(c, "All set. The game starts from these files.", Ui.body, 15, Ui.JADE, x, y, pw);
            c.drawLine(x, h - Ui.u(56), w - Ui.u(28), h - Ui.u(56), Ui.stroke(Ui.LINE, 1));
            Ui.centred(c, "The system's file picker may need the touch screen.", x, h - Ui.u(32), Ui.text(Ui.body, 14, Ui.MUTED));
        });
        place(panel, px, 120, 440, H() - 120 - 76);
        ScrollView scroll = new ScrollView(this); scroll.setVerticalScrollBarEnabled(false);
        LinearLayout list = column(); scroll.addView(list);
        place(scroll, 56, 120, listW, H() - 120 - 76);
        Row[] rows = new Row[SETUP_TITLES.length];
        for (int i = 0; i < rows.length; i++) {
            final int row = i;
            rows[i] = addRow(list, 76, (c, r, on) -> {
                c.drawText(SETUP_TITLES[row], Ui.u(39), Ui.u(29), Ui.text(Ui.title, 21, on ? Ui.GOLD_TEXT : Ui.CREAM));
                wrap(c, setupNote(row), Ui.body, 13, setupBad(row) ? Ui.OCHRE_TEXT : Ui.MUTED, Ui.u(39), Ui.u(37), r.getWidth() - Ui.u(39 + 48));
                if (setupTick(row)) Ui.tick(c, r.getWidth() - Ui.u(26), r.getHeight() / 2f, Ui.u(9), Ui.JADE);
            }, v -> setupPick(row));
            rows[i].onFocus = r -> { if (setupFocus != row) setupError = null; setupFocus = row; panel.invalidate(); };
        }
        if (setupFocus < 0) setupFocus = !s.keysOk() ? SETUP_KEYS : !s.ok(Setup.BASE) && !s.ok(Setup.UPDATE) ? SETUP_FOLDER : !s.ok(Setup.BASE) ? SETUP_BASE : !s.ok(Setup.UPDATE) ? SETUP_UPDATE : SETUP_KEYS;
        prefer(rows[setupFocus]);
    }

    /** The system's pickers, with no filter: file providers disagree on the type of these files, and some would grey them out. */
    private void setupPick(int row) {
        if (setupProgress != null) return;
        setupFocus = row;
        if (row != SETUP_KEYS && !setup.state.hasKeys()) { setup.state.askForKeys(); setupFocus = SETUP_KEYS; show("setup"); return; }
        Intent pick = row == SETUP_FOLDER ? new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE) : new Intent(Intent.ACTION_OPEN_DOCUMENT).addCategory(Intent.CATEGORY_OPENABLE).setType("*/*");
        if (row == SETUP_BASE || row == SETUP_UPDATE) { setupSlot = row == SETUP_BASE ? Setup.BASE : Setup.UPDATE; pick.putExtra(Intent.EXTRA_ALLOW_MULTIPLE, true); }
        pick.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION);
        try { startActivityForResult(pick, row == SETUP_KEYS ? PICK_KEYS : row == SETUP_FOLDER ? PICK_GAME_FOLDER : PICK_GAME_FILES); }
        catch (ActivityNotFoundException e) { setupError = "This device has no file picker for this."; invalidateAll(page); }
    }

    /** Reads a pick off the UI thread. A new player goes on to the startup screen as soon as everything passes. */
    private void setupTask(String first, Runnable work) {
        if (setupProgress != null) return;
        boolean before = setup.state.complete();
        setupProgress = first; current = "setup"; setupError = null;
        invalidateAll(page);
        new Thread(() -> {
            try { work.run(); } catch (RuntimeException e) { setupError = "That could not be read. Try again."; }
            if (Game.MODS) Mods.forgetRomfs();
            runOnUiThread(() -> {
                setupProgress = null;
                if (!before && setup.state.complete() && setupBack.equals("home")) show("home");
                else if (current.equals("setup")) show("setup");
            });
        }).start();
    }

    private void setupResult(int request, Intent data) {
        if (request == PICK_KEYS && data.getData() != null) { Uri uri = data.getData(); setupTask("Reading the keys…", () -> setup.takeKeys(uri)); }
        else if (request == PICK_GAME_FOLDER && data.getData() != null) {
            Uri uri = data.getData();
            setupTask("Looking through the folder…", () -> setup.takeFolder(uri, what -> runOnUiThread(() -> { setupProgress = what; invalidateAll(page); })));
        } else if (request == PICK_GAME_FILES) {
            List<Uri> uris = new ArrayList<>(); android.content.ClipData clip = data.getClipData();
            if (clip != null) for (int i = 0; i < clip.getItemCount(); i++) uris.add(clip.getItemAt(i).getUri());
            else if (data.getData() != null) uris.add(data.getData());
            int slot = setupSlot;
            if (!uris.isEmpty()) setupTask("Reading the files…", () -> setup.takeFiles(uris, slot));
        }
    }

    // ---- Licences: the third-party software in this APK and its licence texts (app/licenses.tsv, assets/licenses) ----

    // the licence text: L1 / R1 page through it (focus stays on the list)
    private ScrollView licenceScroll;

    private void licencesPage() {
        page.addView(new Backdrop(this, false), new FrameLayout.LayoutParams(-1, -1));
        float px = W() - 56 - 560, listW = px - 40 - 56;
        place(new Drawn(this, (c, w, h) -> {
            header(c, w, "Licences", "Tools › Licences");
            float fy = h - Ui.u(24) - Ui.u(24), fx = Ui.hint(c, Ui.u(56), fy, "L R", "Scroll the text"); Ui.hint(c, fx, fy, "B", "Back");
        }), 0, 0, -1, -1);
        List<String[]> rows = new ArrayList<>();
        try (java.io.BufferedReader r = new java.io.BufferedReader(new java.io.InputStreamReader(getAssets().open("licenses/index.tsv"), StandardCharsets.UTF_8))) {
            for (String line; (line = r.readLine()) != null; ) { String[] f = line.split("\t", -1); if (f.length >= 4) rows.add(f); }
        } catch (IOException e) { /* an APK built without them: the list stays empty */ }
        TextView text = new TextView(this);
        text.setTypeface(Typeface.MONOSPACE); text.setTextColor(Ui.SOFT); text.setTextSize(TypedValue.COMPLEX_UNIT_PX, Ui.u(12));
        ScrollView textScroll = new ScrollView(this); textScroll.addView(text); textScroll.setFocusable(false); licenceScroll = textScroll;
        place(new Drawn(this, (c, w, h) -> Ui.panel(c, Ui.u(1), Ui.u(1), w - Ui.u(1), h - Ui.u(1))), px, 120, 560, H() - 120 - 76);
        place(textScroll, px + 20, 120 + 16, 560 - 40, H() - 120 - 76 - 32);
        ScrollView scroll = new ScrollView(this); scroll.setVerticalScrollBarEnabled(false);
        LinearLayout list = column(); scroll.addView(list);
        place(scroll, 56, 120, listW, H() - 120 - 76);
        Row first = null;
        for (String[] f : rows) {
            Row r = addRow(list, 56, (c, row, on) -> {
                c.drawText(f[0], Ui.u(36), Ui.u(25), Ui.text(Ui.body, 17, on ? Ui.GOLD_TEXT : Ui.CREAM));
                c.drawText(f[1], Ui.u(36), Ui.u(44), Ui.text(Ui.body, 13, Ui.MUTED));
            }, null);
            r.onFocus = row -> {
                StringBuilder b = new StringBuilder(f[0]).append("\n").append(f[2]).append("\n");
                for (String file : f[3].split(",")) {
                    if (file.isEmpty()) continue;
                    // as the source names it: build_apk.sh adds .txt
                    b.append("\n").append("──────── ").append(file.substring(3, file.length() - 4)).append(" ────────\n\n");
                    try (InputStream in = getAssets().open("licenses/" + file)) {
                        java.io.ByteArrayOutputStream out = new java.io.ByteArrayOutputStream(); byte[] buf = new byte[1 << 14];
                        for (int n; (n = in.read(buf)) > 0; ) out.write(buf, 0, n);
                        b.append(new String(out.toByteArray(), StandardCharsets.UTF_8));
                    } catch (IOException e) { b.append("(missing)\n"); }
                }
                text.setText(b); textScroll.scrollTo(0, 0);
            };
            if (first == null) first = r;
        }
        if (first != null) prefer(first);
    }

    // ---- GPU driver: the system's, the bundled Turnip, or one installed from a zip (Drivers.java) ----

    private String driverStatus = "", driverArmed, driverFocus;
    private boolean driverBusy;

    /** The selected driver (TOTK_GPU_DRIVER in the preferences), corrected to one this device can use: the bundled Turnip
        and zip drivers need an Adreno GPU; a removed zip falls back to the default. */
    private String driverChoice() {
        boolean adreno = Drivers.adreno(), bundled = turnip() && adreno;
        String def = bundled ? Drivers.TURNIP : Drivers.SYSTEM, v = prefs.getString("TOTK_GPU_DRIVER", def);
        if (v.equals(Drivers.TURNIP)) return bundled ? v : Drivers.SYSTEM;
        if (v.startsWith(Drivers.ZIP)) return adreno && Drivers.find(this, v.substring(Drivers.ZIP.length())) != null ? v : def;
        return Drivers.SYSTEM;
    }

    private String driverLabel(String choice) {
        if (choice.equals(Drivers.TURNIP)) return "Turnip (bundled)";
        if (choice.startsWith(Drivers.ZIP)) { Drivers.Driver d = Drivers.find(this, choice.substring(Drivers.ZIP.length())); return d != null ? d.name : "Installed driver"; }
        return "System driver";
    }

    /** The translated-shader cache of a driver (each subgroup width needs its own translations: host/app.c). */
    private File shaderCache(String choice) {
        return new File(getFilesDir(), choice.equals(Drivers.TURNIP) ? "shader-cache-turnip"
            : choice.startsWith(Drivers.ZIP) ? "shader-cache-zip-" + choice.substring(Drivers.ZIP.length()) : "shader-cache");
    }

    private void driversPage() {
        page.addView(new Backdrop(this, false), new FrameLayout.LayoutParams(-1, -1));
        float px = W() - 56 - 440, listW = px - 40 - 56;
        boolean adreno = Drivers.adreno();
        List<Drivers.Driver> installed = Drivers.list(this);
        place(new Drawn(this, (c, w, h) -> {
            header(c, w, "GPU driver", "Settings › System › GPU driver");
            float fy = h - Ui.u(24) - Ui.u(24), fx = Ui.hint(c, Ui.u(56), fy, "A", "Use");
            if (driverFocus != null && driverFocus.startsWith(Drivers.ZIP)) fx = Ui.hint(c, fx, fy, "X", driverArmed != null ? "Press again to remove" : "Remove");
            Ui.hint(c, fx, fy, "B", "Back");
            Paint n = Ui.text(Ui.body, 14, Ui.JADE); n.setTextAlign(Paint.Align.RIGHT); Ui.centred(c, driverStatus, w - Ui.u(56), fy + Ui.u(12), n);
        }), 0, 0, -1, -1);
        View detail = new Drawn(this, (c, w, h) -> {
            Ui.panel(c, Ui.u(1), Ui.u(1), w - Ui.u(1), h - Ui.u(1));
            float x = Ui.u(26), pw = w - Ui.u(52), y = Ui.u(24);
            String f = driverFocus;
            if (f == null) {
                y = wrap(c, "Installing a driver", Ui.title, 22, Ui.GOLD_TEXT, x, y, pw) + Ui.u(12);
                y = wrap(c, "Pick a driver .zip made for emulators, such as a Turnip build or a packaged Qualcomm driver. The zip holds a meta.json and the driver's "
                    + "library. Each driver keeps its own shader cache, so its first start translates shaders as the game needs them.", Ui.body, 15, Ui.HELP, x, y, pw) + Ui.u(14);
                wrap(c, adreno ? "If a driver does not work, the game stops with the reason instead of switching to another driver. Choose a different driver here and save a bug "
                    + "report from Tools so the result reaches us." : "This device's GPU is not an Adreno. Driver packages only run on Adreno GPUs, so this device uses its system driver.",
                    Ui.body, 14, adreno ? Ui.SOFT : Ui.OCHRE_TEXT, x, y, pw);
                return;
            }
            if (f.equals(Drivers.SYSTEM)) {
                y = wrap(c, "System driver", Ui.title, 22, Ui.GOLD_TEXT, x, y, pw) + Ui.u(10);
                wrap(c, "The driver that came with this device. It needs nothing installed and is the one to use when other drivers fail.", Ui.body, 15, Ui.HELP, x, y, pw);
            } else if (f.equals(Drivers.TURNIP)) {
                y = wrap(c, "Turnip (bundled)", Ui.title, 22, Ui.GOLD_TEXT, x, y, pw) + Ui.u(10);
                wrap(c, "The open-source Mesa driver for Adreno GPUs that comes with this app, with our fixes. It is the fastest choice on the devices we test.", Ui.body, 15, Ui.HELP, x, y, pw);
            } else {
                Drivers.Driver d = Drivers.find(this, f.substring(Drivers.ZIP.length())); if (d == null) return;
                y = wrap(c, d.name, Ui.title, 22, Ui.GOLD_TEXT, x, y, pw) + Ui.u(6);
                String by = (d.author.isEmpty() ? "" : "By " + d.author) + (d.version.isEmpty() ? "" : (d.author.isEmpty() ? "" : " · ") + "version " + d.version);
                if (!by.isEmpty()) y = wrap(c, by, Ui.body, 14, Ui.MUTED, x, y, pw) + Ui.u(10);
                if (!d.description.isEmpty()) y = wrap(c, d.description, Ui.body, 15, Ui.HELP, x, y, pw) + Ui.u(10);
                wrap(c, (d.vendor.isEmpty() ? "" : "Vendor: " + d.vendor + "\n") + (d.driverVersion.isEmpty() ? "" : "Driver version: " + d.driverVersion + "\n") + "Library: " + d.library,
                    Ui.body, 14, Ui.SOFT, x, y, pw);
            }
        });
        place(detail, px, 120, 440, H() - 120 - 76);
        ScrollView scroll = new ScrollView(this); scroll.setVerticalScrollBarEnabled(false);
        LinearLayout list = column(); scroll.addView(list);
        place(scroll, 56, 120, listW, H() - 120 - 76);
        String chosen = driverChoice();
        List<String> choices = new ArrayList<>(); choices.add(Drivers.SYSTEM);
        if (turnip() && adreno) choices.add(Drivers.TURNIP);
        if (adreno) for (Drivers.Driver d : installed) choices.add(Drivers.ZIP + d.id);
        Row focus = null;
        for (String ch : choices) {
            Drivers.Driver d = ch.startsWith(Drivers.ZIP) ? Drivers.find(this, ch.substring(Drivers.ZIP.length())) : null;
            Row r = addRow(list, 52, (c, row, on) -> {
                float mid = row.getHeight() / 2f;
                Ui.centred(c, driverLabel(ch), Ui.u(36), mid, Ui.text(Ui.body, 18, on ? Ui.GOLD_TEXT : Ui.CREAM));
                float right = row.getWidth() - Ui.u(16);
                if (d != null && d.id.equals(driverArmed)) right = Ui.tag(c, right, mid, "PRESS X AGAIN", Ui.OCHRE, Ui.OCHRE_TEXT) - Ui.u(8);
                if (ch.equals(driverChoice())) Ui.tag(c, right, mid, "IN USE", Ui.JADE, Ui.JADE);
            }, v -> { prefs.edit().putString("TOTK_GPU_DRIVER", ch).apply(); driverStatus = driverLabel(ch) + " is used from the next start."; invalidateAll(page); });
            if (d != null) r.setTag(d);
            r.onFocus = row -> { driverFocus = ch; if (d == null || !d.id.equals(driverArmed)) driverArmed = null; invalidateAll(page); };
            if (ch.equals(chosen)) focus = r;
        }
        if (adreno) {
            Row add = addRow(list, 52, (c, r, on) -> Ui.centred(c, "Install a driver…", Ui.u(36), r.getHeight() / 2f, Ui.text(Ui.title, 19, on ? Ui.GOLD_TEXT : Ui.SOFT)),
                v -> startActivityForResult(new Intent(Intent.ACTION_OPEN_DOCUMENT).addCategory(Intent.CATEGORY_OPENABLE).setType("*/*")
                    .putExtra(Intent.EXTRA_MIME_TYPES, new String[]{"application/zip", "application/x-zip-compressed", "application/octet-stream"}), PICK_DRIVER));
            add.onFocus = r -> { driverFocus = null; driverArmed = null; invalidateAll(page); };
            ((LinearLayout.LayoutParams) add.getLayoutParams()).topMargin = Ui.ui(8);
        }
        if (focus != null) prefer(focus);
    }

    /** Copies the picked zip, unpacks it (Drivers.install) and selects the new driver. */
    private void installDriver(Uri uri) {
        if (driverBusy) return; driverBusy = true;
        driverStatus = "Installing…"; current = "drivers"; invalidateAll(page);
        new Thread(() -> {
            File copy = new File(getCacheDir(), "import/driver.zip"); copy.getParentFile().mkdirs();
            String status; String select = null;
            try {
                try (InputStream in = getContentResolver().openInputStream(uri); OutputStream out = new FileOutputStream(copy)) {
                    if (in == null) throw new IOException("The file could not be opened.");
                    byte[] buf = new byte[1 << 16]; for (int n; (n = in.read(buf)) > 0; ) out.write(buf, 0, n);
                }
                Drivers.Driver d = Drivers.install(this, copy);
                select = Drivers.ZIP + d.id; status = "Installed " + d.name + ". It is used from the next start.";
            } catch (IOException e) { status = "Could not install the driver. " + e.getMessage(); }
            finally { copy.delete(); driverBusy = false; }
            String done = status, pick = select;
            runOnUiThread(() -> { driverStatus = done; if (pick != null) { prefs.edit().putString("TOTK_GPU_DRIVER", pick).apply(); driverFocus = pick; } show("drivers"); });
        }).start();
    }

    // ---- Mods: the installed mods, their load order and what is known about each (Mods.java) ----

    private boolean modsOn() { return prefs.getBoolean("mods_on", true); }

    private void modsPage() {
        page.addView(new Backdrop(this, false), new FrameLayout.LayoutParams(-1, -1));
        float px = W() - 56 - 440, listW = px - 40 - 56;
        place(new Drawn(this, (c, w, h) -> {
            header(c, w, "Mods", mods.isEmpty() ? null : "When mods change the same file, the one higher in the list wins.");
            float fy = h - Ui.u(24) - Ui.u(24), fx = Ui.hint(c, Ui.u(56), fy, "A", "On / off");
            fx = Ui.hint(c, fx, fy, "Y", modMoving ? "Done moving" : "Move"); fx = Ui.hint(c, fx, fy, "X", modArmed != null ? "Press again to remove" : "Remove");
            Ui.hint(c, fx, fy, "B", "Back");
            Paint n = Ui.text(Ui.body, 14, Ui.JADE); n.setTextAlign(Paint.Align.RIGHT); Ui.centred(c, modStatus, w - Ui.u(56), fy + Ui.u(12), n);
        }), 0, 0, -1, -1);
        View detail = new Drawn(this, (c, w, h) -> modDetailPanel(c, w, h));
        place(detail, px, 120, 440, H() - 120 - 76);
        ScrollView scroll = new ScrollView(this); scroll.setVerticalScrollBarEnabled(false);
        LinearLayout list = column(); scroll.addView(list);
        place(scroll, 56, 120, listW, H() - 120 - 76);
        Row master = addRow(list, 52, (c, r, on) -> {
            Ui.centred(c, "Use mods", Ui.u(36), r.getHeight() / 2f, Ui.text(Ui.body, 18, on ? Ui.GOLD_TEXT : Ui.CREAM));
            Ui.value(c, r.getWidth() - Ui.u(16), r.getHeight() / 2f, modsOn() ? "On" : "Off", Ui.JADE);
        }, v -> { prefs.edit().putBoolean("mods_on", !modsOn()).apply(); invalidateAll(page); });
        master.stepper = by -> { prefs.edit().putBoolean("mods_on", !modsOn()).apply(); invalidateAll(page); };
        master.onFocus = r -> { modDetail = null; modArmed = null; detail.invalidate(); };
        Row focus = master;
        for (Mods.Mod m : mods) {
            Row r = addRow(list, 60, (c, row, on) -> {
                boolean live = m.on && modsOn();
                c.drawText(m.name, Ui.u(36), Ui.u(26), Ui.text(Ui.body, 18, on ? Ui.GOLD_TEXT : live ? Ui.CREAM : Ui.MUTED));
                String cap = (m.on ? "On" : "Off") + (m.author.isEmpty() ? "" : " · " + m.author) + (m.made.isEmpty() ? "" : " · made for " + m.made)
                    + " · " + (m.replaced + m.added) + " files";
                c.drawText(cap, Ui.u(36), Ui.u(46), Ui.text(Ui.body, 13, Ui.MUTED));
                float right = row.getWidth() - Ui.u(16), mid = row.getHeight() / 2f;
                if (modMoving && m == modDetail) right = Ui.tag(c, right, mid, "MOVING  ↑↓", Ui.GOLD, Ui.GOLD_TEXT) - Ui.u(8);
                else if (modArmed == m) right = Ui.tag(c, right, mid, "PRESS X AGAIN", Ui.OCHRE, Ui.OCHRE_TEXT) - Ui.u(8);
                if (m.problem != null) right = Ui.tag(c, right, mid, m.problem.contains("crashed") ? "CRASH" : "FAILED", Ui.OCHRE, Ui.OCHRE_TEXT) - Ui.u(8);
                else if (!m.warnings.isEmpty() || !m.notes.isEmpty()) right = Ui.tag(c, right, mid, "NOTES", 0x6EEEE8D8, Ui.SOFT) - Ui.u(8);
                if (!m.on) Ui.tag(c, right, mid, "OFF", 0x6EEEE8D8, Ui.MUTED);
            }, v -> { if (modMoving) { modMoving = false; invalidateAll(page); return; } m.on = !m.on; Mods.save(mods); invalidateAll(page); });
            r.setTag(m);
            r.onFocus = row -> { if (modMoving && modDetail != m) modMoving = false; modDetail = m; if (modArmed != m) modArmed = null; invalidateAll(page); };
            if (m.id.equals(modFocus)) focus = r;
        }
        Row add = addRow(list, 52, (c, r, on) -> Ui.centred(c, "Add a mod…", Ui.u(36), r.getHeight() / 2f, Ui.text(Ui.title, 19, on ? Ui.GOLD_TEXT : Ui.SOFT)),
            v -> show("addmod"));
        add.onFocus = r -> { modDetail = null; modArmed = null; modMoving = false; detail.invalidate(); };
        ((LinearLayout.LayoutParams) add.getLayoutParams()).topMargin = Ui.ui(8);
        if (modFocus == null && !mods.isEmpty()) focus = master;
        prefer(focus);
    }

    /** The right panel: the focused mod, or what the page is for. */
    private void modDetailPanel(Canvas c, float w, float h) {
        Ui.panel(c, Ui.u(1), Ui.u(1), w - Ui.u(1), h - Ui.u(1));
        float x = Ui.u(26), pw = w - Ui.u(52), y = Ui.u(24);
        Mods.Mod m = modDetail;
        if (m == null) {
            y = wrap(c, "Mods", Ui.title, 24, Ui.GOLD_TEXT, x, y, pw) + Ui.u(12);
            y = wrap(c, "This port supports RomFS mods, which change game files such as models, textures, actors and text. Install them from a .zip file. "
                + "Mods made for older game versions are merged into 1.4.3. The mod's changes are kept, "
                + "along with everything that 1.4.3 has changed since. Code mods (exefs) are not supported.",
                Ui.body, 15, Ui.HELP, x, y, pw) + Ui.u(14);
            if (!Mods.BASE.exists() && !picked()) y = wrap(c, "The 1.0.0 base file is missing. Without it, a mod's packs replace the game's whole "
                + "instead of being merged, so mods made for older versions may undo changes from 1.4.3.", Ui.body, 14, Ui.OCHRE_TEXT, x, y, pw) + Ui.u(10);
            if (modGeneral != null) wrap(c, modGeneral, Ui.body, 14, Ui.OCHRE_TEXT, x, y, pw);
            return;
        }
        File pic = m.preview();
        if (pic != null) {
            android.graphics.Bitmap b = android.graphics.BitmapFactory.decodeFile(pic.getPath());
            if (b != null) { float ih = Math.min(Ui.u(170), pw * b.getHeight() / b.getWidth()), iw = ih * b.getWidth() / b.getHeight();
                c.drawBitmap(b, null, new RectF(x, y, x + iw, y + ih), new Paint(Paint.FILTER_BITMAP_FLAG)); y += ih + Ui.u(14); }
        }
        y = wrap(c, m.name, Ui.title, 23, Ui.GOLD_TEXT, x, y, pw) + Ui.u(4);
        String by = (m.author.isEmpty() ? "Author not given" : "By " + m.author) + (m.version.isEmpty() ? "" : " · version " + m.version);
        y = wrap(c, by, Ui.body, 14, Ui.MUTED, x, y, pw) + Ui.u(10);
        if (m.problem != null) y = wrap(c, m.problem, Ui.body, 15, Ui.OCHRE_TEXT, x, y, pw) + Ui.u(10);
        if (!m.description.isEmpty()) y = wrap(c, m.description, Ui.body, 15, Ui.HELP, x, y, pw) + Ui.u(10);
        String facts = String.format(Locale.ROOT, "%d files replaced, %d added · %.1f MB", m.replaced, m.added, m.bytes / 1048576.0)
            + (m.contents.isEmpty() ? "" : "\nChanges: " + String.join(", ", m.contents))
            + "\nMade for: " + (m.made.isEmpty() ? "not stated" : m.made) + "\nSource: " + m.source;
        y = wrap(c, facts, Ui.body, 14, Ui.SOFT, x, y, pw) + Ui.u(10);
        for (String warn : m.warnings) y = wrap(c, "◆ " + warn, Ui.body, 14, Ui.OCHRE_TEXT, x, y, pw) + Ui.u(6);
        int shown = 0;
        for (String note : m.notes) { if (y > h - Ui.u(40) || ++shown > 6) { wrap(c, "…and " + (m.notes.size() - shown + 1) + " more notes", Ui.body, 13, Ui.MUTED, x, y, pw); break; }
            y = wrap(c, note, Ui.body, 13, Ui.MUTED, x, y, pw) + Ui.u(4); }
    }


    private void addModPage() {
        page.addView(new Backdrop(this, false), new FrameLayout.LayoutParams(-1, -1));
        float px = W() - 56 - 440, listW = px - 40 - 56;
        place(new Drawn(this, (c, w, h) -> {
            header(c, w, "Add a mod", "Mods › Add");
            float fy = h - Ui.u(24) - Ui.u(24), fx = Ui.hint(c, Ui.u(56), fy, "A", "Install"); Ui.hint(c, fx, fy, "B", "Back");
            Paint n = Ui.text(Ui.body, 14, Ui.JADE); n.setTextAlign(Paint.Align.RIGHT); Ui.centred(c, modStatus, w - Ui.u(56), fy + Ui.u(12), n);
        }), 0, 0, -1, -1);
        View detail = new Drawn(this, (c, w, h) -> {
            Ui.panel(c, Ui.u(1), Ui.u(1), w - Ui.u(1), h - Ui.u(1));
            float x = Ui.u(26), pw = w - Ui.u(52), y = Ui.u(24);
            File f = modCandidate;
            if (f == null) {
                y = wrap(c, "Adding mods", Ui.title, 22, Ui.GOLD_TEXT, x, y, pw) + Ui.u(12);
                wrap(c, "Browse for a mod's .zip file. Unpack .7z and .rar archives first.", Ui.body, 15, Ui.HELP, x, y, pw);
                return;
            }
            y = wrap(c, f.getName(), Ui.title, 21, Ui.GOLD_TEXT, x, y, pw) + Ui.u(8);
            wrap(c, f.getParent() + "\n" + (f.isDirectory() ? "folder" : String.format(Locale.ROOT, "%.1f MB", f.length() / 1048576.0))
                + " · " + new java.text.SimpleDateFormat("d MMM yyyy, HH:mm", Locale.getDefault()).format(new java.util.Date(f.lastModified())), Ui.body, 14, Ui.SOFT, x, y, pw);
        });
        place(detail, px, 120, 440, H() - 120 - 76);
        ScrollView scroll = new ScrollView(this); scroll.setVerticalScrollBarEnabled(false);
        LinearLayout list = column(); scroll.addView(list);
        place(scroll, 56, 120, listW, H() - 120 - 76);
        Row browse = addRow(list, 52, (c, r, on) -> Ui.centred(c, "Browse files…", Ui.u(36), r.getHeight() / 2f, Ui.text(Ui.title, 19, on ? Ui.GOLD_TEXT : Ui.CREAM)), v -> browseMod());
        browse.onFocus = r -> { modCandidate = null; detail.invalidate(); };
        modCandidates = Mods.candidates();
        if (modCandidates.isEmpty()) {
            View none = new Drawn(this, (c, w, h) -> c.drawText("NO MODS FOUND IN DOWNLOAD OR THE MODS INBOX", Ui.u(36), h - Ui.u(12), spaced(Ui.text(Ui.semibold, 12, Ui.MUTED))));
            list.addView(none, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, Ui.ui(40)));
        }
        for (File f : modCandidates) {
            boolean installed = mods.stream().anyMatch(m -> m.source.equals(f.getName()));
            Row r = addRow(list, 52, (c, row, on) -> {
                Ui.centred(c, f.getName(), Ui.u(36), row.getHeight() / 2f, Ui.text(Ui.body, 17, on ? Ui.GOLD_TEXT : Ui.CREAM));
                if (installed) Ui.tag(c, row.getWidth() - Ui.u(16), row.getHeight() / 2f, "INSTALLED", 0x6EEEE8D8, Ui.SOFT);
            }, v -> installMod(f, false));
            r.onFocus = row -> { modCandidate = f; detail.invalidate(); };
        }
        prefer(browse);
    }

    private void browseMod() {
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT).addCategory(Intent.CATEGORY_OPENABLE).setType("*/*")
            .putExtra(Intent.EXTRA_MIME_TYPES, new String[]{"application/zip", "application/x-zip-compressed", "application/octet-stream"});
        startActivityForResult(i, PICK_MOD);
    }

    @Override protected void onActivityResult(int request, int result, Intent data) {
        super.onActivityResult(request, result, data);
        if (Game.SETUP && (request == PICK_KEYS || request == PICK_GAME_FOLDER || request == PICK_GAME_FILES)) { if (result == RESULT_OK && data != null) setupResult(request, data); return; }
        if (request == PICK_DRIVER && result == RESULT_OK && data != null && data.getData() != null) { installDriver(data.getData()); return; }
        if (request == PICK_LSFG && result == RESULT_OK && data != null && data.getData() != null) { installLsfg(data.getData()); return; }
        if (request == PICK_SAVE_ZIP && result == RESULT_OK && data != null && data.getData() != null) { importSaveZip(data.getData()); return; }
        if (request == PICK_SAVE_FOLDER && result == RESULT_OK && data != null && data.getData() != null) { importSaveFolder(data.getData()); return; }
        if (request == MAKE_SAVE_ZIP && result == RESULT_OK && data != null && data.getData() != null) { exportSave(data.getData()); return; }
        if (request != PICK_MOD || result != RESULT_OK || data == null || data.getData() == null) return;
        Uri uri = data.getData(); String name = "mod.zip";
        try (Cursor q = getContentResolver().query(uri, new String[]{OpenableColumns.DISPLAY_NAME}, null, null, null)) { if (q != null && q.moveToFirst()) name = q.getString(0); }
        String fname = name.replaceAll("[/\\\\]", "_");
        modStatus = "Copying " + fname + "…"; current = "addmod"; invalidateAll(page);
        new Thread(() -> {
            File copy = new File(getCacheDir(), "import/" + fname); copy.getParentFile().mkdirs();
            try (InputStream in = getContentResolver().openInputStream(uri); OutputStream out = new FileOutputStream(copy)) {
                byte[] buf = new byte[1 << 16]; for (int n; (n = in.read(buf)) > 0; ) out.write(buf, 0, n);
            } catch (IOException | NullPointerException e) { runOnUiThread(() -> { modStatus = "Could not read " + fname + "."; invalidateAll(page); }); return; }
            runOnUiThread(() -> installMod(copy, true));
        }).start();
    }


    /** Installs in the background, then shows the new mod on the Mods page. */
    private void installMod(File f, boolean temporary) {
        if (modBusy) return; modBusy = true;
        modStatus = "Installing " + f.getName() + "…"; invalidateAll(page);
        new Thread(() -> {
            String status; Mods.Mod made = null;
            try { made = Mods.install(f, what -> runOnUiThread(() -> { modStatus = what; invalidateAll(page); })); status = "Installed " + made.name + "."; }
            catch (IOException | RuntimeException e) { status = "Could not install " + f.getName() + ". " + e.getMessage(); }
            finally { if (temporary) f.delete(); modBusy = false; }
            String done = status; Mods.Mod m = made;
            List<Mods.Mod> list = Mods.list();
            runOnUiThread(() -> { mods = list; modStatus = done; if (m != null) { modFocus = m.id; show("mods"); } else invalidateAll(page); });
        }).start();
    }

    /** Y on a mod: up / down then move it; the order is saved as it changes. */
    private boolean moveMod(int by) {
        if (modDetail == null) return false;
        int i = mods.indexOf(modDetail), j = i + by;
        if (i < 0 || j < 0 || j >= mods.size()) return true;
        Collections.swap(mods, i, j); Mods.save(mods); modFocus = modDetail.id;
        show("mods");
        return true;
    }

    // ---- launch ----

    private void launch() {
        if (!ready) return;
        if (Saves.busy()) { launchError = "A save task is still running. Wait for it to finish."; invalidateAll(page); return; }
        launchError = null;
        StringBuilder env = new StringBuilder("# written by the startup screen (assets/settings.tsv)\n");
        for (SettingsTable.Setting s : table.rows) {
            if (s.page() || s.heading() || (s.has('D') && !turnip())) continue;
            env.append(s.key).append('=').append(s.values[index(s)]).append('\n');
        }
        List<Mods.Mod> on = Game.MODS && modsOn() ? Mods.enabled(mods) : new ArrayList<>();
        String iconNote = null;
        // the icons, below the player's mods (theirs win), even with mods off; the icon mod is a 1.4.3 RomFS mod
        if (Game.MODS && value("TOTK_BUTTON_STYLE").equals("playstation")) {
            Mods.Mod icons = Mods.builtIn(Mods.ICONS_PLAYSTATION);
            // the player's files: made here, once. A folder from the PC does not fit them.
            if (picked() && !iconsCurrent(icons)) {
                if (!iconsTried) { makeIcons(); return; }
                // they could not be made: the game starts with the Switch's buttons
                icons = null; iconNote = iconsProblem;
            // made from picks: with prepared data its layout file meets the PC's overlay, and the game stops
            } else if (!picked() && icons != null && !icons.version.isEmpty()) icons = null;
            if (icons != null) on.add(icons);
        }
        if (!on.isEmpty()) {
            // merge the mods first (a few seconds), then start
            if (!Mods.upToDate(on)) {
                if (launchStatus != null) return;
                launchStatus = "Preparing mods…"; invalidateAll(page);
                boolean prep = preparing;
                new Thread(() -> {
                    String failed = null;
                    try { Mods.apply(on, what -> runOnUiThread(() -> { launchStatus = what; invalidateAll(page); })); }
                    catch (IOException | RuntimeException e) { failed = "The mods could not be prepared. " + e.getMessage(); }
                    String f = failed;
                    /* a failure is shown on the Continue row until the next press (launchStatus also keeps a second
                       merge from starting, so it must not stay set: Continue would do nothing until every mod was off)
                       */
                    // a Prepare shaders press stays one after the merge
                    runOnUiThread(() -> { launchStatus = null; launchError = f; invalidateAll(page); if (f == null) { preparing = prep; launch(); preparing = false; } });
                }).start();
                return;
            }
            env.append("TOTK_ROMFS_MODS=").append(Mods.ACTIVE).append('\n');
        }
        // one start: the game translates and builds every shader, then returns here
        if (preparing) env.append("TOTK_PREPARE=1\n");
        env.append("TOTK_CONTROLS=").append(new File(getFilesDir(), "actions.txt")).append('\n');
        env.append("TOTK_REPORTS_DIR=").append(ReportStore.ROOT).append('\n');
        // read by host/app.c: the bundled Turnip, the system's driver, or one from a zip (Drivers.java)
        String driver = driverChoice();
        Drivers.Driver zip = driver.startsWith(Drivers.ZIP) ? Drivers.find(this, driver.substring(Drivers.ZIP.length())) : null;
        env.append("TOTK_GPU_DRIVER=").append(zip != null ? "zip" : driver).append('\n');
        if (zip != null) env.append("DM_VK_DRIVER_DIR=").append(zip.dir).append("\nDM_VK_DRIVER_NAME=").append(zip.library)
            .append("\nDM_VK_DRIVER_EXPECT=any\nDM_VK_DRIVER_REQUIRE=1\nDM_VK_SHADERS=").append(shaderCache(driver)).append('\n');
        try (FileOutputStream out = new FileOutputStream(new File(getFilesDir(), "launch.env"))) {
            out.write(env.toString().getBytes(StandardCharsets.UTF_8));
        } catch (IOException e) { return; }
        launchStatus = null; launchError = iconNote;
        // a crossfade: the default open transition slides this screen off to the left
        startActivity(new Intent(this, GameActivity.class),
            ActivityOptions.makeCustomAnimation(this, android.R.anim.fade_in, android.R.anim.fade_out).toBundle());
    }

    private byte[] iconArt() {
        try (InputStream in = getAssets().open(IconMod.ASSET)) { return in.readAllBytes(); } catch (IOException e) { return null; }
    }

    private boolean iconsCurrent(Mods.Mod icons) {
        try { return IconMod.current(icons, iconArt()); } catch (IOException e) { return false; }
    }

    /** The first start with PlayStation buttons: makes the icon mod on a worker thread, then starts. If that fails the game
        starts all the same, and the reason shows now and on the Continue row after the game. */
    private void makeIcons() {
        if (launchStatus != null) return;
        launchStatus = "Making the PlayStation buttons…"; invalidateAll(page);
        boolean prep = preparing; byte[] art = iconArt();
        new Thread(() -> {
            String failed = Mods.makeIcons(art, what -> runOnUiThread(() -> { launchStatus = what; invalidateAll(page); }));
            runOnUiThread(() -> {
                launchStatus = null; iconsTried = true; iconsProblem = failed;
                if (failed != null) Toast.makeText(this, failed, Toast.LENGTH_LONG).show();
                preparing = prep; launch(); preparing = false; iconsTried = false;
            });
        }).start();
    }

    // ---- input ----

    /** The physical button (ActionLayout.PHYSICAL order) of a key, as host/app.c physical_of_key reads it; -1 for others. */
    private int physicalOf(int key) {
        boolean label = byLabel();
        switch (key) {
            case KeyEvent.KEYCODE_BUTTON_A: return label ? 1 : 0;
            case KeyEvent.KEYCODE_BUTTON_B: return label ? 0 : 1;
            case KeyEvent.KEYCODE_BUTTON_X: return label ? 3 : 2;
            case KeyEvent.KEYCODE_BUTTON_Y: return label ? 2 : 3;
            case KeyEvent.KEYCODE_BUTTON_THUMBL: return 4;
            case KeyEvent.KEYCODE_BUTTON_THUMBR: return 5;
            case KeyEvent.KEYCODE_BUTTON_L1: return 6;
            case KeyEvent.KEYCODE_BUTTON_R1: return 7;
            case KeyEvent.KEYCODE_BUTTON_L2: return 8;
            case KeyEvent.KEYCODE_BUTTON_R2: return 9;
            case KeyEvent.KEYCODE_BUTTON_START: return 10;
            case KeyEvent.KEYCODE_BUTTON_SELECT: return 11;
            default: return -1;
        }
    }

    /** The face button a key is here: the system's (BUTTON_A confirms, BUTTON_B goes back), whatever the game's face-button
        setting, as in the in-game menu (the game's own controls are set apart, on the Controls page): 'A', 'B', 'X', 'Y', or 0. */
    private char switchFace(int key) {
        switch (key) {
            case KeyEvent.KEYCODE_BUTTON_A: return 'A';
            case KeyEvent.KEYCODE_BUTTON_B: return 'B';
            case KeyEvent.KEYCODE_BUTTON_X: return 'X';
            case KeyEvent.KEYCODE_BUTTON_Y: return 'Y';
            default: return 0;
        }
    }

    @Override public boolean dispatchKeyEvent(KeyEvent e) {
        int k = e.getKeyCode(); boolean up = e.getAction() == KeyEvent.ACTION_UP; char face = switchFace(k);
        if (face == 'A' || k == KeyEvent.KEYCODE_DPAD_CENTER || k == KeyEvent.KEYCODE_ENTER) {
            if (up) {
                // touch mode: nothing focused yet
                View f = getCurrentFocus(); if (f == null || !f.hasFocus()) f = preferred;
                if (f != null && f.isEnabled()) { f.requestFocus(); f.performClick(); }
            }
            return true;
        }
        if (face == 'B' || k == KeyEvent.KEYCODE_BACK) {
            if (up) {
                if (current.equals("home")) finish();
                else if (current.equals("controls")) show(controlsBack);
                else if (current.equals("addmod")) show("mods");
                else if (current.equals("drivers")) show("settings");
                else if (current.equals("licences")) show("tools");
                // a task ends in seconds, and says how it went here
                else if (current.equals("saves")) { if (!Saves.busy()) { toolsSaves = true; show("tools"); } }
                else if (current.equals("saveask")) askBack.run();
                else if (current.equals("setup")) {
                    if (setupProgress != null) { setup.cancel(); setupProgress = "Stopping…"; invalidateAll(page); }
                    else if (setupBack.equals("tools")) { toolsSetup = true; show("tools"); }
                    else show("home");
                }
                else if (current.equals("mods") && modMoving) { modMoving = false; invalidateAll(page); }
                else show("home");
            }
            return true;
        }
        if (current.equals("licences") && licenceScroll != null && (k == KeyEvent.KEYCODE_BUTTON_L1 || k == KeyEvent.KEYCODE_BUTTON_R1)) {
            if (up) licenceScroll.pageScroll(k == KeyEvent.KEYCODE_BUTTON_R1 ? View.FOCUS_DOWN : View.FOCUS_UP);
            return true;
        }
        if (current.equals("settings") && (k == KeyEvent.KEYCODE_BUTTON_L1 || k == KeyEvent.KEYCODE_BUTTON_R1)) {
            if (up && !table.tabs.isEmpty()) { int n = table.tabs.size(); tab = (tab + (k == KeyEvent.KEYCODE_BUTTON_L1 ? n - 1 : 1)) % n; show("settings"); }
            return true;
        }
        if (face == 'X') {
            if (up) {
                View f = getCurrentFocus();
                if (current.equals("settings") && f instanceof Row && f.getTag() instanceof SettingsTable.Setting) {
                    SettingsTable.Setting s = (SettingsTable.Setting) f.getTag(); if (!s.page()) set(s, s.def); f.invalidate(); if (detailView != null) detailView.invalidate();
                } else if (current.equals("controls") && f instanceof Row && padRows.contains(f)) { layout.reset(padRows.indexOf(f)); layout.write(); refreshControls(); }
                else if (current.equals("mods") && f instanceof Row && f.getTag() instanceof Mods.Mod) {
                    Mods.Mod m = (Mods.Mod) f.getTag();
                    if (modArmed == m) { Mods.remove(mods, m); modArmed = null; modDetail = null; modFocus = null; modStatus = "Removed " + m.name; show("mods"); }
                    else { modArmed = m; invalidateAll(page); }
                }
                else if (current.equals("drivers") && f instanceof Row && f.getTag() instanceof Drivers.Driver) {
                    Drivers.Driver d = (Drivers.Driver) f.getTag();
                    if (d.id.equals(driverArmed)) {
                        if (driverChoice().equals(Drivers.ZIP + d.id)) prefs.edit().remove("TOTK_GPU_DRIVER").apply();
                        Drivers.remove(this, d); driverArmed = null; driverFocus = null; driverStatus = "Removed " + d.name; show("drivers");
                    } else { driverArmed = d.id; invalidateAll(page); }
                }
                else if (current.equals("tools") && f instanceof Row && f.getTag() instanceof File) {
                    File r = (File) f.getTag();
                    if (reportArmed == r) { ReportStore.delete(r); new File(r.getPath() + ".zip").delete(); reportArmed = null; toolsReport = null; reportStatus = "Deleted " + r.getName(); show("tools"); }
                    else { reportArmed = r; invalidateAll(page); }
                }
            }
            return true;
        }
        if (face == 'Y' && current.equals("mods")) {
            View f = getCurrentFocus();
            if (up && f instanceof Row && f.getTag() instanceof Mods.Mod) { modDetail = (Mods.Mod) f.getTag(); modMoving = !modMoving; invalidateAll(page); }
            return true;
        }
        if (current.equals("mods") && modMoving && (k == KeyEvent.KEYCODE_DPAD_UP || k == KeyEvent.KEYCODE_DPAD_DOWN)) {
            if (e.getAction() == KeyEvent.ACTION_DOWN) moveMod(k == KeyEvent.KEYCODE_DPAD_UP ? -1 : 1);
            return true;
        }
        // a shoulder button or a stick press puts the focused action on it
        if (current.equals("controls")) {
            int p = physicalOf(k);
            if (p >= 4 && p < 10) {
                View f = getCurrentFocus();
                if (up && f instanceof Row && padRows.contains(f)) { int a = padRows.indexOf(f); if (ActionLayout.carries(a, p)) { layout.button[a] = p; layout.write(); refreshControls(); } }
                return true;
            }
        }
        return super.dispatchKeyEvent(e);
    }

    // ---- files ----

    private static int count(File dir, String suffix) {
        String[] names = dir.list(); int n = 0;
        if (names != null) for (String s : names) if (s.endsWith(suffix)) n++;
        return n;
    }

    private static String tail(File f, int bytes) {
        try (RandomAccessFile r = new RandomAccessFile(f, "r")) {
            long n = r.length(), from = Math.max(0, n - bytes);
            byte[] b = new byte[(int) (n - from)]; r.seek(from); r.readFully(b);
            return "Last run (" + f + "):\n" + new String(b, StandardCharsets.UTF_8);
        } catch (IOException e) { return "There is no log from a previous run yet."; }
    }
}
