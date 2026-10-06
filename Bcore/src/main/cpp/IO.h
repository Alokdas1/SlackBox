



#ifndef VIRTUALM_IO_H
#define VIRTUALM_IO_H

#include <jni.h>

#include <list>
#include <iostream>
#include "BoxCore.h"

using namespace std;

class IO {
public:
    static void init(JNIEnv *env);

    struct RelocateInfo {
        const char *targetPath;
        const char *relocatePath;
    };

    static void addRule(const char *targetPath, const char *relocatePath);

    static jstring redirectPath(JNIEnv *env, jstring path);

    static jobject redirectPath(JNIEnv *env, jobject path);

    static const char *redirectPath(const char *__path);

    // Same decision, but reports ownership. `redirectPath` returns one of three
    // things: the caller's own pointer (no rule matched), a heap buffer from
    // replace() (caller must free), or a string literal such as "/dev/null"
    // (must never be freed). A caller that compares the result against the
    // input pointer to decide whether to free gets that last case wrong and
    // crashes on a literal. Interceptors that sit on libc entry points use
    // this form instead.
    static const char *redirectPath(const char *path, bool *owned);
};


#endif 
