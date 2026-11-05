package dev.tok.totk;

import android.app.NativeActivity;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.os.BatteryManager;
import android.os.Build;
import android.os.PowerManager;
import android.provider.Settings;
import android.os.VibrationEffect;
import android.os.Vibrator;
import android.os.VibratorManager;
import android.view.InputDevice;
import android.os.Bundle;
import android.view.View;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.view.WindowManager;
import java.nio.charset.StandardCharsets;
import java.util.concurrent.CountDownLatch;

/** The game: libtotk.so runs in this NativeActivity, fullscreen and with the screen kept on. */
public class GameActivity extends NativeActivity {
    private static Vibrator vibrator;
    private static GameActivity self;

    /** Called from native code (app.c, for a bug report): the device, its fan and performance modes, battery and
        thermal state. */
    public static String deviceInfo() {
        GameActivity a = self;
        StringBuilder s = new StringBuilder("Device: " + Build.MANUFACTURER + " " + Build.MODEL + ", Android " + Build.VERSION.RELEASE + "\n");
        if (a == null) return s.toString();
        s.append("Fan mode: ").append(Settings.System.getString(a.getContentResolver(), "fan_mode"))
         .append(", performance mode: ").append(Settings.System.getString(a.getContentResolver(), "performance_mode")).append('\n');
        Intent b = a.registerReceiver(null, new IntentFilter(Intent.ACTION_BATTERY_CHANGED));
        if (b != null) {
            int level = b.getIntExtra(BatteryManager.EXTRA_LEVEL, -1), scale = b.getIntExtra(BatteryManager.EXTRA_SCALE, 100);
            s.append("Battery: ").append(scale > 0 ? level * 100 / scale : level).append("%, ").append(b.getIntExtra(BatteryManager.EXTRA_PLUGGED, 0) != 0 ? "plugged in" : "on battery")
             .append(", ").append(b.getIntExtra(BatteryManager.EXTRA_TEMPERATURE, 0) / 10.0).append(" °C\n");
        }
        PowerManager pm = a.getSystemService(PowerManager.class);
        if (pm != null) s.append("Thermal status: ").append(pm.getCurrentThermalStatus()).append(" (0 none .. 6 shutdown)\n");
        return s.toString();
    }

    /** Called from native code (app.c, twice a second): the screen's rotation from the device's natural orientation
        (Surface.ROTATION_*), which the motion sensors' axes follow (the Odin 3's natural orientation is portrait). */
    public static int displayRotation() {
        GameActivity a = self;
        android.view.Display d = a != null ? a.getDisplay() : null;
        return d != null ? d.getRotation() : -1;
    }

    /** Called from native code (app.c rumble thread): the game's motor strength 0..1; 0 stops. */
    public static void rumble(float strength) {
        Vibrator v = vibrator;
        if (v == null) return;
        if (strength <= 0.01f) { v.cancel(); return; }
        int amplitude = Math.max(1, Math.min(255, Math.round(strength * 255f)));
        v.vibrate(VibrationEffect.createOneShot(40, v.hasAmplitudeControl() ? amplitude : VibrationEffect.DEFAULT_AMPLITUDE));
    }

    /** The open TextEntry (UI thread), dismissed in onDestroy. */
    private static android.app.Dialog textDialog;

    /** Called from native code (app.c, the game's software keyboard): shows TextEntry and blocks the calling thread until
        the player confirms (the text, UTF-8) or cancels (null). The game's thread waits in this call, so every way out
        answers: the dialog's dismissal (confirm, cancel, or onDestroy when the activity goes away, e.g. recreated by a
        configuration change, whose windows would otherwise be removed without a dismissal), and a failure to show it. */
    public static byte[] textInput(byte[] prompt, byte[] initial, int maxUnits, int minUnits, boolean secure, boolean digitsOnly, int disabled) {
        GameActivity a = self;
        if (a == null || a.isFinishing()) return null;
        String[] text = {null};
        CountDownLatch done = new CountDownLatch(1);
        a.runOnUiThread(() -> {
            if (a.isFinishing() || a.isDestroyed()) { done.countDown(); return; }
            try {
                textDialog = TextEntry.show(a, new String(prompt, StandardCharsets.UTF_8), new String(initial, StandardCharsets.UTF_8), maxUnits, minUnits, secure, digitsOnly, disabled,
                                            t -> { textDialog = null; text[0] = t; done.countDown(); });
            } catch (RuntimeException e) { textDialog = null; text[0] = null; done.countDown(); }
        });
        try { done.await(); } catch (InterruptedException e) { return null; }
        return text[0] != null ? text[0].getBytes(StandardCharsets.UTF_8) : null;
    }

    /** The controller's own motor when it has one, else the device's (the Odin 3's motor sits in the body). */
    private void findVibrator() {
        for (int id : InputDevice.getDeviceIds()) {
            InputDevice d = InputDevice.getDevice(id);
            if (d == null || (d.getSources() & InputDevice.SOURCE_GAMEPAD) != InputDevice.SOURCE_GAMEPAD) continue;
            VibratorManager m = d.getVibratorManager();
            int[] ids = m.getVibratorIds();
            if (ids.length > 0) { vibrator = m.getVibrator(ids[0]); return; }
        }
        Vibrator v = ((VibratorManager) getSystemService(Context.VIBRATOR_MANAGER_SERVICE)).getDefaultVibrator();
        vibrator = v.hasVibrator() ? v : null;
    }
    /** This process holds the game's descriptors: an activity made again keeps them. */
    private static boolean filesOpen;

    @Override protected void onCreate(Bundle state) {
        /* Before super.onCreate, which loads libtotk.so and starts the game. It cannot be skipped, so with a file gone the
           library sees TOTK_SETUP_STOP and does nothing, and the launcher behind this activity shows why on the Game files page. */
        boolean start = !Game.SETUP || filesOpen || SetupFiles.openForGame(this);
        filesOpen = start;
        super.onCreate(state);
        if (!start) { finish(); return; }
        self = this;
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        getWindow().getAttributes().layoutInDisplayCutoutMode = WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_ALWAYS;
        hideBars();
        findVibrator();
        /* Quit to the startup screen crossfades like the start does (LauncherActivity.launch), instead of sliding */
        if (Build.VERSION.SDK_INT >= 34) overrideActivityTransition(OVERRIDE_TRANSITION_CLOSE, android.R.anim.fade_in, android.R.anim.fade_out);
    }
    @SuppressWarnings("deprecation")
    /* ANativeActivity_finish (Quit) calls this */
    @Override public void finish() {
        super.finish();
        if (Build.VERSION.SDK_INT < 34) overridePendingTransition(android.R.anim.fade_in, android.R.anim.fade_out);
    }
    /** Finishing (Quit to startup screen) ends the :game process too: the game has no way back to its start, and a process
        left behind would be resumed by the next Continue, still paused and with the old settings. */
    @Override protected void onDestroy() {
        if (textDialog != null) textDialog.dismiss();
        boolean finishing = isFinishing();
        super.onDestroy();
        if (finishing) android.os.Process.killProcess(android.os.Process.myPid());
    }
    @Override public void onWindowFocusChanged(boolean focused) {
        super.onWindowFocusChanged(focused);
        if (focused) hideBars();
    }
    private void hideBars() {
        View decor = getWindow().getDecorView();
        WindowInsetsController c = decor.getWindowInsetsController();
        if (c == null) return;
        c.hide(WindowInsets.Type.systemBars());
        c.setSystemBarsBehavior(WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
    }
}
