package top.niunaijun.blackbox.utils;

import android.content.Context;
import android.content.pm.ApplicationInfo;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.os.Build;

import java.io.File;
import java.io.FileWriter;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.PrintWriter;
import java.nio.charset.StandardCharsets;
import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.text.SimpleDateFormat;
import java.util.Date;
import java.util.HashMap;
import java.util.Map;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

import top.niunaijun.blackbox.BlackBoxCore;
import top.niunaijun.blackbox.app.BActivityThread;


public class CrashMonitor {
    private static final String TAG = "CrashMonitor";
    private static boolean sIsInitialized = false;
    
    
    private static final AtomicInteger sTotalCrashes = new AtomicInteger(0);
    private static final AtomicInteger sJavaCrashes = new AtomicInteger(0);
    private static final AtomicInteger sNativeCrashes = new AtomicInteger(0);
    
    
    private static final Map<String, CrashInfo> sCrashHistory = new HashMap<>();
    
    
    private static Thread.UncaughtExceptionHandler sPreviousExceptionHandler;
    private static final Object sEventLogLock = new Object();
    private static final ExecutorService sEventWriter = Executors.newSingleThreadExecutor(runnable -> {
        Thread thread = new Thread(runnable, "SlackBox-Diagnostics");
        thread.setDaemon(true);
        return thread;
    });

    /** Persists guest lifecycle breadcrumbs so the host can explain a process that vanished. */
    public static void recordEvent(String event, String packageName, String processName,
                                   int userId, String component, String detail) {
        Context context = BlackBoxCore.getContext();
        if (context == null) return;
        String json = formatEventLine(event, packageName, processName, userId, component, detail);
        File eventFile = new File(new File(context.getFilesDir(), "crash_logs"), "guest_events.jsonl");
        sEventWriter.execute(() -> {
            File directory = eventFile.getParentFile();
            if (directory == null || (!directory.exists() && !directory.mkdirs() && !directory.isDirectory())) {
                Slog.w(TAG, "Unable to create guest diagnostics directory");
                return;
            }
            synchronized (sEventLogLock) {
                try (FileOutputStream output = new FileOutputStream(eventFile, true);
                     java.nio.channels.FileLock ignored = output.getChannel().lock()) {
                    output.write(json.getBytes(StandardCharsets.UTF_8));
                } catch (IOException e) {
                    Slog.w(TAG, "Unable to persist guest lifecycle event: " + event, e);
                }
            }
        });
    }

    static String formatEventLine(String event, String packageName, String processName,
                                  int userId, String component, String detail) {
        return "{\"time_ms\":" + System.currentTimeMillis()
                + ",\"event\":\"" + jsonEscape(event)
                + "\",\"package\":\"" + jsonEscape(packageName)
                + "\",\"process\":\"" + jsonEscape(processName)
                + "\",\"instance\":" + userId
                + ",\"component\":\"" + jsonEscape(component)
                + "\",\"detail\":\"" + jsonEscape(detail) + "\"}\n";
    }

    private static String jsonEscape(String value) {
        if (value == null) return "";
        StringBuilder escaped = new StringBuilder(value.length() + 8);
        for (int i = 0; i < value.length(); i++) {
            char character = value.charAt(i);
            if (character == '\\' || character == '"') {
                escaped.append('\\').append(character);
            } else if (character == '\n') {
                escaped.append("\\n");
            } else if (character == '\r') {
                escaped.append("\\r");
            } else if (character == '\t') {
                escaped.append("\\t");
            } else if (character < 0x20) {
                escaped.append(String.format("\\u%04x", (int) character));
            } else {
                escaped.append(character);
            }
        }
        return escaped.toString();
    }
    public static class CrashInfo {
        public final String crashType;
        public final String packageName;
        public final String errorMessage;
        public final String stackTrace;
        public final long timestamp;
        public final boolean wasRecovered;
        
        public CrashInfo(String crashType, String packageName, String errorMessage, 
                        String stackTrace, boolean wasRecovered) {
            this.crashType = crashType;
            this.packageName = packageName;
            this.errorMessage = errorMessage;
            this.stackTrace = stackTrace;
            this.timestamp = System.currentTimeMillis();
            this.wasRecovered = wasRecovered;
        }
        
        @Override
        public String toString() {
            return "Crash[" + crashType + "] " + packageName + " - " + 
                   (wasRecovered ? "RECOVERED" : "FAILED") + " at " + 
                   new SimpleDateFormat("yyyy-MM-dd HH:mm:ss").format(new Date(timestamp));
        }
    }
    
    
    public static synchronized void initialize() {
        if (sIsInitialized) {
            return;
        }
        
        try {
            Slog.d(TAG, "Initializing comprehensive crash monitoring system...");
            
            
            installGlobalCrashHandlers();
            
            
            sIsInitialized = true;
            Slog.d(TAG, "Crash monitoring system initialized successfully");
            
        } catch (Exception e) {
            Slog.e(TAG, "Failed to initialize crash monitoring: " + e.getMessage(), e);
        }
    }
    
    
    private static void installGlobalCrashHandlers() {
        try {
            sPreviousExceptionHandler = Thread.getDefaultUncaughtExceptionHandler();
            Thread.setDefaultUncaughtExceptionHandler(new Thread.UncaughtExceptionHandler() {
                @Override
                public void uncaughtException(Thread thread, Throwable throwable) {
                    try {
                        reportAndForward(sPreviousExceptionHandler, thread, throwable,
                                (crashedThread, error) -> handleCrash("JavaException", crashedThread, error));
                    } finally {
                        if (sPreviousExceptionHandler == null || sPreviousExceptionHandler == this) {
                            android.os.Process.killProcess(android.os.Process.myPid());
                            System.exit(10);
                        }
                    }
                }
            });
            
            
            installSystemErrorHandler();
            
            Slog.d(TAG, "Global crash handlers installed");
            
        } catch (Exception e) {
            Slog.w(TAG, "Failed to install global crash handlers: " + e.getMessage());
        }
    }

    static void reportAndForward(Thread.UncaughtExceptionHandler previous,
                                 Thread thread,
                                 Throwable throwable,
                                 CrashReporter reporter) {
        try {
            reporter.record(thread, throwable);
        } finally {
            if (previous != null) previous.uncaughtException(thread, throwable);
        }
    }

    interface CrashReporter {
        void record(Thread thread, Throwable throwable);
    }
    
    
    private static void installSystemErrorHandler() {
        try {
            
            Slog.d(TAG, "System error handler prepared");
        } catch (Exception e) {
            Slog.w(TAG, "Failed to install system error handler: " + e.getMessage());
        }
    }
    
    
    public static void handleCrash(String crashType, Thread thread, Throwable throwable) {
        try {
            sTotalCrashes.incrementAndGet();
            
            
            if (crashType.equals("JavaException")) {
                sJavaCrashes.incrementAndGet();
            } else if (crashType.equals("NativeCrash")) {
                sNativeCrashes.incrementAndGet();
            }
            
            
            CrashInfo crashInfo = createCrashInfo(crashType, thread, throwable);

            String processName = "unknown";
            int userId = -1;
            try {
                processName = BActivityThread.getAppProcessName();
                userId = BActivityThread.getUserId();
            } catch (Throwable ignored) {
                // Host and early-startup crashes may not have a bound virtual process.
            }
            recordEvent("java_crash", crashInfo.packageName, processName, userId, "",
                    throwable == null ? "unknown" : throwable.getClass().getName() + ": "
                            + String.valueOf(throwable.getMessage()));
            
            
            Slog.w(TAG, "Crash detected: " + crashInfo);
            
            
            String crashKey = crashType + "_" + System.currentTimeMillis();
            sCrashHistory.put(crashKey, crashInfo);
            
            
            writeCrashLog(crashInfo);
            
        } catch (Exception e) {
            Slog.e(TAG, "Error handling crash: " + e.getMessage());
        }
    }
    
    
    private static CrashInfo createCrashInfo(String crashType, Thread thread, Throwable throwable) {
        try {
            String packageName = getCurrentPackageName();
            String errorMessage = throwable != null ? throwable.getMessage() : "Unknown error";
            String stackTrace = getStackTrace(throwable);
            
            return new CrashInfo(crashType, packageName, errorMessage, stackTrace, false);
            
        } catch (Exception e) {
            Slog.w(TAG, "Error creating crash info: " + e.getMessage());
            return new CrashInfo(crashType, "unknown", "Error creating crash info", "", false);
        }
    }
    
    
    private static String getCurrentPackageName() {
        try {
            return BActivityThread.getAppPackageName();
        } catch (Exception e) {
            try {
                Context context = BlackBoxCore.getContext();
                if (context != null) {
                    return context.getPackageName();
                }
            } catch (Exception ex) {
                
            }
            return "unknown";
        }
    }
    
    
    private static String getStackTrace(Throwable throwable) {
        if (throwable == null) return "";
        
        try {
            java.io.StringWriter sw = new java.io.StringWriter();
            java.io.PrintWriter pw = new java.io.PrintWriter(sw);
            throwable.printStackTrace(pw);
            return sw.toString();
        } catch (Exception e) {
            return "Error getting stack trace: " + e.getMessage();
        }
    }
    
    
    private static void writeCrashLog(CrashInfo crashInfo) {
        try {
            Context context = BlackBoxCore.getContext();
            if (context == null) return;
            
            File logDir = new File(context.getFilesDir(), "crash_logs");
            if (!logDir.exists()) {
                logDir.mkdirs();
            }
            
            String timestamp = new SimpleDateFormat("yyyyMMdd_HHmmss_SSS").format(new Date(crashInfo.timestamp));
            File logFile = new File(logDir, "crash_" + timestamp + ".log");
            
            try (PrintWriter writer = new PrintWriter(new FileWriter(logFile))) {
                writer.println("=== CRASH LOG ===");
                writer.println("Timestamp: " + new Date(crashInfo.timestamp));
                writer.println("Crash Type: " + crashInfo.crashType);
                writer.println("Package: " + crashInfo.packageName);
                writer.println("Error: " + crashInfo.errorMessage);
                writer.println("Recovered: " + crashInfo.wasRecovered);
                writer.println("=== STACK TRACE ===");
                writer.println(crashInfo.stackTrace);
                writer.println("=== END ===");
            }
            
            Slog.d(TAG, "Crash log written to: " + logFile.getAbsolutePath());
            
        } catch (Exception e) {
            Slog.w(TAG, "Failed to write crash log: " + e.getMessage());
        }
    }
    
    
    public static String getCrashStats() {
        return "Crash Statistics:\n" +
               "Total Crashes: " + sTotalCrashes.get() + "\n" +
               "Java Crashes: " + sJavaCrashes.get() + "\n" +
               "Native Crashes: " + sNativeCrashes.get();
    }
    
    
    public static String getStatus() {
        StringBuilder status = new StringBuilder();
        status.append("Crash Diagnostics Status:\n");
        status.append("Initialized: ").append(sIsInitialized).append("\n");
        status.append("Crash History Size: ").append(sCrashHistory.size()).append("\n");
        status.append("\n").append(getCrashStats());
        
        return status.toString();
    }
    
    
    public static void clearCrashHistory() {
        sCrashHistory.clear();
        sTotalCrashes.set(0);
        sJavaCrashes.set(0);
        sNativeCrashes.set(0);
        Slog.d(TAG, "Crash history cleared");
    }

    /**
     * Best-effort, sandbox-legal cause for a guest process that just vanished.
     *
     * A Binder death proves the guest is gone but not why. Guest apps run as
     * proxy processes of the host package (host:pN), so the host can ask the
     * platform for its own historical exit reasons. That API is API 30+, so the
     * ApplicationExitInfo-touching code lives in a nested holder that is never
     * loaded on older devices -- the outer method resolves it only after the
     * SDK_INT guard.
     *
     * The hook layer also rewrites getHistoricalProcessExitReasons to an empty
     * list; that hook is the reason this may still report no record. If it does,
     * the death is being hidden from us by our own proxy, not missing from the
     * platform, and the capture path is logcat/tombstone instead.
     */
    public static String describeProcessExit(int pid, String processName) {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.R) {
            return "cause=unknown; ApplicationExitInfo requires API 30+ (device is API "
                    + Build.VERSION.SDK_INT + ")";
        }
        return ExitReasonReader.describe(pid, processName);
    }

    private static final class ExitReasonReader {
        static String describe(int pid, String processName) {
            Context context = BlackBoxCore.getContext();
            if (context == null) {
                return "cause=unknown; no host context for exit-reason query";
            }
            try {
                android.app.ActivityManager manager =
                        (android.app.ActivityManager) context.getSystemService(Context.ACTIVITY_SERVICE);
                if (manager == null) {
                    return "cause=unknown; no ActivityManager";
                }
                // 128, not 32: a game that respawns generates exits faster than a
                // human can open the diagnostics screen, so the guest's record is
                // routinely outside the most recent 32.
                java.util.List<android.app.ApplicationExitInfo> reasons =
                        manager.getHistoricalProcessExitReasons(null, 0, 128);
                android.app.ApplicationExitInfo match = null;
                // Prefer a name match over a pid match. ApplicationExitInfo reports
                // the pids the platform saw, which are usually but not always the
                // same value SlackBox recorded for the guest proxy.
                for (android.app.ApplicationExitInfo info : reasons) {
                    if (matchesProcessName(info.getProcessName(), processName)) {
                        match = info;
                        break;
                    }
                }
                if (match == null) {
                    for (android.app.ApplicationExitInfo info : reasons) {
                        if (pid > 0 && info.getPid() == pid) {
                            match = info;
                            break;
                        }
                    }
                }
                if (match == null) {
                    return "cause=unknown; platform kept no exit record for pid=" + pid
                            + " process=" + processName
                            + " (scanned " + reasons.size() + " recent exits)";
                }
                CharSequence description = match.getDescription();
                return "reason=" + reasonName(match.getReason())
                        + "; status=" + match.getStatus()
                        + "; importance=" + match.getImportance()
                        + "; pss=" + match.getPss() + "kB"
                        + "; rss=" + match.getRss() + "kB"
                        + "; at=" + match.getTimestamp()
                        + "; description=" + (description == null ? "" : description);
            } catch (Throwable t) {
                return "cause=unknown; exit-reason query failed: " + t;
            }
        }

        /**
         * ApplicationExitInfo.getProcessName() returns the platform's full
         * "pkg:proc" string. ProxyManifest.getProcessName(bpid) returns our own
         * generated guest process label, which for a multi-process guest does not
         * carry the same suffix, so equals() never matched and the pid fallback
         * was doing all the work. Compare on containment in both directions:
         * either string containing the other covers the exact case, the colon
         * suffix, and the "app.p0" style names the platform reports.
         */
        private static boolean matchesProcessName(String reported, String wanted) {
            if (reported == null || wanted == null || wanted.isEmpty()) {
                return false;
            }
            return reported.equals(wanted)
                    || reported.contains(wanted)
                    || wanted.contains(reported);
        }

        private static String reasonName(int reason) {
            switch (reason) {
                case android.app.ApplicationExitInfo.REASON_EXIT_SELF: return "EXIT_SELF";
                case android.app.ApplicationExitInfo.REASON_SIGNALED: return "SIGNALED";
                case android.app.ApplicationExitInfo.REASON_LOW_MEMORY: return "LOW_MEMORY";
                case android.app.ApplicationExitInfo.REASON_CRASH: return "CRASH_JAVA";
                case android.app.ApplicationExitInfo.REASON_CRASH_NATIVE: return "CRASH_NATIVE";
                case android.app.ApplicationExitInfo.REASON_ANR: return "ANR";
                case android.app.ApplicationExitInfo.REASON_INITIALIZATION_FAILURE: return "INIT_FAILURE";
                case android.app.ApplicationExitInfo.REASON_PERMISSION_CHANGE: return "PERMISSION_CHANGE";
                case android.app.ApplicationExitInfo.REASON_EXCESSIVE_RESOURCE_USAGE: return "EXCESSIVE_RESOURCE";
                case android.app.ApplicationExitInfo.REASON_USER_REQUESTED: return "USER_REQUESTED";
                case android.app.ApplicationExitInfo.REASON_USER_STOPPED: return "USER_STOPPED";
                case android.app.ApplicationExitInfo.REASON_DEPENDENCY_DIED: return "DEPENDENCY_DIED";
                case android.app.ApplicationExitInfo.REASON_FREEZER: return "FREEZER";
                case android.app.ApplicationExitInfo.REASON_PACKAGE_STATE_CHANGE: return "PACKAGE_STATE_CHANGE";
                case android.app.ApplicationExitInfo.REASON_PACKAGE_UPDATED: return "PACKAGE_UPDATED";
                case android.app.ApplicationExitInfo.REASON_OTHER: return "OTHER";
                default: return "UNKNOWN(" + reason + ")";
            }
        }
    }
}
