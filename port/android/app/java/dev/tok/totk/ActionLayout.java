package dev.tok.totk;

import android.content.Context;
import java.io.BufferedReader;
import java.io.File;
import java.io.FileOutputStream;
import java.io.FileReader;
import java.io.IOException;
import java.io.InputStreamReader;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;

/** The field controls (files/actions.txt, TOTK_CONTROLS): "action = physical button" lines for the actions moved from
    where the game puts them, the format host/app.c reads (actions_parse) and the in-game menu writes. The actions are
    runtime/game_actions.h's, in its order; menus keep confirm and back on the Switch layout's A and B. */
final class ActionLayout {
    static final String[] PHYSICAL = {"south", "east", "west", "north", "l3", "r3", "l1", "r1", "l2", "r2", "menu", "view",
        "left", "up", "right", "down", "touchpad"};
    /* The actions and buttons as assets/actions.tsv lists them (the in-game menu shows the same table; load first) */
    static String[] NAMES, LABELS, HELP;
    static int ACTIONS;
    private static int[] GAME_BUTTON;   // the Switch button the game gives each action (controller_switch_names index)
    private static boolean[] HELD;      // read while held (the game reads no held stick clicks)
    static int[] CARRY;                 // the buttons an action can go on, in the order left / right steps
    private static int[] SWITCH_PHYSICAL;   // the physical button pressing the Switch layout's A, B, X, Y
    private static String[] SHORT, LABEL_SHORT;   // physical buttons as shown: by position, face buttons by printed label

    /** Reads assets/actions.tsv once; a table that does not hold together stops the startup screen with the reason. */
    static synchronized void load(Context c) {
        if (NAMES != null) return;
        List<String[]> actions = new ArrayList<>(); int[] carry = null, sw = null;
        String[] shortName = new String[PHYSICAL.length], labelShort = new String[4];
        try (BufferedReader r = new BufferedReader(new InputStreamReader(c.getAssets().open("actions.tsv"), StandardCharsets.UTF_8))) {
            for (String line; (line = r.readLine()) != null; ) {
                if (line.isEmpty() || line.startsWith("#")) continue;
                String[] f = line.split("\t");
                switch (f[0]) {
                    case "action": if (f.length < 6) throw new IllegalStateException("assets/actions.tsv: " + line); actions.add(f); break;
                    case "carry": carry = ints(f[1]); break;
                    case "switch": sw = ints(f[1]); break;
                    case "button": { int i = Integer.parseInt(f[1]); shortName[i] = f[2]; if (i < 4 && f.length > 3) labelShort[i] = f[3]; break; }
                    default: throw new IllegalStateException("assets/actions.tsv: " + line);
                }
            }
        } catch (IOException | RuntimeException e) { throw new IllegalStateException("assets/actions.tsv: " + e.getMessage(), e); }
        if (carry == null || sw == null || sw.length != 4) throw new IllegalStateException("assets/actions.tsv: no carry / switch row");
        int n = actions.size(); String[] names = new String[n], labels = new String[n], help = new String[n]; int[] game = new int[n]; boolean[] held = new boolean[n];
        for (int a = 0; a < n; a++) { String[] f = actions.get(a); names[a] = f[1]; labels[a] = f[2]; game[a] = Integer.parseInt(f[3]); held[a] = f[4].equals("1"); help[a] = f[5]; }
        LABELS = labels; HELP = help; GAME_BUTTON = game; HELD = held; CARRY = carry; SWITCH_PHYSICAL = sw; SHORT = shortName; LABEL_SHORT = labelShort;
        ACTIONS = n; NAMES = names;
    }
    private static int[] ints(String s) {
        String[] p = s.trim().split(" +"); int[] v = new int[p.length];
        for (int i = 0; i < p.length; i++) v[i] = Integer.parseInt(p[i]);
        return v;
    }

    final int[] button = new int[ACTIONS];   // physical button (PHYSICAL index), -1 = none (ACTIONS: load() first)
    private final File file;

    ActionLayout(File file) { this.file = file; reset(); read(); }

    /** The physical button of the Switch layout (east A, south B, north X, west Y, the rest by index) */
    static int defaultButton(int a) { int b = GAME_BUTTON[a]; return b < 4 ? SWITCH_PHYSICAL[b] : b; }

    void reset() { for (int a = 0; a < ACTIONS; a++) button[a] = defaultButton(a); }
    void reset(int a) { button[a] = defaultButton(a); }
    boolean changed(int a) { return button[a] != defaultButton(a); }
    int changes() { int n = 0; for (int a = 0; a < ACTIONS; a++) if (changed(a)) n++; return n; }
    /** Whether action a can go on physical button p */
    static boolean carries(int a, int p) {
        if (p < 0) return true;
        boolean ok = false; for (int c : CARRY) ok |= c == p;
        return ok && !(HELD[a] && (p == 4 || p == 5));
    }

    private static int index(String[] names, String s) {
        for (int i = 0; i < names.length; i++) if (names[i].equalsIgnoreCase(s)) return i;
        return -2;
    }

    /** A file the game would reject (any line not understood) leaves the game's own controls, as host/app.c does. */
    private void read() {
        if (!file.exists()) return;
        try (BufferedReader r = new BufferedReader(new FileReader(file))) {
            int[] got = button.clone();
            for (String line; (line = r.readLine()) != null; ) {
                int hash = line.indexOf('#'); if (hash >= 0) line = line.substring(0, hash);
                if (line.trim().isEmpty()) continue;
                int eq = line.indexOf('='); if (eq < 0) return;
                int a = index(NAMES, line.substring(0, eq).trim()); String v = line.substring(eq + 1).trim();
                int p = v.equalsIgnoreCase("none") ? -1 : index(PHYSICAL, v);
                if (a < 0 || p == -2) return;
                got[a] = carries(a, p) ? p : defaultButton(a);
            }
            System.arraycopy(got, 0, button, 0, ACTIONS);
        } catch (IOException ignored) {
            // unreadable: the game's own
        }
    }

    void write() {
        StringBuilder s = new StringBuilder("# field controls: action = physical button (game_actions.h); actions not listed are where the game puts them\n");
        for (int a = 0; a < ACTIONS; a++)
            if (changed(a)) s.append(NAMES[a]).append(" = ").append(button[a] < 0 ? "none" : PHYSICAL[button[a]]).append('\n');
        File tmp = new File(file.getPath() + ".tmp");
        try (FileOutputStream out = new FileOutputStream(tmp)) { out.write(s.toString().getBytes(StandardCharsets.UTF_8)); }
        catch (IOException e) { return; }
        if (!tmp.renameTo(file)) tmp.delete();
    }

    /** Left / right on a row: the buttons that can carry the action, then none. */
    void step(int a, int by) {
        int n = CARRY.length + 1, cur = n - 1;
        for (int k = 0; k < CARRY.length; k++) if (CARRY[k] == button[a]) cur = k;
        for (int tries = 0; tries < n; tries++) {
            cur = ((cur + by) % n + n) % n;
            int p = cur == n - 1 ? -1 : CARRY[cur];
            if (carries(a, p)) { button[a] = p; return; }
        }
    }

    /** A physical button as this device shows it: face buttons by printed letter (an Xbox-layout device such as the Odin
        3) or by position */
    static String buttonName(int p, boolean byLabel) { return p < 0 ? "Nothing" : p < 4 && byLabel ? LABEL_SHORT[p] : SHORT[p]; }
}
