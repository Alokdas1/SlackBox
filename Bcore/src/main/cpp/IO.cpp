



#include "IO.h"
#include "Log.h"

#include <cstdlib>
#include <cstring>

jmethodID getAbsolutePathMethodId;

list<IO::RelocateInfo> relocate_rule;

// Returns a malloc'd copy of `str` with every occurrence of `src` replaced by
// `dst`, or NULL on allocation failure. Caller frees.
//
// This had three defects, all of which were latent while the function had no
// caller and became the crash that killed guest startup once the filesystem
// hook layer started routing real I/O through it (SIGSEGV, SEGV_ACCERR, frame
// #00 __memset_aarch64, during com.proxima.dfm's DEX load):
//
//   1. memset(result, 0, strlen(result)) called strlen on the uninitialized
//      buffer malloc had just returned. strlen walked forward until it happened
//      to find a zero byte, which can be far past the end of the allocation,
//      and memset then wrote that many bytes over unrelated heap. That is the
//      fault in the backtrace.
//   2. strlen(dst) - strlen(src) is size_t arithmetic. When dst is shorter than
//      src -- which is the normal case for these relocation rules, e.g.
//      /sdcard -> /storage/emulated/0/... -- the subtraction underflows to a
//      value near SIZE_MAX, result_len wraps, and the sizing arithmetic is
//      meaningless.
//   3. malloc's result was never checked, so a failed allocation was written
//      through unconditionally.
//
// The rewrite below counts non-overlapping occurrences first (so the exact
// length is known up front), then builds the result with an explicit cursor
// instead of repeated strncat/strcat. strncat has to walk the whole buffer to
// find its end on every call, which made the original quadratic in the path
// length; with thousands of intercepted opens during startup that is real time
// on a hot path.
static char *replace(const char *str, const char *src, const char *dst) {
    if (str == nullptr || src == nullptr || dst == nullptr) {
        return nullptr;
    }
    const size_t srcLength = strlen(src);
    if (srcLength == 0) {
        // strstr would match everywhere and the loop would never advance.
        return strdup(str);
    }
    const size_t dstLength = strlen(dst);
    const size_t strLength = strlen(str);

    // Count non-overlapping occurrences. srcLength > 0 guarantees progress.
    size_t occurrences = 0;
    for (const char *scan = str; (scan = strstr(scan, src)) != nullptr; scan += srcLength) {
        ++occurrences;
    }
    if (occurrences == 0) {
        return strdup(str);
    }

    // Written as a subtraction of products rather than a sum of differences:
    // each replacement contributes (dstLength - srcLength), which is negative
    // whenever dst is shorter, and accumulating that into a size_t is exactly
    // the underflow that broke the original. Shrinking and growing are
    // therefore both handled by ordinary subtraction on size_t operands.
    const size_t shrinkPerOccurrence = srcLength > dstLength ? srcLength - dstLength : 0;
    const size_t growPerOccurrence = dstLength > srcLength ? dstLength - srcLength : 0;

    size_t resultLength;
    if (shrinkPerOccurrence > 0 && occurrences > strLength / shrinkPerOccurrence + 1) {
        // Cannot happen with non-overlapping counting, but the check keeps the
        // arithmetic honest if that ever changes.
        return nullptr;
    }
    resultLength = strLength + occurrences * growPerOccurrence;
    if (shrinkPerOccurrence > 0) {
        resultLength -= occurrences * shrinkPerOccurrence;
    }
    resultLength += 1;  // terminator

    char *result = (char *) malloc(resultLength);
    if (result == nullptr) {
        return nullptr;
    }

    char *write = result;
    const char *left = str;
    for (const char *hit; (hit = strstr(left, src)) != nullptr; left = hit + srcLength) {
        const size_t chunk = (size_t) (hit - left);
        memcpy(write, left, chunk);
        write += chunk;
        memcpy(write, dst, dstLength);
        write += dstLength;
    }
    memcpy(write, left, strLength - (size_t) (left - str));
    write += strLength - (size_t) (left - str);
    *write = '\0';
    return result;
}

const char *IO::redirectPath(const char *__path) {
    
    if (strstr(__path, "resource-cache")) {
        ALOGD("Blocking resource-cache path: %s", __path);
        return "/dev/null";
    }
    
    
    if (strstr(__path, "@idmap")) {
        ALOGD("Blocking idmap path: %s", __path);
        return "/dev/null";
    }
    
    
    if (strstr(__path, "systemui") && (strstr(__path, ".frro") || strstr(__path, "-accent-") || strstr(__path, "-dynamic-") || strstr(__path, "-neutral-"))) {
        ALOGD("Blocking systemui problematic path: %s", __path);
        return "/dev/null";
    }
    
    
    if (strstr(__path, "data@resource-cache@")) {
        ALOGD("Blocking data@resource-cache@ pattern: %s", __path);
        return "/dev/null";
    }
    
    
    if (strstr(__path, ".frro")) {
        ALOGD("Blocking .frro file: %s", __path);
        return "/dev/null";
    }
    
    
    if (strstr(__path, "systemui")) {
        ALOGD("Blocking systemui path: %s", __path);
        return "/dev/null";
    }

    if (__path == nullptr) {
        return nullptr;
    }

    list<IO::RelocateInfo>::iterator iterator;
    for (iterator = relocate_rule.begin(); iterator != relocate_rule.end(); ++iterator) {
        IO::RelocateInfo info = *iterator;
        if (info.targetPath == nullptr || strstr(__path, info.targetPath) == nullptr) {
            continue;
        }
        // Our own storage must keep its real path. Relocating a path that is
        // already inside the virtual root would rewrite it a second time and
        // produce a path that does not exist.
        if (strstr(__path, "/blackbox/") != nullptr) {
            continue;
        }
        char *ret = replace(__path, info.targetPath, info.relocatePath);
        if (ret == nullptr) {
            // Allocation failed. Returning __path would hand the caller an
            // unrelocated path, which reads the host filesystem directly, so
            // report absence instead of leaking the real layout.
            ALOGE("IO: allocation failed while relocating path");
            return "/dev/null";
        }
        return ret;
    }
    return __path;
}

jstring IO::redirectPath(JNIEnv *env, jstring path) {




    return BoxCore::redirectPathString(env, path);
}

jobject IO::redirectPath(JNIEnv *env, jobject path) {






    return BoxCore::redirectPathFile(env, path);
}

void IO::addRule(const char *targetPath, const char *relocatePath) {
    IO::RelocateInfo info{};
    info.targetPath = targetPath;
    info.relocatePath = relocatePath;
    relocate_rule.push_back(info);
}

void IO::init(JNIEnv *env) {
    jclass tmpFile = env->FindClass("java/io/File");
    getAbsolutePathMethodId = env->GetMethodID(tmpFile, "getAbsolutePath", "()Ljava/lang/String;");
}
