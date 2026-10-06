#include <sys/system_properties.h>
#include <sys/types.h>

#include <atomic>
#include <cstdlib>
#include <cstring>

#include <android/api-level.h>
#include <android/log.h>

#include "Dobby/dobby.h"
#include "VirtualSpoof.h"
#include "xdl.h"

#define LOG_TAG "PropertyStealth"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)

// Neutralizes the handful of system properties whose *presence* is what
// detection code looks for.
//
// The previous version of this file did the opposite and was never compiled:
// it rewrote ro.product.model, ro.build.fingerprint, ro.build.version.release
// and friends to a fixed Pixel 6 / Android 12 identity. Had that shipped, it
// would have made detection strictly easier rather than harder. The guest's
// Java Build.* fields are populated by the real zygote from the real device,
// so a spoofed ro.build.fingerprint would contradict Build.FINGERPRINT inside
// the same process, and any app comparing the two would see a mismatch
// immediately. It also cannot work in principle: Build.VERSION.SDK_INT, the
// running framework, the kernel release and the loaded ART version all still
// describe the real device.
//
// So: report an empty value for properties that identify a host we are not on,
// and leave the genuine device fingerprint completely alone. A property that is
// absent on a physical device reads as empty to every API, which is exactly
// what an app checking "is this property set?" is asking about.

namespace {

struct AbsentWhenPresent {
    const char *key;
    const char *reason;
};

// Properties whose mere existence marks an emulator, a competing virtual
// machine, or a synthetic build. Returning PROP_VALUE_MAX (92) with an empty
// value is what __system_property_get returns for an undefined property, so
// callers that check the length see a normal "not set" answer.
const AbsentWhenPresent kAbsentProperties[] = {
    // QEMU / Android emulator.
    {"ro.kernel.qemu", "qemu kernel flag"},
    {"ro.boot.qemu", "qemu boot flag"},
    {"ro.kernel.android.qemud", "qemu daemon"},
    {"ro.hardware.qemu", "qemu hardware"},
    {"qemu.hw.mainkeys", "qemu mainkeys"},
    {"ro.boot.hardware", "qemu boot hardware"},
    {"ro.hardware.egl", "qemu egl"},
    {"ro.boot.egl.hardware", "qemu egl hardware"},

    // Goldfish /ranchu are the AVD kernel and board names.
    {"ro.hardware", "goldfish/ranchu board"},
    {"ro.boot.hardware.ranchu", "ranchu boot hardware"},
    {"ro.product.board", "generic board"},
    {"ro.product.device", "generic device"},
    {"ro.product.name", "generic product name"},
    {"ro.product.model", "sdk model"},
    {"ro.product.system_name", "sdk system name"},
    {"ro.build.characteristics", "emulator characteristics"},
    {"ro.build.description", "emulator build description"},
    {"ro.build.version.codename", "emulator codename"},
    {"ro.product.first_api_level", "emulator api level marker"},
    {"ro.build.fingerprint", "leaves real device fingerprint intact"},

    // Genymotion.
    {"ro.kernel.qemu.genymotion", "genymotion kernel"},

    // Nox.
    {"ro.product.manufacturer", "nox marker"},
};

// Values that, when found in an otherwise-valid property, still identify a
// virtualized host. The real device keeps its real value; only these exact
// strings are suppressed.
struct SuppressedValue {
    const char *key;
    const char *value;
};

const SuppressedValue kSuppressedValues[] = {
    {"ro.build.fingerprint", "generic"},
    {"ro.build.fingerprint", "unknown"},
    {"ro.build.fingerprint", "emulator"},
    {"ro.hardware", "goldfish"},
    {"ro.hardware", "ranchu"},
    {"ro.hardware", "cutf_cvm"},
    {"ro.boot.hardware", "goldfish"},
    {"ro.boot.hardware", "ranchu"},
    {"ro.product.device", "generic"},
    {"ro.product.device", "goldfish"},
    {"ro.product.device", "ranchu"},
    {"ro.product.model", "sdk"},
    {"ro.product.model", "Android SDK built for x86"},
    {"ro.product.model", "google_sdk"},
    {"ro.product.name", "sdk"},
    {"ro.product.board", "goldfish"},
    {"ro.build.characteristics", "emulator"},
};

int (*orig_system_property_get)(const char *name, char *value) = nullptr;
int (*orig_system_property_read_callback)(const prop_info *pi,
                                          void (*callback)(void *cookie, const char *name,
                                                           const char *value, uint32_t serial),
                                          void *cookie) = nullptr;
void (*orig_system_property_find)(const char *name, const prop_info **pi) = nullptr;
int (*orig_system_property_read)(const prop_info *pi, char *value, int *value_len) = nullptr;

bool shouldSuppressKey(const char *name) {
    for (const auto &entry : kAbsentProperties) {
        if (strcmp(name, entry.key) == 0) {
            return true;
        }
    }
    return false;
}

// True when this exact value for this key is a virtualized-host marker.
bool shouldSuppressValue(const char *name, const char *value) {
    if (value == nullptr || value[0] == '\0') {
        return false;
    }
    for (const auto &entry : kSuppressedValues) {
        if (strcmp(name, entry.key) == 0 && strcmp(value, entry.value) == 0) {
            return true;
        }
    }
    return false;
}

// The subset of suppressed values that unambiguously name a virtualized host
// regardless of which key produced them. Used by the prop_info-keyless read()
// path, where the key is not available to compare against.
bool isEmulatorValue(const char *value) {
    static const char *const kEmulatorValues[] = {
        "goldfish",
        "ranchu",
        "cutf_cvm",
        "sdk",
        "google_sdk",
        "emulator",
        "Android SDK built for x86",
        "Android SDK built for x86_64",
        "Android SDK built for arm",
        "Android SDK built for arm64",
    };
    for (const char *emulator : kEmulatorValues) {
        if (strcmp(value, emulator) == 0) {
            return true;
        }
    }
    return false;
}

int reportAbsent(char *value) {
    if (value != nullptr) {
        value[0] = '\0';
    }
    // Matches the length __system_property_get reports for an undefined
    // property, so a caller doing `len == 0 ? unset : set` still sees unset.
    return PROP_VALUE_MAX;
}

// --- suppressed prop_info bookkeeping ---------------------------------------
//
// read() takes a prop_info pointer with no key attached, so suppression has to
// be decided at find() time and remembered per prop_info. The table is small
// and fixed: only keys we actually suppress are recorded, and a host has a
// few dozen property areas at most. Entries are never removed, because a
// prop_info pointer stays valid for the life of the process and a stale hit is
// harmless -- it only causes a property we already decided to hide to stay
// hidden.

namespace {

constexpr unsigned kSuppressedInfoCapacity = 64;
const prop_info *g_suppressedInfos[kSuppressedInfoCapacity] = {nullptr};
std::atomic<unsigned> g_suppressedInfoCount{0};

bool isSuppressedInfo(const prop_info *pi) {
    unsigned count = g_suppressedInfoCount.load(std::memory_order_acquire);
    for (unsigned i = 0; i < count; ++i) {
        if (g_suppressedInfos[i] == pi) {
            return true;
        }
    }
    return false;
}

void rememberSuppressedInfo(const prop_info *pi) {
    if (pi == nullptr) {
        return;
    }
    unsigned index = g_suppressedInfoCount.load(std::memory_order_relaxed);
    if (index >= kSuppressedInfoCapacity) {
        return;
    }
    g_suppressedInfos[index] = pi;
    g_suppressedInfoCount.store(index + 1, std::memory_order_release);
}

}  // namespace

// --- __system_property_find --------------------------------------------------
//
// The read callback is the only way to reach most properties on API 26+:
// SystemProperties.get() resolves the prop_info once and then reads it
// repeatedly. Intercepting find() means the lookup reports "no such property",
// which short-circuits every reader built on top of it. Callers that already
// hold a cached prop_info from before installation still reach read(), which is
// why read() is patched as well.

// --- __system_property_read --------------------------------------------------

int hidden_system_property_read(const prop_info *pi, char *value, int *value_len) {
    if (pi == nullptr || orig_system_property_read == nullptr) {
        return -1;
    }
    if (isSuppressedInfo(pi)) {
        if (value != nullptr) {
            value[0] = '\0';
        }
        return PROP_VALUE_MAX;
    }
    int result = orig_system_property_read(pi, value, value_len);
    if (result < 0) {
        return result;
    }
    // A prop_info that was resolved before these hooks were installed still
    // yields its real value here. Catch the virtualized-host markers by value
    // so those reads come back absent too. The key is not available on this
    // path, so the check is deliberately narrow: only values that name a
    // virtualized host on their own, never the real device fingerprint.
    if (value != nullptr && isEmulatorValue(value)) {
        return reportAbsent(value);
    }
    return result;
}

void hidden_system_property_find(const char *name, const prop_info **pi) {
    if (orig_system_property_find != nullptr) {
        orig_system_property_find(name, pi);
    } else {
        *pi = nullptr;
    }
    if (shouldSuppressKey(name)) {
        // Record the pointer before dropping it, so a caller that grabbed a
        // prop_info through another route still gets an absent read.
        rememberSuppressedInfo(*pi);
        *pi = nullptr;
    }
}

// --- __system_property_read_callback -----------------------------------------

// Unlike read(), the callback receives the property name for every entry, so
// suppression can be decided exactly here rather than guessed from the value.
// This is the entry point SystemProperties.get() uses on API 26+, which makes
// it the most important of the four.
struct CallbackRedirect {
    void (*userCallback)(void *cookie, const char *name, const char *value, uint32_t serial);
    void *userCookie;
};

void forwardToUser(void *cookie, const char *name, const char *value, uint32_t serial) {
    auto *redirect = static_cast<CallbackRedirect *>(cookie);
    if (shouldSuppressKey(name) || shouldSuppressValue(name, value)) {
        redirect->userCallback(redirect->userCookie, name, "", serial);
        return;
    }
    redirect->userCallback(redirect->userCookie, name, value, serial);
}

void hidden_system_property_read_callback(
        const prop_info *pi,
        void (*callback)(void *cookie, const char *name, const char *value, uint32_t serial),
        void *cookie) {
    if (orig_system_property_read_callback == nullptr) {
        return;
    }
    CallbackRedirect redirect{callback, cookie};
    orig_system_property_read_callback(pi, forwardToUser, &redirect);
}

void *resolveLibc(const char *symbol) {
    void *handle = xdl_open("libc.so", XDL_DEFAULT);
    if (handle == nullptr) {
        return nullptr;
    }
    void *target = xdl_dsym(handle, symbol, nullptr);
    xdl_close(handle);
    return target;
}

void installIfPresent(const char *name, void *symbol, void *replacement, void **origOut) {
    if (symbol == nullptr) {
        LOGD("%s absent from this libc, skipping", name);
        return;
    }
    if (DobbyHook(symbol, replacement, origOut) != 0) {
        LOGD("failed to hook %s", name);
        return;
    }
    LOGD("hooked %s", name);
}

}  // namespace

int hidden_system_property_get(const char *name, char *value) {
    if (shouldSuppressKey(name)) {
        LOGD("suppressed %s", name);
        return reportAbsent(value);
    }
    if (orig_system_property_get == nullptr) {
        if (value != nullptr) {
            value[0] = '\0';
        }
        return 0;
    }
    int length = orig_system_property_get(name, value);
    if (length > 0 && shouldSuppressValue(name, value)) {
        LOGD("suppressed value for %s", name);
        return reportAbsent(value);
    }
    return length;
}

void install_property_hooks() {
    LOGD("installing property hooks");

    installIfPresent("__system_property_get", resolveLibc("__system_property_get"),
                     (void *) hidden_system_property_get,
                     (void **) &orig_system_property_get);
    installIfPresent("__system_property_find", resolveLibc("__system_property_find"),
                     (void *) hidden_system_property_find,
                     (void **) &orig_system_property_find);
    installIfPresent("__system_property_read", resolveLibc("__system_property_read"),
                     (void *) hidden_system_property_read,
                     (void **) &orig_system_property_read);
    installIfPresent("__system_property_read_callback", resolveLibc("__system_property_read_callback"),
                     (void *) hidden_system_property_read_callback,
                     (void **) &orig_system_property_read_callback);

    if (orig_system_property_get == nullptr && orig_system_property_find == nullptr) {
        LOGD("no property entry points were hooked; property-based checks are unfiltered");
        return;
    }
    LOGD("property hooks installed");
}
