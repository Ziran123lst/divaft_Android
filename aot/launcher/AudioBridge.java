package com.divaft.launcher;

import android.media.AudioAttributes;
import android.media.AudioFormat;
import android.media.AudioTrack;
import android.system.Os;
import java.io.File;
import java.io.FileInputStream;

/** Reads the runtime's mixed PCM FIFO (s16 stereo 48 kHz) and plays it via AudioTrack. */
public final class AudioBridge {
    private static Thread thread;
    private static AudioTrack track;

    public static String start(File fifo) {
        try {
            if (!fifo.exists()) Os.mkfifo(fifo.getPath(), 0600);
        } catch (Exception e) { return "mkfifo failed: " + e; }
        int min = AudioTrack.getMinBufferSize(48000, AudioFormat.CHANNEL_OUT_STEREO, AudioFormat.ENCODING_PCM_16BIT);
        track = new AudioTrack(new AudioAttributes.Builder()
                .setUsage(AudioAttributes.USAGE_GAME)
                .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC).build(),
            new AudioFormat.Builder().setSampleRate(48000)
                .setChannelMask(AudioFormat.CHANNEL_OUT_STEREO)
                .setEncoding(AudioFormat.ENCODING_PCM_16BIT).build(),
            Math.max(min, 16384), AudioTrack.MODE_STREAM, 0);
        track.play();
        thread = new Thread(() -> {
            byte[] buf = new byte[8192];
            try (FileInputStream in = new FileInputStream(fifo)) {
                int n;
                while ((n = in.read(buf)) > 0) {
                    int off = 0;
                    while (off < n) {
                        int w = track.write(buf, off, n - off);
                        if (w <= 0) break;
                        off += w;
                    }
                }
            } catch (Exception ignored) {}
        }, "diva-audio");
        thread.setDaemon(true);
        thread.start();
        return "audio fifo " + fifo.getPath();
    }

    public static void stop() {
        if (track != null) { try { track.stop(); track.release(); } catch (Exception ignored) {} track = null; }
    }
}
