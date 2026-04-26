package com.th06.game;

import android.content.Context;
import android.graphics.Color;
import android.graphics.Rect;
import android.os.Build;
import android.util.Log;
import android.view.MotionEvent;
import android.view.View;

import org.libsdl.app.SDLActivity;

import java.io.BufferedWriter;
import java.io.File;
import java.io.FileWriter;
import java.io.IOException;
import java.text.SimpleDateFormat;
import java.util.ArrayList;
import java.util.Date;
import java.util.List;
import java.util.Locale;

/**
 * 全屏透明触摸 overlay，作为 SDL Android 触摸路径的兼容补丁。
 *
 * 问题背景：
 *   SDL2 的 SDLSurface 是 SurfaceView 的子类，在 Android 12+ BLAST 渲染管线下
 *   触摸事件分发可能被 GL 渲染线程阻塞，表现为"卡 100ms~2s"。
 *   此外 SDLSurface.onTouch 在 ACTION_MOVE 分支不读 historical events，
 *   高刷屏下中间样本被吞，导致"瞬移"。
 *
 * 解决思路：
 *   1) 用一个普通 View（不是 SurfaceView）覆盖在 SDL surface 上方接管触摸；
 *      普通 View 走标准 ViewRootImpl 事件链，与 GL 渲染解耦。
 *   2) 把 MotionEvent 转发给 SDLActivity.onNativeTouch（保持 native 处理逻辑不变）。
 *   3) ACTION_MOVE 分支显式遍历 getHistoricalX/Y，补回被合并的中间样本。
 *
 * 与现有"全屏滑动拖自机"逻辑（AndroidTouchInput.cpp）的关系：
 *   完全兼容。本 overlay 只是替换事件来源 View，转发给同一个 native 入口。
 *   AndroidTouchInput 看到的 SDL_FINGERDOWN/MOTION/UP 序列与之前一致，
 *   但事件不再丢失或被延迟。
 */
public class CompatTouchOverlay extends View
{
    private static final String TAG = "CompatTouch";

    private int mSurfaceW = 1;
    private int mSurfaceH = 1;

    // ── 文件日志（不依赖 logcat） ──────────────────────────────────────────
    // 路径：${context.getExternalFilesDir(null)}/touch_diag/compat_touch_<yyyyMMdd_HHmmss>.log
    // 该目录与 CrashHandlerPosix 写入的 ${UserPath}/crash/ 处于同一级。
    // ACTION_DOWN/UP/CANCEL 全量记录；ACTION_MOVE 每 32 个事件采样 1 条防爆。
    private BufferedWriter mLogWriter = null;
    private boolean mLogInitDone = false;
    private long mLogStartMs = 0;
    private int mMoveLogCounter = 0;
    private static final int MOVE_LOG_EVERY = 8;

    public CompatTouchOverlay(Context ctx)
    {
        super(ctx);
        setBackgroundColor(Color.TRANSPARENT);
        setFocusable(false);
        setFocusableInTouchMode(false);
        setClickable(false);
        setLongClickable(false);
        initFileLog(ctx);
    }

    /**
     * 把整个 overlay 区域排除出系统手势识别，避免 Android 10+ 的边缘返回/导航
     * 手势在拖动中"咬掉"一段触摸事件，把一次连续 drag 切成多段 DOWN/UP。
     * 这是日志里 17115ms UP→17116ms DOWN→17127ms UP 这种 1ms 切片的根因。
     *
     * Android 10 (API 29) 起支持 setSystemGestureExclusionRects；最大覆盖范围
     * 系统会限制为屏幕高度的 ~200dp 在屏幕边缘，不过 onLayout 中提交整个矩形
     * 系统会自动取交集，效果一样。
     */
    @Override
    protected void onLayout(boolean changed, int left, int top, int right, int bottom)
    {
        super.onLayout(changed, left, top, right, bottom);
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q /* 29 */)
        {
            try
            {
                List<Rect> rects = new ArrayList<>(1);
                rects.add(new Rect(0, 0, getWidth(), getHeight()));
                setSystemGestureExclusionRects(rects);
            }
            catch (Throwable t)
            {
                Log.w(TAG, "setSystemGestureExclusionRects failed: " + t);
            }
        }
    }

    private void initFileLog(Context ctx)
    {
        if (mLogInitDone) return;
        mLogInitDone = true;
        try
        {
            // 优先用 SessionLogCollector 建好的会话目录
            File diagDir = SessionLogCollector.getSessionDir();
            if (diagDir == null)
            {
                // 兜底：把日志写到 ${externalFilesDir}/touch_diag/（旧路径）
                File baseDir = ctx.getExternalFilesDir(null);
                if (baseDir == null) baseDir = ctx.getFilesDir();
                diagDir = new File(baseDir, "touch_diag");
                if (!diagDir.exists()) diagDir.mkdirs();
            }
            File logFile;
            if (SessionLogCollector.getSessionDir() != null)
            {
                logFile = new File(diagDir, "compat_touch.log");
            }
            else
            {
                String stamp = new SimpleDateFormat("yyyyMMdd_HHmmss",
                        Locale.US).format(new Date());
                logFile = new File(diagDir, "compat_touch_" + stamp + ".log");
            }
            mLogWriter = new BufferedWriter(new FileWriter(logFile, true));
            mLogStartMs = System.currentTimeMillis();
            mLogWriter.write("# CompatTouchOverlay session start\n");
            mLogWriter.write("# device=" + Build.MANUFACTURER + " " + Build.MODEL
                    + " sdk=" + Build.VERSION.SDK_INT
                    + " release=" + Build.VERSION.RELEASE + "\n");
            mLogWriter.write("# log path = " + logFile.getAbsolutePath() + "\n");
            mLogWriter.flush();
            Log.v(TAG, "file log = " + logFile.getAbsolutePath());
        }
        catch (Throwable t)
        {
            Log.w(TAG, "failed to init file log: " + t);
            mLogWriter = null;
        }
    }

    private void writeLog(String line)
    {
        if (mLogWriter == null) return;
        try
        {
            long ms = System.currentTimeMillis() - mLogStartMs;
            mLogWriter.write(ms + "ms " + line + "\n");
            mLogWriter.flush();
        }
        catch (IOException e)
        {
            // 一次失败就放弃，避免把每帧都拖死
            try { mLogWriter.close(); } catch (IOException ignore) {}
            mLogWriter = null;
        }
    }

    /**
     * SDL surface 大小变化时调用，用于把 overlay 的像素坐标归一化到与 SDL 一致的 [0,1]。
     * 如果不调用，会回退到 overlay 自身的 getWidth/getHeight。
     */
    public void setSurfaceSize(int w, int h)
    {
        mSurfaceW = (w > 0) ? w : 1;
        mSurfaceH = (h > 0) ? h : 1;
    }

    // ── 静态共享尺寸（供 SDLActivity.dispatchTouchEvent 顶层路径复用）─────────
    // SDLActivity.dispatchTouchEvent 在 Activity 层拦截，比任何 View 都早；
    // 它不持有 overlay 实例引用，所以把当前 SDL surface 尺寸放静态字段共享。
    private static volatile int sStaticSurfaceW = 1;
    private static volatile int sStaticSurfaceH = 1;
    private static volatile boolean sStaticLogInited = false;
    private static BufferedWriter sStaticLogWriter = null;
    private static long sStaticLogStartMs = 0;
    private static int sStaticMoveLogCounter = 0;
    private static final Object sStaticLogLock = new Object();

    @Override
    public boolean onTouchEvent(MotionEvent event)
    {
        // overlay 还作为 fallback view 留在 layout 中（兼容 dispatchTouchEvent
        // 关闭/异常返回 false 的情况），但优先路径已经在 SDLActivity.dispatchTouchEvent
        // 用静态方法吃掉了，所以这里通常根本不会触发。
        final int W = (mSurfaceW > 1) ? mSurfaceW : Math.max(getWidth(), 1);
        final int H = (mSurfaceH > 1) ? mSurfaceH : Math.max(getHeight(), 1);
        return dispatchToNative(event, W, H);
    }

    /**
     * 顶层共享的事件转发入口。SDLActivity.dispatchTouchEvent 直接调用这个方法，
     * 不经过任何 View 层级。返回 true 表示已消费，调用方应当短路掉默认 dispatch。
     */
    public static boolean dispatchToNative(MotionEvent event, int viewW, int viewH)
    {
        int touchDevId = event.getDeviceId();
        // SDLSurface 里的同款保护：避免 deviceId == -1 与 SDL 内部合成事件冲突
        if (touchDevId < 0)
        {
            touchDevId -= 1;
        }

        final int action = event.getActionMasked();
        final int pointerCount = event.getPointerCount();

        final int W = (viewW > 1) ? viewW : 1;
        final int H = (viewH > 1) ? viewH : 1;

        switch (action)
        {
            case MotionEvent.ACTION_MOVE:
            {
                // 修补 SDL 漏读 historical events 的 bug：
                // Android 在高刷屏会把多个 sample batch 进同一个 MotionEvent，
                // 中间位置放在 getHistoricalX/Y。先按时间顺序补发，再发当前位置。
                final int historySize = event.getHistorySize();
                for (int h = 0; h < historySize; h++)
                {
                    for (int i = 0; i < pointerCount; i++)
                    {
                        int pid = event.getPointerId(i);
                        float x = event.getHistoricalX(i, h) / W;
                        float y = event.getHistoricalY(i, h) / H;
                        float p = event.getHistoricalPressure(i, h);
                        if (p > 1.0f) p = 1.0f;
                        SDLActivity.onNativeTouch(touchDevId, pid, action, x, y, p);
                    }
                }
                for (int i = 0; i < pointerCount; i++)
                {
                    int pid = event.getPointerId(i);
                    float x = event.getX(i) / W;
                    float y = event.getY(i) / H;
                    float p = event.getPressure(i);
                    if (p > 1.0f) p = 1.0f;
                    SDLActivity.onNativeTouch(touchDevId, pid, action, x, y, p);
                }
                // 节流：每 N 个 MOVE 才落一行，避免日志爆炸
                if ((sStaticMoveLogCounter++ % MOVE_LOG_EVERY) == 0)
                {
                    staticWriteLog("MOVE pc=" + pointerCount + " hist=" + historySize
                            + " x0=" + event.getX(0) + " y0=" + event.getY(0)
                            + " W=" + W + " H=" + H);
                }
                break;
            }

            case MotionEvent.ACTION_DOWN:
            case MotionEvent.ACTION_UP:
            {
                // 注意：requestUnbufferedDispatch 是 View 实例方法，无法在静态
                // helper 里调；调用方（SDLActivity.dispatchTouchEvent / overlay
                // onTouchEvent）应在 ACTION_DOWN 时各自调一次。
                // 主指按下/抬起，actionIndex 永远是 0
                int i = 0;
                int pid = event.getPointerId(i);
                float x = event.getX(i) / W;
                float y = event.getY(i) / H;
                float p = event.getPressure(i);
                if (p > 1.0f) p = 1.0f;
                SDLActivity.onNativeTouch(touchDevId, pid, action, x, y, p);
                staticWriteLog((action == MotionEvent.ACTION_DOWN ? "DOWN" : "UP")
                        + " pid=" + pid
                        + " x=" + event.getX(i) + " y=" + event.getY(i)
                        + " W=" + W + " H=" + H);
                sStaticMoveLogCounter = 0;
                break;
            }

            case MotionEvent.ACTION_POINTER_DOWN:
            case MotionEvent.ACTION_POINTER_UP:
            {
                int i = event.getActionIndex();
                int pid = event.getPointerId(i);
                float x = event.getX(i) / W;
                float y = event.getY(i) / H;
                float p = event.getPressure(i);
                if (p > 1.0f) p = 1.0f;
                SDLActivity.onNativeTouch(touchDevId, pid, action, x, y, p);
                staticWriteLog((action == MotionEvent.ACTION_POINTER_DOWN
                        ? "PTRDOWN" : "PTRUP")
                        + " pid=" + pid
                        + " x=" + event.getX(i) + " y=" + event.getY(i));
                break;
            }

            case MotionEvent.ACTION_CANCEL:
            {
                // 与 SDLSurface 同款：把所有 pointer 当作 UP 抬起，避免悬挂
                for (int i = 0; i < pointerCount; i++)
                {
                    int pid = event.getPointerId(i);
                    float x = event.getX(i) / W;
                    float y = event.getY(i) / H;
                    float p = event.getPressure(i);
                    if (p > 1.0f) p = 1.0f;
                    SDLActivity.onNativeTouch(touchDevId, pid,
                            MotionEvent.ACTION_UP, x, y, p);
                }
                staticWriteLog("CANCEL pc=" + pointerCount);
                break;
            }

            default:
                break;
        }

        // 必须 return true 以消费事件，防止它继续向下传给 SDLSurface（避免重复触发）。
        return true;
    }

    /**
     * 由 SDLActivity 在 onCreate 时调一次，建立静态文件日志（与实例 initFileLog
     * 共享同一个 BufferedWriter，先调到的负责打开文件）。
     */
    public static synchronized void ensureStaticLogInited(Context ctx)
    {
        if (sStaticLogInited) return;
        sStaticLogInited = true;
        try
        {
            File diagDir = SessionLogCollector.getSessionDir();
            if (diagDir == null)
            {
                File baseDir = ctx.getExternalFilesDir(null);
                if (baseDir == null) baseDir = ctx.getFilesDir();
                diagDir = new File(baseDir, "touch_diag");
                if (!diagDir.exists()) diagDir.mkdirs();
            }
            File logFile;
            if (SessionLogCollector.getSessionDir() != null)
            {
                logFile = new File(diagDir, "compat_touch.log");
            }
            else
            {
                String stamp = new SimpleDateFormat("yyyyMMdd_HHmmss",
                        Locale.US).format(new Date());
                logFile = new File(diagDir, "compat_touch_" + stamp + ".log");
            }
            sStaticLogWriter = new BufferedWriter(new FileWriter(logFile, true));
            sStaticLogStartMs = System.currentTimeMillis();
            sStaticLogWriter.write("# CompatTouch (top-level dispatch) session start\n");
            sStaticLogWriter.write("# device=" + Build.MANUFACTURER + " " + Build.MODEL
                    + " sdk=" + Build.VERSION.SDK_INT
                    + " release=" + Build.VERSION.RELEASE + "\n");
            sStaticLogWriter.write("# log path = " + logFile.getAbsolutePath() + "\n");
            sStaticLogWriter.flush();
            Log.v(TAG, "static file log = " + logFile.getAbsolutePath());
        }
        catch (Throwable t)
        {
            Log.w(TAG, "failed to init static file log: " + t);
            sStaticLogWriter = null;
        }
    }

    public static void setSurfaceSizeStatic(int w, int h)
    {
        sStaticSurfaceW = (w > 0) ? w : 1;
        sStaticSurfaceH = (h > 0) ? h : 1;
    }

    public static int getSurfaceWStatic() { return sStaticSurfaceW; }
    public static int getSurfaceHStatic() { return sStaticSurfaceH; }

    public static void staticWriteLog(String line)
    {
        synchronized (sStaticLogLock)
        {
            if (sStaticLogWriter == null) return;
            try
            {
                long ms = System.currentTimeMillis() - sStaticLogStartMs;
                sStaticLogWriter.write(ms + "ms " + line + "\n");
                sStaticLogWriter.flush();
            }
            catch (IOException e)
            {
                try { sStaticLogWriter.close(); } catch (IOException ignore) {}
                sStaticLogWriter = null;
            }
        }
    }
}
