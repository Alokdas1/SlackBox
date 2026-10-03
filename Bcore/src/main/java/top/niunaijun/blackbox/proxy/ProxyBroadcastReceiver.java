package top.niunaijun.blackbox.proxy;

import android.content.BroadcastReceiver;
import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.os.RemoteException;

import top.niunaijun.blackbox.BlackBoxCore;
import top.niunaijun.blackbox.entity.am.PendingResultData;
import top.niunaijun.blackbox.proxy.record.ProxyBroadcastRecord;


public class ProxyBroadcastReceiver extends BroadcastReceiver {
    public static final String TAG = "ProxyBroadcastReceiver";
    private final int mRegisteredUserId;
    private final ComponentName mRegisteredReceiver;

    /** Used by the manifest stub, whose target and user are carried by the intent. */
    public ProxyBroadcastReceiver() {
        this(-1, null);
    }

    /** Used for a dynamically registered guest manifest receiver. */
    public ProxyBroadcastReceiver(int userId, ComponentName receiver) {
        mRegisteredUserId = userId;
        mRegisteredReceiver = receiver;
    }

    @Override
    public void onReceive(Context context, Intent intent) {
        intent.setExtrasClassLoader(context.getClassLoader());
        ProxyBroadcastRecord record = ProxyBroadcastRecord.create(intent);
        final Intent target;
        final int userId;
        if (record.mIntent != null) {
            // A guest-originated broadcast is routed only to the instance that
            // sent it. Dynamic registrations for other instances must ignore it.
            if (mRegisteredUserId >= 0 && mRegisteredUserId != record.mUserId) {
                return;
            }
            target = record.mIntent;
            userId = record.mUserId;
        } else if (mRegisteredReceiver != null && mRegisteredUserId >= 0) {
            // Framework broadcasts do not carry SlackBox extras. Convert them
            // into an explicit guest receiver callback for this instance.
            target = new Intent(intent);
            target.setComponent(mRegisteredReceiver);
            target.setPackage(null);
            userId = mRegisteredUserId;
        } else {
            return;
        }
        PendingResult pendingResult = goAsync();
        try {
            BlackBoxCore.getBActivityManager().scheduleBroadcastReceiver(target, new PendingResultData(pendingResult), userId);
        } catch (RemoteException e) {
            pendingResult.finish();
        }
    }
}
