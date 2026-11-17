package com.divaft.launcher;

import android.app.Activity;
import android.os.Bundle;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.WindowManager;

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
}
