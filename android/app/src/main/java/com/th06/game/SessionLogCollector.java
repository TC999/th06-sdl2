package com.th06.game;

import android.content.Context;
import android.os.Build;
import android.util.Log;

import java.io.BufferedReader;
import java.io.BufferedWriter;
import java.io.File;
import java.io.FileWriter;
import java.io.InputStreamReader;
import java.text.SimpleDateFormat;
import java.util.Date;
import java.util.Locale;
import java.util.UUID;

/**
 * 每次 Activity onCreate 时建立一份"会话目录"，把所有诊断输出（含 logcat）
 * 都收拢到同一个目录里，免开 adb。
 *
 *   ${getExternalFilesDir(null)}/Crash/<yyyyMMdd_HHmmss>-<8hex>/
 *      ├─ logcat.log          ← 本地用 ProcessBuilder 启动 logcat 抓取本进程
 *      ├─ compat_touch.log    ← CompatTouchOverlay 写入
 *      ├─ touch_native.log    ← C++ TouchDiag 写入（通过 current_session.txt）
 *      ├─ diag.log            ← C++ AssetIO::DiagLog 写入（同样机制）
 *      └─ crash_*.txt         ← CrashHandlerPosix（已有路径，不动）
 *
 * 同时把 ${UserPath}/current_session.txt 里写入会话目录的绝对路径，C++ 端
 * （TouchDiag::Init / AssetIO::DiagLog）会在第一次写日志前读取它。
 */
public final class SessionLogCollector
{
    private static final String TAG = "SessionLog";

    private static volatile File sSessionDir = null;
    private static Thread sLogcatThread = null;
    private static java.lang.Process sLogcatProcess = null;
    private static volatile boolean sStopRequested = false;

    private SessionLogCollector() {}

    /**
     * 必须在 SDL 初始化之前（onCreate 最早期）调用。
     * 多次调用是幂等的。
     */
    public static synchronized void start(Context ctx)
    {
        if (sSessionDir != null) return;
        try
        {
            File baseDir = ctx.getExternalFilesDir(null);
            if (baseDir == null) baseDir = ctx.getFilesDir();
            File crashRoot = new File(baseDir, "Crash");
            if (!crashRoot.exists()) crashRoot.mkdirs();

            String stamp = new SimpleDateFormat("yyyyMMdd_HHmmss",
                    Locale.US).format(new Date());
            String shortId = UUID.randomUUID().toString().replace("-", "")
                    .substring(0, 8);
            File sessionDir = new File(crashRoot, stamp + "-" + shortId);
            if (!sessionDir.exists()) sessionDir.mkdirs();
            sSessionDir = sessionDir;

            // 让 native 端也能找到这个目录
            File marker = new File(baseDir, "current_session.txt");
            BufferedWriter mw = new BufferedWriter(new FileWriter(marker, false));
            mw.write(sessionDir.getAbsolutePath());
            mw.write("\n");
            mw.close();

            // 启动 logcat 抓取线程
            startLogcatThread(sessionDir);

            // 写一个 session 头，方便人看
            File header = new File(sessionDir, "session.txt");
            BufferedWriter hw = new BufferedWriter(new FileWriter(header, false));
            hw.write("session = " + stamp + "-" + shortId + "\n");
            hw.write("pid     = " + android.os.Process.myPid() + "\n");
            hw.write("device  = " + Build.MANUFACTURER + " " + Build.MODEL + "\n");
            hw.write("sdk     = " + Build.VERSION.SDK_INT + " (" + Build.VERSION.RELEASE + ")\n");
            hw.write("abi     = " + Build.SUPPORTED_ABIS[0] + "\n");
            hw.close();

            Log.v(TAG, "session dir = " + sessionDir.getAbsolutePath());
        }
        catch (Throwable t)
        {
            Log.e(TAG, "failed to setup session dir: " + t);
            sSessionDir = null;
        }
    }

    public static File getSessionDir() { return sSessionDir; }

    /**
     * Activity onDestroy 调用，停止 logcat 抓取。
     */
    public static synchronized void stop()
    {
        sStopRequested = true;
        try { if (sLogcatProcess != null) sLogcatProcess.destroy(); }
        catch (Throwable ignore) {}
        sLogcatProcess = null;
        try { if (sLogcatThread != null) sLogcatThread.interrupt(); }
        catch (Throwable ignore) {}
        sLogcatThread = null;
    }

    private static void startLogcatThread(final File sessionDir)
    {
        sStopRequested = false;
        sLogcatThread = new Thread("th06-logcat-collector")
        {
            @Override
            public void run()
            {
                final int myPid = android.os.Process.myPid();
                File outFile = new File(sessionDir, "logcat.log");
                BufferedWriter bw = null;
                try
                {
                    // -d=不持续 / 我们要持续，所以不加 -d
                    // -T 1 = 从最后 1 条开始（不要历史 buffer 灌爆）
                    // --pid 限定本进程
                    // -v threadtime = 时间 + tid 信息齐全
                    java.lang.Process p;
                    try
                    {
                        p = new ProcessBuilder(
                                "logcat", "-v", "threadtime",
                                "-T", "1",
                                "--pid", String.valueOf(myPid))
                                .redirectErrorStream(true)
                                .start();
                    }
                    catch (Throwable t)
                    {
                        // 老的 Android 不支持 --pid，回退到无过滤模式
                        p = new ProcessBuilder(
                                "logcat", "-v", "threadtime", "-T", "1")
                                .redirectErrorStream(true)
                                .start();
                    }
                    sLogcatProcess = p;

                    bw = new BufferedWriter(new FileWriter(outFile, false));
                    bw.write("# logcat capture, pid=" + myPid + "\n");
                    bw.flush();

                    BufferedReader br = new BufferedReader(
                            new InputStreamReader(p.getInputStream()));
                    String line;
                    int sinceFlush = 0;
                    while (!sStopRequested && (line = br.readLine()) != null)
                    {
                        bw.write(line);
                        bw.write('\n');
                        // 每 32 行 flush 一次，避免崩溃丢尾巴又不至于太抖
                        if ((++sinceFlush & 31) == 0)
                        {
                            bw.flush();
                        }
                    }
                }
                catch (Throwable t)
                {
                    Log.w(TAG, "logcat collector died: " + t);
                }
                finally
                {
                    try { if (bw != null) { bw.flush(); bw.close(); } }
                    catch (Throwable ignore) {}
                }
            }
        };
        sLogcatThread.setDaemon(true);
        sLogcatThread.start();
    }
}
