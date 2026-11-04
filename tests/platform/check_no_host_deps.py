#!/usr/bin/env python3
"""P3.6 acceptance: runtime/ compiles without direct platform dependencies.

Fails if a source file under the given directories includes an OS, windowing,
audio or browser header, or branches on an OS/target macro. Such code belongs
in platform/ (lgp_platform and its backends). Standard C++ headers and the
lgp/platform/*.hpp interfaces are fine.

Usage: check_no_host_deps.py <dir>...      (exit 1 and a list on violations)
       check_no_host_deps.py --self-test
"""
import os
import re
import sys
import tempfile

SOURCE_EXT = (".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx", ".inl")

# Header names (or prefixes ending in '/') that tie code to a host platform.
FORBIDDEN_HEADERS = [
    "windows.h", "winbase.h", "process.h", "io.h", "direct.h",
    "pthread.h", "unistd.h", "fcntl.h", "dlfcn.h", "poll.h", "termios.h",
    "sys/", "mach/", "mach-o/", "linux/", "libkern/",
    "emscripten.h", "emscripten/",
    "SDL.h", "SDL2/", "SDL3/",
    "webgpu.h", "webgpu/", "dawn/", "wgpu.h",
    "AudioToolbox/", "CoreAudio/", "alsa/", "pulse/",
]

# Target/OS macros. Compiler macros (__clang__, _MSC_VER) are allowed: they
# select compiler features, not host APIs.
FORBIDDEN_MACROS = [
    "_WIN32", "_WIN64", "__EMSCRIPTEN__", "__APPLE__", "__MACH__", "__linux__",
    "__unix__", "__ANDROID__", "__FreeBSD__",
]

INCLUDE_RE = re.compile(r'^\s*#\s*include\s*[<"]([^>"]+)[>"]')
MACRO_RE = re.compile(r"\b(" + "|".join(re.escape(m) for m in FORBIDDEN_MACROS) + r")\b")


def strip_comments(line, in_block):
    out = []
    i = 0
    while i < len(line):
        if in_block:
            end = line.find("*/", i)
            if end < 0:
                return "".join(out), True
            i = end + 2
            in_block = False
        elif line.startswith("//", i):
            break
        elif line.startswith("/*", i):
            in_block = True
            i += 2
        else:
            out.append(line[i])
            i += 1
    return "".join(out), in_block


def check_file(path):
    problems = []
    in_block = False
    with open(path, encoding="utf-8", errors="replace") as f:
        for n, raw in enumerate(f, 1):
            line, in_block = strip_comments(raw.rstrip("\n"), in_block)
            m = INCLUDE_RE.match(line)
            if m:
                header = m.group(1)
                for bad in FORBIDDEN_HEADERS:
                    if header == bad or (bad.endswith("/") and header.startswith(bad)):
                        problems.append(f"{path}:{n}: includes <{header}>")
            elif line.lstrip().startswith("#"):
                for macro in MACRO_RE.findall(line):
                    problems.append(f"{path}:{n}: uses {macro}")
    return problems


def check_dirs(dirs):
    problems = []
    count = 0
    for d in dirs:
        for root, _, files in os.walk(d):
            for name in sorted(files):
                if name.endswith(SOURCE_EXT):
                    count += 1
                    problems += check_file(os.path.join(root, name))
    return count, problems


def self_test():
    with tempfile.TemporaryDirectory() as tmp:
        good = os.path.join(tmp, "good.cpp")
        with open(good, "w") as f:
            f.write("#include <thread>\n#include \"lgp/platform/storage.hpp\"\n"
                    "// #include <windows.h> in a comment\n/* #ifdef _WIN32 */\n#if defined(__clang__)\n#endif\n")
        bad = os.path.join(tmp, "bad.hpp")
        with open(bad, "w") as f:
            f.write("#include <sys/mman.h>\n#  include <SDL2/SDL.h>\n#if defined(_WIN32)\n#endif\n"
                    "#ifdef __EMSCRIPTEN__\n#endif\nint _WIN32_like = 0;\n")
        assert check_file(good) == [], check_file(good)
        found = check_file(bad)
        assert len(found) == 4, found
    print("self-test passed")
    return 0


def main(argv):
    if argv == ["--self-test"]:
        return self_test()
    if not argv:
        print(__doc__)
        return 2
    count, problems = check_dirs(argv)
    if count == 0:
        print("no source files found")
        return 1
    for p in problems:
        print(p)
    if problems:
        print(f"{len(problems)} host dependencies in {count} files; move them to platform/")
        return 1
    print(f"{count} files without host dependencies")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
