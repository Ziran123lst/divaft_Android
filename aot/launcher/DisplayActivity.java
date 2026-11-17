package com.divaft.launcher;

import android.app.Activity;
import android.os.Bundle;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.WindowManager;
import android.view.MotionEvent;
import java.io.File;
import java.io.FileOutputStream;

/** Full-screen stable surface for the game frames (translucent window so the
 *  SurfaceView shows through). No competing layout => no surface churn. */
public class DisplayActivity extends Activity {
    @Override protected void onCreate(Bundle b) {
        super.onCreate(b);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        SurfaceView sv = new SurfaceView(this);
        setContentView(sv);
        sv.getHolder().addCallback(new SurfaceHolder.Callback() {
            @Override public void surfaceCreated(SurfaceHolder h) { BbPresent.surface(h.getSurface()); }
            @Override public void surfaceChanged(SurfaceHolder h, int f, int w, int hh) { BbPresent.surface(h.getSurface()); }
            @Override public void surfaceDestroyed(SurfaceHolder h) { BbPresent.surface(null); }
        });
        if (sv.getHolder().getSurface().isValid()) BbPresent.surface(sv.getHolder().getSurface());
    }
    @Override protected void onResume() {
        super.onResume();
        BbPresent.setHold(false);
    }
    @Override protected void onPause() {
        super.onPause();
        // Keep the game running while away: release frames unseen so it never stalls on a display.
        BbPresent.setHold(true);
        BbPresent.surface(null);
    }

    @Override public boolean onTouchEvent(MotionEvent e) {
        int w = getWindow().getDecorView().getWidth(), h = getWindow().getDecorView().getHeight();
        float x = e.getX(), y = e.getY();
        boolean down = e.getActionMasked() == MotionEvent.ACTION_DOWN || e.getActionMasked() == MotionEvent.ACTION_MOVE;
        int btn = 0;
        if (down) {
            boolean left = x < w / 2f, top = y < h / 2f;
            btn = left ? (top ? 0x8000 : 0x4000) : (top ? 0x1000 : 0x2000);
        }
        int tx = (int) (x / Math.max(1, w) * 1920), ty = (int) (y / Math.max(1, h) * 1080);
        try (FileOutputStream os = new FileOutputStream(new File(getFilesDir(), "touch.state"))) {
            os.write((btn + " " + tx + " " + ty + " " + (down ? 1 : 0) + "\n").getBytes());
        } catch (Exception ignored) {}
        return true;
    }
}
