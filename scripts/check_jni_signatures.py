#!/usr/bin/env python3
"""Verify every RegisterNatives entry matches its C function's real arity.

An arity mismatch in a JNI natives table is silent and immediate: the VM pushes
exactly as many arguments as the declared signature says, and the C function
reads whatever is past them. That garbage then gets dereferenced, and the
process dies before any of its own diagnostics exist to explain why.

This happened on 2026-10-06. installNativeCrashHandler gained a third jstring
for the Download crash-log sink; NativeCore.java was updated, the C function was
updated, and the table still said "(Ljava/lang/String;Ljava/lang/String;)V".
The whole container became unlaunchable. The compiler cannot catch this, because
the cast to (void *) erases the type.

Exits non-zero on any mismatch so it can gate a build.
"""
import re
import sys
from pathlib import Path

CPP = Path(__file__).resolve().parent.parent / "Bcore/src/main/cpp/BoxCore.cpp"
# The first two parameters of every entry are the JNIEnv* and the receiver.
LEADING_JNI_PARAMS = 2


def declared_arity(signature: str) -> int:
    """Count parameters in a JNI descriptor, ignoring the trailing ';'."""
    body = signature[signature.index("(") + 1 : signature.rindex(")")]
    return len([arg for arg in body.split(";") if arg])


def function_arity(source: str, name: str):
    """Parameter count of a C function definition, or None if not found."""
    pattern = re.compile(
        r"^(?:static\s+)?[A-Za-z_][\w:*&<>\s]*?\b" + re.escape(name) + r"\s*\(([^;{]*)\)\s*\{",
        re.M | re.S,
    )
    for match in pattern.finditer(source):
        params = [p for p in match.group(1).split(",") if p.strip()]
        return max(len(params) - LEADING_JNI_PARAMS, 0)
    return None


def main() -> int:
    source = CPP.read_text()
    table = re.search(r"JNINativeMethod\s+gMethods\[\]\s*=\s*\{(.*?)\n\};", source, re.S)
    if table is None:
        print(f"FAIL: no gMethods natives table found in {CPP}")
        return 1

    rows = re.findall(
        r'\{"([^"]+)",\s*"([^"]+)",\s*\(void \*\)\s*(\w+)\}', table.group(1)
    )
    if not rows:
        print("FAIL: natives table parsed but contained no entries")
        return 1

    mismatches = []
    print(f"checked {len(rows)} natives in {CPP.name}\n")
    for name, signature, function in rows:
        declared = declared_arity(signature)
        actual = function_arity(source, function)
        status = "ok"
        if actual is None:
            status = "DEFINITION NOT FOUND"
            mismatches.append((name, signature, declared, actual))
        elif declared != actual:
            status = "MISMATCH"
            mismatches.append((name, signature, declared, actual))
        print(f"  {name:<32} declared={declared} actual={actual}  {status}")

    print()
    if mismatches:
        print("FAIL: declared JNI signature does not match the C function.")
        for name, signature, declared, actual in mismatches:
            print(f"  {name}: descriptor {signature} declares {declared}, "
                  f"C function takes {actual}")
        print("\nThe VM pushes `declared` arguments. The C function reads "
              "`actual`.\nAny extra read is stack garbage and dereferenced. "
              "A signature that does not\nmatch the C function is a hard crash "
              "with no diagnostic; update both together.")
        return 1

    print("PASS: every declared signature matches its C function")
    return 0


if __name__ == "__main__":
    sys.exit(main())