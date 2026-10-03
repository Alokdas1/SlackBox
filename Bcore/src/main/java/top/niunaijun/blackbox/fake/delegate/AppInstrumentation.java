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
import android.view.SurfaceView;
import android.view.TextureView;
import android.view.View;
import android.view.ViewGroup;
import android.view.Window;

import java.lang.reflect.Field;

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
                countRenderSurfaces(decor, surfaces);
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

    private static void countRenderSurfaces(View view, SurfaceCounts counts) {
        if (view instanceof SurfaceView) {
            counts.surfaceViews++;
            try {
                if (((SurfaceView) view).getHolder().getSurface().isValid()) {
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
                countRenderSurfaces(group.getChildAt(index), counts);
            }
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
