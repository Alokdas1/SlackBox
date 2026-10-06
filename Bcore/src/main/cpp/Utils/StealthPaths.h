#ifndef SLACKBOX_STEALTHPATHS_H
#define SLACKBOX_STEALTHPATHS_H

// Central registry of filesystem entries that must read back as absent from
// guest processes.
//
// This replaces the two divergent blocklists that previously lived in
// Utils/AntiDetection.cpp. They disagreed with each other, matched with
// strstr() (so "/data/data/com.lody.virtualfoo" matched the entry
// "/data/data/com.lody.virtual"), and were never actually installed.
//
// Matching is component-aware: an entry matches only at a '/' boundary, so a
// blocked directory cannot shadow an unrelated sibling whose name merely
// starts with the same characters. Entries are compared exactly -- there is no
// globbing, because every entry is already a concrete path and wildcards only
// add ways to get the boundary logic wrong.
//
// The tables are const and are never mutated after static init, so isBlocked()
// is safe to call from any thread with no locking. Guest I/O is genuinely
// multi-threaded (render threads, network pools, JIT threads all touch the
// filesystem concurrently), so this matters.
//
// Nothing here allocates. These checks sit on every open()/stat() the guest
// performs, including from signal handlers and from threads that may already
// hold the libc malloc lock.

class StealthPaths {
public:
    // True when `path` names an entry that must not be observable from a
    // guest. Checks both the filesystem blocklist and the package blocklist.
    // `path` may be null, which returns false.
    static bool isBlocked(const char *path);

    // Blocklist maintenance. Entries are copied into fixed-capacity storage;
    // the caller keeps ownership of the string it passes. Intended for the
    // host process only, before guests start -- not for use on an I/O hot
    // path, and not safe to call concurrently with isBlocked() on a table
    // that is being appended to.
    static void blockFile(const char *path);
    static void blockPackage(const char *packageName);

    // Diagnostics for start-up logging. Sizes only; the contents are host
    // details that have no business reaching a guest log.
    static unsigned blockedFileCount();
    static unsigned blockedPackageCount();

    // True when `path` contains `entry` at a component boundary: the whole
    // path, or preceded by '/' with the next character also '/' or the
    // terminator. Exposed for the filesystem hook, which needs the same
    // boundary semantics when it checks a path it has already rewritten.
    static bool matchesComponent(const char *path, const char *entry);
};

#endif  // SLACKBOX_STEALTHPATHS_H
