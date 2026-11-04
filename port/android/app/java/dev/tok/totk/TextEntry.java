package dev.tok.totk;

import android.app.Activity;
import android.app.Dialog;
import android.graphics.Canvas;
import android.graphics.ColorFilter;
import android.graphics.PixelFormat;
import android.graphics.drawable.ColorDrawable;
import android.graphics.drawable.Drawable;
import android.graphics.drawable.GradientDrawable;
import android.graphics.drawable.StateListDrawable;
import android.text.Editable;
import android.text.InputFilter;
import android.text.InputType;
import android.text.TextWatcher;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.KeyEvent;
import android.view.View;
import android.view.Window;
import android.view.WindowInsets;
import android.view.WindowManager;
import android.view.inputmethod.EditorInfo;
import android.view.inputmethod.InputMethodManager;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.TextView;
import java.util.function.Consumer;

/** The game's software keyboard (nn::swkbd, TOTK: naming a horse): a dialog with a text field and the system's on-screen
    keyboard, touch operated as the Switch's keyboard applet is; a controller moves between the field and the buttons
    (A selects, B cancels). The game's limits apply as the player types: at most maxUnits UTF-16 units, at least minUnits
    to confirm, digits only, and the applet's disabled keys (bit 1 space, 2 '@', 3 '%', 4 '/', 5 '\', 6 digits);
    runtime/swkbd_host.c applies them again to what comes back. */
final class TextEntry {
    /** Whether the applet would let this character through (swkbd_host.c allowed()). */
    static boolean allowed(char c, int disabled, boolean digitsOnly) {
        boolean digit = c >= '0' && c <= '9';
        if (digitsOnly && !digit) return false;
        if (c < 0x20 || c == 0x7f) return false;
        if (((disabled & 1 << 1) != 0 && c == ' ') || ((disabled & 1 << 2) != 0 && c == '@') || ((disabled & 1 << 3) != 0 && c == '%')) return false;
        return !(((disabled & 1 << 4) != 0 && c == '/') || ((disabled & 1 << 5) != 0 && c == '\\') || ((disabled & 1 << 6) != 0 && digit));
    }

    /** Shows the dialog (UI thread) and returns it; done gets the text, or null when the player cancels or the dialog is
        dismissed for any other reason (GameActivity.onDestroy dismisses it). */
    static Dialog show(Activity a, String prompt, String initial, int maxUnits, int minUnits, boolean secure, boolean digitsOnly, int disabled, Consumer<String> done) {
        Ui.init(a);
        Dialog d = new Dialog(a);
        d.requestWindowFeature(Window.FEATURE_NO_TITLE);
        boolean[] answered = {false};
        Consumer<String> answer = text -> { if (!answered[0]) { answered[0] = true; done.accept(text); } };

        LinearLayout box = new LinearLayout(a);
        box.setOrientation(LinearLayout.VERTICAL);
        box.setPadding(Ui.ui(28), Ui.ui(22), Ui.ui(28), Ui.ui(20));
        box.setBackground(new PanelDrawable());

        TextView title = new TextView(a);
        title.setText(prompt);
        title.setTypeface(Ui.title); title.setTextColor(Ui.GOLD_TEXT); title.setTextSize(TypedValue.COMPLEX_UNIT_PX, Ui.u(22));
        box.addView(title);

        EditText field = new EditText(a);
        field.setTypeface(Ui.medium); field.setTextColor(Ui.CREAM); field.setTextSize(TypedValue.COMPLEX_UNIT_PX, Ui.u(20));
        field.setHighlightColor(Ui.GOLD_FRAME); field.setSingleLine(true);
        field.setPadding(Ui.ui(14), Ui.ui(10), Ui.ui(14), Ui.ui(10));
        field.setBackground(frame(0x14EEE8D8, Ui.LINE, Ui.GOLD_FRAME));
        field.setInputType(digitsOnly ? InputType.TYPE_CLASS_NUMBER | (secure ? InputType.TYPE_NUMBER_VARIATION_PASSWORD : 0)
                           : InputType.TYPE_CLASS_TEXT | (secure ? InputType.TYPE_TEXT_VARIATION_PASSWORD : InputType.TYPE_TEXT_FLAG_CAP_WORDS));
        /* the field stays in the dialog: no full-screen extract editor over the game in landscape */
        field.setImeOptions(EditorInfo.IME_ACTION_DONE | EditorInfo.IME_FLAG_NO_EXTRACT_UI | EditorInfo.IME_FLAG_NO_FULLSCREEN);
        field.setFilters(new InputFilter[] {
            (src, start, end, dst, dstart, dend) -> {
                StringBuilder kept = null;
                for (int i = start; i < end; i++) {
                    char c = src.charAt(i);
                    if (allowed(c, disabled, digitsOnly)) { if (kept != null) kept.append(c); }
                    else if (kept == null) kept = new StringBuilder(src.subSequence(start, i));
                }
                return kept;   /* null keeps the source as it is */
            },
            new InputFilter.LengthFilter(Math.max(1, maxUnits)) });
        field.setText(initial);
        LinearLayout.LayoutParams fieldAt = new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT);
        fieldAt.topMargin = Ui.ui(16);
        box.addView(field, fieldAt);

        TextView count = new TextView(a);
        count.setTypeface(Ui.body); count.setTextColor(Ui.MUTED); count.setTextSize(TypedValue.COMPLEX_UNIT_PX, Ui.u(13));
        count.setGravity(Gravity.END);
        LinearLayout.LayoutParams countAt = new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT);
        countAt.topMargin = Ui.ui(6);
        box.addView(count, countAt);

        LinearLayout buttons = new LinearLayout(a);
        buttons.setGravity(Gravity.END);
        TextView cancel = button(a, a.getString(android.R.string.cancel), false), ok = button(a, a.getString(android.R.string.ok), true);
        LinearLayout.LayoutParams gap = new LinearLayout.LayoutParams(LinearLayout.LayoutParams.WRAP_CONTENT, LinearLayout.LayoutParams.WRAP_CONTENT);
        gap.leftMargin = Ui.ui(12);
        buttons.addView(cancel); buttons.addView(ok, gap);
        LinearLayout.LayoutParams buttonsAt = new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT);
        buttonsAt.topMargin = Ui.ui(14);
        box.addView(buttons, buttonsAt);

        Runnable update = () -> {
            int n = field.length();
            count.setText(n + " / " + maxUnits);
            ok.setEnabled(n >= minUnits); ok.setAlpha(n >= minUnits ? 1f : 0.4f);
        };
        field.addTextChangedListener(new TextWatcher() {
            @Override public void beforeTextChanged(CharSequence s, int start, int count, int after) {}
            @Override public void onTextChanged(CharSequence s, int start, int before, int count) {}
            @Override public void afterTextChanged(Editable s) { update.run(); }
        });
        update.run();

        Runnable confirm = () -> { if (field.length() >= minUnits) { answer.accept(field.getText().toString()); d.dismiss(); } };
        ok.setOnClickListener(v -> confirm.run());
        cancel.setOnClickListener(v -> d.cancel());
        field.setOnEditorActionListener((v, action, event) -> { if (action == EditorInfo.IME_ACTION_DONE) { confirm.run(); return true; } return false; });
        d.setOnDismissListener(x -> answer.accept(null));   /* cancel, Back, B, or the activity finishing */
        /* B cancels at once: left to its fallback (Back), the first press only hid the on-screen keyboard */
        d.setOnKeyListener((x, code, event) -> {
            if (code != KeyEvent.KEYCODE_BUTTON_B) return false;
            if (event.getAction() == KeyEvent.ACTION_UP) d.cancel();
            return true;
        });

        d.setContentView(box);
        d.setCanceledOnTouchOutside(false);
        Window w = d.getWindow();
        if (w != null) {
            w.setBackgroundDrawable(new ColorDrawable(0));
            /* near the top, so the keyboard below leaves it and some of the game in view */
            WindowManager.LayoutParams p = w.getAttributes();
            p.width = Math.min(Ui.ui(560), a.getResources().getDisplayMetrics().widthPixels - Ui.ui(32));
            p.gravity = Gravity.TOP | Gravity.CENTER_HORIZONTAL; p.y = Ui.ui(40);
            p.layoutInDisplayCutoutMode = WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_ALWAYS;
            w.setAttributes(p);
            w.setSoftInputMode(WindowManager.LayoutParams.SOFT_INPUT_STATE_ALWAYS_VISIBLE | WindowManager.LayoutParams.SOFT_INPUT_ADJUST_NOTHING);
        }
        d.setOnShowListener(x -> {
            if (w != null && w.getInsetsController() != null) w.getInsetsController().hide(WindowInsets.Type.systemBars());
            field.requestFocus(); field.selectAll();
            InputMethodManager im = a.getSystemService(InputMethodManager.class);
            if (im != null) im.showSoftInput(field, InputMethodManager.SHOW_IMPLICIT);
        });
        d.show();
        return d;
    }

    /** A button: gold text in a hairline frame; OK filled faintly gold. Focus (controller) and press light the frame. */
    private static TextView button(Activity a, String label, boolean primary) {
        TextView b = new TextView(a);
        b.setText(label);
        b.setTypeface(Ui.semibold); b.setTextColor(primary ? Ui.GOLD_TEXT : Ui.SOFT); b.setTextSize(TypedValue.COMPLEX_UNIT_PX, Ui.u(15));
        b.setGravity(Gravity.CENTER); b.setMinWidth(Ui.ui(96));
        b.setPadding(Ui.ui(18), Ui.ui(9), Ui.ui(18), Ui.ui(9));
        b.setFocusable(true); b.setClickable(true);
        b.setBackground(frame(primary ? Ui.GOLD_FILL : 0, Ui.LINE, Ui.GOLD));
        return b;
    }

    /** A rounded frame: fill and hairline, the line gold (lit) when focused or pressed. */
    private static Drawable frame(int fill, int line, int lit) {
        StateListDrawable s = new StateListDrawable();
        s.addState(new int[] {android.R.attr.state_pressed}, rounded(fill, lit));
        s.addState(new int[] {android.R.attr.state_focused}, rounded(fill, lit));
        s.addState(new int[] {}, rounded(fill, line));
        return s;
    }

    private static GradientDrawable rounded(int fill, int line) {
        GradientDrawable g = new GradientDrawable();
        g.setColor(fill); g.setCornerRadius(Ui.u(4)); g.setStroke(Math.max(1, Ui.ui(1)), line);
        return g;
    }

    /** The startup screen's panel (Ui.panel): dark fill, hairline and gold corner brackets. */
    private static final class PanelDrawable extends Drawable {
        @Override public void draw(Canvas c) {
            float e = Ui.u(2);
            Ui.panel(c, getBounds().left + e, getBounds().top + e, getBounds().right - e, getBounds().bottom - e);
        }
        @Override public void setAlpha(int alpha) {}
        @Override public void setColorFilter(ColorFilter filter) {}
        @SuppressWarnings("deprecation") @Override public int getOpacity() { return PixelFormat.TRANSLUCENT; }
    }

    private TextEntry() {}
}
