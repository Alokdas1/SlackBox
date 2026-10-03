package top.niunaijun.blackbox.core.system.user;

import android.os.Parcel;
import android.os.RemoteException;

import androidx.core.util.AtomicFile;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;

import top.niunaijun.blackbox.core.env.BEnvironment;
import top.niunaijun.blackbox.core.system.am.BActivityManagerService;
import top.niunaijun.blackbox.core.system.ISystemService;
import top.niunaijun.blackbox.core.system.pm.BPackageManagerService;
import top.niunaijun.blackbox.utils.CloseUtils;
import top.niunaijun.blackbox.utils.FileUtils;


public class BUserManagerService extends IBUserManagerService.Stub implements ISystemService {
    private static BUserManagerService sService = new BUserManagerService();
    public final HashMap<Integer, BUserInfo> mUsers = new HashMap<>();
    public final Object mUserLock = new Object();

    public static BUserManagerService get() {
        return sService;
    }

    @Override
    public void systemReady() {
        scanUserL();
    }

    @Override
    public BUserInfo getUserInfo(int userId) {
        synchronized (mUserLock) {
            return mUsers.get(userId);
        }
    }

    @Override
    public boolean exists(int userId) {
        synchronized (mUsers) {
            return mUsers.get(userId) != null;
        }
    }

    @Override
    public BUserInfo createUser(int userId) throws RemoteException {
        synchronized (mUserLock) {
            if (exists(userId)) {
                return getUserInfo(userId);
            }
            return createUserLocked(userId);
        }
    }

    @Override
    public boolean renameUser(int userId, String name) {
        if (name == null) {
            return false;
        }
        String normalizedName = name.trim();
        if (normalizedName.isEmpty() || normalizedName.length() > 64) {
            return false;
        }
        synchronized (mUserLock) {
            BUserInfo user = mUsers.get(userId);
            if (user == null) {
                return false;
            }
            user.name = normalizedName;
            saveUserInfoLocked();
            return true;
        }
    }

    @Override
    public List<BUserInfo> getUsers() {
        synchronized (mUsers) {
            ArrayList<BUserInfo> bUsers = new ArrayList<>();
            for (BUserInfo value : mUsers.values()) {
                if (value.id >= 0) {
                    bUsers.add(value);
                }
            }
            return bUsers;
        }
    }

    public List<BUserInfo> getAllUsers() {
        synchronized (mUsers) {
            return new ArrayList<>(mUsers.values());
        }
    }

    @Override
    public void deleteUser(int userId) throws RemoteException {
        synchronized (mUserLock) {
            BPackageManagerService.get().deleteUser(userId);
            synchronized (mUsers) {
                mUsers.remove(userId);
                saveUserInfoLocked();
            }
            BActivityManagerService.get().removeUser(userId);
            FileUtils.deleteDir(BEnvironment.getUserDir(userId));
            FileUtils.deleteDir(BEnvironment.getUserDeDir(userId));
            FileUtils.deleteDir(BEnvironment.getExternalUserDir(userId));
            FileUtils.deleteDir(BEnvironment.getInstanceMetadataDir(userId).getParentFile());
        }
    }

    private BUserInfo createUserLocked(int userId) {
        BUserInfo bUserInfo = new BUserInfo();
        bUserInfo.id = userId;
        bUserInfo.status = BUserStatus.ENABLE;
        bUserInfo.name = "Instance " + userId;
        bUserInfo.createTime = System.currentTimeMillis();
        FileUtils.mkdirs(BEnvironment.getUserDir(userId));
        FileUtils.mkdirs(BEnvironment.getUserDeDir(userId));
        FileUtils.mkdirs(BEnvironment.getExternalUserDir(userId));
        FileUtils.mkdirs(BEnvironment.getInstanceMetadataDir(userId));
        mUsers.put(userId, bUserInfo);
        synchronized (mUsers) {
            saveUserInfoLocked();
        }
        return bUserInfo;
    }

    private void saveUserInfoLocked() {
        Parcel parcel = Parcel.obtain();
        AtomicFile atomicFile = new AtomicFile(BEnvironment.getUserInfoConf());
        FileOutputStream fileOutputStream = null;
        try {
            ArrayList<BUserInfo> bUsers = new ArrayList<>(mUsers.values());
            parcel.writeTypedList(bUsers);
            try {
                fileOutputStream = atomicFile.startWrite();
                FileUtils.writeParcelToOutput(parcel, fileOutputStream);
                atomicFile.finishWrite(fileOutputStream);
            } catch (IOException e) {
                e.printStackTrace();
                atomicFile.failWrite(fileOutputStream);
            } finally {
                CloseUtils.close(fileOutputStream);
            }
        } finally {
            parcel.recycle();
        }
    }

    private void scanUserL() {
        synchronized (mUserLock) {
            Parcel parcel = Parcel.obtain();
            InputStream is = null;
            try {
                File userInfoConf = BEnvironment.getUserInfoConf();
                if (!userInfoConf.exists()) {
                    return;
                }
                is = new FileInputStream(BEnvironment.getUserInfoConf());
                byte[] bytes = FileUtils.toByteArray(is);
                parcel.unmarshall(bytes, 0, bytes.length);
                parcel.setDataPosition(0);

                ArrayList<BUserInfo> loadUsers = parcel.createTypedArrayList(BUserInfo.CREATOR);
                if (loadUsers == null)
                    return;
                synchronized (mUsers) {
                    mUsers.clear();
                    boolean migrated = false;
                    for (BUserInfo loadUser : loadUsers) {
                        if (loadUser.name == null || loadUser.name.trim().isEmpty()) {
                            loadUser.name = "Instance " + loadUser.id;
                            migrated = true;
                        }
                        if (loadUser.createTime <= 0L) {
                            loadUser.createTime = System.currentTimeMillis();
                            migrated = true;
                        }
                        FileUtils.mkdirs(BEnvironment.getUserDir(loadUser.id));
                        FileUtils.mkdirs(BEnvironment.getUserDeDir(loadUser.id));
                        FileUtils.mkdirs(BEnvironment.getExternalUserDir(loadUser.id));
                        FileUtils.mkdirs(BEnvironment.getInstanceMetadataDir(loadUser.id));
                        mUsers.put(loadUser.id, loadUser);
                    }
                    if (migrated) {
                        saveUserInfoLocked();
                    }
                }
            } catch (Exception e) {
                e.printStackTrace();
            } finally {
                parcel.recycle();
                CloseUtils.close(is);
            }
        }
    }
}
