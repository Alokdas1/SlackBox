package top.niunaijun.blackbox.utils;

import org.junit.Test;

import java.util.Arrays;
import java.util.HashSet;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertNull;

public class AbiUtilsTest {
    @Test
    public void prefersArm64For64BitVirtualProcess() {
        assertEquals("arm64-v8a", AbiUtils.selectAbi(
                new HashSet<>(Arrays.asList("arm64-v8a", "armeabi-v7a")), true,
                new String[]{"arm64-v8a", "armeabi-v7a"}));
    }

    @Test
    public void rejects32BitOnlyLibrariesIn64BitProcess() {
        assertNull(AbiUtils.selectAbi(new HashSet<>(Arrays.asList("armeabi-v7a")), true,
                new String[]{"arm64-v8a", "armeabi-v7a"}));
    }

    @Test
    public void selects32BitAbiOnlyFor32BitProcess() {
        assertEquals("armeabi-v7a", AbiUtils.selectAbi(
                new HashSet<>(Arrays.asList("arm64-v8a", "armeabi-v7a")), false,
                new String[]{"arm64-v8a", "armeabi-v7a"}));
    }

    @Test
    public void ignoresUnsupportedHostArchitectures() {
        assertNull(AbiUtils.selectAbi(new HashSet<>(Arrays.asList("x86_64")), true,
                new String[]{"x86_64", "arm64-v8a"}));
    }

    @Test
    public void rejectsApkContainingOnlyX86NativeCode() {
        assertNull(AbiUtils.selectAbi(new HashSet<>(Arrays.asList("x86", "x86_64")), true,
                new String[]{"arm64-v8a", "armeabi-v7a"}));
    }
}
