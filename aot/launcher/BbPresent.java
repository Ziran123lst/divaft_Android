package com.divaft.launcher;

import android.view.Surface;
import android.view.SurfaceHolder;
import java.io.File;

public final class BbPresent {
    private static boolean loaded;
    private static native String nativeStart(String socketPath, String diagPath);
    private static native void nativeSetSurface(Surface surface);
    private static native long nativeFrames();
    private static native void nativeSetHold(boolean hold);
    private static native void nativeShutdown();

    /** Load libbbpresent from an override copy if present, else the bundled one. */
    public static synchronized void init(String overridePath) {
        if (loaded) return;
        if (overridePath != null && new File(overridePath).exists()) {
            System.load(overridePath);
        } else {
            System.loadLibrary("bbpresent");
        }
        loaded = true;
    }

    private static SurfaceHolder.Callback callback;
    private static SurfaceHolder attached;

    public static synchronized String start(SurfaceHolder holder, String socketPath, String diagPath) {
        init(null);
        String result = nativeStart(socketPath, diagPath);
        used = true;
        if (!result.equals("present running")) return result;
        if (attached != null && callback != null) attached.removeCallback(callback);
        attached = holder;
        callback = new SurfaceHolder.Callback() {
            @Override public void surfaceCreated(SurfaceHolder h) { setSurface(h.getSurface()); }
            @Override public void surfaceChanged(SurfaceHolder h, int format, int w, int height) { setSurface(h.getSurface()); }
            @Override public void surfaceDestroyed(SurfaceHolder h) { setSurface(null); }
        };
        holder.addCallback(callback);
        setSurface(holder.getSurface().isValid() ? holder.getSurface() : null);
        return "bridge running: bionic present on " + socketPath;
    }

    private static synchronized void setSurface(Surface surface) { nativeSetSurface(surface); }
    /** Listen only (no surface yet); the DisplayActivity attaches the surface later. */
    public static synchronized String listen(String socketPath, String diagPath) {
        init(null);
        String r = nativeStart(socketPath, diagPath);
        used = true;
        return r;
    }
    /** Called by DisplayActivity when its full-screen surface is ready/lost. */
    public static synchronized void surface(Surface s) { if (used) nativeSetSurface(s); }
    public static synchronized void setHold(boolean hold) { if (used) nativeSetHold(hold); }
    public static long frames() { return nativeFrames(); }
    private static boolean used;
    public static long framesIfLoaded() { return used ? nativeFrames() : 0; }
    public static synchronized void stop() { nativeShutdown(); }
}
