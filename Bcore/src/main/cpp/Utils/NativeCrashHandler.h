#ifndef SLACKBOX_NATIVE_CRASH_HANDLER_H
#define SLACKBOX_NATIVE_CRASH_HANDLER_H

namespace NativeCrashHandler {
    // Installs async-signal-safe handlers for the fatal signals a guest process
    // can die from. Each record is appended to <internalDirectory>/native_crash_<pid>.log
    // and, when the caller supplies one, <sharedDirectory>/native_crash_<pid>.log.
    //
    // The shared directory exists because the internal one lives under the host
    // app's private data, unreadable by an external log collector. Writes bypass
    // the libc wrappers with raw syscalls so the container's own path redirect
    // cannot swallow the record.
    void install(const char *internalDirectory, const char *sharedDirectory);
}

#endif
