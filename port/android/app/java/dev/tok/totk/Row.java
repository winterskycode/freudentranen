package dev.tok.totk;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Rect;
import android.view.KeyEvent;
import android.view.View;

/** A focusable row of a page, drawn by its page (painter), with the canvas's selection when focused. Left / right go to
    the stepper (a setting's value) instead of moving the focus. */
final class Row extends View {
    interface Painter { void paint(Canvas c, Row row, boolean focused); }
    interface Stepper { void step(int by); }
    interface Focus { void focused(Row row); }

    final Painter painter;
    Stepper stepper;
    Focus onFocus;
    Row loopUp, loopDown;   // the first row of a list goes up to its last, and the last down to the first (LauncherActivity.linkLoops)
    boolean preferred;   // shown selected while the screen is in touch mode (nothing has focus there)

    Row(Context c, Painter painter) {
        super(c);
        this.painter = painter;
        setFocusable(true); setFocusableInTouchMode(false); setClickable(true);
        setDefaultFocusHighlightEnabled(false);
    }

    @Override protected void onDraw(Canvas c) {
        boolean on = isFocused() || isPressed() || (preferred && isInTouchMode());
        if (on) Ui.selection(c, 0, 0, getWidth(), getHeight());
        painter.paint(c, this, on);
    }

    @Override protected void onFocusChanged(boolean gained, int direction, Rect previous) {
        super.onFocusChanged(gained, direction, previous);
        if (gained && onFocus != null) onFocus.focused(this);
        invalidate();
    }

    @Override protected void drawableStateChanged() { super.drawableStateChanged(); invalidate(); }

    @Override public boolean onKeyDown(int code, KeyEvent e) {
        Row to = code == KeyEvent.KEYCODE_DPAD_UP ? loopUp : code == KeyEvent.KEYCODE_DPAD_DOWN ? loopDown : null;
        if (to != null) { if (e.getRepeatCount() == 0) to.requestFocus(); return true; }   // a fresh press wraps; held, the list stops at its end
        if (stepper != null && (code == KeyEvent.KEYCODE_DPAD_LEFT || code == KeyEvent.KEYCODE_DPAD_RIGHT)) {
            stepper.step(code == KeyEvent.KEYCODE_DPAD_LEFT ? -1 : 1); invalidate(); return true;
        }
        return super.onKeyDown(code, e);
    }
}
