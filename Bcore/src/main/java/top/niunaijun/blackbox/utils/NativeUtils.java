package top.niunaijun.blackbox.utils;

import android.os.Build;
import android.util.Log;

import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.util.Enumeration;
import java.util.Set;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;

import top.niunaijun.blackbox.BlackBoxCore;


public class NativeUtils {
    public static final String TAG = "VirtualM";

    public static void copyNativeLib(File apk, File nativeLibDir) throws Exception {
        long startTime = System.currentTimeMillis();
        if (!nativeLibDir.exists() && !nativeLibDir.mkdirs())
            throw new java.io.IOException("Unable to create native library directory: " + nativeLibDir);
        try (ZipFile zipfile = new ZipFile(apk.getAbsolutePath())) {
            Set<String> packagedAbis = AbiUtils.packagedAbis(zipfile);
            String abi = AbiUtils.selectAbi(packagedAbis, BlackBoxCore.is64Bit(), Build.SUPPORTED_ABIS);
            if (abi == null) {
                if (!packagedAbis.isEmpty()) throw new java.io.IOException(
                        "APK native ABIs " + packagedAbis + " cannot run in this " +
                                (BlackBoxCore.is64Bit() ? "64-bit" : "32-bit") + " SlackBox process");
                return;
            }
            copyAbi(zipfile, abi, nativeLibDir);
        } finally {
            Log.d(TAG, "Done! +" + (System.currentTimeMillis() - startTime) + "ms");
        }
    }

    private static void copyAbi(ZipFile zipfile, String cpuArch, File nativeLibDir) throws Exception {
        Log.d(TAG, "Copying guest native libraries for ABI: " + cpuArch);
        String libPrefix = "lib/" + cpuArch + "/";
        byte[] buffer = new byte[16 * 1024];
        Enumeration<? extends ZipEntry> entries = zipfile.entries();
        while (entries.hasMoreElements()) {
            ZipEntry entry = entries.nextElement();
            String entryName = entry.getName();
            if (entry.isDirectory() || !entryName.startsWith(libPrefix) || !entryName.endsWith(".so")) continue;
            String libName = entryName.substring(libPrefix.length());
            if (libName.isEmpty() || libName.contains("/")) continue;
            File libFile = new File(nativeLibDir, libName);
            Log.d(TAG, "Extracting " + entryName);
            try (InputStream input = zipfile.getInputStream(entry);
                 FileOutputStream output = new FileOutputStream(libFile)) {
                int count;
                while ((count = input.read(buffer)) != -1) output.write(buffer, 0, count);
            }
        }
    }
}
