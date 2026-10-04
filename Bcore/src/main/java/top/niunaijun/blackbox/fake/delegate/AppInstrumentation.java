package top.niunaijun.blackbox.fake.delegate;

import android.app.Activity;
import android.app.Application;
import android.app.Instrumentation;
import android.content.Context;
import android.content.Intent;
import android.content.pm.ActivityInfo;
import android.os.Bundle;
import android.os.PersistableBundle;
import android.util.Log;
import android.view.Display;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.TextureView;
import android.view.View;
import android.view.ViewGroup;
import android.view.Window;

import java.lang.reflect.Field;
import java.util.WeakHashMap;

import black.android.app.BRActivity;
import black.android.app.BRActivityThread;
import top.niunaijun.blackbox.BlackBoxCore;
import top.niunaijun.blackbox.app.BActivityThread;
import top.niunaijun.blackbox.entity.AppConfig;
import top.niunaijun.blackbox.fake.hook.HookManager;
import top.niunaijun.blackbox.fake.hook.IInjectHook;
import top.niunaijun.blackbox.fake.service.HCallbackProxy;
import top.niunaijun.blackbox.fake.service.IActivityClientProxy;
import top.niunaijun.blackbox.utils.HackAppUtils;
import top.niunaijun.blackbox.utils.CrashMonitor;
import top.niunaijun.blackbox.utils.compat.ActivityCompat;
import top.niunaijun.blackbox.utils.compat.ActivityManagerCompat;
import top.niunaijun.blackbox.utils.compat.ContextCompat;

public final class AppInstrumentation extends BaseInstrumentationDelegate implements IInjectHook {

    private static final String TAG = AppInstrumentation.class.getSimpleName();

    private static AppInstrumentation sAppInstrumentation;
    private static final WeakHashMap<SurfaceView, Boolean> sTrackedSurfaceViews = new WeakHashMap<>();

    private static final class SurfaceCounts {
        int surfaceViews;
        int validSurfaceViews;
        int textureViews;
        int availableTextureViews;
    }

    public static AppInstrumentation get() {
        if (sAppInstrumentation == null) {
            synchronized (AppInstrumentation.class) {
                if (sAppInstrumentation == null) {
                    sAppInstrumentation = new AppInstrumentation();
                }
            }
        }
        return sAppInstrumentation;
    }

    public AppInstrumentation() {
    }

    @Override
    public void injectHook() {
        try {
            Instrumentation mInstrumentation = getCurrInstrumentation();
            if (mInstrumentation == this || checkInstrumentation(mInstrumentation))
                return;
            mBaseInstrumentation = (Instrumentation) mInstrumentation;
            BRActivityThread.get(BlackBoxCore.mainThread())._set_mInstrumentation(this);
        } catch (Exception e) {
            e.printStackTrace();
        }
    }

    private Instrumentation getCurrInstrumentation() {
        Object currentActivityThread = BlackBoxCore.mainThread();
        return BRActivityThread.get(currentActivityThread).mInstrumentation();
    }

    @Override
    public boolean isBadEnv() {
        return !checkInstrumentation(getCurrInstrumentation());
    }

    private boolean checkInstrumentation(Instrumentation instrumentation) {
        if (instrumentation instanceof AppInstrumentation) {
            return true;
        }
        Class<?> clazz = instrumentation.getClass();
        if (Instrumentation.class.equals(clazz)) {
            return false;
        }
        do {
            assert clazz != null;
            Field[] fields = clazz.getDeclaredFields();
            for (Field field : fields) {
                if (Instrumentation.class.isAssignableFrom(field.getType())) {
                    field.setAccessible(true);
                    try {
                        Object obj = field.get(instrumentation);
                        if ((obj instanceof AppInstrumentation)) {
                            return true;
                        }
                    } catch (Exception e) {
                        return false;
                    }
                }
            }
            clazz = clazz.getSuperclass();
        } while (!Instrumentation.class.equals(clazz));
        return false;
    }

    private void checkHCallback() {
        HookManager.get().checkEnv(HCallbackProxy.class);
    }

    private void checkActivity(Activity activity) {
        Log.d(TAG, "callActivityOnCreate: " + activity.getClass().getName());
        HackAppUtils.enableQQLogOutput(activity.getPackageName(), activity.getClassLoader());
        checkHCallback();
        HookManager.get().checkEnv(IActivityClientProxy.class);
        ActivityInfo info = BRActivity.get(activity).mActivityInfo();
        ContextCompat.fix(activity);
        ActivityCompat.fix(activity);
        if (info.theme != 0) {
            activity.getTheme().applyStyle(info.theme, true);
        }
        ActivityManagerCompat.setActivityOrientation(activity, info.screenOrientation);
    }

    @Override
    public Application newApplication(ClassLoader cl, String className, Context context) throws InstantiationException, IllegalAccessException, ClassNotFoundException {
        ContextCompat.fix(context);

        return super.newApplication(cl, className, context);
    }

    @Override
    public void callActivityOnCreate(Activity activity, Bundle icicle, PersistableBundle persistentState) {
        checkActivity(activity);
        super.callActivityOnCreate(activity, icicle, persistentState);
        recordGuestLifecycle("activity_created", activity, "");
    }

    @Override
    public void callActivityOnCreate(Activity activity, Bundle icicle) {
        checkActivity(activity);
        super.callActivityOnCreate(activity, icicle);
        recordGuestLifecycle("activity_created", activity, "");
    }

    @Override
    public void callActivityOnStart(Activity activity) {
        super.callActivityOnStart(activity);
        recordGuestLifecycle("activity_started", activity, "");
    }

    @Override
    public void callActivityOnResume(Activity activity) {
        super.callActivityOnResume(activity);
        recordGuestLifecycle("activity_resumed", activity, "");
        scheduleWindowSnapshot(activity);
    }

    @Override
    public void callActivityOnPause(Activity activity) {
        super.callActivityOnPause(activity);
        recordGuestLifecycle("activity_paused", activity, "");
    }

    @Override
    public void callActivityOnStop(Activity activity) {
        super.callActivityOnStop(activity);
        recordGuestLifecycle("activity_stopped", activity, "");
    }

    @Override
    public void callActivityOnDestroy(Activity activity) {
        super.callActivityOnDestroy(activity);
        recordGuestLifecycle("activity_destroyed", activity, "");
    }

    private void recordGuestLifecycle(String event, Activity activity, String detail) {
        try {
            AppConfig config = BActivityThread.getAppConfig();
            if (config == null) return;
            CrashMonitor.recordEvent(event, config.packageName, config.processName, config.userId,
                    activity.getClass().getName(), detail);
        } catch (RuntimeException error) {
            Log.w(TAG, "Unable to record virtual Activity event: " + event, error);
        }
    }

    private void scheduleWindowSnapshot(Activity activity) {
        Window window = activity.getWindow();
        if (window == null) return;
        View decor = window.getDecorView();
        if (decor == null) return;
        decor.postDelayed(() -> {
            try {
                if (activity.isFinishing() || activity.isDestroyed()) return;
                SurfaceCounts surfaces = new SurfaceCounts();
                countRenderSurfaces(decor, surfaces, activity);
                Display display = activity.getDisplay();
                float refreshRate = display == null ? 0f : display.getRefreshRate();
                String detail = "width=" + decor.getWidth()
                        + ",height=" + decor.getHeight()
                        + ",attached=" + decor.isAttachedToWindow()
                        + ",hardwareAccelerated=" + decor.isHardwareAccelerated()
                        + ",windowFocus=" + activity.hasWindowFocus()
                        + ",densityDpi=" + activity.getResources().getDisplayMetrics().densityDpi
                        + ",orientation=" + activity.getResources().getConfiguration().orientation
                        + ",refreshHz=" + refreshRate
                        + ",surfaceViews=" + surfaces.surfaceViews
                        + ",validSurfaceViews=" + surfaces.validSurfaceViews
                        + ",textureViews=" + surfaces.textureViews
                        + ",availableTextureViews=" + surfaces.availableTextureViews;
                recordGuestLifecycle("guest_window_snapshot", activity, detail);
            } catch (RuntimeException error) {
                Log.w(TAG, "Unable to inspect virtual Activity window", error);
            }
        }, 500L);
    }

    private static void countRenderSurfaces(View view, SurfaceCounts counts, Activity activity) {
        if (view instanceof SurfaceView) {
            SurfaceView surfaceView = (SurfaceView) view;
            trackSurfaceView(surfaceView, activity);
            counts.surfaceViews++;
            try {
                if (surfaceView.getHolder().getSurface().isValid()) {
                    counts.validSurfaceViews++;
                }
            } catch (RuntimeException ignored) {
                // A surface may be between destroy/create while the view hierarchy settles.
            }
        } else if (view instanceof TextureView) {
            counts.textureViews++;
            if (((TextureView) view).isAvailable()) counts.availableTextureViews++;
        }
        if (view instanceof ViewGroup) {
            ViewGroup group = (ViewGroup) view;
            for (int index = 0; index < group.getChildCount(); index++) {
                countRenderSurfaces(group.getChildAt(index), counts, activity);
            }
        }
    }

    private static void trackSurfaceView(SurfaceView view, Activity activity) {
        synchronized (sTrackedSurfaceViews) {
            if (sTrackedSurfaceViews.containsKey(view)) return;
            try {
                SurfaceHolder holder = view.getHolder();
                AppConfig config = BActivityThread.getAppConfig();
                if (config == null) return;
                String packageName = config.packageName;
                String processName = config.processName;
                int userId = config.userId;
                String component = activity.getClass().getName();
                holder.addCallback(new SurfaceHolder.Callback() {
                    @Override
                    public void surfaceCreated(SurfaceHolder callbackHolder) {
                        recordSurfaceEvent("render_surface_created", packageName, processName,
                                userId, component, callbackHolder, 0, 0, 0);
                    }

                    @Override
                    public void surfaceChanged(SurfaceHolder callbackHolder, int format,
                                               int width, int height) {
                        recordSurfaceEvent("render_surface_changed", packageName, processName,
                                userId, component, callbackHolder, format, width, height);
                    }

                    @Override
                    public void surfaceDestroyed(SurfaceHolder callbackHolder) {
                        recordSurfaceEvent("render_surface_destroyed", packageName, processName,
                                userId, component, callbackHolder, 0, 0, 0);
                    }
                });
                sTrackedSurfaceViews.put(view, Boolean.TRUE);
                recordGuestEvent("render_surface_observed", packageName, processName, userId,
                        component, "valid=" + holder.getSurface().isValid()
                                + ",width=" + view.getWidth() + ",height=" + view.getHeight());
            } catch (RuntimeException error) {
                Log.w(TAG, "Unable to observe guest SurfaceView", error);
            }
        }
    }

    private static void recordSurfaceEvent(String event, String packageName, String processName,
                                          int userId, String component, SurfaceHolder holder,
                                          int format, int width, int height) {
        boolean valid;
        try {
            valid = holder.getSurface().isValid();
        } catch (RuntimeException error) {
            valid = false;
        }
        String detail = "valid=" + valid;
        if (event.endsWith("changed")) {
            detail += ",format=" + format + ",width=" + width + ",height=" + height;
        }
        recordGuestEvent(event, packageName, processName, userId, component, detail);
    }

    private static void recordGuestEvent(String event, String packageName, String processName,
                                         int userId, String component, String detail) {
        try {
            CrashMonitor.recordEvent(event, packageName, processName, userId, component, detail);
        } catch (RuntimeException error) {
            Log.w(TAG, "Unable to record guest surface event: " + event, error);
        }
    }

    @Override
    public void callApplicationOnCreate(Application app) {
        checkHCallback();
        super.callApplicationOnCreate(app);
    }

    public Activity newActivity(ClassLoader cl, String className, Intent intent) throws InstantiationException, IllegalAccessException, ClassNotFoundException {
        try {
            return super.newActivity(cl, className, intent);
        } catch (ClassNotFoundException e) {
            return mBaseInstrumentation.newActivity(cl, className, intent);
        }
    }
}
