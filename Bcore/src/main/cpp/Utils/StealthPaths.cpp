#include "StealthPaths.h"

#include <atomic>
#include <cstdlib>
#include <cstring>

#include <android/log.h>

#define LOG_TAG "StealthPaths"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)

// Entries are stored in fixed-capacity storage rather than a std::vector so
// that isBlocked() touches no allocator. A guest thread can call isBlocked()
// from inside malloc, from a signal handler, or from a thread that already
// holds the allocator lock; any of those would deadlock against vector growth.
namespace {

constexpr unsigned kMaxEntries = 128;
constexpr unsigned kMaxEntryLength = 160;

// su binaries. A guest that can stat() these has root.
const char *const kBlockedFiles[] = {
    "/system/xbin/su",
    "/system/bin/su",
    "/system/sbin/su",
    "/sbin/su",
    "/su/bin/su",
    "/system/bin/failsafe/su",
    "/system/sd/xbin/su",
    "/system/xbin/daemonsu",
    "/system/xbin/sugote",
    "/system/xbin/sugote-mksh",
    "/system/bin/sugote-mksh",
    "/data/local/xbin/su",
    "/data/local/bin/su",
    "/data/local/su",
    "/data/local/tmp/su",
    "/system/app/Superuser.apk",
    "/system/app/SuperSU.apk",
    "/system/etc/init.d/99SuperSUDaemon",

    // Magisk.
    "/system/bin/magisk",
    "/system/xbin/magisk",
    "/sbin/magisk",
    "/data/adb/magisk",

    // Emulator indicators. Present on no physical device.
    "/dev/vboxguest",
    "/dev/vboxuser",
    "/dev/qemu_pipe",
    "/dev/goldfish_pipe",
    "/dev/goldfish_events",
    "/dev/socket/qemud",
    "/dev/socket/genyd",
    "/dev/socket/baseband_genyd",
    "/sys/qemu_trace",
    "/sys/module/goldfish_audio",
    "/sys/module/goldfish_sync",
    "/proc/tty/drivers/goldfish",
    "/system/lib/libc_malloc_debug_qemu.so",
    "/system/lib/libdroid4x.so",
    "/system/lib/libnoxspeedup.so",
    "/system/lib/libmemu.so",
    "/system/lib/libbluelog.so",
    "/system/bin/qemu-props",
    "/system/bin/nox-prop",
    "/system/bin/windroyed",

    // Xposed / EdXposed / Dreamland / Substrate. A guest that can see these
    // knows the process is instrumented.
    "/system/xposed.prop",
    "/system/framework/XposedBridge.jar",
    "/system/framework/XposedBridge.dex",
};

// Installed-package blocklist, matched against any path component so that
// /data/data/<pkg>, /data/user/0/<pkg> and the package-manager data
// directories are all covered by one entry.
const char *const kBlockedPackages[] = {
    // Superuser variants.
    "com.noshufou.android.su",
    "com.noshufou.android.su.elite",
    "eu.chainfire.supersu",
    "eu.chainfire.supersu.pro",
    "com.koushikdutta.superuser",
    "com.thirdparty.superuser",
    "com.yellowes.su",
    "me.phh.superuser",
    "com.kingouser.com",
    "com.topjohnwu.magisk",
    "com.koushikdutta.rommanager",

    // Root cloaking.
    "com.dimonvideo.luckypatcher",
    "com.chelpus.lackypatch",
    "com.ramdroid.appquarantine",
    "com.ramdroid.appquarantinepro",
    "com.devadvance.rootcloak",
    "com.devadvance.rootcloakplus",
    "com.amphoras.hidemyroot",
    "com.amphoras.hidemyrootadfree",
    "com.formyhm.hideroot",
    "com.formyhm.hiderootPremium",
    "com.zachspong.temprootremovejb",

    // Hooking frameworks.
    "de.robv.android.xposed.installer",
    "org.meowcat.edxposed.manager",
    "top.canyie.dreamland.manager",
    "com.saurik.substrate",

    // Competing virtualization containers. Seeing these means the guest
    // resolved a container package instead of running standalone, which also
    // identifies which container this is.
    "com.lody.virtual",
    "com.lbe.parallel",
    "com.dual.dualspace",
    "com.excelliance.dualaid",
    "io.va.exposed",
    "com.benny.openlauncher",
};

struct EntryTable {
    const char *const *builtin;
    unsigned builtinCount;
    char (*runtime)[kMaxEntryLength];
    std::atomic<unsigned> runtimeCount{0};
    unsigned capacity;
};

EntryTable gFileTable = {kBlockedFiles, sizeof(kBlockedFiles) / sizeof(kBlockedFiles[0]),
                         nullptr, std::atomic<unsigned>(0), 0};
EntryTable gPackageTable = {kBlockedPackages, sizeof(kBlockedPackages) / sizeof(kBlockedPackages[0]),
                            nullptr, std::atomic<unsigned>(0), 0};

void *g_runtimeStorage[kMaxEntries * 2] = {nullptr};

void ensureStorage(EntryTable &table, unsigned slot) {
    if (table.runtime != nullptr) {
        return;
    }
    table.runtime = static_cast<char (*)[kMaxEntryLength]>(
        calloc(table.capacity == 0 ? kMaxEntries : table.capacity, kMaxEntryLength));
    table.capacity = table.capacity == 0 ? kMaxEntries : table.capacity;
    if (table.runtime != nullptr) {
        g_runtimeStorage[slot] = table.runtime;
    }
}

void addEntry(EntryTable &table, unsigned storageSlot, const char *entry) {
    if (entry == nullptr || entry[0] == '\0' || strlen(entry) >= kMaxEntryLength) {
        return;
    }
    ensureStorage(table, storageSlot);
    if (table.runtime == nullptr) {
        LOGD("runtime table unavailable, ignoring entry");
        return;
    }
    unsigned index = table.runtimeCount.load(std::memory_order_relaxed);
    if (index >= table.capacity) {
        LOGD("runtime table full (%u entries), ignoring entry", table.capacity);
        return;
    }
    strncpy(table.runtime[index], entry, kMaxEntryLength - 1);
    table.runtime[index][kMaxEntryLength - 1] = '\0';
    table.runtimeCount.store(index + 1, std::memory_order_release);
}

bool tableIsBlocked(const EntryTable &table, const char *path) {
    for (unsigned i = 0; i < table.builtinCount; ++i) {
        if (StealthPaths::matchesComponent(path, table.builtin[i])) {
            return true;
        }
    }
    unsigned runtimeCount = table.runtimeCount.load(std::memory_order_acquire);
    if (table.runtime != nullptr) {
        for (unsigned i = 0; i < runtimeCount; ++i) {
            if (StealthPaths::matchesComponent(path, table.runtime[i])) {
                return true;
            }
        }
    }
    return false;
}

}  // namespace

// An entry matches when it occurs in `path` at a component boundary: either the
// whole path, or preceded by '/' with the next character also '/' or the
// terminator. Comparing this way is what stops "/data/data/com.lody.virtual"
// from also hiding "/data/data/com.lody.virtualclone".
bool StealthPaths::matchesComponent(const char *path, const char *entry) {
    if (path == nullptr || entry == nullptr) {
        return false;
    }
    size_t entryLength = strlen(entry);
    if (entryLength == 0) {
        return false;
    }

    const char *cursor = path;
    while (true) {
        const char *hit = strstr(cursor, entry);
        if (hit == nullptr) {
            return false;
        }
        bool boundaryBefore = (hit == path) || (hit[-1] == '/');
        char after = hit[entryLength];
        bool boundaryAfter = (after == '\0') || (after == '/');
        if (boundaryBefore && boundaryAfter) {
            return true;
        }
        cursor = hit + 1;
        if (*cursor == '\0') {
            return false;
        }
    }
}

bool StealthPaths::isBlocked(const char *path) {
    if (path == nullptr || path[0] == '\0') {
        return false;
    }
    return tableIsBlocked(gFileTable, path) || tableIsBlocked(gPackageTable, path);
}

void StealthPaths::blockFile(const char *path) {
    addEntry(gFileTable, 0, path);
}

void StealthPaths::blockPackage(const char *packageName) {
    addEntry(gPackageTable, 1, packageName);
}

unsigned StealthPaths::blockedFileCount() {
    return gFileTable.builtinCount + gFileTable.runtimeCount.load(std::memory_order_acquire);
}

unsigned StealthPaths::blockedPackageCount() {
    return gPackageTable.builtinCount + gPackageTable.runtimeCount.load(std::memory_order_acquire);
}
