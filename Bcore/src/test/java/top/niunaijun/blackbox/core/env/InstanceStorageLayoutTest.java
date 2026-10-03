package top.niunaijun.blackbox.core.env;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertNotEquals;
import static org.junit.Assert.assertThrows;

import java.io.File;

import org.junit.Rule;
import org.junit.Test;
import org.junit.rules.TemporaryFolder;

public class InstanceStorageLayoutTest {
    @Rule
    public final TemporaryFolder temporaryFolder = new TemporaryFolder();

    @Test
    public void packageRootsAreDifferentAcrossVirtualUsers() throws Exception {
        File virtualRoot = temporaryFolder.newFolder("private");
        File externalRoot = temporaryFolder.newFolder("external");
        InstanceStorageLayout layout = new InstanceStorageLayout(virtualRoot, externalRoot);

        assertEquals(new File(virtualRoot, "data/user/4/com.example.app"),
                layout.packageData("com.example.app", 4));
        assertNotEquals(layout.packageData("com.example.app", 4),
                layout.packageData("com.example.app", 9));
        assertNotEquals(layout.packageDeviceData("com.example.app", 4),
                layout.packageDeviceData("com.example.app", 9));
        assertNotEquals(layout.packageExternalData("com.example.app", 4),
                layout.packageExternalData("com.example.app", 9));
    }

    @Test
    public void externalPackageRootsDoNotOverlapWithinAnInstance() throws Exception {
        InstanceStorageLayout layout = new InstanceStorageLayout(
                temporaryFolder.newFolder("private"), temporaryFolder.newFolder("external"));

        assertNotEquals(layout.packageExternalData("com.example.first", 2),
                layout.packageExternalData("com.example.second", 2));
        assertEquals(new File(layout.externalUserRoot(2), "Android/obb/com.example.first"),
                layout.packageObb("com.example.first", 2));
    }

    @Test
    public void rejectsInvalidUserIdsAndPathLikePackageNames() throws Exception {
        InstanceStorageLayout layout = new InstanceStorageLayout(
                temporaryFolder.newFolder("private"), temporaryFolder.newFolder("external"));

        assertThrows(IllegalArgumentException.class, () -> layout.credentialUserRoot(-1));
        assertThrows(IllegalArgumentException.class, () -> layout.packageData("../escape", 0));
        assertThrows(IllegalArgumentException.class, () -> layout.packageExternalData("", 0));
    }

    @Test
    public void unavailableExternalStorageFallsBackToPrivateVirtualRoot() throws Exception {
        File virtualRoot = temporaryFolder.newFolder("private");
        InstanceStorageLayout layout = new InstanceStorageLayout(virtualRoot, null);

        assertEquals(new File(virtualRoot, "external-fallback/storage/emulated/3"),
                layout.externalUserRoot(3));
    }
}
