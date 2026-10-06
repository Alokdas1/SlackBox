#include "FileSystemHook.h"

#include <Log.h>
#include <IO.h>
#include <Utils/StealthPaths.h>

#include "Dobby/dobby.h"
#include "xdl.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

// libc interception for guest processes.
//
// Before this file, the hooks were resolved but never installed: init() called
// xdl_sym() into orig_open/orig_open64, logged the addresses, and returned.
// Nothing was patched, so a guest calling open() or stat() from native code
// reached the real libc and saw the host filesystem exactly as it is. The Java
// path was covered -- UnixFileSystemHook patches the java.io.UnixFileSystem
// natives, so java.io.File went through IOCore -- but the majority of
// commercial detection SDKs read the filesystem from native code precisely
// because the Java surface is the one a container is expected to intercept.
//
// Every interceptor runs the same three steps in order:
//
//   1. Ask StealthPaths whether the path must appear not to exist. If so,
//      report ENOENT without touching the real filesystem.
//   2. Otherwise run IOCore's relocation rules, so native callers get the same
//      virtual data root, /sdcard redirect and /proc/self/cmdline rewrite that
//      java.io callers get.
//   3. Forward to the original function.
//
// Step 2 must not run before step 1. A blocked path is reported absent; if it
// were relocated first, /data/data/com.lody.virtual would be rewritten into the
// instance root and would then resolve to a real directory, turning an
// intended ENOENT into a successful open.
//
// The relocation rules are read from a std::list that the host populates
// during setup. That is not synchronized, so these hooks are only correct once
// IOCore.enableRedirect() has finished -- which is the case by the time
// NativeCore.enableIO() runs, since it is the last statement there. Guests
// start after that.

// The original pointers are null until DobbyHook succeeds. Every interceptor
// checks before use: a null orig means the symbol was absent from this
// platform's libc, and calling through a null pointer would take the guest
// down. Returning ENOENT in that case is the conservative answer -- it fails
// the individual call rather than crashing the process the guest is running in.

namespace {

void *resolveLibc(const char *symbol) {
    void *handle = xdl_open("libc.so", XDL_DEFAULT);
    if (handle == nullptr) {
        return nullptr;
    }
    void *target = xdl_dsym(handle, symbol, nullptr);
    xdl_close(handle);
    return target;
}

// open/openat take a mode_t only when a flag requires it. Reading the vararg
// unconditionally is undefined on platforms where the caller did not pass one;
// on AArch64 it usually reads a register the caller never set, so it yields
// garbage rather than a crash. Pass a real mode only when asked.
bool modeRequired(int flags) {
#ifdef O_TMPFILE
    if ((flags & O_TMPFILE) == O_TMPFILE) {
        return true;
    }
#endif
    return (flags & O_CREAT) != 0;
}

}  // namespace

// --- open / openat family ----------------------------------------------------

static int (*orig_open)(const char *pathname, int flags, ...) = nullptr;
static int (*orig_open64)(const char *pathname, int flags, ...) = nullptr;
static int (*orig_openat)(int dirfd, const char *pathname, int flags, ...) = nullptr;
static int (*orig_openat64)(int dirfd, const char *pathname, int flags, ...) = nullptr;

int new_open(const char *pathname, int flags, ...) {
    if (StealthPaths::isBlocked(pathname)) {
        errno = ENOENT;
        return -1;
    }
    bool owned = false;
    const char *redirected = IO::redirectPath(pathname, &owned);
    if (orig_open == nullptr) {
        errno = ENOENT;
        return -1;
    }
    va_list args;
    va_start(args, flags);
    mode_t mode = modeRequired(flags) ? va_arg(args, mode_t) : 0;
    va_end(args);
    int result;
    if (modeRequired(flags)) {
        result = orig_open(redirected, flags, mode);
    } else {
        result = orig_open(redirected, flags);
    }
    // IO::redirectPath returns either the input pointer or a freshly malloc'd
    // replacement. Only the latter is ours to release.
    if (owned) {
        free((void *) redirected);
    }
    return result;
}

int new_open64(const char *pathname, int flags, ...) {
    if (StealthPaths::isBlocked(pathname)) {
        errno = ENOENT;
        return -1;
    }
    bool owned = false;
    const char *redirected = IO::redirectPath(pathname, &owned);
    if (orig_open64 == nullptr) {
        errno = ENOENT;
        return -1;
    }
    va_list args;
    va_start(args, flags);
    mode_t mode = modeRequired(flags) ? va_arg(args, mode_t) : 0;
    va_end(args);
    int result;
    if (modeRequired(flags)) {
        result = orig_open64(redirected, flags, mode);
    } else {
        result = orig_open64(redirected, flags);
    }
    if (owned) {
        free((void *) redirected);
    }
    return result;
}

int new_openat(int dirfd, const char *pathname, int flags, ...) {
    if (StealthPaths::isBlocked(pathname)) {
        errno = ENOENT;
        return -1;
    }
    bool owned = false;
    const char *redirected = IO::redirectPath(pathname, &owned);
    if (orig_openat == nullptr) {
        errno = ENOENT;
        return -1;
    }
    va_list args;
    va_start(args, flags);
    mode_t mode = modeRequired(flags) ? va_arg(args, mode_t) : 0;
    va_end(args);
    int result;
    if (modeRequired(flags)) {
        result = orig_openat(dirfd, redirected, flags, mode);
    } else {
        result = orig_openat(dirfd, redirected, flags);
    }
    if (owned) {
        free((void *) redirected);
    }
    return result;
}

int new_openat64(int dirfd, const char *pathname, int flags, ...) {
    if (StealthPaths::isBlocked(pathname)) {
        errno = ENOENT;
        return -1;
    }
    bool owned = false;
    const char *redirected = IO::redirectPath(pathname, &owned);
    if (orig_openat64 == nullptr) {
        errno = ENOENT;
        return -1;
    }
    va_list args;
    va_start(args, flags);
    mode_t mode = modeRequired(flags) ? va_arg(args, mode_t) : 0;
    va_end(args);
    int result;
    if (modeRequired(flags)) {
        result = orig_openat64(dirfd, redirected, flags, mode);
    } else {
        result = orig_openat64(dirfd, redirected, flags);
    }
    if (owned) {
        free((void *) redirected);
    }
    return result;
}

// --- stat family -------------------------------------------------------------
//
// bionic implements the stat() family through inline wrappers around
// __xstat/__lxstat/__fxstatat on older releases and through direct symbols on
// newer ones, so both spellings are installed where present. On API 24+
// (APP_PLATFORM) the __xstat variants are gone and the plain names resolve.

static int (*orig_stat)(const char *pathname, struct stat *buf) = nullptr;
static int (*orig_lstat)(const char *pathname, struct stat *buf) = nullptr;
static int (*orig_fstatat)(int dirfd, const char *pathname, struct stat *buf, int flags) = nullptr;
static int (*orig_xstat)(int ver, const char *pathname, struct stat *buf) = nullptr;
static int (*orig_lxstat)(int ver, const char *pathname, struct stat *buf) = nullptr;
static int (*orig_fxstatat)(int ver, int dirfd, const char *pathname, struct stat *buf, int flags) = nullptr;

int new_stat(const char *pathname, struct stat *buf) {
    if (StealthPaths::isBlocked(pathname)) {
        errno = ENOENT;
        return -1;
    }
    bool owned = false;
    const char *redirected = IO::redirectPath(pathname, &owned);
    int result = orig_stat != nullptr ? orig_stat(redirected, buf) : -1;
    if (owned) {
        free((void *) redirected);
    }
    return result;
}

int new_lstat(const char *pathname, struct stat *buf) {
    if (StealthPaths::isBlocked(pathname)) {
        errno = ENOENT;
        return -1;
    }
    bool owned = false;
    const char *redirected = IO::redirectPath(pathname, &owned);
    int result = orig_lstat != nullptr ? orig_lstat(redirected, buf) : -1;
    if (owned) {
        free((void *) redirected);
    }
    return result;
}

int new_fstatat(int dirfd, const char *pathname, struct stat *buf, int flags) {
    if (StealthPaths::isBlocked(pathname)) {
        errno = ENOENT;
        return -1;
    }
    bool owned = false;
    const char *redirected = IO::redirectPath(pathname, &owned);
    int result = orig_fstatat != nullptr ? orig_fstatat(dirfd, redirected, buf, flags) : -1;
    if (owned) {
        free((void *) redirected);
    }
    return result;
}

int new_xstat(int ver, const char *pathname, struct stat *buf) {
    if (StealthPaths::isBlocked(pathname)) {
        errno = ENOENT;
        return -1;
    }
    bool owned = false;
    const char *redirected = IO::redirectPath(pathname, &owned);
    int result = orig_xstat != nullptr ? orig_xstat(ver, redirected, buf) : -1;
    if (owned) {
        free((void *) redirected);
    }
    return result;
}

int new_lxstat(int ver, const char *pathname, struct stat *buf) {
    if (StealthPaths::isBlocked(pathname)) {
        errno = ENOENT;
        return -1;
    }
    bool owned = false;
    const char *redirected = IO::redirectPath(pathname, &owned);
    int result = orig_lxstat != nullptr ? orig_lxstat(ver, redirected, buf) : -1;
    if (owned) {
        free((void *) redirected);
    }
    return result;
}

int new_fxstatat(int ver, int dirfd, const char *pathname, struct stat *buf, int flags) {
    if (StealthPaths::isBlocked(pathname)) {
        errno = ENOENT;
        return -1;
    }
    bool owned = false;
    const char *redirected = IO::redirectPath(pathname, &owned);
    int result = orig_fxstatat != nullptr ? orig_fxstatat(ver, dirfd, redirected, buf, flags) : -1;
    if (owned) {
        free((void *) redirected);
    }
    return result;
}

// --- access ------------------------------------------------------------------

static int (*orig_access)(const char *pathname, int mode) = nullptr;
static int (*orig_faccessat)(int dirfd, const char *pathname, int mode, int flags) = nullptr;

int new_access(const char *pathname, int mode) {
    if (StealthPaths::isBlocked(pathname)) {
        errno = ENOENT;
        return -1;
    }
    bool owned = false;
    const char *redirected = IO::redirectPath(pathname, &owned);
    int result = orig_access != nullptr ? orig_access(redirected, mode) : -1;
    if (owned) {
        free((void *) redirected);
    }
    return result;
}

int new_faccessat(int dirfd, const char *pathname, int mode, int flags) {
    if (StealthPaths::isBlocked(pathname)) {
        errno = ENOENT;
        return -1;
    }
    bool owned = false;
    const char *redirected = IO::redirectPath(pathname, &owned);
    int result = orig_faccessat != nullptr ? orig_faccessat(dirfd, redirected, mode, flags) : -1;
    if (owned) {
        free((void *) redirected);
    }
    return result;
}

// --- fopen / fdopen ----------------------------------------------------------

static FILE *(*orig_fopen)(const char *pathname, const char *mode) = nullptr;
static FILE *(*orig_fopen64)(const char *pathname, const char *mode) = nullptr;

FILE *new_fopen(const char *pathname, const char *mode) {
    if (StealthPaths::isBlocked(pathname)) {
        errno = ENOENT;
        return nullptr;
    }
    bool owned = false;
    const char *redirected = IO::redirectPath(pathname, &owned);
    FILE *result = orig_fopen != nullptr ? orig_fopen(redirected, mode) : nullptr;
    if (owned) {
        free((void *) redirected);
    }
    return result;
}

FILE *new_fopen64(const char *pathname, const char *mode) {
    if (StealthPaths::isBlocked(pathname)) {
        errno = ENOENT;
        return nullptr;
    }
    bool owned = false;
    const char *redirected = IO::redirectPath(pathname, &owned);
    FILE *result = orig_fopen64 != nullptr ? orig_fopen64(redirected, mode) : nullptr;
    if (owned) {
        free((void *) redirected);
    }
    return result;
}

// --- opendir -----------------------------------------------------------------

static DIR *(*orig_opendir)(const char *name) = nullptr;

DIR *new_opendir(const char *name) {
    if (StealthPaths::isBlocked(name)) {
        errno = ENOENT;
        return nullptr;
    }
    bool owned = false;
    const char *redirected = IO::redirectPath(name, &owned);
    DIR *result = orig_opendir != nullptr ? orig_opendir(redirected) : nullptr;
    if (owned) {
        free((void *) redirected);
    }
    return result;
}

// --- readlink ----------------------------------------------------------------

static ssize_t (*orig_readlink)(const char *pathname, char *buf, size_t bufsiz) = nullptr;
static ssize_t (*orig_readlinkat)(int dirfd, const char *pathname, char *buf, size_t bufsiz) = nullptr;

ssize_t new_readlink(const char *pathname, char *buf, size_t bufsiz) {
    if (StealthPaths::isBlocked(pathname)) {
        errno = ENOENT;
        return -1;
    }
    bool owned = false;
    const char *redirected = IO::redirectPath(pathname, &owned);
    ssize_t result = orig_readlink != nullptr ? orig_readlink(redirected, buf, bufsiz) : -1;
    if (owned) {
        free((void *) redirected);
    }
    return result;
}

ssize_t new_readlinkat(int dirfd, const char *pathname, char *buf, size_t bufsiz) {
    if (StealthPaths::isBlocked(pathname)) {
        errno = ENOENT;
        return -1;
    }
    bool owned = false;
    const char *redirected = IO::redirectPath(pathname, &owned);
    ssize_t result = orig_readlinkat != nullptr ? orig_readlinkat(dirfd, redirected, buf, bufsiz) : -1;
    if (owned) {
        free((void *) redirected);
    }
    return result;
}

// --- installation ------------------------------------------------------------

namespace {

// Counts installed hooks so start-up logging can state what actually landed
// rather than claiming a blanket success. A missing symbol is normal across
// API levels; a DobbyHook failure is not, and silently continuing there is how
// the previous version of this file ended up pretending to work.
unsigned g_installed = 0;
unsigned g_requested = 0;

void installOne(const char *name, void *symbol, void *replacement, void **origOut) {
    if (symbol == nullptr) {
        ALOGD("FileSystemHook: %s not present in libc, skipping", name);
        return;
    }
    ++g_requested;
    if (DobbyHook(symbol, replacement, origOut) != 0) {
        ALOGE("FileSystemHook: failed to hook %s", name);
        return;
    }
    ++g_installed;
    ALOGD("FileSystemHook: hooked %s", name);
}

}  // namespace

void FileSystemHook::init() {
    ALOGD("FileSystemHook: installing libc interception (blocklist: %u files, %u packages)",
          StealthPaths::blockedFileCount(), StealthPaths::blockedPackageCount());

    installOne("open", resolveLibc("open"), (void *) new_open, (void **) &orig_open);
    installOne("open64", resolveLibc("open64"), (void *) new_open64, (void **) &orig_open64);
    installOne("openat", resolveLibc("openat"), (void *) new_openat, (void **) &orig_openat);
    installOne("openat64", resolveLibc("openat64"), (void *) new_openat64, (void **) &orig_openat64);

    installOne("stat", resolveLibc("stat"), (void *) new_stat, (void **) &orig_stat);
    installOne("lstat", resolveLibc("lstat"), (void *) new_lstat, (void **) &orig_lstat);
    installOne("fstatat", resolveLibc("fstatat"), (void *) new_fstatat, (void **) &orig_fstatat);
    // Legacy bionic spellings. Absent on API 24+, which is this module's floor.
    installOne("__xstat", resolveLibc("__xstat"), (void *) new_xstat, (void **) &orig_xstat);
    installOne("__lxstat", resolveLibc("__lxstat"), (void *) new_lxstat, (void **) &orig_lxstat);
    installOne("__fxstatat", resolveLibc("__fxstatat"), (void *) new_fxstatat, (void **) &orig_fxstatat);

    installOne("access", resolveLibc("access"), (void *) new_access, (void **) &orig_access);
    installOne("faccessat", resolveLibc("faccessat"), (void *) new_faccessat, (void **) &orig_faccessat);

    installOne("fopen", resolveLibc("fopen"), (void *) new_fopen, (void **) &orig_fopen);
    installOne("fopen64", resolveLibc("fopen64"), (void *) new_fopen64, (void **) &orig_fopen64);

    installOne("opendir", resolveLibc("opendir"), (void *) new_opendir, (void **) &orig_opendir);

    installOne("readlink", resolveLibc("readlink"), (void *) new_readlink, (void **) &orig_readlink);
    installOne("readlinkat", resolveLibc("readlinkat"), (void *) new_readlinkat, (void **) &orig_readlinkat);

    ALOGD("FileSystemHook: installed %u of %u requested symbols", g_installed, g_requested);
    if (g_installed == 0) {
        ALOGE("FileSystemHook: no libc symbols were hooked; guest native I/O is unfiltered");
    }
}
