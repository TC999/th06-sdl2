package com.th06.game;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.Service;
import android.content.Context;
import android.content.Intent;
import android.os.Build;
import android.os.IBinder;
import android.os.PowerManager;
import android.util.Log;

/**
 * Foreground service to prevent vivo PEM (Process Energy Management) from
 * cgroup-freezing our process while the user is actively playing.
 *
 * Symptom this fixes: vivo Android 16 puts the game process in
 *   - sys.vivo.frozen.uids
 *   - cpuset/cpu cgroup = /background
 *   - oom_score_adj ~= 430 (cached level)
 * resulting in ~1Hz input MOVE delivery (touch stalls).
 *
 * Adding a foreground service with a persistent notification forces:
 *   - oom_score_adj < 100
 *   - process moved out of /background cgroup
 *   - PEM exclusion from freeze list (foreground services are normally exempt)
 *
 * Also acquires a PARTIAL_WAKE_LOCK to keep CPU running.
 */
public class GamePerformanceService extends Service {
    private static final String TAG = "GamePerfService";
    private static final String CHANNEL_ID = "th06_game_performance";
    private static final int NOTIFICATION_ID = 0x7406;

    private PowerManager.WakeLock mWakeLock;

    @Override
    public void onCreate() {
        super.onCreate();
        Log.i(TAG, "onCreate");
        writeLog("[GamePerfService] onCreate");
        try {
            createNotificationChannel();
            Notification notification = buildNotification();
            startForeground(NOTIFICATION_ID, notification);
            Log.i(TAG, "startForeground OK");
            writeLog("[GamePerfService] startForeground OK");
        } catch (Throwable t) {
            Log.e(TAG, "startForeground failed: " + t);
            writeLog("[GamePerfService] startForeground FAILED: " + t);
        }

        try {
            PowerManager pm = (PowerManager) getSystemService(Context.POWER_SERVICE);
            if (pm != null) {
                mWakeLock = pm.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "th06:GamePerf");
                mWakeLock.setReferenceCounted(false);
                mWakeLock.acquire();
                Log.i(TAG, "WakeLock acquired");
                writeLog("[GamePerfService] WakeLock acquired");
            }
        } catch (Throwable t) {
            Log.e(TAG, "WakeLock failed: " + t);
            writeLog("[GamePerfService] WakeLock FAILED: " + t);
        }
    }

    private static void writeLog(String s) {
        try { CompatTouchOverlay.staticWriteLog(s); } catch (Throwable ignored) {}
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        return START_STICKY;
    }

    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }

    @Override
    public void onDestroy() {
        Log.i(TAG, "onDestroy");
        try {
            if (mWakeLock != null && mWakeLock.isHeld()) {
                mWakeLock.release();
            }
        } catch (Throwable ignored) {
        }
        super.onDestroy();
    }

    private void createNotificationChannel() {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.O) return;
        NotificationManager nm = (NotificationManager) getSystemService(Context.NOTIFICATION_SERVICE);
        if (nm == null) return;
        if (nm.getNotificationChannel(CHANNEL_ID) != null) return;
        NotificationChannel ch = new NotificationChannel(
                CHANNEL_ID,
                "Game performance",
                NotificationManager.IMPORTANCE_LOW);
        ch.setDescription("Keeps the game responsive while playing.");
        ch.setShowBadge(false);
        ch.setSound(null, null);
        ch.enableVibration(false);
        nm.createNotificationChannel(ch);
    }

    private Notification buildNotification() {
        Notification.Builder b;
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            b = new Notification.Builder(this, CHANNEL_ID);
        } else {
            b = new Notification.Builder(this);
        }
        b.setContentTitle("th06")
         .setContentText("游戏运行中")
         .setOngoing(true)
         .setShowWhen(false)
         .setSmallIcon(android.R.drawable.star_on);
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.LOLLIPOP) {
            b.setPriority(Notification.PRIORITY_LOW);
        }
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            b.setCategory(Notification.CATEGORY_SERVICE);
        }
        return b.build();
    }

    public static void start(Context ctx) {
        try {
            Intent i = new Intent(ctx, GamePerformanceService.class);
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
                ctx.startForegroundService(i);
            } else {
                ctx.startService(i);
            }
        } catch (Throwable t) {
            Log.e(TAG, "start failed: " + t);
        }
    }

    public static void stop(Context ctx) {
        try {
            ctx.stopService(new Intent(ctx, GamePerformanceService.class));
        } catch (Throwable ignored) {
        }
    }
}
