#include "NativeCrashHandler.h"

#include <android/log.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
#include <ucontext.h>

#include "Dobby/dobby.h"
#include "xdl.h"

// A guest death that arrives as a Binder death carries no reason. The platform
// exit reason says SIGNALED and hands back the signal number, but not the
// address or the library that faulted, so a native crash is unattributable.
// This handler runs in the crashing process and writes the one thing that makes
// it attributable without root: signal, fault address, program counter, and the
// library/offset the program counter falls in.
//
// Everything here must be async-signal-safe. No allocation, no stdio, no JNI.
// Output is formatted by hand and written with raw syscalls: the guest process
// has the container's filesystem redirect installed, and going through libc
// open(3) would let our own hook reroute a crash record into the sandbox.

namespace {
const int kHandledSignals[] = {SIGBUS, SIGSEGV, SIGILL, SIGABRT, SIGFPE, SIGTRAP};
const int kHandledCount = (int) (sizeof(kHandledSignals) / sizeof(kHandledSignals[0]));

struct sigaction g_previous[kHandledCount];
char g_internalPath[256];
char g_sharedPath[256];
char g_alternateStack[64 * 1024];
volatile sig_atomic_t g_internalReady = 0;
volatile sig_atomic_t g_sharedReady = 0;
volatile sig_atomic_t g_reentered = 0;

void appendLiteral(char *out, size_t capacity, size_t *length, const char *text) {
    while (*text != '\0' && *length + 1 < capacity) {
        out[(*length)++] = *text++;
    }
}

void appendSigned(char *out, size_t capacity, size_t *length, long value) {
    char digits[24];
    int count = 0;
    unsigned long magnitude = value < 0 ? (unsigned long) (-value) : (unsigned long) value;
    if (value < 0) {
        appendLiteral(out, capacity, length, "-");
    }
    do {
        digits[count++] = (char) ('0' + (magnitude % 10));
        magnitude /= 10;
    } while (magnitude != 0 && count < (int) sizeof(digits));
    while (count > 0 && *length + 1 < capacity) {
        out[(*length)++] = digits[--count];
    }
}

void appendHex(char *out, size_t capacity, size_t *length, uintptr_t value) {
    static const char kHexDigits[] = "0123456789abcdef";
    char digits[16];
    int count = 0;
    do {
        digits[count++] = kHexDigits[value & 0xFu];
        value >>= 4;
    } while (value != 0 && count < (int) sizeof(digits));
    while (count > 0 && *length + 1 < capacity) {
        out[(*length)++] = digits[--count];
    }
}

const char *signalName(int signalNumber) {
    switch (signalNumber) {
        case SIGBUS: return "SIGBUS";
        case SIGSEGV: return "SIGSEGV";
        case SIGILL: return "SIGILL";
        case SIGABRT: return "SIGABRT";
        case SIGFPE: return "SIGFPE";
        case SIGTRAP: return "SIGTRAP";
        default: return "SIGNAL";
    }
}

void buildCrashPath(char *out, size_t capacity, const char *directory) {
    size_t length = 0;
    appendLiteral(out, capacity, &length, directory);
    appendLiteral(out, capacity, &length, "/native_crash_");
    appendSigned(out, capacity, &length, (long) getpid());
    appendLiteral(out, capacity, &length, ".log");
    out[capacity - 1] = '\0';
}

// Raw syscalls on purpose -- see the file header. Never touches libc wrappers.
void appendRecordToFile(const char *path, volatile sig_atomic_t *ready,
                        const char *line, size_t length) {
    if (*ready == 0) {
        return;
    }
    long fd = syscall(__NR_openat, AT_FDCWD, path, O_CREAT | O_WRONLY | O_APPEND, 0600);
    if (fd >= 0) {
        syscall(__NR_write, fd, line, length);
        syscall(__NR_close, fd);
    }
}

// The guest may have its own crash reporter (Tencent's CrashSight, UE4 itself).
// Pass the fault through so their pipeline still sees it.
void chainToPrevious(int signalNumber, siginfo_t *info, void *context) {
    for (int i = 0; i < kHandledCount; i++) {
        if (kHandledSignals[i] != signalNumber) {
            continue;
        }
        const struct sigaction &previous = g_previous[i];
        if ((previous.sa_flags & SA_SIGINFO) != 0) {
            if (previous.sa_sigaction != nullptr) {
                previous.sa_sigaction(signalNumber, info, context);
            }
        } else if (previous.sa_handler != SIG_DFL && previous.sa_handler != SIG_IGN) {
            previous.sa_handler(signalNumber);
        }
        break;
    }
    // If the foreign handler returned instead of ending the process, take the
    // default action: a synchronous fault would otherwise retry forever.
    signal(signalNumber, SIG_DFL);
    raise(signalNumber);
}

void handleSignal(int signalNumber, siginfo_t *info, void *rawContext) {
    if (g_reentered) {
        _exit(128 + signalNumber);
    }
    g_reentered = 1;

    void *faultAddress = info != nullptr ? info->si_addr : nullptr;
    void *programCounter = nullptr;
    ucontext_t *context = (ucontext_t *) rawContext;
    if (context != nullptr) {
#if defined(__aarch64__)
        programCounter = (void *) context->uc_mcontext.pc;
#elif defined(__arm__)
        programCounter = (void *) context->uc_mcontext.arm_pc;
#endif
    }

    char line[512];
    size_t length = 0;
    appendLiteral(line, sizeof(line), &length, "native_crash signal=");
    appendLiteral(line, sizeof(line), &length, signalName(signalNumber));
    appendLiteral(line, sizeof(line), &length, "(");
    appendSigned(line, sizeof(line), &length, signalNumber);
    appendLiteral(line, sizeof(line), &length, ") tid=");
    appendSigned(line, sizeof(line), &length, (long) gettid());
    appendLiteral(line, sizeof(line), &length, " fault_addr=0x");
    appendHex(line, sizeof(line), &length, (uintptr_t) faultAddress);
    appendLiteral(line, sizeof(line), &length, " pc=0x");
    appendHex(line, sizeof(line), &length, (uintptr_t) programCounter);
    appendLiteral(line, sizeof(line), &length, "\n");
    line[length < sizeof(line) ? length : sizeof(line) - 1] = '\0';

    // Commit the async-signal-safe essentials before consulting the dynamic
    // linker. dladdr() is not async-signal-safe; if it faults while the loader
    // lock is held, a re-entry would otherwise _exit before writing any report.
    appendRecordToFile(g_internalPath, &g_internalReady, line, length);
    appendRecordToFile(g_sharedPath, &g_sharedReady, line, length);

    if (programCounter != nullptr) {
        Dl_info symbol;
        memset(&symbol, 0, sizeof(symbol));
        if (dladdr(programCounter, &symbol) != 0 && symbol.dli_fname != nullptr) {
            char moduleLine[512];
            size_t moduleLength = 0;
            appendLiteral(moduleLine, sizeof(moduleLine), &moduleLength,
                          "native_crash_module signal=");
            appendLiteral(moduleLine, sizeof(moduleLine), &moduleLength,
                          signalName(signalNumber));
            appendLiteral(moduleLine, sizeof(moduleLine), &moduleLength, " pc=0x");
            appendHex(moduleLine, sizeof(moduleLine), &moduleLength,
                      (uintptr_t) programCounter);
            appendLiteral(moduleLine, sizeof(moduleLine), &moduleLength, " lib=");
            appendLiteral(moduleLine, sizeof(moduleLine), &moduleLength, symbol.dli_fname);
            appendLiteral(moduleLine, sizeof(moduleLine), &moduleLength, "+0x");
            appendHex(moduleLine, sizeof(moduleLine), &moduleLength,
                      (uintptr_t) programCounter - (uintptr_t) symbol.dli_fbase);
            appendLiteral(moduleLine, sizeof(moduleLine), &moduleLength, "\n");
            moduleLine[moduleLength < sizeof(moduleLine) ? moduleLength : sizeof(moduleLine) - 1] = '\0';
            appendRecordToFile(g_internalPath, &g_internalReady, moduleLine, moduleLength);
            appendRecordToFile(g_sharedPath, &g_sharedReady, moduleLine, moduleLength);
            __android_log_write(ANDROID_LOG_ERROR, "SLACKBOX_NATIVE_CRASH", moduleLine);
        }
    }

    // The durable record is already written. Logcat is a best-effort extra
    // channel because Android logging is not async-signal-safe either.
    __android_log_write(ANDROID_LOG_ERROR, "SLACKBOX_NATIVE_CRASH", line);

    chainToPrevious(signalNumber, info, rawContext);
}

// Records that the write path works, before any crash has happened. If the file
// exists but never grows a crash line, the handler is not being invoked -- which
// points at a crash reporter installed after us, not at a broken write.
void writeArmProbe() {
    char line[384];
    size_t length = 0;
    appendLiteral(line, sizeof(line), &length, "native_handler_armed pid=");
    appendSigned(line, sizeof(line), &length, (long) getpid());
    appendLiteral(line, sizeof(line), &length, " internal=");
    appendLiteral(line, sizeof(line), &length, g_internalPath);
    appendLiteral(line, sizeof(line), &length, " shared=");
    appendLiteral(line, sizeof(line), &length, g_sharedPath);
    appendLiteral(line, sizeof(line), &length, "\n");
    appendRecordToFile(g_internalPath, &g_internalReady, line, length);
    appendRecordToFile(g_sharedPath, &g_sharedReady, line, length);
}

// Resolving one libc symbol through xdl, the same pattern VirtualSpoof uses.
// Kept as a helper because the abort hook below needs it too.
static void *resolve_libc(const char *symbol) {
    void *handle = xdl_open("libc.so", XDL_DEFAULT);
    if (!handle) {
        return nullptr;
    }
    void *target = xdl_dsym(handle, symbol, nullptr);
    xdl_close(handle);
    return target;
}

void armHandlers() {
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_sigaction = handleSignal;
    action.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&action.sa_mask);
    for (int i = 0; i < kHandledCount; i++) {
        struct sigaction existing;
        memset(&existing, 0, sizeof(existing));
        if (sigaction(kHandledSignals[i], &action, &existing) != 0) {
            continue;
        }
        // Remember the newest foreign handler so we can chain to it. Our own
        // handler must never become the "previous" -- that would recurse forever.
        if (existing.sa_sigaction != handleSignal) {
            g_previous[i] = existing;
        }
    }
}

// Re-arm only when the disposition is no longer ours.
//
// The previous version re-armed unconditionally every 500ms. That is a fight
// neither side wins: the engine installs its reporter, our timer overwrites it,
// the engine reinstalls, and whichever call lands last owns SIGABRT when the
// process dies. Measured on com.proxima.dfm: the arm probe wrote at t+0.1s, the
// guest died by abort() at t+51.5s, and the crash file contained the arm line
// and nothing else -- our handler never ran, because the engine's reporter
// owned the disposition at that moment.
//
// Comparing before writing costs six sigaction calls only when something
// actually changed, and lets the engine's handler stand until it needs to be
// replaced. SIGABRT is additionally caught at the abort() call site below, so a
// self-abort is recorded regardless of who owns the signal disposition.
static bool ourHandlerIsInstalled(int signalNumber) {
    struct sigaction current;
    memset(&current, 0, sizeof(current));
    if (sigaction(signalNumber, nullptr, &current) != 0) {
        return false;
    }
    return current.sa_sigaction == handleSignal;
}

static void ensureHandlersArmed() {
    for (int i = 0; i < kHandledCount; i++) {
        if (ourHandlerIsInstalled(kHandledSignals[i])) {
            continue;
        }
        struct sigaction action;
        memset(&action, 0, sizeof(action));
        action.sa_sigaction = handleSignal;
        action.sa_flags = SA_SIGINFO | SA_ONSTACK;
        sigemptyset(&action.sa_mask);
        struct sigaction existing;
        memset(&existing, 0, sizeof(existing));
        if (sigaction(kHandledSignals[i], &action, &existing) != 0) {
            continue;
        }
        if (existing.sa_sigaction != handleSignal) {
            g_previous[i] = existing;
        }
    }
}

// --- abort() interception ---------------------------------------------------
//
// A game that decides to kill itself does not usually raise a signal by hand; it
// calls abort(). That lands on the disposition currently installed for SIGABRT,
// which by then belongs to the engine's reporter, not to us. Hooking abort()
// makes the record unconditional: it is the last point before the process dies
// and no other library can take it from us.
//
// Everything below runs with the same async-signal-safe constraints as the
// signal handler: no allocation, no stdio, raw syscalls only.

static void (*orig_abort)(void) = nullptr;

// Walk the caller stack to find the first frame outside libc, so the record
// points at the code that called abort() rather than at abort() itself.
static void *callerFrame(void *pc) {
    Dl_info info;
    memset(&info, 0, sizeof(info));
    if (dladdr(pc, &info) == 0 || info.dli_fname == nullptr) {
        return nullptr;
    }
    if (strstr(info.dli_fname, "/libc.so") != nullptr) {
        return nullptr;
    }
    return pc;
}

static void my_abort() {
    // __builtin_return_address(0) is the caller of abort(), i.e. the game.
    void *pc = (void *) __builtin_return_address(0);
    void *reported = callerFrame(pc);

    char line[512];
    size_t length = 0;
    appendLiteral(line, sizeof(line), &length, "native_crash signal=SIGABRT(6) tid=");
    appendSigned(line, sizeof(line), &length, (long) gettid());
    appendLiteral(line, sizeof(line), &length, " source=abort()");
    appendLiteral(line, sizeof(line), &length, " fault_addr=0x");
    appendHex(line, sizeof(line), &length, (uintptr_t) reported);
    appendLiteral(line, sizeof(line), &length, " pc=0x");
    appendHex(line, sizeof(line), &length, (uintptr_t) pc);
    appendLiteral(line, sizeof(line), &length, "\n");
    line[length < sizeof(line) ? length : sizeof(line) - 1] = '\0';
    appendRecordToFile(g_internalPath, &g_internalReady, line, length);
    appendRecordToFile(g_sharedPath, &g_sharedReady, line, length);

    if (reported != nullptr) {
        Dl_info info;
        memset(&info, 0, sizeof(info));
        if (dladdr(reported, &info) != 0 && info.dli_fname != nullptr) {
            char moduleLine[512];
            size_t moduleLength = 0;
            appendLiteral(moduleLine, sizeof(moduleLength), &moduleLength,
                          "native_crash_module signal=SIGABRT(6) pc=0x");
            appendHex(moduleLine, sizeof(moduleLength), &moduleLength, (uintptr_t) reported);
            appendLiteral(moduleLine, sizeof(moduleLength), &moduleLength, " lib=");
            appendLiteral(moduleLine, sizeof(moduleLength), &moduleLength, info.dli_fname);
            appendLiteral(moduleLine, sizeof(moduleLength), &moduleLength, "+0x");
            appendHex(moduleLine, sizeof(moduleLength), &moduleLength,
                      (uintptr_t) reported - (uintptr_t) info.dli_fbase);
            appendLiteral(moduleLine, sizeof(moduleLength), &moduleLength, "\n");
            moduleLine[moduleLength < sizeof(moduleLength) ? moduleLength
                                                          : sizeof(moduleLength) - 1] = '\0';
            appendRecordToFile(g_internalPath, &g_internalReady, moduleLine, moduleLength);
            appendRecordToFile(g_sharedPath, &g_sharedReady, moduleLine, moduleLength);
            __android_log_write(ANDROID_LOG_ERROR, "SLACKBOX_NATIVE_CRASH", moduleLine);
        }
    }
    __android_log_write(ANDROID_LOG_ERROR, "SLACKBOX_NATIVE_CRASH", line);

    if (orig_abort != nullptr) {
        orig_abort();
    }
    // abort() is not allowed to return, but if our replacement did, leave no
    // way for the caller to continue past a deliberate self-termination.
    _exit(134);
}

static void installAbortHook() {
    void *target = resolve_libc("abort");
    if (target == nullptr) {
        return;
    }
    if (DobbyHook(target, (void *) my_abort, (void **) &orig_abort) != 0) {
        return;
    }
}

// A game engine and its crash reporter install their own fatal-signal handlers
// well after the guest binds, silently replacing ours. This thread notices and
// re-arms, but only for the signals whose disposition actually drifted.
void *rearmLoop(void *) {
    for (;;) {
        struct timespec pause;
        pause.tv_sec = 1;
        pause.tv_nsec = 0;
        nanosleep(&pause, nullptr);
        ensureHandlersArmed();
    }
    return nullptr;
}
} // namespace

void NativeCrashHandler::install(const char *internalDirectory, const char *sharedDirectory) {
    if (internalDirectory != nullptr) {
        buildCrashPath(g_internalPath, sizeof(g_internalPath), internalDirectory);
        g_internalReady = 1;
    }
    if (sharedDirectory != nullptr) {
        buildCrashPath(g_sharedPath, sizeof(g_sharedPath), sharedDirectory);
        g_sharedReady = 1;
    }

    stack_t alternate; // run the handler off a dedicated stack (stack-overflow SIGSEGV)
    memset(&alternate, 0, sizeof(alternate));
    alternate.ss_sp = g_alternateStack;
    alternate.ss_size = sizeof(g_alternateStack);
    alternate.ss_flags = 0;
    sigaltstack(&alternate, nullptr);

    writeArmProbe();
    armHandlers();
    installAbortHook();

    pthread_t watchdog;
    if (pthread_create(&watchdog, nullptr, rearmLoop, nullptr) == 0) {
        pthread_detach(watchdog);
    }
}
