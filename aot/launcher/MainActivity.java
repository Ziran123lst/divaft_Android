package com.divaft.launcher;

import android.app.Activity;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.provider.Settings;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import android.view.SurfaceView;
import android.os.Handler;
import android.os.Looper;
import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.io.BufferedReader;
import java.io.InputStreamReader;

public class MainActivity extends Activity {
    private TextView log;
    private TextView status;
    private ScrollView scroll;
    private SurfaceView surfaceView;
    private final Handler handler = new Handler(Looper.getMainLooper());
    private String gamePath = null;
    private File dataDir;
    private static final int REQ_GAME = 41;
    private static final int REQ_DATA = 42;
    private Process proc;

    @Override protected void onCreate(Bundle b) {
        super.onCreate(b);
        dataDir = new File(Environment.getExternalStoragePublicDirectory(
            Environment.DIRECTORY_DOWNLOADS), "diva-data");

        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setPadding(24, 24, 24, 24);

        Button perm = new Button(this);
        perm.setText("1 授权存储");
        perm.setOnClickListener(v -> requestStorage());
        Button pickData = new Button(this);
        pickData.setText("2 设置数据目录(默认Download/diva-data)");
        pickData.setOnClickListener(v -> pick(REQ_DATA));
        Button pick = new Button(this);
        pick.setText("3 选择游戏文件夹");
        pick.setOnClickListener(v -> pick(REQ_GAME));
        Button start = new Button(this);
        start.setText("4 开始游戏");
        start.setOnClickListener(v -> startGame());
        Button stop = new Button(this);
        stop.setText("停止");
        stop.setOnClickListener(v -> { if (proc != null) proc.destroy(); });

        surfaceView = new SurfaceView(this);
        status = new TextView(this);
        status.setTextSize(12);
        status.setText("画面帧数: 0");
        log = new TextView(this);
        log.setTextSize(11);
        log.setTextIsSelectable(true);
        scroll = new ScrollView(this);
        scroll.addView(log);

        root.addView(perm); root.addView(pickData); root.addView(pick);
        root.addView(start); root.addView(stop);
        root.addView(surfaceView, new LinearLayout.LayoutParams(
            LinearLayout.LayoutParams.MATCH_PARENT, 0, 2f));
        root.addView(status);
        root.addView(scroll, new LinearLayout.LayoutParams(
            LinearLayout.LayoutParams.MATCH_PARENT, 0, 1f));
        setContentView(root);
        append("DIVA Launcher v0.4\n数据目录(默认): " + dataDir.getPath()
            + "\n顺序: 1授权 -> 3选游戏目录 -> 4开始。\n日志与运行数据都在数据目录里可见。\n");
        if (Build.VERSION.SDK_INT >= 23) {
            requestPermissions(new String[]{
                android.Manifest.permission.WRITE_EXTERNAL_STORAGE,
                android.Manifest.permission.READ_EXTERNAL_STORAGE}, 7);
        }
    }

    private void requestStorage() {
        if (Build.VERSION.SDK_INT >= 23) {
            requestPermissions(new String[]{
                android.Manifest.permission.WRITE_EXTERNAL_STORAGE,
                android.Manifest.permission.READ_EXTERNAL_STORAGE}, 7);
        }
        if (Build.VERSION.SDK_INT >= 30 && !Environment.isExternalStorageManager()) {
            try {
                startActivity(new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
                    Uri.parse("package:" + getPackageName())));
            } catch (Exception ignored) {}
        }
        append("已请求存储权限。\n");
    }

    private void pick(int req) {
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
        i.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION
            | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION);
        startActivityForResult(i, req);
    }

    private String uriToPath(Uri uri) {
        String doc = uri.getLastPathSegment();
        String base = Environment.getExternalStorageDirectory().getPath();
        String rel = doc.contains(":") ? doc.substring(doc.indexOf(':') + 1) : "";
        String vol = doc.contains(":") ? doc.substring(0, doc.indexOf(':')) : "primary";
        if (!vol.equals("primary")) base = "/storage/" + vol;
        return base + "/" + rel;
    }

    @Override protected void onActivityResult(int req, int res, Intent data) {
        super.onActivityResult(req, res, data);
        if (res != RESULT_OK || data == null) return;
        Uri uri = data.getData();
        try {
            getContentResolver().takePersistableUriPermission(uri,
                Intent.FLAG_GRANT_READ_URI_PERMISSION);
        } catch (Exception ignored) {}
        String path = uriToPath(uri);
        if (req == REQ_GAME) { gamePath = path; append("游戏目录: " + path + "\n"); }
        else if (req == REQ_DATA) { dataDir = new File(path); append("数据目录: " + path + "\n"); }
    }

    private void append(final String s) {
        runOnUiThread(() -> {
            log.append(s);
            scroll.post(() -> scroll.fullScroll(ScrollView.FOCUS_DOWN));
        });
    }

    private File extractAsset(String name, File dir) throws Exception {
        File out = new File(dir, name);
        if (out.exists() && out.length() > 0) return out;
        append("释放资产 " + name + " -> " + dir.getPath() + " ...\n");
        try (InputStream in = getAssets().open(name);
             OutputStream os = new FileOutputStream(out)) {
            byte[] buf = new byte[1 << 20]; int n;
            while ((n = in.read(buf)) > 0) os.write(buf, 0, n);
        }
        return out;
    }

    private static void copyFile(File src, File dst) throws Exception {
        try (InputStream in = new java.io.FileInputStream(src);
             OutputStream os = new FileOutputStream(dst)) {
            byte[] buf = new byte[1 << 20]; int n;
            while ((n = in.read(buf)) > 0) os.write(buf, 0, n);
        }
    }

    // Override .so from dataDir or Download -> copy into private dir (linker-permitted).
    private File overrideOr(String name, String nld) {
        File[] cands = { new File(dataDir, name),
            new File(Environment.getExternalStoragePublicDirectory(
                Environment.DIRECTORY_DOWNLOADS), name) };
        for (File ov : cands) {
            if (ov.exists()) {
                try {
                    File dst = new File(getFilesDir(), name);
                    copyFile(ov, dst);
                    dst.setExecutable(true);
                    append("使用覆盖: " + ov.getPath() + "\n");
                    return dst;
                } catch (Exception e) { append("覆盖拷贝失败: " + e + "\n"); }
            }
        }
        return new File(nld, name);
    }

    private void startGame() {
        if (gamePath == null) { append("请先选择游戏文件夹。\n"); return; }
        new Thread(() -> {
            try {
                if (!dataDir.exists()) dataDir.mkdirs();
                if (!dataDir.canWrite()) {
                    append("数据目录不可写: " + dataDir.getPath() + " 请先授权存储或换目录。\n");
                    return;
                }
                File boot = extractAsset("boot-linked.bin", dataDir);
                File content = extractAsset("content.bin", dataDir);
                File aotSrc = extractAsset("libbbaot.so", dataDir);
                // dlopen cannot load from /sdcard (linker namespace); stage AOT in private dir.
                File aot = new File(getFilesDir(), "libbbaot.so");
                if (!aot.exists() || aot.length() != aotSrc.length()) {
                    append("拷贝 AOT 到私有目录供加载...\n");
                    copyFile(aotSrc, aot);
                }
                String nld = getApplicationInfo().nativeLibraryDir;
                File exe = overrideOr("libbbprobe.so", nld);
                exe.setExecutable(true);
                File diag = overrideOr("libdivadiag.so", nld);
                File crashLog = new File(dataDir, "diva-crash.log");
                File logFile = new File(dataDir, "diva-boot.log");
                String sockPath = new File(getFilesDir(), "bbpresent.sock").getPath();
                String pr = BbPresent.start(surfaceView.getHolder(), sockPath);
                append("呈现: " + pr + "\n");
                append("exe=" + exe.getPath() + "\napp0=" + gamePath
                    + "\n数据目录=" + dataDir.getPath() + "\n启动 bb-probe ...\n");
                ProcessBuilder pb = new ProcessBuilder(
                    exe.getPath(), boot.getPath(),
                    "--app0", gamePath,
                    "--content-profile", content.getPath(),
                    "--timeout", "600");
                String home = getFilesDir().getPath();
                pb.environment().put("LD_LIBRARY_PATH", nld);
                if (diag.exists()) {
                    pb.environment().put("LD_PRELOAD", diag.getPath());
                    pb.environment().put("DIVA_DIAG_LOG", crashLog.getPath());
                }
                pb.environment().put("HOME", home);
                pb.environment().put("XDG_DATA_HOME", home);
                pb.environment().put("XDG_CONFIG_HOME", home);
                pb.environment().put("XDG_CACHE_HOME", home);
                pb.environment().put("TMPDIR", home);
                pb.environment().put("BB_AOT", aot.getPath());
                pb.environment().put("BB_PRESENT_SOCKET", sockPath);
                pb.environment().put("BB_IMPORT_STUBS", "1");
                pb.environment().put("BB_FILE_TRACE", "1");
                pb.environment().put("BB_HLE_TRACE", "1");
                pb.redirectErrorStream(true);
                pb.directory(dataDir);
                proc = pb.start();
                handler.post(new Runnable() {
                    @Override public void run() {
                        status.setText("画面帧数: " + BbPresent.frames()
                            + (proc != null && proc.isAlive() ? " (运行中)" : " (已退出)"));
                        if (proc != null && proc.isAlive()) handler.postDelayed(this, 2000);
                    }
                });
                FileOutputStream fos = new FileOutputStream(logFile);
                BufferedReader br = new BufferedReader(new InputStreamReader(proc.getInputStream()));
                String line;
                while ((line = br.readLine()) != null) {
                    fos.write((line + "\n").getBytes());
                    append(line + "\n");
                }
                int code = proc.waitFor();
                fos.close();
                append("进程退出 code=" + code + "\n日志: " + logFile.getPath() + "\n");
                if (crashLog.exists()) {
                    append("---- 崩溃回溯 ----\n");
                    BufferedReader cr = new BufferedReader(new java.io.FileReader(crashLog));
                    String cl;
                    while ((cl = cr.readLine()) != null) append(cl + "\n");
                    cr.close();
                }
            } catch (Exception e) {
                append("错误: " + e + "\n");
            }
        }).start();
    }
}
