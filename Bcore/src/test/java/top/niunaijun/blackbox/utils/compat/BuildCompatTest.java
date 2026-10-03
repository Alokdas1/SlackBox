package top.niunaijun.blackbox.utils.compat;

import org.junit.Test;

import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertTrue;

public class BuildCompatTest {
    @Test
    public void stableReleaseBoundariesMatchAndroidApiLevels() {
        assertFalse(BuildCompat.isTiramisu(32, 0));
        assertTrue(BuildCompat.isTiramisu(33, 0));
        assertFalse(BuildCompat.isU(33, 0));
        assertTrue(BuildCompat.isU(34, 0));
    }

    @Test
    public void previewSdkUsesThePreviewApiLevel() {
        assertTrue(BuildCompat.isTiramisu(32, 1));
        assertFalse(BuildCompat.isU(32, 1));
        assertTrue(BuildCompat.isU(33, 1));
    }

    @Test
    public void android16UsesBothNewerCompatibilityBranches() {
        assertTrue(BuildCompat.isTiramisu(36, 0));
        assertTrue(BuildCompat.isU(36, 0));
    }
}
