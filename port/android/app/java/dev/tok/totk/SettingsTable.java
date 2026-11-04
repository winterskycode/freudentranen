package dev.tok.totk;

import android.content.Context;
import java.io.BufferedReader;
import java.io.IOException;
import java.io.InputStreamReader;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;

/** assets/settings.tsv: the settings of the startup screen and of the in-game menu (vk/overlay.cpp reads the same file).
    The file's header explains the columns and flags. */
final class SettingsTable {
    static final class Setting {
        final String key, tab, flags, label, help, needsKey; final String[] values, names, needsValues; final int def;
        Setting(String[] f) {
            key = f[0]; tab = f[1]; flags = f[2]; label = f[6]; help = f[7];
            boolean none = page() || heading();
            values = none ? new String[0] : f[4].split("\\|", -1); names = none ? new String[0] : f[5].split("\\|", -1);
            int eq = f.length > 8 ? f[8].indexOf('=') : -1;
            needsKey = eq > 0 ? f[8].substring(0, eq) : null; needsValues = eq > 0 ? f[8].substring(eq + 1).split("\\|", -1) : new String[0];
            int d = 0; for (int i = 0; i < values.length; i++) if (values[i].equals(f[3])) d = i;
            def = d;
        }
        boolean has(char flag) { return flags.indexOf(flag) >= 0; }
        boolean live() { return has('L'); }
        boolean page() { return has('P'); }
        boolean heading() { return has('H'); }
        int index(String value) { for (int i = 0; i < values.length; i++) if (values[i].equals(value)) return i; return -1; }
    }

    final List<Setting> rows = new ArrayList<>();
    final List<String> tabs = new ArrayList<>();

    SettingsTable(Context c) {
        try (BufferedReader r = new BufferedReader(new InputStreamReader(c.getAssets().open("settings.tsv"), StandardCharsets.UTF_8))) {
            for (String line; (line = r.readLine()) != null; ) {
                if (line.isEmpty() || line.startsWith("#")) continue;
                String[] f = line.split("\t", -1);
                if (f.length < 8) continue;
                Setting s = new Setting(f);
                if (!s.page() && !s.heading() && (s.values.length == 0 || s.values.length != s.names.length)) continue;
                rows.add(s);
                if (!tabs.contains(s.tab)) tabs.add(s.tab);
            }
        } catch (IOException ignored) {
            // an APK without the table: no settings rows
        }
    }

    /** Do other rows show or hide with this setting? */
    boolean governs(Setting s) { for (Setting r : rows) if (s.key.equals(r.needsKey)) return true; return false; }

    Setting find(String key) { for (Setting s : rows) if (s.key.equals(key)) return s; return null; }
}
