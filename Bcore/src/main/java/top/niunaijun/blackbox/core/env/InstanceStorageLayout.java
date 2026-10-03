package top.niunaijun.blackbox.core.env;

import java.io.File;

/** Computes the durable host paths assigned to each virtual Android user. */
public final class InstanceStorageLayout {
    private final File virtualRoot;
    private final File externalRoot;

    public InstanceStorageLayout(File virtualRoot, File externalRoot) {
        if (virtualRoot == null) {
            throw new IllegalArgumentException("Virtual storage root must be non-null");
        }
        this.virtualRoot = virtualRoot;
        this.externalRoot = externalRoot;
    }

    public File credentialUserRoot(int userId) {
        return new File(virtualRoot, "data/user/" + requireUserId(userId));
    }

    public File deviceUserRoot(int userId) {
        return new File(virtualRoot, "data/user_de/" + requireUserId(userId));
    }

    public File externalUserRoot(int userId) {
        File root = externalRoot != null ? externalRoot : new File(virtualRoot, "external-fallback");
        return new File(root, "storage/emulated/" + requireUserId(userId));
    }

    public File metadataRoot(int userId) {
        return new File(virtualRoot, "instances/" + requireUserId(userId) + "/metadata");
    }

    public File packageData(String packageName, int userId) {
        return new File(credentialUserRoot(userId), requirePackageName(packageName));
    }

    public File packageDeviceData(String packageName, int userId) {
        return new File(deviceUserRoot(userId), requirePackageName(packageName));
    }

    public File packageExternalData(String packageName, int userId) {
        return new File(externalUserRoot(userId), "Android/data/" + requirePackageName(packageName));
    }

    public File packageObb(String packageName, int userId) {
        return new File(externalUserRoot(userId), "Android/obb/" + requirePackageName(packageName));
    }

    private static int requireUserId(int userId) {
        if (userId < 0) {
            throw new IllegalArgumentException("Virtual user ID must be non-negative");
        }
        return userId;
    }

    private static String requirePackageName(String packageName) {
        if (packageName == null || !packageName.matches("[A-Za-z0-9_]+(\\.[A-Za-z0-9_]+)*")) {
            throw new IllegalArgumentException("Invalid package name");
        }
        return packageName;
    }
}
