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
#include <unwind.h>

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

// Stack capture, shared by the signal handler and the abort() hook. Defined
// below with the rest of the abort interception; declared here because
// handleSignal() records a backtrace too.
const int kMaxTraceFrames = 32;
uintptr_t g_traceFrames[kMaxTraceFrames];
volatile sig_atomic_t g_traceFrameCount = 0;

int captureTraceFrames();
void appendRawFrameRecord(char *out, size_t capacity, const char *tag, uintptr_t pc);
void appendModuleRecord(char *out, size_t capacity, const char *tag, uintptr_t pc);
void commitRecord(const char *record);
void dumpProcMaps();

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

    // Raw program counters first, still without the loader lock. On SIGBUS and
    // SIGSEGV the fault address in si_addr is the single most useful field and
    // it is already in the line above; the stack is what turns it into a file
    // and an offset.
    int frameCount = captureTraceFrames();
    for (int i = 0; i < frameCount; i++) {
        char frameLine[256];
        appendRawFrameRecord(frameLine, sizeof(frameLine), "native_crash_frame", g_traceFrames[i]);
        commitRecord(frameLine);
    }

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
            appendHex(moduleLine, sizeof(moduleLength), &moduleLength,
                      (uintptr_t) programCounter);
            appendLiteral(moduleLine, sizeof(moduleLength), &moduleLength, " lib=");
            appendLiteral(moduleLine, sizeof(moduleLength), &moduleLength, symbol.dli_fname);
            appendLiteral(moduleLine, sizeof(moduleLength), &moduleLength, "+0x");
            appendHex(moduleLine, sizeof(moduleLength), &moduleLength,
                      (uintptr_t) programCounter - (uintptr_t) symbol.dli_fbase);
            appendLiteral(moduleLine, sizeof(moduleLength), &moduleLength, "\n");
            moduleLine[moduleLength < sizeof(moduleLength) ? moduleLength : sizeof(moduleLength) - 1] = '\0';
            appendRecordToFile(g_internalPath, &g_internalReady, moduleLine, moduleLength);
            appendRecordToFile(g_sharedPath, &g_sharedReady, moduleLine, moduleLength);
            __android_log_write(ANDROID_LOG_ERROR, "SLACKBOX_NATIVE_CRASH", moduleLine);
        }
    }

    for (int i = 0; i < frameCount; i++) {
        char symbolLine[512];
        appendModuleRecord(symbolLine, sizeof(symbolLine), "native_crash_symbol", g_traceFrames[i]);
        commitRecord(symbolLine);
    }

    dumpProcMaps();

    // The durable record is already written. Logcat is a best-effort extra
    // channel because Android logging is not async-signal-safe either.
    __android_log_write(ANDROID_LOG_ERROR, "SLACKBOX_NATIVE_CRASH", line);

    chainToPrevious(signalNumber, info, rawContext);
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
        // Somebody replaced us. Record who before taking it back, so a crash that
        // still bypasses this handler can be traced to the library that owns the
        // disposition at that moment instead of being guessed at.
        struct sigaction current;
        memset(&current, 0, sizeof(current));
        if (sigaction(kHandledSignals[i], nullptr, &current) == 0 &&
            current.sa_sigaction != handleSignal) {
            char stolenLine[512];
            appendModuleRecord(stolenLine, sizeof(stolenLine), "native_crash_displaced",
                               (uintptr_t) current.sa_sigaction);
            size_t len = strlen(stolenLine);
            while (len > 0 && stolenLine[len - 1] != '\n') {
                stolenLine[--len] = '\0';
            }
            size_t used = len;
            appendLiteral(stolenLine, sizeof(stolenLine), &used, " signal=");
            appendLiteral(stolenLine, sizeof(stolenLine), &used, signalName(kHandledSignals[i]));
            appendLiteral(stolenLine, sizeof(stolenLine), &used, "\n");
            commitRecord(stolenLine);
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
// Measured on com.proxima.dfm at t+45.6s: the guest's own crash reporter caught
// SIGBUS(7), and its own termination path called abort(). The kernel still
// reported the original fatal signal, so the exit reason stays SIGNALED/7 while
// the abort hook is the only place the fault is visible. That is why this hook
// unwinds a real stack instead of logging one return address -- the frame above
// abort() is inside libc (fault_addr came back 0x0 and no module line was
// emitted), so a single pc names libc and nothing else.
//
// Everything below runs with the same async-signal-safe constraints as the
// signal handler: no allocation, no stdio, raw syscalls only. The raw program
// counters are committed to disk before dladdr() is consulted, because dladdr
// takes the loader lock and can fault if that lock is already held.

static void (*orig_abort)(void) = nullptr;

_Unwind_Reason_Code collectTraceFrame(struct _Unwind_Context *context, void *argument) {
    (void) argument;
    if (g_traceFrameCount >= kMaxTraceFrames) {
        return _URC_END_OF_STACK;
    }
    uintptr_t pc = (uintptr_t) _Unwind_GetIP(context);
    if (pc == 0) {
        return _URC_END_OF_STACK;
    }
    g_traceFrames[g_traceFrameCount++] = pc;
    return _URC_NO_REASON;
}

int captureTraceFrames() {
    g_traceFrameCount = 0;
    _Unwind_Backtrace(collectTraceFrame, nullptr);
    return (int) g_traceFrameCount;
}

static bool addressIsInLibc(uintptr_t pc) {
    Dl_info info;
    memset(&info, 0, sizeof(info));
    if (dladdr((void *) pc, &info) == 0 || info.dli_fname == nullptr) {
        return false;
    }
    return strstr(info.dli_fname, "/libc.so") != nullptr;
}

// First frame that is not libc. The frame directly above abort() belongs to
// libc's own terminate path (__libc_fatal, __fortify_fail, __stack_chk_fail),
// so naming it would only ever produce "libc.so". Returns the caller pc itself
// when the whole visible stack is libc, so the record still carries something.
static void *firstNonLibcFrame(const uintptr_t *frames, int count) {
    for (int i = 0; i < count; i++) {
        if (!addressIsInLibc(frames[i])) {
            return (void *) frames[i];
        }
    }
    if (count > 0) {
        return (void *) frames[0];
    }
    return nullptr;
}

void appendModuleRecord(char *out, size_t capacity, const char *tag,
                               uintptr_t pc) {
    size_t length = 0;
    appendLiteral(out, capacity, &length, tag);
    appendLiteral(out, capacity, &length, " pc=0x");
    appendHex(out, capacity, &length, pc);
    Dl_info info;
    memset(&info, 0, sizeof(info));
    if (dladdr((void *) pc, &info) != 0 && info.dli_fname != nullptr) {
        appendLiteral(out, capacity, &length, " lib=");
        appendLiteral(out, capacity, &length, info.dli_fname);
        appendLiteral(out, capacity, &length, "+0x");
        appendHex(out, capacity, &length, pc - (uintptr_t) info.dli_fbase);
        const char *symbol = info.dli_sname != nullptr ? info.dli_sname : "?";
        appendLiteral(out, capacity, &length, " sym=");
        appendLiteral(out, capacity, &length, symbol);
    } else {
        // dladdr failing is itself a finding: it means the address is outside
        // every loaded object, which is what an unmapped or JIT page looks like.
        appendLiteral(out, capacity, &length, " lib=<unresolved> pc_is_unmapped=1");
    }
    appendLiteral(out, capacity, &length, "\n");
    out[capacity - 1] = '\0';
}

void appendRawFrameRecord(char *out, size_t capacity, const char *tag,
                                 uintptr_t pc) {
    size_t length = 0;
    appendLiteral(out, capacity, &length, tag);
    appendLiteral(out, capacity, &length, " pc=0x");
    appendHex(out, capacity, &length, pc);
    appendLiteral(out, capacity, &length, "\n");
    out[capacity - 1] = '\0';
}

void commitRecord(const char *record) {
    size_t length = strlen(record);
    appendRecordToFile(g_internalPath, &g_internalReady, record, length);
    appendRecordToFile(g_sharedPath, &g_sharedReady, record, length);
    __android_log_write(ANDROID_LOG_ERROR, "SLACKBOX_NATIVE_CRASH", record);
}

// Snapshot of the address space at the moment of death. Without it, frames that
// dladdr cannot resolve cannot be attributed after the process is gone.
void dumpProcMaps() {
    long fd = syscall(__NR_openat, AT_FDCWD, "/proc/self/maps", O_RDONLY, 0);
    if (fd < 0) {
        return;
    }
    char mapPath[256];
    size_t pathLength = 0;
    appendLiteral(mapPath, sizeof(mapPath), &pathLength, g_sharedPath);
    // native_crash_1234.log -> native_maps_1234.log
    const char *lastSlash = strrchr(mapPath, '/');
    if (lastSlash != nullptr) {
        pathLength = (size_t) (lastSlash - mapPath) + 1;
    } else {
        pathLength = 0;
    }
    appendLiteral(mapPath, sizeof(mapPath), &pathLength, "native_maps_");
    appendSigned(mapPath, sizeof(mapPath), &pathLength, (long) getpid());
    appendLiteral(mapPath, sizeof(mapPath), &pathLength, ".log\n");

    long out = syscall(__NR_openat, AT_FDCWD, mapPath,
                       O_CREAT | O_WRONLY | O_TRUNC, 0600);
    if (out < 0) {
        syscall(__NR_close, fd);
        return;
    }
    char buffer[4096];
    for (;;) {
        long got = syscall(__NR_read, fd, buffer, sizeof(buffer));
        if (got <= 0) {
            break;
        }
        syscall(__NR_write, out, buffer, (size_t) got);
    }
    syscall(__NR_close, out);
    syscall(__NR_close, fd);
}

static void my_abort() {
    int frameCount = captureTraceFrames();
    uintptr_t callerPc = (uintptr_t) __builtin_return_address(0);
    void *reported = frameCount > 0 ? firstNonLibcFrame(g_traceFrames, frameCount)
                                    : (void *) callerPc;

    // 1. Essentials, async-signal-safe, no loader lock.
    char line[512];
    size_t length = 0;
    appendLiteral(line, sizeof(line), &length, "native_crash signal=SIGABRT(6) tid=");
    appendSigned(line, sizeof(line), &length, (long) gettid());
    appendLiteral(line, sizeof(line), &length, " source=abort()");
    appendLiteral(line, sizeof(line), &length, " frames=");
    appendSigned(line, sizeof(line), &length, frameCount);
    appendLiteral(line, sizeof(line), &length, " fault_addr=0x");
    appendHex(line, sizeof(line), &length, (uintptr_t) reported);
    appendLiteral(line, sizeof(line), &length, " pc=0x");
    appendHex(line, sizeof(line), &length, callerPc);
    appendLiteral(line, sizeof(line), &length, "\n");
    line[length < sizeof(line) ? length : sizeof(line) - 1] = '\0';
    commitRecord(line);

    // 2. Raw program counters, still no loader lock. These survive even if the
    //    resolution pass below faults.
    for (int i = 0; i < frameCount; i++) {
        char frameLine[256];
        appendRawFrameRecord(frameLine, sizeof(frameLine), "native_crash_frame", g_traceFrames[i]);
        commitRecord(frameLine);
    }

    // 3. Resolution pass. Loader lock from here on.
    if (reported != nullptr) {
        char moduleLine[512];
        appendModuleRecord(moduleLine, sizeof(moduleLine),
                           "native_crash_module signal=SIGABRT(6)", (uintptr_t) reported);
        commitRecord(moduleLine);
    }
    for (int i = 0; i < frameCount; i++) {
        char symbolLine[512];
        appendModuleRecord(symbolLine, sizeof(symbolLine), "native_crash_symbol", g_traceFrames[i]);
        commitRecord(symbolLine);
    }

    dumpProcMaps();

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
