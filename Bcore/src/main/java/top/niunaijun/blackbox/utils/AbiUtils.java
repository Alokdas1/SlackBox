package top.niunaijun.blackbox.utils;

import java.io.File;
import java.util.Enumeration;
import java.util.HashSet;
import java.util.Set;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;

import top.niunaijun.blackbox.BlackBoxCore;

/** Selects native libraries that can be loaded by SlackBox's own process ABI. */
public final class AbiUtils {
    private static final String ARM64 = "arm64-v8a";
    private static final String ARM32 = "armeabi-v7a";
    private static final String LEGACY_ARM32 = "armeabi";

    private AbiUtils() { }

    public static boolean isSupport(File apkFile) {
        if (apkFile == null || !apkFile.isFile()) return false;
        try (ZipFile archive = new ZipFile(apkFile)) {
            Set<String> packagedAbis = packagedAbis(archive);
            return packagedAbis.isEmpty() || selectAbi(packagedAbis,
                    BlackBoxCore.is64Bit(), new String[]{ARM64, ARM32, LEGACY_ARM32}) != null;
        } catch (Exception e) {
            return false;
        }
    }

    static Set<String> packagedAbis(ZipFile archive) {
        Set<String> abis = new HashSet<>();
        Enumeration<? extends ZipEntry> entries = archive.entries();
        while (entries.hasMoreElements()) {
            String name = entries.nextElement().getName();
            if (!name.startsWith("lib/")) continue;
            int end = name.indexOf('/', 4);
            if (end < 0 || !name.endsWith(".so")) continue;
            String abi = name.substring(4, end);
            if (!abi.isEmpty()) abis.add(abi);
        }
        return abis;
    }

    static String selectAbi(Set<String> packagedAbis, boolean process64Bit, String[] supportedAbis) {
        for (String abi : supportedAbis) {
            if (!ARM64.equals(abi) && !ARM32.equals(abi) && !LEGACY_ARM32.equals(abi)) continue;
            if (process64Bit != ARM64.equals(abi)) continue;
            if (packagedAbis.contains(abi)) return abi;
        }
        if (!process64Bit && packagedAbis.contains(LEGACY_ARM32)) return LEGACY_ARM32;
        return null;
    }
}
