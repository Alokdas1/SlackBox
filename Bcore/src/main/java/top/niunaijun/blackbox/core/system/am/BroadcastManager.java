package top.niunaijun.blackbox.core.system.am;

import android.content.BroadcastReceiver;
import android.content.ComponentName;
import android.content.Context;
import android.os.Build;
import android.os.Handler;
import android.os.Looper;
import android.os.Message;

import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

import top.niunaijun.blackbox.BlackBoxCore;
import top.niunaijun.blackbox.core.system.pm.BPackage;
import top.niunaijun.blackbox.core.system.pm.BPackageManagerService;
import top.niunaijun.blackbox.core.system.pm.BPackageSettings;
import top.niunaijun.blackbox.core.system.pm.PackageMonitor;
import top.niunaijun.blackbox.core.system.user.BUserManagerService;
import top.niunaijun.blackbox.entity.am.PendingResultData;
import top.niunaijun.blackbox.proxy.ProxyBroadcastReceiver;
import top.niunaijun.blackbox.utils.Slog;


public class BroadcastManager implements PackageMonitor {
    public static final String TAG = "BroadcastManager";

    public static final int TIMEOUT = 9000;

    public static final int MSG_TIME_OUT = 1;

    private static BroadcastManager sBroadcastManager;

    private final BActivityManagerService mAms;
    private final BPackageManagerService mPms;
    private final Map<ReceiverKey, List<BroadcastReceiver>> mReceivers = new HashMap<>();
    private final Map<String, PendingResultData> mReceiversData = new HashMap<>();

    private final Handler mHandler = new Handler(Looper.getMainLooper()) {
        @Override
        public void handleMessage(Message msg) {
            super.handleMessage(msg);
            switch (msg.what) {
                case MSG_TIME_OUT:
                    try {
                        PendingResultData data = (PendingResultData) msg.obj;
                        if (data == null) {
                            return;
                        }
                        synchronized (mReceiversData) {
                            if (mReceiversData.remove(data.mBToken) == null) {
                                return;
                            }
                        }
                        data.build().finish();
                        Slog.d(TAG, "Timed out virtual broadcast: " + data.mBToken);
                    } catch (Throwable ignore) {
                    }
                    break;
            }
        }
    };

    public static BroadcastManager startSystem(BActivityManagerService ams, BPackageManagerService pms) {
        if (sBroadcastManager == null) {
            synchronized (BroadcastManager.class) {
                if (sBroadcastManager == null) {
                    sBroadcastManager = new BroadcastManager(ams, pms);
                }
            }
        }
        return sBroadcastManager;
    }

    public BroadcastManager(BActivityManagerService ams, BPackageManagerService pms) {
        mAms = ams;
        mPms = pms;
    }

    public void startup() {
        mPms.addPackageMonitor(this);
        List<BPackageSettings> bPackageSettings = mPms.getBPackageSettings();
        for (BPackageSettings bPackageSetting : bPackageSettings) {
            for (Integer userId : bPackageSetting.getUserIds()) {
                if (userId >= 0 && bPackageSetting.getInstalled(userId)
                        && BUserManagerService.get().exists(userId)) {
                    registerPackage(bPackageSetting.pkg, userId);
                }
            }
        }
    }

    private void registerPackage(BPackage bPackage, int userId) {
        synchronized (mReceivers) {
            Slog.d(TAG, "Registering virtual receivers: package=" + bPackage.packageName
                    + ", userId=" + userId + ", count=" + bPackage.receivers.size());
            for (BPackage.Activity receiver : bPackage.receivers) {
                List<BPackage.ActivityIntentInfo> intents = receiver.intents;
                for (BPackage.ActivityIntentInfo intent : intents) {
                    ProxyBroadcastReceiver proxyBroadcastReceiver = new ProxyBroadcastReceiver(
                            userId, new ComponentName(bPackage.packageName, receiver.info.name));
                    // Receiver export flags were added in Android 13. Guest manifest
                    // receivers can legitimately receive framework/other-app broadcasts,
                    // so this bridge has to be exported on API 33+.
                    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                        BlackBoxCore.getContext().registerReceiver(proxyBroadcastReceiver, intent.intentFilter, Context.RECEIVER_EXPORTED);
                    }else{
                        BlackBoxCore.getContext().registerReceiver(proxyBroadcastReceiver, intent.intentFilter);
                    }
                    addReceiver(bPackage.packageName, userId, proxyBroadcastReceiver);
                }
            }
        }
    }

    private void addReceiver(String packageName, int userId, BroadcastReceiver receiver) {
        ReceiverKey key = new ReceiverKey(packageName, userId);
        List<BroadcastReceiver> broadcastReceivers = mReceivers.get(key);
        if (broadcastReceivers == null) {
            broadcastReceivers = new ArrayList<>();
            mReceivers.put(key, broadcastReceivers);
        }
        broadcastReceivers.add(receiver);
    }

    public void sendBroadcast(PendingResultData pendingResultData) {
        synchronized (mReceiversData) {
            
            mReceiversData.put(pendingResultData.mBToken, pendingResultData);
            Message obtain = Message.obtain(mHandler, MSG_TIME_OUT, pendingResultData);
            mHandler.sendMessageDelayed(obtain, TIMEOUT);
        }
    }

    public void finishBroadcast(PendingResultData data) {
        if (data == null || data.mBToken == null) {
            return;
        }
        synchronized (mReceiversData) {
            PendingResultData pending = mReceiversData.remove(data.mBToken);
            if (pending != null) {
                mHandler.removeMessages(MSG_TIME_OUT, pending);
            }
        }
    }

    @Override
    public void onPackageUninstalled(String packageName, boolean removeApp, int userId) {
        synchronized (mReceivers) {
            // Dynamic receiver registrations are instance-scoped. Remove this
            // instance even when another instance still keeps the APK installed.
            unregisterPackageLocked(packageName, userId);
        }
    }

    @Override
    public void onPackageInstalled(String packageName, int userId) {
        synchronized (mReceivers) {
            // Reinstall/update replaces the package's parsed receiver list. Unregister
            // first so old filters cannot receive duplicate broadcasts or retain the host.
            unregisterPackageLocked(packageName, userId);
            BPackageSettings bPackageSetting = mPms.getBPackageSetting(packageName);
            if (bPackageSetting != null && bPackageSetting.getInstalled(userId)) {
                registerPackage(bPackageSetting.pkg, userId);
            }
        }
    }

    private void unregisterPackageLocked(String packageName, int userId) {
        List<BroadcastReceiver> broadcastReceivers = mReceivers.remove(new ReceiverKey(packageName, userId));
        if (broadcastReceivers == null) {
            return;
        }
        Slog.d(TAG, "Unregistering virtual receivers: package=" + packageName
                + ", userId=" + userId + ", count=" + broadcastReceivers.size());
        for (BroadcastReceiver broadcastReceiver : broadcastReceivers) {
            try {
                BlackBoxCore.getContext().unregisterReceiver(broadcastReceiver);
            } catch (IllegalArgumentException ignored) {
                // The framework may have already removed a receiver during process teardown.
            }
        }
    }

    private static final class ReceiverKey {
        private final String packageName;
        private final int userId;

        ReceiverKey(String packageName, int userId) {
            this.packageName = packageName;
            this.userId = userId;
        }

        @Override
        public boolean equals(Object other) {
            if (this == other) {
                return true;
            }
            if (!(other instanceof ReceiverKey)) {
                return false;
            }
            ReceiverKey key = (ReceiverKey) other;
            return userId == key.userId && packageName.equals(key.packageName);
        }

        @Override
        public int hashCode() {
            return 31 * packageName.hashCode() + userId;
        }
    }
}
