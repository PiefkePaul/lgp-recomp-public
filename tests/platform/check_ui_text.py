#!/usr/bin/env python3
"""The app's interface text in two languages (lgp/platform/ui_text.hpp).

Checks that
- every tr("...")/trf("...") with a literal in the sources, and every mod's
  title and description (ModInfo in runtime/mods/src), has a German line in
  the table of platform/src/ui_text.cpp,
- every English text of the table still occurs as a string literal in the
  sources (no stale lines), with no duplicates,
- English and German have the same number of "{}" placeholders,
- Android's values/strings.xml and values-de/strings.xml name the same texts
  with the same format arguments,
- the German .strings files of the iOS/tvOS bundles only name texts of their
  plists.

Usage: check_ui_text.py <repo root>
       check_ui_text.py --self-test
"""
import os
import re
import sys
import xml.etree.ElementTree as ET

SOURCE_DIRS = ("platform", "runtime")
SOURCE_EXT = (".cpp", ".hpp", ".mm")
SKIP_PARTS = ("third_party",)
TABLE = os.path.join("platform", "src", "ui_text.cpp")

# A string literal, then more literals separated by blanks (C++ concatenation).
LITERAL = r'"(?:[^"\\\n]|\\.)*"'
SEQUENCE = re.compile(LITERAL + r'(?:\s*' + LITERAL + r')*')
CALL = re.compile(r'(?<![\w.>])(?:tr|trf)\(\s*(' + LITERAL + r'(?:\s*' + LITERAL + r')*)')
# A mod's ModInfo in its ModImpl: ModImpl m = {{"id", "Title", "Description", ...
MOD_INFO = re.compile(r'ModImpl\s+\w+\s*=\s*\{\s*\{\s*"([a-z0-9-]+)",\s*(' + LITERAL + r'(?:\s*' + LITERAL + r')*),\s*(' +
                      LITERAL + r'(?:\s*' + LITERAL + r')*)\s*,')
MODS_DIR = os.path.join("runtime", "mods", "src")
ROW = re.compile(r'\{\s*(' + LITERAL + r'(?:\s*' + LITERAL + r')*)\s*,\s*(' + LITERAL + r'(?:\s*' + LITERAL +
                 r')*)\s*\}')


def decode(sequence):
    """The bytes of a C++ literal sequence as UTF-8 text."""
    out = bytearray()
    for lit in re.findall(LITERAL, sequence):
        body = lit[1:-1]
        i = 0
        while i < len(body):
            c = body[i]
            if c != "\\":
                out += c.encode("utf-8")
                i += 1
                continue
            n = body[i + 1]
            if n == "x":
                j = i + 2
                while j < len(body) and body[j] in "0123456789abcdefABCDEF":
                    j += 1
                out.append(int(body[i + 2:j], 16) & 0xFF)
                i = j
                continue
            if n in "01234567":
                j = i + 1
                while j < len(body) and j < i + 4 and body[j] in "01234567":
                    j += 1
                out.append(int(body[i + 1:j], 8) & 0xFF)
                i = j
                continue
            simple = {"n": "\n", "t": "\t", "r": "\r", "a": "\a", "b": "\b", "f": "\f", "v": "\v"}
            out += simple.get(n, n).encode("utf-8")
            i += 2
    return out.decode("utf-8", errors="replace")


def strip_comments(text):
    """C++ text without // and /* */ comments (string literals kept)."""
    out, i = [], 0
    while i < len(text):
        if text.startswith("//", i):
            j = text.find("\n", i)
            i = len(text) if j < 0 else j
        elif text.startswith("/*", i):
            j = text.find("*/", i + 2)
            i = len(text) if j < 0 else j + 2
        elif text[i] == '"':
            m = re.compile(LITERAL).match(text, i)
            if m is None:
                out.append(text[i])
                i += 1
            else:
                out.append(m.group(0))
                i = m.end()
        elif text[i] == "'":
            m = re.compile(r"'(?:[^'\\\n]|\\.)*'").match(text, i)
            out.append(m.group(0) if m else text[i])
            i = m.end() if m else i + 1
        else:
            out.append(text[i])
            i += 1
    return "".join(out)


def placeholders(text):
    # Only trf()'s "{}": a strftime format may order its fields differently.
    return text.count("{}")


def read_table(text):
    """(english, german) rows of ui_text.cpp's table."""
    body = strip_comments(text)
    start = body.find("kGerman[] = {")
    if start < 0:
        raise ValueError("no kGerman[] table")
    end = body.find("};", start)
    return [(decode(a), decode(b)) for a, b in ROW.findall(body[start:end])]


def check(root):
    problems = []
    with open(os.path.join(root, TABLE), encoding="utf-8") as f:
        rows = read_table(f.read())
    english = {}
    for en, de in rows:
        if en in english:
            problems.append(f"{TABLE}: {en!r} twice")
        english[en] = de
        if placeholders(en) != placeholders(de):
            problems.append(f"{TABLE}: placeholders differ: {en!r} / {de!r}")
    called, literals = {}, set()
    for top in SOURCE_DIRS:
        for dirpath, dirnames, filenames in os.walk(os.path.join(root, top)):
            dirnames[:] = [d for d in dirnames if d not in SKIP_PARTS]
            for name in filenames:
                if not name.endswith(SOURCE_EXT):
                    continue
                path = os.path.join(dirpath, name)
                rel = os.path.relpath(path, root)
                if rel == TABLE:
                    continue
                with open(path, encoding="utf-8") as f:
                    text = strip_comments(f.read())
                for m in CALL.finditer(text):
                    called.setdefault(decode(m.group(1)), rel)
                if rel.startswith(MODS_DIR):  # the menu shows tr(title), tr(description)
                    for m in MOD_INFO.finditer(text):
                        called.setdefault(decode(m.group(2)), rel)
                        called.setdefault(decode(m.group(3)), rel)
                for m in SEQUENCE.finditer(text):
                    literals.add(decode(m.group(0)))
    for en, where in sorted(called.items()):
        if en not in english:
            problems.append(f"{where}: no German line for {en!r}")
    for en in english:
        if en not in literals:
            problems.append(f"{TABLE}: {en!r} occurs nowhere in the sources")
    problems += check_android(root)
    problems += check_apple(root)
    return problems, len(english), len(called)


def check_android(root):
    problems = []
    res = os.path.join(root, "platform", "android", "res")
    tables = []
    for values in ("values", "values-de"):
        path = os.path.join(res, values, "strings.xml")
        if not os.path.exists(path):
            problems.append(f"{path}: missing")
            return problems
        tables.append({e.get("name"): e.text or "" for e in ET.parse(path).getroot().iter("string")})
    en, de = tables
    if set(en) != set(de):
        problems.append(f"android strings: names differ: {sorted(set(en) ^ set(de))}")
    for name in set(en) & set(de):
        if sorted(re.findall(r"%\d+\$[a-z]", en[name])) != sorted(re.findall(r"%\d+\$[a-z]", de[name])):
            problems.append(f"android strings: arguments of {name} differ")
    java = os.path.join(root, "platform", "android", "java")
    used = set()
    for dirpath, _, filenames in os.walk(java):
        for name in filenames:
            if name.endswith(".java"):
                with open(os.path.join(dirpath, name), encoding="utf-8") as f:
                    used |= set(re.findall(r'text\(\w+,\s*"(\w+)"', f.read()))
    for name in sorted(used - set(en)):
        problems.append(f"android strings: {name} used but not in values/strings.xml")
    for name in sorted(set(en) - used):
        problems.append(f"android strings: {name} unused")
    return problems


def check_apple(root):
    problems = []
    pairs = [
        (os.path.join("platform", "ios", "Settings.bundle", "Root.plist"),
         os.path.join("platform", "ios", "Settings.bundle", "de.lproj", "Root.strings"), "string"),
        (os.path.join("platform", "ios", "Info.plist.in"),
         os.path.join("platform", "ios", "de.lproj", "InfoPlist.strings"), "key"),
        (os.path.join("platform", "tvos", "Info.plist.in"),
         os.path.join("platform", "tvos", "de.lproj", "InfoPlist.strings"), "key"),
    ]
    for plist, strings, kind in pairs:
        with open(os.path.join(root, plist), encoding="utf-8") as f:
            names = set(re.findall(rf"<{kind}>([^<]*)</{kind}>", f.read()))
        with open(os.path.join(root, strings), encoding="utf-8") as f:
            text = f.read()
        keys = re.findall(r'^"((?:[^"\\]|\\.)*)"\s*=\s*"(?:[^"\\]|\\.)*";\s*$', text, re.M)
        if not keys:
            problems.append(f"{strings}: no entries")
        for key in keys:
            if key.replace('\\"', '"') not in names:
                problems.append(f"{strings}: {key!r} is no {kind} of {plist}")
    return problems


def self_test():
    assert decode(r'"Gr\xC3\xB6\xC3\x9F" "e"') == "Größe"
    assert decode(r'"a\"b" "\n"') == 'a"b\n'
    assert placeholders("Save {} at %H:%M") == 1
    rows = read_table('const Row kGerman[] = {\n  {"On", "An"},\n  // {"x", "y"},\n  {"A {}" " b", "C {}"},\n};')
    assert rows == [("On", "An"), ("A {} b", "C {}")], rows
    calls = [decode(m.group(1)) for m in CALL.finditer('x = tr("One"); y = platform::trf("Two {}" "!", {a}); '
                                                        'str("no"); p->tr("no");')]
    assert calls == ["One", "Two {}!"], calls
    mods = [(m.group(1), decode(m.group(2)), decode(m.group(3))) for m in MOD_INFO.finditer(
        'static const ModImpl m = {\n {"wild-battles", "Wild battles",\n "Touch" " first.", true, true}, {"a", "b", "c",')]
    assert mods == [("wild-battles", "Wild battles", "Touch first.")], mods
    assert strip_comments('a // "x"\nb /* "y" */ "c // d"') == 'a \nb  "c // d"'
    print("check_ui_text: self-test ok")
    return 0


def main(argv):
    if argv[1:] == ["--self-test"]:
        return self_test()
    if len(argv) != 2:
        print(__doc__)
        return 2
    problems, table, calls = check(argv[1])
    for p in problems:
        print(p)
    print(f"check_ui_text: {table} German lines, {calls} texts in tr()/trf(), {len(problems)} problems")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
