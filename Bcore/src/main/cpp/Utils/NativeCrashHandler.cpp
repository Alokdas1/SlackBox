#include "NativeCrashHandler.h"

#include <android/log.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/types.h>
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
char g_downloadPath[256];
char g_alternateStack[64 * 1024];
volatile sig_atomic_t g_internalReady = 0;
volatile sig_atomic_t g_sharedReady = 0;
volatile sig_atomic_t g_downloadReady = 0;
volatile sig_atomic_t g_reentered = 0;

// Single writer at a time. The re-arm thread and the crashing thread can both
// append, and without this the records interleave: an on-device run produced
// binary garbage spliced between native_crash_displaced lines. A test-and-set
// spin is used rather than a pthread mutex because mutexes are not
// async-signal-safe. It gives up rather than blocking, because losing a record
// to a stuck writer is worse than losing one line to a torn write.
volatile sig_atomic_t g_writeLock = 0;

static bool acquireWriteLock() {
    for (int spin = 0; spin < 4096; spin++) {
        if (__sync_lock_test_and_set(&g_writeLock, 1) == 0) {
            return true;
        }
    }
    return false;
}

static void releaseWriteLock() {
    __sync_lock_release(&g_writeLock);
}

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
    if (*ready == 0 || path[0] == '\0') {
        return;
    }
    long fd = syscall(__NR_openat, AT_FDCWD, path, O_CREAT | O_WRONLY | O_APPEND, 0600);
    if (fd >= 0) {
        syscall(__NR_write, fd, line, length);
        syscall(__NR_close, fd);
    }
}

// Every record goes to all three sinks under the write lock. The Download
// sink is the one the user can actually open without root: Android 30+ scoped
// storage makes Android/data/<pkg>/ opaque to Termux and to every file
// manager, so a log that only lands there is a log nobody can read.
void writeRecordToAllSinks(const char *line, size_t length) {
    if (!acquireWriteLock()) {
        // Fall through unlocked rather than dropping the record entirely.
        appendRecordToFile(g_internalPath, &g_internalReady, line, length);
        appendRecordToFile(g_sharedPath, &g_sharedReady, line, length);
        appendRecordToFile(g_downloadPath, &g_downloadReady, line, length);
        return;
    }
    appendRecordToFile(g_internalPath, &g_internalReady, line, length);
    appendRecordToFile(g_sharedPath, &g_sharedReady, line, length);
    appendRecordToFile(g_downloadPath, &g_downloadReady, line, length);
    releaseWriteLock();
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
int walkInterruptedStack(uintptr_t *out, int maxFrames, uintptr_t framePointer,
                         uintptr_t linkRegister, uintptr_t anchorSp) {
    (void) framePointer;
    (void) anchorSp;
    int count = 0;
    // Only the link register. An earlier version also followed the frame-pointer
    // chain and produced one frame per run: libart.so+0x270fc4 on one run, an
    // unresolved address on the next, then a stop.
    //
    // That is the expected result, because Android's arm64 code is built without
    // frame pointers -- x29 is an ordinary callee-saved register holding
    // whatever the faulting code last put there. "Validating" a garbage x29
    // against a plausible stack range passes often enough to be dangerous, and
    // dereferencing it inside a signal handler is exactly how this path turned a
    // guest crash into EXIT_SELF status=139. One frame is all the chain could
    // honestly give, so take the one frame that needs no dereference.
    if (plausibleCodeAddress(linkRegister) && count < maxFrames) {
        out[count++] = linkRegister;
    }
    return count;
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
    // Terminate at the append length, not at capacity-1. commitRecord() measures
    // with strlen(), and terminating at the far end made every record carry the
    // previous call's leftover stack bytes -- an on-device run produced repeated
    // signal= lines and a truncated tail that way.
    out[length < capacity ? length : capacity - 1] = '\0';
}

void appendRawFrameRecord(char *out, size_t capacity, const char *tag,
                                 uintptr_t pc) {
    size_t length = 0;
    appendLiteral(out, capacity, &length, tag);
    appendLiteral(out, capacity, &length, " pc=0x");
    appendHex(out, capacity, &length, pc);
    appendLiteral(out, capacity, &length, "\n");
    out[length < capacity ? length : capacity - 1] = '\0';
}

void commitRecord(const char *record) {
    size_t length = strlen(record);
    writeRecordToAllSinks(record, length);
    __android_log_write(ANDROID_LOG_ERROR, "SLACKBOX_NATIVE_CRASH", record);
}

// Snapshot of the address space at the moment of death. Without it, frames that
// dladdr cannot resolve cannot be attributed after the process is gone.
void dumpProcMaps() {
    long fd = syscall(__NR_openat, AT_FDCWD, "/proc/self/maps", O_RDONLY, 0);
    if (fd < 0) {
        return;
    }
    char buffer[4096];
    const char *sinks[3];
    sinks[0] = g_internalReady ? g_internalPath : nullptr;
    sinks[1] = g_sharedReady ? g_sharedPath : nullptr;
    sinks[2] = g_downloadReady ? g_downloadPath : nullptr;

    for (int s = 0; s < 3; s++) {
        if (sinks[s] == nullptr) {
            continue;
        }
        char mapPath[256];
        size_t pathLength = 0;
        appendLiteral(mapPath, sizeof(mapPath), &pathLength, sinks[s]);
        // Terminate before scanning: appendLiteral() does not terminate, so
        // strrchr() ran off into uninitialised stack, produced a garbage path,
        // and the open failed. That is why no native_maps_<pid>.log ever
        // appeared on any run.
        mapPath[pathLength < sizeof(mapPath) ? pathLength : sizeof(mapPath) - 1] = '\0';
        // native_crash_1234.log -> native_maps_1234.log
        const char *lastSlash = strrchr(mapPath, '/');
        pathLength = lastSlash != nullptr ? (size_t) (lastSlash - mapPath) + 1 : 0;
        appendLiteral(mapPath, sizeof(mapPath), &pathLength, "native_maps_");
        appendSigned(mapPath, sizeof(mapPath), &pathLength, (long) getpid());
        appendLiteral(mapPath, sizeof(mapPath), &pathLength, ".log\n");

        long out = syscall(__NR_openat, AT_FDCWD, mapPath,
                           O_CREAT | O_WRONLY | O_TRUNC, 0600);
        if (out < 0) {
            continue;
        }
        // Rewind: /proc/self/maps is a seq_file, one read pass only.
        syscall(__NR_lseek, fd, 0, SEEK_SET, nullptr);
        for (;;) {
            long got = syscall(__NR_read, fd, buffer, sizeof(buffer));
            if (got <= 0) {
                break;
            }
            syscall(__NR_write, out, buffer, (size_t) got);
        }
        syscall(__NR_close, out);
    }
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

// --- sigaction() interception ------------------------------------------------
//
// Measured on com.proxima.dfm (pid 27316): the guest's own crash reporters
// displaced our fatal-signal handlers twelve times in a single run, alternating
// between libCrashSight.so+0x1c2e8 (Tencent) and libcrashlytics-common.so
// +0x9d7c0 (Google). Our one-second re-arm took the disposition back, they took
// it again, and the crash landed in whichever window they happened to hold --
// so the crash file held the arm line and nothing else. A timer cannot win that.
//
// Rather than fight for the disposition, wrap it. The installer still gets what
// it asked for and still believes it owns the signal; we remember their handler
// and put ours back on top. handleSignal() writes the record and then chains to
// them, so their reporter behaves exactly as before. Nothing about the guest
// changes -- we only observe before it runs.

static int (*orig_sigaction)(int, const struct sigaction *, struct sigaction *) = nullptr;

static int indexOfSignal(int signum) {
    for (int i = 0; i < kHandledCount; i++) {
        if (kHandledSignals[i] == signum) {
            return i;
        }
    }
    return -1;
}

// Records a foreign handler for a signal. Deliberately does no file I/O and no
// logging.
//
// This runs inside somebody else's sigaction() call, six times during guest
// startup. The previous version opened and wrote three files and called
// __android_log_write from there, on a caller's stack and possibly while that
// caller held a lock that logd or the filesystem also wants. Whatever the
// precise mechanism, the guest stopped rendering entirely after that change
// (zero activity_started, versus a 2400x1080 surface in earlier runs), and every
// death was reported as EXIT_SELF status=139.
//
// Observation has to be inert. Note the owner in memory here, and let the
// one-second thread do the writing in flushDisplacementLog().
static volatile sig_atomic_t g_displaced[kHandledCount];
static struct sigaction g_loggedForeign[kHandledCount];

static void noteForeignHandler(int index, const struct sigaction *act) {
    if (index < 0 || act == nullptr) {
        return;
    }
    if (g_previous[index].sa_sigaction == act->sa_sigaction) {
        return;
    }
    g_previous[index] = *act;
    g_displaced[index] = 1;
}

// Runs on the re-arm thread, never in a signal path. One record per owner, so
// the file does not fill with repeats.
static void flushDisplacementLog() {
    for (int i = 0; i < kHandledCount; i++) {
        if (g_displaced[i] == 0) {
            continue;
        }
        g_displaced[i] = 0;
        uintptr_t owner = (uintptr_t) g_previous[i].sa_sigaction;
        if (owner == 0 || g_loggedForeign[i].sa_sigaction == g_previous[i].sa_sigaction) {
            continue;
        }
        g_loggedForeign[i] = g_previous[i];

        char line[512];
        appendModuleRecord(line, sizeof(line), "native_crash_displaced", owner);
        size_t length = strlen(line);
        while (length > 0 && line[length - 1] != '\n') {
            line[--length] = '\0';
        }
        size_t used = length;
        appendLiteral(line, sizeof(line), &used, " signal=");
        appendLiteral(line, sizeof(line), &used, signalName(kHandledSignals[i]));
        appendLiteral(line, sizeof(line), &used, " disposition=wrapped_owned_by_us=1\n");
        line[used < sizeof(line) ? used : sizeof(line) - 1] = '\0';
        commitRecord(line);
    }
}

static void installOurHandler(int signum) {
    if (orig_sigaction == nullptr) {
        return;
    }
    struct sigaction ours;
    memset(&ours, 0, sizeof(ours));
    ours.sa_sigaction = handleSignal;
    ours.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&ours.sa_mask);
    orig_sigaction(signum, &ours, nullptr);
}

static int my_sigaction(int signum, const struct sigaction *act,
                        struct sigaction *oldact) {
    if (orig_sigaction == nullptr) {
        if (oldact != nullptr) {
            memset(oldact, 0, sizeof(*oldact));
        }
        return -1;
    }
    // Forward first, so *oldact holds what the caller would have seen.
    int result = orig_sigaction(signum, act, oldact);
    if (result != 0 || act == nullptr) {
        return result;
    }
    int index = indexOfSignal(signum);
    if (index < 0) {
        return result;
    }
    if ((act->sa_flags & SA_SIGINFO) == 0) {
        return result; // not a SA_SIGINFO handler; nothing to wrap
    }
    if (act->sa_sigaction == handleSignal) {
        return result; // our own re-install
    }
    noteForeignHandler(index, act);
    installOurHandler(signum);
    return result;
}

static void installSigactionHook() {
    void *target = resolve_libc("sigaction");
    if (target == nullptr) {
        return;
    }
    DobbyHook(target, (void *) my_sigaction, (void **) &orig_sigaction);
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
        flushDisplacementLog();
    }
    return nullptr;
}
} // namespace

void NativeCrashHandler::install(const char *internalDirectory, const char *sharedDirectory,
                                 const char *downloadDirectory) {
    if (internalDirectory != nullptr) {
        buildCrashPath(g_internalPath, sizeof(g_internalPath), internalDirectory);
        g_internalReady = 1;
    }
    if (sharedDirectory != nullptr) {
        buildCrashPath(g_sharedPath, sizeof(g_sharedPath), sharedDirectory);
        g_sharedReady = 1;
    }
    if (downloadDirectory != nullptr) {
        buildCrashPath(g_downloadPath, sizeof(g_downloadPath), downloadDirectory);
        g_downloadReady = 1;
    }

    stack_t alternate; // run the handler off a dedicated stack (stack-overflow SIGSEGV)
    memset(&alternate, 0, sizeof(alternate));
    alternate.ss_sp = g_alternateStack;
    alternate.ss_size = sizeof(g_alternateStack);
    alternate.ss_flags = 0;
    sigaltstack(&alternate, nullptr);

    writeArmProbe();
    armHandlers();
    // Order matters: sigaction is wrapped before abort so a reporter that
    // installs handlers during its own init is already captured.
    installSigactionHook();
    installAbortHook();

    pthread_t watchdog;
    if (pthread_create(&watchdog, nullptr, rearmLoop, nullptr) == 0) {
        pthread_detach(watchdog);
    }
}
